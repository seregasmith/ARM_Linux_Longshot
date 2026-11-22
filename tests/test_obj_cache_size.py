"""The per-thread cache in front of the sync objects' table, sized by HOS_OBJ_CACHE (host/nn_os_sync.c obj_cache_entries,
obj_lookup_cached, obj_nockpt; 16384 entries unless HOS_OBJ_CACHE says otherwise).

Compiles the real object table (find_locked, bury_locked, find_dead_locked, insert_locked, find_unlocked, obj_lookup),
the cache and obj_nockpt from host/nn_os_sync.c with a single-threaded harness (the SRW locks and the clock stubbed;
obj_lookup counted). Checks:

- obj_cache_entries: unset, empty or not a number is 16384; 0 is 0 (the fixed cache); otherwise 256 to 65536, rounded up
  to a power of two;
- with HOS_OBJ_CACHE=0, a thread that walks 4,000 objects (a table whose slots each have a mutex, as OoT's f_80364080 searches one) keeps
  missing in the fixed 1024-entry cache pass after pass; unset (16384) the passes after the first miss under 2 % (47 of 4,000
  here: the sets that hold five of the walk's slots); with HOS_OBJ_CACHE=256 they miss again (the size is honoured);
- with the cache sized, an entry is not used once its object belongs to another address or kind (re-initialized as
  another kind, buried, its spare taken by another address): every lookup still returns the table's object for that
  address and kind, compared with obj_lookup itself over 200,000 random operations;
- the log line comes once 65,536 lookups are counted and 10 s have passed, with the lookups and misses, made by
  obj_cache_flush as obj() calls it (nothing obj_nockpt reaches logs: it runs under mw_lock, test_multiwait_wake.py).

`--bench` prints a micro-benchmark (not a frame rate): ns per lookup for that 4,000-object walk among 60,000 objects, with
the fixed cache (HOS_OBJ_CACHE=0) and with 4096, 16384 and 65536 entries.

python -I -B tests/test_obj_cache_size.py [--cc clang] [--bench]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block, between  # noqa: E402

RUN_ALL_ARGS = ["--cc", "clang"]

ROOT = Path(__file__).resolve().parents[1]
ENV_ON_H = ROOT / "gpu" / "vk" / "compat" / "longshot_env.h"

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
typedef int SRWLOCK;
typedef int CONDITION_VARIABLE;
typedef long long LONG64;
typedef struct HosThread HosThread;
#define SRWLOCK_INIT 0
static void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }
static void AcquireSRWLockShared(SRWLOCK *l) { (void)l; }
static void ReleaseSRWLockShared(SRWLOCK *l) { (void)l; }
static void InitializeSRWLock(SRWLOCK *l) { *l = 0; }
static void InitializeConditionVariable(CONDITION_VARIABLE *c) { *c = 0; }
static LONG64 InterlockedAdd64(volatile LONG64 *v, LONG64 d) { return *v += d; }
static LONG64 InterlockedIncrement64(volatile LONG64 *v) { return ++*v; }
static LONG64 InterlockedExchange64(volatile LONG64 *v, LONG64 x) { LONG64 o = *v; *v = x; return o; }
static LONG64 InterlockedCompareExchange64(volatile LONG64 *v, LONG64 x, LONG64 c) { LONG64 o = *v; if (o == c) *v = x; return o; }
static int64_t clock_ns = 1;
static int64_t hos_now_ns(void) { return clock_ns; }
static int logs;
static long long logged_looks, logged_misses;
static void hos_log(const char *fmt, ...);
enum { K_MUTEX = 1, K_COND, K_EVENT, K_SEMA, K_QUEUE, K_RWLOCK, K_MULTIWAIT, K_HOLDER, K_TIMER };
'''

MAIN = r'''
#include <stdarg.h>
static void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    (void)va_arg(a, int);                     /* entries */
    (void)va_arg(a, long long);               /* made */
    logged_looks = va_arg(a, long long);
    logged_misses = va_arg(a, long long);
    va_end(a);
    logs++;
}
static void require(int ok, const char *m) { if (!ok) { fprintf(stderr, "FAIL: %s\n", m); exit(1); } }
static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double now_s(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec + t.tv_nsec * 1e-9; }
static long lookups_made;
/* (obj_lookup counted: the real one, through a wrapper the cut code calls) */
static Obj *counted_lookup(uint64_t key, int kind) { lookups_made++; return obj_lookup_real(key, kind); }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "check";
    const char *size = getenv("HOS_OBJ_CACHE");
    if (!strcmp(mode, "entries")) {
        struct { const char *v; int want; } cases[] = {{NULL, 16384}, {"", 16384}, {"x", 16384}, {"0", 0}, {"-5", 16384}, {"12abc", 16384},
            {"1", 256}, {"256", 256}, {"1000", 1024}, {"1024", 1024}, {"16384", 16384}, {"16385", 32768}, {"100000", 65536}};
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
            if (obj_cache_entries(cases[i].v) != cases[i].want) {
                fprintf(stderr, "FAIL: obj_cache_entries(%s) = %d, not %d\n", cases[i].v ? cases[i].v : "NULL",
                        obj_cache_entries(cases[i].v), cases[i].want);
                return 1;
            }
        puts("ok: obj_cache_entries");
        return 0;
    }
    /* the game's other objects: the table as large as in a scene */
    const int others = !strcmp(mode, "bench") ? 60000 : 20000;
    for (int i = 0; i < others; i++) obj_nockpt(0x100000000ull + (next() % (1ull << 34)) * 8, K_EVENT);
    /* the walk: 4,000 slots of 0x80 bytes, each with its mutex, passed again and again */
    enum { SLOTS = 4000 };
    const uint64_t base = 0x2000000000ull;
    if (!strcmp(mode, "bench")) {
        double best = 1e9;
        for (int rep = 0; rep < 7; rep++) {
            const double t0 = now_s();
            for (int pass = 0; pass < 50; pass++)
                for (int s = 0; s < SLOTS; s++) {
                    Obj *m = obj_nockpt(base + (uint64_t)s * 0x80, K_MUTEX);
                    if (!m) return 2;
                }
            const double ns = (now_s() - t0) / (50.0 * SLOTS) * 1e9;
            if (ns < best) best = ns;
        }
        printf("bench: HOS_OBJ_CACHE %-6s: %.1f ns a lookup (a walk of %d mutexes among %d objects; best of 7)\n",
               size ? size : "unset", best, SLOTS, others);
        return 0;
    }
    long before = 0;
    for (int pass = 0; pass < 4; pass++) {
        before = lookups_made;
        for (int s = 0; s < SLOTS; s++) {
            Obj *m = obj_nockpt(base + (uint64_t)s * 0x80, K_MUTEX);
            require(m && m->key == base + (uint64_t)s * 0x80 && m->kind == K_MUTEX, "a lookup returns its object");
        }
    }
    const long last_pass = lookups_made - before;
    const int entries = obj_cache_entries(size);
    if (!entries) require(last_pass > SLOTS / 2, "=0: the fixed cache misses a 4,000-object walk pass after pass");
    else if (entries >= 16384) require(last_pass <= SLOTS / 50, "sized: the walk's later passes miss under 2 %");
    else require(last_pass > SLOTS / 2, "a cache smaller than the walk misses again: the size is honoured");
    /* objects changing under the cache: compared with the table itself */
    enum { KEYS = 3000 };
    for (int it = 0; it < 200000; it++) {
        const uint64_t key = 0x3000000000ull + (next() % KEYS) * 0x40;
        const int kind = 1 + (int)(next() % 3);
        const uint64_t op = next() % 100;
        if (op < 3) {   /* finalized: a tombstone (drop's way) */
            AcquireSRWLockExclusive(&table_lock);
            Obj *o = find_locked(key);
            if (o) __atomic_store_n(&o->kind, 0, __ATOMIC_RELEASE);
            ReleaseSRWLockExclusive(&table_lock);
            continue;
        }
        Obj *got = obj_nockpt(key, kind);
        require(got && got->key == key && got->kind == kind, "a lookup returns an object of that address and kind");
        AcquireSRWLockExclusive(&table_lock);
        Obj *t = find_locked(key);
        ReleaseSRWLockExclusive(&table_lock);
        require(t == got, "the cache returns the table's object for the address");
    }
    if (entries) {
        logs = 0;
        clock_ns += 20000000000ll;
        for (int i = 0; i < 70000; i++) {   /* (obj()'s way: the lookup, then the counts when they are due) */
            obj_nockpt(base + (uint64_t)(i % SLOTS) * 0x80, K_MUTEX);
            if (__atomic_load_n(&obj_cache_due, __ATOMIC_RELAXED)) obj_cache_flush();
        }
        require(logs == 1 && logged_looks >= 65536 && logged_misses >= 0 && logged_misses <= logged_looks,
                "the log line, once, with its counts");
    }
    printf("ok: HOS_OBJ_CACHE %s: the walk's last pass reached obj_lookup %ld times\n", size ? size : "unset", last_pass);
    return 0;
}
'''


def source_pieces(src):
    obj = between(src, "typedef struct Obj {", "static int wake_lat_on(void);")
    table = between(src, "/* ---- the object table: open addressing on the guest address", "static Obj *obj_lookup(uint64_t key, int kind);")
    cache = between(src, "static int obj_cache_entries(const char *v) {", "static Obj *obj_lookup(uint64_t key, int kind) {")
    real = block(src, "static Obj *obj_lookup(uint64_t key, int kind) {")
    return [obj, table,
            "static Obj *obj_lookup_real(uint64_t key, int kind);\nstatic Obj *counted_lookup(uint64_t key, int kind);\n"
            "static int obj_cache_due;\n"
            "#define obj_lookup counted_lookup",
            cache.replace("static Obj *obj_nockpt(uint64_t key, int kind);", ""),
            "#undef obj_lookup",
            real.replace("static Obj *obj_lookup(uint64_t key, int kind) {", "static Obj *obj_lookup_real(uint64_t key, int kind) {", 1)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--bench", action="store_true")
    a = ap.parse_args()
    cc = shutil.which(a.cc)
    if not cc:
        raise SystemExit("SKIP: no %s" % a.cc)
    src = (ROOT / "host/nn_os_sync.c").read_text(encoding="utf-8")
    if "obj_cache_flush()" not in block(src, "static Obj *obj(uint64_t key, int kind) {"):
        raise SystemExit("FAIL: obj() does not add the sized cache's counts up (obj_cache_flush)")
    nock = block(src, "static Obj *obj_nockpt(uint64_t key, int kind) {")
    if "obj_lookup_cached(key, kind, entries)" not in nock or 'getenv("HOS_OBJ_CACHE")' not in nock:
        raise SystemExit("FAIL: obj_nockpt does not take HOS_OBJ_CACHE's sized cache")
    code = ENV_ON_H.read_text(encoding="utf-8") + HARNESS + "\n".join(source_pieces(src)) + MAIN
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "obj_cache.c")
        exe = os.path.join(tmp, "obj_cache.exe" if os.name == "nt" else "obj_cache")
        Path(c).write_text(code, encoding="utf-8")
        flags = ["-O2", "-std=gnu11", "-w"]
        if os.name != "nt":
            flags.append("-fsanitize=address,undefined")
        r = subprocess.run([cc, *flags, c, "-o", exe], capture_output=True, text=True)
        if r.returncode:
            sys.stderr.write(r.stdout + r.stderr)
            raise SystemExit("FAIL: the fixture does not build")
        runs = [("entries", None)] + ([("bench", v) for v in ("0", "4096", None, "65536")] if a.bench else
                                      [("check", v) for v in (None, "0", "256")])
        for mode, value in runs:
            env = dict(os.environ)
            env.pop("HOS_OBJ_CACHE", None)
            env.pop("HOS_NO_OBJ_CACHE", None)
            if value:
                env["HOS_OBJ_CACHE"] = value
            r = subprocess.run([exe, mode], capture_output=True, text=True, env=env, timeout=600)
            sys.stdout.write(r.stdout)
            if r.returncode:
                sys.stderr.write(r.stderr)
                raise SystemExit("FAIL: %s with HOS_OBJ_CACHE %s" % (mode, "=" + value if value else "unset"))
    print("ok: test_obj_cache_size")


if __name__ == "__main__":
    main()
