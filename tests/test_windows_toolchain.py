"""The Windows build needs no emsdk: every compiler and LLVM tool it looks up is llvm-mingw's.

tools.lock's Windows lines once carried emsdk (115 MiB, Hookshot v2.1.0's), which every Windows install downloaded. With
llvm-mingw set (nx2/make_game.py bundled_mingw, the builder's tools/llvm-mingw), third_party/a64-kit/tools/toolchain.py
find takes clang, clang++, lld and llvm-* from llvm-mingw's bin and looks in EMSDK only for a tool llvm-mingw lacks. This
check lists the tools the build looks up (find("<name>") in the build's Python) and fails when one is not in llvm-mingw's
bin, which would send a Windows build back to an emsdk it no longer has; and it runs find with a made-up llvm-mingw and an
EMSDK that holds nothing, as a Windows build does now.
python -I -B tests/test_windows_toolchain.py
"""
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party" / "a64-kit" / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))
import toolchain   # noqa: E402
import tools       # noqa: E402

# (the LLVM tools in bin of llvm-mingw-20261104-ucrt-x86_64.zip, tools.lock's, listed from the release's zip. A newer
# llvm-mingw in tools.lock: list its bin again, and keep only names it still has)
LISTED_VERSION = "20261104"
LLVM_MINGW_BIN = {
    "clang", "clang++", "ld.lld", "llvm-addr2line", "llvm-ar", "llvm-cov", "llvm-cvtres", "llvm-cxxfilt", "llvm-dlltool",
    "llvm-lib", "llvm-ml", "llvm-nm", "llvm-objcopy", "llvm-objdump", "llvm-pdbutil", "llvm-profdata", "llvm-ranlib", "llvm-rc",
    "llvm-readelf", "llvm-readobj", "llvm-size", "llvm-strings", "llvm-strip", "llvm-symbolizer", "llvm-windres",
}
# (where the build's Python looks its tools up; tests/ is left out, its checks find their own compilers)
SOURCES = ("nx2", "scripts", "builder", os.path.join("third_party", "a64-kit", "tools"))
LOOKUP = re.compile(r'\bfind\("(clang\+\+|clang|lld|llvm-[\w-]+)"')


def bin_name(tool):
    return "ld.lld" if tool == "lld" else tool   # (toolchain.find: lld is llvm-mingw's ld.lld)


def looked_up():
    found = {}
    for top in SOURCES:
        for dirpath, _, files in os.walk(ROOT / top):
            for f in files:
                if f.endswith(".py"):
                    path = Path(dirpath) / f
                    for name in LOOKUP.findall(path.read_text(encoding="utf-8", errors="replace")):
                        found.setdefault(name, path.relative_to(ROOT).as_posix())
    return found


def main():
    bad = []
    mingw = tools.named(tools.load(str(ROOT / "tools.lock")), "llvm-mingw")
    if mingw.version != LISTED_VERSION:
        bad.append("tools.lock's llvm-mingw is %s, LLVM_MINGW_BIN lists %s's bin: list the new one's" % (mingw.version, LISTED_VERSION))
    if "emsdk" in [t.name for t in tools.of_system(tools.load(str(ROOT / "tools.lock")), "windows-x64")]:
        bad.append("tools.lock has an emsdk for Windows again: nothing of the Windows build uses it")
    names = looked_up()
    for want in ("clang", "clang++"):
        if want not in names:
            bad.append("no find(%r) found in %s: the scan no longer sees the build's lookups" % (want, ", ".join(SOURCES)))
    for name, where in sorted(names.items()):
        if bin_name(name) not in LLVM_MINGW_BIN:
            bad.append("%s looks up %s, which llvm-mingw does not have: a Windows build would look for it in an emsdk that "
                       "tools.lock no longer installs" % (where, name))
    saved = {k: os.environ.get(k) for k in ("RX_TARGET", "LONGSHOT_MINGW", "EMSDK", "PATH")}
    try:
        with tempfile.TemporaryDirectory() as d:
            mingw_dir, empty = Path(d) / "llvm-mingw", Path(d) / "emsdk"
            (mingw_dir / "bin").mkdir(parents=True)
            empty.mkdir()
            ext = ".exe" if os.name == "nt" else ""
            for name in names:
                (mingw_dir / "bin" / (bin_name(name) + ext)).write_bytes(b"")
            os.environ.update(RX_TARGET="windows", LONGSHOT_MINGW=str(mingw_dir), EMSDK=str(empty), PATH=str(empty))
            for name in sorted(names):
                got = toolchain.find(name)
                if Path(got).parent != mingw_dir / "bin":
                    bad.append("toolchain.find(%r) gave %s, not llvm-mingw's" % (name, got))
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    if bad:
        raise SystemExit("FAIL:\n  " + "\n  ".join(bad))
    print("PASS: the Windows build's %d tools (%s) are llvm-mingw's; no emsdk" % (len(names), ", ".join(sorted(names))))


if __name__ == "__main__":
    main()
