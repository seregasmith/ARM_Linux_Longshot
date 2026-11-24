"""svcSetThreadCoreMask (host/hos_svc.c): an empty mask returns InvalidCombination and changes nothing.

The kernel refuses an empty affinity with svc::ResultInvalidCombination; only the nn::os wrapper's 0 is this game's
no-affinity. The helper is cut from the source and compiled with stubs for the thread and affinity function, then:

- a null handle returns InvalidHandle;
- an empty mask returns 0xE801 and does not call the affinity function, so the thread's mask stays as it was;
- a valid mask returns success and passes the ideal core and mask through.

python -I -B tests/test_svc_core_mask.py [--cc clang]
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "clang"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from srccut import block  # noqa: E402

FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#define R_OK 0
#define R_INVALID_COMBINATION 0xE801
typedef struct { uint64_t core_mask; int mask_set; } HosThread;
static HosThread *seen;
static int seen_ideal;
static uint64_t seen_mask;
static void hos_thread_affinity(HosThread *t, int ideal, uint64_t mask) {
    seen = t, seen_ideal = ideal, seen_mask = mask;
}
@HELPER@
static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
int main(void) {
    HosThread t = { 2, 1 };
    check(hos_svc_set_thread_core_mask(NULL, 0, 1) == 0xE401, "a null handle is InvalidHandle");
    seen = NULL;
    check(hos_svc_set_thread_core_mask(&t, 1, 0) == R_INVALID_COMBINATION, "an empty mask is InvalidCombination");
    check(seen == NULL, "an empty mask does not set an affinity");
    check(t.core_mask == 2 && t.mask_set == 1, "an empty mask leaves the thread's affinity as it was");
    check(hos_svc_set_thread_core_mask(&t, 3, 0x7) == R_OK, "a valid mask succeeds");
    check(seen == &t && seen_ideal == 3 && seen_mask == 0x7, "a valid mask passes the ideal core and mask through");
    if (!bad) puts("ok");
    return bad;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    src = (ROOT / "host/hos_svc.c").read_text(encoding="utf-8")
    fixture = FIXTURE.replace("@HELPER@", block(src, "int hos_svc_set_thread_core_mask(HosThread *t, int ideal, uint64_t mask) {"))
    compiler = shutil.which(args.cc)
    if not compiler:
        raise SystemExit("C compiler not found: " + args.cc)
    with tempfile.TemporaryDirectory(prefix="svc-core-mask-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.c").write_text(fixture, encoding="utf-8")
        exe = folder / "fixture"
        subprocess.run([compiler, "-std=c11", "-O1", "-w", str(folder / "fixture.c"), "-o", str(exe)],
                       check=True, timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        if result.returncode or "ok" not in result.stdout:
            raise SystemExit("the svcSetThreadCoreMask checks failed")


if __name__ == "__main__":
    main()
