"""scripts/renhdrum.py: RenHDrum (renhdrum.dll, renhdrum.so) built from third_party/renhdrum, and checked before it ships
(no network, no Rust, nothing loaded or run).

`check` reads the file's headers only. Over made-up PE and ELF images: a good one passes; one needing a library a player
would have to install (the VC++ Runtime, llvm-mingw's libunwind.dll or libc++; a libstdc++) fails and names it; one
without an export host/rumble_renhdrum.c binds fails and names it; one for another machine fails; one of the other
system's format fails (a Linux library named renhdrum.dll: Windows refuses it with a "Bad Image" box, 0xc000012f); a
file that is neither stops with a message, not a traceback. The script never loads the library (no ctypes): checking never runs it.

`build` is run with cargo and the toolchain stubbed: the command is a locked, offline build of the tracked source, for
x86_64-pc-windows-gnullvm linked by the llvm-mingw given (the unwinder static, no time stamp) or for
x86_64-unknown-linux-gnu linked by the C compiler given (no build id), with no local path in it either way; the source is
copied out of the tree with build.rs, and libopus from third_party/opus where build.rs reads it (its demo program left
out); libopus is compiled by the same C compiler that links it, its paths relative; cargo reads crates.io only from the
crates Cargo.lock names, unpacked from the cache (each checked by its Cargo.lock SHA-256, one that is not refused); what
fails the check is not copied out. A Rust download whose SHA-256 is not the pinned one is refused and not kept.

The Builder's release builds the DLL (.forgejo/workflows/build-windows.yml) and scripts/builder_package.py --renhdrum puts
it where nx2/build.py looks in a player's tools (tools/third_party/renhdrum/renhdrum.dll), refusing one that fails the
check or is not a Windows DLL; the step builds it for Windows, on the Linux runner the release is made on (without
--target it built a Linux library there); scripts/check_windows_files.py wants it in the zip. The AppImage's workflow builds the .so and scripts/linux/
appimage.sh carries it. third_party/renhdrum holds the source, with its upstream archive named.

    python -I -B tests/test_renhdrum.py
"""
import contextlib
import hashlib
import importlib.util
import io
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rh = load("renhdrum_under_test", ROOT / "scripts" / "renhdrum.py")
EXPORTS = rh.EXPORTS
GOOD_IMPORTS = ("kernel32.dll", "ntdll.dll", "api-ms-win-core-synch-l1-2-0.dll", "api-ms-win-crt-heap-l1-1-0.dll")


def elf(needed=("libc.so.6", "libgcc_s.so.1", "libm.so.6"), exports=EXPORTS, machine=62):
    """A 64-bit ELF with three sections: .dynstr, .dynamic (DT_NEEDED each of `needed`), .dynsym (`exports`, defined)"""
    strtab = bytearray(b"\0")
    def add(name):
        at = len(strtab)
        strtab.extend(name.encode() + b"\0")
        return at
    needs = [add(n) for n in needed]
    syms = [add(n) for n in exports]
    dynamic = b"".join(struct.pack("<qQ", 1, n) for n in needs) + struct.pack("<qQ", 0, 0)
    dynsym = b"\0" * 24 + b"".join(struct.pack("<IBBHQQ", n, 0x12, 0, 7, 0x1000, 0) for n in syms)
    body = bytes(strtab), dynamic, dynsym
    at = 64
    offsets = []
    for part in body:
        offsets.append(at)
        at += len(part)
    shoff = at
    head = bytearray(64)
    head[0:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", head, 18, machine)
    struct.pack_into("<Q", head, 40, shoff)
    struct.pack_into("<HH", head, 58, 64, 4)
    # (Elf64_Shdr: name, type, flags, addr, offset, size, link, info, addralign, entsize: null, .dynstr, .dynamic, .dynsym)
    shdrs = struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    shdrs += struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, offsets[0], len(body[0]), 0, 0, 1, 0)
    shdrs += struct.pack("<IIQQQQIIQQ", 0, 6, 0, 0, offsets[1], len(body[1]), 1, 0, 8, 16)
    shdrs += struct.pack("<IIQQQQIIQQ", 0, 11, 0, 0, offsets[2], len(body[2]), 1, 0, 8, 24)
    return bytes(head) + b"".join(body) + shdrs


def pe(imports=GOOD_IMPORTS, exports=EXPORTS, machine=0x8664, magic=0x20B):
    """A PE image with one .rdata section: an export directory naming `exports`, an import table naming `imports`"""
    rva = 0x1000
    body = bytearray()

    def put(data, align=4):
        while len(body) % align:
            body.append(0)
        at = rva + len(body)
        body.extend(data)
        return at

    strings = {name: put(name.encode() + b"\0", 1) for name in tuple(imports) + tuple(exports)}
    export_dir = put(b"\0" * 40)
    funcs = put(b"\0" * (4 * len(exports)))
    names = put(b"\0" * (4 * len(exports)))
    ords = put(b"\0" * (2 * len(exports)))
    for i, name in enumerate(exports):
        struct.pack_into("<I", body, names - rva + 4 * i, strings[name])
        struct.pack_into("<H", body, ords - rva + 2 * i, i)
    struct.pack_into("<IIIIII", body, export_dir - rva + 16, 1, len(exports), len(exports), funcs, names, ords)
    import_dir = put(b"\0" * 20 * (len(imports) + 1))
    for i, name in enumerate(imports):
        struct.pack_into("<I", body, import_dir - rva + 20 * i + 12, strings[name])

    opt_size = 240
    raw_at = 0x200
    raw_size = (len(body) + 0x1FF) // 0x200 * 0x200
    out = bytearray(0x40)
    out[0:2] = b"MZ"
    struct.pack_into("<I", out, 0x3C, 0x40)
    out += b"PE\0\0" + struct.pack("<HHIIIHH", machine, 1, 0, 0, 0, opt_size, 0x2022)
    opt = bytearray(opt_size)
    struct.pack_into("<H", opt, 0, magic)
    dirs = 112 if magic == 0x20B else 96
    struct.pack_into("<IIII", opt, dirs, export_dir, 40, import_dir, 20 * (len(imports) + 1))
    out += opt
    # (Name, VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData)
    out += struct.pack("<8sIIII", b".rdata", len(body), rva, raw_size, raw_at)
    out += b"\0" * (raw_at - len(out))
    out += body + b"\0" * (raw_size - len(body))
    return bytes(out)


class Check(unittest.TestCase):
    def test_a_good_dll(self):
        self.assertEqual(rh.problems(pe()), [])
        machine, imports, exports = rh.pe_info(pe())
        self.assertEqual((machine, imports, exports), (0x8664, list(GOOD_IMPORTS), list(EXPORTS)))

    def test_a_runtime_a_player_lacks(self):
        for lib in ("vcruntime140.dll", "msvcp140.dll", "libunwind.dll", "libc++.dll", "libwinpthread-1.dll"):
            bad = rh.problems(pe(imports=GOOD_IMPORTS + (lib,)))
            self.assertEqual(len(bad), 1, lib)
            self.assertIn(lib, bad[0])

    def test_missing_exports(self):
        bad = rh.problems(pe(exports=EXPORTS[:4]))
        self.assertEqual(bad, ["no export %s (host/rumble_renhdrum.c binds it)" % n for n in EXPORTS[4:]])
        self.assertIn("renhdrum_xbox_rumble_tick", EXPORTS)   # (the module's RH_FUNCS: tests/test_renhdrum.py Ships)

    def test_elf(self):
        self.assertEqual(rh.problems(elf()), [])
        machine, needed, symbols = rh.elf_info(elf())
        self.assertEqual((machine, needed, symbols), (62, ["libc.so.6", "libgcc_s.so.1", "libm.so.6"], list(EXPORTS)))
        bad = rh.problems(elf(needed=("libc.so.6", "libstdc++.so.6")))
        self.assertEqual(len(bad), 1)
        self.assertIn("libstdc++.so.6", bad[0])
        self.assertEqual(rh.problems(elf(exports=EXPORTS[1:])), ["no export %s (host/rumble_renhdrum.c binds it)" % EXPORTS[0]])
        self.assertEqual(rh.problems(elf(machine=183)), ["machine 0xb7, not x86-64"])

    def test_another_machine(self):
        self.assertEqual(rh.problems(pe(machine=0x14C)), ["machine 0x14c, not x86-64"])

    def test_not_a_pe(self):
        for data in (b"", b"MZ" + b"\0" * 10, b"\x7fELF" + b"\0" * 200, b"\x7fELF\x02\x01" + b"\xff" * 100):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                rh.problems(data)

    def test_the_command(self):
        with tempfile.TemporaryDirectory() as d:
            for name, data, want in (("good.dll", pe(), 0), ("bad.dll", pe(imports=("libunwind.dll",)), 1)):
                path = os.path.join(d, name)
                with open(path, "wb") as f:
                    f.write(data)
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    self.assertEqual(rh.main(["check", path]), want)
                self.assertIn(hashlib.sha256(data).hexdigest(), out.getvalue())
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                rh.main(["check", os.path.join(d, "none.dll")])

    def test_the_other_systems_format(self):
        # (a Linux library named renhdrum.dll: Windows will not load it, "Bad Image" 0xc000012f; and a DLL named .so)
        with tempfile.TemporaryDirectory() as d:
            for name, data in (("renhdrum.dll", elf()), ("renhdrum.so", pe())):
                path = os.path.join(d, name)
                with open(path, "wb") as f:
                    f.write(data)
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    self.assertEqual(rh.main(["check", path]), 1, name)
                self.assertIn("not usable", out.getvalue(), name)

    def test_nothing_is_loaded(self):
        src = (ROOT / "scripts" / "renhdrum.py").read_text(encoding="utf-8")
        for word in ("ctypes", "LoadLibrary", "CDLL", "WinDLL"):
            self.assertNotIn(word, src, "scripts/renhdrum.py must not load the DLL it checks")


class Build(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="renhdrum-")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.mingw = os.path.join(self.dir, "llvm-mingw")
        exe = ".exe" if os.name == "nt" else ""
        self.linker = os.path.join(self.mingw, "bin", "x86_64-w64-mingw32-clang" + exe)
        os.makedirs(os.path.dirname(self.linker))
        open(self.linker, "wb").close()
        self.cache = os.path.join(self.dir, "cache")
        self.cc = os.path.join(self.dir, "cc" + exe)   # (linux: the C compiler that links it)
        open(self.cc, "wb").close()
        os.chmod(self.cc, 0o755)
        self.bin = os.path.join(self.cache, "rust", "bin")

    def build(self, dll, target="windows"):
        calls = []

        def run(cmd, cwd=None, env=None, **kw):
            calls.append((cmd, cwd, dict(env)))
            for f in ("Cargo.toml", "Cargo.lock", "build.rs", os.path.join("src", "lib.rs"),   # (the tracked source)
                      os.path.join("vendor", "opus", "src", "opus_encoder.c"), os.path.join("vendor", "opus", "COPYING")):
                assert os.path.isfile(os.path.join(cwd, f)), f
            assert not os.path.exists(os.path.join(cwd, "vendor", "opus", "celt", "opus_custom_demo.c"))
            with open(os.path.join(env["CARGO_HOME"], "config.toml"), encoding="utf-8") as f:
                assert 'replace-with = "pinned"' in f.read()
            made = os.path.join(env["CARGO_TARGET_DIR"], rh.TARGETS[target], "release")
            os.makedirs(made)
            with open(os.path.join(made, rh.BUILT[target]), "wb") as f:
                f.write(dll)
            return subprocess.CompletedProcess(cmd, 0)
        out = os.path.join(self.dir, "out", "renhdrum.dll")
        with mock.patch.object(rh, "toolchain", lambda cache, target: self.bin), mock.patch.object(rh.subprocess, "run", run), \
                mock.patch.object(rh, "vendor_crates", lambda cache, into: os.makedirs(into)), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            try:
                rc = rh.main(["build", "--target", target, "--llvm-mingw", self.mingw, "--linker", self.cc, "--cache",
                              self.cache, "--out", out])
            except SystemExit as e:
                rc = e.code
        return rc, out, calls

    def test_the_build(self):
        rc, out, calls = self.build(pe())
        self.assertEqual(rc, 0)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), pe())
        (cmd, cwd, env), = calls
        self.assertEqual(cmd[1:], ["build", "--release", "--locked", "--offline", "--lib", "--target",
                                   "x86_64-pc-windows-gnullvm"])
        self.assertTrue(cmd[0].startswith(self.bin))
        self.assertFalse(cwd.startswith(str(ROOT)), "the build writes into the checkout")
        self.assertEqual(env["CARGO_TARGET_X86_64_PC_WINDOWS_GNULLVM_LINKER"], self.linker)
        flags = env["RUSTFLAGS"]
        for want in ("-C target-feature=+crt-static", "--no-insert-timestamp", "--remap-path-prefix=%s=renhdrum" % cwd):
            self.assertIn(want, flags)
        self.assertTrue(env["CARGO_HOME"].startswith(self.cache))
        self.assertEqual(env["CC_x86_64_pc_windows_gnullvm"], self.linker)   # (libopus: the compiler that links it)
        self.assertEqual(env["AR_x86_64_pc_windows_gnullvm"], os.path.join(self.mingw, "bin", "llvm-ar" + (".exe" if os.name == "nt" else "")))
        self.assertIn("-ffile-prefix-map=", env["CFLAGS_x86_64_pc_windows_gnullvm"])
        self.assertFalse(os.path.exists(os.path.join(self.cache, "work")), "the work folder is left behind")

    def test_the_linux_build(self):
        rc, out, calls = self.build(elf(), "linux")
        self.assertEqual(rc, 0)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), elf())
        (cmd, cwd, env), = calls
        self.assertEqual(cmd[-2:], ["--target", "x86_64-unknown-linux-gnu"])
        self.assertIn("--locked", cmd)
        self.assertIn("--offline", cmd)
        self.assertEqual(os.path.normcase(env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"]), os.path.normcase(self.cc))
        self.assertEqual(os.path.normcase(env["CC_x86_64_unknown_linux_gnu"]), os.path.normcase(self.cc))
        self.assertIn("--build-id=none", env["RUSTFLAGS"])
        self.assertNotIn("crt-static", env["RUSTFLAGS"])   # (a glibc shared library: the C library stays the system's)
        self.assertEqual(rh.NAMES["linux"], "renhdrum.so")   # (host/rumble_renhdrum.c's name for it)

    def test_a_bad_build_is_not_copied(self):
        rc, out, _ = self.build(pe(imports=GOOD_IMPORTS + ("libunwind.dll",)))
        self.assertEqual(rc, 1)
        self.assertFalse(os.path.exists(out))

    def test_no_llvm_mingw(self):
        with contextlib.redirect_stderr(io.StringIO()), mock.patch.dict(os.environ, {"LONGSHOT_MINGW": ""}), \
                self.assertRaises(SystemExit):
            rh.main(["build", "--target", "windows", "--llvm-mingw", os.path.join(self.dir, "nothing"), "--cache",
                     self.cache])

    def test_a_download_that_is_not_the_pin(self):
        class Body(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False
        with mock.patch.object(rh.helpers.urllib.request, "urlopen", lambda req, timeout=0: Body(b"not rust")), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit):
            rh.fetch("rustc-x", "0" * 64, self.cache)
        self.assertEqual(os.listdir(self.cache), [])

    def test_the_locked_crates(self):
        with open(ROOT / "third_party" / "renhdrum" / "Cargo.lock", encoding="utf-8") as f:
            crates = rh.locked_crates(f.read())
        self.assertIn("cc", [n for n, _, _ in crates])
        for name, version, sha in crates:
            self.assertRegex(sha, r"^[0-9a-f]{64}$", name)
        git = '[[package]]\nname = "x"\nversion = "1.0.0"\nsource = "git+https://example.invalid/x"\n'
        unsummed = '[[package]]\nname = "x"\nversion = "1.0.0"\nsource = "registry+https://github.com/rust-lang/crates.io-index"\n'
        for lock in (git, unsummed):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                rh.locked_crates(lock)

    def test_a_crate_from_the_cache(self):
        crate = os.path.join(self.dir, "made")
        os.makedirs(os.path.join(crate, "x-1.0.0"))
        with open(os.path.join(crate, "x-1.0.0", "Cargo.toml"), "w") as f:
            f.write("[package]\n")
        archive = os.path.join(self.cache, "crates", "x-1.0.0.crate")
        os.makedirs(os.path.dirname(archive))
        import tarfile
        with tarfile.open(archive, "w:gz") as t:
            t.add(os.path.join(crate, "x-1.0.0"), "x-1.0.0")
        sha = hashlib.sha256(open(archive, "rb").read()).hexdigest()
        into = os.path.join(self.dir, "crates")

        def no_network(*a, **k):
            raise AssertionError("downloaded a crate the cache holds")
        with mock.patch.object(rh, "locked_crates", lambda lock: [("x", "1.0.0", sha)]), \
                mock.patch.object(rh.helpers, "download", no_network):
            rh.vendor_crates(self.cache, into)
        self.assertTrue(os.path.isfile(os.path.join(into, "x-1.0.0", "Cargo.toml")))
        with open(os.path.join(into, "x-1.0.0", ".cargo-checksum.json"), encoding="utf-8") as f:
            self.assertIn('"package":"%s"' % sha, f.read())

    def test_every_part_is_pinned(self):
        for system, parts in rh.RUST.items():
            for name, sha in parts + list(rh.RUST_STD.values()):
                self.assertIn(rh.RUST_VERSION, name)
                self.assertRegex(sha, r"^[0-9a-f]{64}$", name)
        self.assertEqual(rh.RUST_STD["windows"][0], "rust-std-%s-x86_64-pc-windows-gnullvm" % rh.RUST_VERSION)
        self.assertEqual(rh.RUST_STD["linux"][0], "rust-std-%s-x86_64-unknown-linux-gnu" % rh.RUST_VERSION)


class Ships(unittest.TestCase):
    def test_the_source(self):
        here = ROOT / "third_party" / "renhdrum"
        for f in ("Cargo.toml", "Cargo.lock", "build.rs", "src/lib.rs", "src/ffi.rs", "include/renhdrum.h", "NOTICE",
                  "LICENSE-MIT", "LICENSE-APACHE"):
            self.assertTrue((here / f).is_file(), f)
        note = (here / "PROVENANCE.md").read_text(encoding="utf-8")
        self.assertIn("upstream: https://github.com/trzz1214/RenHDrum/archive/refs/tags/v0.5.0.tar.gz", note)
        self.assertRegex(note, r"\nupstream-sha256: [0-9a-f]{64}\n")
        lock = (here / "Cargo.lock").read_text(encoding="utf-8")
        toml = (here / "Cargo.toml").read_text(encoding="utf-8").replace("\r", "")
        self.assertRegex(toml, r"(?m)^\[dependencies\]\n\s*(^\[|\Z)",
                         "RenHDrum gained a dependency the library links: review it, and its licence")
        self.assertEqual(lock.count("[[package]]") - 1, len(rh.locked_crates(lock)))
        # (build.rs compiles libopus from vendor/opus, which the build fills from third_party/opus: the same release)
        self.assertIn('Path::new("vendor/opus")', (here / "build.rs").read_text(encoding="utf-8"))
        self.assertIn('.define("PACKAGE_VERSION", "\\"1.5.2\\"")', (here / "build.rs").read_text(encoding="utf-8"))
        self.assertIn("libopus 1.5.2", (ROOT / "third_party" / "opus" / "PROVENANCE.md").read_text(encoding="utf-8"))
        header = (here / "include" / "renhdrum.h").read_text(encoding="utf-8")
        self.assertIn("#define RENHDRUM_VERSION 0x000500", header)
        module = (ROOT / "host" / "rumble_renhdrum.c").read_text(encoding="utf-8")
        self.assertIn('#include "../third_party/renhdrum/include/renhdrum.h"', module)
        # (every function the module binds is one the check wants, and is in the header)
        bound = ["renhdrum_" + n for n in re.findall(r"^\s*X\([^,]+, (\w+), \(", module, re.M)]
        self.assertEqual(sorted(bound), sorted(EXPORTS))
        for name in bound:
            self.assertIn(name + "(", header)

    def test_the_release_builds_and_packs_it(self):
        wf = (ROOT / ".forgejo" / "workflows" / "build-windows.yml").read_text(encoding="utf-8")
        step = wf[wf.index("- name: renhdrum.dll"):wf.index("- name: Package folder")]
        self.assertIn('scripts/renhdrum.py build --target windows --llvm-mingw "$LLVM_MINGW"', step)
        package = wf[wf.index("- name: Package folder"):]
        self.assertIn('--renhdrum "$RUNNER_TEMP/renhdrum/renhdrum.dll"', package[:package.index("- name: The zip")])
        checker = load("check_windows_files_under_test", ROOT / "scripts" / "check_windows_files.py")
        self.assertIn("tools/third_party/renhdrum/renhdrum.dll", checker.BUILDER)

    def test_the_package(self):
        bp = load("builder_package_under_test", ROOT / "scripts" / "builder_package.py")
        with tempfile.TemporaryDirectory() as d:
            publish, setup = os.path.join(d, "publish"), os.path.join(d, "setup.exe")
            os.makedirs(publish)
            open(setup, "wb").close()
            for name, data, want in (("good.dll", pe(), 0), ("bad.dll", pe(exports=()), 1), ("elf.dll", elf(), 1)):
                dll, out = os.path.join(d, name), os.path.join(d, "out-" + name)
                with open(dll, "wb") as f:
                    f.write(data)
                a = mock.MagicMock(out=out, publish=publish, setup=setup, third_party_objects=None, renhdrum=dll)
                with mock.patch.object(bp, "tracked", lambda *p: []), mock.patch.object(bp, "state_list", lambda t: 0), \
                        contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(bp.package(a), want, name)
                shipped = os.path.join(out, "tools", "third_party", "renhdrum", "renhdrum.dll")
                self.assertEqual(os.path.isfile(shipped), want == 0, name)
        # (a player's build takes it from there: the tools' copy of the checkout's layout)
        build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        self.assertIn('rh = os.path.join(kit.HERE, "..", "third_party", "renhdrum", name)', build)
        self.assertIn('copy_renhdrum(out, "renhdrum.dll")', build)
        self.assertIn('copy_renhdrum(out, "renhdrum.so")', build)


if __name__ == "__main__":
    unittest.main()
