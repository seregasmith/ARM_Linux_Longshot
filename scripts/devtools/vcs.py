"""The checkout's version control, for the development tools: jj when a .jj folder is in the tree or above it (a jj
workspace has no .git), else git (the Test Farm's checkouts). Every function takes the checkout's folder.

    commit(tree)            the commit the tree is on, and which one it is
    changed_files(tree, c)  the files changed since commit c: committed, uncommitted and new
    is_ancestor(tree, c)    whether commit c is in the tree's history
    distance(tree, c)       how many commits the tree is ahead of c
    resolve(tree, rev)      a revision as its full commit id
    clean(tree)             whether the tree has no uncommitted changes to tracked files
    show(tree, c, path)     a file's text at commit c (path / separated, from the checkout's top)
"""
import os
import subprocess


def run(tree, cmd, check=True):
    """A command's output in the tree (stripped); check: a failure raises CalledProcessError, else gives None"""
    r = subprocess.run(cmd, cwd=tree, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        if check:
            raise subprocess.CalledProcessError(r.returncode, cmd, r.stdout, r.stderr)
        return None
    return (r.stdout or "").strip()


def is_jj(tree):
    d = os.path.abspath(tree)
    while True:
        if os.path.isdir(os.path.join(d, ".jj")):
            return True
        if os.path.exists(os.path.join(d, ".git")):   # (a git checkout inside a jj one is git's)
            return False
        up = os.path.dirname(d)
        if up == d:
            return False
        d = up


def _jj_ids(tree, revset):
    return (run(tree, ["jj", "log", "--no-graph", "--ignore-working-copy", "-r", revset, "-T", 'commit_id ++ "\\n"'])
            or "").split()


def resolve(tree, rev):
    if is_jj(tree):
        ids = _jj_ids(tree, rev)
        if len(ids) != 1:
            raise subprocess.CalledProcessError(1, ["jj", "log", "-r", rev], "", "%r is %d commits" % (rev, len(ids)))
        return ids[0]
    return run(tree, ["git", "rev-parse", "--verify", rev + "^{commit}"])


def clean(tree):
    if is_jj(tree):   # (jj's working copy is the commit @: clean when @ changes nothing)
        return not run(tree, ["jj", "diff", "-r", "@", "--summary"])
    return not run(tree, ["git", "status", "--porcelain", "--untracked-files=no"])


def commit(tree):
    """(commit id, which): git's HEAD; jj's @ when it has changes, else its parent @- (the commit the work is on)"""
    if is_jj(tree):
        rev = "@-" if clean(tree) else "@"
        return resolve(tree, rev), rev
    return resolve(tree, "HEAD"), "HEAD"


def changed_files(tree, base):
    """The files changed since a commit: in commits, uncommitted, and new ones the ignore files do not exclude"""
    if is_jj(tree):   # (jj tracks new files itself)
        names = run(tree, ["jj", "diff", "--name-only", "--from", base, "--to", "@"]).split("\n")
    else:
        names = run(tree, ["git", "diff", "--name-only", base]).split("\n") + \
            run(tree, ["git", "ls-files", "--others", "--exclude-standard"]).split("\n")
    return set(names) - {""}


def is_ancestor(tree, base):
    if is_jj(tree):
        return bool(run(tree, ["jj", "log", "--no-graph", "--ignore-working-copy", "-r", "%s & ::@" % base,
                               "-T", "commit_id"], check=False))
    return subprocess.run(["git", "merge-base", "--is-ancestor", base, "HEAD"], cwd=tree,
                          capture_output=True).returncode == 0


def distance(tree, base):
    """Commits from base to the tree's commit (1 << 30 when it cannot say)"""
    if is_jj(tree):
        ids = run(tree, ["jj", "log", "--no-graph", "--ignore-working-copy", "-r", "%s..@" % base, "-T", 'commit_id ++ "\\n"'],
                  check=False)
        return len(ids.split()) if ids is not None else 1 << 30
    try:
        return int(run(tree, ["git", "rev-list", "--count", "%s..HEAD" % base], check=False) or "")
    except ValueError:
        return 1 << 30


def show(tree, base, path):
    """A file's text as commit base has it; raises CalledProcessError when it has no such file"""
    if is_jj(tree):
        return run(tree, ["jj", "file", "show", "--ignore-working-copy", "-r", base, "root:" + path])
    return run(tree, ["git", "show", "%s:%s" % (base, path)])
