"""No host code writes guest memory in bulk without marking it written, unless listed here with the reason (host/*.c).

The renderer learns that the CPU wrote a page of guest memory from the page's mark alone: the lifted code sets it
after each store (A64_MARK), host code with HOS_WRITTEN. A host write with no mark is invisible to it: its mirror, its
captures and its write-back shadow keep the bytes from before, and a write-back can put them back. Three such writes
zeroed pages the renderer had imported, at a free (hos_mem.c discard_pages and FreeMemoryPages, hos_svc.c
UnmapPhysicalMemory).

The rule this check keeps: host code fills or copies into guest memory with hos_guest_fill or hos_guest_copy
(host/hos.h: the write, then the mark), or follows its own memset, memcpy or memmove with HOS_WRITTEN in the same
function. Any other is on ALLOWED below with why its target is never memory the renderer reads.

What it finds, by the text: a memset, memcpy or memmove whose destination starts with a guest pointer (GP, A64_PTR,
A64_MEM +, hos_guest_base() +, hos_guest_range, and nvn_native.c's P, PN, gw and g2h), or with a local the same
function set from one of those (not a pointer to const). It is marked when HOS_WRITTEN, hos_mark_written or
rx_backend.c's host_mark_written follows before the function's closing brace. What it cannot see: a write through a
pointer handed to another function, a store through G64 or G32 or an index, a read by the system into guest memory
(ReadFile, a decoder), a pointer reached through another macro. Those were read one by one when this list was made.

A failure names the file and the line. A new write the renderer may read: use hos_guest_fill or hos_guest_copy. One it
never reads: add it to ALLOWED with the reason. An entry that matches nothing any more fails too, so the list stays
the size of what is there.

python -I -B tests/test_host_guest_writes.py
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

ACCESSOR = r"(?:GP|A64_PTR|P|PN|gw|g2h|hos_guest_range)"
CASTS = r"(?:\([^()]*\)\s*)*"
CALL = re.compile(r"\b(memset|memcpy|memmove)\s*\(")
# (the destination, the text after the call's parenthesis: a guest pointer made in place)
DIRECT = re.compile(r"\s*" + CASTS + r"(?:" + ACCESSOR + r"\s*\(|(?:A64_MEM|hos_guest_base\s*\(\s*\))\s*\+)")
# (or a plain name, which may be a local the function set from one)
LOCAL = re.compile(r"\s*" + CASTS + r"([A-Za-z_]\w*)\b(?!\s*\()")
ASSIGN = re.compile(r"(?<![\w.>])([A-Za-z_]\w*)\s*=\s*(?:[^;?]*\?\s*)?" + CASTS + ACCESSOR + r"\s*\(")
MARK = re.compile(r"\b(?:HOS_WRITTEN|hos_mark_written|host_mark_written)\s*\(")

# file -> [(texts, reason)]: an unmarked write on a line holding one of the texts is allowed, for that reason.
STACK = "the calling guest thread's stack, which the GPU never reads (the lifted code does not mark its stack stores either)"
OUT = "a small out-parameter of fixed size, in an object or on the stack of the game's: never pool memory"
NVN_OBJECT = "an NVN object's or builder's own state, in the game's object: CPU-only, the renderer is told through the layer's calls"
AUDIO = "audio data and names for the audio renderer: nothing the GPU reads"
ALLOWED = {
    "host/hos_main.c": [
        (["A64_PTR(event", "A64_PTR(frame + 0x10)"], STACK),
        (["A64_PTR(name_buf)", "A64_PTR(buf), event"], "an event's name, into the argument object the game made for it"),
        (["A64_PTR(strings"], "the cheats' event strings, in the system region (hos_sys_alloc): the runtime's own objects"),
    ],
    "host/hos_svc.c": [
        (["memset(GP(out), 0, 0x28)"], OUT),
        (["memset(tls, 0, 0x100)"], "the thread's IPC buffer, in its TLS block (the system region)"),
    ],
    "host/mouse_native.c": [(["memcpy(GP(a), &f, 4)"], "one float of the game's camera object")],
    "host/nn_applet.c": [
        (["memcpy(GP(ARG(0)), &one, 4)", "memcpy(GP(ARG(1)), &one, 4)", "memset(GP(ARG(0)), 0, 33)", '"Player"', '"lp1"',
          "memcpy(GP(out), ct, 8)", "memset(GP(info), 0, 0x18)", '"UTC"', "memset(GP(ARG(0)), 0, 0xD0)",
          "memset(GP(ARG(0)), 0, 16)", '"1.8.0"'], OUT),
    ],
    "host/nn_audio.c": [
        (["memset(GP(ARG(0)), 0, 256)", '"MainAudioOut"', "memset(GP(ARG(3)), 0, (size_t)bytes)",
          "memset(GP(ARG(4)), 0, (size_t)bytes)", "memcpy(GP(ARG(1)), a->in", "memcpy(GP(ARG(2)), a->out"], AUDIO),
    ],
    "host/nn_audio_in.c": [
        (["memset(GP(ARG(0)), 0, 256)", '"MainAudioIn"', "memset(GP(c->x[8]), 0, 16)"], AUDIO),
    ],
    "host/nn_fs.c": [
        (["memset(GP(ARG(1) + 0x20 * n), 0, 0x20)", "memset(GP(e), 0, 0x310)"],
         "directory and cache-storage entries, fixed-size records into the game's own array"),
    ],
    "host/nn_gs.c": [(["memset(GP(ARG(0)), 0, 0x40)"], OUT)],
    "host/nn_hid.c": [
        (["memset(GP(e), 0, STATE_SIZE)", "memcpy(GP(e + 16), stick, 16)", "memset(GP(ARG(0)), 0, 8)", "memset(GP(e), 0, 0x60)",
          "memcpy(GP(e + 0x10), v, sizeof v)", "memcpy(GP(v), idle, sizeof idle)", "memset(GP(ARG(0)), 0, 16 + 16 * 0x28)"],
         "input states, fixed-size records into the game's own array: nothing the GPU reads"),
    ],
    "host/nn_video.c": [(["memcpy(GP(out), &p, sizeof p)"], OUT)],
    "host/nvdrv.c": [
        (["memset(GP(p), 0, 16)", "memcpy(GP(p), v, sizeof v)", "memcpy(GP(out2 && out2size >= 0xA0 ? out2 : p + 16), c, 0xA0)"],
         "an ioctl's answer, into the caller's parameter block"),
    ],
    "host/nvn_native.c": [
        (["memset(P(0), 0, sizeof(nvn_", "memset(o, 0, sizeof *o)", "memset(b, 0, sizeof *b)", "memcpy(o->crop, v, sizeof v)"],
         NVN_OBJECT),
        (["memcpy(PN(1, 16), CSP(P(0))->border, 16)", "memset(P(1), 0, sizeof(NVNqueueErrorInfo))", "memset(out, 0, 32)"], OUT),
        (["memcpy(gw(at, sizeof mark), mark, sizeof mark)"],
         "16 bytes in a command buffer's control memory, which NVN keeps for the CPU: the GPU reads command memory, not control memory"),
    ],
    "host/oot_menu.c": [(["memcpy(GP(buf), key, n + 1)"], "an item's key, into the menu screen's own string buffer")],
    "host/prompts.c": [
        (["memcpy(q, b + 8, (size_t)n - 8)"],
         "the font file's bytes, which the game's own (lifted, marked) code rasterizes: the GPU reads the glyphs it draws, not the font"),
    ],
}


def scan(text):
    """The bulk writes of guest memory in a C source's text: (line number, line, marked)"""
    lines = text.split("\n")
    found = []
    start = 0
    # (a function ends at a closing brace in the first column, as every one in host/ is written)
    for end in [i for i, line in enumerate(lines) if line.startswith("}")] + [len(lines) - 1]:
        guest_locals = {}   # (name -> the line that set it from a guest pointer)
        for i in range(start, end + 1):
            line = lines[i]
            for m in ASSIGN.finditer(line):
                name = m.group(1)
                if re.search(r"\bconst\b[^;,=()]*\*\s*" + re.escape(name) + r"\s*=", line):
                    continue   # (a pointer to const: read through, not written)
                guest_locals.setdefault(name, i)
            hit = False
            for m in CALL.finditer(line):
                dest = line[m.end():]
                local = LOCAL.match(dest)
                if DIRECT.match(dest) or (local and guest_locals.get(local.group(1), i + 1) <= i):
                    hit = True
            if hit:
                found.append((i + 1, line.strip(), any(MARK.search(lines[j]) for j in range(i, end + 1))))
        start = end + 1
    return found


SELF_TEST = '''
static void unmarked(uint64_t a) {
    memset(GP(a), 0, 64);
}
static void marked(uint64_t a) {
    memcpy(GP(a), src, 64);
    HOS_WRITTEN(GP(a), 64);
}
static void through_a_local(uint64_t a, uint64_t n) {
    uint8_t *to = a ? (uint8_t *)hos_guest_range(a, n) : NULL;
    memcpy(to + 8, src, n - 8);
}
static void reads_only(uint64_t a) {
    const uint8_t *from = (const uint8_t *)GP(a);
    uint8_t copy[64];
    memcpy(copy, from, sizeof copy);
    memcpy(host_buffer, GP(a), 64);
}
static void by_the_helper(uint64_t a) {
    hos_guest_fill(a, 0, 64);
}
'''


def main():
    # (the scan itself, on made-up source: it flags an unmarked write, direct or through a local, and nothing else)
    got = [(n, marked) for n, _, marked in scan(SELF_TEST)]
    if got != [(3, False), (6, True), (11, False)]:
        raise SystemExit("FAIL: the scan of the made-up source found %r, not the three writes in it" % (got,))

    # (one write the scan cannot see, the renderer's copy of a texture's texels into a buffer of the game's choosing:
    # nvnTextureReadTexels. The buffer may be pool memory the game uploads from, so it is marked, after the copy)
    texels = block((ROOT / "host" / "nvn_native.c").read_text(encoding="utf-8"), "static void texels(Ctx *c, int write, int strided) {")
    read, mark = texels.find("rx_nvn_read_texels(t, level, layer0, r, data, row, img);"), texels.find("HOS_WRITTEN(data, (size_t)extent);")
    if read < 0 or mark < read:
        raise SystemExit("FAIL: host/nvn_native.c texels(): the texels read into the game's buffer are not marked written after "
                         "the copy (HOS_WRITTEN(data, (size_t)extent) after rx_nvn_read_texels)")

    failures = []
    used = set()
    for path in sorted((ROOT / "host").glob("*.c")):
        rel = "host/" + path.name
        allowed = ALLOWED.get(rel, [])
        for number, line, marked in scan(path.read_text(encoding="utf-8")):
            if marked:
                continue
            hits = [(rel, text) for texts, _ in allowed for text in texts if text in line]
            if hits:
                used.update(hits)
            else:
                failures.append("%s:%d: guest memory written with no mark: %s\n    (hos_guest_fill or hos_guest_copy from host/hos.h, "
                                "or HOS_WRITTEN after it; or, if the renderer never reads the target, an ALLOWED entry in "
                                "tests/test_host_guest_writes.py with the reason)" % (rel, number, line))
    for rel, entries in sorted(ALLOWED.items()):
        if not (ROOT / rel).is_file():
            failures.append("tests/test_host_guest_writes.py: ALLOWED names %s, which is not there" % rel)
            continue
        for texts, _ in entries:
            for text in texts:
                if (rel, text) not in used:
                    failures.append("tests/test_host_guest_writes.py: the ALLOWED entry %r of %s matches no unmarked write any more: "
                                    "remove it" % (text, rel))
    if failures:
        raise SystemExit("FAIL:\n" + "\n".join(failures))
    print("PASS: every bulk write of guest memory in host/*.c is marked written or listed with its reason (%d listed)" % len(used))


if __name__ == "__main__":
    main()
