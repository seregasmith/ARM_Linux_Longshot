"""The controller speaker reads the player's sound archives at the start (host/audio_source.c), and a wave buffer in
the game's memory gets its asset's class.

Compiles host/audio_source.c as it is, with third_party/zstd's decoder, over a RomFS folder made up here (no game
file): Pack/ZsDic.pack.zs holding a dictionary in a SARC, and Sound/Resource/*.bars.zs as Zstandard frames of raw
blocks, written here with the standard library (the dictionary still loaded and handed to the decoder, as the game's). The "game's memory" is an array with one archive in it.

python -I -B tests/test_audio_source_load.py [--cc gcc]     (standard library only: the frames are written here)
"""
import argparse
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = []
ROOT = Path(__file__).resolve().parents[1]
ZSTD = ROOT / "third_party/zstd/lib"
ZSTD_SOURCES = ["common/entropy_common.c", "common/error_private.c", "common/fse_decompress.c", "common/xxhash.c", "common/zstd_common.c",
                "decompress/huf_decompress.c", "decompress/zstd_ddict.c", "decompress/zstd_decompress.c", "decompress/zstd_decompress_block.c"]

HOS_H = r'''#pragma once
#include <stdint.h>
#include <stddef.h>
extern unsigned char guest[0x40000];
#define GUEST 0x1000000000ull
#define GP(a) ((void *)(guest + ((a) - GUEST)))
void hos_log(const char *fmt, ...);
int hos_mem_region(uint64_t addr, uint64_t size);
'''

MAIN = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hos.h"
#include "audio_source.h"
unsigned char guest[0x40000];
const char *hos_romfs_dir;
static char logged[4096];
void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const size_t at = strlen(logged);
    vsnprintf(logged + at, sizeof logged - at - 2, fmt, ap);
    va_end(ap);
    strcat(logged, "\n");
}
static uint64_t mapped_from = GUEST;
int hos_mem_region(uint64_t addr, uint64_t size) { return addr >= mapped_from && addr + size <= GUEST + sizeof guest; }
void hos_audio_source_load(void);
int hos_audio_source_of(uint64_t data, uint32_t *crc);
const char *hos_audio_source_name(int cls);

int main(int argc, char **argv) {
    assert(argc == 4);
    char romfs[1024];
    if (!strcmp(argv[3], "full")) {
        snprintf(romfs, sizeof romfs, "%s/romfs_\xe9\x9b\xaa", argv[1]);
        hos_romfs_dir = romfs;
    } else hos_romfs_dir = argv[1];
    FILE *f = fopen(argv[2], "rb");   /* the archive as the game holds it, put at 0x10040 of its memory */
    assert(f);
    const size_t n = fread(guest + 0x10040, 1, sizeof guest - 0x10040, f);
    fclose(f);
    assert(n > 0x100);
    const uint64_t bars = GUEST + 0x10040;
    uint32_t crc = 0;
    assert(hos_audio_source_of(bars + 0x200 + 0x80, &crc) == -1);   /* before the archives are read: nothing is classed */
    hos_audio_source_load();
    fputs(logged, stdout);
    if (!strcmp(argv[3], "none")) {   /* a folder without the archives: no classes, and it says so once */
        assert(strstr(logged, "audio: 0 sounds classed from 0 archives"));
        assert(hos_audio_source_of(bars + 0x200 + 0x80, &crc) == AUDIO_SOURCE_NONE);
        puts("audio source load (no archives): ok");
        return 0;
    }
    assert(strstr(logged, "audio: 6 sounds classed from 4 archives"));
    assert(strstr(logged, "Navi's voice 1") && strstr(logged, "Navi's sounds 1") && strstr(logged, "targeting 1") &&
           strstr(logged, "ocarina 2") && strstr(logged, "pick-ups 0") && strstr(logged, "low health 1"));
    assert(strstr(logged, "Item_Broken.bars.zs is not read as a sound archive"));
    assert(!strstr(logged, "Player_Adult"));                          /* an archive nothing is classed from is not opened */
    /* Npc_Navi in the game's memory: SE_Navi_LockOn at 0x200, SE_Navi_FlyApproach (2 channels) at 0x2000, a cutscene's at 0x6000 */
    assert(hos_audio_source_of(bars + 0x200 + 0x80, &crc) == AUDIO_SOURCE_TARGET && crc == 0xb2db631e);
    assert(hos_audio_source_of(bars + 0x2000 + 0xc0, &crc) == AUDIO_SOURCE_NAVI_SOUND && crc == 0x7da57872);
    assert(hos_audio_source_of(bars + 0x2000 + 0xc0 + 0x1c40, &crc) == AUDIO_SOURCE_NAVI_SOUND);   /* its second channel */
    assert(hos_audio_source_of(bars + 0x6000 + 0x80, &crc) == AUDIO_SOURCE_NONE && crc == 0xa4a6a6ef);   /* found, not classed */
    assert(hos_audio_source_of(bars + 0x200 + 0x80 + 0x40, &crc) == -1);   /* the middle of a sound: no asset's start */
    assert(hos_audio_source_of(GUEST + 0x30000, &crc) == -1);              /* a streamed sound's buffer */
    assert(hos_audio_source_of(0, &crc) == -1);
    assert(hos_audio_source_of(GUEST + 0x40, &crc) == -1);                 /* too near the start of the mapped memory to have a header */
    mapped_from = bars + 0x200;                                            /* memory mapped from the header on: still found */
    assert(hos_audio_source_of(bars + 0x200 + 0x80, &crc) == AUDIO_SOURCE_TARGET);
    mapped_from = bars + 0x200 + 0x40;                                     /* the header itself not mapped: not read */
    assert(hos_audio_source_of(bars + 0x200 + 0x80, &crc) == -1);
    assert(!strcmp(hos_audio_source_name(AUDIO_SOURCE_TARGET), "targeting") && !strcmp(hos_audio_source_name(99), "?"));
    logged[0] = 0;
    hos_audio_source_load();                                               /* read once */
    assert(!logged[0]);
    puts("audio source load: ok");
    return 0;
}
'''


def bwav(crc, channels, each=0x1c40):
    first = (0x10 + channels * 0x4c + 0x3f) & ~0x3f
    head = b"BWAV" + struct.pack("<HHIHH", 0xfeff, 1, crc, 1, channels)
    for ch in range(channels):
        at = first + ch * each
        head += struct.pack("<HHIII", 1, 0, 48000, 14336, 14336) + bytes(32) + struct.pack("<II", at, at) + bytes(0x4c - 0x38)
    return head.ljust(first, b"\0") + bytes((i * 7) & 0xff for i in range(channels * each))


def amta(name):
    body = bytes(0x28) + name.encode() + b"\0"
    body = body.ljust((len(body) + 3) & ~3, b"\0")
    return b"AMTA" + struct.pack("<HHI", 0xfeff, 5, 12 + len(body)) + body


def bars(assets, wave_at=None):
    """assets: (name, crc, channels or 0 for no wave); wave_at: fixed offsets for the waves"""
    count = len(assets)
    out = bytearray(b"BARS" + struct.pack("<IHHI", 0, 0xfeff, 0x102, count) + bytes(4 * count) + bytes(8 * count))
    pairs = []
    metas = []
    for name, _, _ in assets:
        metas.append(len(out))
        out += amta(name)
    for i, (_, crc, channels) in enumerate(assets):
        if not channels:
            pairs.append((metas[i], 0xffffffff))
            continue
        at = wave_at[i] if wave_at else (len(out) + 0x3f) & ~0x3f
        assert at >= len(out)
        out += bytes(at - len(out))
        pairs.append((metas[i], at))
        out += bwav(crc, channels)
    for i, (a, w) in enumerate(pairs):
        struct.pack_into("<II", out, 0x10 + 4 * count + 8 * i, a, w)
    struct.pack_into("<I", out, 4, len(out))
    return bytes(out)


def sarc(files):
    names = b""
    nodes = b""
    data = b""
    for name, content in files:
        nodes += struct.pack("<IIII", 0, 0x01000000 | (len(names) // 4), len(data), len(data) + len(content))
        names += (name.encode() + b"\0").ljust((len(name) + 4) & ~3, b"\0")
        data += content.ljust((len(content) + 3) & ~3, b"\0")
    head = 0x14 + 0x0c + len(nodes) + 8 + len(names)
    return (b"SARC" + struct.pack("<HHIII", 0x14, 0xfeff, head + len(data), head, 0x100) + b"SFAT" + struct.pack("<HHI", 0x0c, len(files), 0x65)
            + nodes + b"SFNT" + struct.pack("<HH", 8, 0) + names + data)


def zstd_frame(content):
    """content as a Zstandard frame (RFC 8878) of raw blocks, written here with the standard library only: a single
    segment, its size in the header, no dictionary id and no checksum. Any decoder reads it, with a dictionary or without."""
    out = struct.pack("<IB", 0xFD2FB528, (2 << 6) | (1 << 5)) + struct.pack("<I", len(content))
    step = 128 * 1024
    for at in range(0, max(len(content), 1), step):
        part = content[at:at + step]
        last = at + step >= len(content)
        out += struct.pack("<I", int(last) | (0 << 1) | (len(part) << 3))[:3] + part
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    navi = bars([("SE_Navi_LockOn", 0xb2db631e, 1), ("SE_Navi_FlyApproach", 0x7da57872, 2), ("SE_Dm_Kokiri_0060_Navi_02", 0xa4a6a6ef, 1),
                 ("SE_Navi_Emote00", 0x62e5b743, 0)], wave_at=[0x200, 0x2000, 0x6000, None])
    archives = {
        "Npc_Navi": navi,
        "NV_USen_Custom_Navi": bars([("NV_USen_Custom_Navi_Attention01", 0x0c6a85b0, 1)]),
        "Ocarina": bars([("Ocarina_G", 0xa96bbafa, 1), ("PlayNote", 0x0211f355, 3)]),
        "Player_Adult": bars([("SE_Player_Child_LeafTrack02", 0xa03bfd8c, 1)]),
        "Player": bars([("SE_Player_Child_LeafTrack03", 0xa03bfd8d, 1), ("SE_Sys_HeartAlert", 0x5eed1e55, 1)]),
    }
    samples = [navi[i:i + 0x400] for i in range(0, len(navi), 0x400)] * 4
    dictionary = b"".join(samples)[:0x4000]   # (a raw-content dictionary: its bytes as they are, as the game's)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        romfs = tmp / "romfs_雪"
        (romfs / "Pack").mkdir(parents=True)
        (romfs / "Sound/Resource").mkdir(parents=True)
        (romfs / "Pack/ZsDic.pack.zs").write_bytes(zstd_frame(sarc([("pack.zsdic", b"other"), ("zs.zsdic", dictionary)])))
        for name, content in archives.items():
            (romfs / "Sound/Resource" / (name + ".bars.zs")).write_bytes(zstd_frame(content))
        (romfs / "Sound/Resource/Item_Broken.bars.zs").write_bytes(zstd_frame(b"not an archive at all"))
        (romfs / "Sound/Resource/Item_Notes.txt").write_bytes(b"x")
        empty = tmp / "empty"
        empty.mkdir()
        (tmp / "navi.bars").write_bytes(navi)
        # (the source beside a hos.h of the test's: the real one brings the whole runtime)
        inc = tmp / "host"
        inc.mkdir()
        (inc / "hos.h").write_text(HOS_H, encoding="utf-8")
        for name in ("audio_source.c", "audio_source.h"):
            shutil.copy(ROOT / "host" / name, inc / name)
        (tmp / "gpu/vk/compat").mkdir(parents=True)
        shutil.copy(ROOT / "gpu/vk/compat/longshot_env.h", tmp / "gpu/vk/compat/longshot_env.h")
        (tmp / "main.c").write_text(MAIN, encoding="utf-8")
        exe = tmp / ("audio_source_load.exe" if os.name == "nt" else "audio_source_load")
        command = [args.cc, "-std=gnu11", "-O1", "-w", "-I", str(inc), "-I", str(ZSTD), "-DZSTD_DISABLE_ASM"]
        sources = [str(tmp / "main.c"), str(inc / "audio_source.c")] + [str(ZSTD / s) for s in ZSTD_SOURCES]
        if os.name != "nt":
            command += ["-I", str(ROOT / "host/posix"), "-D_GNU_SOURCE"]
            sources.append(str(ROOT / "host/posix/win32_posix.c"))
        subprocess.run(command + sources + ["-o", str(exe)] + ([] if os.name == "nt" else ["-lpthread", "-lm", "-ldl"]), check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_AUDIO_SOURCE")}
        subprocess.run([str(exe), str(tmp), str(tmp / "navi.bars"), "full"], check=True, env=env)
        subprocess.run([str(exe), str(empty), str(tmp / "navi.bars"), "none"], check=True, env=env)


if __name__ == "__main__":
    main()
