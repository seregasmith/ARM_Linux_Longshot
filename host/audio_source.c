/* (longshot) The game's sounds classed for the controller speaker: the player's own archives read at the start, and the
   class of the asset a wave buffer plays (host/audio_source.h has the formats and the classes' rules).

   hos_audio_source_load reads the archives audio_source_wants names from <romfs>/Sound/Resource/*.bars.zs: each is one
   zstd frame made with the game's dictionary (zs.zsdic, a file of <romfs>/Pack/ZsDic.pack.zs, itself a plain zstd frame
   holding a SARC). Some 40 files, a few megabytes: read once, when the audio renderer opens, and only the checksums
   and classes of their assets are kept. A folder without them (another game, a dump that left them out) has no
   classes, and every sound stays on the game's output.

   HOS_AUDIO_SOURCE=0: nothing is read and no sound is classed. HOS_AUDIO_SOURCE_TRACE=1: each classed sound is logged
   when a voice starts it (checking the classes in the game). */
#include "hos.h"
#include "audio_source.h"
#include "zstd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef LONGSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/longshot_env.h"
#endif

extern const char *hos_romfs_dir;   /* nn_fs.c: the game's RomFS folder */

enum { AS_MAX_FILE = 64 << 20 };   /* (an archive's size, packed or not: the largest wanted one is about 1 MB) */

static AudioSourceTable as_table;
static volatile LONG as_ready;   /* (set last: the table is only read after it) */

static char *as_path(const char *suffix, const char *name) {
    const size_t root_n = strlen(hos_romfs_dir), suffix_n = strlen(suffix), name_n = name ? strlen(name) : 0;
    if (root_n > SIZE_MAX - suffix_n || root_n + suffix_n > SIZE_MAX - name_n - 1) return NULL;
    const size_t n = root_n + suffix_n + name_n + 1;
    char *path = (char *)malloc(n);
    if (path) {
        const int written = snprintf(path, n, "%s%s%s", hos_romfs_dir, suffix, name ? name : "");
        if (written < 0 || (size_t)written >= n) free(path), path = NULL;
    }
    return path;
}

#ifdef _WIN32
static wchar_t *as_wide(const char *path) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide = n ? (wchar_t *)malloc((size_t)n * sizeof *wide) : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) free(wide), wide = NULL;
    if (wide && n >= MAX_PATH && wcsncmp(wide, L"\\\\?\\", 4)) {
        const DWORD needed = GetFullPathNameW(wide, 0, NULL, NULL);
        wchar_t *full = needed ? (wchar_t *)malloc(((size_t)needed + 8) * sizeof *full) : NULL;
        if (full) {
            const DWORD got = GetFullPathNameW(wide, needed, full + 8, NULL);
            if (!got || got >= needed) free(full), full = NULL;
        }
        free(wide);
        if (!full) return NULL;
        wchar_t *absolute = full + 8;
        for (wchar_t *p = absolute; *p; p++) if (*p == L'/') *p = L'\\';
        const int unc = absolute[0] == L'\\' && absolute[1] == L'\\';
        wide = full;
        if (unc) {
            memcpy(wide, L"\\\\?\\UNC\\", 8 * sizeof *wide);
            memmove(wide + 8, absolute + 2, (wcslen(absolute + 2) + 1) * sizeof *wide);
        } else {
            memcpy(wide, L"\\\\?\\", 4 * sizeof *wide);
            memmove(wide + 4, absolute, (wcslen(absolute) + 1) * sizeof *wide);
        }
    }
    return wide;
}
#endif

const char *hos_audio_source_name(int cls) {
    static const char *const names[AUDIO_SOURCE_CLASSES] = {"none", "Navi's voice", "Navi's sounds", "targeting", "ocarina", "pick-ups",
                                                            "items", "fairies", "interface", "Stone of Agony", "low health",
                                                            "fishing", "bottles"};
    return cls >= 0 && cls < AUDIO_SOURCE_CLASSES ? names[cls] : "?";
}

static uint8_t *as_read(const char *path, size_t *n) {
#ifdef _WIN32
    wchar_t *wide = as_wide(path);
    FILE *f = wide ? _wfopen(wide, L"rb") : NULL;
    free(wide);
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f) return NULL;
    uint8_t *b = NULL;
    long size = 0;
    if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) > 0 && size <= AS_MAX_FILE && !fseek(f, 0, SEEK_SET) && (b = (uint8_t *)malloc((size_t)size)) &&
        fread(b, 1, (size_t)size, f) != (size_t)size) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) *n = (size_t)size;
    return b;
}

/* One zstd frame, with the dictionary when there is one (NULL: none). The content (freed by the caller) or NULL. */
static uint8_t *as_unpack(const uint8_t *in, size_t n, const ZSTD_DDict *dict, size_t *out_n) {
    const unsigned long long size = ZSTD_getFrameContentSize(in, n);
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || !size || size > AS_MAX_FILE) return NULL;
    uint8_t *out = (uint8_t *)malloc((size_t)size);
    ZSTD_DCtx *z = out ? ZSTD_createDCtx() : NULL;
    const size_t got = !z ? 0 : dict ? ZSTD_decompress_usingDDict(z, out, (size_t)size, in, n, dict) : ZSTD_decompressDCtx(z, out, (size_t)size, in, n);
    ZSTD_freeDCtx(z);
    if (!z || ZSTD_isError(got) || got != size) {
        free(out);
        return NULL;
    }
    *out_n = (size_t)size;
    return out;
}

/* A file of a SARC by its name: its bytes inside the archive (*n of them), or NULL */
static const uint8_t *as_sarc_file(const uint8_t *b, size_t n, const char *name, size_t *out_n) {
    if (n < 0x28 || memcmp(b, "SARC", 4) || memcmp(b + 0x14, "SFAT", 4)) return NULL;
    const uint32_t data = audio_source_u32(b + 0x0c), count = audio_source_u16(b + 0x1a);
    const size_t names = 0x20 + 16 * (size_t)count + 8;
    if (names > n || data > n) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *node = b + 0x20 + 16 * (size_t)i;
        const size_t at = names + 4 * (size_t)(audio_source_u32(node + 4) & 0xffffff);
        const uint32_t start = audio_source_u32(node + 8), end = audio_source_u32(node + 12);
        if (at >= n || !memchr(b + at, 0, n - at) || strcmp((const char *)b + at, name)) continue;
        if (start > end || end > n - data) return NULL;
        *out_n = end - start;
        return b + data + start;
    }
    return NULL;
}

/* The game's dictionary for its .zs files (its own copy of the bytes), or NULL */
static ZSTD_DDict *as_dictionary(void) {
    char *path = as_path("/Pack/ZsDic.pack.zs", NULL);
    size_t n = 0, sarc_n = 0, dict_n = 0;
    uint8_t *packed = path ? as_read(path, &n) : NULL;
    free(path);
    uint8_t *sarc = packed ? as_unpack(packed, n, NULL, &sarc_n) : NULL;
    free(packed);
    const uint8_t *dict = sarc ? as_sarc_file(sarc, sarc_n, "zs.zsdic", &dict_n) : NULL;
    ZSTD_DDict *d = dict ? ZSTD_createDDict(dict, dict_n) : NULL;
    free(sarc);
    return d;
}

static int as_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("HOS_AUDIO_SOURCE") || env_on("HOS_AUDIO_SOURCE");   /* (unset: on; 0, false, off, no: off) */
    return on;
}

void hos_audio_source_load(void) {
    static volatile LONG started;
    if (!as_on() || !hos_romfs_dir || InterlockedExchange(&started, 1)) return;
    ZSTD_DDict *dict = as_dictionary();   /* (none: the archives are tried as plain frames) */
    char *pattern = as_path("/Sound/Resource/*.bars.zs", NULL);
    char archive[MAX_PATH];
#ifdef _WIN32
    wchar_t *wide = pattern ? as_wide(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE find = wide ? FindFirstFileW(wide, &fd) : INVALID_HANDLE_VALUE;
    free(wide);
#else
    WIN32_FIND_DATAA fd;
    HANDLE find = pattern ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
#endif
    free(pattern);
    int files = 0, failed = 0, per[AUDIO_SOURCE_CLASSES] = {0};
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char name[1024];
#ifdef _WIN32
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
#else
            snprintf(name, sizeof name, "%s", fd.cFileName);
#endif
            const size_t len = strlen(name);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || len <= 8 || len >= sizeof archive || strcmp(name + len - 8, ".bars.zs")) continue;
            memcpy(archive, name, len - 8);
            archive[len - 8] = 0;
            if (!audio_source_wants(archive)) continue;
            char *path = as_path("/Sound/Resource/", name);
            size_t n = 0, bars_n = 0;
            uint8_t *packed = path ? as_read(path, &n) : NULL;
            free(path);
            uint8_t *bars = packed ? as_unpack(packed, n, dict, &bars_n) : NULL;
            if (!bars && packed && dict) bars = as_unpack(packed, n, NULL, &bars_n);
            free(packed);
            if (!bars || audio_source_add_bars(&as_table, archive, bars, bars_n) < 0) {
                if (failed++ < 4) hos_log("audio: %s is not read as a sound archive (%s): its sounds stay on the game's output", name,
                                          bars ? "its table" : "not unpacked");
            } else {
                files++;
            }
            free(bars);
#ifdef _WIN32
        } while (FindNextFileW(find, &fd));
#else
        } while (FindNextFileA(find, &fd));
#endif
        FindClose(find);
    }
    ZSTD_freeDDict(dict);
    for (int i = 0; i < AUDIO_SOURCE_SLOTS; i++) per[as_table.cls[i]]++;
    char line[256];
    int at = 0;
    for (int cls = 1; cls < AUDIO_SOURCE_CLASSES; cls++)
        at += snprintf(line + at, sizeof line - (size_t)at, "%s%s %d", cls > 1 ? ", " : "", hos_audio_source_name(cls), per[cls]);
    hos_log("audio: %d sounds classed from %d archives (%s)%s", as_table.count, files, line,
            as_table.full ? "; the table is full, some are left out" : "");
    InterlockedExchange(&as_ready, 1);
}

static DWORD WINAPI as_load_thread(void *unused) {
    (void)unused;
    hos_audio_source_load();
    return 0;
}

void hos_audio_source_load_async(void) {
    static volatile LONG requested;
    if (!hos_romfs_dir) return;
    if (InterlockedCompareExchange(&requested, 1, 0)) return;
    HANDLE thread = CreateThread(NULL, 0, as_load_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    else InterlockedExchange(&requested, 0);
}

int hos_audio_source_ready(void) { return InterlockedCompareExchange(&as_ready, 0, 0) != 0; }

int hos_audio_source_trace(void) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_AUDIO_SOURCE_TRACE");
    return on;
}

/* How many of the `before` bytes just in front of `data` the host may read, from `data` back (committed, readable pages) */
static size_t readable_before(uint64_t data, size_t before) {
#ifdef _WIN32
    size_t span_ok = 0;
    while (span_ok < before) {
        MEMORY_BASIC_INFORMATION info;
        const uintptr_t at = (uintptr_t)GP(data - span_ok) - 1;
        if (!VirtualQuery((const void *)at, &info, sizeof info) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) break;
        const size_t span = at - (uintptr_t)info.BaseAddress + 1;
        span_ok += span < before - span_ok ? span : before - span_ok;
    }
    return span_ok;
#else
    (void)data;
    return before;
#endif
}

/* What the searches found, by the address of the samples. A wave's channel starts, taken from its header's own table when
   one of its channels is found: its later channels lie behind the first one's samples, up to megabytes from the header,
   and a sound plays from the same memory each time it plays. An entry is trusted only while the same wave's header (its
   magic and checksum) is still in front of it: the memory may hold another wave by then. And (back 0) a place where the
   whole search found no header: a streamed sound's buffers, which come round again in its ring. One caller at a time
   (host/nn_audio.c's audio lock). */
enum { KNOWN = 512 };
static struct { uint64_t data; uint32_t back, crc; } known[KNOWN];
static unsigned known_slot(uint64_t data) { return (unsigned)(((data >> 6) * 0x9E3779B97F4A7C15ull) >> 55) & (KNOWN - 1); }

/* (a header found `back` bytes in front of `data`: every channel's start kept, by its table) */
static void known_channels(uint64_t data, size_t back, uint32_t crc) {
    const uint8_t *h = (const uint8_t *)GP(data - back);
    const unsigned channels = audio_source_u16(h + BWAV_CHANNELS);
    for (unsigned ch = 0; ch < channels; ch++) {
        const uint32_t off = audio_source_u32(h + BWAV_HEADER + ch * BWAV_CHANNEL + BWAV_CHANNEL_DATA);
        if (off < BWAV_ALIGN * 2) continue;
        const uint64_t start = data - back + off;
        const unsigned s = known_slot(start);
        known[s].data = start, known[s].back = off, known[s].crc = crc;
    }
}

/* The class of the asset whose samples start at the guest address `data` (a wave buffer's), with its checksum in *crc.
   -1 when no asset's samples start there (the buffer continues a streamed sound, or is no archive's): the voice keeps
   the class it had. Asked for every buffer the game queues, so it reads little: a place seen before, else the few spots
   right behind a header where a wave's first channel starts (BWAV_FIRST_MAX); a place found by neither is searched as far
   as a later channel's header can be (BWAV_MAX_BEFORE), once: a streamed sound's ring, which has no header, then costs a
   look in the table each time it comes round. */
int hos_audio_source_of(uint64_t data, uint32_t *crc) {
    if (!hos_audio_source_ready() || !data) return -1;
    const unsigned slot = known_slot(data);
    const int no_header = known[slot].data == data && !known[slot].back;
    if (known[slot].data == data && known[slot].back) {   /* (only its header is read: mapped and readable) */
        const uint64_t at = data - known[slot].back;
        if (data >= known[slot].back && hos_mem_region(at, BWAV_HEADER) &&
            (at >> 12 == data >> 12 || readable_before(at + BWAV_HEADER, BWAV_HEADER) == BWAV_HEADER)) {
            const uint8_t *h = (const uint8_t *)GP(at);
            if (audio_source_is_bwav(h, known[slot].back) && audio_source_u32(h + BWAV_CRC) == known[slot].crc) {
                *crc = known[slot].crc;
                return audio_source_get(&as_table, *crc);
            }
        }
        known[slot].data = 0;
    }
    /* (a wave's first channel: right behind its header; the samples' own page is readable, the game just queued them) */
    size_t before = BWAV_FIRST_MAX;
    while (before >= BWAV_ALIGN * 2 && (data < before || !hos_mem_region(data - before, before))) before -= BWAV_ALIGN;
    if (before >= BWAV_ALIGN * 2 && (data - before) >> 12 != data >> 12) before = readable_before(data, before);
    size_t back = before >= BWAV_ALIGN * 2 ? audio_source_find_back((const uint8_t *)GP(data), before, crc) : 0;
    if (!back && !no_header) {   /* (a later channel's header, as far as it can be) */
        before = BWAV_MAX_BEFORE;
        while (before >= BWAV_ALIGN * 2 && (data < before || !hos_mem_region(data - before, before))) before >>= 1;
        if (before >= BWAV_ALIGN * 2) before = readable_before(data, before);
        if (before >= BWAV_ALIGN * 2) back = audio_source_find_back((const uint8_t *)GP(data), before, crc);
        if (!back) known[slot].data = data, known[slot].back = 0;   /* (none here: not searched that far again) */
    }
    if (!back) return -1;
    known_channels(data, back, *crc);
    return audio_source_get(&as_table, *crc);
}
