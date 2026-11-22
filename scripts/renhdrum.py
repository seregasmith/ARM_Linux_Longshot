"""RenHDrum (renhdrum.dll, renhdrum.so on Linux), the library host/rumble_renhdrum.c plays some controllers' vibration
with, built from its source in third_party/renhdrum.

    python -I scripts/renhdrum.py build [--target windows|linux] [--llvm-mingw <folder>] [--linker <cc>]
                                        [--cache <folder>] [--out <file>]
    python -I scripts/renhdrum.py check <file>

`build` compiles third_party/renhdrum (RenHDrum v0.5.0, MIT OR Apache-2.0) with a pinned Rust (RUST below: each part's
SHA-256 checked, kept in --cache). Its one build dependency, the cc crate (and the two crates cc needs), is downloaded from
crates.io and checked by the SHA-256 its Cargo.lock records; nothing else is asked of the network. Its build.rs compiles
libopus 1.5.2 into the library, from third_party/opus (the same release as RenHDrum's own vendor/opus, which
third_party/renhdrum leaves out), with the same C compiler that links it:
- windows (the default on Windows): x86_64-pc-windows-gnullvm, linked by llvm-mingw, the one the game is built with
  (--llvm-mingw, else LONGSHOT_MINGW), its unwinder linked in and the C runtime Windows' own UCRT, as the game's;
- linux (the default elsewhere): x86_64-unknown-linux-gnu, linked by --linker (default cc) on the system it is for (the
  AppImage's: Ubuntu 22.04's C library).
No time stamp and no path of this machine go in: the same source gives the same bytes. Its output (--out, default
third_party/renhdrum/renhdrum.dll or .so, which .gitignore leaves out) is checked as `check` checks it.

The Builder's release makes the DLL in CI (.forgejo/workflows/build-windows.yml, scripts/builder_package.py --renhdrum)
and the AppImage the .so (.forgejo/workflows/build-linux.yml, scripts/linux/appimage.sh), so a player's build places it
beside the game (nx2/build.py, nx2/make_game.py) with no Rust on their PC. A build without it plays every controller's
motors or its own HD rumble.

`check` reads the file's headers only (nothing in it is loaded or run): x86-64, the exports host/rumble_renhdrum.c binds,
and no library a player would have to install (a C++ or unwinder runtime, a VC++ Runtime).
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script's folder off the path)
import helpers  # noqa: E402
from helpers import ROOT  # noqa: E402

SRC = os.path.join(ROOT, "third_party", "renhdrum")
TARGETS = {"windows": "x86_64-pc-windows-gnullvm", "linux": "x86_64-unknown-linux-gnu"}
BUILT = {"windows": "renhdrum.dll", "linux": "librenhdrum.so"}   # (cargo's names)
NAMES = {"windows": "renhdrum.dll", "linux": "renhdrum.so"}      # (the game's)
RUST_VERSION = "1.99.1"
DIST = "https://static.rust-lang.org/dist/"
# (the parts of a Rust that compiles the crate: rustc and cargo for this system, the target's standard library, and this
# system's own when it is not the target's, for build.rs, which runs here)
RUST = {
    "linux": [
        ("rustc-%s-x86_64-unknown-linux-gnu" % RUST_VERSION, "2ff05ff9c08a90861e7f6e4065b40a8b87463b4576186fd613871c3e37f2091f"),
        ("cargo-%s-x86_64-unknown-linux-gnu" % RUST_VERSION, "53f3521949404d595efb06a55c12c5658d4ed1c75566f33d3d514eaebe70bd8a"),
    ],
    "windows": [
        ("rustc-%s-x86_64-pc-windows-gnullvm" % RUST_VERSION, "00eb55f0c1fd330a0f7b82d288b32648b867bd523e55039d69c5914804dd7ad5"),
        ("cargo-%s-x86_64-pc-windows-gnullvm" % RUST_VERSION, "44753601667a1b201c9e2e4233d8311e2e7d3321273d6041a07e3d4e7966025b"),
    ],
}
RUST_STD = {
    "windows": ("rust-std-%s-x86_64-pc-windows-gnullvm" % RUST_VERSION,
                "8ddc26705e13c46530c898524b0d2657b7d416a25e8d9ffe369a97b23a619bf6"),
    "linux": ("rust-std-%s-x86_64-unknown-linux-gnu" % RUST_VERSION,
              "109b074225bf5b9ea3820890876bfdf4ed161adbfaf94b2b4b5984c85ee5d958"),
}
# (host/rumble_renhdrum.c's RH_FUNCS, each with the renhdrum_ prefix)
EXPORTS = tuple("renhdrum_" + n for n in (
    "version", "set_nx2_mode", "dualsense_dsp_create", "dualsense_dsp_destroy", "dualsense_dsp_set_half_target",
    "dualsense_dsp_fill_f32", "dualsense_dsp_build_bt_report", "dualsense_dsp_set_speaker",
    "dualsense_dsp_build_bt36_report", "tri_trigger_create", "tri_trigger_destroy", "tri_trigger_update",
    "tri_trigger_note_action", "hd_haptics_speaker_block", "hd_haptics_trigger_block", "valve_grip_dsp_create_ex",
    "valve_grip_build_report", "valve_grip_mode_tick", "valve_grip_mode_off", "xbox_rumble_create",
    "xbox_rumble_destroy",
    "xbox_rumble_push_half_sample", "xbox_rumble_tick", "engine_create", "engine_destroy", "engine_push_half_sample",
    "engine_tick"))
# (Windows' own: present on every Windows 10 and 11, as they are for longshot.exe, which llvm-mingw links to the UCRT too)
SYSTEM_IMPORT = re.compile(r"^(kernel32|ntdll|advapi32|bcrypt|bcryptprimitives|userenv|ws2_32|ucrtbase|"
                           r"api-ms-win-[a-z0-9-]+)\.dll$")
# (every Linux system's own: the C library's parts, and GCC's unwinder, which every desktop system carries)
SYSTEM_NEEDED = re.compile(r"^(libc|libm|libdl|libpthread|librt|libgcc_s|ld-linux-x86-64)\.so\.\d+$")
CRATES = "https://static.crates.io/crates/"
OPUS = os.path.join(ROOT, "third_party", "opus")
# (third_party/opus's files that RenHDrum's vendor/opus does not have: build.rs compiles every .c of celt/, and this one
# is a program of its own, with a main)
OPUS_LEFT_OUT = ("celt/opus_custom_demo.c",)


def shown(path):
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:
        return os.path.abspath(path)


def fail(message):
    print("renhdrum: " + message, file=sys.stderr)
    raise SystemExit(1)


# ---- the file's own headers: no tool, and nothing loaded ----

def pe_info(data):
    """(machine, imported DLL names in lower case, exported names) of a PE image's bytes; SystemExit when it is not one"""
    try:
        if data[:2] != b"MZ":
            raise ValueError("no MZ header")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("no PE header")
        machine, sections, _, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic not in (0x10B, 0x20B):
            raise ValueError("optional header magic %#x" % magic)
        dirs = opt + (112 if magic == 0x20B else 96)
        table = opt + opt_size

        def at(rva):
            for i in range(sections):
                _, vsize, vaddr, rawsize, rawptr = struct.unpack_from("<8sIIII", data, table + i * 40)
                if vaddr <= rva < vaddr + max(vsize, rawsize):
                    return rawptr + (rva - vaddr)
            raise ValueError("%#x is in no section" % rva)

        def name(rva):
            start = at(rva)
            return data[start:data.index(b"\0", start)].decode("ascii", "replace")

        imports = []
        rva = struct.unpack_from("<I", data, dirs + 8)[0]
        if rva:
            off = at(rva)
            while len(data[off:off + 20]) == 20 and data[off:off + 20] != b"\0" * 20:
                n = struct.unpack_from("<I", data, off + 12)[0]
                if n:
                    imports.append(name(n).lower())
                off += 20
        exports = []
        rva = struct.unpack_from("<I", data, dirs)[0]
        if rva:
            d = at(rva)
            count, names = struct.unpack_from("<I", data, d + 24)[0], struct.unpack_from("<I", data, d + 32)[0]
            if names:
                base = at(names)
                exports = [name(struct.unpack_from("<I", data, base + 4 * i)[0]) for i in range(min(count, 4096))]
        return machine, imports, exports
    except (struct.error, ValueError, OverflowError) as e:
        fail("not a usable PE image (%s)" % e)


def elf_info(data):
    """(machine, DT_NEEDED names, defined dynamic symbols) of a 64-bit little-endian ELF's bytes; SystemExit otherwise"""
    try:
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise ValueError("not a 64-bit little-endian ELF")
        machine = struct.unpack_from("<H", data, 18)[0]
        shoff = struct.unpack_from("<Q", data, 40)[0]
        shentsize, shnum = struct.unpack_from("<HH", data, 58)
        sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]

        def strings(index, off):
            base = sections[index][4] + off
            return data[base:data.index(b"\0", base)].decode("ascii", "replace")
        needed, symbols = [], []
        for _, kind, _, _, offset, size, link, _, _, entsize in sections:
            if kind == 6:   # SHT_DYNAMIC: {d_tag, d_val} pairs, DT_NEEDED = 1
                for at in range(offset, offset + size, 16):
                    tag, val = struct.unpack_from("<qQ", data, at)
                    if tag == 0:
                        break
                    if tag == 1:
                        needed.append(strings(link, val))
            elif kind == 11:   # SHT_DYNSYM: Elf64_Sym, 24 bytes; a defined one has a section index (not 0)
                for at in range(offset + 24, offset + size, entsize or 24):
                    name, info, _, shndx = struct.unpack_from("<IBBH", data, at)
                    if name and shndx:
                        symbols.append(strings(link, name))
        return machine, needed, symbols
    except (struct.error, ValueError, IndexError, OverflowError) as e:   # (an offset past the file: OverflowError)
        fail("not a usable ELF image (%s)" % e)


def problems(data, target=None):
    """What makes the library unusable by host/rumble_renhdrum.c on a player's PC: [] when nothing does. target (windows,
    linux): the system it is for, whose format it must be. A Linux library named renhdrum.dll is refused by Windows with a
    "Bad Image" box at the game's start (0xc000012f): the release once built one, on its Linux runner with no --target"""
    is_elf = data[:4] == b"\x7fELF"
    if target == "windows" and is_elf:
        return ["a Linux (ELF) library, not a Windows DLL (scripts/renhdrum.py build --target windows)"]
    if target == "linux" and not is_elf:
        return ["not a Linux (ELF) library (scripts/renhdrum.py build --target linux)"]
    if is_elf:
        machine, deps, exports = elf_info(data)
        ok_machine, own = 62, SYSTEM_NEEDED   # (EM_X86_64)
    else:
        machine, deps, exports = pe_info(data)
        ok_machine, own = 0x8664, SYSTEM_IMPORT
    out = []
    if machine != ok_machine:
        out.append("machine %#x, not x86-64" % machine)
    out += ["no export %s (host/rumble_renhdrum.c binds it)" % n for n in EXPORTS if n not in exports]
    out += ["needs %s, which is not part of the system: a player would need it installed" % i
            for i in deps if not own.match(i)]
    return out


def cmd_check(a):
    if not os.path.isfile(a.file):
        fail("%s is not there (python -I scripts/renhdrum.py build makes it)" % shown(a.file))
    with open(a.file, "rb") as f:
        data = f.read()
    # (the game's names: renhdrum.dll on Windows, renhdrum.so on Linux)
    target = {".dll": "windows", ".so": "linux"}.get(os.path.splitext(a.file)[1].lower())
    deps = elf_info(data)[1] if data[:4] == b"\x7fELF" else pe_info(data)[1]
    print("%s: %d bytes, sha256 %s" % (shown(a.file), len(data), hashlib.sha256(data).hexdigest()))
    print("  needs: %s" % (", ".join(sorted(deps)) or "(nothing)"))
    bad = problems(data, target)
    for p in bad:
        print("  " + p)
    print("  usable" if not bad else "  not usable")
    return 1 if bad else 0


# ---- the build ----

def host():
    import platform
    if platform.machine().lower() not in ("x86_64", "amd64"):
        fail("Rust is pinned for x86-64 Linux and Windows only, not %s" % platform.machine())
    return "windows" if os.name == "nt" else "linux"


def fetch(name, want, cache):
    """<cache>/<name>.tar.xz, downloaded when missing or not the pinned bytes; SystemExit when the download is not them"""
    path = os.path.join(cache, name + ".tar.xz")
    if not (os.path.isfile(path) and helpers.sha256_file(path) == want):
        print("renhdrum: downloading %s.tar.xz" % name, flush=True)
        try:
            helpers.download(DIST + name + ".tar.xz", path, want, timeout=300)
        except (OSError, ValueError) as e:
            fail(str(e))
    return path


def unpack_component(archive, name, prefix):
    """A Rust dist part's files into prefix: each component folder inside it (rustc/, cargo/, rust-std-<target>/) holds a
    tree laid out as the installed one (bin/, lib/), which install.sh would copy; merged here without running it"""
    with tempfile.TemporaryDirectory(dir=os.path.dirname(prefix)) as tmp:
        helpers.extract(archive, tmp)
        top = os.path.join(tmp, name)
        with open(os.path.join(top, "components"), encoding="utf-8") as f:
            components = f.read().split()
        for c in components:
            src = os.path.join(top, c)
            for dirpath, _, files in os.walk(src):
                rel = os.path.relpath(dirpath, src)
                for fn in files:
                    if rel == "." and fn == "manifest.in":
                        continue
                    dst = os.path.join(prefix, rel, fn)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(os.path.join(dirpath, fn), dst)


def toolchain(cache, target):
    """The pinned Rust for target, unpacked once under cache (a folder named by the parts' hashes): its bin/ folder"""
    parts = RUST[host()] + [RUST_STD[target]] + ([RUST_STD[host()]] if host() != target else [])
    key = hashlib.sha256("".join(h for _, h in parts).encode()).hexdigest()[:16]
    prefix = os.path.join(cache, "rust-%s-%s" % (RUST_VERSION, key))
    done = os.path.join(prefix, ".complete")
    if not os.path.isfile(done):
        shutil.rmtree(prefix, ignore_errors=True)
        os.makedirs(prefix)
        for name, want in parts:
            unpack_component(fetch(name, want, cache), name, prefix)
        with open(done, "w") as f:
            f.write("\n".join(n for n, _ in parts) + "\n")
    return os.path.join(prefix, "bin")


def locked_crates(lock):
    """[(name, version, sha256)] of Cargo.lock's crates.io packages (the build's dependencies; the crate itself has none)"""
    out = []
    for block in lock.split("[[package]]")[1:]:
        f = dict(re.findall(r'^(name|version|source|checksum) = "([^"]*)"', block, re.M))
        if f.get("source", "").startswith("registry+"):
            if not re.fullmatch(r"[0-9a-f]{64}", f.get("checksum", "")):
                fail("Cargo.lock's %s %s has no checksum" % (f.get("name"), f.get("version")))
            out.append((f["name"], f["version"], f["checksum"]))
        elif "source" in f:
            fail("Cargo.lock's %s comes from %s, not crates.io" % (f.get("name"), f["source"]))
    return out


def vendor_crates(cache, into):
    """Cargo.lock's crates, each downloaded once into cache and checked by its SHA-256, unpacked into into/ as a cargo
    directory source (the .cargo-checksum.json cargo asks for, with the package's checksum, which --locked compares)"""
    with open(os.path.join(SRC, "Cargo.lock"), encoding="utf-8") as f:
        crates = locked_crates(f.read())
    os.makedirs(into)
    for name, version, want in crates:
        archive = os.path.join(cache, "crates", "%s-%s.crate" % (name, version))
        if not (os.path.isfile(archive) and helpers.sha256_file(archive) == want):
            print("renhdrum: downloading the %s %s crate" % (name, version), flush=True)
            os.makedirs(os.path.dirname(archive), exist_ok=True)
            try:
                helpers.download("%s%s/%s-%s.crate" % (CRATES, name, name, version), archive, want, timeout=300)
            except (OSError, ValueError) as e:
                fail(str(e))
        helpers.extract(archive, into)   # (a gzipped tar of <name>-<version>/; checked by its SHA-256 above)
        with open(os.path.join(into, "%s-%s" % (name, version), ".cargo-checksum.json"), "w", encoding="utf-8") as f:
            f.write('{"files":{},"package":"%s"}' % want)
    return crates


def copy_opus(into):
    """third_party/opus's sources as build.rs reads them from vendor/opus (celt, silk, src, include, its licence)"""
    for sub in ("celt", "silk", "src", "include"):
        shutil.copytree(os.path.join(OPUS, sub), os.path.join(into, sub),
                        ignore=lambda d, names: [n for n in names
                                                 if os.path.relpath(os.path.join(d, n), OPUS).replace(os.sep, "/")
                                                 in OPUS_LEFT_OUT])
    shutil.copy2(os.path.join(OPUS, "COPYING"), into)


def cmd_build(a):
    target = a.target or host()
    triple = TARGETS[target]
    exe = ".exe" if os.name == "nt" else ""
    if target == "windows":
        mingw = a.llvm_mingw or os.environ.get("LONGSHOT_MINGW", "")
        linker = os.path.join(mingw, "bin", "x86_64-w64-mingw32-clang" + exe)
        if not mingw or not os.path.isfile(linker):
            fail("no llvm-mingw: pass --llvm-mingw <folder> or set LONGSHOT_MINGW (%s is not there)" % (linker or "its clang"))
        archiver = os.path.join(mingw, "bin", "llvm-ar" + exe)
    else:
        linker = shutil.which(a.linker or "cc")
        if not linker:
            fail("no %s to link with: pass --linker <cc>" % (a.linker or "cc"))
        archiver = shutil.which("ar") or "ar"
    cache = os.path.abspath(a.cache or os.path.join(ROOT, "build", "renhdrum-rust"))
    bin_dir = toolchain(cache, target)
    out = os.path.abspath(a.out or os.path.join(SRC, NAMES[target]))
    # (the tracked source only, copied out of the tree: target/ and the cargo home are not written into third_party/. One
    # fixed folder, emptied first, so two builds of the same source are the same bytes)
    work = os.path.join(cache, "work")
    shutil.rmtree(work, ignore_errors=True)
    try:
        crate = os.path.join(work, "renhdrum")
        os.makedirs(crate)
        for f in ("Cargo.toml", "Cargo.lock", "build.rs"):
            shutil.copy2(os.path.join(SRC, f), crate)
        shutil.copytree(os.path.join(SRC, "src"), os.path.join(crate, "src"))
        copy_opus(os.path.join(crate, "vendor", "opus"))
        # (cargo reads crates.io from the downloaded crates only: a cargo directory source, set in its home's config)
        home = os.path.join(work, "cargo-home")
        vendor_crates(cache, os.path.join(work, "crates"))
        os.makedirs(home)
        with open(os.path.join(home, "config.toml"), "w", encoding="utf-8") as f:
            f.write('[source.crates-io]\nreplace-with = "pinned"\n\n[source.pinned]\ndirectory = "%s"\n'
                    % os.path.join(work, "crates").replace("\\", "/"))
        # (no path of this machine in it. Windows: crt-static links llvm-mingw's unwinder in, not libunwind.dll, which a
        # player does not have, the C runtime staying Windows' UCRT; no time stamp in the PE header. Linux: no build id.
        # libopus's C: the same compiler, its file names relative to the work folder)
        flags = "--remap-path-prefix=%s=renhdrum" % crate
        flags += (" -C target-feature=+crt-static -C link-arg=-Wl,--no-insert-timestamp" if target == "windows"
                  else " -C link-arg=-Wl,--build-id=none")
        under = triple.replace("-", "_")
        env = dict(os.environ)
        env.update({
            # (llvm-mingw's bin/ too on Windows: build.rs, built for this system, links with its clang there)
            "PATH": os.pathsep.join([bin_dir] + ([os.path.dirname(linker)] if target == "windows" else [])
                                    + [env.get("PATH", "")]),
            "RUSTC": os.path.join(bin_dir, "rustc" + exe),
            "CARGO_HOME": home,
            "CARGO_TARGET_DIR": os.path.join(work, "target"),
            "CARGO_TARGET_%s_LINKER" % under.upper(): linker,
            "RUSTFLAGS": flags,
            "CC_" + under: linker,
            "AR_" + under: archiver,
            "CFLAGS_" + under: "-ffile-prefix-map=%s=renhdrum" % work,
        })
        for k in ("RUSTUP_TOOLCHAIN", "CARGO_BUILD_TARGET", "CARGO_ENCODED_RUSTFLAGS", "CC", "AR", "CFLAGS",
                  "TARGET_CC", "TARGET_AR", "TARGET_CFLAGS", "CRATE_CC_NO_DEFAULTS"):
            env.pop(k, None)
        cmd = [os.path.join(bin_dir, "cargo" + exe), "build", "--release", "--locked", "--offline", "--lib",
               "--target", triple]
        print("renhdrum: %s (Rust %s, linker %s)" % (" ".join(cmd[1:]), RUST_VERSION, linker), flush=True)
        if subprocess.run(cmd, cwd=crate, env=env).returncode:
            fail("cargo build failed")
        built = os.path.join(work, "target", triple, "release", BUILT[target])
        if not os.path.isfile(built):
            fail("cargo made no %s" % built)
        with open(built, "rb") as f:
            data = f.read()
        bad = problems(data, target)
        if bad:
            fail("what was built is not usable: " + "; ".join(bad))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shutil.copyfile(built, out)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("renhdrum: %s, %d bytes, sha256 %s" % (shown(out), len(data), hashlib.sha256(data).hexdigest()))
    return 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    s = p.add_subparsers(dest="what", required=True)
    b = s.add_parser("build", help="build RenHDrum from third_party/renhdrum with the pinned Rust")
    b.add_argument("--target", choices=sorted(TARGETS), help="windows or linux (default: this system's)")
    b.add_argument("--llvm-mingw", help="windows: llvm-mingw's folder (default: LONGSHOT_MINGW)")
    b.add_argument("--linker", help="linux: the C compiler that links it (default: cc)")
    b.add_argument("--cache", help="where the pinned Rust is kept (default: build/renhdrum-rust)")
    b.add_argument("--out", help="its path (default: third_party/renhdrum/renhdrum.dll or renhdrum.so)")
    b.set_defaults(fn=cmd_build)
    c = s.add_parser("check", help="check a renhdrum.dll or .so's headers: x86-64, the exports, the system's libraries only")
    c.add_argument("file")
    c.set_defaults(fn=cmd_check)
    a = p.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
