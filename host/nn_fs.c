/* nn::fs: mounts map to host folders.
     rom:      the game's RomFS (extracted by tools/nx.py romfs), read-only
     save:     <user>/save-work/<uid> (MountSaveData; committed files in <user>/save/<uid>)
     cache:    <user>/cache/<index>  (MountCacheStorage)
     temp:     <user>/temp           (MountTemporaryStorage)
   hos_romfs_dir and hos_user_dir are set by hos_main.c. Reads are positional (ReadFile with an offset), so any number
   of guest threads can read one file at once. Results: switchbrew "Error codes" (module 2 = fs). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef LONGSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/longshot_env.h"
#endif

/* (longshot) A game folder whose deepest file passes 260 characters (MAX_PATH: "E:\Games\...\romfs\Event\EventSetting\
   Ev..." at 262) is read too. The host paths here are up to HOS_FS_PATH bytes, and on Windows each file call goes through
   its wide function: the path converted from the game's UTF-8 strings and, when it is
   long and absolute, given the extended-length prefix \\?\ (with its / made \ and a doubled separator made one), which
   Windows takes without its long-paths setting. Short paths are as they were. */
#define HOS_FS_PATH 1024
#ifdef _WIN32
static const wchar_t *fs_wide(const char *p, wchar_t *w, size_t cap) {
    const size_t n = strlen(p);
    const int already = p[0] == '\\' && p[1] == '\\' && p[2] == '?';
    const int unc = !already && p[0] == '\\' && p[1] == '\\';
    const int drive = p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
    const int extended = !already && n >= 240 && (drive || unc);
    size_t at = 0;
    if (extended) {
        wcscpy(w, unc ? L"\\\\?\\UNC" : L"\\\\?\\");
        at = wcslen(w);
        if (unc) p++;   /* (\\server\share -> \\?\UNC\server\share) */
    }
    if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w + at, (int)(cap - at)) <= 0) w[0] = 0;
    if (extended) {
        /* (under the prefix Windows takes a path as it is: a / is no separator, and a doubled one is an invalid name
           (ERROR_INVALID_NAME), where a short path has both tidied. The game doubles one itself, in its streamed
           voices' "Voice/Resource//Stream/...": in a game folder long enough their files would be "not found" and the
           cutscenes silent. So here every / is a \, and a run of them one) */
        wchar_t *d = w + at;
        for (const wchar_t *q = w + at; *q; q++) {
            const wchar_t c = *q == L'/' ? L'\\' : *q;
            if (c == L'\\' && d > w + at && d[-1] == L'\\') continue;
            *d++ = c;
        }
        *d = 0;
    }
    return w;
}
#define FS_W(p) fs_wide((p), (wchar_t[HOS_FS_PATH + 8]){0}, HOS_FS_PATH + 8)
static void fs_find_data(const WIN32_FIND_DATAW *w, WIN32_FIND_DATAA *a) {
    a->dwFileAttributes = w->dwFileAttributes;
    a->ftCreationTime = w->ftCreationTime, a->ftLastAccessTime = w->ftLastAccessTime, a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh, a->nFileSizeLow = w->nFileSizeLow;
    a->dwReserved0 = w->dwReserved0, a->dwReserved1 = w->dwReserved1;
    if (WideCharToMultiByte(CP_UTF8, 0, w->cFileName, -1, a->cFileName, (int)sizeof a->cFileName, NULL, NULL) <= 0) a->cFileName[0] = 0;
    if (WideCharToMultiByte(CP_UTF8, 0, w->cAlternateFileName, -1, a->cAlternateFileName, (int)sizeof a->cAlternateFileName, NULL,
                            NULL) <= 0)
        a->cAlternateFileName[0] = 0;
}
static HANDLE fs_CreateFile(const char *p, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD how, DWORD flags, HANDLE t) {
    return CreateFileW(FS_W(p), access, share, sa, how, flags, t);
}
static DWORD fs_GetFileAttributes(const char *p) { return GetFileAttributesW(FS_W(p)); }
static BOOL fs_CreateDirectory(const char *p, LPSECURITY_ATTRIBUTES sa) { return CreateDirectoryW(FS_W(p), sa); }
static BOOL fs_DeleteFile(const char *p) { return DeleteFileW(FS_W(p)); }
static BOOL fs_RemoveDirectory(const char *p) { return RemoveDirectoryW(FS_W(p)); }
static BOOL fs_MoveFileEx(const char *a, const char *b, DWORD flags) { return MoveFileExW(FS_W(a), FS_W(b), flags); }
static HANDLE fs_FindFirstFile(const char *p, WIN32_FIND_DATAA *fd) {
    WIN32_FIND_DATAW w;
    const HANDLE h = FindFirstFileW(FS_W(p), &w);
    if (h != INVALID_HANDLE_VALUE) fs_find_data(&w, fd);
    return h;
}
static BOOL fs_FindNextFile(HANDLE h, WIN32_FIND_DATAA *fd) {
    WIN32_FIND_DATAW w;
    if (!FindNextFileW(h, &w)) return FALSE;
    fs_find_data(&w, fd);
    return TRUE;
}
#define CreateFileA fs_CreateFile
#define GetFileAttributesA fs_GetFileAttributes
#define CreateDirectoryA fs_CreateDirectory
#define DeleteFileA fs_DeleteFile
#define RemoveDirectoryA fs_RemoveDirectory
#define MoveFileExA fs_MoveFileEx
#define FindFirstFileA fs_FindFirstFile
#define FindNextFileA fs_FindNextFile
#endif

const char *hos_romfs_dir, *hos_user_dir;

#define R_PATH_NOT_FOUND 0x202          /* fs::ResultPathNotFound */
#define R_PATH_EXISTS 0x402             /* fs::ResultPathAlreadyExists */
#define R_NOT_MOUNTED 0xCA02            /* fs::ResultInvalidMountName-ish: no such mount */
#define R_TARGET_NOT_FOUND 0x7D402      /* fs::ResultTargetNotFound (save data) */
/* (0xCA01: the out-of-memory code hos_mem.c and hos_svc.c return; by Horizon's numbering it is svc::ResultInvalidSize) */
#define R_OUT_OF_MEMORY 0xCA01          /* svc::ResultOutOfMemory: the guest buffer could not be committed (Windows) */

#define FS_PATH_MAX 0x302                /* (nn::fs paths: 0x300 bytes and the NUL; DirectoryEntry's name is 0x301) */

typedef struct SaveNote SaveNote;
typedef struct Save Save;
struct SaveNote { SaveNote *next; char path[HOS_FS_PATH]; char kind; int failed, writers; };
struct Save {
    Save *next; char committed[HOS_FS_PATH], work[HOS_FS_PATH], journal[HOS_FS_PATH];
    SaveNote *notes, *deleted; int lost, handles, writers, cleanup, owned, direct;
    intptr_t owner;   /* (kept open until process exit, including after Unmount or a failed work copy) */
};
/* (save_lock comes before mount_lock or handle_lock; neither is held while taking save_lock.
   Only staged saves take it: exclusive for changes and publishing, shared for reads in W.) */
static SRWLOCK save_lock = SRWLOCK_INIT;
static Save *saves;
static int fs_trace(void);
typedef struct { char name[32]; char root[HOS_FS_PATH]; int writable; Save *save; } Mount;
static Mount mounts[16];
static int nmounts;
static SRWLOCK mount_lock = SRWLOCK_INIT;

static int mount_entry(const char *name, const char *root, int writable, Save *save) {   /* 0: the table is full */
    AcquireSRWLockExclusive(&mount_lock);
    int i;
    for (i = 0; i < nmounts && strcmp(mounts[i].name, name); i++) {}
    if (i == nmounts) {   /* (longshot: a 17th name was written past the table) */
        if (nmounts == 16) {
            ReleaseSRWLockExclusive(&mount_lock);
            hos_log("fs: %s: not mounted (16 mounts already)", name);
            return 0;
        }
        nmounts++;
    }
    snprintf(mounts[i].name, sizeof mounts[i].name, "%s", name);
    snprintf(mounts[i].root, sizeof mounts[i].root, "%s", root);
    mounts[i].writable = writable;
    mounts[i].save = save;
    ReleaseSRWLockExclusive(&mount_lock);
    if (writable) CreateDirectoryA(root, NULL);
    hos_log("fs: mounted %s: -> %s", name, root);
    return 1;
}

static int mount(const char *name, const char *root, int writable) { return mount_entry(name, root, writable, NULL); }

int hos_mods_romfs(const char *rel, char *out, size_t cap);   /* mods.c */
int hos_mods_dir_extras(const char *rel, const char *base_dir, void (*fn)(void *, const char *, int, uint64_t), void *arg);

/* (longshot) a guest path below its mount stays below it on the host: no "." or ".." part, no \\ or : (a drive or a
   Windows stream), no control characters; on Windows also no part ending in a dot or a space (Windows drops them) and
   no device name (CON, NUL, COM1, COM\u00b9, "NUL .txt"...) */
static int guest_path_ok(const char *rest) {
#ifdef _WIN32
    static const char *const devices[] = {"CON", "PRN", "AUX", "NUL", "COM", "LPT", "CONIN$", "CONOUT$", "CLOCK$"};
#endif
    for (const char *p = rest; *p;) {
        const char *e = strchr(p, '/');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p[i] < 32 || p[i] == '\\' || p[i] == ':') return 0;
#ifdef _WIN32
        if (n && (p[n - 1] == '.' || p[n - 1] == ' ')) return 0;
        for (size_t d = 0; d < sizeof devices / sizeof *devices; d++) {
            const size_t k = strlen(devices[d]);
            if (n < k || _strnicmp(p, devices[d], k)) continue;
            const char *after = p + k;
            if (k == 3 && (!strcmp(devices[d], "COM") || !strcmp(devices[d], "LPT"))) {
                /* (a digit, or a superscript 1, 2 or 3 in UTF-8, the game's code page: Windows takes those too) */
                if (after < p + n && *after >= '0' && *after <= '9') after++;
                else if (after + 1 < p + n && (unsigned char)after[0] == 0xC2 &&
                         ((unsigned char)after[1] == 0xB9 || (unsigned char)after[1] == 0xB2 || (unsigned char)after[1] == 0xB3))
                    after += 2;
                else continue;
            }
            while (after < p + n && *after == ' ') after++;   /* ("NUL .txt" is NUL too) */
            if (after == p + n || *after == '.') return 0;
        }
#endif
        p += n;
        while (*p == '/') p++;
    }
    return 1;
}

static int save_guard(const char *root, const char *rel);

/* "mount:/a/b" -> host path (a mod's file instead of the RomFS one, when a mod has it: mods.c). Returns 0 if the mount
   is unknown, or the path leaves it (guest_path_ok); 2 for a staged save, 1 otherwise. */
static int host_path(const char *guest_mem, char *out, size_t cap, int *writable) {
    /* (longshot) one copy of the path, read once: another guest thread may change guest memory between the check and
       the use */
    char guest[FS_PATH_MAX];
    if ((size_t)snprintf(guest, sizeof guest, "%s", guest_mem) >= sizeof guest) return 0;
    const char *colon = strchr(guest, ':');
    if (!colon) return 0;
    size_t n = (size_t)(colon - guest);
    int ok = 0;
    AcquireSRWLockShared(&mount_lock);
    for (int i = 0; i < nmounts; i++) {
        if (strlen(mounts[i].name) == n && !strncmp(mounts[i].name, guest, n)) {
            const char *rest = colon + 1;
            while (*rest == '/') rest++;
            if (!guest_path_ok(rest) || (mounts[i].save && !save_guard(mounts[i].root, rest))) break;
            if (mounts[i].writable || !hos_mods_romfs(rest, out, cap))
                if ((size_t)snprintf(out, cap, "%s/%s", mounts[i].root, rest) >= cap) break;   /* (cut short: not this path) */
            if (writable) *writable = mounts[i].writable;
            ok = mounts[i].save ? 2 : 1;
            break;
        }
    }
    ReleaseSRWLockShared(&mount_lock);
    for (char *p = out; ok && *p; p++) if (*p == '/') *p = '\\';
    return ok;
}

/* For other OS layer parts that open game files themselves (videos): "mount:/a/b" -> host path, 0 if unknown. */
int hos_fs_host_path(const char *guest, char *out, size_t cap) { return host_path(guest, out, cap, NULL); }

/* (longshot) a guest string: "" unless it lies in guest memory and ends within FS_PATH_MAX bytes */
static const char *gstr(uint64_t a) {
    if (!a || a >= (uint64_t)HOS_SPACE_END) return "";
    const uint64_t room = (uint64_t)HOS_SPACE_END - a;
    const char *s = (const char *)GP(a);
    const size_t max = room < FS_PATH_MAX ? (size_t)room : FS_PATH_MAX;
    return strnlen(s, max) < max ? s : "";
}

static void make_dirs(const char *path) {   /* every parent of path */
    char tmp[HOS_FS_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++)
        if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
}

/* ---- handles: a FileHandle / DirectoryHandle is one pointer-sized value; ours are table indices + 1 ----------- */
typedef struct { char name[HOS_FS_PATH]; int is_dir; uint64_t size; } ExtraEntry;   /* (a listed folder's mod-only entries) */
typedef struct {
    HANDLE h; int writable; HANDLE find; WIN32_FIND_DATAA fd; int find_done, is_dir; char path[HOS_FS_PATH];
    ExtraEntry *extras; int nextras, extra_pos;
    Save *save; char relative[HOS_FS_PATH]; int written;
} Handle;
static Handle *handles[4096];
static SRWLOCK handle_lock = SRWLOCK_INIT;
/* Protect file handle lifetime across lookup and the complete host operation. */
static SRWLOCK file_call_lock = SRWLOCK_INIT;

static uint64_t new_handle(Handle *h) {
    AcquireSRWLockExclusive(&handle_lock);
    for (int i = 0; i < 4096; i++)
        if (!handles[i]) { handles[i] = h; ReleaseSRWLockExclusive(&handle_lock); return (uint64_t)i + 1; }
    ReleaseSRWLockExclusive(&handle_lock);
    hos_fatal(NULL, "fs: out of handles");
    return 0;
}
static Handle *get_handle(Ctx *c, uint64_t id) {
    Handle *h = id && id <= 4096 ? handles[id - 1] : NULL;
    if (!h) hos_fatal(c, "fs: bad handle %#llx", (unsigned long long)id);
    return h;
}
static void free_handle(uint64_t id) {
    AcquireSRWLockExclusive(&handle_lock);
    if (id && id <= 4096) handles[id - 1] = NULL;
    ReleaseSRWLockExclusive(&handle_lock);
}

/* (longshot, ported from Hookshot 7d22d4fb, pixl27) Whether the game is writing a file now: one is open for writing and
   was written in the last second. Every close (hos_relaunch.c hos_close_finish) waits up to 3 s for such a write
   to end before the process exits. The game's threads go on meanwhile */
static int writes_open;
static int64_t last_write_ns;
static void wrote_now(void) { __atomic_store_n(&last_write_ns, hos_now_ns(), __ATOMIC_RELAXED); }
static int fs_writing(void) {
    return __atomic_load_n(&writes_open, __ATOMIC_RELAXED) > 0 &&
           hos_now_ns() - __atomic_load_n(&last_write_ns, __ATOMIC_RELAXED) < 1000000000LL;
}
void hos_fs_wait_writes(void) {
    const int64_t t0 = hos_now_ns();
    int waited = 0;
    while (fs_writing() && hos_now_ns() - t0 < 3000000000LL) waited = 1, hos_sleep_ns(10000000);
    if (waited) hos_log("stop: the game was writing a file: waited %.0f ms for it", (double)(hos_now_ns() - t0) / 1e6);
}

/* (a missing entry is different from a failed lookup: never publish a deletion after an access error) */
static int save_attr(const char *path, DWORD *at) {
#ifdef _WIN32
    *at = GetFileAttributesA(path);
#else
    extern DWORD hos_posix_save_attributes(const char *name);
    *at = hos_posix_save_attributes(path);
#endif
    return *at != INVALID_FILE_ATTRIBUTES || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}
static int save_join(char *out, const char *root, const char *rel) {
    return snprintf(out, HOS_FS_PATH, "%s%s%s", root, *rel ? "\\" : "", rel) < HOS_FS_PATH;
}
static int save_sync_directory(const char *path) {
#ifdef _WIN32
    (void)path;
    return 1;
#else
    extern BOOL hos_posix_sync_directory(const char *name);
    return hos_posix_sync_directory(path);
#endif
}
static int save_sync_parent(const char *path) {
    char parent[HOS_FS_PATH];
    snprintf(parent, sizeof parent, "%s", path);
    char *a = strrchr(parent, '/'), *b = strrchr(parent, '\\');
    char *last = !a ? b : !b ? a : a > b ? a : b;
    if (!last) return save_sync_directory(".");
    if (last == parent) last[1] = 0; else *last = 0;
    return save_sync_directory(parent);
}
static int save_relative(const char *p) {
    const size_t n = strlen(p);
    if (!n || *p == '/' || !guest_path_ok(p) || p[n - 1] == '/' || strstr(p, "//")) return 0;
    for (const char *q = p; *q; q++) if (strchr("*?\"<>|", *q)) return 0;
    for (const char *q = p; *q;) {
        const char *e = strchr(q, '/');
        const size_t k = e ? (size_t)(e - q) : strlen(q);
        if (k >= 13 && !_strnicmp(q + k - 13, ".longshot-new", 13)) return 0;
        q += k;
        if (*q) q++;
    }
    return 1;
}
/* (the configured committed root may itself be a link to another drive; nothing below either root may be a link) */
static int save_path(char *out, const char *root, const char *rel) {
    if (*rel && !save_relative(rel)) return 0;
    if (!save_join(out, root, rel)) return 0;
    if (!*rel) return 1;
    const size_t base = strlen(root);
    for (char *p = out + base; *p; p++) {
        if (*p != '\\' && *p != '/') continue;
        if (p == out + base) continue;
        const char ch = *p;
        *p = 0;
        DWORD at;
        const int ok = save_attr(out, &at);
        *p = ch;
        if (!ok || (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_REPARSE_POINT))) return 0;
        if (at == INVALID_FILE_ATTRIBUTES) return 1;
    }
    DWORD at;
    return save_attr(out, &at) && (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_REPARSE_POINT));
}
static int save_guard(const char *root, const char *rel) {
    DWORD at;
    if (!save_attr(root, &at) || at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY) ||
        (at & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    char normalized[HOS_FS_PATH], checked[HOS_FS_PATH];
    char *to = normalized;
    while (*rel) {
        if (*rel == '/') { *to++ = '/'; while (*rel == '/') rel++; }
        else *to++ = *rel++;
    }
    if (to > normalized && to[-1] == '/') to--;
    *to = 0;
    return save_path(checked, root, normalized);
}
static int save_mkdir(const char *p) {
    DWORD at;
    if (!save_attr(p, &at)) return 0;
    if (at != INVALID_FILE_ATTRIBUTES) return (at & FILE_ATTRIBUTE_DIRECTORY) && !(at & FILE_ATTRIBUTE_REPARSE_POINT);
    return CreateDirectoryA(p, NULL);
}
static int save_copy(const char *from, const char *to, int replace, int flush) {
    HANDLE in = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    HANDLE out = CreateFileA(to, GENERIC_WRITE, FILE_SHARE_READ, NULL, replace ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = out != INVALID_HANDLE_VALUE;
    char buf[65536];
    while (ok) {
        DWORD got = 0, put = 0;
        if (!ReadFile(in, buf, sizeof buf, &got, NULL)) { ok = 0; break; }
        if (!got) break;
        if (!WriteFile(out, buf, got, &put, NULL) || put != got) ok = 0;
    }
    if (ok && flush && !FlushFileBuffers(out)) ok = 0;
    if (out != INVALID_HANDLE_VALUE && !CloseHandle(out)) ok = 0;
    if (!CloseHandle(in)) ok = 0;
    if (!ok && out != INVALID_HANDLE_VALUE && !replace && !DeleteFileA(to))
        hos_log("fs: save: could not remove incomplete copy: %s", to);
    return ok;
}
typedef int (*SaveVisit)(void *, const char *, DWORD);
static int save_walk(const char *dir, SaveVisit visit, void *arg) {
#ifndef _WIN32
    extern BOOL hos_posix_save_walk(const char *name, SaveVisit visit, void *arg);
    return hos_posix_save_walk(dir, visit, arg);
#else
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    if (!save_join(pat, dir, "*")) return 0;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    int ok = 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (!fd.cFileName[0] || strchr(fd.cFileName, '/') || strchr(fd.cFileName, '\\') ||
            !visit(arg, fd.cFileName, fd.dwFileAttributes)) { ok = 0; break; }
    } while (FindNextFileA(f, &fd));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = 0;
    if (!FindClose(f)) ok = 0;
    return ok;
#endif
}
static int save_remove(const char *path);
static int save_remove_child(void *arg, const char *name, DWORD at) {
    (void)at;
    char p[HOS_FS_PATH];
    return save_join(p, arg, name) && save_remove(p);
}
static int save_remove(const char *path) {
    DWORD at;
    if (!save_attr(path, &at)) return 0;
    if (at == INVALID_FILE_ATTRIBUTES) return 1;
    if (at & FILE_ATTRIBUTE_REPARSE_POINT)
        return (at & FILE_ATTRIBUTE_DIRECTORY) ? (DeleteFileA(path) || RemoveDirectoryA(path)) : DeleteFileA(path);
    if (!(at & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileA(path);
    return save_walk(path, save_remove_child, (void *)path) && RemoveDirectoryA(path);
}
typedef struct { const char *from, *to; } SaveCopy;
static int save_copy_tree(const char *from, const char *to);
static int save_copy_child(void *arg, const char *name, DWORD at) {
    SaveCopy *d = arg;
    char a[HOS_FS_PATH], b[HOS_FS_PATH];
    /* A linked entry needs the in-place mount to retain its existing semantics. */
    if (at & FILE_ATTRIBUTE_REPARSE_POINT) return 0;
    /* Names the guest cannot address need not be present in its working copy. */
    if (!save_relative(name)) return 1;
    if (!save_join(a, d->from, name) || !save_join(b, d->to, name)) return 0;
    return (at & FILE_ATTRIBUTE_DIRECTORY) ? save_copy_tree(a, b) : save_copy(a, b, 0, 0);
}
static int save_copy_tree(const char *from, const char *to) {
    SaveCopy d = {from, to};
    return save_mkdir(to) && save_walk(from, save_copy_child, &d);
}
static void save_free_notes(SaveNote **notes) {
    while (*notes) { SaveNote *n = *notes; *notes = n->next; free(n); }
}
static void save_clear(Save *s) {
    save_free_notes(&s->notes);
    save_free_notes(&s->deleted);
    s->lost = s->cleanup = 0;
}
static SaveNote *save_note(Save *s, const char *rel, char kind) {
    for (SaveNote *n = s->notes; n; n = n->next)
        if (!strcmp(n->path, rel)) { if (kind == 'T') n->kind = kind; return n; }
    SaveNote *n = calloc(1, sizeof *n);
    if (!n) { s->lost = 1; return NULL; }
    snprintf(n->path, sizeof n->path, "%s", rel);
    n->kind = kind, n->next = s->notes, s->notes = n;
    return n;
}
/* (remember what the game removes from W, never infer a deletion from another process's entries in C) */
static int save_remember_deleted(SaveNote **notes, const char *root, const char *rel);
typedef struct { SaveNote **notes; const char *root, *rel; } SaveDeleteScan;
static int save_remember_child(void *arg, const char *name, DWORD at) {
    (void)at;
    SaveDeleteScan *d = arg;
    char rel[HOS_FS_PATH];
    return snprintf(rel, sizeof rel, "%s/%s", d->rel, name) < (int)sizeof rel &&
        save_remember_deleted(d->notes, d->root, rel);
}
static int save_remember_deleted(SaveNote **notes, const char *root, const char *rel) {
    char path[HOS_FS_PATH]; DWORD at;
    if (!save_path(path, root, rel) || !save_attr(path, &at)) return 0;
    if (at == INVALID_FILE_ATTRIBUTES) return 1;
    SaveNote *n = calloc(1, sizeof *n);
    if (!n) return 0;
    snprintf(n->path, sizeof n->path, "%s", rel);
    n->next = *notes, *notes = n;
    SaveDeleteScan d = {notes, root, rel};
    return !(at & FILE_ATTRIBUTE_DIRECTORY) || save_walk(path, save_remember_child, &d);
}
static void save_keep_deleted(Save *s, SaveNote *notes) {
    /* (children go before parents when publishing; keep each path only once) */
    while (notes) {
        SaveNote *n = notes; notes = n->next;
        SaveNote **link = &s->deleted;
        while (*link && strlen((*link)->path) > strlen(n->path)) link = &(*link)->next;
        while (*link && strlen((*link)->path) == strlen(n->path) && strcmp((*link)->path, n->path)) link = &(*link)->next;
        if (*link && !strcmp((*link)->path, n->path)) free(n);
        else { n->next = *link; *link = n; }
    }
}
/* (save roots stay allocated; mount_lock protects the list while a new root is added) */
static Save *save_for_path(const char *path, char *rel) {
    AcquireSRWLockShared(&mount_lock);
    for (Save *s = saves; s; s = s->next) {
        const size_t n = strlen(s->work);
        size_t i = 0;
        for (; i < n; i++) {
            char a = path[i], b = s->work[i];
            if (a == '/') a = '\\';
            if (b == '/') b = '\\';
#ifdef _WIN32
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
#endif
            if (a != b) break;
        }
        if (i != n || (path[n] != '/' && path[n] != '\\' && path[n])) continue;
        const char *p = path + n;
        while (*p == '/' || *p == '\\') p++;
        char *out = rel;
        while (*p) {
            if (*p == '/' || *p == '\\') { *out++ = '/'; while (*p == '/' || *p == '\\') p++; }
            else *out++ = *p++;
        }
        if (out > rel && out[-1] == '/') out--;
        *out = 0;
        ReleaseSRWLockShared(&mount_lock);
        return s;
    }
    ReleaseSRWLockShared(&mount_lock);
    return NULL;
}
static int save_restore(Save *s, SaveNote *n) {
    char a[HOS_FS_PATH], b[HOS_FS_PATH];
    DWORD at;
    if (n->writers || !save_path(a, s->committed, n->path) || !save_path(b, s->work, n->path) || !save_attr(a, &at)) return 0;
    int ok = at == INVALID_FILE_ATTRIBUTES ? save_remove(b) :
        !(at & FILE_ATTRIBUTE_DIRECTORY) && save_copy(a, b, 1, 0);
    if (ok) n->failed = 0;
    return ok;
}
typedef struct SaveOp { struct SaveOp *next; char kind; char path[HOS_FS_PATH]; unsigned index; int staged; } SaveOp;
typedef struct { Save *save; SaveOp *first, *last; unsigned count, files; } SavePlan;
static void save_plan_free(SavePlan *p) {
    while (p->first) { SaveOp *o = p->first; p->first = o->next; free(o); }
}
static SaveOp *save_op(SavePlan *p, char kind, const char *rel) {
    for (SaveOp *o = p->first; o; o = o->next) if (o->kind == kind && !strcmp(o->path, rel)) return o;
    SaveOp *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->kind = kind, o->index = p->count++;
    snprintf(o->path, sizeof o->path, "%s", rel);
    if (p->last) p->last->next = o; else p->first = o;
    p->last = o;
    return o;
}
/* (flat staging stays on C's drive and works before a new parent exists, or when a file becomes a folder) */
static int save_stage_path(char *out, Save *s, unsigned index) {
    char name[64];
    snprintf(name, sizeof name, "%u.longshot-new", index);
    return save_join(out, s->committed, name);
}
static int save_same(const char *a, const char *b) {
    HANDLE x = CreateFileA(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (x == INVALID_HANDLE_VALUE) return -1;
    HANDLE y = CreateFileA(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int same = y == INVALID_HANDLE_VALUE ? -1 : 1;
    char xb[65536], yb[65536];
    while (same == 1) {
        DWORD nx = 0, ny = 0;
        if (!ReadFile(x, xb, sizeof xb, &nx, NULL) || !ReadFile(y, yb, sizeof yb, &ny, NULL)) { same = -1; break; }
        if (nx != ny || memcmp(xb, yb, nx)) { same = 0; break; }
        if (!nx) break;
    }
    if (!CloseHandle(x)) same = -1;
    if (y != INVALID_HANDLE_VALUE && !CloseHandle(y)) same = -1;
    return same;
}
static int save_plan_path(SavePlan *p, const char *rel, int tree);
typedef struct { SavePlan *plan; const char *rel; } SaveScan;
static int save_plan_child(void *arg, const char *name, DWORD at) {
    SaveScan *d = arg;
    char rel[HOS_FS_PATH];
    if (at & FILE_ATTRIBUTE_REPARSE_POINT) return 0;
    if (snprintf(rel, sizeof rel, "%s%s%s", d->rel, *d->rel ? "/" : "", name) >= (int)sizeof rel || !save_relative(rel)) return 0;
    return save_plan_path(d->plan, rel, 1);
}
static int save_plan_dir(SavePlan *p, const char *rel) {
    if (!*rel) return 1;
    char parent[HOS_FS_PATH], cpath[HOS_FS_PATH]; DWORD ca;
    snprintf(parent, sizeof parent, "%s", rel);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; if (!save_plan_dir(p, parent)) return 0; }
    if (!save_path(cpath, p->save->committed, rel) || !save_attr(cpath, &ca)) return 0;
    if (ca != INVALID_FILE_ATTRIBUTES && (ca & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    if (ca != INVALID_FILE_ATTRIBUTES && !save_op(p, 'Y', rel)) return 0;
    return save_op(p, 'M', rel) != NULL;
}
static int save_failed_path(Save *s, const char *rel) {
    for (SaveNote *n = s->notes; n; n = n->next) if (n->failed) {
        size_t a = strlen(rel), b = strlen(n->path);
        if (!strcmp(rel, n->path) || (!strncmp(rel, n->path, b) && rel[b] == '/') ||
            (!strncmp(n->path, rel, a) && n->path[a] == '/')) return 1;
    }
    return 0;
}
static int save_plan_deleted(SavePlan *p, const char *rel) {
    if (save_failed_path(p->save, rel)) return 1;
    char w[HOS_FS_PATH], cpath[HOS_FS_PATH]; DWORD wa, ca;
    if (!save_path(w, p->save->work, rel) || !save_attr(w, &wa)) return 0;
    if (wa != INVALID_FILE_ATTRIBUTES) return 1;
    if (!save_path(cpath, p->save->committed, rel) || !save_attr(cpath, &ca)) return 0;
    return ca == INVALID_FILE_ATTRIBUTES || save_op(p, 'D', rel);
}
static int save_plan_path(SavePlan *p, const char *rel, int tree) {
    Save *s = p->save;
    if (save_failed_path(s, rel)) return 1;
    char w[HOS_FS_PATH], cpath[HOS_FS_PATH]; DWORD wa, ca;
    if (!save_path(w, s->work, rel) || !save_path(cpath, s->committed, rel) || !save_attr(w, &wa) || !save_attr(cpath, &ca)) return 0;
    if (wa == INVALID_FILE_ATTRIBUTES) {
        if (!tree) {
            hos_log("fs: save: %s is gone from the work folder; the committed file is kept", rel);
            return 2;
        }
        return 1;
    }
    if (wa & FILE_ATTRIBUTE_DIRECTORY) {
        if (!save_plan_dir(p, rel)) return 0;
        if (!tree) return 1;
        SaveScan d = {p, rel};
        return save_walk(w, save_plan_child, &d);
    }
    char parent[HOS_FS_PATH];
    snprintf(parent, sizeof parent, "%s", rel);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; if (!save_plan_dir(p, parent)) return 0; }
    if (ca != INVALID_FILE_ATTRIBUTES && !(ca & FILE_ATTRIBUTE_DIRECTORY)) {
        int same = save_same(w, cpath);
        if (same < 0) return 0;
        if (same) return 1;
    }
    if (ca != INVALID_FILE_ATTRIBUTES && (ca & FILE_ATTRIBUTE_DIRECTORY) && !save_op(p, 'X', rel)) return 0;
    for (SaveOp *o = p->first; o; o = o->next) if (o->kind == 'R' && !strcmp(o->path, rel)) return 1;
    SaveOp *o = save_op(p, 'R', rel);
    char stage[HOS_FS_PATH];
    if (!o || !save_stage_path(stage, s, o->index) || !save_copy(w, stage, 0, 1)) return 0;
    o->staged = 1;
    p->files++;
    return 1;
}
static int save_not_empty(void *arg, const char *name, DWORD at) {
    (void)name, (void)at;
    *(int *)arg = 1;
    return 1;
}
static int save_apply(SavePlan *p) {
    if (!save_sync_parent(p->save->journal)) return 0;
    for (SaveOp *o = p->first; o; o = o->next) {
        char path[HOS_FS_PATH], stage[HOS_FS_PATH]; DWORD at;
        if (!save_path(path, p->save->committed, o->path) || !save_attr(path, &at)) return 0;
        if (o->kind == 'D') {
            if (at == INVALID_FILE_ATTRIBUTES) continue;
            if (at & FILE_ATTRIBUTE_DIRECTORY) {
                /* (a new child from the in-place copy keeps its folder, even during replay) */
                int nonempty = 0;
                if (!save_walk(path, save_not_empty, &nonempty)) return 0;
                if (!nonempty && !RemoveDirectoryA(path)) return 0;
            } else if (!DeleteFileA(path)) return 0;
        }
        else if (o->kind == 'X' || o->kind == 'Y') {
            if (at != INVALID_FILE_ATTRIBUTES && (!!(at & FILE_ATTRIBUTE_DIRECTORY) == (o->kind == 'X')) &&
                !((at & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryA(path) : DeleteFileA(path))) return 0;
        } else if (o->kind == 'M') { if (!save_mkdir(path)) return 0; }
        else if (o->kind == 'R') {
            DWORD sa;
            if (!save_stage_path(stage, p->save, o->index) || !save_attr(stage, &sa)) return 0;
            if (sa == INVALID_FILE_ATTRIBUTES) {
                if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) return 0;
                continue;
            }
            if (sa & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
            int moved = 0;
            for (int tries = 0; tries <= 20; tries++) {
                if (MoveFileExA(stage, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { moved = 1; break; }
                if (tries != 20) Sleep(50);
            }
            if (!moved) return 0;
        }
    }
    /* (persist changed directory entries before discarding the recovery record) */
    for (SaveOp *o = p->first; o; o = o->next) {
        char parent[HOS_FS_PATH];
        if (!save_path(parent, p->save->committed, o->path)) return 0;
        char *last = strrchr(parent, '/');
        char *back = strrchr(parent, '\\');
        if (!last || (back && back > last)) last = back;
        while (last && last > parent + strlen(p->save->committed)) {
            *last = 0;
            DWORD at;
            if (!save_attr(parent, &at)) return 0;
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!save_sync_directory(parent)) return 0;
                break;
            }
            last = strrchr(parent, '/'); back = strrchr(parent, '\\');
            if (!last || (back && back > last)) last = back;
        }
    }
    if (!save_sync_directory(p->save->committed) || !DeleteFileA(p->save->journal)) return 0;
    return save_sync_parent(p->save->journal);
}
/* (validate the entire journal before applying any line; -1 keeps a valid journal whose replay failed) */
static int save_replay(Save *s) {
    DWORD at;
    if (!save_attr(s->journal, &at)) return -1;
    if (at == INVALID_FILE_ATTRIBUTES) return 0;
    if (at & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return -1;
    HANDLE h = CreateFileA(s->journal, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size;
    int ok = GetFileSizeEx(h, &size);
    char *buf = NULL;
    if (ok && size.QuadPart > 0 && size.QuadPart <= 16 * 1024 * 1024) buf = malloc((size_t)size.QuadPart + 1);
    if (!buf) {
        const int closed = CloseHandle(h);
        if (ok && size.QuadPart == 0 && closed) {
            if (!DeleteFileA(s->journal)) return -1;
            s->cleanup = 1;
            return 0;
        }
        return -1;
    }
    DWORD got = 0;
    ok = ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart;
    if (!CloseHandle(h)) ok = 0;
    if (!ok) { free(buf); return -1; }
    buf[got] = 0;
    SavePlan p = {0}; p.save = s;
    int whole = 0;
    char *line = buf, *end = buf + got;
    while (line < end) {
        char *e = memchr(line, '\n', (size_t)(end - line));
        if (!e) break;
        *e = 0;
        if (e - line == 1 && line[0] == 'E' && e + 1 == end) { whole = 1; break; }
        if (strlen(line) != (size_t)(e - line) || e - line < 3 || line[1] != ' ' || !strchr("XYMRD", line[0]) ||
            !save_relative(line + 2)) break;
        char path[HOS_FS_PATH];
        if (!save_path(path, s->committed, line + 2)) { save_plan_free(&p); free(buf); return -1; }
        /* (one index per line, including repeated ops: the staging name uses its position) */
        SaveOp *o = calloc(1, sizeof *o);
        if (!o) { save_plan_free(&p); free(buf); return -1; }
        o->kind = line[0], o->index = p.count++;
        if (o->kind == 'R') p.files++;
        snprintf(o->path, sizeof o->path, "%s", line + 2);
        if (p.last) p.last->next = o; else p.first = o;
        p.last = o;
        line = e + 1;
    }
    const int64_t start = hos_now_ns();
    int result = whole ? (save_apply(&p) ? 1 : -1) : (DeleteFileA(s->journal) ? 0 : -1);
    if (!whole && result == 0) s->cleanup = 1;
    if (result == 1 && p.count) hos_log("fs: save: published %u files in %.3f ms", p.files, (hos_now_ns() - start) / 1e6);
    save_plan_free(&p); free(buf);
    return result;
}
static int save_journal(SavePlan *p) {
    if (!save_sync_directory(p->save->committed) || !save_sync_parent(p->save->committed) ||
        !save_sync_directory(hos_user_dir) || !save_sync_parent(hos_user_dir)) return 0;
    size_t bytes = 2;
    for (SaveOp *o = p->first; o; o = o->next) bytes += strlen(o->path) + 3;
    if (bytes > 16 * 1024 * 1024) return 0;
    char tmp[HOS_FS_PATH];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", p->save->journal) >= (int)sizeof tmp) return 0;
    if (!save_remove(tmp)) return 0;
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    for (SaveOp *o = p->first; ok && o; o = o->next) {
        char line[HOS_FS_PATH + 4];
        DWORD put = 0;
        const DWORD n = (DWORD)snprintf(line, sizeof line, "%c %s\n", o->kind, o->path);
        ok = WriteFile(h, line, n, &put, NULL) && put == n;
    }
    DWORD put = 0;
    if (ok) ok = WriteFile(h, "E\n", 2, &put, NULL) && put == 2 && FlushFileBuffers(h);
    if (!CloseHandle(h)) ok = 0;
    return ok && MoveFileExA(tmp, p->save->journal, MOVEFILE_WRITE_THROUGH);
}
static int save_commit_refused(Save *s, int64_t start, const char *why) {
    if (fs_trace()) hos_log("fs: Commit: 0 files, 0 ops, %d open writers, result %#x in %.3f ms: %s",
                           s->writers, 0x2EE202, (hos_now_ns() - start) / 1e6, why);
    return 0;
}
static int save_publish(Save *s) {
    const int64_t start = hos_now_ns();
    DWORD root_at;
    if (!save_attr(s->work, &root_at) || root_at == INVALID_FILE_ATTRIBUTES ||
        !(root_at & FILE_ATTRIBUTE_DIRECTORY) || (root_at & FILE_ATTRIBUTE_REPARSE_POINT))
        return save_commit_refused(s, start, "work folder unavailable");
    if (s->writers || s->lost) return save_commit_refused(s, start, s->writers ? "files open for writing" : "notes unavailable");
    int replay = save_replay(s);
    if (replay < 0) return save_commit_refused(s, start, "journal replay pending");
    if (!s->notes && !s->deleted) {
        if (fs_trace()) hos_log("fs: Commit: 0 files, 0 ops, 0 open writers, result 0 in %.3f ms", (hos_now_ns() - start) / 1e6);
        return 1;
    }
    for (SaveNote *n = s->notes; n; n = n->next)
        if (n->failed && !save_restore(s, n)) return save_commit_refused(s, start, "failed file restore pending");
    SavePlan p = {0}; p.save = s;
    int ok = 1, failed = 0;
    for (SaveNote *n = s->deleted; ok && n; n = n->next) ok = save_plan_deleted(&p, n->path);
    for (SaveNote **link = &s->notes; ok && *link;) {
        SaveNote *n = *link;
        if (n->failed) { failed++; link = &n->next; continue; }
        int planned = save_plan_path(&p, n->path, n->kind == 'T');
        ok = planned != 0;
        if (planned == 2) { *link = n->next; free(n); }
        else link = &n->next;
    }
    if (ok && p.count) ok = save_journal(&p);
    if (ok && p.count) ok = save_apply(&p);
    /* (once J exists, only replay may consume staging; before that C still has its old entries) */
    DWORD ja;
    if (!ok && save_attr(s->journal, &ja) && ja == INVALID_FILE_ATTRIBUTES) {
        /* Only files created by this attempt are known to be ours.  An orphan
           after a crash is left alone; CREATE_NEW will refuse a collision. */
        for (SaveOp *o = p.first; o; o = o->next) if (o->staged) {
            char stage[HOS_FS_PATH];
            if (!save_stage_path(stage, s, o->index) || !save_remove(stage))
                hos_log("fs: save: could not remove staged file %u", o->index);
        }
    }
    const double ms = (hos_now_ns() - start) / 1e6;
    if (fs_trace()) hos_log("fs: Commit: %u files, %u ops, %d failed paths, %d open writers, result %#x in %.3f ms",
                           p.files, p.count, failed, s->writers, ok ? 0 : 0x2EE202, ms);
    if (ok && p.count) hos_log("fs: save: published %u files in %.3f ms", p.files, ms);
    if (ok && !failed) save_clear(s);
    save_plan_free(&p);
    return ok;
}

/* ---- mounts --------------------------------------------------------------------------------------------------- */
HLE(_ZN2nn2fs22QueryMountRomCacheSizeEPm) { G64(ARG(0)) = 0x10000; RET(0); }
HLE(_ZN2nn2fs8MountRomEPKcPvm) {
    if (!hos_romfs_dir) hos_fatal(c, "fs: MountRom but no RomFS folder given (--romfs)");
    RET(mount(gstr(ARG(0)), hos_romfs_dir, 0) ? 0 : R_NOT_MOUNTED);
}
/* (longshot) MountRom(char const* name): the cacheless overload OoT uses (SDK 22). Left to the SDK it opens the
   file-system proxy over IPC and aborts on the empty reply. */
HLE(_ZN2nn2fs8MountRomEPKc) {
    if (!hos_romfs_dir) hos_fatal(c, "fs: MountRom but no RomFS folder given (--romfs)");
    RET(mount(gstr(ARG(0)), hos_romfs_dir, 0) ? 0 : R_NOT_MOUNTED);
}

/* (longshot) the committed folder for the control bridge's saves (host/control.c) */
static char save_root[HOS_FS_PATH];
int hos_fs_save_root(char *out, size_t cap) {
    AcquireSRWLockShared(&save_lock);
    AcquireSRWLockShared(&mount_lock);
    Save *s;
    for (s = saves; s && strcmp(s->committed, save_root); s = s->next) {}
    DWORD at;
    int direct = 0;
    for (int i = 0; i < nmounts; i++)
        if (mounts[i].writable && !mounts[i].save && !strcmp(mounts[i].root, save_root)) direct = 1;
    const int have = s && !s->direct && !direct && save_attr(s->journal, &at) &&
                     at == INVALID_FILE_ATTRIBUTES && strlen(save_root) < cap;
    if (have) snprintf(out, cap, "%s", save_root);
    ReleaseSRWLockShared(&mount_lock);
    if (!have) ReleaseSRWLockShared(&save_lock);
    return have;
}
void hos_fs_save_end(void) { ReleaseSRWLockShared(&save_lock); }

static int user_mount(const char *name, const char *sub) {
    char root[HOS_FS_PATH];
    const int is_save = !strncmp(sub, "save\\", 5);
    if (is_save) AcquireSRWLockExclusive(&save_lock);
    snprintf(root, sizeof root, "%s\\%s", hos_user_dir, sub);
    make_dirs(root);
    CreateDirectoryA(root, NULL);
    const int ok = mount(name, root, 1);
    if (ok && is_save) {
        AcquireSRWLockExclusive(&mount_lock);
        snprintf(save_root, sizeof save_root, "%s", root);
        ReleaseSRWLockExclusive(&mount_lock);
    }
    if (is_save) ReleaseSRWLockExclusive(&save_lock);
    return ok;
}

/* (1: owned, 0: unavailable, -1: another process owns it; never remove the ownership file) */
static int save_own(Save *s) {
    if (s->owned) return 1;
    char path[HOS_FS_PATH];
    if (snprintf(path, sizeof path, "%s.lock", s->work) >= (int)sizeof path) return 0;
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION ? -1 : 0;
    s->owner = (intptr_t)h;
#else
    extern int hos_posix_save_owner(const char *name, int *busy);
    int busy = 0, fd = hos_posix_save_owner(path, &busy);
    if (fd < 0) return busy ? -1 : 0;
    s->owner = fd;
#endif
    s->owned = 1;
    return 1;
}

static int save_mount(const char *name, const char *uid) {
    char committed[HOS_FS_PATH], work[HOS_FS_PATH], parent[HOS_FS_PATH];
    if (snprintf(committed, sizeof committed, "%s\\save\\%s", hos_user_dir, uid) >= (int)sizeof committed ||
        snprintf(parent, sizeof parent, "%s\\save-work", hos_user_dir) >= (int)sizeof parent ||
        !save_join(work, parent, uid)) return 0;
    AcquireSRWLockExclusive(&save_lock);
    Save *s;
    for (s = saves; s && strcmp(s->work, work); s = s->next) {}
    int live = 0, direct = s && s->direct;
    AcquireSRWLockShared(&mount_lock);
    for (int i = 0; i < nmounts; i++) {
        if (!strcmp(mounts[i].root, work)) live = 1;
        if (!strcmp(mounts[i].root, committed) && mounts[i].writable && !mounts[i].save) direct = 1;
    }
    ReleaseSRWLockShared(&mount_lock);
    if (direct) {
        int mounted = mount_entry(name, committed, 1, NULL);
        ReleaseSRWLockExclusive(&save_lock);
        return mounted;
    }
    if (s && s->handles) live = 1;
    int ok = strlen(work) + sizeof ".journal.tmp" <= HOS_FS_PATH, pending = 0;
    if (ok && !s) {
        s = calloc(1, sizeof *s);
        if (!s) ok = 0;
        else {
            snprintf(s->committed, sizeof s->committed, "%s", committed);
            snprintf(s->work, sizeof s->work, "%s", work);
            if (snprintf(s->journal, sizeof s->journal, "%s.journal", work) >= (int)sizeof s->journal) ok = 0;
            AcquireSRWLockExclusive(&mount_lock);
            s->next = saves, saves = s;
            ReleaseSRWLockExclusive(&mount_lock);
        }
    }
    if (ok && !live) make_dirs(committed);
    int owned = s && s->owned, busy = 0;
    if (ok && !owned) {
        make_dirs(parent);
        ok = save_mkdir(parent);
        if (ok) {
            int result = save_own(s);
            owned = result > 0, busy = result < 0;
            ok = owned;
        }
    }
    if (ok && !live) {
        DWORD at;
        ok = save_attr(committed, &at);
        /* (C itself is allowed to be a link, as it was before staging) */
        if (ok && at == INVALID_FILE_ATTRIBUTES) ok = CreateDirectoryA(committed, NULL);
        else if (ok) ok = (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (ok) ok = save_mkdir(parent);
        if (ok) {
            int r = save_replay(s);
            if (r < 0) {
                hos_log("fs: save: an unfinished save could not be completed: %s", s->journal);
                pending = 1;
            }
            if (pending) ok = 0;
        }
        if (ok) {
            char tmp[HOS_FS_PATH];
            ok = snprintf(tmp, sizeof tmp, "%s.tmp", s->journal) < (int)sizeof tmp && save_remove(tmp);
        }
        if (ok) {
            ok = save_remove(work);
            if (!ok) {
                hos_log("fs: save: could not remove old work copy; refusing mount");
                pending = 1;
            }
            if (ok && !save_copy_tree(committed, work)) {
                ok = 0;
                if (!save_remove(work)) {
                    hos_log("fs: save: could not remove incomplete work copy; refusing mount");
                    pending = 1;
                }
            }
        }
        if (ok) save_clear(s);
    }
    if (ok && live) {
        DWORD at;
        ok = save_attr(work, &at) && at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) &&
             !(at & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    if (!ok && s && owned) {
        DWORD journal_at;
        if (!save_attr(s->journal, &journal_at) || journal_at != INVALID_FILE_ATTRIBUTES) pending = 1;
    }
    if (!owned) pending = 1;
    if (busy) hos_log("fs: save: another running copy of the game has this save; refusing mount");
    else if (!ok) hos_log("fs: save: work copy unavailable%s", pending ? "; journal recovery must finish before mounting" : "; using the committed folder");
    int mounted = !pending && mount_entry(name, ok ? work : committed, 1, ok ? s : NULL);
    if (mounted) {
        if (!ok && s) s->direct = 1;
        AcquireSRWLockExclusive(&mount_lock);
        snprintf(save_root, sizeof save_root, "%s", committed);
        ReleaseSRWLockExclusive(&mount_lock);
    }
    ReleaseSRWLockExclusive(&save_lock);
    return mounted;
}

/* nn::fs::IsSaveDataExisting(ApplicationId, account::Uid const&) -> bool */
HLE(_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE) {
    char root[HOS_FS_PATH];
    snprintf(root, sizeof root, "%s\\save\\%016llx%016llx", hos_user_dir, (unsigned long long)G64(ARG(1) + 8),
             (unsigned long long)G64(ARG(1)));
    RET(GetFileAttributesA(root) != INVALID_FILE_ATTRIBUTES);
}
/* MountSaveData(char const*, account::Uid const&) */
HLE(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE) {
    char sub[64];
    snprintf(sub, sizeof sub, "save\\%016llx%016llx", (unsigned long long)G64(ARG(1) + 8), (unsigned long long)G64(ARG(1)));
    char name[32];
    if (snprintf(name, sizeof name, "%s", gstr(ARG(0))) >= (int)sizeof name) { RET(R_NOT_MOUNTED); return; }
    const int away = hos_vcore_away();
    const int ok = save_mount(name, sub + 5);
    if (away) hos_vcore_back();
    RET(ok ? 0 : R_NOT_MOUNTED);
}
/* MountSaveDataReadOnly(char const*, ApplicationId, account::Uid const&) */
HLE(_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE) {
    char sub[64];
    snprintf(sub, sizeof sub, "save\\%016llx%016llx", (unsigned long long)G64(ARG(2) + 8), (unsigned long long)G64(ARG(2)));
    RET(user_mount(gstr(ARG(0)), sub) ? 0 : R_NOT_MOUNTED);
}
HLE(_ZN2nn2fs21MountTemporaryStorageEPKc) { RET(user_mount(gstr(ARG(0)), "temp") ? 0 : R_NOT_MOUNTED); }
/* MountCacheStorage(char const*, int index) */
HLE(_ZN2nn2fs17MountCacheStorageEPKci) {
    char sub[32];
    snprintf(sub, sizeof sub, "cache\\%d", (int)ARG(1));
    RET(user_mount(gstr(ARG(0)), sub) ? 0 : R_NOT_MOUNTED);
}
/* CreateCacheStorage(int index, long size, long journal) */
HLE(_ZN2nn2fs18CreateCacheStorageEill) {
    char root[HOS_FS_PATH];
    snprintf(root, sizeof root, "%s\\cache\\%d\\", hos_user_dir, (int)ARG(0));
    make_dirs(root);
    RET(0);
}
HLE(_ZN2nn2fs18DeleteCacheStorageEi) { RET(0); }
/* The cache-storage list: report the indices that exist on disk. Handle: one value; info: {s32 index, pad[0x1C]} */
static int cache_list_pos;
HLE(_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE) { G64(ARG(0)) = 1; cache_list_pos = 0; RET(0); }
HLE(_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi) {
    int n = 0, max = (int)ARG(3);
    for (; cache_list_pos < 16 && n < max; cache_list_pos++) {
        char root[HOS_FS_PATH];
        snprintf(root, sizeof root, "%s\\cache\\%d", hos_user_dir, cache_list_pos);
        if (GetFileAttributesA(root) != INVALID_FILE_ATTRIBUTES) {
            memset(GP(ARG(1) + 0x20 * n), 0, 0x20);
            G32(ARG(1) + 0x20 * n) = (uint32_t)cache_list_pos;
            n++;
        }
    }
    G32(ARG(0)) = (uint32_t)n;
    RET(0);
}
HLE(_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE) {}
HLE(_ZN2nn2fs7UnmountEPKc) {
    AcquireSRWLockExclusive(&mount_lock);
    for (int i = 0; i < nmounts; i++)
        if (!strcmp(mounts[i].name, gstr(ARG(0)))) { mounts[i] = mounts[--nmounts]; break; }
    ReleaseSRWLockExclusive(&mount_lock);
}
HLE(_ZN2nn2fs6CommitEPKc) {
    Save *s = NULL;
    AcquireSRWLockShared(&mount_lock);
    for (int i = 0; i < nmounts; i++) if (!strcmp(mounts[i].name, gstr(ARG(0)))) s = mounts[i].save;
    ReleaseSRWLockShared(&mount_lock);
    const int away = s ? hos_vcore_away() : 0;
    if (s) AcquireSRWLockExclusive(&save_lock);
    int ok = !s || save_publish(s);
    if (!s && fs_trace()) hos_log("fs: Commit(%s): in place, 0 staged files, result 0", gstr(ARG(0)));
    if (s) ReleaseSRWLockExclusive(&save_lock);
    if (away) hos_vcore_back();
    RET(ok ? 0 : 0x2EE202);
}
HLE(_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE) {}

/* ---- files ---------------------------------------------------------------------------------------------------- */
int hos_blank_cached_icon(const char *guest, const char *host);   /* (totk_icon_cache.c: TOTK's fused weapon icons) */
/* HOS_FS_TRACE=1: opens, writable changes and commits, in the order the game calls them */
static int fs_trace(void) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_FS_TRACE");
    return on;
}

/* Use the same UTF-8 and long-path handling as OpenFile for the damaged-texture probe. */
HANDLE hos_fs_open_read(const char *path) {
    return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void fs_overlay_path(const char *guest, char *path, size_t cap) {
    static const char *overlay = "";
    const char *ov = __atomic_load_n(&overlay, __ATOMIC_RELAXED);
    if (ov && !*ov) {
        ov = getenv("HOS_FS_OVERLAY");
        if (ov && !*ov) ov = NULL;
        __atomic_store_n(&overlay, ov, __ATOMIC_RELAXED);
    }
    const char *rel = ov ? strstr(guest, ":/") : NULL;
    if (rel) {
        char alt[HOS_FS_PATH];
        if ((size_t)snprintf(alt, sizeof alt, "%s/%s", ov, rel + 2) < sizeof alt &&
            GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES) {
            hos_log("fs: OpenFile(%s): from the overlay", guest);
            snprintf(path, cap, "%s", alt);
        }
    }
}

/* Resolve a stand-in with the same mod, overlay and mounted game-file precedence. */
int hos_fs_resolve_read(const char *guest, char *path, size_t cap) {
    if (!host_path(guest, path, cap, NULL)) return 0;
    fs_overlay_path(guest, path, cap);
    return 1;
}
/* OpenFile(FileHandle*, char const* path, int mode): mode bit 0 read, bit 1 write, bit 2 append */
HLE(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {
    char path[HOS_FS_PATH];
    int writable = 0;
    const char *g = gstr(ARG(1));
    /* (mkw-recomp 16bc3bc: the host's file calls block, so the virtual core goes to another thread meanwhile, as
       Horizon would run another while this one waits for the file system. OoT's ARes loaders held it up to 250 ms.)
       (longshot) From before host_path: the first open of a button-prompt file builds it there (prompts.c,
       ~300 ms on Linux), and other threads that open it meanwhile wait in there for the build; with the core held
       through that, threads waiting for the core would run without one */
    const int away = hos_vcore_away();
    const int resolved = host_path(g, path, sizeof path, &writable);
    if (!resolved) {
        if (away) hos_vcore_back();
        hos_log("fs: OpenFile(%s): unknown mount", g);
        RET(R_PATH_NOT_FOUND);
        return;
    }
    int want_write = (ARG(2) & 6) != 0;
    if (want_write && !writable) { if (away) hos_vcore_back(); RET(R_PATH_NOT_FOUND); return; }
    /* (mkw-recomp) HOS_NO_MOVIES=1: MKW's WebM movies (cup openings, billboards, emblems) are not found, and the
       game goes on without them (decoded by nn_video.c otherwise) */
    if (!want_write && env_on("HOS_NO_MOVIES")) {
        const size_t n = strlen(g);
        if (n > 5 && !_stricmp(g + n - 5, ".webm")) {
            static int logged;
            if (!logged++) hos_log("fs: OpenFile(%s): movies are left out (HOS_NO_MOVIES)", g);
            if (away) hos_vcore_back();
            RET(R_PATH_NOT_FOUND);
            return;
        }
    }
    { int hos_ultrawide_redirect(const char *guest, char *out, size_t cap);   /* (totk_ultrawide_ui.c: the HUD) */
      char alt[HOS_FS_PATH];
      if (!want_write && hos_ultrawide_redirect(g, alt, sizeof alt)) snprintf(path, sizeof path, "%s", alt); }
    if (!want_write && hos_blank_cached_icon(g, path)) { if (away) hos_vcore_back(); RET(R_PATH_NOT_FOUND); return; }
    if (!want_write) fs_overlay_path(g, path, sizeof path);
    { int hos_stand_in_damaged(const char *guest, char *path, size_t cap);   /* (oot_damaged_files.c: an incomplete dump) */
      if (!want_write) hos_stand_in_damaged(g, path, sizeof path); }
    char relative[HOS_FS_PATH], checked[HOS_FS_PATH];
    Save *save = resolved == 2 ? save_for_path(path, relative) : NULL;
    if (save) AcquireSRWLockExclusive(&save_lock);
    if (save && (!save_relative(relative) || !save_path(checked, save->work, relative))) {
        ReleaseSRWLockExclusive(&save_lock);
        if (away) hos_vcore_back();
        RET(R_PATH_NOT_FOUND); return;
    }
    HANDLE h = CreateFileA(path, GENERIC_READ | (want_write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!save && away) hos_vcore_back();
    if (h == INVALID_HANDLE_VALUE) {
        if (save) { ReleaseSRWLockExclusive(&save_lock); if (away) hos_vcore_back(); }
        if (hos_trace || fs_trace()) hos_log("fs: OpenFile(%s): not found", g);
        RET(R_PATH_NOT_FOUND);
        return;
    }
    Handle *fh = (Handle *)calloc(1, sizeof *fh);
    if (!fh) {
        if (!CloseHandle(h)) hos_log("fs: OpenFile(%s): could not close host handle", g);
        if (save) { ReleaseSRWLockExclusive(&save_lock); if (away) hos_vcore_back(); }
        RET(R_OUT_OF_MEMORY); return;
    }
    fh->h = h;
    fh->writable = want_write;
    if (want_write) __atomic_add_fetch(&writes_open, 1, __ATOMIC_RELAXED);
    fh->save = save;
    if (save) {
        save->handles++;
        snprintf(fh->relative, sizeof fh->relative, "%s", relative);
        if (want_write) {
            SaveNote *n = save_note(save, relative, 'F');
            save->writers++;
            if (n) n->writers++;
        }
    }
    snprintf(fh->path, sizeof fh->path, "%s", g);
    G64(ARG(0)) = new_handle(fh);
    if (save) { ReleaseSRWLockExclusive(&save_lock); if (away) hos_vcore_back(); }
    if (hos_trace || fs_trace()) hos_log("fs: OpenFile(%s)", g);
    RET(0);
}

/* (only staged handles wait for publication or failed-write restoration; other mounts keep their own path) */
typedef void (*SaveHandleFn)(Ctx *c, Handle *fh);
static void save_handle_call(Ctx *c, SaveHandleFn fn, int closing) {
    const int away = hos_vcore_away();
    if (closing) AcquireSRWLockExclusive(&file_call_lock);
    else AcquireSRWLockShared(&file_call_lock);
    Handle *fh = get_handle(c, ARG(0));
    const int saving = fh->save != NULL;
    if (saving) AcquireSRWLockExclusive(&save_lock);
    fn(c, fh);
    if (saving) ReleaseSRWLockExclusive(&save_lock);
    if (closing) ReleaseSRWLockExclusive(&file_call_lock);
    else ReleaseSRWLockShared(&file_call_lock);
    if (away) hos_vcore_back();
}

/* (resolve first without the save lock; check a staged path again after taking it) */
typedef void (*SavePathFn)(Ctx *c, const char *path, Save *save, const char *relative);
static void save_path_call(Ctx *c, SavePathFn fn) {
    char path[HOS_FS_PATH], relative[HOS_FS_PATH], checked[HOS_FS_PATH];
    int writable = 0;
    const int resolved = host_path(gstr(ARG(0)), path, sizeof path, &writable);
    if (!resolved || !writable) { RET(R_PATH_NOT_FOUND); return; }
    Save *save = resolved == 2 ? save_for_path(path, relative) : NULL;
    const int away = save ? hos_vcore_away() : 0;
    if (save) AcquireSRWLockExclusive(&save_lock);
    if (save && (!save_relative(relative) || !save_path(checked, save->work, relative))) RET(R_PATH_NOT_FOUND);
    else fn(c, path, save, relative);
    if (save) ReleaseSRWLockExclusive(&save_lock);
    if (away) hos_vcore_back();
}

/* (oom: set to 1 when the buffer could not be committed, and nothing is read) */
static uint64_t read_at(Ctx *c, uint64_t id, int64_t off, uint64_t buf, uint64_t size, int *oom) {
    uint64_t done = 0;
    if (!hos_guest_range(buf, size)) return 0;   /* (longshot: the file's bytes go into guest memory only) */
    const int away = hos_vcore_away();
    AcquireSRWLockShared(&file_call_lock);
    Handle *fh = get_handle(c, id);
    static int64_t delay_ns = -1, delay_after_ns;   /* HOS_FS_DELAY_MS=<ms>[,<after s>] (experiment): slower reads */
    if (delay_ns < 0) {
        const char *e = getenv("HOS_FS_DELAY_MS");
        delay_ns = e ? (int64_t)(atof(e) * 1e6) : 0;
        const char *a = e ? strchr(e, ',') : NULL;
        delay_after_ns = hos_now_ns() + (a ? (int64_t)(atof(a + 1) * 1e9) : 0);
    }
    if (delay_ns && hos_now_ns() >= delay_after_ns) {
        if (fs_trace()) hos_log("fs: read %s at %lld, %llu bytes", fh->path, (long long)off, (unsigned long long)size);
        hos_sleep_ns(delay_ns);
    }
    /* (the kernel's ReadFile does not fault pages in: a heap page nothing touched yet (hos_mem.c, committed on touch) is
       committed for it first, or the read fails with ERROR_NOACCESS. Refused (the commit limit, logged with the out-of-memory
       notice): nothing is read, and the caller reports an error rather than a short read) */
    if (size && !hos_commit(buf, size)) {
        ReleaseSRWLockShared(&file_call_lock);
        if (away) hos_vcore_back();
        *oom = 1;
        return 0;
    }
    if (fh->save) AcquireSRWLockShared(&save_lock);
    while (done < size) {
        OVERLAPPED ov = {0};
        int64_t o = off + (int64_t)done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = (DWORD)(size - done > 0x40000000 ? 0x40000000 : size - done), got = 0;
        if (!ReadFile(fh->h, GP(buf + done), chunk, &got, &ov) || !got) break;
        done += got;
    }
    if (fh->save) ReleaseSRWLockShared(&save_lock);
    HOS_WRITTEN(GP(buf), (size_t)done);   /* (into guest memory: the GPU may read it) */
    {   /* HOS_FS_WATCH=<lo hex>:<hi hex> (debugging): reads into that guest range, with the file */
        static int on = -1;
        static unsigned long long wlo, whi;
        if (on < 0) on = getenv("HOS_FS_WATCH") && sscanf(getenv("HOS_FS_WATCH"), "%llx:%llx", &wlo, &whi) == 2;
        if (on && buf < whi && buf + done > wlo)
            hos_log("fs: read into watched memory: %s at %lld, %llu bytes to %#llx", fh->path, (long long)off,
                    (unsigned long long)done, (unsigned long long)buf);
    }
    ReleaseSRWLockShared(&file_call_lock);
    if (away) hos_vcore_back();
    return done;
}

/* ReadFile(FileHandle, long offset, void* buf, unsigned long size) -> Result (must read it all) */
HLE(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm) {
    int oom = 0;
    uint64_t n = read_at(c, ARG(0), (int64_t)ARG(1), ARG(2), ARG(3), &oom);
    RET(oom ? R_OUT_OF_MEMORY : n == ARG(3) ? 0 : 0x2EE202);   /* fs::ResultOutOfRange */
}
/* ReadFile(unsigned long* read, FileHandle, long offset, void* buf, unsigned long size[, ReadOption const&]) */
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm) {
    int oom = 0;
    G64(ARG(0)) = read_at(c, ARG(1), (int64_t)ARG(2), ARG(3), ARG(4), &oom);
    RET(oom ? R_OUT_OF_MEMORY : 0);
}
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE) {
    int oom = 0;
    G64(ARG(0)) = read_at(c, ARG(1), (int64_t)ARG(2), ARG(3), ARG(4), &oom);
    RET(oom ? R_OUT_OF_MEMORY : 0);
}
/* GetFileSize(long*, FileHandle) */
HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE) {
    LARGE_INTEGER sz = {0};   /* (longshot: a folder's handle has no size: 0, not the host stack's bytes) */
    const int away = hos_vcore_away();
    AcquireSRWLockShared(&file_call_lock);
    GetFileSizeEx(get_handle(c, ARG(1))->h, &sz);
    ReleaseSRWLockShared(&file_call_lock);
    if (away) hos_vcore_back();
    G64(ARG(0)) = (uint64_t)sz.QuadPart;
    RET(0);
}
/* WriteFile(FileHandle, long offset, void const* buf, unsigned long size, WriteOption const&) */
static void fs_write_file(Ctx *c, Handle *fh) {
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)ARG(1);
    ov.OffsetHigh = (DWORD)(ARG(1) >> 32);
    DWORD put = 0;
    int ok = 0;
    fh->written = 1;
    /* (longshot) from guest memory only, and the whole size: a DWORD cut wrote less than asked and said so */
    if (ARG(3) <= 0xFFFFFFFFu && hos_guest_range(ARG(2), ARG(3))) {
        if (ARG(3) && !hos_commit(ARG(2), ARG(3))) {   /* (as the read: refused, an error, not a short write) */
            if (fh->save) { SaveNote *n = save_note(fh->save, fh->relative, 'F'); if (n) n->failed = 1; }
            if (fh->writable && fs_trace()) hos_log("fs: write %s at %llu, %llu bytes, result %#x", fh->path,
                (unsigned long long)ARG(1), (unsigned long long)ARG(3), R_OUT_OF_MEMORY);
            RET(R_OUT_OF_MEMORY);
            return;
        }
        wrote_now();
        ok = WriteFile(fh->h, GP(ARG(2)), (DWORD)ARG(3), &put, &ov) && put == ARG(3);
        wrote_now();
    }
    if (fh->save && !ok) { SaveNote *n = save_note(fh->save, fh->relative, 'F'); if (n) n->failed = 1; }
    const int success = fh->save ? ok : put == ARG(3);
    if (fh->writable && fs_trace()) hos_log("fs: write %s at %llu, %llu bytes, %lu written, result %#x", fh->path,
        (unsigned long long)ARG(1), (unsigned long long)ARG(3), (unsigned long)put, success ? 0 : 0x2EE202);
    RET(success ? 0 : 0x2EE202);
}
HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE) { save_handle_call(c, fs_write_file, 0); }
static void fs_set_file_size(Ctx *c, Handle *fh) {
    LARGE_INTEGER sz;
    sz.QuadPart = (LONGLONG)ARG(1);
    wrote_now();
    const int ok = SetFilePointerEx(fh->h, sz, NULL, FILE_BEGIN) && SetEndOfFile(fh->h);
    fh->written = 1;
    if (fh->save && !ok) { SaveNote *n = save_note(fh->save, fh->relative, 'F'); if (n) n->failed = 1; }
    if (fh->writable && fs_trace()) hos_log("fs: resize %s to %llu", fh->path, (unsigned long long)ARG(1));
    RET(ok ? 0 : 0x2EE202);
}
HLE(_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl) { save_handle_call(c, fs_set_file_size, 0); }
static void fs_flush_file(Ctx *c, Handle *fh) { FlushFileBuffers(fh->h); RET(0); }
HLE(_ZN2nn2fs9FlushFileENS0_10FileHandleE) { save_handle_call(c, fs_flush_file, 0); }
static void fs_close_file(Ctx *c, Handle *fh) {
    const int closed = CloseHandle(fh->h);
    if (fh->writable) __atomic_sub_fetch(&writes_open, 1, __ATOMIC_RELAXED);
    if (fh->save) {
        Save *s = fh->save;
        s->handles--;
        if (fh->writable) s->writers--;
        SaveNote *n = fh->writable ? save_note(s, fh->relative, 'F') : NULL;
        if (n) {
            n->writers--;
            if (!closed) n->failed = 1;
            if (n->failed && !n->writers) {
                int restored = save_restore(s, n);
                hos_log("fs: save: failed write to %s; %s", fh->relative, restored ? "restored committed file" : "restore pending");
            }
        }
    }
    if (fh->writable && fs_trace()) hos_log("fs: close %s, written %d, result %d", fh->path, fh->written, closed);
    free_handle(ARG(0));
    free(fh);
}
HLE(_ZN2nn2fs9CloseFileENS0_10FileHandleE) { save_handle_call(c, fs_close_file, 1); }

/* (renaming or removing an open writer would detach its failed-write record on the POSIX host) */
static int save_busy(Save *s, const char *rel) {
    if (!s) return 0;
    const size_t n = strlen(rel);
    for (SaveNote *p = s->notes; p; p = p->next)
        if ((p->writers || p->failed) && !strncmp(p->path, rel, n) && (!p->path[n] || p->path[n] == '/')) return 1;
    return 0;
}
static int save_parents(Save *s, const char *rel) {
    char relative[HOS_FS_PATH], path[HOS_FS_PATH];
    snprintf(relative, sizeof relative, "%s", rel);
    for (char *p = relative; *p; p++) if (*p == '/') {
        *p = 0;
        DWORD at;
        int ok = save_path(path, s->work, relative) && save_attr(path, &at);
        if (ok && at == INVALID_FILE_ATTRIBUTES) {
            ok = CreateDirectoryA(path, NULL);
            if (ok) {
                ok = save_note(s, relative, 'D') != NULL;
                if (fs_trace()) hos_log("fs: create directory %s", relative);
            }
        } else if (ok) ok = (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
        *p = '/';
        if (!ok) return 0;
    }
    return 1;
}
static void save_changed(Save *s, const char *rel, char kind, const char *what) {
    if (s) save_note(s, rel, kind);
    if (fs_trace()) hos_log("fs: %s %s", what, rel);
}

/* CreateFile(char const*, long size) */
static void fs_create_file(Ctx *c, const char *path, Save *save, const char *relative) {
    if (save) { if (!save_parents(save, relative)) { RET(0x2EE202); return; } }
    else make_dirs(path);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { RET(GetLastError() == ERROR_FILE_EXISTS ? R_PATH_EXISTS : R_PATH_NOT_FOUND); return; }
    LARGE_INTEGER sz;
    sz.QuadPart = (LONGLONG)ARG(1);
    const int positioned = SetFilePointerEx(h, sz, NULL, FILE_BEGIN);
    const int sized = SetEndOfFile(h);
    const int closed = CloseHandle(h);
    if (save && (!positioned || !sized || !closed)) {
        SaveNote *n = save_note(save, relative, 'F');
        if (n) { n->failed = 1; save_restore(save, n); }
        hos_log("fs: save: failed create of %s; restore %s", relative, n && !n->failed ? "done" : "pending");
        RET(0x2EE202); return;
    }
    save_changed(save, save ? relative : path, 'F', "create file");
    RET(0);
}
HLE(_ZN2nn2fs10CreateFileEPKcl) { save_path_call(c, fs_create_file); }
static void fs_delete_file(Ctx *c, const char *path, Save *save, const char *relative) {
    if (save_busy(save, relative)) { RET(R_PATH_NOT_FOUND); return; }
    SaveNote *deleted = NULL;
    if (save && !save_remember_deleted(&deleted, save->work, relative)) {
        save_free_notes(&deleted); RET(0x2EE202); return;
    }
    if (!DeleteFileA(path)) { save_free_notes(&deleted); RET(R_PATH_NOT_FOUND); return; }
    if (save) save_keep_deleted(save, deleted);
    save_changed(save, save ? relative : path, 'T', "delete file");
    RET(0);
}
HLE(_ZN2nn2fs10DeleteFileEPKc) { save_path_call(c, fs_delete_file); }
static void fs_create_directory(Ctx *c, const char *path, Save *save, const char *relative) {
    if (save) { if (!save_parents(save, relative)) { RET(0x2EE202); return; } }
    else make_dirs(path);
    int made = CreateDirectoryA(path, NULL);
    if (made) save_changed(save, save ? relative : path, 'D', "create directory");
    RET(made ? 0 : GetLastError() == ERROR_ALREADY_EXISTS ? R_PATH_EXISTS : R_PATH_NOT_FOUND);
}
HLE(_ZN2nn2fs15CreateDirectoryEPKc) { save_path_call(c, fs_create_directory); }
static void delete_tree(const char *dir) {
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char sub[HOS_FS_PATH];
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            /* (longshot) a link (a symbolic link, a junction) goes itself: never what it points to */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (!DeleteFileA(sub)) RemoveDirectoryA(sub); }
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(sub);
            else DeleteFileA(sub);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryA(dir);
}
static void fs_delete_directory_recursively(Ctx *c, const char *path, Save *save, const char *relative) {
    if (save_busy(save, relative)) { RET(R_PATH_NOT_FOUND); return; }
    if (save) {
        SaveNote *deleted = NULL;
        if (!save_remember_deleted(&deleted, save->work, relative)) {
            save_free_notes(&deleted); RET(0x2EE202); return;
        }
        int ok = save_remove(path);
        /* (a failed recursive removal may already have removed children) */
        save_keep_deleted(save, deleted);
        if (ok) save_changed(save, relative, 'T', "delete tree");
        RET(ok ? 0 : R_PATH_NOT_FOUND); return;
    }
    /* (longshot) the folder named itself a link: the link goes, never what it points to (as for the links inside) */
    const DWORD at = GetFileAttributesA(path);
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (!DeleteFileA(path)) RemoveDirectoryA(path);
        RET(0);
        return;
    }
    delete_tree(path);
    RET(0);
}
HLE(_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc) { save_path_call(c, fs_delete_directory_recursively); }
static void fs_rename_file(Ctx *c, const char *a, const char *b, Save *sa, Save *sb, const char *ra, const char *rb) {
    if (save_busy(sa, ra) || save_busy(sb, rb)) { RET(R_PATH_NOT_FOUND); return; }
    SaveNote *deleted = NULL;
    if (sa && !save_remember_deleted(&deleted, sa->work, ra)) {
        save_free_notes(&deleted); RET(0x2EE202); return;
    }
    DWORD at = sa ? GetFileAttributesA(a) : 0;
    int ok = MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
    if (ok) {
        if (sa) save_keep_deleted(sa, deleted);
        save_changed(sa, sa ? ra : a, 'T', "rename source");
        save_changed(sb, sb ? rb : b, (at & FILE_ATTRIBUTE_DIRECTORY) ? 'T' : 'F', "rename target");
    } else save_free_notes(&deleted);
    RET(ok ? 0 : R_PATH_NOT_FOUND);
}
HLE(_ZN2nn2fs10RenameFileEPKcS2_) {
    char a[HOS_FS_PATH], b[HOS_FS_PATH], ra[HOS_FS_PATH], rb[HOS_FS_PATH], checked[HOS_FS_PATH];
    int w1 = 0, w2 = 0;
    const int r1 = host_path(gstr(ARG(0)), a, sizeof a, &w1);
    if (!r1 || !w1) { RET(R_PATH_NOT_FOUND); return; }
    const int r2 = host_path(gstr(ARG(1)), b, sizeof b, &w2);
    if (!r2 || !w2) { RET(R_PATH_NOT_FOUND); return; }
    Save *sa = r1 == 2 ? save_for_path(a, ra) : NULL, *sb = r2 == 2 ? save_for_path(b, rb) : NULL;
    const int saving = sa || sb;
    const int away = saving ? hos_vcore_away() : 0;
    if (saving) AcquireSRWLockExclusive(&save_lock);
    if ((saving && sa != sb) || (sa && (!save_relative(ra) || !save_path(checked, sa->work, ra))) ||
        (sb && (!save_relative(rb) || !save_path(checked, sb->work, rb)))) RET(R_PATH_NOT_FOUND);
    else fs_rename_file(c, a, b, sa, sb, ra, rb);
    if (saving) ReleaseSRWLockExclusive(&save_lock);
    if (away) hos_vcore_back();
}
/* a RomFS folder that only mods have: rom:/x -> some mod entry lies under x */
static void count_extra(void *arg, const char *name, int is_dir, uint64_t size) { (void)name, (void)is_dir, (void)size; ++*(int *)arg; }
static const char *rom_rel(const char *guest) {   /* the path inside the RomFS mount, or NULL */
    const char *colon = strchr(guest, ':');
    if (!colon || !hos_romfs_dir) return NULL;
    char root[HOS_FS_PATH];
    if (!host_path(guest, root, sizeof root, NULL)) return NULL;
    AcquireSRWLockShared(&mount_lock);
    int rom = 0;
    for (int i = 0; i < nmounts; i++)
        if (strlen(mounts[i].name) == (size_t)(colon - guest) && !strncmp(mounts[i].name, guest, (size_t)(colon - guest)))
            rom = !mounts[i].writable && !strcmp(mounts[i].root, hos_romfs_dir);
    ReleaseSRWLockShared(&mount_lock);
    if (!rom) return NULL;
    const char *rest = colon + 1;
    while (*rest == '/') rest++;
    return rest;
}
static int is_mod_dir(const char *guest) {
    const char *rel = rom_rel(guest);
    int n = 0;
    if (rel) hos_mods_dir_extras(rel, "<", count_extra, &n);   /* ("<": no such folder: every entry counts) */
    return n > 0;
}

/* GetEntryType(DirectoryEntryType*, char const*): 0 directory, 1 file */
HLE(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc) {
    char path[HOS_FS_PATH];
    if (!host_path(gstr(ARG(1)), path, sizeof path, NULL)) { RET(R_PATH_NOT_FOUND); return; }
    hos_blank_cached_icon(gstr(ARG(1)), path);
    DWORD at = GetFileAttributesA(path);
    if (at == INVALID_FILE_ATTRIBUTES && is_mod_dir(gstr(ARG(1)))) at = FILE_ATTRIBUTE_DIRECTORY;   /* (only in mods) */
    if (fs_trace())
        hos_log("fs: GetEntryType(%s): %s", gstr(ARG(1)), at == INVALID_FILE_ATTRIBUTES ? "not found"
                : (at & FILE_ATTRIBUTE_DIRECTORY) ? "directory" : "file");
    if (at == INVALID_FILE_ATTRIBUTES) { RET(R_PATH_NOT_FOUND); return; }
    G32(ARG(0)) = (at & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
    RET(0);
}

/* ---- directories: DirectoryEntry = { char name[0x301]; pad[3]; s8 type at 0x304 (0 directory, 1 file); pad[3]; s64 size at 0x308 } ---- */
static void add_extra(void *arg, const char *name, int is_dir, uint64_t size) {
    Handle *dh = (Handle *)arg;
    if (!(dh->nextras & 15)) dh->extras = (ExtraEntry *)realloc(dh->extras, sizeof *dh->extras * (size_t)(dh->nextras + 16));
    ExtraEntry *e = &dh->extras[dh->nextras++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->is_dir = is_dir, e->size = size;
}
HLE(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci) {
    char path[HOS_FS_PATH], pat[HOS_FS_PATH];
    if (!host_path(gstr(ARG(1)), path, sizeof path, NULL)) { RET(R_PATH_NOT_FOUND); return; }
    snprintf(pat, sizeof pat, "%s\\*", path);
    Handle *dh = (Handle *)calloc(1, sizeof *dh);
    dh->is_dir = (int)ARG(2);   /* mode: 1 directories, 2 files, 3 both */
    snprintf(dh->path, sizeof dh->path, "%s", pat);   /* (for GetDirectoryEntryCount) */
    dh->find = FindFirstFileA(pat, &dh->fd);
    if (dh->find == INVALID_HANDLE_VALUE) dh->find_done = 1;
    if (rom_rel(gstr(ARG(1)))) hos_mods_dir_extras(rom_rel(gstr(ARG(1))), path, add_extra, dh);
    if (fs_trace())
        hos_log("fs: OpenDirectory(%s, mode %d)%s, %d entries from mods", gstr(ARG(1)), (int)ARG(2),
                dh->find == INVALID_HANDLE_VALUE ? ": not found" : "", dh->nextras);
    if (dh->find == INVALID_HANDLE_VALUE && !dh->nextras) { free(dh); RET(R_PATH_NOT_FOUND); return; }
    G64(ARG(0)) = new_handle(dh);
    RET(0);
}
/* (longshot) A directory's listing is read, counted and closed under one lock: a CloseDirectory on another guest thread freed
   the handle (and its find handle) while a ReadDirectory or GetDirectoryEntryCount still walked it */
static SRWLOCK dir_lock = SRWLOCK_INIT;
/* ReadDirectory(long* count, DirectoryEntry* entries, DirectoryHandle, long max) */
HLE(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl) {
    AcquireSRWLockExclusive(&dir_lock);
    Handle *dh = get_handle(c, ARG(2));
    int64_t n = 0, max = (int64_t)ARG(3);
    while (n < max && !dh->find_done) {
        WIN32_FIND_DATAA *fd = &dh->fd;
        int isdir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int want = (isdir && (dh->is_dir & 1)) || (!isdir && (dh->is_dir & 2));
        if (strcmp(fd->cFileName, ".") && strcmp(fd->cFileName, "..") && want) {
            uint64_t e = ARG(1) + 0x310 * n;
            memset(GP(e), 0, 0x310);
            snprintf((char *)GP(e), 0x301, "%s", fd->cFileName);
            *(uint8_t *)GP(e + 0x304) = isdir ? 0 : 1;
            G64(e + 0x308) = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
            n++;
        }
        if (!FindNextFileA(dh->find, &dh->fd)) dh->find_done = 1;
    }
    while (n < max && dh->find_done && dh->extra_pos < dh->nextras) {   /* then the mods' own entries */
        const ExtraEntry *x = &dh->extras[dh->extra_pos++];
        if (!((x->is_dir && (dh->is_dir & 1)) || (!x->is_dir && (dh->is_dir & 2)))) continue;
        uint64_t e = ARG(1) + 0x310 * n;
        memset(GP(e), 0, 0x310);
        snprintf((char *)GP(e), 0x301, "%s", x->name);
        *(uint8_t *)GP(e + 0x304) = x->is_dir ? 0 : 1;
        G64(e + 0x308) = x->size;
        n++;
    }
    ReleaseSRWLockExclusive(&dir_lock);
    G64(ARG(0)) = (uint64_t)n;
    RET(0);
}
/* (longshot) GetDirectoryEntryCount(long* count, DirectoryHandle): every entry the open mode lists, read or not */
HLE(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE) {
    AcquireSRWLockExclusive(&dir_lock);
    Handle *dh = get_handle(c, ARG(1));
    int64_t n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE f = dh->path[0] ? FindFirstFileA(dh->path, &fd) : INVALID_HANDLE_VALUE;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") &&
                ((isdir && (dh->is_dir & 1)) || (!isdir && (dh->is_dir & 2))))
                n++;
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    for (int i = 0; i < dh->nextras; i++)
        n += (dh->extras[i].is_dir && (dh->is_dir & 1)) || (!dh->extras[i].is_dir && (dh->is_dir & 2));
    ReleaseSRWLockExclusive(&dir_lock);
    G64(ARG(0)) = (uint64_t)n;
    RET(0);
}
HLE(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE) {
    AcquireSRWLockExclusive(&dir_lock);
    Handle *dh = get_handle(c, ARG(0));
    if (dh->find != INVALID_HANDLE_VALUE) FindClose(dh->find);
    free_handle(ARG(0));
    ReleaseSRWLockExclusive(&dir_lock);
    free(dh->extras);
    free(dh);
}
