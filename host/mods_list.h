/* mods.json, the one list of which mods are on and in which order (host/mods_list.c): its text read and written, no files
   (host/mods.c has those; nx2/exefs_patches.py reads the same file when the game is built, by the same rules).

     { "mods":   [ { "id": "local/<the mod's folder under mods>", "enabled": true }, ... ],
       "cheats": { "<a cheat's key>": true, ... } }

   The array's order is the list's order: a mod lower in it wins a file two mods have. A mod's id is "local/" and its path
   under the mods folder ('/' between folders, UTF-8) until mods have manifests. The launcher writes this file too, so what
   this code does not know is kept as it is written: other keys beside "mods" and "cheats", other keys in a mod's object
   ("version"), entries of mods that are not there, a cheat whose value is not true or false.

   Read strictly: the text is one JSON object (RFC 8259) in UTF-8, a byte order mark before it allowed; "mods" an array of
   objects, each with one "id" that is a string and at most one "enabled" that is true or false (none: on); "cheats" an
   object; "mods" and "cheats" at most once. Anything else is not a list, and nothing of it is used. An id twice: its
   first entry is the mod's. A cheat's key twice: the later one counts, and both are kept. */
#pragma once
#include <stddef.h>

enum { MODS_LIST_DEPTH = 64 };   /* arrays and objects inside each other: more is not a list */
enum { MODS_LIST_MAX = 1 << 20 };   /* a file of this many bytes or more is not a list (a thousand mods are about 60 KB) */

typedef struct { char *key, *value; } ModsListExtra;   /* a key this code does not know, and its value as the file has it */
typedef struct {
    char *id;
    int enabled;
    ModsListExtra *extra;
    int nextra;
} ModsListMod;
typedef struct { char *key, *value; int on; } ModsListCheat;   /* value: NULL for a switch (on), else what the file has */
typedef struct {
    ModsListMod *mods;
    int nmods;
    ModsListCheat *cheats;
    int ncheats, has_cheats;   /* has_cheats: the file has the key, also when it holds nothing */
    ModsListExtra *extra;
    int nextra;
} ModsList;

/* The list of a file's text: 1, or 0 with *why (a constant) and the list empty. list needs no value before */
int mods_list_parse(ModsList *list, const char *text, size_t len, const char **why);
/* The list as the file's text (malloc'd, NUL-terminated, *len without the NUL); NULL when out of memory */
char *mods_list_write(const ModsList *list, size_t *len);
void mods_list_free(ModsList *list);
/* A cheat's switch: the list's when it has one for key, else otherwise */
int mods_list_cheat(const ModsList *list, const char *key, int otherwise);
void mods_list_cheat_set(ModsList *list, const char *key, int on);
/* 1 when s is UTF-8 text a JSON string can hold as it is (no NUL: it ends there) */
int mods_list_utf8(const char *s);
