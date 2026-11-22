"""Mask 0 and the cached-core preference (host/hos_thread.c).

The virtual-core block is compiled with a host-only harness; the wrapper and the TLS cell
are stubs, and the SRW locks are real. The checks are:
- mask 0 is no affinity: it uses every host core and prefers the core the guest cached;
- a mask change that excludes the held core is queued, and the next checkpoint moves it;
- every take writes the guest's TLS core cell: core + 1 for a 0/single-bit mask, 0 for a
  multi-bit one, and a same-core mask change updates it without moving the thread;
- a free and reallocation on another thread cannot land between the slot check and the
  store: the store holds the slot lock, so the free waits for it;
- a raw nonzero mask with no bit inside the virtual cores stays pinned.

python -I -B tests/test_vcore_mask0.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + (";" if source[end:end + 1] == ";" else "")


HARNESS = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
typedef void *HANDLE;
typedef int BOOL;
typedef unsigned long DWORD;
typedef struct { void *p; } SRWLOCK, *PSRWLOCK;
typedef struct { void *p; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define SRWLOCK_INIT {0}
#define HOS_TLS_SLOTS 16
typedef struct { uint64_t x[32]; volatile int poll; } Ctx;
static int64_t now_ns = 1000000000;
static int64_t hos_now_ns(void) { return now_ns; }
enum { HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL, HOS_CHAIN_SLEEP, HOS_CHAIN_AWAKE, HOS_CHAIN_PRESENT };
#define HOS_CHAIN(kind, key) ((void)(kind), (void)(key))
static void hos_log(const char *fmt, ...) { (void)fmt; }
static int hos_cores(void) { return 2; }
static volatile int hos_fps_timing;   /* (off: the frame trace's times not counted) */
static const char *hos_profiling_dir(void) { return 0; }
static void hos_profiling_event(const char *fmt, ...) { (void)fmt; }
static BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lk, DWORD ms, unsigned long f) {
    (void)cv, (void)lk, (void)ms, (void)f;
    return 1;
}
static void WakeConditionVariable(PCONDITION_VARIABLE cv) { (void)cv; }
/* (real locks: one test runs a free on another thread between the alias check and the store) */
static void AcquireSRWLockExclusive(PSRWLOCK l) {
    if (!l->p) l->p = new std::recursive_timed_mutex();
    static_cast<std::recursive_timed_mutex *>(l->p)->lock();
}
static void ReleaseSRWLockExclusive(PSRWLOCK l) {
    static_cast<std::recursive_timed_mutex *>(l->p)->unlock();
}
static int wrapper_valid, wrapper_mask;
static int alias_now;
static volatile int core_tls_slot = 0;
static SRWLOCK tls_slot_lock = SRWLOCK_INIT;
static int alias_frees;
static std::atomic<int> alias_helper_start{0}, alias_helper_done{0}, alias_helper_blocked{0};
static int alias_thread_arm;
static int core_tls_alias_now(void) {
    if (alias_thread_arm) {
        alias_thread_arm = 0;
        alias_helper_start.store(1);
        for (int i = 0; i < 400 && !alias_helper_done.load() && !alias_helper_blocked.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return !(alias_helper_blocked.load() && core_tls_slot == 3);   /* (0: valid, write it) */
    }
    if (alias_frees) core_tls_slot = -1;   /* (a free that lands right after the check) */
    return alias_now;
}
static void alias_helper(void) {
    while (!alias_helper_start.load()) std::this_thread::yield();
    if (!tls_slot_lock.p) tls_slot_lock.p = new std::recursive_timed_mutex();
    auto *m = static_cast<std::recursive_timed_mutex *>(tls_slot_lock.p);
    if (m->try_lock_for(std::chrono::milliseconds(100))) {
        core_tls_slot = 5;   /* (free slot 3 and reallocate it as another slot) */
        m->unlock();
        alias_helper_done.store(1);
    } else {
        alias_helper_blocked.store(1);
        m->lock();
        core_tls_slot = 5;
        m->unlock();
        alias_helper_done.store(1);
    }
}
static void *hos_guest_range(uint64_t a, uint64_t n) { (void)a, (void)n; return NULL; }
@STRUCT@
static HosThread *cur;
@CORE@
static int hos_thread_wrapper_mask(HosThread *t, uint32_t *out) {
    (void)t;
    if (!wrapper_valid) return 0;
    *out = (uint32_t)wrapper_mask;
    return 1;
}
@VCORES@
static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
static void thread(HosThread *t, const char *name, int prio, uint64_t mask) {
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->priority = prio, t->core_mask = mask, t->ideal_core = -2;
}
int main(void) {
    HosThread t, other;

    /* mask 0 is no affinity, but the guest's cached core is preferred */
    thread(&t, "mask0", 16, 0);
    check(vc_allowed(&t) == 3, "mask 0 allows both host cores");
    thread(&t, "mask1", 16, 1);
    check(vc_allowed(&t) == 1, "a one-core mask allows only that core");

    thread(&t, "mask0", 16, 0);
    wrapper_valid = 1, wrapper_mask = 0, alias_now = 0;
    t.tls_slots[0] = 2;   /* core 1 + 1, the cell the trampoline wrote */
    check(vc_pref_core(&t) == 1, "mask 0 prefers the core the guest cached");
    alias_now = 1;
    check(vc_pref_core(&t) == 0, "a stale/aliased cell is not trusted");
    alias_now = 0;

    memset(vc_owner, 0, sizeof vc_owner);
    check(vc_free_for(&t) == 1, "the cached core is taken while it is free");
    thread(&other, "holder", 16, 2);
    vc_owner[1] = &other;   /* the cached core is taken: another free core */
    check(vc_free_for(&t) == 0, "contention moves the thread to another free core");
    memset(vc_owner, 0, sizeof vc_owner);

    /* a mask change that excludes the held core is queued; the checkpoint moves it */
    thread(&t, "force", 16, 2);
    memset(vc_owner, 0, sizeof vc_owner);
    vc_nq = 0;
    vc_owner[0] = &t;
    cur = &t;
    t.vc_held = 1, t.vc_kick = 0, t.vc_last = 1, t.ctx.poll = 0;
    vc_note_mask_change(&t);
    check(t.vc_kick == 2, "a mask change outside the held core queues a move");
    check(t.ctx.poll == 1, "a remask sets the running context's interrupt");
    t.vc_kick = 0;
    t.ctx.poll = 0;
    Ctx fiber;
    memset(&fiber, 0, sizeof fiber);
    t.run_ctx = &fiber;
    vc_note_mask_change(&t);
    check(fiber.poll == 1, "a remask sets the running fiber's interrupt");
    t.run_ctx = NULL;
    /* (the request is still pending: the checkpoint's fast path returns early with no kick and no waiter) */
    vc_yield(&t, 0);
    check(t.vc_held == 2, "the next checkpoint moves to the allowed core");

    /* the TLS cell follows the core on every take and on a same-core mask change */
    wrapper_valid = 1, wrapper_mask = 2, alias_now = 0;
    thread(&t, "cell", 16, 2);
    t.tls_slots[0] = 9;
    vc_take(&t, 1);
    check(t.tls_slots[0] == 2, "a take writes core + 1");
    t.core_mask = 3, t.tls_slots[0] = 9;
    vc_take(&t, 0);
    check(t.tls_slots[0] == 0, "a multi-bit mask clears the cell");
    t.core_mask = 2;
    t.vc_held = 2, t.tls_slots[0] = 9;
    vc_note_mask_change(&t);
    check(t.vc_kick == 0 && t.tls_slots[0] == 2, "a same-core mask change syncs the cell");

    /* a free between the alias check and the store cannot turn the slot into -1 */
    wrapper_valid = 1, wrapper_mask = 2, alias_now = 0;
    thread(&t, "freed", 16, 2);
    core_tls_slot = 3;
    t.tls_slots[3] = 0;
    alias_frees = 1;
    vc_sync_tls(&t, 0);
    alias_frees = 0;
    check(t.core_mask == 2, "a freed slot never becomes a negative index");
    check(t.tls_slots[3] == 1, "the slot validated before the free is the one written");

    /* a free and reallocation on another thread cannot land between the check and the store */
    wrapper_valid = 1, wrapper_mask = 2, alias_now = 0;
    thread(&t, "raced", 16, 2);
    core_tls_slot = 3, t.tls_slots[3] = 0;
    alias_helper_start.store(0), alias_helper_done.store(0), alias_helper_blocked.store(0);
    std::thread helper(alias_helper);
    alias_thread_arm = 1;
    vc_sync_tls(&t, 0);
    helper.join();
    check(t.tls_slots[3] == 1, "the slot lock makes the free wait for the store");
    check(core_tls_slot == 5, "the free and reallocation finish after the store");
    alias_thread_arm = 0;
    core_tls_slot = 0;

    /* a raw mask with no bit inside the virtual cores stays pinned, not floating */
    thread(&t, "outside", 16, 0x40);
    wrapper_valid = 1, wrapper_mask = 0x40, alias_now = 0;
    t.vc_held = 1, t.vc_last = 1, t.ideal_core = 0, t.vc_kick = 0;
    t.tls_slots[0] = 2;   /* the guest cached core 1 */
    vc_nq = 0;
    memset(vc_owner, 0, sizeof vc_owner);
    vc_owner[0] = &t;
    check(vc_allowed(&t) == (1u << hos_thread_core(&t)), "a mask outside the virtual cores stays pinned");
    vc_owner[0] = &other;
    check(vc_free_for(&t) == -1, "a pinned out-of-range mask does not float to another core");
    vc_owner[0] = &t;
    vc_note_mask_change(&t);
    check(t.vc_kick == 0, "an out-of-range mask change does not queue a move");
    vc_take(&t, 0);
    check(t.tls_slots[0] == 1, "the pinned thread's cell is its core + 1");

    if (!bad) puts("ok");
    return bad;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "host/hos_thread.c")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    start = src.index("static SRWLOCK vc_lock = SRWLOCK_INIT;")
    end = src.index("\n#else", src.index("int hos_cv_sleep(void *cv, void *lk, unsigned ms) {", start))
    fixture = (HARNESS.replace("@STRUCT@", block(src, "struct HosThread {") + "\ntypedef struct HosThread HosThread;")
               .replace("@CORE@", block(src, "int hos_thread_core(HosThread *t) {"))
               .replace("@VCORES@", src[start:end]))
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="vcore-mask-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        flags = ["-pthread"] if os.name != "nt" else []
        # (env_on_or, the switches' rule HOS_VCORES is read by: gpu/vk/compat/longshot_env.h, as hos_thread.c has it)
        env_h = ROOT / "gpu/vk/compat/longshot_env.h"
        subprocess.run([compiler, "-std=c++17", "-O1", "-w", *flags, "-include", str(env_h), str(folder / "fixture.cpp"),
                        "-o", str(exe)], check=True, timeout=120)
        env = {k: v for k, v in os.environ.items() if k != "HOS_VCORES"}   # (the default: the virtual cores on)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, env=env)
        print(result.stdout, end="")
        if result.returncode or "ok" not in result.stdout:
            raise SystemExit("the virtual-core mask/TLS checks failed")


if __name__ == "__main__":
    main()
