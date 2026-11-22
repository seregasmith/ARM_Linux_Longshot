"""What the build, SDK and third-party scripts share: the checkout's root, SHA-256 of a file, pinned downloads, archives
(read, unpacked, written deterministically), atomic writes and subprocesses that stop with a clear message. Python's
standard library only.

tools.py, fetch.py, renhdrum.py, check_third_party.py, builder_package.py and shaders.py import it. python -I leaves
scripts/ off sys.path, so each puts its own folder there first. (The release tool, release.py, lives in MagicBeanDev/Tooling
and does not import it.)
"""
import hashlib
import io
import os
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZIP_DATE = (2026, 1, 1, 0, 0, 0)   # (every member's date: the same files give the same zip)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def urlopen(url, timeout):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "longshot"}), timeout=timeout)


def read_url(url, timeout=120):
    with urlopen(url, timeout) as r:
        return r.read()


def download(urls, dest, sha256, size=None, timeout=600, tries=1, opener=None, progress=None):
    """The first of urls (one URL or a list) that gives bytes with this SHA-256, into dest through dest.part, which is
    removed whatever happens: True when downloaded, False when dest has those bytes already. A URL whose connection fails
    is asked again, tries times in all; more than size bytes are refused. Raises OSError (no answer) or ValueError (other
    bytes) for the last URL. opener(url, timeout=...): a file-like answer in place of urllib's (the tests);
    progress(bytes so far) after each block."""
    if os.path.isfile(dest) and sha256_file(dest) == sha256:
        return False
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    part, error = dest + ".part", OSError("no URL")
    for url in [urls] if isinstance(urls, str) else urls:
        for _ in range(tries):
            try:
                _fetch(url, part, sha256, size, timeout, opener or urlopen, progress)
                os.replace(part, dest)
                return True
            except ValueError as e:
                error = e
                break   # (other bytes: asking again gives the same)
            except OSError as e:
                error = OSError("%s: %s" % (url, e))
            finally:
                if os.path.exists(part):
                    os.remove(part)
    raise error


def _fetch(url, part, sha256, size, timeout, opener, progress):
    h, have = hashlib.sha256(), 0
    with opener(url, timeout=timeout) as r, open(part, "wb") as f:
        for block in iter(lambda: r.read(1 << 20), b""):
            have += len(block)
            if size is not None and have > size:
                raise ValueError("%s: larger than the %d bytes pinned" % (url, size))
            h.update(block)
            f.write(block)
            if progress:
                progress(have)
    if h.hexdigest() != sha256:
        raise ValueError("%s: not the file pinned (SHA-256 %s, want %s): not used" % (url, h.hexdigest(), sha256))


def archive_files(source, lead=""):
    """{name less lead: bytes} of the files whose name starts with lead in a zip or a tar (.gz, .xz, .bz2); source is the
    archive's path or its bytes"""
    f = io.BytesIO(source) if isinstance(source, bytes) else open(source, "rb")
    with f:
        if zipfile.is_zipfile(f):
            with zipfile.ZipFile(f) as z:
                return {m.filename[len(lead):]: z.read(m) for m in z.infolist()
                        if not m.is_dir() and m.filename.startswith(lead)}
        f.seek(0)
        with tarfile.open(fileobj=f) as t:
            return {m.name[len(lead):]: t.extractfile(m).read() for m in t.getmembers()
                    if m.isfile() and m.name.startswith(lead)}


def extract(archive, folder):
    """A zip's or a tar's members into folder. A tar goes through the "data" filter where Python has it (3.12): nothing
    written outside folder, its links inside it kept"""
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            z.extractall(folder)
        return
    with tarfile.open(archive) as t:
        t.extractall(folder, **({"filter": "data"} if hasattr(tarfile, "data_filter") else {}))


def walk_files(folder):
    """[(path relative to folder with /, full path)] of every file under folder"""
    return [(os.path.relpath(os.path.join(d, n), folder).replace(os.sep, "/"), os.path.join(d, n))
            for d, _, names in os.walk(folder) for n in names]


def write_zip(path, entries, modes=None):
    """entries {name: bytes, or a file's path} as a deterministic zip at path (through path.new): sorted, ZIP_DATE,
    deflated at level 6, each member 0644, or, with modes {name: st_mode}, a Unix member with its own mode (a link's
    bytes are its target). Returns (SHA-256, size)"""
    with zipfile.ZipFile(path + ".new", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            if modes:
                info.create_system = 3
            info.external_attr = (modes[name] if modes else 0o644) << 16
            data = entries[name]
            if not isinstance(data, bytes):
                with open(data, "rb") as f:
                    data = f.read()
            z.writestr(info, data, compresslevel=6)
    os.replace(path + ".new", path)
    return sha256_file(path), os.path.getsize(path)


def write_text(path, text):
    """text into path as UTF-8 with \\n line ends, through path.new: never a half-written file"""
    with open(path + ".new", "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(path + ".new", path)


def run(cmd, what=None, cwd=None):
    """cmd's standard output (text). SystemExit naming what (default: the program) with its exit status and its output
    when it fails, or with the reason when it cannot start"""
    what = what or os.path.basename(str(cmd[0]))
    try:
        r = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True)
    except OSError as e:
        raise SystemExit("%s: %s" % (what, e))
    if r.returncode:
        raise SystemExit("%s failed (%d)\n%s%s" % (what, r.returncode, r.stdout.decode("utf-8", "replace"),
                                                   r.stderr.decode("utf-8", "replace")))
    return r.stdout.decode("utf-8", "replace")


def git_files(*paths, root=ROOT):
    """The files git tracks under paths, relative to root with /"""
    return [f for f in run(["git", "ls-files", "-z", "--", *paths], "git ls-files", cwd=root).split("\0") if f]
