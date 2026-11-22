"""The Windows Longshot Builder's release zip (docs/dev/reference/releases.md), made in CI (.forgejo/workflows/release.yml, ci.yml). No game
data is involved. Python's standard library only.

    python -I scripts/builder_package.py llvm-mingw <folder> [--name] [--host windows|linux]
        llvm-mingw (its SHA-256 checked) unpacked into <folder>; prints its folder. --name: only its folder's name (a cache
        key). --host (default: this system): windows is tools.lock's, the one players get; linux is the same release's Linux
        build (LINUX_LLVM_MINGW below), which the release's Windows files are made with on the Linux runner
    python -I scripts/builder_package.py package <out folder> --publish <dotnet publish folder> --setup <setup exe>
            [--third-party-objects <folder>] [--renhdrum <dll>]
        the builder's folder, LongshotBuilder\\ in the zip: the published builder, the setup (a game folder's longshot.exe:
        make_game.py copies it there), LICENSE,
        the tracked gpu, host, nx2, scripts and third_party under tools\\, the play profiles (assets/profiles/: the hot list
        and the renderer's profile, which make_game.py builds with), the logo (assets/longshot.png), and the pipeline state list's sources
        (assets/pipeline_states/, which must be there and read). None of tools.lock's tools: the setup or the launcher adds
        those on the player's PC. --third-party-objects: the third-party code's objects (nx2/third_party_objects.py make, with
        llvm-mingw and tools.lock's cpm), as tools\\third_party_objects, which a player's build links instead
        of compiling them. --renhdrum: renhdrum.dll (scripts/renhdrum.py build), as tools\\third_party\\renhdrum\\renhdrum.dll,
        which a player's build places beside the game (nx2/build.py) for the controllers RenHDrum plays (host/vibration.h)
    python -I scripts/builder_package.py zip <package folder> <version> --setup <setup exe> --out <zip>
        LongshotBuilder-<version>-windows-x64.zip: LongshotBuilder.exe (the setup), LongshotBuilder\\ (the package),
        tools.lock, VERSION and recomplauncher.json (RecompLauncher reads it at the top; release_manifest). Deterministic: the
        same files give the same bytes
    python -I scripts/builder_package.py manifest <version> --out <file>
        recomplauncher.json as a release ships it (release_manifest), for the AppImage (scripts/linux/appimage.sh)
"""
import argparse
import importlib.util
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script's folder off the path)
import helpers  # noqa: E402
import tools  # noqa: E402
from helpers import ROOT  # noqa: E402


# llvm-mingw's Linux build, the same release as tools.lock's (tests/test_windows_on_linux.py keeps the two the same): CI has no
# Windows runner (docs/dev/reference/ci.md), so the setup, the Windows Vulkan back end and dav1d are compiled on Linux with it.
# (version, folder in the archive, SHA-256, size, URL), as a tools.lock line has them
LINUX_LLVM_MINGW = ("20261104", "llvm-mingw-20261104-ucrt-ubuntu-22.04-x86_64",
                    "919380403de87dca3c581c2053230cd298af5e891286628ba0613b7fed370926", 84104244,
                    "https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/"
                    "llvm-mingw-20261104-ucrt-ubuntu-22.04-x86_64.tar.xz")


def llvm_mingw(a):
    if a.host == "windows":
        t = tools.named(tools.load(), "llvm-mingw")
    else:
        version, prefix, sha256, size, url = LINUX_LLVM_MINGW
        t = tools.Tool("llvm-mingw", version, "linux-x64", sha256, size, "-", prefix, [url])
    if a.name:
        print(t.prefix)
        return 0
    folder = os.path.join(a.folder, t.prefix)
    clang = os.path.join(folder, "bin", "clang.exe" if a.host == "windows" else "clang")
    if not os.path.isfile(clang):
        helpers.extract(tools.fetch(t, os.path.join(a.folder, ".cache")), a.folder)   # (its size and SHA-256 checked)
    print(folder)
    return 0


def tracked(*paths):
    return helpers.git_files(*paths, root=ROOT)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def package(a):
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    shutil.copytree(a.publish, out)
    copy(a.setup, os.path.join(out, "LongshotBuilder-Setup.exe"))   # (a game folder's longshot.exe: make_game.py copies it there)
    copy(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE"))
    dest = os.path.join(out, "tools")
    # (assets/profiles/: the hot list and the renderer's play profile, which nx2/make_game.py looks for under tools/ as in a
    # checkout: without them a player's build compiles every lifted function at -O1 and the renderer without its profile).
    # assets/longshot.png: the logo the renderer compiles in (nx2/logo_rgba.py); without it a player's build drew the start-up
    # screen and the menu's banner with no logo, the wordmark alone
    for f in tracked("gpu", "host", "nx2", "scripts", "third_party", "assets/profiles", "assets/longshot.png"):
        copy(os.path.join(ROOT, f), os.path.join(dest, *f.split("/")))
    if state_list(dest) != 0:
        return 1
    objects = getattr(a, "third_party_objects", None)
    if objects:   # (nx2/third_party_objects.py: found by their keys alone, so a set that does not fit is not used)
        if not os.path.isfile(os.path.join(objects, "manifest.json")):
            print("error: %s has no manifest.json (nx2/third_party_objects.py make)" % objects, file=sys.stderr)
            return 1
        shutil.copytree(objects, os.path.join(dest, "third_party_objects"))
    dll = getattr(a, "renhdrum", None)
    if dll:   # (scripts/renhdrum.py build: where nx2/build.py looks for it, as in a checkout)
        import renhdrum
        with open(dll, "rb") as f:
            bad = renhdrum.problems(f.read(), "windows")   # (a Linux library under the name: a "Bad Image" box on Windows)
        if bad:
            print("error: %s: %s" % (dll, "; ".join(bad)), file=sys.stderr)
            return 1
        copy(dll, os.path.join(dest, "third_party", "renhdrum", "renhdrum.dll"))
    print("package folder: %s" % out)
    return 0


def state_list(tools):
    """The sources of the pipeline state list a release ships (assets/pipeline_states/: the tour's list, the contributed ones and
    their notes), into tools/ at the same path: nx2/make_game.py merges them beside the game. Required: without them a new
    install's first field load is drawn white for seconds while its pipelines compile. 1, with the reason, when there are
    none or one does not read (nx2/pipeline_states.py check)"""
    spec = importlib.util.spec_from_file_location("pipeline_states", os.path.join(ROOT, "nx2", "pipeline_states.py"))
    states = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(states)
    src = states.sources_dir(ROOT)
    try:
        recs = states.check_sources(src)
    except SystemExit as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    dst = states.sources_dir(tools)
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)
    print("the pipeline state list: %s" % states.describe(recs))
    return 0


def release_manifest(version):
    """recomplauncher.json as a release ships it: its prepare.version is the release's version. RecompLauncher keeps a game set
    up through an update and builds it again only when prepare.version changes, and every Longshot release needs a new build
    (the game is what the builder makes): with a fixed version, a player who updated kept playing the build of the release they
    first set up. With prepare.keeps_copy that build runs from the game files the folder kept, without the player's copy"""
    with open(os.path.join(ROOT, "recomplauncher.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    manifest["prepare"]["version"] = version
    return (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def write_manifest(version, out):
    with open(out, "wb") as f:
        f.write(release_manifest(version))
    print("%s: prepare.version %s" % (out, version))
    return 0


def make_zip(package_dir, version, setup, out):
    entries = {"LongshotBuilder.exe": setup, "tools.lock": os.path.join(ROOT, "tools.lock"),
               "recomplauncher.json": release_manifest(version), "VERSION": (version + "\n").encode(),
               # (the release again where the builder and the game read it: Runner.cs's Version, nx2/version.py's release(),
               # and through them the game folder's longshot_version.txt and the first line of longshot_log.txt)
               "LongshotBuilder/tools/version.txt": (version + "\n").encode()}
    entries.update(("LongshotBuilder/" + rel, full) for rel, full in helpers.walk_files(package_dir))
    helpers.write_zip(out, entries)
    print("%s: %d files, %d MB" % (out, len(entries), os.path.getsize(out) >> 20))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("llvm-mingw")
    s.add_argument("folder", nargs="?", default="")
    s.add_argument("--name", action="store_true")
    s.add_argument("--host", choices=("windows", "linux"), default="windows" if os.name == "nt" else "linux")
    s = sub.add_parser("package")
    s.add_argument("out")
    s.add_argument("--publish", required=True)
    s.add_argument("--setup", required=True)
    s.add_argument("--third-party-objects")
    s.add_argument("--renhdrum")
    s = sub.add_parser("zip")
    s.add_argument("package")
    s.add_argument("version")
    s.add_argument("--setup", required=True)
    s.add_argument("--out", required=True)
    s = sub.add_parser("manifest")
    s.add_argument("version")
    s.add_argument("--out", required=True)
    a = p.parse_args()
    if a.cmd == "zip":
        return make_zip(a.package, a.version, a.setup, a.out)
    if a.cmd == "manifest":
        return write_manifest(a.version, a.out)
    return {"llvm-mingw": llvm_mingw, "package": package}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
