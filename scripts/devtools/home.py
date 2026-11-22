"""This PC's development folder (common.dev_home): what it holds, and the move of what older tools kept elsewhere into it.
Linux and Windows; Python's standard library only. docs/dev/reference/build.md, *Where dev data lives*.

    dev.py paths [NAME]         the development folder and what it holds; NAME (home, work or a part such as bases or
                                stores/lifts): that path alone, for scripts
    dev.py migrate [--dry-run]  move the older places into it (once per PC; --dry-run: say what would move)

migrate moves ~/.cache/longshot-dev (bases/, builder/), ~/.cache/longshot-{objects,lifts,queue} and job_queue.json and
dev_build.json of ~/.config/longshot (Windows: %LOCALAPPDATA%\\Longshot\\{dev,objects,lifts,queue} and those two files of
%APPDATA%\\Longshot) into the development folder (common.old_places). On one drive each is a rename; across drives a copy
(hard links among the copied files kept, clones where the file system makes them), checked file by file before the old one
is deleted. It leaves a link at each old place it emptied (Linux; a junction for a folder on Windows), so a session still
running older tools finds everything where it was; the one exception is the Windows queue folder, which keeps its tickets/:
its history.jsonl, tmp/ and nvcache/ move out and no link is left, so an older queue there starts a fresh history at the old
place (nothing is lost: the moved history is in the development folder). It refuses while the job queue has a ticket or a base is in use, and holds each base's
dev_packs.busy while it moves them. A part already in the development folder is merged: what it lacks moves in, a name both
have stays at the old place, said, for you to compare. Never moved: the player's data (~/.local/share/longshot,
%APPDATA%\\Longshot's saves and options), Tooling's sign-in and chat files beside the two settings files, the queue's tickets
($XDG_RUNTIME_DIR/longshot-queue; on Windows the old queue folder's tickets/, which every version of the queue reads).
"""
import argparse
import contextlib
import errno
import json
import os
import shutil
import sys
import time

from . import common, jobqueue, packs

WHO = "dev.py migrate"


def say(text):
    common.say(WHO, text)


def work_dir():
    """`mise run build`'s work folder: work, or work-windows for the Windows game built on Linux (its objects apart)"""
    cross = not common.WINDOWS and os.environ.get("RX_TARGET", "").strip().lower() == "windows"
    return os.path.join(common.dev_home(), "work-windows" if cross else "work")


def path_of(name):
    if name == "home":
        return common.dev_home()
    if name == "work":
        return work_dir()
    return common.dev_path(name)


def cmd_paths(a):
    if a.name:
        known = ["home"] + [rel for rel, _ in common.LAYOUT]
        if a.name not in known:
            sys.exit("dev.py paths: no %r: one of %s" % (a.name, ", ".join(known)))
        print(path_of(a.name))
        return 0
    home = common.dev_home()
    print("development folder: %s%s" % (home, " (LONGSHOT_DEV_HOME)" if os.environ.get("LONGSHOT_DEV_HOME") else ""))
    old = False
    for rel, what in common.LAYOUT:
        p = work_dir() if rel == "work" else common.dev_path(rel)
        moved = os.path.normcase(p).startswith(os.path.normcase(os.path.join(home, "")))
        old |= not moved
        print("  %-15s %s%s\n  %-15s   %s" % (rel, p, "" if moved else "   (its old place)", "", what))
    if old:
        print("dev.py migrate moves the old places into the development folder")
    return 0


# ---- the move ----

def exdev(e):
    return e.errno == errno.EXDEV or getattr(e, "winerror", None) == 17   # (ERROR_NOT_SAME_DEVICE)


def clone(src, dst):
    """src's bytes into a new dst: copy_file_range where there is one (a clone on btrfs and XFS: no second copy on disk)"""
    with open(src, "rb") as i, open(dst, "wb") as o:
        if hasattr(os, "copy_file_range"):
            try:
                left = os.fstat(i.fileno()).st_size
                while left > 0:
                    n = os.copy_file_range(i.fileno(), o.fileno(), min(left, 1 << 30))
                    if n == 0:
                        break
                    left -= n
                if left == 0:
                    return
            except OSError:
                pass
            i.seek(0)
            o.seek(0)
            o.truncate()
        shutil.copyfileobj(i, o, 1 << 20)


def copy_entry(src, dst, seen):
    """One file or link: a link as the same link, a file hard-linked elsewhere as a link to its first copy (seen: (device,
    inode) -> that copy, kept while the move lasts: a base's objects are links to the object store's, and the first of the
    two may be deleted, its move done, before the other is copied)"""
    st = os.lstat(src)
    if os.path.islink(src):
        os.symlink(os.readlink(src), dst)
        return
    if (st.st_dev, st.st_ino) in seen:
        os.link(seen[(st.st_dev, st.st_ino)], dst)
        return
    clone(src, dst)
    shutil.copystat(src, dst)
    if st.st_nlink > 1:
        seen[(st.st_dev, st.st_ino)] = dst


def copy_tree(src, dst, seen):
    if not os.path.isdir(src) or os.path.islink(src):
        copy_entry(src, dst, seen)
        return
    os.makedirs(dst)
    for top, dirs, files in os.walk(src):
        out = os.path.join(dst, os.path.relpath(top, src))
        for d in dirs:
            if os.path.islink(os.path.join(top, d)):   # (walk lists a linked folder here and does not go into it)
                copy_entry(os.path.join(top, d), os.path.join(out, d), seen)
            else:
                os.makedirs(os.path.join(out, d))
        for f in files:
            copy_entry(os.path.join(top, f), os.path.join(out, f), seen)
    for top, _, _ in os.walk(src, topdown=False):   # (the folders' times last: `dev.py bases` shows a base's as its last use)
        shutil.copystat(top, os.path.join(dst, os.path.relpath(top, src)), follow_symlinks=False)


def difference(src, dst):
    """The first entry the copy dst lacks or has otherwise (kind, size, link target), else None"""
    for top, dirs, files in os.walk(src):
        for n in dirs + files:
            a = os.path.join(top, n)
            b = os.path.join(dst, os.path.relpath(a, src))
            if not os.path.lexists(b) or os.path.islink(a) != os.path.islink(b):
                return a
            if os.path.islink(a):
                if os.readlink(a) != os.readlink(b):
                    return a
            elif os.path.isdir(a) != os.path.isdir(b) or (os.path.isfile(a) and os.path.getsize(a) != os.path.getsize(b)):
                return a
    if not os.path.isdir(src) and os.path.getsize(src) != os.path.getsize(dst):
        return src
    return None


def remove(p):
    if os.path.isdir(p) and not os.path.islink(p):
        shutil.rmtree(p)
    elif os.path.lexists(p):
        os.remove(p)


def move(old, new, seen, keep=()):
    """old into new: a rename, or a copy checked before old goes. Into a folder new already is, each entry of old that new
    lacks (keep: names left in old). The paths left at old because new has them too"""
    if os.path.lexists(new) or keep:
        if not (os.path.isdir(old) and not os.path.islink(old) and (os.path.isdir(new) or not os.path.lexists(new))):
            return [old]
        common.ensure_dir(new)
        left = []
        for n in sorted(os.listdir(old)):
            if n not in keep:
                left += move(os.path.join(old, n), os.path.join(new, n), seen)
        if not left and not keep:
            os.rmdir(old)
        return left
    common.ensure_dir(os.path.dirname(new))
    try:
        os.rename(old, new)
        return []
    except OSError as e:
        if not exdev(e):
            raise
    part = new + ".part"
    remove(part)   # (an earlier copy that stopped half way)
    try:
        copy_tree(old, part, seen)
        bad = difference(old, part)
        if bad:
            raise OSError("the copy of %s differs from it (%s)" % (old, bad))
    except BaseException:
        remove(part)
        raise
    os.rename(part, new)
    for k, v in seen.items():   # (the copies' own paths now: part itself when old is a file)
        if v == part or v.startswith(part + os.sep):
            seen[k] = new + v[len(part):]
    remove(old)
    return []


def leave_link(old, new):
    """A link at old to new, for tools that still look at old: a symbolic link (Linux), a junction for a folder (Windows: no
    privilege needed; a file gets none)"""
    try:
        if common.WINDOWS:
            if not os.path.isdir(new):
                return False
            import _winapi
            _winapi.CreateJunction(new, old)
        else:
            os.symlink(new, old)
        return True
    except (OSError, ImportError, AttributeError):
        return False


def same(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def plan(qdir):
    """[(part, old, new, keep)] still to move: an old place there that is not the link a move left. keep: the names that stay
    in it (the queue's tickets, when they are in its old folder)"""
    home = common.dev_home()
    out = []
    for rel, old in sorted(common.old_places().items(), key=lambda x: len(x[0])):   # ("" first: the old folder may become it)
        new = os.path.join(home, *rel.split("/")) if rel else home
        keep = (os.path.basename(qdir),) if os.path.dirname(os.path.abspath(qdir)) == os.path.abspath(old) else ()
        if not os.path.lexists(old) or same(old, new):   # (nothing there, or the link a move left)
            continue
        if keep and os.path.isdir(old) and set(os.listdir(old)) <= set(keep):
            continue
        out.append((rel, old, new, keep))
    return out


@contextlib.contextmanager
def bases_held(bases_dir):
    """Every base in bases_dir held for the move: its <base>.lock taken (`dev.py base`, `pack` and `rt` hold it for a job's length) and its
    dev_packs.busy written (`dev.py packs` refuses a busy base). One already held stops the move. (Windows renames no folder
    with a file open in it: there each lock is only tried, then let go)"""
    token = "%d migrate %s" % (os.getpid(), time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    names = sorted(os.listdir(bases_dir)) if os.path.isdir(bases_dir) else []
    marked = []
    with contextlib.ExitStack() as held:
        for n in names:
            if n.endswith(".lock"):
                lock = common.Lock(os.path.join(bases_dir, n), wait=False)
                try:
                    if os.name == "nt":
                        with lock:
                            pass
                    else:
                        held.enter_context(lock)
                except BlockingIOError:
                    sys.exit("%s: the base %s is in use (its lock is held): run this when no job uses a base" % (WHO, n[:-5]))
        try:
            for n in names:
                busy = os.path.join(bases_dir, n, packs.BUSY)
                if not os.path.isdir(os.path.join(bases_dir, n)) or n.endswith(".src"):
                    continue
                try:
                    fd = os.open(busy, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                except FileExistsError:
                    sys.exit("%s: the base %s is busy (%s): wait for that job, or delete the marker if it is no longer "
                             "running" % (WHO, n, busy))
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    f.write(token)
                marked.append(n)
            yield marked
        finally:
            for n in marked:   # (where the move took it, or where it stayed)
                for d in (os.path.join(common.dev_home(), "bases"), bases_dir):
                    busy = os.path.join(d, n, packs.BUSY)
                    try:
                        with open(busy, encoding="utf-8") as f:
                            if f.read().strip() == token:
                                os.remove(busy)
                    except OSError:
                        pass


def repoint_bases(old_root, home):
    """base.json's work and game folders under the old folder, named under the new one"""
    d = os.path.join(home, "bases")
    for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        p = os.path.join(d, n, "base.json")
        try:
            with open(p, encoding="utf-8") as f:
                b = json.load(f)
        except (OSError, ValueError):
            continue
        changed = False
        for k in ("work", "game"):
            v = b.get(k)
            if isinstance(v, str) and (v == old_root or v.startswith(old_root + os.sep)):
                b[k] = home + v[len(old_root):]
                changed = True
        if changed:
            with open(p + ".tmp", "w", encoding="utf-8") as f:
                json.dump(b, f, indent=1)
            os.replace(p + ".tmp", p)


def cmd_migrate(a):
    home = common.dev_home()
    s = jobqueue.settings()
    qdir = jobqueue.queue_dir(s)
    todo = plan(qdir)
    if not todo:
        say("nothing to move: everything is in %s" % home)
        return 0
    for _, old, new, _ in todo:
        say("%s -> %s" % (old, new))
    if a.dry_run:
        return 0
    if common.on_ram(home):
        sys.exit("%s: %s is on a RAM file system (tmpfs): set LONGSHOT_DEV_HOME to a folder on a disk" % (WHO, home))
    with common.Lock(os.path.join(qdir, ".lock"), waiting="%s: waiting for the job queue's lock" % WHO):
        # (held to the end: no job starts while its folders move)
        jobs = jobqueue.read_tickets(qdir, clean=False)
        if jobs:
            sys.exit("%s: the job queue has %d job(s) (%s): run this when `dev.py queue status` shows none" %
                     (WHO, len(jobs), ", ".join(sorted({str(t.get("name")) for t in jobs}))))
        old_root = {rel: old for rel, old, _, _ in todo}.get("")
        with bases_held(os.path.join(old_root, "bases")) if old_root else contextlib.nullcontext():
            seen, left, failed = {}, [], []
            for rel, old, new, keep in todo:
                try:
                    stayed = move(old, new, seen, keep)
                except OSError as e:
                    failed.append(old)
                    say("%s was not moved (%s): what moved is in %s, the rest where it was; run this again" % (old, e, new))
                    continue
                left += stayed
                if not os.path.lexists(old):
                    say("moved %s%s" % (old, "" if leave_link(old, new) else
                                        " (no link left there: older tools no longer find it)"))
            if old_root:
                repoint_bases(old_root, home)
    for p in left:
        say("left %s where it was: the development folder has one too; compare them and delete the one you don't need" % p)
    if not failed:
        say("done: `dev.py paths` shows the development folder")
    return 1 if failed else 0


def add_commands(sub):
    """`dev.py paths` and `dev.py migrate`"""
    p = sub.add_parser("paths", help="this PC's development folder and what it holds",
                       description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("name", nargs="?", help="one path alone: home, work, or a part such as bases or stores/lifts")
    p.set_defaults(func=cmd_paths)
    p = sub.add_parser("migrate", help="move what older tools kept elsewhere into the development folder",
                       description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dry-run", action="store_true", help="say what would move, move nothing")
    p.set_defaults(func=cmd_migrate)
