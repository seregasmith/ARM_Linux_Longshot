"""The controller speaker's sound classes (host/audio_source.h): which asset a wave buffer plays, and its class.

Compiles host/audio_source.h as it is over archives made up here (no game file): a BARS table with AMTA and BWAV
blocks in the layout the game's have, wave buffers at their channels' samples, and the names the classes go by.

python -I -B tests/test_audio_source.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = []
ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "audio_source.h"

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

/* A BWAV at b: its channels' samples follow the tables, 0x40-aligned, `each` bytes a channel. Its size. */
static size_t bwav(uint8_t *b, uint32_t crc, unsigned channels, size_t each) {
    memcpy(b, "BWAV", 4), put16(b + 4, 0xfeff), put16(b + 6, 1), put32(b + 8, crc), put16(b + 0x0c, 1), put16(b + 0x0e, (uint16_t)channels);
    size_t data = (0x10 + channels * 0x4c + 0x3f) & ~(size_t)0x3f;
    for (unsigned ch = 0; ch < channels; ch++, data += each) {
        uint8_t *info = b + 0x10 + ch * 0x4c;
        put16(info, 1), put32(info + 4, 48000), put32(info + 0x30, (uint32_t)data), put32(info + 0x34, (uint32_t)data);
        memset(b + data, 0x5a, each);
    }
    return data;
}
/* An AMTA at b with junk and the name inside. Its size. */
static size_t amta(uint8_t *b, const char *name, int junk) {
    memcpy(b, "AMTA", 4), put16(b + 4, 0xfeff), put16(b + 6, 5);
    size_t at = 0x34;
    if (junk) memcpy(b + at, "8wRA", 4), at += 4;   /* (bytes of the block that read as name characters, before the name) */
    else b[at++] = 0;
    memcpy(b + at, name, strlen(name) + 1), at += strlen(name) + 1;
    at = (at + 3) & ~(size_t)3;
    put32(b + 8, (uint32_t)at);
    return at;
}

int main(void) {
    /* ---- the classes, by the names of the game's archives and assets */
    assert(audio_source_class("NV_USen_Custom_Navi", "NV_USen_Custom_Navi_Attention01") == AUDIO_SOURCE_NAVI_VOICE);
    assert(audio_source_class("NV_JPja_Custom_Navi", "NV_JPja_Custom_Navi_LockOnEnemy02") == AUDIO_SOURCE_NAVI_VOICE);
    assert(audio_source_class("NV_USen_Custom_Zelda", "NV_USen_Custom_Zelda_Laugh00") == AUDIO_SOURCE_NONE);
    assert(audio_source_class("Npc_Navi", "SE_Navi_LockOn") == AUDIO_SOURCE_TARGET);
    assert(audio_source_class("Npc_Navi", "SE_Navi_LockOff00") == AUDIO_SOURCE_TARGET);
    assert(audio_source_class("Npc_Navi", "SE_Navi_LockOnEnemy") == AUDIO_SOURCE_TARGET);
    assert(audio_source_class("Npc_Navi", "SE_Navi_FlyApproach") == AUDIO_SOURCE_NAVI_SOUND);
    assert(audio_source_class("Npc_Navi", "SE_Navi_Emote03") == AUDIO_SOURCE_NAVI_SOUND);
    assert(audio_source_class("Npc_Navi", "SE_Dm_TimeTemple_0065_Navi_MV_01") == AUDIO_SOURCE_NONE);   /* a cutscene's 5.1 bed */
    assert(audio_source_class("Dm_Npc_Navi", "ASE_FDm_Kokiri_0060_Navi_MV_01") == AUDIO_SOURCE_NONE);
    assert(audio_source_class("Ocarina", "Ocarina_HighF") == AUDIO_SOURCE_OCARINA);
    assert(audio_source_class("Ocarina", "PlayNote") == AUDIO_SOURCE_OCARINA);
    assert(audio_source_class("Obj_Ocarina_Time_A_01", "x") == AUDIO_SOURCE_NONE);
    assert(audio_source_class("Armor_Child_AgonyStone", "SE_AgonyStone_Near00") == AUDIO_SOURCE_AGONY);
    assert(audio_source_class("Fairy", "SE_Obj_Fairy_Flying02") == AUDIO_SOURCE_FAIRY);
    assert(audio_source_class("RevivalFairy", "SE_Fairy_Fly00") == AUDIO_SOURCE_FAIRY);
    assert(audio_source_class("Npc_Fairy_Saria", "x") == AUDIO_SOURCE_NONE);
    assert(audio_source_class("Obj_Rupee_Green", "SE_Obj_Rupee_Land00") == AUDIO_SOURCE_PICKUP);
    assert(audio_source_class("Obj_Heart_Piece", "SE_Heart_Piece_Land_01") == AUDIO_SOURCE_PICKUP);
    assert(audio_source_class("DropItem", "SE_Obj_DropStart00") == AUDIO_SOURCE_PICKUP);
    assert(audio_source_class("Item_Bomb", "SE_Item_Bomb_Explosion_Water01") == AUDIO_SOURCE_ITEM);
    assert(audio_source_class("Weapon_Range_SlingShot", "SE_Weapon_Range_SlingShot_Release_00") == AUDIO_SOURCE_ITEM);
    assert(audio_source_class("Weapon_Sword_Master", "x") == AUDIO_SOURCE_NONE);
    assert(audio_source_class("UI_Static.Product.111", "SE_UI_PickUp_Rupee00") == AUDIO_SOURCE_PICKUP);
    assert(audio_source_class("UI_Static.Product.111", "SE_Sys_KeySmallGet_00") == AUDIO_SOURCE_PICKUP);
    assert(audio_source_class("UI_Static.Product.111", "8wRASE_Sys_PeaceOfHeart_5") == AUDIO_SOURCE_PICKUP);   /* metadata before the name */
    assert(audio_source_class("UI_Static.Product.111", "SE_Sys_Cursor00") == AUDIO_SOURCE_UI);
    assert(audio_source_class("UI_Static.Product.111", "SE_UI_EquipmentMenu_Decide_Armor") == AUDIO_SOURCE_UI);
    assert(audio_source_class("UI_Static.Product.111", "NV_FrogTenor_Sad01") == AUDIO_SOURCE_NONE);
    assert(audio_source_class("Player", "SE_Player_Child_LeafTrack02") == AUDIO_SOURCE_NONE);
    assert(audio_source_class("Player", "SE_Sys_HeartAlert") == AUDIO_SOURCE_LOW_HEALTH);
    assert(audio_source_class("Player_Adult", "SE_Sys_HeartAlert") == AUDIO_SOURCE_NONE);
    assert(audio_source_class("Obj_Token_Gold", "SE_Obj_Token_Gold_Get00") == AUDIO_SOURCE_PICKUP);
    assert(audio_source_class("Obj_MagicJar_Small", "SE_Obj_MagicJar_Land00") == AUDIO_SOURCE_PICKUP);
    assert(audio_source_class("Weapon_FishingRod", "SE_Weapon_FishingRod_ReelLoop_00") == AUDIO_SOURCE_FISHING);
    assert(audio_source_class("Weapon_SinkingLure", "x") == AUDIO_SOURCE_FISHING);
    assert(audio_source_class("Animal_Fish_Bass", "SE_Animal_Fish_Bass_Bite_Underwater_S_00") == AUDIO_SOURCE_FISHING);
    assert(audio_source_class("Animal_Fish_Bass", "SE_Animal_Fish_Bass_Hooking_00") == AUDIO_SOURCE_FISHING);
    assert(audio_source_class("Animal_Fish_Bass", "SE_Animal_Fish_Bass_WaterIn_00") == AUDIO_SOURCE_NONE);   /* (its splashes: the world's) */
    assert(audio_source_class("Weapon_Bottle_Bug", "x") == AUDIO_SOURCE_BOTTLE);
    assert(audio_source_class("Obj_Bottle_Letter", "x") == AUDIO_SOURCE_NONE);
    static const char *const archives[] = {"NV_USen_Custom_Navi", "Npc_Navi", "Dm_Npc_Navi", "Ocarina", "Armor_Child_AgonyStone", "Fairy",
        "RevivalFairy", "Obj_Rupee_Green", "Obj_Heart_Piece", "DropItem", "Item_Bomb", "Weapon_Range_Hookshot", "UI_Static.Product.111",
        "Player", "Obj_Token_Gold", "Obj_MagicJar_Large", "Weapon_FishingRod", "Weapon_Lure", "Animal_Fish_Bass", "Weapon_Bottle_Poe"};
    for (size_t i = 0; i < sizeof archives / sizeof *archives; i++) assert(audio_source_wants(archives[i]));
    assert(!audio_source_wants("Player_Adult") && !audio_source_wants("NV_USen_Custom_Zelda") && !audio_source_wants("BGM_Static"));

    /* ---- an archive: four assets, one with no wave, as the game's are laid out */
    enum { N = 0x20000 };
    uint8_t *bars = calloc(1, N);
    static const char *const names[] = {"SE_Navi_LockOn", "SE_Navi_FlyApproach", "SE_Dm_Kokiri_0060_Navi_02", "SE_Navi_Emote00"};
    const uint32_t crcs[] = {0xb2db631e, 0x7da57872, 0xa4a6a6ef, 0x62e5b743};
    const unsigned channels[] = {1, 2, 6, 0};
    memcpy(bars, "BARS", 4), put16(bars + 8, 0xfeff), put16(bars + 0x0a, 0x102), put32(bars + 0x0c, 4);
    size_t at = 0x10 + 4 * 4 + 4 * 8, wave[4], data[4][8];
    uint32_t amtas[4];
    for (int i = 0; i < 4; i++) amtas[i] = (uint32_t)at, at += amta(bars + at, names[i], i == 1);
    for (int i = 0; i < 4; i++) {
        put32(bars + 0x10 + 16 + 8 * i, amtas[i]);
        if (!channels[i]) { put32(bars + 0x10 + 16 + 8 * i + 4, 0xffffffffu); continue; }
        at = (at + 0x3f) & ~(size_t)0x3f;
        wave[i] = at;
        put32(bars + 0x10 + 16 + 8 * i + 4, (uint32_t)at);
        const size_t first = (0x10 + channels[i] * 0x4c + 0x3f) & ~(size_t)0x3f;
        for (unsigned ch = 0; ch < channels[i]; ch++) data[i][ch] = at + first + ch * 0x1c40;
        at += bwav(bars + at, crcs[i], channels[i], 0x1c40);
    }
    assert(at <= N);
    put32(bars + 4, (uint32_t)at);
    assert(data[0][0] == wave[0] + 0x80 && data[1][0] == wave[1] + 0xc0 && data[2][0] == wave[2] + 0x200);   /* the game's offsets */

    char name[128];
    audio_source_asset_name(bars + amtas[1], audio_source_u32(bars + amtas[1] + 8), name, sizeof name);
    assert(strstr(name, "SE_Navi_FlyApproach"));
    audio_source_asset_name(bars + amtas[0], audio_source_u32(bars + amtas[0] + 8), name, sizeof name);
    assert(!strcmp(name, "SE_Navi_LockOn"));
    audio_source_asset_name(bars + amtas[0], audio_source_u32(bars + amtas[0] + 8), name, 5);
    assert(!strcmp(name, "SE_N"));

    static AudioSourceTable table;
    assert(audio_source_add_bars(&table, "Npc_Navi", bars, at) == 2);   /* the cutscene's is not classed; one has no wave */
    assert(table.count == 2 && !table.full);
    assert(audio_source_get(&table, crcs[0]) == AUDIO_SOURCE_TARGET);
    assert(audio_source_get(&table, crcs[1]) == AUDIO_SOURCE_NAVI_SOUND);
    assert(audio_source_get(&table, crcs[2]) == AUDIO_SOURCE_NONE && audio_source_get(&table, crcs[3]) == AUDIO_SOURCE_NONE);
    assert(audio_source_get(&table, 0) == AUDIO_SOURCE_NONE);
    assert(audio_source_add_bars(&table, "Npc_Navi", bars, at) == 2 && table.count == 2);   /* read twice: no second entry */
    assert(audio_source_add_bars(&table, "Player", bars, at) == 0);

    /* ---- a wave buffer -> its asset: every channel's start, and nothing else */
    uint32_t crc = 0;
    for (int i = 0; i < 3; i++)
        for (unsigned ch = 0; ch < channels[i]; ch++) {
            crc = 0;
            assert(audio_source_find(bars + data[i][ch], data[i][ch], &crc) && crc == crcs[i]);
        }
    assert(!audio_source_find(bars + data[0][0] + 0x40, data[0][0] + 0x40, &crc));    /* the middle of the samples */
    assert(!audio_source_find(bars + data[2][1] - 0x40, data[2][1] - 0x40, &crc));
    assert(!audio_source_find(bars + data[0][0] + 1, data[0][0] + 1, &crc));          /* off the alignment */
    assert(!audio_source_find(bars + data[0][0], 0x40, &crc));                        /* the header may not be read */
    assert(!audio_source_find(bars + data[0][0], 0, &crc));
    uint8_t *stream = calloc(1, 0x20000);                                             /* a streamed sound's buffer: no header at all */
    assert(!audio_source_find(stream + 0x20000 - 0x100, 0x20000 - 0x100, &crc));
    assert(audio_source_get(&table, crcs[0]) == AUDIO_SOURCE_TARGET);

    /* ---- bytes that are not such an archive add nothing */
    static AudioSourceTable none;
    uint8_t *bad = malloc(at);
    memcpy(bad, bars, at), bad[0] = 'X';
    assert(audio_source_add_bars(&none, "Npc_Navi", bad, at) == -1);
    memcpy(bad, bars, at), put32(bad + 0x0c, 0x7fffffff);                             /* a count past the file */
    assert(audio_source_add_bars(&none, "Npc_Navi", bad, at) == -1);
    memcpy(bad, bars, at), put32(bad + 0x10 + 16 + 4, (uint32_t)at + 0x1000);         /* a wave past the file */
    assert(audio_source_add_bars(&none, "Npc_Navi", bad, at) == -1);
    memcpy(bad, bars, at), put32(bad + 0x10 + 16 + 8, 0x18);                          /* metadata that is not AMTA */
    assert(audio_source_add_bars(&none, "Npc_Navi", bad, at) == -1);
    memcpy(bad, bars, at), put16(bad + wave[1] + 0x0e, 200);                          /* a wave with 200 channels */
    assert(audio_source_add_bars(&none, "Npc_Navi", bad, at) == -1);
    assert(audio_source_add_bars(&none, "Npc_Navi", bars, 8) == -1);
    assert(none.count == 0);

    /* ---- a full table leaves assets out and says so; what is in stays found */
    static AudioSourceTable full;
    for (uint32_t i = 1; i <= AUDIO_SOURCE_MAX + 10; i++) audio_source_put(&full, i * 0x9e3779b1u, AUDIO_SOURCE_ITEM);
    assert(full.count == AUDIO_SOURCE_MAX && full.full == 10);
    for (uint32_t i = 1; i <= AUDIO_SOURCE_MAX; i++) assert(audio_source_get(&full, i * 0x9e3779b1u) == AUDIO_SOURCE_ITEM);
    assert(audio_source_get(&full, (AUDIO_SOURCE_MAX + 1) * 0x9e3779b1u) == AUDIO_SOURCE_NONE);
    audio_source_put(&full, 5, AUDIO_SOURCE_NONE), audio_source_put(&full, 5, AUDIO_SOURCE_CLASSES);
    assert(full.full == 10);
    free(bars), free(stream), free(bad);
    puts("audio source: ok");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "audio_source_test.c"
        source.write_text(FIXTURE, encoding="utf-8")
        exe = Path(tmp) / ("audio_source_test.exe" if os.name == "nt" else "audio_source_test")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined" if os.name != "nt" else "-O1",
                        "-I", str(ROOT / "host"), str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
