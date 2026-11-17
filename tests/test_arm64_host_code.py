"""The runtime's x86-64-only code has an ARM64 Linux side (the arm64 port, docs/arm64-port-plan.md). Each piece is cut from
the source and compiled with clang for aarch64-linux-gnu; what can run here runs:

  - host/hos_setjmp.c: recomp_sj_setjmp / recomp_sj_longjmp were x86-64 assembly on every non-Windows system ("unknown token
    in expression" for ARM64). Assembled for ARM64 here, and on an ARM64 Linux PC run: setjmp returns 0, then the longjmp's
    value (1 for a 0) in the caller's own frame after a jump from deeper calls, with FPCR as setjmp saw it.
  - host/hos_thread.c: <emmintrin.h> unguarded ("This header is only meant to be used on x86 and x64 architecture") and
    _mm_pause in the present poll's spin; host/posix/windows.h: YieldProcessor as __builtin_ia32_pause. Compiled for ARM64
    and for x86-64.
  - host/posix/win32_posix.c: the signal's context as a CONTEXT from gregs[REG_RIP] and the rest ("no member named 'gregs' in
    'mcontext_t'" on ARM64), and a fault's direction from REG_ERR. Run with glibc's ARM64 mcontext_t laid out as a stand-in,
    on any PC: Rip, Rsp and Rbp are pc, sp and x29 and go back; a write is a data abort's ESR with WnR set, found among the
    signal frame's records after others; anything else (a read, an instruction fetch, no ESR record, a broken record) is not.
  - host/posix/prof.c: the sampler's and the watch's interrupted pc, from gregs[REG_RIP]: pc on ARM64.

Game-free; the host files need no lift for these pieces.

    python -I -B tests/test_arm64_host_code.py [--cc clang]
"""
import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "clang"]

ROOT = Path(__file__).resolve().parents[1]
ARM64 = "--target=aarch64-linux-gnu"
X86_64 = "--target=x86_64-linux-gnu"
ON_ARM64_LINUX = sys.platform.startswith("linux") and platform.machine().lower() in ("aarch64", "arm64")


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def between(text, start, end, what):
    """text from start (included) to end (left out), each the first after the one before"""
    if start not in text or end not in text[text.index(start):]:
        raise SystemExit("FAIL: %s: the source moved (%r .. %r not found): update this test" % (what, start[:40], end[:40]))
    a = text.index(start)
    return text[a:text.index(end, a)]


def compile_c(cc, tmp, name, source, flags, what, run=False):
    src = Path(tmp) / (name + ".c")
    src.write_text(source, encoding="utf-8")
    out = Path(tmp) / (name + (".exe" if os.name == "nt" and run else "" if run else ".o"))
    r = subprocess.run([cc, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function"] + flags +
                       ([] if run else ["-c"]) + [str(src), "-o", str(out)], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("FAIL: %s:\n%s" % (what, (r.stdout + r.stderr)[-3000:]))
    if run:
        r = subprocess.run([str(out)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: %s (exit %d):\n%s" % (what, r.returncode, (r.stdout + r.stderr)[-3000:]))
        print(r.stdout.strip())


SETJMP_RUN = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static void *buf[32];
static uint64_t fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static void set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v)); }
__attribute__((noinline)) static void deep(int n, int value) {
    volatile char pad[512];   /* (a frame of its own below the caller's) */
    pad[n & 511] = (char)n;
    if (n) deep(n - 1, value + pad[n & 511] - (char)n);
    set_fpcr(fpcr() ^ (3ull << 22));   /* (the rounding mode changed after setjmp, as lifted code may) */
    recomp_sj_longjmp(buf, value);
}
__attribute__((noinline)) static int owner(int value, volatile int *runs) {
    volatile int mark = 0x5a5a5a5a;   /* (the caller's frame, which the longjmp comes back to) */
    const uint64_t before = fpcr();
    const int r = recomp_sj_setjmp(buf);
    CHECK(++*runs <= 2);   /* (a longjmp that returned 0 would run the owner again, and again) */
    if (r == 0) {
        deep(8, value);
        CHECK(!"deep returned");
    }
    CHECK(mark == 0x5a5a5a5a);
    CHECK(fpcr() == before);
    return r;
}
int main(void) {
    volatile int runs = 0;
    CHECK(owner(7, &runs) == 7 && runs == 2);
    runs = 0;
    CHECK(owner(0, &runs) == 1 && runs == 2);   /* (a longjmp of 0 returns 1) */
    for (int i = 1; i < 100; i++) {
        runs = 0;
        CHECK(owner(i, &runs) == i && runs == 2);
    }
    puts("PASS: recomp_sj_setjmp / recomp_sj_longjmp on ARM64: the value, the caller's frame and FPCR");
    return 0;
}
'''

CONTEXT_RUN = r'''
#ifndef __aarch64__
#define __aarch64__ 1   /* (the ARM64 side of a cut that includes no header: glibc's types are the stand-ins below) */
#endif
typedef unsigned long long DWORD64, uint64_t, greg_t;
typedef unsigned int DWORD, uint32_t;
typedef __SIZE_TYPE__ size_t;
#define memset __builtin_memset
#define memcpy __builtin_memcpy
int printf(const char *, ...);
void exit(int);
#define CHECK(x) do { if (!(x)) { printf("FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* glibc's ARM64 <sys/ucontext.h>, the members the cut reads */
typedef struct { unsigned long long fault_address, regs[31], sp, pc, pstate;
                 unsigned char __reserved[4096] __attribute__((aligned(16))); } mcontext_t;
typedef struct { unsigned long uc_flags; void *uc_link; struct { void *ss_sp; int ss_flags; size_t ss_size; } uc_stack;
                 mcontext_t uc_mcontext; } ucontext_t;
typedef __UINTPTR_TYPE__ uintptr_t;
@CONTEXT@
@CUT@
@PROF@
static ucontext_t uc;
static void record(size_t *off, uint32_t magic, uint32_t size, uint64_t esr) {
    memcpy(uc.uc_mcontext.__reserved + *off, &magic, 4), memcpy(uc.uc_mcontext.__reserved + *off + 4, &size, 4);
    if (magic == 0x45535201) memcpy(uc.uc_mcontext.__reserved + *off + 8, &esr, 8);
    *off += size;
}
static int write_with(int fpsimd_first, uint64_t esr) {
    memset(&uc.uc_mcontext.__reserved, 0, sizeof uc.uc_mcontext.__reserved);
    size_t off = 0;
    if (fpsimd_first) record(&off, 0x46508001, 528, 0);   /* (FPSIMD_MAGIC: the kernel writes it first) */
    if (esr != ~0ull) record(&off, 0x45535201, 16, esr);
    return fault_was_write(&uc);
}
int main(void) {
    for (int i = 0; i < 31; i++) uc.uc_mcontext.regs[i] = 0x100 + i;
    uc.uc_mcontext.pc = 0x7f0012345678ull, uc.uc_mcontext.sp = 0x7ffe0000ull, uc.uc_mcontext.pstate = 0x60001000ull;
    CONTEXT c;
    memset(&c, 0xee, sizeof c);
    context_from(&c, &uc);
    CHECK(c.Rip == 0x7f0012345678ull && c.Rsp == 0x7ffe0000ull && c.Rbp == 0x100 + 29);
    CHECK(c.EFlags == 0x60001000u && c.ContextFlags == CONTEXT_FULL);
    CHECK(c.Rax == 0 && c.R15 == 0 && c.Dr6 == 0);   /* (nothing left from before: no handler reads x86 registers that are not there) */
    c.Rip += 4, c.Rsp -= 16, c.Rbp = 0x4242;
    context_to(&uc, &c);
    CHECK(uc.uc_mcontext.pc == 0x7f001234567cull && uc.uc_mcontext.sp == 0x7ffe0000ull - 16 && uc.uc_mcontext.regs[29] == 0x4242);
    for (int i = 0; i < 29; i++) CHECK(uc.uc_mcontext.regs[i] == 0x100u + i);   /* (the others as they were) */
    CHECK(UC_PC(&uc) == 0x7f001234567cull);   /* (host/posix/prof.c's sampler and watch) */
    const uint64_t dabt = 0x24ull << 26, dabt_same = 0x25ull << 26, iabt = 0x20ull << 26, wnr = 1ull << 6;
    CHECK(write_with(1, dabt | wnr | 0x07) == 1);
    CHECK(write_with(0, dabt | wnr) == 1);
    CHECK(write_with(1, dabt_same | wnr) == 1);
    CHECK(write_with(1, dabt | 0x07) == 0);   /* (a read: a translation fault, level 3) */
    CHECK(write_with(1, iabt | wnr) == 0);    /* (an instruction fetch) */
    CHECK(write_with(1, ~0ull) == 0);         /* (no ESR record) */
    memset(&uc.uc_mcontext.__reserved, 0, sizeof uc.uc_mcontext.__reserved);
    size_t off = 0;
    record(&off, 0x12345678, 4, 0);           /* (a broken record: no walk past it) */
    CHECK(fault_was_write(&uc) == 0);
    printf("PASS: the ARM64 signal context as a CONTEXT (pc, sp, x29 and back), a fault's write from its ESR, prof.c's pc\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="clang")
    a = p.parse_args()
    setjmp_c = read("host/hos_setjmp.c")
    thread_c = read("host/hos_thread.c")
    windows_h = read("host/posix/windows.h")
    posix_c = read("host/posix/win32_posix.c")
    prof_c = read("host/posix/prof.c")
    # (hos_setjmp.c's assembly, every system's but Windows: the cut includes no header)
    sj = between(setjmp_c, "#ifdef _WIN32\n", "\nvoid **recomp_sj_push(", "host/hos_setjmp.c recomp_sj_setjmp")
    # (hos_thread.c's lines between the Windows SDK's define and the Emscripten block, and the present poll's spin)
    pause = between(thread_c, "#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002\n#endif\n", "#ifdef __EMSCRIPTEN__",
                    "host/hos_thread.c's includes")
    pause = pause[pause.index("#endif\n") + len("#endif\n"):]
    spin = between(thread_c, "        for (int i = 0; i < 64; i++) ", "\n", "host/hos_thread.c's present poll spin")
    yp = between(windows_h, "#define _ReadWriteBarrier()", "\n\n/* ---- locks", "host/posix/windows.h YieldProcessor")
    yp = yp[yp.index("\n") + 1:]
    context = between(windows_h, "typedef struct {\n    DWORD64 Rax", "#define CONTEXT_DEBUG_REGISTERS", "host/posix/windows.h CONTEXT")
    context += between(windows_h, "#define CONTEXT_FULL", "\n", "host/posix/windows.h CONTEXT_FULL") + "\n"
    cut = between(posix_c, "static pthread_mutex_t handlers_mu", "static void on_signal(", "host/posix/win32_posix.c context")
    cut = cut[cut.index("\n") + 1:]
    prof = between(prof_c, "#include <windows.h>\n", "#ifndef TRAP_PERF", "host/posix/prof.c's pc")
    prof = prof[len("#include <windows.h>\n"):]
    prof += "static uintptr_t sampled_pc(const ucontext_t *u) {\n" + \
        between(prof_c, "        const uintptr_t rip = ", "\n", "host/posix/prof.c on_sample") + "\n    return rip;\n}\n"
    if "UC_PC" not in prof:   # (before the port: the stand-in's UC_PC is the line itself)
        prof += "#define UC_PC(u) sampled_pc(u)\n"
    with tempfile.TemporaryDirectory(prefix="arm64-host-") as tmp:
        compile_c(a.cc, tmp, "setjmp_arm64", sj, [ARM64, "-ffreestanding", "-O2"], "host/hos_setjmp.c for ARM64")
        compile_c(a.cc, tmp, "setjmp_x86_64", sj, [X86_64, "-ffreestanding", "-O2"], "host/hos_setjmp.c for x86-64")
        spin_c = pause + "void spin(void) {\n" + spin + "\n}\n"
        compile_c(a.cc, tmp, "pause_arm64", spin_c, [ARM64, "-ffreestanding"], "host/hos_thread.c's pause for ARM64")
        compile_c(a.cc, tmp, "pause_x86_64", spin_c, [X86_64, "-ffreestanding"], "host/hos_thread.c's pause for x86-64")
        yield_c = yp + "\nvoid yield(void) { YieldProcessor(); }\n"
        compile_c(a.cc, tmp, "yield_arm64", yield_c, [ARM64, "-ffreestanding"], "host/posix/windows.h YieldProcessor for ARM64")
        compile_c(a.cc, tmp, "yield_x86_64", yield_c, [X86_64, "-ffreestanding"],
                  "host/posix/windows.h YieldProcessor for x86-64")
        compile_c(a.cc, tmp, "context", CONTEXT_RUN.replace("@CONTEXT@", context).replace("@CUT@", cut).replace("@PROF@", prof),
                  [], "host/posix/win32_posix.c's ARM64 context and prof.c's pc", run=True)
        if ON_ARM64_LINUX:
            compile_c(a.cc, tmp, "setjmp_run", sj + SETJMP_RUN, ["-O2"], "host/hos_setjmp.c run on ARM64", run=True)
        else:
            print("(the setjmp run needs an ARM64 Linux PC: assembled only)")
    print("PASS: the runtime's ARM64 pieces")


if __name__ == "__main__":
    main()
