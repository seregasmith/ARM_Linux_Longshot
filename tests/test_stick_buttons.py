"""The sticks' direction pseudo-buttons in every Npad state (host/nn_hid.c stick_buttons).

Real hid sets HidNpadButton_StickLLeft, Up, Right, Down (bits 16-19) and the right stick's (20-23) from the axes, y up,
past half their travel (yuzu's TransformToStick, threshold 0.5); libnx's hid.h names them HidNpadButton_StickLLeft
(bit 16) .. HidNpadButton_StickRDown (bit 23). Main set none of them, so code that reads them (a mod's menu combo) never
saw a stick direction. The function is compiled from host/nn_hid.c and checked:

- each direction of each stick sets its own bit only, and only past half (16383 none, 16384 set);
- the right stick down is bit 23, up is 21;
- a diagonal sets both of its bits; a centred or slightly tilted stick none; no bit outside 16-23;
- npad_states ORs them into every state it writes.

python -I -B tests/test_stick_buttons.py [--cc clang]
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
    fn = block(hid, "static uint64_t stick_buttons(") + "\n"
    if "buttons |= stick_buttons(stick);" not in block(hid, "static void npad_states(Ctx *c)"):
        raise SystemExit("test_stick_buttons: npad_states does not add the sticks' direction bits to its buttons")
    fixture = r'''#include <stdint.h>
#include <stdio.h>
''' + fn + r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
static uint64_t sb(int32_t lx, int32_t ly, int32_t rx, int32_t ry) { const int32_t s[4] = {lx, ly, rx, ry}; return stick_buttons(s); }
int main(void) {
    CHECK(sb(0, 0, 0, 0) == 0);
    CHECK(sb(16383, -16383, -16383, 16383) == 0);   /* (half: not yet) */
    CHECK(sb(-16384, 0, 0, 0) == 1u << 16);   /* StickLLeft */
    CHECK(sb(0, 16384, 0, 0) == 1u << 17);    /* StickLUp (y up) */
    CHECK(sb(32767, 0, 0, 0) == 1u << 18);    /* StickLRight */
    CHECK(sb(0, -32767, 0, 0) == 1u << 19);   /* StickLDown */
    CHECK(sb(0, 0, -32767, 0) == 1u << 20);   /* StickRLeft */
    CHECK(sb(0, 0, 0, 32767) == 1u << 21);    /* StickRUp */
    CHECK(sb(0, 0, 20000, 0) == 1u << 22);    /* StickRRight */
    CHECK(sb(0, 0, 0, -32767) == 1u << 23);   /* StickRDown */
    CHECK(sb(0, 0, 0, -32768) == 1u << 23);
    CHECK(sb(-30000, 30000, 30000, -30000) == ((1u << 16) | (1u << 17) | (1u << 22) | (1u << 23)));
    CHECK((sb(-32768, -32768, -32768, -32768) & ~(uint64_t)0xFF0000) == 0);
    printf("PASS: the sticks' direction bits 16-23 past half their travel, right stick down bit 23\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="stick-buttons-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture.exe"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        print(r.stdout, end="")
        if r.returncode:
            print(r.stderr, end="")
            raise SystemExit("test_stick_buttons: failed")


if __name__ == "__main__":
    main()
