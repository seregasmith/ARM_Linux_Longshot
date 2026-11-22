"""The output preview's native window swap and its way back, from host/nvn_native.c, with no game.

The preview publishes its own window into the game's window owner at a native acquisition, and every present of that
window carries the trial's generation; the renderer drops a tagged present once its trial is gone
(gpu/vk/vk_replay.cpp, output_trial_skip_present), and hos_defer_native_output_rebuild holds the game's own window
rebuild while a generation is published. This check cuts the controller (the window owner's swap, the idle marker of the
presentation queue, the rollback at acquisition, the present's tag and the rebuild hook) out of the real source and runs
it against a fake window owner and renderer:

- publication at an idle acquisition, and a rollback that swaps the stock window back;
- a rollback that cannot swap back (the owner's texture slots no longer name either window's textures) after the trial
  expired: within a second its presents carry no generation, so they are shown, the game's rebuild runs again, no
  acquisition waits for the rollback any more, and the candidate is retired once the rebuilt window names none of its
  textures.

Before the fix the failed rollback kept the generation published for good: every present of the candidate window was
dropped (the picture stayed on its last frame) and the game's rebuild was skipped.

It does not model the threads (the presentation thread and the game's are one here), the renderer's trial (a stub
answers its phase) or the game's real window owner.

python -I tests/test_output_rollback.py [--cc gcc] [--source host/nvn_native.c]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import between

RUN_ALL_ARGS = ["--cc", "{cc}"]
ROOT = Path(__file__).resolve().parents[1]

STUBS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rx_nvn.h"

enum { OUTPUT_TRIAL_EXPIRED, OUTPUT_TRIAL_PREPARING, OUTPUT_TRIAL_READY, OUTPUT_TRIAL_ACTIVE, OUTPUT_TRIAL_SHOWN };
typedef struct { uint64_t x[32]; } Ctx;
typedef uint32_t DWORD;
typedef long LONG;
typedef struct { int held; } SRWLOCK;
#define SRWLOCK_INIT {0}
static void AcquireSRWLockExclusive(SRWLOCK *s) { if (s->held) abort(); s->held = 1; }
static int TryAcquireSRWLockExclusive(SRWLOCK *s) { if (s->held) return 0; s->held = 1; return 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *s) { if (!s->held) abort(); s->held = 0; }
static DWORD GetCurrentThreadId(void) { return 7; }
typedef struct HosThread HosThread;
static HosThread *hos_self(void) { return NULL; }
static const char *hos_thread_name(HosThread *t) { (void)t; return ""; }

/* Guest memory is the host's here: a guest address is the host pointer, but for the game's framework pointer */
static uint8_t framework[0x100];
static uint64_t h2g(const void *p) { return (uint64_t)(uintptr_t)p; }
#define GP(a) ((void *)(uintptr_t)(a))
static void *hos_guest_range(uint64_t a, uint64_t n) { (void)n; return GP(a); }
int hos_mem_mapped(uint64_t a, uint64_t n) { (void)n; return a != 0; }
static uint64_t recomp_load64(Ctx *c, uint64_t a) {
    (void)c;
    if (a == 0x84ea2de0) return h2g(framework);
    uint64_t v;
    memcpy(&v, GP(a), sizeof v);
    return v;
}
static uint8_t recomp_load8(Ctx *c, uint64_t a) { (void)c; return *(const uint8_t *)GP(a); }
static void recomp_store64(Ctx *c, uint64_t a, uint64_t v) { (void)c; memcpy(GP(a), &v, sizeof v); }
static void recomp_store8(Ctx *c, uint64_t a, uint8_t v) { (void)c; *(uint8_t *)GP(a) = v; }

static int64_t now = 1000000000;
static int sleeps;
static int64_t hos_now_ns(void) { return now; }
static void hos_sleep_ns(int64_t n) { now += n; sleeps++; }
static int hos_vcore_away(void) { return 0; }
static void hos_vcore_back(void) {}
static void hos_log(const char *fmt, ...) { (void)fmt; }

/* The renderer's trial: its phase, and its publication of the window at the acquisition */
static int phase = OUTPUT_TRIAL_EXPIRED;
static uint64_t generations;
int rx_output_trial_phase(uint64_t serial, int cancel) {
    (void)serial;
    if (cancel) phase = OUTPUT_TRIAL_EXPIRED;
    return phase;
}
int rx_output_trial_publish(uint64_t serial, void (*apply)(void *, uint64_t), void *state) {
    (void)serial;
    if (phase != OUTPUT_TRIAL_READY) return 0;
    phase = OUTPUT_TRIAL_ACTIVE;
    apply(state, ++generations);
    return 1;
}
'''

CASES = r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint8_t owner[0x200], receiver[0x200], graphics[0x600];
static nvn_win stock, rebuilt;
static nvn_tex stock_tex[3], rebuilt_tex[3];
static detached_window candidate;
static SRWLOCK queue = SRWLOCK_INIT;
static unsigned life = 1;
static int queued;

static void owner_names(const nvn_win *w) {
    recomp_store64(NULL, h2g(owner) + 0x70, h2g(w));
    for (int i = 0; i < 3; ++i) recomp_store64(NULL, h2g(owner) + 0x88 + i * 8, w->tex[i]);
}

/* The presentation thread: it takes a present from its queue, presents it, then waits on the empty queue */
static uint64_t present_then_idle(nvn_win *o) {
    nvn_detached_present_received(&queue, life);
''' + "@@TAG@@" + r'''
    nvn_detached_present_wait(&queued, &queue, &life);
    return preview;
}

/* The game's thread, at its wait before the window's next acquisition */
static void acquire(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[30] = 0x81207a6c;
    c.x[19] = h2g(receiver);
    nvn_detached_pre_acquire(&c);
}

static int game_rebuild_held(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = h2g(owner);
    return hos_defer_native_output_rebuild(&c);
}

static uint64_t publish(void) {
    for (int i = 0; i < 3; ++i) {
        candidate.window.tex[i] = h2g(&candidate.textures[i]);
        candidate.textures[i].serial = 101 + i;
    }
    detached_candidate = &candidate;
    detached_retired = NULL;
    detached_activate_until = now + 2000000000LL;
    owner_names(&stock);
    owner[0xc8] = 0;
    phase = OUTPUT_TRIAL_READY;
    CHECK(present_then_idle(&stock) == 0);
    acquire();
    CHECK(detached_published);
    CHECK(recomp_load64(NULL, h2g(owner) + 0x70) == h2g(&candidate.window));
    CHECK(graphics[0x549] == 1);
    const uint64_t generation = present_then_idle(&candidate.window);
    CHECK(generation && generation == generations);
    return generation;
}

int main(void) {
    for (int i = 0; i < 3; ++i) {
        stock.tex[i] = h2g(&stock_tex[i]);
        rebuilt.tex[i] = h2g(&rebuilt_tex[i]);
    }
    stock.ntex = stock.active = rebuilt.ntex = rebuilt.active = 3;
    owner[0xaa] = 3;
    recomp_store64(NULL, h2g(framework) + 0xd8, h2g(graphics));
    recomp_store64(NULL, h2g(receiver) + 0x180, h2g(owner));
    detached_owner = h2g(owner);
    detached_original = detached_old_window = h2g(&stock);
    detached_boot_addr = h2g(&candidate);
    detached_boot_thread = GetCurrentThreadId();
    detached_idle_enabled = 1;

    /* Published, then No: the next idle acquisition swaps the stock window back */
    publish();
    phase = OUTPUT_TRIAL_EXPIRED;
    acquire();
    CHECK(!detached_published && !detached_published_generation);
    CHECK(recomp_load64(NULL, h2g(owner) + 0x70) == h2g(&stock) && graphics[0x549] == 0);
    CHECK(detached_retired == &candidate);
    CHECK(present_then_idle(&stock) == 0);

    /* Published, then the owner's slots change under it and the game asks for its rebuild: the swap back cannot be made */
    const uint64_t generation = publish();
    recomp_store64(NULL, h2g(owner) + 0x88, h2g(&rebuilt_tex[0]));
    owner[0xc8] = 1;
    CHECK(game_rebuild_held());
    phase = OUTPUT_TRIAL_EXPIRED;
    uint64_t tag = generation;
    for (int frame = 0; frame < 40; ++frame) {
        tag = present_then_idle(&candidate.window);
        acquire();
        now += 100000000;
    }
    if (tag) {
        fprintf(stderr, "FAIL: 4 s after its trial expired and the swap back failed, a present of the preview window still "
                        "carries generation %llu, which the renderer drops: the picture stays on its last frame\n",
                (unsigned long long)tag);
        return 1;
    }
    CHECK(!detached_published_generation);
    CHECK(!game_rebuild_held());   /* (the game's own rebuild runs again) */
    const int slept = sleeps;
    acquire();
    CHECK(sleeps == slept);   /* (no acquisition waits for a rollback given up) */
    CHECK(detached_published && detached_candidate == &candidate);   /* (its storage is still named by the owner) */

    /* The game rebuilt its window: once it names none of the candidate's textures, the candidate is retired */
    owner_names(&rebuilt);
    owner[0xc8] = 0;
    CHECK(present_then_idle(&rebuilt) == 0);
    acquire();
    CHECK(!detached_published && !detached_candidate && detached_retired == &candidate);
    puts("PASS: the preview window is published and swapped back; a swap back that fails ends the trial's tag and "
         "the held rebuild, and the candidate is retired once the rebuilt window drops it");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--source", type=Path, default=ROOT / "host" / "nvn_native.c")
    args = parser.parse_args()
    native = args.source.read_text(encoding="utf-8")
    controller = between(native, "/* An alternate native window and three output textures",
                         "static void detached_probe_capture(Ctx *c, const nvn_win *win) {")
    tag = between(native, "        uint64_t preview = 0;\n        if (__atomic_load_n(&detached_idle_enabled",
                  "        if (preview)\n")
    cc = shutil.which(args.cc)
    if not cc:
        raise SystemExit("test_output_rollback: no C compiler %r (pass --cc)" % args.cc)
    with tempfile.TemporaryDirectory(prefix="output-rollback-") as tmp:
        work = Path(tmp)
        source = work / "rollback.c"
        program = work / "rollback.exe"
        source.write_text(STUBS + controller + CASES.replace("@@TAG@@", tag), encoding="utf-8")
        subprocess.run([cc, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                        "-I", str(ROOT / "gpu" / "render"), str(source), "-o", str(program)], check=True)
        subprocess.run([str(program)], check=True, timeout=10)


if __name__ == "__main__":
    main()
