"""Which GPU pass made a screen flash, from the renderer's two flash captures.

    python -I scripts/flash.py framelog <file> [out.png]     the flashes in a frame log (RX_VK_FRAMELOG)
    python -I scripts/flash.py framelog <file> --inspect     validate its headers only (no NumPy)
    python -I scripts/flash.py probe work/runs/<run>/probe_<frame>.txt [--ratio 4] [--lines 25]
                                                             the passes of one flash frame (RX_VK_PROBE)

framelog finds the frames that flash; probe, on a capture of the frame before, the flash and the frame after, names
the pass that went wrong.

framelog: the capture samples (gpu/vk/vk_replay.cpp), 320x180, timestamped when the commands are recorded, before GPU
submission. A frame is a flash where a part of the picture goes brighter (or darker) than BOTH its neighbours by more
than they differ from each other, with the brightness of that part back within reach the next frame. Blocks of 20x20,
so that an edge moving past counts little; a block must jump in brightness, not only in detail. Prints the events by
size, with whether the frame was the game's or a made one, and writes the worst as rows of (before, frame, after).
Only consecutive sequence numbers form a triplet: dropped records cannot establish a one-frame flash. Inspect a
completed, stable capture; --inspect validates its headers without NumPy. Sequence numbers may wrap at uint32;
nonadvancing headers, partial records and captures shorter than three frames are rejected.

probe: reads a probe_<frame>.txt (vk_draw.cpp) and aligns the frames before, at and after the flash by (kind,
program, target, nth use), then prints the entries of the flash frame in order whose colour is far from both
neighbours' (more than --ratio apart), decoded by format. The comparison skips decoded NaNs. The first reported entry
is the pass that went wrong.
"""
import argparse
import collections
import math
import mmap
import struct
from pathlib import Path
from typing import NamedTuple

# --- framelog -----------------------------------------------------------------------------------------------------

W, H = 320, 180
SIZE = 16 + W * H * 4
MASK = (1 << 32) - 1


class CaptureInfo(NamedTuple):
    timestamps: list[int]
    sequences: list[int]
    kinds: list[int]
    missing: int
    centres: list[int]


def triplet_centres(sequences):
    """Only stored consecutive frames can establish a one-frame brightness jump."""
    return [i for i in range(1, len(sequences) - 1)
            if ((sequences[i] - sequences[i - 1]) & MASK) == 1
            and ((sequences[i + 1] - sequences[i]) & MASK) == 1]


def capture_info(path):
    size = Path(path).stat().st_size
    count, tail = divmod(size, SIZE)
    if tail:
        raise ValueError("truncated record %d: %d bytes, expected %d" % (count + 1, tail, SIZE))
    if count < 3:
        raise ValueError("need at least 3 frames for flash analysis; got %d" % count)
    timestamps, sequences, kinds = [], [], []
    missing = 0
    with open(path, "rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as raw:
        for i in range(count):
            t, s, k = struct.unpack_from("<QII", raw, i * SIZE)
            if k not in (0, 1):
                raise ValueError("invalid frame kind %d at record %d (expected 0 or 1)" % (k, i + 1))
            if timestamps and t <= timestamps[-1]:
                raise ValueError("timestamp does not advance at record %d" % (i + 1))
            if sequences:
                delta = (s - sequences[-1]) & MASK
                if not delta or delta >= (1 << 31):
                    raise ValueError("sequence does not advance at record %d" % (i + 1))
                missing += delta - 1
            timestamps.append(t), sequences.append(s), kinds.append(k)
    return CaptureInfo(timestamps, sequences, kinds, missing, triplet_centres(sequences))


def blocks(a):
    """luma means of 20x20 blocks"""
    l = a[..., 0] * 0.3 + a[..., 1] * 0.6 + a[..., 2] * 0.1
    return l[:H // 20 * 20, :].reshape(H // 20, 20, W // 20, 20).mean(axis=(1, 3))


def framelog(args, parser):
    path, out = args.path, args.out
    try:
        info = capture_info(path)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    n = len(info.sequences)
    print(n, "frames")
    eligible = set(info.centres)
    print("flash triplets: %d eligible, %d skipped across sequence gaps; missing %d frames" % (
        len(eligible), n - 2 - len(eligible), info.missing))
    if args.inspect:
        return 0
    try:
        import numpy as np
    except ImportError:
        parser.error("image analysis requires NumPy; --inspect validates capture metadata without it")
    raw = np.memmap(path, dtype=np.uint8, mode="r")
    ts = np.array(info.timestamps, dtype=np.uint64)
    kinds = np.array(info.kinds)
    d = np.diff(ts) / 1e6
    print("capture samples apart: median %.1f ms, 99%% %.1f, worst %.1f; made %d of %d; missing %d" % (
        np.median(d), np.percentile(d, 99), d.max(), kinds.sum(), n, info.missing))
    frame = lambda i: np.asarray(raw[i * SIZE + 16:(i + 1) * SIZE]).reshape(H, W, 4)[..., :3].astype(np.float32)
    prev = blocks(frame(0))
    cur = blocks(frame(1))
    events = []
    for i in range(1, n - 1):
        nxt = blocks(frame(i + 1))
        if i in eligible:
            up = np.minimum(cur - prev, cur - nxt)
            down = np.minimum(prev - cur, nxt - cur)
            span = np.abs(prev - nxt)
            for sign, jump in ((1, up), (-1, down)):
                hit = (jump > 12) & (jump > span * 1.5)
                c = int(hit.sum())
                if c:
                    events.append((float(jump[hit].sum()), i, sign, c, float(jump.max())))
        prev, cur = cur, nxt
    events.sort(reverse=True)
    tot = {0: 0, 1: 0}
    for e in events:
        tot[int(kinds[e[1]])] += 1
    print("events: %d on the game's frames, %d on made frames" % (tot[0], tot[1]))
    for size, i, sign, c, peak in events[:24]:
        print("frame %d (%s) %s %d blocks, peak %.0f, at %.2f s" % (i, "made" if kinds[i] else "game", "brighter" if sign > 0 else "darker",
                                                                     c, peak, (ts[i] - ts[0]) / 1e9))
    if out and events:
        from PIL import Image
        rows = events[:8]
        im = Image.new("RGB", (W * 3, H * len(rows)))
        for r, e in enumerate(rows):
            for k in range(3):
                im.paste(Image.fromarray(frame(e[1] - 1 + k).astype(np.uint8)), (k * W, r * H))
        im.save(out)
    return 0


# --- probe --------------------------------------------------------------------------------------------------------

def half(h):
    return struct.unpack("<e", struct.pack("<H", h))[0]


def uf(v, mbits):
    e, m = v >> mbits, v & ((1 << mbits) - 1)
    return (m / (1 << mbits)) * 2.0 ** -14 if e == 0 else 1e30 if e == 31 else (1 + m / (1 << mbits)) * 2.0 ** (e - 15)


def decode(fmt, w):
    if fmt == 122:   # B10G11R11
        return [uf(w[0] & 2047, 6), uf((w[0] >> 11) & 2047, 6), uf(w[0] >> 22, 5)]
    if fmt == 123:   # E5B9G9R9
        s = 2.0 ** ((w[0] >> 27) - 24)
        return [(w[0] & 511) * s, ((w[0] >> 9) & 511) * s, ((w[0] >> 18) & 511) * s]
    if fmt == 97:    # R16G16B16A16_SFLOAT
        return [half(w[0] & 0xffff), half(w[0] >> 16), half(w[1] & 0xffff)]
    if fmt == 83:    # R16G16_SFLOAT
        return [half(w[0] & 0xffff), half(w[0] >> 16)]
    if fmt == 76:    # R16_SFLOAT
        return [half(w[0] & 0xffff)]
    if fmt == 100:   # R32_SFLOAT
        return list(struct.unpack("<f", struct.pack("<I", w[0])))
    if fmt == 109:   # R32G32B32A32_SFLOAT
        return list(struct.unpack("<4f", struct.pack("<4I", *w)))
    return [(w[0] >> k) & 255 for k in (0, 8, 16, 24)]


def key_rows(rows):
    """{(kind, program, target, nth use): row}"""
    seen, out = collections.Counter(), {}
    for r in rows:
        k = (r[2], r[3], r[4])
        out[k + (seen[k],)] = r
        seen[k] += 1
    return out


def probe_value(r):
    return decode(int(r[6]), [int(x, 16) for x in r[7:11]])


def far(x, y, ratio):
    for p, q in zip(x, y):
        if math.isnan(p) or math.isnan(q):
            continue
        hi, lo = max(abs(p), abs(q)), min(abs(p), abs(q))
        if hi > 1e-3 and hi > lo * ratio:
            return True
    return False


def probe(args, parser):
    frames = collections.OrderedDict()
    with open(args.path, encoding="utf-8") as capture:
        for line_number, line in enumerate(capture, 1):
            r = line.split()
            if not r:
                continue
            if len(r) < 11:
                raise SystemExit(f"Probe capture line {line_number} is incomplete. Capture a new probe_*.txt file.")
            frames.setdefault(r[0], []).append(r)
    if len(frames) != 3:
        raise SystemExit(f"Probe capture expected 3 frames, found {len(frames)}. "
                         "Capture a new probe_*.txt file with the frame before, the flash, and the frame after.")
    a, b, c = [key_rows(v) for v in frames.values()]
    print("frames", list(frames), {k: len(v) for k, v in frames.items()}, "unmatched at the flash:",
          sum(1 for k in b if k not in a or k not in c))
    told = 0
    for k, r in b.items():
        if k not in a or k not in c:
            continue
        va, vb, vc = probe_value(a[k]), probe_value(r), probe_value(c[k])
        if far(va, vb, args.ratio) and far(vc, vb, args.ratio):
            print("%4s %s prog %s %s %s f%s: %s | %s | %s" % (r[1], r[2], r[3][:8], r[4], r[5], r[6], ["%.4g" % x for x in va],
                                                             ["%.4g" % x for x in vb], ["%.4g" % x for x in vc]))
            told += 1
            if told >= args.lines:
                break
    print("%d entries far from both neighbours" % told)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("framelog", help="the flashes in a frame log (RX_VK_FRAMELOG)")
    p.add_argument("path", type=Path)
    p.add_argument("out", nargs="?", help="write the worst events to this PNG")
    p.add_argument("--inspect", action="store_true", help="validate metadata and report eligible triplets without NumPy")
    p.set_defaults(run=framelog, parser=p)
    p = sub.add_parser("probe", help="the pass that made a flash, from a probe_<frame>.txt (RX_VK_PROBE)")
    p.add_argument("path", type=Path)
    p.add_argument("--ratio", type=float, default=4.0, help="how far apart a colour must be from both neighbours'")
    p.add_argument("--lines", type=int, default=25, help="the most entries to print")
    p.set_defaults(run=probe, parser=p)
    args = parser.parse_args(argv)
    return args.run(args, args.parser)


if __name__ == "__main__":
    raise SystemExit(main())
