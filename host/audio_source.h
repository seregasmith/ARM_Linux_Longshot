/* (longshot) Which of the game's sounds a voice plays: the classes the controller speaker's menu routes.

   The game keeps its sounds in BARS archives, one per actor (romfs/Sound/Resource/<archive>.bars.zs): a table of
   {asset metadata (AMTA, with the asset's name), wave (BWAV)} pairs. A BWAV is a header (a checksum of its own at +8,
   its channels' tables from +0x10, each with the offset of its samples) and the samples, or their first 14336 when the
   rest is streamed ("prefetch"). The game plays an asset by handing nn::audio a wave buffer that points at those
   samples, inside the archive it holds in memory: so the BWAV header lies a known distance before the wave buffer, and
   its checksum names the asset.

   Nothing of the game is in this file or in the build: the checksums are read from the player's own archives at the
   start (audio_source_add_bars), and classed here by the archive's and the asset's names.

   Everything here is plain C over bytes it is handed, so the checks compile it without a game (tests/test_audio_source.py). */
#ifndef LONGSHOT_AUDIO_SOURCE_H
#define LONGSHOT_AUDIO_SOURCE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    AUDIO_SOURCE_NONE,         /* everything else: the game's output alone */
    AUDIO_SOURCE_NAVI_VOICE,   /* Navi's calls ("Hey!", "Listen!", "Watch out!"): NV_<language>_Custom_Navi */
    AUDIO_SOURCE_NAVI_SOUND,   /* her flight and emotes: Npc_Navi's SE_Navi_* */
    AUDIO_SOURCE_TARGET,       /* Z-targeting: SE_Navi_LockOn, LockOff, LockOnEnemy */
    AUDIO_SOURCE_OCARINA,      /* the ocarina's notes and songs */
    AUDIO_SOURCE_PICKUP,       /* rupees, hearts, keys, drops, Gold Skulltula tokens and magic jars */
    AUDIO_SOURCE_ITEM,         /* items in use: bombs, the boomerang, the slingshot, the bow, the hookshot */
    AUDIO_SOURCE_FAIRY,        /* fairies */
    AUDIO_SOURCE_UI,           /* menus, cursors and message sounds */
    AUDIO_SOURCE_AGONY,        /* the Stone of Agony */
    AUDIO_SOURCE_LOW_HEALTH,   /* the alarm while the hearts run low: Player's SE_Sys_HeartAlert */
    AUDIO_SOURCE_FISHING,      /* the rod's reel, the lures, and a fish biting or hooked */
    AUDIO_SOURCE_BOTTLE,       /* a bottle in use: a bug, a fish, a Poe caught; a potion */
    AUDIO_SOURCE_CLASSES
};

static inline int audio_source_starts(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

/* The class of an asset, by its archive's name (the file's, without ".bars.zs") and its own. An asset's name may carry
   bytes of its metadata before it (it is found as the last name-like run in it), so it is matched by what it contains. */
static inline int audio_source_class(const char *archive, const char *asset) {
    if (audio_source_starts(archive, "NV_") && strstr(archive, "_Custom_Navi")) return AUDIO_SOURCE_NAVI_VOICE;
    if (!strcmp(archive, "Npc_Navi") || !strcmp(archive, "Dm_Npc_Navi")) {
        if (strstr(asset, "SE_Navi_Lock")) return AUDIO_SOURCE_TARGET;
        return strstr(asset, "SE_Navi_") ? AUDIO_SOURCE_NAVI_SOUND : AUDIO_SOURCE_NONE;   /* (not the cutscenes' beds) */
    }
    if (!strcmp(archive, "Ocarina")) return AUDIO_SOURCE_OCARINA;
    if (!strcmp(archive, "Armor_Child_AgonyStone")) return AUDIO_SOURCE_AGONY;
    if (!strcmp(archive, "Fairy") || !strcmp(archive, "RevivalFairy")) return AUDIO_SOURCE_FAIRY;
    if (audio_source_starts(archive, "Obj_Rupee") || audio_source_starts(archive, "Obj_Heart") || !strcmp(archive, "DropItem") ||
        !strcmp(archive, "Obj_Token_Gold") || audio_source_starts(archive, "Obj_MagicJar_"))
        return AUDIO_SOURCE_PICKUP;
    if (!strcmp(archive, "Player")) return strstr(asset, "SE_Sys_HeartAlert") ? AUDIO_SOURCE_LOW_HEALTH : AUDIO_SOURCE_NONE;
    if (!strcmp(archive, "Weapon_FishingRod") || !strcmp(archive, "Weapon_Lure") || !strcmp(archive, "Weapon_SinkingLure"))
        return AUDIO_SOURCE_FISHING;
    if (audio_source_starts(archive, "Animal_Fish_"))   /* (a fish's bite and the hook taking it; not its splashes) */
        return strstr(asset, "Bite") || strstr(asset, "Hooking") ? AUDIO_SOURCE_FISHING : AUDIO_SOURCE_NONE;
    if (audio_source_starts(archive, "Weapon_Bottle_")) return AUDIO_SOURCE_BOTTLE;
    if (audio_source_starts(archive, "Item_") || audio_source_starts(archive, "Weapon_Range_")) return AUDIO_SOURCE_ITEM;
    if (audio_source_starts(archive, "UI_Static")) {
        static const char *const pickup[] = {"SE_UI_PickUp_", "SE_Sys_HeartRecover", "SE_Sys_KeySmallGet", "SE_Sys_BossKeyGet",
                                             "SE_Sys_PieceofHeart", "SE_Sys_PeaceOfHeart", "SE_Sys_HeartMax_Up", "SE_Sys_MagicRecover"};
        for (size_t i = 0; i < sizeof pickup / sizeof *pickup; i++)
            if (strstr(asset, pickup[i])) return AUDIO_SOURCE_PICKUP;
        return strstr(asset, "SE_Sys_") || strstr(asset, "SE_UI_") ? AUDIO_SOURCE_UI : AUDIO_SOURCE_NONE;
    }
    return AUDIO_SOURCE_NONE;
}

/* Whether the archive is one audio_source_class reads at all: the start opens only these */
static inline int audio_source_wants(const char *archive) {
    return (audio_source_starts(archive, "NV_") && strstr(archive, "_Custom_Navi")) || !strcmp(archive, "Npc_Navi") ||
           !strcmp(archive, "Dm_Npc_Navi") || !strcmp(archive, "Ocarina") || !strcmp(archive, "Armor_Child_AgonyStone") ||
           !strcmp(archive, "Fairy") || !strcmp(archive, "RevivalFairy") || audio_source_starts(archive, "Obj_Rupee") ||
           audio_source_starts(archive, "Obj_Heart") || !strcmp(archive, "DropItem") || audio_source_starts(archive, "Item_") ||
           audio_source_starts(archive, "Weapon_Range_") || audio_source_starts(archive, "UI_Static") ||
           !strcmp(archive, "Obj_Token_Gold") || audio_source_starts(archive, "Obj_MagicJar_") || !strcmp(archive, "Player") ||
           !strcmp(archive, "Weapon_FishingRod") || !strcmp(archive, "Weapon_Lure") || !strcmp(archive, "Weapon_SinkingLure") ||
           audio_source_starts(archive, "Animal_Fish_") || audio_source_starts(archive, "Weapon_Bottle_");
}

/* ---- the table: a BWAV's checksum -> its class (open addressing; the classed assets are a few hundred) */
enum { AUDIO_SOURCE_SLOTS = 4096, AUDIO_SOURCE_MAX = AUDIO_SOURCE_SLOTS / 2 };
typedef struct {
    uint32_t crc[AUDIO_SOURCE_SLOTS];
    uint8_t cls[AUDIO_SOURCE_SLOTS];   /* (0: the slot is free) */
    int count, full;                   /* (full: assets left out because the table had no room) */
} AudioSourceTable;

static inline uint32_t audio_source_slot(uint32_t crc) { return (crc * 2654435761u) >> 20; }   /* (12 bits) */

/* Two assets with one checksum (the same sound in two languages) keep the first's class: they are classed alike. */
static inline void audio_source_put(AudioSourceTable *t, uint32_t crc, int cls) {
    if (cls <= AUDIO_SOURCE_NONE || cls >= AUDIO_SOURCE_CLASSES) return;
    for (uint32_t at = audio_source_slot(crc), n = 0; n < AUDIO_SOURCE_SLOTS; n++, at = (at + 1) % AUDIO_SOURCE_SLOTS) {
        if (t->cls[at] && t->crc[at] == crc) return;
        if (t->cls[at]) continue;
        if (t->count >= AUDIO_SOURCE_MAX) break;
        t->crc[at] = crc, t->cls[at] = (uint8_t)cls, t->count++;
        return;
    }
    t->full++;
}

static inline int audio_source_get(const AudioSourceTable *t, uint32_t crc) {
    for (uint32_t at = audio_source_slot(crc), n = 0; n < AUDIO_SOURCE_SLOTS; n++, at = (at + 1) % AUDIO_SOURCE_SLOTS) {
        if (!t->cls[at]) return AUDIO_SOURCE_NONE;
        if (t->crc[at] == crc) return t->cls[at];
    }
    return AUDIO_SOURCE_NONE;
}

static inline uint32_t audio_source_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t audio_source_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

enum {
    BWAV_HEADER = 0x10, BWAV_CRC = 8, BWAV_CHANNELS = 0x0e, BWAV_CHANNEL = 0x4c, BWAV_CHANNEL_DATA = 0x30, BWAV_MAX_CHANNELS = 8,
    BWAV_ALIGN = 0x40,
    BWAV_MAX_BEFORE = 0x10000,  /* how far before its samples a wave's header can be (6 channels' prefetch: 0xa200) */
    /* where a wave's first channel starts at the latest: right behind the header and its 8 channels' table, aligned */
    BWAV_FIRST_MAX = (BWAV_HEADER + BWAV_MAX_CHANNELS * BWAV_CHANNEL + BWAV_ALIGN - 1) / BWAV_ALIGN * BWAV_ALIGN
};

/* Whether the n bytes at b start a little-endian BWAV with 1 to 8 channels and room for their tables */
static inline int audio_source_is_bwav(const uint8_t *b, size_t n) {
    if (n < BWAV_HEADER || memcmp(b, "BWAV", 4) || audio_source_u16(b + 4) != 0xfeff) return 0;
    const unsigned channels = audio_source_u16(b + BWAV_CHANNELS);
    return channels >= 1 && channels <= BWAV_MAX_CHANNELS && n >= BWAV_HEADER + (size_t)channels * BWAV_CHANNEL;
}

/* The BWAV a wave buffer plays from. samples: the wave buffer's data; before: how many bytes before it may be read.
   Yes (1, the header's checksum in *crc) when a BWAV header lies before it, on the archive's alignment, and one of its
   channels names exactly this distance as the offset of its samples: the wave buffer is that channel's start. A
   buffer that continues a streamed sound, or belongs to no archive, has none (0). */
static inline size_t audio_source_find_back(const uint8_t *samples, size_t before, uint32_t *crc) {   /* (its distance; 0: none) */
    const size_t reach = before < BWAV_MAX_BEFORE ? before : BWAV_MAX_BEFORE;
    for (size_t back = BWAV_ALIGN * 2; back <= reach; back += BWAV_ALIGN) {
        const uint8_t *b = samples - back;
        if (!audio_source_is_bwav(b, back)) continue;
        const unsigned channels = audio_source_u16(b + BWAV_CHANNELS);
        for (unsigned ch = 0; ch < channels; ch++)
            if (audio_source_u32(b + BWAV_HEADER + ch * BWAV_CHANNEL + BWAV_CHANNEL_DATA) == back) {
                *crc = audio_source_u32(b + BWAV_CRC);
                return back;
            }
        return 0;   /* (the nearest header before the samples is another wave's: these are not a channel's start) */
    }
    return 0;
}
static inline int audio_source_find(const uint8_t *samples, size_t before, uint32_t *crc) {
    return audio_source_find_back(samples, before, crc) != 0;
}

/* An asset's name in its AMTA block (n bytes from its magic): the last run of 4 or more name characters. "" if none. */
static inline void audio_source_asset_name(const uint8_t *amta, size_t n, char *out, size_t cap) {
    size_t best = 0, best_len = 0, run = 0;
    for (size_t i = 4; i <= n; i++) {
        const int c = i < n ? amta[i] : 0;
        const int name = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (name) { run++; continue; }
        if (run >= 4) best = i - run, best_len = run;
        run = 0;
    }
    if (best_len >= cap) best_len = cap - 1;
    memcpy(out, amta + best, best_len);
    out[best_len] = 0;
}

/* One archive's assets into the table. bars: the archive as the game holds it (decompressed), n bytes; archive: its
   name. The archive's own table is followed: a count at +0xc, that many name hashes, then that many {AMTA offset, BWAV
   offset} pairs (an asset with no wave has offset 0xffffffff). The assets added, or -1 when the bytes are not such an
   archive (nothing is added then). */
static inline int audio_source_add_bars(AudioSourceTable *t, const char *archive, const uint8_t *bars, size_t n) {
    if (n < 0x10 || memcmp(bars, "BARS", 4) || audio_source_u16(bars + 8) != 0xfeff) return -1;
    const uint32_t count = audio_source_u32(bars + 0x0c);
    if (count > (n - 0x10) / 12) return -1;
    const uint8_t *pairs = bars + 0x10 + 4 * (size_t)count;
    for (uint32_t i = 0; i < count; i++) {   /* (checked whole before anything is added) */
        const uint32_t amta = audio_source_u32(pairs + 8 * (size_t)i), bwav = audio_source_u32(pairs + 8 * (size_t)i + 4);
        if (amta > n - 12 || memcmp(bars + amta, "AMTA", 4)) return -1;
        if (bwav != 0xffffffffu && (bwav > n - BWAV_HEADER || !audio_source_is_bwav(bars + bwav, n - bwav))) return -1;
    }
    int added = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t amta = audio_source_u32(pairs + 8 * (size_t)i), bwav = audio_source_u32(pairs + 8 * (size_t)i + 4);
        if (bwav == 0xffffffffu) continue;
        size_t size = audio_source_u32(bars + amta + 8);   /* (the block's own size; the next block bounds a wrong one) */
        if (size < 12 || size > n - amta) size = n - amta;
        for (uint32_t j = 0; j < count; j++) {
            const uint32_t other[2] = {audio_source_u32(pairs + 8 * (size_t)j), audio_source_u32(pairs + 8 * (size_t)j + 4)};
            for (int k = 0; k < 2; k++)
                if (other[k] > amta && other[k] - amta < size) size = other[k] - amta;
        }
        char name[128];
        audio_source_asset_name(bars + amta, size, name, sizeof name);
        const int cls = audio_source_class(archive, name);
        if (cls == AUDIO_SOURCE_NONE) continue;
        audio_source_put(t, audio_source_u32(bars + bwav + BWAV_CRC), cls);
        added++;
    }
    return added;
}

#endif
