"""tools.lock: the build tools the Longshot Builders need, windows-x64 and linux-x64 (docs/dev/reference/releases.md,
"tools.lock" and "Bumping a tool"). Python's standard library only. --system picks a system's lines (default
windows-x64). The Linux builder reads its lines with this file too (builder/linux/linux_tools.py).

    python -I scripts/tools.py check [--require [<name> ...]]
        reads tools.lock and refuses a bad line (name, SHA-256, size, a path that leaves the install, a link that is not
        https). --require alone: every tool each builder needs (REQUIRED), and each line's copy in Longshot's store there
        with its size and SHA-256 (the release workflow); --require <name>...: those tools
    python -I scripts/tools.py install <folder> [--system <s>] [--cache <folder>] [--only <name> ...]
        each tool of the system (or the ones named) unpacked into a builder install: <prefix> of the archive becomes <dest>
    python -I scripts/tools.py mirror <name> [--system <s>] [--cache <folder>]
        the tool's archive, fetched from its URLs, put in Longshot's store; that copy becomes its line's first URLs
    python -I scripts/tools.py add <name> <version> <archive> --dest <folder> [--prefix <folder>] [--system <s>]
        an archive Longshot made, uploaded to Longshot's store and written into tools.lock in place of the tool's line
    python -I scripts/tools.py pack <folder> --out <zip>
        a folder as a tool's archive under its own name, with its files' modes and its relative links (scripts/linux/
        deps.sh tools packs the Linux tools with it). Then `add` it
    python -I scripts/tools.py assemble python --requirements nx2/prompt_requirements.txt --out <zip>
    python -I scripts/tools.py assemble cpm --add <CPM folder> --out <zip>
    python -I scripts/tools.py assemble third-party --from <archive with tools/third_party> --out <zip>
        a new archive from the tool's current one with what Longshot adds: the pinned prompt packages (pip, for the
        archive's Python and win_amd64), packages for the CPM cache (dav1d: scripts/windows/dav1d.sh), or only the prebuilt
        files of third_party (a64emit.dll, lld-link.exe, SDL2.dll, SDL3.dll, zbic.dll). Then `add` it

Store: RELEASE_TOKEN (your token) to upload; FORGEJO_UPLOAD_API (<server>/api/v1) uploads past Cloudflare's 100 MB limit.
Every archive this makes is deterministic: the same files give the same bytes.
"""
import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script's folder off the path)
import helpers  # noqa: E402
from helpers import ROOT, sha256_file  # noqa: E402,F401  (sha256_file: the tests)

LOCK = os.path.join(ROOT, "tools.lock")
CACHE = os.path.join(ROOT, "build-tools-cache")
OWNER = "MagicBeanDev"
SERVER = "https://git.magicbean.dev"
# (the same files from Cloudflare's edge: git-cdn serves git.magicbean.dev's package paths from its R2 store, cached)
CDN = "https://git-cdn.magicbean.dev"
SYSTEMS = ("windows-x64", "linux-x64")
# (the tools each builder needs: the release workflow refuses a lock without one of them. The Linux AppImage carries its
# Python; the rest is here, builder/linux/linux_tools.py)
REQUIRED = {"windows-x64": ("llvm-mingw", "python", "cpm", "third-party"),
            "linux-x64": ("emsdk", "cpm", "ffmpeg", "sdl2", "sysroot")}
NAME = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
VERSION = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$")
PREBUILT_EXT = (".dll", ".exe", ".lib", ".sha1")
STAMPS = ".tools"


class Tool(object):
    def __init__(self, name, version, system, sha256, size, dest, prefix, urls):
        self.name, self.version, self.system, self.sha256 = name, version, system, sha256
        self.size, self.dest, self.prefix, self.urls = size, dest, prefix, urls

    def line(self):
        return "tool %s %s %s %s %d %s %s %s" % (self.name, self.version, self.system, self.sha256, self.size, self.dest,
                                                self.prefix, " ".join(self.urls))

    def lead(self):
        """The archive's member names start so for what becomes dest"""
        return "" if self.prefix == "-" else self.prefix.rstrip("/") + "/"


def fail(message):
    raise SystemExit("tools.py: " + message)


# ---- tools.lock ----

def path_ok(p):
    """a relative path with / that stays inside the folder it is under (as builder/setup/setup.c's list_name_ok)"""
    parts = p.split("/")
    return bool(p) and not p.startswith("/") and "\\" not in p and ":" not in p and all(x not in ("", ".", "..") for x in parts) \
        and all(re.match(r"^[0-9A-Za-z._+-]+$", x) for x in parts)


def parse(text):
    tools = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        w = line.split()
        if w[0] != "tool" or len(w) < 9:
            fail("tools.lock line %d: not 'tool <name> <version> <system> <sha256> <size> <dest> <prefix> <url>...'" % n)
        name, version, system, sha, size, dest, prefix, urls = w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8:]
        problems = [why for ok, why in (
            (NAME.match(name), "name"), (VERSION.match(version), "version"), (system in SYSTEMS, "system"),
            (re.fullmatch(r"[0-9a-f]{64}", sha), "sha256"), (size.isdigit() and 0 < int(size) < (1 << 34), "size"),
            (path_ok(dest), "dest"), (prefix == "-" or path_ok(prefix), "prefix"),
            (all(u.startswith("https://") and " " not in u for u in urls), "url")) if not ok]
        if problems:
            fail("tools.lock line %d (%s): bad %s" % (n, name, ", ".join(problems)))
        if any(t.name == name and t.system == system for t in tools):
            fail("tools.lock line %d: %s for %s twice" % (n, name, system))
        tools.append(Tool(name, version, system, sha, int(size), dest, prefix, urls))
    return tools


def load(path=LOCK):
    with open(path, encoding="utf-8") as f:
        return parse(f.read())


def named(tools, name, system="windows-x64"):
    for t in tools:
        if t.name == name and t.system == system:
            return t
    fail("no %s for %s in tools.lock" % (name, system))


def of_system(tools, system):
    return [t for t in tools if t.system == system]


def write_line(tool, path=LOCK):
    """tool's line in place of the one with its name and system, or added at the end; the comments stay"""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    mine = [len(w) > 3 and w[0] == "tool" and w[1] == tool.name and w[3] == tool.system for w in map(str.split, lines)]
    out = [tool.line() if m else line for m, line in zip(mine, lines)] + ([] if any(mine) else [tool.line()])
    parse("\n".join(out))
    helpers.write_text(path, "\n".join(out) + "\n")


# ---- fetching and unpacking ----

def fetch(tool, cache=CACHE, getter=None, progress=None):
    """The tool's archive in cache as <sha256>.zip (one cache for every version), from its first URL that gives the right
    bytes. getter(url) -> bytes in place of the download (the tests); progress(bytes so far) while it downloads"""
    path = os.path.join(cache, tool.sha256 + ".zip")
    opener = (lambda url, timeout: io.BytesIO(getter(url))) if getter else None
    try:
        helpers.download(tool.urls, path, tool.sha256, tool.size, opener=opener, progress=progress)
    except (OSError, ValueError) as e:
        fail("%s: %s" % (tool.name, e))
    return path


def link_ok(rel, target):
    """A link at rel (inside the folder) to target: relative, its ".." first, and landing inside the folder (as the
    launcher's Toolchains.Symlink)"""
    parts = target.split("/")
    ups = 0
    while ups < len(parts) and parts[ups] == "..":
        ups += 1
    if not target or target.startswith("/") or "\\" in target or ".." in parts[ups:]:
        return False
    return ups <= len(rel.split("/")) - 1


def unpack(archive, prefix, dest):
    """The archive's members under prefix ("-": all) into dest, which must not exist yet, through dest.part: nothing
    outside dest is written and nothing half unpacked is left. A member made on Linux keeps its mode (executable or not)
    and, when it is a link, stays a link, relative and inside dest"""
    if os.path.exists(dest):
        fail("%s is there already" % dest)
    tmp = dest + ".part"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    try:
        n = _unpack_into(archive, "" if prefix == "-" else prefix.rstrip("/") + "/", tmp)
        if not n:
            fail("%s has nothing under %s" % (archive, prefix))
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    os.replace(tmp, dest)
    return n


def _unpack_into(archive, lead, tmp):
    root = os.path.realpath(tmp)
    n = 0
    with zipfile.ZipFile(archive) as z:
        for m in z.infolist():
            if m.is_dir() or not m.filename.startswith(lead):
                continue
            rel = m.filename[len(lead):]
            out = os.path.join(tmp, *rel.split("/"))
            if ".." in rel.split("/") or os.path.commonpath([os.path.realpath(os.path.dirname(out)), root]) != root \
                    or os.path.lexists(out):
                fail("%s: %s leaves the folder, or is there twice" % (archive, m.filename))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            mode = m.external_attr >> 16
            if (mode & 0o170000) == 0o120000:
                target = z.read(m).decode("utf-8")
                if not link_ok(rel, target):
                    fail("%s: %s links outside the folder (%s)" % (archive, m.filename, target))
                os.symlink(target, out)
            else:
                with z.open(m) as src, open(out, "wb") as f:
                    shutil.copyfileobj(src, f)
                if mode & 0o111:
                    os.chmod(out, 0o755)
            n += 1
    return n


def stamp_of(folder, name):
    """The SHA-256 of the archive the tool in folder was unpacked from (folder/.tools/<name>), or None"""
    try:
        with open(os.path.join(folder, STAMPS, name), encoding="utf-8") as f:
            return f.read().split()[0]
    except (OSError, IndexError):
        return None


def write_stamp(folder, tool):
    os.makedirs(os.path.join(folder, STAMPS), exist_ok=True)
    with open(os.path.join(folder, STAMPS, tool.name), "w", encoding="utf-8", newline="\n") as f:
        f.write("%s %s %s\n" % (tool.sha256, tool.prefix, tool.dest))


def install(folder, tools, cache=CACHE, getter=None):
    """Each tool's archive unpacked at its dest under folder, its stamp beside (folder/.tools/<name>). A tool there with
    another stamp (tools.lock changed) is unpacked again; one with none is kept"""
    for t in tools:
        dest = os.path.join(folder, *t.dest.split("/"))
        if os.path.isdir(dest):
            if stamp_of(folder, t.name) in (None, t.sha256):
                print("%-12s there already" % t.name)
                continue
            print("%-12s %s: tools.lock names another archive, unpacked again" % (t.name, t.version))
            shutil.rmtree(dest)
        n = unpack(fetch(t, cache, getter), t.prefix, dest)
        write_stamp(folder, t)
        print("%-12s %s: %d files into %s" % (t.name, t.version, n, t.dest))


# ---- making archives ----

def pack(folder, out):
    """folder as an archive under its own name (the tool's prefix): each file 0755 when it is executable, else 0644, and
    each link as a link (relative, inside the folder; anything else stops)"""
    folder = os.path.abspath(folder)
    top = os.path.basename(folder)
    entries, modes = {}, {}
    for d, dirs, names in os.walk(folder):
        links = [n for n in dirs if os.path.islink(os.path.join(d, n))]   # (a link to a folder stays a link)
        dirs[:] = [n for n in dirs if n not in links]
        for name in names + links:
            full = os.path.join(d, name)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            if os.path.islink(full):
                target = os.readlink(full)
                if not link_ok(rel, target):
                    fail("%s: %s links outside the folder (%s)" % (folder, rel, target))
                data, mode = target.encode("utf-8"), 0o120777
            elif os.path.isfile(full):
                data, mode = full, 0o100755 if os.access(full, os.X_OK) else 0o100644
            else:
                fail("%s: %s is not a file" % (folder, rel))
            entries[top + "/" + rel], modes[top + "/" + rel] = data, mode
    if not entries:
        fail("%s is empty" % folder)
    sha, size = helpers.write_zip(out, entries, modes)
    print("%s: %d files, %d bytes, sha256 %s (prefix %s)" % (out, len(entries), size, sha, top))
    return sha, size


def members(archive, tool):
    return helpers.archive_files(archive, tool.lead())


def assemble_python(tool, archive, requirements, out, pip=None):
    """The Python archive with requirements' pins (name==version) installed into Lib/site-packages, when it lacks one"""
    with open(requirements, encoding="utf-8") as f:
        pins = [p for p in (l.split("#")[0].strip() for l in f.read().splitlines()) if p]
    files = members(archive, tool)
    have = {m.split("/")[-2].lower() for m in files if "/" in m and m.split("/")[-2].endswith(".dist-info")}
    missing = [p for p in pins if "%s-%s.dist-info" % tuple(x.strip().lower().replace("-", "_") for x in p.split("==")) not in have]
    if not missing:
        print("the Python archive has %s already" % ", ".join(pins))
        return None
    dll = [m for m in files if "/" not in m and m.lower().startswith("python3") and m[7:-4].isdigit()]
    if len(dll) != 1:
        fail("%s: no pythonXY.dll to give the version" % archive)
    version = "3." + dll[0][7:-4]
    stage = out + ".site"
    shutil.rmtree(stage, ignore_errors=True)
    (pip or subprocess.run)([sys.executable, "-m", "pip", "install", "--no-deps", "--only-binary", ":all:", "--no-compile",
                             "--no-cache-dir", "--disable-pip-version-check", "-q", "--platform", "win_amd64", "--python-version",
                             version, "--implementation", "cp", "--target", stage] + missing, check=True)
    files.update(("Lib/site-packages/" + rel, full) for rel, full in helpers.walk_files(stage))
    result = helpers.write_zip(out, {"python/" + rel: v for rel, v in files.items()})
    shutil.rmtree(stage, ignore_errors=True)
    print("python %s with %s: %s" % (version, ", ".join(missing), out))
    return result


def assemble_cpm(tool, archive, add, out):
    """The CPM cache archive with the <package>/<version> folders of add (dav1d), when it lacks one"""
    want = sorted((n, v) for n in os.listdir(add) if os.path.isdir(os.path.join(add, n))
                  for v in os.listdir(os.path.join(add, n)) if os.path.isdir(os.path.join(add, n, v)))
    if not want:
        fail("%s: no <package>/<version> folders" % add)
    files = members(archive, tool)
    have = {tuple(m.split("/")[:2]) for m in files}
    missing = [nv for nv in want if nv not in have]
    if not missing:
        print("the CPM archive has %s already" % ", ".join("/".join(nv) for nv in want))
        return None
    for name, version in missing:
        files.update(("%s/%s/%s" % (name, version, rel), full) for rel, full in helpers.walk_files(os.path.join(add, name, version)))
    result = helpers.write_zip(out, {"cpm/" + rel: v for rel, v in files.items()})
    print("cpm with %s: %s" % (", ".join("/".join(nv) for nv in missing), out))
    return result


def assemble_third_party(source, prefix, out, tracked):
    """The prebuilt files of a tools/third_party (rebuilt on the player's PC when their sources' stamps differ), never a
    source file: third_party/'s sources are the repository's"""
    files = {"third_party/" + rel: data for rel, data in helpers.archive_files(source, prefix.rstrip("/") + "/").items()
             if rel.lower().endswith(PREBUILT_EXT) and ("third_party/" + rel) not in tracked}
    if not files:
        fail("%s: no prebuilt files under %s" % (source, prefix))
    result = helpers.write_zip(out, files)
    print("third-party: %s (%s)" % (out, ", ".join(sorted(f[len("third_party/"):] for f in files))))
    return result


# ---- Longshot's store: Forgejo's generic package registry ----
# Each archive Longshot keeps a copy of, or makes, is a file of a generic package of the MagicBeanDev organization (its
# Packages tab): the package <name>-<system>, its version the tool's, the file <sha256>-<name>.zip. A package version's
# files never change (Forgejo refuses a second upload of a name, HTTP 409) and nothing prunes them. Anyone downloads them
# with no account. Each system has its own package: a tool's Windows and Linux builds have versions of their own.

def store_url(tool, server=SERVER):
    """The tool's copy in Longshot's store (server: the address it is reached by)"""
    return "%s/api/packages/%s/generic/%s-%s/%s/%s-%s.zip" % (server, OWNER, tool.name, tool.system, tool.version,
                                                              tool.sha256, tool.name)


def store_urls(tool):
    """tools.lock's first URLs for a tool in Longshot's store: the edge's copy, then the store's own address"""
    return [store_url(tool, CDN), store_url(tool)]


def in_store(url):
    return url.startswith("%s/api/packages/%s/generic/" % (SERVER, OWNER))


def exists(url, timeout=60):
    """(there, size, sha256) of a file in the store, from the package API's list of its version's files (anonymous; a
    HEAD through Cloudflare gives no size). (False, None, None) when it is not there"""
    package, version, name = url[len("%s/api/packages/%s/generic/" % (SERVER, OWNER)):].split("/")
    req = urllib.request.Request("%s/api/v1/packages/%s/generic/%s/%s/files" % (SERVER, OWNER, package, version),
                                 headers={"User-Agent": "longshot-tools", "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            files = json.load(r)
    except (urllib.error.URLError, OSError, ValueError):
        return False, None, None
    for f in files if isinstance(files, list) else ():
        if isinstance(f, dict) and f.get("name") == name:
            return True, f.get("Size", f.get("size")), f.get("sha256")
    return False, None, None


def store(path, tool):
    """The archive at path, put in Longshot's store as the tool's copy (PUT; a copy there already, 409, is kept when it is
    the same: its bytes cannot change). Returns the copy's URL"""
    url = store_url(tool)
    base = (os.environ.get("FORGEJO_UPLOAD_API") or "").rstrip("/")
    put = store_url(tool, base[:-len("/api/v1")] if base.endswith("/api/v1") else SERVER)
    token = os.environ.get("RELEASE_TOKEN") or fail("RELEASE_TOKEN is needed (your token, with write access to %s's packages)" % OWNER)
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        req = urllib.request.Request(put, data=f, method="PUT", headers={
            "Authorization": "token " + token, "User-Agent": "longshot-tools", "Content-Type": "application/zip",
            "Content-Length": str(size)})
        try:
            with urllib.request.urlopen(req, timeout=1800):
                pass
            print("uploaded %s" % url)
        except urllib.error.HTTPError as e:
            if e.code != 409:
                fail("PUT %s: HTTP %d %s" % (url, e.code, e.read().decode("utf-8", "replace")[:300]))
            there, got, sha = exists(url)
            if not there or got != size or sha != tool.sha256:
                fail("%s is in the package registry already with other bytes (%s bytes, SHA-256 %s): a new version, then"
                     % (url, got, sha))
            print("%s is in the package registry already" % url)
    return url


# ---- commands ----

def check(tools, require):
    """0, or 1 with what is wrong: the tools require names (all of REQUIRED when it is []), and with require given at
    all, each line's copy in the store there as tools.lock names it (upstream fallbacks are not asked)"""
    have = {(t.name, t.system) for t in tools}
    if require == []:
        missing = ["%s (%s)" % (n, s) for s in SYSTEMS for n in REQUIRED[s] if (n, s) not in have]
    else:
        missing = [n for n in require or () if n not in {t.name for t in tools}]
    for t in tools:
        print("%-12s %-22s %s %6d MB  %s" % (t.name, t.version, t.system, t.size >> 20, t.urls[0]))
    if missing:
        print("error: tools.lock is missing: %s (a builder needs them); docs/dev/reference/releases.md, One-time setup"
              % ", ".join(missing), file=sys.stderr)
        return 1
    if require is None:
        return 0
    gone = []
    for t in tools:
        if not any(in_store(u) for u in t.urls):
            continue
        if t.urls[:2] != store_urls(t):
            gone.append("%s (%s): its first URLs are not %s" % (t.name, t.system, " ".join(store_urls(t))))
            continue
        there, size, sha = exists(store_url(t))
        if not there or size != t.size or sha != t.sha256:
            gone.append("%s (%s): %s" % (t.name, t.system, "%s bytes, SHA-256 %s" % (size, sha) if there else "not there"))
    if gone:
        print("error: not in Longshot's store as tools.lock names them: %s; docs/dev/reference/releases.md, Bumping a tool"
              % "; ".join(gone), file=sys.stderr)
        return 1
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--lock", default=LOCK)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("check")
    s.add_argument("--require", nargs="*")
    s = sub.add_parser("install")
    s.add_argument("folder")
    s.add_argument("--only", nargs="*")
    s = sub.add_parser("mirror")
    s.add_argument("name")
    s = sub.add_parser("add")
    s.add_argument("name")
    s.add_argument("version")
    s.add_argument("archive")
    s.add_argument("--dest", required=True)
    s.add_argument("--prefix", default="-")
    s = sub.add_parser("pack")
    s.add_argument("folder")
    s.add_argument("--out", required=True)
    s = sub.add_parser("assemble")
    s.add_argument("what", choices=("python", "cpm", "third-party"))
    s.add_argument("--requirements", default=os.path.join(ROOT, "nx2", "prompt_requirements.txt"))
    s.add_argument("--add")
    s.add_argument("--from", dest="source")
    s.add_argument("--from-prefix", default="HookshotBuilder/tools/third_party")
    s.add_argument("--out", required=True)
    for name in ("install", "mirror", "add"):
        sub.choices[name].add_argument("--system", choices=SYSTEMS, default="windows-x64")
    for name in ("install", "mirror", "assemble"):
        sub.choices[name].add_argument("--cache", default=CACHE)
    a = p.parse_args(argv)
    tools = load(a.lock)

    if a.cmd == "check":
        return check(tools, a.require)
    if a.cmd == "install":
        install(a.folder, [t for t in of_system(tools, a.system) if not a.only or t.name in a.only], a.cache)
    elif a.cmd == "pack":
        pack(a.folder, a.out)
    elif a.cmd == "mirror":
        t = named(tools, a.name, a.system)
        store(fetch(t, a.cache), t)
        first = store_urls(t)
        if t.urls[:2] != first:
            t.urls = first + [u for u in t.urls if u not in first]
            write_line(t, a.lock)
        print("%s: %s" % (t.name, " ".join(t.urls)))
    elif a.cmd == "add":
        t = Tool(a.name, a.version, a.system, sha256_file(a.archive), os.path.getsize(a.archive), a.dest, a.prefix, [])
        store(a.archive, t)
        t.urls = store_urls(t)
        write_line(t, a.lock)
        print("tools.lock: %s %s %s" % (a.name, a.version, t.sha256))
    elif a.what == "python":
        t = named(tools, "python")
        assemble_python(t, fetch(t, a.cache), a.requirements, a.out)
    elif a.what == "cpm":
        t = named(tools, "cpm")
        assemble_cpm(t, fetch(t, a.cache), a.add or fail("--add <CPM folder>"), a.out)
    else:
        assemble_third_party(a.source or fail("--from <archive>"), a.from_prefix, a.out, set(helpers.git_files("third_party")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
