"""The guest's pools placed in the GPU's own memory (gpu/vk/placed_memory.h, used by vk_memory.cpp's placed_*), where the
driver takes neither host pointers nor dma-bufs of the guest window: Turnip, Mesa's Adreno driver, on a kernel without
/dev/udmabuf (the AYN Odin 3's Armada OS), where no pool reached the GPU and nothing was drawn. Compiles and runs
tests/placed_memory_test.cpp, which places a memfd standing in for the device's dma-buf over a pool in a window made as
host/hos_main.c makes it, and gives it back; places one while threads write into it without pause, as a pool left in
ordinary memory is placed at its first GPU binding (RX_VK_PLACED_LATER_MB), and finds every write in the placement
(place_live), and makes one write meet a held range for sure (hold); and picks the pools left in ordinary memory a binding places (later_range). Then checks that vk_memory.cpp,
the host and the API between them are wired to it. No game, no GPU; Linux only (mmap, memfd, /proc/self/pagemap).

    python -I tests/test_placed_memory.py [--cxx g++]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import between, block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "mmap, memfd and /proc/self/pagemap: Linux only, as the placements are"}

ROOT = Path(__file__).resolve().parents[1]


def need(path, *needles):
    src = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in src:
            raise SystemExit(f"{path} no longer has: {needle}")
    return src


def body(src, head):
    """The function starting with head, to its closing brace at the start of a line"""
    start = src.index(head)
    return src[start:src.index("\n}\n", start)]


def check_source():
    src = need("gpu/vk/vk_memory.cpp",
               '#include "placed_memory.h"',
               "placed::plan(placements, lo, hi)",
               "placed::copy_in(fresh, at, n, placed_page, placed_pagemap, olds)",
               "placed::place(at, n, fd)",
               "placed::unplace(host->guest_base() + it->first, n, placed_page, &copied)",
               # (on where neither host pointers nor dma-bufs of the window work, and when asked for)
               "if (placed_setup(false)) return true;",
               'if (const char* e = getenv("RX_VK_PLACED"); e && !env_value_off(e) && !dma_buf_setup() && placed_setup(true)) return true;')
    # (an import is the placement's memory imported again, before the other ways are tried)
    imp = body(src, "bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {")
    if not 0 <= imp.find("if (placed_on && placed_import(lo, hi, p))") < imp.find("if (import_dma_buf(lo, hi, p))"):
        raise SystemExit("import_pool no longer imports from a pool's placement first")
    # (an import is the whole placement holding what is looked up: pools in one another's memory are one import)
    if "const bool placed = placed_on && placed_find(guest, size, placement);" not in body(src, "Pool* pool_lookup(uint64_t guest, uint64_t size) {"):
        raise SystemExit("pool_lookup no longer imports the placement holding the range")
    if "if (placed_on) return 0;" not in body(src, "uint64_t import_chunk() {"):
        raise SystemExit("import_chunk: chunks again where the pools are placed (a chunk can run past its placement)")
    # (a pool made is placed at once; one finalized is given back; and the imports of what went are retired before a lookup)
    if "placed_back(" not in body(src, "void memory_pool_created(uint64_t gpu, uint64_t size) {"):
        raise SystemExit("memory_pool_created no longer places the game's pools")
    if "placed_unback(" not in body(src, "void memory_pool_finalized(uint64_t gpu, uint64_t size) {"):
        raise SystemExit("memory_pool_finalized no longer gives the placements back")
    # (RX_VK_PLACED_LATER_MB: a big pool left in ordinary memory when made, placed at its first GPU binding with the game's
    # writes held back, forgotten when let go before; a large texture in one unswizzled on the CPU, not through a binding)
    # (what a pool made becomes is placed::made's, which tests/placed_memory_test.cpp runs: none left in ordinary memory ever
    # runs into a placement, so placing one at a binding takes in no placement the GPU may be writing; a refused one is
    # remembered, so that letting it go takes nothing off a placement made over it since)
    back = body(src, "void placed_back(uint64_t lo, uint64_t hi) {")
    for needle in ("switch (placed::made(plan, placed_later, lo, hi, placed_later_min)) {",
                   "placed_later.add(lo, hi);",
                   'if (!placed_wake(lo, hi, "as a pool made runs into it")) {\n            placed_refused.emplace_back(lo, hi);',
                   "if (!placed_make(lo, hi, 1, false)) placed_refused.emplace_back(lo, hi);"):
        if needle not in back:
            raise SystemExit("placed_back no longer does as placed::made says: " + needle)
    make = body(src, 'bool placed_make(uint64_t lo, uint64_t hi, int pools_over, bool live, const char* why = "at its first GPU binding") {')
    if "if (live || !plan.absorbed.empty()) {" not in make:
        raise SystemExit("placed_make no longer holds the game's writes back while it takes placements in")
    if "if (placed && placed_gone_any.load(std::memory_order_acquire)) placed_retire_gone();" not in \
            body(src, "Pool* pool_lookup(uint64_t guest, uint64_t size) {"):
        raise SystemExit("pool_lookup no longer retires the imports of placements a placement at its lookup took in")
    if "placed_later_min = mb > 0 && !forced ?" not in body(src, "bool placed_setup(bool forced) {"):
        raise SystemExit("placed_setup leaves pools in ordinary memory where host pointers work (RX_VK_PLACED=1)")
    if 'if (woke || !placed_wake(guest, guest + size, "at its first GPU binding")) return false;' not in \
            body(src, "bool placed_find(uint64_t guest, uint64_t size, GuestPool& out) {"):
        raise SystemExit("placed_find no longer places a pool left in ordinary memory at its first GPU binding")
    if "if (placed_make(r.lo, r.hi, pools, true, why)) return true;" not in body(src, "bool placed_wake(uint64_t lo, uint64_t hi, const char* why) {"):
        raise SystemExit("placed_wake no longer places what the binding runs into as a pool the game may be writing")
    if "ok = placed::place_live(placed_barrier, at, n, fd, fresh, placed_page, placed_pagemap, olds, &copied, &err);" not in make:
        raise SystemExit("placed_make no longer holds the game's writes back while it places a pool it may be writing")
    if "return placed_barrier.held((uintptr_t)r->ExceptionInformation[1]) ? EXCEPTION_CONTINUE_EXECUTION" not in \
            body(src, "LONG CALLBACK placed_held(EXCEPTION_POINTERS* e) {"):
        raise SystemExit("placed_held no longer lets a held-back write run again")
    setup = body(src, "bool placed_setup(bool forced) {")
    # (whatever RX_VK_PLACED_LATER_MB says: a placement that takes in others holds the game's writes back too)
    lines = setup.splitlines()
    at = [i for i, line in enumerate(lines) if "AddVectoredExceptionHandler(1, placed_held);" in line]
    if len(at) != 1 or lines[at[0]] != "        AddVectoredExceptionHandler(1, placed_held);" or \
            lines[at[0] - 1].strip().startswith(("if", "else")) or not setup.find(lines[at[0]]) < setup.find("placed_on = true;"):
        raise SystemExit("placed_setup no longer registers the handler always, before any pool is placed")
    unback = body(src, "void placed_unback(uint64_t lo, uint64_t hi) {")
    if unback.find("placed_refused.erase(r);") < 0 or not unback.find("placed_refused.erase(r);") < unback.find("placements.upper_bound(lo)"):
        raise SystemExit("placed_unback no longer takes a refused pool off before it looks at the placements")
    if "return placed_later_let_go(lo, hi);" not in unback or \
            "if (!placed_later.let_go(lo, hi)) return;" not in body(src, "void placed_later_let_go(uint64_t lo, uint64_t hi) {"):
        raise SystemExit("placed_unback no longer forgets a pool let go while in ordinary memory")
    tex = body(need("gpu/vk/vk_texture.cpp"), "bool upload_unswizzle_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {")
    if not 0 <= tex.find("if (memory_placed_later(tex.gpu, total)) return false;") < tex.find("gpu_buffer(tex.gpu, total)"):
        raise SystemExit("upload_unswizzle_gpu would bind, and so place, a pool left in ordinary memory for one texture")
    if "if (placed_gone_any.load(std::memory_order_acquire)) placed_retire_gone();" not in body(src, "Pool* pool_of(uint64_t guest, uint64_t size) {"):
        raise SystemExit("pool_of no longer retires the imports of placements given back: the GPU would keep the old memory")
    # (a placed page given back with madvise would not read as zero: the host zeroes it instead, as an imported one)
    if "placements.lower_bound(hi)" not in body(src, "int memory_guest_imported(uint64_t lo, uint64_t hi) {"):
        raise SystemExit("memory_guest_imported no longer counts the placements")
    need("gpu/vk/vk_loader.h", "X(vkGetMemoryFdKHR)")
    need("gpu/vk/vk_api.h", "void (*nvn_pool_finalized)(uint64_t gpu, uint64_t size);")
    need("gpu/vk/vk_replay.cpp", "t.nvn_pool_finalized = memory_pool_finalized;")
   # (host/rx_backend.c's side: check_told_while_starting runs it)
    fin = body(need("host/nvn_native.c"), "NV(nvnMemoryPoolFinalize) {")
    if "rx_nvn_pool_finalized(o->gpu, o->size);" not in fin:
        raise SystemExit("nvnMemoryPoolFinalize no longer tells the renderer: the pool's memory would stay the device's")


# (host/rx_backend.c: the pools the game makes and lets go while the renderer starts are told to it. The game's thread runs on
# while the Vulkan back end starts (3 s of shaders on the Odin 3), and the pools it made then (the render target pool, the queues'
# event memory) were told to no renderer: never placed, never imported, every draw that used them skipped, a black screen)
TOLD_FIXTURE = r"""
#include <stdint.h>
#include <stdio.h>
typedef struct MkwVkRenderer {
    int (*start)(void *hwnd);
    void (*nvn_pool_created)(uint64_t gpu, uint64_t size);
    void (*nvn_pool_finalized)(uint64_t gpu, uint64_t size);
} MkwVkRenderer;
static void hos_log(const char *f, ...) { (void)f; }
static void flush_early(void) {}
static int wgr_start(void *hwnd) { (void)hwnd; return 0; }
static int made, gone;
static void created(uint64_t gpu, uint64_t size) { (void)gpu, (void)size; made++; }
static void finalized(uint64_t gpu, uint64_t size) { (void)gpu, (void)size; gone++; }
void rx_nvn_pool_created(uint64_t gpu, uint64_t size);
void rx_nvn_pool_finalized(uint64_t gpu, uint64_t size);
static int start(void *hwnd) {   /* (the game's thread makes a pool and lets one go while the renderer starts) */
    (void)hwnd;
    rx_nvn_pool_created(0x100c867000ull, 0x20000000ull);
    rx_nvn_pool_finalized(0x100c867000ull, 0x20000000ull);
    return 1;
}
static const MkwVkRenderer renderer = {start, created, finalized};
static const MkwVkRenderer *load_vulkan(const char *dll) { (void)dll; return &renderer; }
@@STATE@@
@@FUNCS@@
int main(void) {
    rx_nvn_pool_created(1, 1);
    if (made) return puts("FAIL: a pool told to a renderer before there was one"), 1;
    if (!rx_start(NULL)) return puts("FAIL: the renderer did not start"), 1;
    if (made != 1 || gone != 1) {
        printf("FAIL: the pool the game made and let go while the renderer started: told %d and %d times, not 1 and 1\n", made, gone);
        return 1;
    }
    rx_nvn_pool_created(2, 2);
    if (made != 2) return puts("FAIL: a pool made after the start not told"), 1;
    puts("pools made while the renderer starts: told");
    return 0;
}
"""


def check_told_while_starting(cxx, tmp):
    src = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    state = between(src, "static const MkwVkRenderer *vk;", "static HMODULE vk_module;")
    funcs = "\n".join(block(src, marker) for marker in ("int rx_start(void *hwnd) {", "void rx_nvn_pool_created(uint64_t gpu",
                                                      "void rx_nvn_pool_finalized(uint64_t gpu"))
    c = Path(tmp) / "told.c"
    c.write_text(TOLD_FIXTURE.replace("@@STATE@@", state).replace("@@FUNCS@@", funcs), encoding="utf-8")
    exe = Path(tmp) / "told"
    subprocess.run([cxx, "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="placed-memory-") as tmp:
        exe = Path(tmp) / "placed-memory"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                        str(ROOT / "tests/placed_memory_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=120)
        check_told_while_starting(args.cxx, tmp)


if __name__ == "__main__":
    main()
