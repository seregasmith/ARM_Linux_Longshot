"""Staged save commits through the actual nn::fs HLEs and Linux Win32 file layer, without a game.

Cases (a)-(h) cover failed overwrites, publication, discarded work, crash recovery, incomplete journals,
delete/rename, staging failure and mount fallback. Extra checks cover links, hostile journals, retries,
type changes, shared mounts, open writers and traces. --source accepts an unchanged nn_fs.c for the regression proof.
"""
import argparse
import json
import select
import ctypes
import os
import re
from pathlib import Path
import signal
import shutil
import threading
import time
import subprocess
import sys
import tempfile

if sys.platform == "linux":
    import resource

RUN_ALL_ARGS = ["--cc", "{cc}"]
ROOT = Path(__file__).resolve().parents[1]

# (every nn::fs call the save change touches, as the game imports them)
HOOKS = [
    "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", "_ZN2nn2fs7UnmountEPKc", "_ZN2nn2fs6CommitEPKc",
    "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",
    "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl",
    "_ZN2nn2fs9FlushFileENS0_10FileHandleE", "_ZN2nn2fs9CloseFileENS0_10FileHandleE", "_ZN2nn2fs10CreateFileEPKcl",
    "_ZN2nn2fs10DeleteFileEPKc", "_ZN2nn2fs15CreateDirectoryEPKc", "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc",
    "_ZN2nn2fs10RenameFileEPKcS2_",
]
UID = "00000000000000000000000000000001"
ERROR = 0x2EE202
SYMS = {
    "mount": "13MountSaveDataEPKcRKNS_7account3UidE",
    "open": "8OpenFileEPNS0_10FileHandleEPKci",
    "write": "9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE",
    "resize": "11SetFileSizeENS0_10FileHandleEl",
    "close": "9CloseFileENS0_10FileHandleE",
    "read": "8ReadFileENS0_10FileHandleElPvm",
    "flush": "9FlushFileENS0_10FileHandleE",
    "create": "10CreateFileEPKcl",
    "delete": "10DeleteFileEPKc",
    "mkdir": "15CreateDirectoryEPKc",
    "rmtree": "26DeleteDirectoryRecursivelyEPKc",
    "rename": "10RenameFileEPKcS2_",
    "commit": "6CommitEPKc",
    "unmount": "7UnmountEPKc",
    "readonly": "21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE",
    "cache": "17MountCacheStorageEPKci",
    "temp": "21MountTemporaryStorageEPKc",
}
A64 = r'''
#ifndef A64_H
#define A64_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>   /* (as the real a64.h: hos.h's inline fill and copy use memset and memcpy) */
typedef struct { uint64_t x[32]; } Ctx;
extern unsigned char test_guest[2 * 1024 * 1024];
#define A64_PTR(a) (test_guest + (uint64_t)(a))
#endif
'''
STUBS = r'''
#include "hos.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <pthread.h>
#endif
unsigned char test_guest[2 * 1024 * 1024];
char test_logs[1024 * 1024];
int hos_trace, test_fail_renames, test_crash_apply, test_apply_count;
int hos_hle_stats;   /* (HOS_HLE_STATS: off here, every HLE runs its body directly) */
void hos_hle_counted(HleCount *h, void (*fn)(Ctx *c), Ctx *c) { (void)h; fn(c); }
int test_fail_journal_move, test_fail_journal_delete, test_committed_deletes;
#ifndef _WIN32
static pthread_rwlock_t *test_save_lock;
_Atomic int test_save_locks;
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *lock);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
    if (!test_save_lock) test_save_lock = lock;   /* (the first mount takes save_lock first) */
    if (lock == test_save_lock) atomic_fetch_add(&test_save_locks, 1);
    return __real_pthread_rwlock_wrlock(lock);
}
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *lock) {
    if (lock == test_save_lock) atomic_fetch_add(&test_save_locks, 1);
    return __real_pthread_rwlock_rdlock(lock);
}
void test_hold_save_lock(void) { __real_pthread_rwlock_wrlock(test_save_lock); }
void test_release_save_lock(void) { pthread_rwlock_unlock(test_save_lock); }
#endif
void hos_log(const char *fmt, ...) {
    size_t at = strlen(test_logs);
    va_list ap; va_start(ap, fmt);
    vsnprintf(test_logs + at, sizeof test_logs - at, fmt, ap);
    va_end(ap);
    at = strlen(test_logs);
    if (at + 1 < sizeof test_logs) test_logs[at] = '\n', test_logs[at + 1] = 0;
}
void hos_fatal(Ctx *c, const char *fmt, ...) { (void)c; (void)fmt; abort(); }
int hos_vcore_away(void) { return 0; }
void hos_vcore_back(void) {}
int hos_commit(uint64_t a, uint64_t n) { return a < sizeof test_guest && n <= sizeof test_guest - a; }
int64_t hos_now_ns(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64() * 1000000;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
#endif
}
void hos_sleep_ns(int64_t n) { (void)n; }
int hos_mods_romfs(const char *r, char *o, size_t n) { (void)r; (void)o; (void)n; return 0; }
int hos_mods_dir_extras(const char *r, const char *b, void (*f)(void *, const char *, int, uint64_t), void *a) {
    (void)r; (void)b; (void)f; (void)a; return 0;
}
int hos_blank_cached_icon(const char *g, const char *h) { (void)g; (void)h; return 0; }
int hos_ultrawide_redirect(const char *g, char *o, size_t n) { (void)g; (void)o; (void)n; return 0; }
int hos_stand_in_damaged(const char *g, char *p, size_t n) { (void)g; (void)p; (void)n; return 0; }
#ifndef _WIN32
BOOL __real_MoveFileExA(LPCSTR a, LPCSTR b, DWORD flags);
/* (failure injection at the boundary; successful calls always use the real POSIX implementation) */
BOOL __wrap_MoveFileExA(LPCSTR a, LPCSTR b, DWORD flags) {
    size_t n = strlen(a);
    if (n >= 8 && !strcmp(a + n - 8, ".journal") && test_fail_journal_move) {
        SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    if (n >= 13 && !strcmp(a + n - 13, ".longshot-new")) {
        if (test_crash_apply && ++test_apply_count == test_crash_apply) _exit(86);
        if (test_fail_renames) { test_fail_renames--; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    }
    return __real_MoveFileExA(a, b, flags);
}
BOOL __real_DeleteFileA(LPCSTR path);
BOOL __wrap_DeleteFileA(LPCSTR path) {
    if (strstr(path, "\\save\\") || strstr(path, "/save/")) test_committed_deletes++;
    size_t n = strlen(path);
    if (n >= 8 && !strcmp(path + n - 8, ".journal") && test_fail_journal_delete) {
        SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    return __real_DeleteFileA(path);
}
#endif
'''


class Fs:
    def __init__(self, lib, root):
        self.lib, self.root = lib, root
        self.c = root / "save" / UID
        self.w = root / "save-work" / UID
        self.j = self.w.with_name(UID + ".journal")
        self.c.mkdir(parents=True, exist_ok=True)
        self.user = os.fsencode(root)
        ctypes.c_char_p.in_dll(lib, "hos_user_dir").value = self.user
        self.guest = (ctypes.c_ubyte * (2 * 1024 * 1024)).in_dll(lib, "test_guest")
        self.logs = (ctypes.c_char * (1024 * 1024)).in_dll(lib, "test_logs")
        self.logs[0] = b"\0"
        self.put(64, b"save\0")
        self.put(128, b"\1" + b"\0" * 15)
        for key, sym in SYMS.items():
            fn = getattr(lib, "imp__ZN2nn2fs" + sym)
            fn.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
            fn.restype = None
            setattr(self, "hle_" + key, fn)

    def put(self, at, data):
        ctypes.memmove(ctypes.addressof(self.guest) + at, data, len(data))
        return at

    def path(self, rel, at=256):
        return self.put(at, ("save:/" + rel).encode() + b"\0")

    def call(self, key, *args):
        ctx = (ctypes.c_uint64 * 32)(*args)
        getattr(self, "hle_" + key)(ctx)
        return ctx[0]

    def mount(self):
        assert self.call("mount", 64, 128) == 0

    def unmount(self):
        self.call("unmount", 64)

    def commit(self):
        return self.call("commit", 64)

    def create(self, rel, size=0):
        assert self.call("create", self.path(rel), size) == 0

    def open(self, rel, mode=3):
        assert self.call("open", 1024, self.path(rel), mode) == 0
        return ctypes.c_uint64.from_buffer(self.guest, 1024).value

    def write(self, handle, data, offset=0):
        return self.call("write", handle, offset, self.put(65536, data), len(data), 0)

    def close(self, handle):
        self.call("close", handle)

    def replace(self, rel, data):
        if not (self.w / rel).exists():
            self.create(rel)
        h = self.open(rel)
        assert self.call("resize", h, len(data)) == 0
        assert self.write(h, data) == 0
        self.close(h)

    def clean(self):
        assert not self.j.exists()
        assert not list(self.c.rglob("*.longshot-new"))

    def text(self):
        return self.logs.value.decode()


def limited(size, fn):
    old = resource.getrlimit(resource.RLIMIT_FSIZE)
    resource.setrlimit(resource.RLIMIT_FSIZE, (size, old[1]))
    try:
        return fn()
    finally:
        resource.setrlimit(resource.RLIMIT_FSIZE, old)


def run_cases(lib, tmp, only):
    def new(case):
        return Fs(lib, tmp / case)

    f = new("a")
    old = b"old saved bytes!" * 256
    (f.c / "progress.sav").write_bytes(old)
    f.mount()
    h = f.open("progress.sav")
    assert limited(8, lambda: f.write(h, b"N" * 64)) == ERROR
    # (the actual pwrite wrote its first eight bytes, then failed; lift the limit before copying the old file back)
    f.close(h)
    assert (f.c / "progress.sav").read_bytes() == old, "(a): failed overwrite damaged committed bytes"
    assert (f.w / "progress.sav").read_bytes() == old, "(a): CloseFile did not restore work bytes"
    f.unmount()
    print("PASS (a): a real partial overwrite fails; C stays old and CloseFile restores W", flush=True)
    if only == "a":
        return

    f = new("b")
    (f.c / "option.sav").write_bytes(b"old")
    f.mount()
    for rel, data in [("slot_00/progress.sav", b"progress"), ("slot_00/caption.sav", b"caption"), ("option.sav", b"options")]:
        f.replace(rel, data)
    assert (f.c / "option.sav").read_bytes() == b"old"
    assert not (f.c / "slot_00").exists()
    assert f.commit() == 0
    assert (f.c / "option.sav").read_bytes() == b"options"
    assert (f.c / "slot_00/progress.sav").read_bytes() == b"progress"
    assert (f.c / "slot_00/caption.sav").read_bytes() == b"caption"
    f.clean()
    before = {p: p.stat().st_mtime_ns for p in f.c.rglob("*")}
    assert f.commit() == 0
    assert before == {p: p.stat().st_mtime_ns for p in f.c.rglob("*")}
    assert "fs: save: published 3 files in " in f.text()
    for token in ["create file", "write save:", "resize save:", "close save:", "fs: Commit:"]:
        assert token in f.text(), token
    f.unmount()
    print("PASS (b): three files become committed together; no staging or journal remains; no-op commit", flush=True)

    f = new("c")
    (f.c / "option.sav").write_bytes(b"old")
    f.mount()
    f.replace("option.sav", b"uncommitted")
    f.unmount()
    f.mount()
    assert (f.w / "option.sav").read_bytes() == b"old"
    f.unmount()
    print("PASS (c): remount discards work without Commit", flush=True)

    for crash in (1, 2):
        f = new("d%d" % crash)
        (f.c / "option.sav").write_bytes(b"old")
        pid = os.fork()
        if pid == 0:
            f.mount()
            f.replace("option.sav", b"new")
            f.replace("account.sav", b"account")
            ctypes.c_int.in_dll(lib, "test_crash_apply").value = crash
            f.commit()
            os._exit(87)
        _, status = os.waitpid(pid, 0)
        assert os.waitstatus_to_exitcode(status) == 86
        assert f.j.read_text().endswith("E\n")
        if crash == 1:
            assert (f.c / "option.sav").read_bytes() == b"old"
            assert not (f.c / "account.sav").exists()
        f.mount()
        assert (f.c / "option.sav").read_bytes() == b"new"
        assert (f.c / "account.sav").read_bytes() == b"account"
        assert (f.w / "account.sav").read_bytes() == b"account"
        f.clean()
        f.unmount()
    print("PASS (d): crash after journal publication and crash during apply both replay on mount", flush=True)

    for journal in (b"R option.sav\n", b"R option.sav\nD ../outside\nE\n", b"R option.sav\nD /outside\nE\n", b"R option.sav\0x\nE\n", b"R option.sav\nE\0x\n", b""):
        f = new("e%d" % len(list(tmp.glob("e*"))))
        (f.c / "option.sav").write_bytes(b"old")
        f.w.parent.mkdir()
        f.j.write_bytes(journal)
        (f.c / "0.longshot-new").write_bytes(b"new")
        (f.c / "stray.longshot-new").write_bytes(b"stray")
        f.mount()
        assert (f.c / "option.sav").read_bytes() == b"old"
        assert not f.j.exists()
        assert (f.c / "0.longshot-new").read_bytes() == b"new"
        assert (f.c / "stray.longshot-new").read_bytes() == b"stray"
        f.unmount()
    print("PASS (e): incomplete and hostile journals are rejected without deleting unrelated files", flush=True)

    f = new("f")
    (f.c / "slot_00").mkdir()
    (f.c / "slot_00/progress.sav").write_bytes(b"old")
    (f.c / "old.sav").write_bytes(b"rename me")
    (f.c / "old-slot/inner").mkdir(parents=True)
    (f.c / "old-slot/inner/progress.sav").write_bytes(b"folder rename")
    (f.c / "kept").mkdir()
    (f.c / "kept/option.sav").write_bytes(b"old")
    f.mount()
    (f.c / "player.txt").write_bytes(b"keep")
    (f.c / "kept/player.txt").write_bytes(b"keep nested")
    f.replace("kept/option.sav", b"new")
    assert f.call("rmtree", f.path("slot_00")) == 0
    h = f.open("old.sav")
    assert f.call("rename", f.path("old.sav"), f.path("new.sav", 2048)) != 0
    assert f.call("delete", f.path("old.sav")) != 0
    f.close(h)
    assert f.call("rename", f.path("old.sav"), f.path("new.sav", 2048)) == 0
    assert f.call("rename", f.path("old-slot"), f.path("new-slot", 2048)) == 0
    f.create("delete.sav")
    assert f.call("delete", f.path("delete.sav")) == 0
    assert f.call("mkdir", f.path("empty")) == 0
    assert (f.c / "slot_00/progress.sav").exists()
    assert f.commit() == 0
    assert not (f.c / "slot_00").exists()
    assert not (f.c / "old.sav").exists()
    assert not (f.c / "delete.sav").exists()
    assert (f.c / "new.sav").read_bytes() == b"rename me"
    assert (f.c / "empty").is_dir()
    assert (f.c / "player.txt").read_bytes() == b"keep"
    assert (f.c / "kept/player.txt").read_bytes() == b"keep nested"
    assert (f.c / "kept/option.sav").read_bytes() == b"new"
    assert not (f.c / "old-slot").exists()
    assert (f.c / "new-slot/inner/progress.sav").read_bytes() == b"folder rename"
    f.clean()
    f.unmount()
    print("PASS (f): delete tree, rename, create directory and delete file; unrelated player file survives", flush=True)

    f = new("g")
    (f.c / "option.sav").write_bytes(b"old")
    f.mount()
    f.replace("option.sav", b"N" * 4096)
    f.replace("account.sav", b"A" * 4096)
    assert limited(16, f.commit) == ERROR
    assert (f.c / "option.sav").read_bytes() == b"old"
    assert not (f.c / "account.sav").exists()
    f.clean()
    assert f.commit() == 0
    assert (f.c / "option.sav").read_bytes() == b"N" * 4096
    assert (f.c / "account.sav").read_bytes() == b"A" * 4096
    f.clean()
    f.unmount()
    print("PASS (g): phase 1 failure preserves C and notes; a later Commit publishes", flush=True)

    f = new("h")
    (f.c / "option.sav").write_bytes(old)
    outside = f.root / "outside"
    outside.mkdir()
    (outside / "kept").write_bytes(b"outside")
    (f.c / "linked").symlink_to(outside, target_is_directory=True)
    (f.c / "file-link").symlink_to(outside / "kept")
    limited(8, f.mount)
    f.create("linked/through")
    assert (outside / "through").exists(), "in-place fallback must keep its old link behavior"
    h = f.open("file-link", 1)
    assert f.call("read", h, 0, 8192, 7) == 0
    assert bytes(f.guest[8192:8199]) == b"outside"
    f.close(h)
    h = f.open("option.sav")
    assert f.write(h, b"direct", 0) == 0
    f.close(h)
    assert (f.c / "option.sav").read_bytes().startswith(b"direct")
    assert "using the committed folder" in f.text()
    f.unmount()
    print("PASS (h): mount copy failure falls back to in-place writes", flush=True)

    f = new("ownership-unavailable")
    (f.c / "option.sav").write_bytes(b"committed")
    f.w.mkdir(parents=True)
    (f.w / "option.sav").write_bytes(b"work")
    f.w.with_name(UID + ".lock").mkdir()
    f.j.write_bytes(b"R option.sav\nE\n")
    (f.c / "0.longshot-new").write_bytes(b"pending")
    before = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in f.root.rglob("*") if p.is_file()}
    assert f.call("mount", 64, 128) != 0
    assert "journal recovery must finish before mounting" in f.text()
    assert "another running copy" not in f.text()
    assert before == {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in f.root.rglob("*") if p.is_file()}
    print("PASS: unavailable ownership refuses the mount without touching recovery files", flush=True)

    f = new("extra")
    (f.c / "folder").mkdir()
    (f.c / "folder/old").write_bytes(b"old")
    (f.c / "file").write_bytes(b"file")
    f.mount()
    assert f.call("rmtree", f.path("folder")) == 0
    f.replace("folder", b"now a file")
    assert f.call("delete", f.path("file")) == 0
    assert f.call("mkdir", f.path("file")) == 0
    f.replace("file/child", b"now a folder")
    assert f.commit() == 0
    assert (f.c / "folder").read_bytes() == b"now a file"
    assert (f.c / "file/child").read_bytes() == b"now a folder"
    h = f.open("folder")
    assert f.commit() == ERROR
    f.close(h)
    f.replace("folder", b"retry")
    ctypes.c_int.in_dll(lib, "test_fail_renames").value = 100
    assert f.commit() == ERROR and f.j.exists()
    ctypes.c_int.in_dll(lib, "test_fail_renames").value = 0
    assert f.commit() == 0
    assert (f.c / "folder").read_bytes() == b"retry"
    f.clean()
    f.unmount()
    print("PASS: type changes, open-writer refusal and replay after rename refusal", flush=True)

    f = new("outside-children")
    (f.c / "removed/inner").mkdir(parents=True)
    (f.c / "removed/inner/old").write_bytes(b"old")
    (f.c / "replaced").mkdir()
    (f.c / "replaced/old").write_bytes(b"old")
    f.mount()
    assert f.call("rmtree", f.path("removed")) == 0
    assert f.call("rmtree", f.path("replaced")) == 0
    f.replace("replaced", b"file")
    (f.c / "removed/inner/outside").write_bytes(b"keep nested")
    (f.c / "replaced/outside").write_bytes(b"keep")
    assert f.commit() == ERROR and f.j.exists(), "a foreign child must prevent replacing its directory"
    assert not (f.c / "removed/inner/old").exists()
    assert (f.c / "removed/inner/outside").read_bytes() == b"keep nested"
    assert (f.c / "replaced/outside").read_bytes() == b"keep"
    # (a child arriving after the journal promise must survive replay too)
    (f.c / "removed/late").write_bytes(b"keep late")
    (f.c / "replaced/outside").unlink()
    assert f.commit() == 0
    assert (f.c / "replaced").read_bytes() == b"file"
    assert (f.c / "removed/late").read_bytes() == b"keep late"
    assert (f.c / "removed/inner/outside").read_bytes() == b"keep nested"
    f.clean()
    f.unmount()
    print("PASS: only game-deleted entries are removed; outside children survive deletion and replay or block a type change", flush=True)

    f = new("retry-and-shared")
    (f.c / "option.sav").write_bytes(b"old")
    f.mount()
    f.replace("option.sav", b"work")
    f.put(4096, b"alias\0")
    assert f.call("mount", 4096, 128) == 0
    assert (f.w / "option.sav").read_bytes() == b"work"
    assert f.call("commit", 4096) == 0
    assert (f.c / "option.sav").read_bytes() == b"work"
    f.call("unmount", 4096)
    h = f.open("option.sav")
    assert limited(2, lambda: f.write(h, b"bad bytes")) == ERROR
    # (restoration also fails with no room; Commit must keep this path out until restoration works)
    limited(2, lambda: f.close(h))
    assert (f.c / "option.sav").read_bytes() == b"work"
    assert limited(2, f.commit) == ERROR
    assert (f.c / "option.sav").read_bytes() == b"work"
    assert f.commit() == 0
    assert (f.w / "option.sav").read_bytes() == b"work"
    f.unmount()
    print("PASS: shared mounts and failed-write restoration retry", flush=True)

    f = new("bad-resize")
    (f.c / "option.sav").write_bytes(b"old tail")
    f.mount()
    h = f.open("option.sav")
    assert f.call("resize", h, 0xffffffffffffffff) == ERROR
    assert f.write(h, b"new") == 0
    f.close(h)
    assert f.commit() == 0
    assert (f.c / "option.sav").read_bytes() == b"old tail"
    f.unmount()

    f = new("unaddressable")
    (f.c / "what?").write_bytes(b"keep")
    (f.c / "a:b").write_bytes(b"keep too")
    (f.c / "backup.longshot-new").write_bytes(b"backup")
    f.mount()
    f.replace("option.sav", b"new")
    assert f.commit() == 0
    assert (f.c / "what?").read_bytes() == b"keep"
    assert (f.c / "a:b").read_bytes() == b"keep too"
    assert (f.c / "backup.longshot-new").read_bytes() == b"backup"
    f.unmount()

    for retire in ("move", "delete", "refuse"):
        f = new("failed-replay-" + retire)
        (f.c / "option.sav").write_bytes(b"old")
        f.mount()
        f.replace("option.sav", b"unfinished")
        ctypes.c_int.in_dll(lib, "test_fail_renames").value = 1000
        assert f.commit() == ERROR and f.j.exists()
        assert f.commit() == ERROR and f.j.exists(), "Commit must retain a journal it cannot replay"
        f.unmount()
        failed = f.j.with_suffix(".journal.failed")
        if retire == "move":
            failed.write_bytes(b"older failure")
        ctypes.c_int.in_dll(lib, "test_fail_journal_move").value = retire != "move"
        ctypes.c_int.in_dll(lib, "test_fail_journal_delete").value = retire == "refuse"
        result = f.call("mount", 64, 128)
        assert result != 0 and f.j.exists()
        assert list(f.c.glob("*.longshot-new")), "refusal must keep the journal's staged files"
        ctypes.c_int.in_dll(lib, "test_fail_journal_move").value = 0
        ctypes.c_int.in_dll(lib, "test_fail_journal_delete").value = 0
        ctypes.c_int.in_dll(lib, "test_fail_renames").value = 0
        f.mount()
        assert (f.c / "option.sav").read_bytes() == b"unfinished"
        f.clean()
        f.unmount()
    print("PASS: failed replay refuses the mount and retains the journal until recovery", flush=True)

    f = new("missing-notes")
    (f.c / "file").write_bytes(b"committed file")
    (f.c / "directory").mkdir()
    (f.c / "directory/kept").write_bytes(b"committed child")
    f.mount()
    f.replace("healthy", b"N" * 4096)
    h = f.open("file")
    f.close(h)
    (f.w / "file").unlink()
    shutil.rmtree(f.w / "directory")
    assert f.call("mkdir", f.path("directory")) == 0
    (f.w / "directory").rmdir()
    assert limited(16, f.commit) == ERROR
    for rel in ("file", "directory"):
        assert f.text().count("fs: save: %s is gone from the work folder; the committed file is kept" % rel) == 1
    # (dropped notes stay dropped even if an outside writer brings a different work entry back before a retry)
    (f.w / "file").write_bytes(b"outside file")
    (f.w / "directory").write_bytes(b"outside replacement")
    assert f.commit() == 0
    assert (f.c / "file").read_bytes() == b"committed file"
    assert (f.c / "directory/kept").read_bytes() == b"committed child"
    assert (f.c / "healthy").read_bytes() == b"N" * 4096
    assert f.text().count("is gone from the work folder;") == 2
    f.clean()
    f.unmount()
    print("PASS: missing FILE/DIR notes keep C, are dropped once, and do not stop other publication", flush=True)

    f = new("create-full")
    f.mount()
    assert limited(8, lambda: f.call("create", f.path("too-big"), 4096)) == ERROR
    assert not (f.w / "too-big").exists() and not (f.c / "too-big").exists()
    assert f.commit() == 0
    f.clean()
    f.unmount()
    print("PASS: a failed CreateFile reservation is rejected and removed from W", flush=True)

    f = new("journal-full")
    (f.c / "option.sav").write_bytes(b"old")
    f.mount()
    f.replace("a" * 80, b"x")
    assert limited(16, f.commit) == ERROR
    assert (f.c / "option.sav").read_bytes() == b"old"
    assert not (f.c / ("a" * 80)).exists()
    f.clean()
    assert f.commit() == 0
    assert (f.c / ("a" * 80)).read_bytes() == b"x"
    f.clean()
    f.unmount()
    print("PASS: a short journal write leaves C unchanged and can be retried", flush=True)

    f = new("linked-root")
    f.c.rmdir()
    target = f.root / "kept-save"
    target.mkdir()
    f.c.symlink_to(target, target_is_directory=True)
    (target / "option.sav").write_bytes(b"old")
    f.mount()
    f.replace("option.sav", b"new")
    assert f.commit() == 0
    assert f.c.is_symlink() and (target / "option.sav").read_bytes() == b"new"
    f.unmount()
    print("PASS: C itself may remain a linked save folder", flush=True)

    f = new("links")
    outside = f.root / "outside"
    outside.mkdir()
    (outside / "keep").write_bytes(b"outside")
    (f.c / "link").symlink_to(outside, target_is_directory=True)
    (f.c / "file-link").symlink_to(outside / "keep")
    f.mount()
    assert not f.w.exists() and "using the committed folder" in f.text()
    h = f.open("file-link", 1)
    assert f.call("read", h, 0, 8192, 7) == 0
    assert bytes(f.guest[8192:8199]) == b"outside"
    f.close(h)
    f.unmount()

    f = new("work-links")
    f.mount()
    (f.w / "bad").symlink_to(outside, target_is_directory=True)
    assert f.call("create", f.path("bad/escaped"), 1) != 0
    (f.w / "dangling").symlink_to(outside / "missing")
    assert f.call("create", f.path("dangling"), 1) != 0
    assert not (outside / "missing").exists()
    f.create("safe")
    assert f.commit() == 0
    assert (outside / "keep").read_bytes() == b"outside"
    f.unmount()
    print("PASS: folder/file/dangling links do not escape C or W", flush=True)

    f = new("replaced-work-root")
    (f.c / "option.sav").write_bytes(b"old")
    outside = f.root / "outside"
    outside.mkdir()
    f.mount()
    kept = f.w.with_name("kept-work")
    f.w.rename(kept)
    assert f.call("create", f.path("escaped"), 1) != 0
    assert f.commit() == ERROR
    f.w.symlink_to(outside, target_is_directory=True)
    assert f.call("create", f.path("escaped"), 1) != 0
    assert f.commit() == ERROR
    assert not (outside / "escaped").exists()
    assert (f.c / "option.sav").read_bytes() == b"old"
    f.w.unlink()
    kept.rename(f.w)
    f.unmount()
    print("PASS: a missing or replaced W root cannot turn into committed deletions or escape W", flush=True)

    f = new("read-lock")
    (f.c / "option.sav").write_bytes(b"old")
    f.mount()
    h = f.open("option.sav", 1)
    touches = ctypes.c_int.in_dll(lib, "test_save_locks")
    touches.value = 0
    done = threading.Event()
    results = []
    def read_save():
        results.append(f.call("read", h, 0, 8192, 3))
        done.set()
    lib.test_hold_save_lock()
    worker = threading.Thread(target=read_save)
    try:
        worker.start()
        deadline = time.monotonic() + 2
        while not touches.value and time.monotonic() < deadline:
            time.sleep(0.001)
        assert touches.value > 0, "a staged ReadFile must take save_lock"
        assert not done.wait(0.05), "ReadFile ran during a restore or publish"
    finally:
        lib.test_release_save_lock()
        worker.join(2)
    assert done.is_set() and results == [0]
    assert bytes(f.guest[8192:8195]) == b"old"
    f.close(h)
    f.unmount()
    print("PASS: staged reads wait while the restore/publish lock is held", flush=True)

    f = new("bridge-lock")
    (f.c / "option.sav").write_bytes(b"old")
    f.mount()
    f.replace("option.sav", b"new")
    lib.hos_fs_save_root.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.hos_fs_save_root.restype = ctypes.c_int
    root = ctypes.create_string_buffer(2048)
    assert lib.hos_fs_save_root(root, len(root)) == 1
    done = threading.Event()
    results = []
    touches.value = 0
    def publish():
        results.append(f.commit())
        done.set()
    worker = threading.Thread(target=publish)
    worker.start()
    try:
        deadline = time.monotonic() + 2
        while not touches.value and time.monotonic() < deadline:
            time.sleep(0.001)
        assert touches.value and not done.wait(0.05)
        assert (f.c / "option.sav").read_bytes() == b"old"
    finally:
        lib.hos_fs_save_end()
        worker.join(2)
    assert done.is_set() and results == [0]
    assert (f.c / "option.sav").read_bytes() == b"new"
    f.j.write_bytes(b"pending")
    assert lib.hos_fs_save_root(root, len(root)) == 0
    f.j.unlink()
    f.unmount()
    print("PASS: bridge listing excludes publication and refuses a pending journal", flush=True)

    for kind in ("readonly", "cache", "temp"):
        f = new("in-place-" + kind)
        if kind == "readonly":
            assert f.call(kind, 64, 0, 128) == 0
            target = f.c
        elif kind == "cache":
            assert f.call(kind, 64, 0) == 0
            target = f.root / "cache" / "0"
        else:
            assert f.call(kind, 64) == 0
            target = f.root / "temp"
        touches.value = 0
        f.create("plain")
        h = f.open("plain")
        assert f.write(h, b"direct") == 0
        assert f.call("resize", h, 6) == 0
        assert f.call("flush", h) == 0
        assert f.call("read", h, 0, 8192, 6) == 0
        assert bytes(f.guest[8192:8198]) == b"direct"
        f.close(h)
        assert touches.value == 0, kind + " operations touched save_lock"
        assert (target / "plain").read_bytes() == b"direct"
        assert f.commit() == 0 and not f.w.exists()
        f.unmount()
    print("PASS: read-only-save, cache and temp mount behavior stays in place", flush=True)



def process_worker(libpath, root):
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    f = Fs(ctypes.CDLL(libpath), Path(root))
    for line in sys.stdin:
        request = json.loads(line)
        op, args = request["op"], request["args"]
        if op in ("fail_renames", "crash_apply", "committed_deletes"):
            value = ctypes.c_int.in_dll(f.lib, "test_" + op)
            if args:
                value.value = args[0]
            result = value.value
        elif op == "write":
            result = f.write(args[0], args[1].encode())
        elif op == "replace":
            result = f.replace(args[0], args[1].encode())
        elif op == "put":
            result = f.put(args[0], bytes(args[1]))
        elif op == "limited_mount":
            result = limited(8, f.mount)
        elif op == "read_bytes":
            assert f.call("read", args[0], 0, 8192, args[1]) == 0
            result = bytes(f.guest[8192:8192 + args[1]]).decode()
        else:
            result = getattr(f, op)(*args)
        print(json.dumps(result), flush=True)


class ProcessFs:
    def __init__(self, libpath, root):
        self.process = subprocess.Popen([sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                                         "--worker", str(libpath), str(root)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def request(self, op, *args):
        p = self.process
        p.stdin.write(json.dumps({"op": op, "args": args}) + "\n")
        p.stdin.flush()
        assert select.select([p.stdout], [], [], 10)[0], "save worker did not answer: " + op
        line = p.stdout.readline()
        assert line, "save worker stopped: " + p.stderr.read()
        return json.loads(line)

    def close(self):
        p = self.process
        if p.poll() is None:
            p.stdin.close()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        assert p.returncode in (0, 86), p.stderr.read()
        p.stdin.close()
        p.stdout.close()
        p.stderr.close()


def process_cases(libpath, tmp):
    root = tmp / "processes"
    c, w = root / "save" / UID, root / "save-work" / UID
    j = w.with_name(UID + ".journal")
    lock = w.with_name(UID + ".lock")
    (c / "slot").mkdir(parents=True)
    (c / "slot/progress.sav").write_bytes(b"old")
    workers = []
    def start():
        worker = ProcessFs(libpath, root)
        workers.append(worker)
        return worker.request
    def recovery_files():
        return {str(p.relative_to(root)): (p.read_bytes(), p.stat().st_mtime_ns)
                for base in (w.parent, c) for p in base.rglob("*")
                if p.is_file() and (base == w.parent or p.name.endswith(".longshot-new"))}
    busy = "fs: save: another running copy of the game has this save; refusing mount"
    try:
        a, b = start(), start()
        a("mount")
        inode = lock.stat().st_ino
        assert a("call", "rmtree", a("path", "slot")) == 0
        a("replace", "slot/progress.sav", "owner")
        before = recovery_files()
        assert b("call", "mount", 64, 128) != 0
        assert b("text").count(busy) == 1 and "work copy unavailable" not in b("text")
        assert recovery_files() == before, "second mount touched the owner's work"
        a("committed_deletes", 0)
        assert a("commit") == 0
        assert (c / "slot/progress.sav").read_bytes() == b"owner"
        assert a("committed_deletes") == 0, "publication deleted a committed file before replacing it"
        print("PASS: two processes: second writable mount is refused", flush=True)

        a("replace", "slot/progress.sav", "retry")
        a("fail_renames", 1000)
        assert a("commit") == ERROR and j.exists()
        failed = j.with_suffix(".journal.failed")
        failed.write_bytes(b"retired marker")
        before = recovery_files()
        assert b("call", "mount", 64, 128) != 0
        assert recovery_files() == before, "the non-owner replayed, retired or cleaned a pending save"
        assert a("commit") == ERROR and j.exists()
        assert recovery_files() == before
        assert a("committed_deletes") == 0
        a("fail_renames", 0)
        assert a("commit") == 0 and not j.exists()
        assert (c / "slot/progress.sav").read_bytes() == b"retry"
        print("PASS: non-owner leaves pending recovery alone; refused rename keeps J and C; next Commit replays", flush=True)

        a("unmount")
        before = recovery_files()
        assert b("call", "mount", 64, 128) != 0
        assert b("text").count(busy) == 3
        assert recovery_files() == before, "Unmount released ownership before process exit"
        a("mount")
        a("put", 4096, list(b"alias\0"))
        assert a("call", "mount", 4096, 128) == 0
        a("replace", "slot/progress.sav", "pending")
        a("call", "unmount", 4096)
        a("crash_apply", 1)
        # (the real publish exits at its first rename, after flushing J)
        p = workers[0].process
        p.stdin.write(json.dumps({"op": "commit", "args": []}) + "\n")
        p.stdin.flush()
        assert p.wait(timeout=5) == 86 and j.exists()
        assert (c / "slot/progress.sav").read_bytes() == b"retry"
        before = recovery_files()
        b("mount")
        assert (c / "slot/progress.sav").read_bytes() == b"pending"
        assert (w / "slot/progress.sav").read_bytes() == b"pending"
        assert not j.exists()
        assert failed.read_bytes() == b"retired marker" and lock.stat().st_ino == inode
        print("PASS: ownership survives Unmount and aliases; after owner exit a fresh process owns and replays J", flush=True)

        fallback = tmp / "fallback-owner"
        committed = fallback / "save" / UID
        committed.mkdir(parents=True)
        (committed / "option.sav").write_bytes(b"old" * 32)
        owner, second = ProcessFs(libpath, fallback), ProcessFs(libpath, fallback)
        workers.extend((owner, second))
        owner.request("limited_mount")
        assert "using the committed folder" in owner.request("text")
        owner.request("unmount")
        assert second.request("call", "mount", 64, 128) != 0
        assert busy in second.request("text"), "a failed staging setup released ownership"
        print("PASS: failed staging setup keeps ownership after an in-place mount and Unmount", flush=True)
    finally:
        for worker in reversed(workers):
            worker.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", nargs=2, metavar=("LIB", "ROOT"))
    parser.add_argument("--windows-worker", nargs=2, metavar=("LIB", "ROOT"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_fs.c")
    parser.add_argument("--case", choices=["a", "all"], default="all")
    args = parser.parse_args()
    if args.worker:
        process_worker(*args.worker)
        return
    if args.windows_worker:
        libpath, root = args.windows_worker
        f = Fs(ctypes.CDLL(libpath), Path(root))
        (f.c / "option.sav").write_bytes(b"old")
        f.mount()
        f.replace("option.sav", b"new")
        f.create("extra.sav")
        assert (f.c / "option.sav").read_bytes() == b"old"
        assert f.commit() == 0
        assert (f.c / "option.sav").read_bytes() == b"new"
        assert (f.c / "extra.sav").exists()
        f.replace("option.sav", b"discarded")
        f.unmount()
        f.mount()
        assert (f.w / "option.sav").read_bytes() == b"new"
        assert f.call("delete", f.path("extra.sav")) == 0
        assert f.commit() == 0
        assert not (f.c / "extra.sav").exists()
        f.clean()
        f.unmount()
        print("PASS: Windows save publish, discard, and delete")
        return
    if sys.platform not in ("linux", "win32"):
        print("SKIP: requires Windows or the Linux file layer")
        return
    # (the build hooks the functions it finds as HLE(name) at the start of a line, nx2/standalone.py: one declared
    #  through another macro is left to the game's own SDK code, which takes our handle for a pointer)
    hooked = set(re.findall(r"^HLE\((\w+)\)", args.source.read_text(encoding="utf-8"), re.M))
    for name in HOOKS:
        assert name in hooked, "not declared as HLE(%s) at the start of a line: the build would not hook it" % name
    if sys.platform == "linux":
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    os.environ["HOS_FS_TRACE"] = "1"
    with tempfile.TemporaryDirectory(prefix="fs-save-test-") as temporary:
        tmp = Path(temporary)
        (tmp / "a64.h").write_text(A64)
        (tmp / "stubs.c").write_text(STUBS)
        if sys.platform == "win32":
            libpath = tmp / "fs.dll"
            subprocess.run([args.cc, "-std=gnu11", "-shared", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
                            "-I", str(tmp), "-I", str(ROOT / "host"),
                            "-include", str(ROOT / "gpu/vk/compat/longshot_env.h"),
                            str(args.source.resolve()), str(tmp / "stubs.c"),
                            "-Wl,--export-all-symbols", "-o", str(libpath)], check=True)
            # The owner handle is held to process exit, as in the game.
            subprocess.run([sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                            "--windows-worker", str(libpath), str(tmp / "portable")], check=True)
            return
        libpath = tmp / "fs.so"
        flags = [args.cc, "-std=gnu11", "-D_GNU_SOURCE", "-fPIC", "-shared", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
                 "-I", str(tmp), "-I", str(ROOT / "host/posix"), "-I", str(ROOT / "host"),
                 "-include", str(ROOT / "gpu/vk/compat/longshot_env.h")]
        subprocess.run([args.cc, "-std=gnu11", "-fPIC", "-O1", "-Wall", "-Wextra", "-I", str(ROOT / "host/posix"),
                        "-c", str(ROOT / "host/posix/win32_posix.c"), "-o", str(tmp / "posix.o")], check=True)
        subprocess.run(flags + [str(args.source.resolve()), str(tmp / "stubs.c"),
                               str(tmp / "posix.o"), "-Wl,--wrap=MoveFileExA", "-Wl,--wrap=DeleteFileA",
                               "-Wl,--wrap=pthread_rwlock_wrlock", "-Wl,--wrap=pthread_rwlock_rdlock", "-Wl,-Bsymbolic",
                               "-lpthread", "-ldl", "-o", str(libpath)], check=True)
        run_cases(ctypes.CDLL(str(libpath)), tmp, args.case)
        if args.case == "all":
            process_cases(libpath, tmp)
    print("PASS: save commit HLE checks")


if __name__ == "__main__":
    main()
