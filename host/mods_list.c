/* mods.json's text (mods_list.h): read in two steps, written in one.
   Reading: first the whole text is checked to be JSON and nothing else (value: RFC 8259's grammar, strings in well-formed
   UTF-8 with no control character, no NUL and no half of a surrogate pair from an escape, at most MODS_LIST_DEPTH arrays and
   objects inside each other), then the list is taken from it (read_top), where the text can no longer end early or be
   malformed and only the list's own rules are left to check. What is not known is copied as it is written (take_raw), so
   writing the list again keeps it byte for byte.
   nx2/exefs_patches.py (parse_list) takes and refuses the same texts: tests/test_mods_list.py runs both over the same files. */
#include "mods_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p, *end, *why; int depth; } Scan;

static int fail(Scan *s, const char *why) {
    if (!s->why) s->why = why;
    return 0;
}
static void space(Scan *s) {
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}
static int digit(char c) { return c >= '0' && c <= '9'; }

/* the length of the well-formed UTF-8 sequence at p (1 to 4), 0 when there is none: no overlong form, no surrogate, nothing
   past U+10FFFF */
static int utf8_seq(const unsigned char *p, size_t left) {
    const unsigned c = p[0];
    unsigned lo = 0x80, hi = 0xBF;
    int n;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3, lo = c == 0xE0 ? 0xA0 : lo, hi = c == 0xED ? 0x9F : hi;
    else if (c >= 0xF0 && c <= 0xF4) n = 4, lo = c == 0xF0 ? 0x90 : lo, hi = c == 0xF4 ? 0x8F : hi;
    else return 0;
    if ((size_t)n > left || p[1] < lo || p[1] > hi) return 0;
    for (int i = 2; i < n; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    return n;
}
int mods_list_utf8(const char *s) {
    for (size_t left = strlen(s); left;) {
        const int n = utf8_seq((const unsigned char *)s, left);
        if (!n) return 0;
        s += n, left -= (size_t)n;
    }
    return 1;
}

static int hex4(Scan *s, unsigned *v) {
    if (s->end - s->p < 4) return 0;
    *v = 0;
    for (int i = 0; i < 4; i++) {
        const char h = *s->p++;
        const int d = digit(h) ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
        if (d < 0) return 0;
        *v = *v * 16 + (unsigned)d;
    }
    return 1;
}
/* At a string's opening quote: past its closing one. out, when given, takes the string (never longer than its text) */
static int string(Scan *s, char *out) {
    s->p++;
    for (;;) {
        if (s->p >= s->end) return fail(s, "a string without its end");
        unsigned c = (unsigned char)*s->p;
        if (c == '"') {
            s->p++;
            if (out) *out = 0;
            return 1;
        }
        if (c < 0x20) return fail(s, "a control character in a string");
        if (c != '\\') {
            const int n = utf8_seq((const unsigned char *)s->p, (size_t)(s->end - s->p));
            if (!n) return fail(s, "text that is not UTF-8");
            if (out) memcpy(out, s->p, (size_t)n), out += n;
            s->p += n;
            continue;
        }
        if (++s->p >= s->end) return fail(s, "a string without its end");
        c = (unsigned char)*s->p++;
        switch (c) {
        case '"': case '\\': case '/': break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u': {
            unsigned low;
            if (!hex4(s, &c)) return fail(s, "a bad \\u escape");
            if (c >= 0xD800 && c <= 0xDBFF) {   /* (a pair: one character past U+FFFF) */
                if (s->end - s->p < 2 || s->p[0] != '\\' || s->p[1] != 'u') return fail(s, "half a surrogate pair");
                s->p += 2;
                if (!hex4(s, &low) || low < 0xDC00 || low > 0xDFFF) return fail(s, "half a surrogate pair");
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
            } else if (c >= 0xDC00 && c <= 0xDFFF) {
                return fail(s, "half a surrogate pair");
            }
            if (!c) return fail(s, "a NUL in a string");
            break;
        }
        default: return fail(s, "a bad escape in a string");
        }
        if (!out) continue;
        if (c < 0x80) *out++ = (char)c;
        else if (c < 0x800) *out++ = (char)(0xC0 | c >> 6), *out++ = (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) *out++ = (char)(0xE0 | c >> 12), *out++ = (char)(0x80 | (c >> 6 & 0x3F)), *out++ = (char)(0x80 | (c & 0x3F));
        else
            *out++ = (char)(0xF0 | c >> 18), *out++ = (char)(0x80 | (c >> 12 & 0x3F)), *out++ = (char)(0x80 | (c >> 6 & 0x3F)),
            *out++ = (char)(0x80 | (c & 0x3F));
    }
}
static int number(Scan *s) {
    const char *p = s->p;
    if (p < s->end && *p == '-') p++;
    if (p >= s->end || !digit(*p)) return fail(s, "a bad number");
    if (*p == '0') p++;
    else
        while (p < s->end && digit(*p)) p++;
    if (p < s->end && *p == '.') {
        if (++p >= s->end || !digit(*p)) return fail(s, "a bad number");
        while (p < s->end && digit(*p)) p++;
    }
    if (p < s->end && (*p == 'e' || *p == 'E')) {
        if (++p < s->end && (*p == '+' || *p == '-')) p++;
        if (p >= s->end || !digit(*p)) return fail(s, "a bad number");
        while (p < s->end && digit(*p)) p++;
    }
    s->p = p;
    return 1;
}
static int word(Scan *s, const char *w) {
    const size_t n = strlen(w);
    if ((size_t)(s->end - s->p) < n || memcmp(s->p, w, n)) return 0;
    s->p += n;
    return 1;
}
/* Any JSON value, with the space before it: past it, or 0 with s->why */
static int value(Scan *s) {
    space(s);
    if (s->p >= s->end) return fail(s, "the text ends early");
    const char c = *s->p;
    if (c == '"') return string(s, NULL);
    if (c == '{' || c == '[') {
        const char close = c == '{' ? '}' : ']';
        if (++s->depth > MODS_LIST_DEPTH) return fail(s, "arrays and objects too deep inside each other");
        s->p++;
        space(s);
        if (s->p < s->end && *s->p == close) {
            s->p++, s->depth--;
            return 1;
        }
        for (;;) {
            if (c == '{') {
                space(s);
                if (s->p >= s->end || *s->p != '"') return fail(s, "an object's key is not a string");
                if (!string(s, NULL)) return 0;
                space(s);
                if (s->p >= s->end || *s->p != ':') return fail(s, "no ':' after a key");
                s->p++;
            }
            if (!value(s)) return 0;
            space(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            if (s->p < s->end && *s->p == close) {
                s->p++, s->depth--;
                return 1;
            }
            return fail(s, c == '{' ? "no ',' or '}' in an object" : "no ',' or ']' in an array");
        }
    }
    if (c == '-' || digit(c)) return number(s);
    if (word(s, "true") || word(s, "false") || word(s, "null")) return 1;
    return fail(s, "not JSON");
}

/* ---- the list, from text that value took: nothing ends early here */
static char *copy(const char *p, size_t n) {
    char *s = (char *)malloc(n + 1);
    if (s) memcpy(s, p, n), s[n] = 0;
    return s;
}
static char *take_string(Scan *s) {   /* at a string: the string, past it */
    Scan at = *s;
    string(s, NULL);
    char *out = (char *)malloc((size_t)(s->p - at.p) + 1);
    if (out) string(&at, out);
    return out;
}
static char *take_raw(Scan *s) {   /* a value as it is written, past it */
    space(s);
    const char *start = s->p;
    value(s);
    return copy(start, (size_t)(s->p - start));
}
/* After an opening bracket or a member: 1 at the next member, 0 past the closing bracket */
static int more(Scan *s, char close) {
    space(s);
    if (*s->p == ',') s->p++, space(s);
    if (*s->p != close) return 1;
    s->p++;
    return 0;
}
/* A member's key (malloc'd), past its ':' and the space after it */
static char *take_key(Scan *s) {
    char *key = take_string(s);
    space(s);
    s->p++;
    space(s);
    if (!key) fail(s, "out of memory");
    return key;
}
static void keep(ModsListExtra **extra, int *n, char *key, Scan *s) {   /* (takes key) */
    char *raw = take_raw(s);
    ModsListExtra *grown = raw ? (ModsListExtra *)realloc(*extra, sizeof *grown * (size_t)(*n + 1)) : NULL;
    if (!grown) {
        free(key), free(raw);
        fail(s, "out of memory");
        return;
    }
    *extra = grown;
    grown[(*n)++] = (ModsListExtra){key, raw};
}
/* One more cheat with this key; NULL out of memory. Reading adds each key as it comes, with no search for it (a file of
   a hundred thousand keys took the game's start 20 seconds that way): a key that is there twice is kept twice, and the
   later one counts (cheat_of) */
static ModsListCheat *cheat_slot(ModsList *list, const char *key) {
    char *own = copy(key, strlen(key));
    ModsListCheat *grown = own ? (ModsListCheat *)realloc(list->cheats, sizeof *grown * (size_t)(list->ncheats + 1)) : NULL;
    if (!grown) {
        free(own);
        return NULL;
    }
    list->cheats = grown;
    grown[list->ncheats] = (ModsListCheat){own, NULL, 0};
    return &grown[list->ncheats++];
}

static void read_mods(ModsList *list, Scan *s) {
    if (*s->p != '[') {
        fail(s, "\"mods\" is not an array");
        return;
    }
    s->p++;
    while (!s->why && more(s, ']')) {
        if (*s->p != '{') {
            fail(s, "an entry of \"mods\" is not an object");
            return;
        }
        ModsListMod *grown = (ModsListMod *)realloc(list->mods, sizeof *grown * (size_t)(list->nmods + 1));
        if (!grown) {
            fail(s, "out of memory");
            return;
        }
        list->mods = grown;
        ModsListMod *m = &grown[list->nmods++];
        memset(m, 0, sizeof *m);
        m->enabled = 1;
        int ids = 0, switches = 0;
        s->p++;
        while (!s->why && more(s, '}')) {
            char *key = take_key(s);
            if (!key) return;
            if (!strcmp(key, "id")) {
                free(key);
                if (ids++) fail(s, "an entry of \"mods\" has \"id\" twice");
                else if (*s->p != '"') fail(s, "a mod's \"id\" is not a string");
                else if (!(m->id = take_string(s))) fail(s, "out of memory");
            } else if (!strcmp(key, "enabled")) {
                free(key);
                const int on = word(s, "true");
                if (switches++) fail(s, "an entry of \"mods\" has \"enabled\" twice");
                else if (!on && !word(s, "false")) fail(s, "a mod's \"enabled\" is not true or false");
                else m->enabled = on;
            } else {
                keep(&m->extra, &m->nextra, key, s);
            }
        }
        if (!s->why && !ids) fail(s, "an entry of \"mods\" has no \"id\"");
    }
}
static void read_cheats(ModsList *list, Scan *s) {
    if (*s->p != '{') {
        fail(s, "\"cheats\" is not an object");
        return;
    }
    list->has_cheats = 1;
    s->p++;
    while (!s->why && more(s, '}')) {
        char *key = take_key(s);
        if (!key) return;
        const int on = word(s, "true"), sw = on || word(s, "false");
        char *raw = sw ? NULL : take_raw(s);   /* (not a switch: kept as it is written, and the cheat has none) */
        ModsListCheat *c = sw || raw ? cheat_slot(list, key) : NULL;
        free(key);
        if (!c) {
            free(raw);
            fail(s, "out of memory");
            return;
        }
        c->value = raw, c->on = on;
    }
}
static void read_top(ModsList *list, Scan *s) {
    int had_mods = 0, had_cheats = 0;
    s->p++;
    while (!s->why && more(s, '}')) {
        char *key = take_key(s);
        if (!key) return;
        if (!strcmp(key, "mods")) {
            free(key);
            if (had_mods++) fail(s, "\"mods\" twice");
            else read_mods(list, s);
        } else if (!strcmp(key, "cheats")) {
            free(key);
            if (had_cheats++) fail(s, "\"cheats\" twice");
            else read_cheats(list, s);
        } else {
            keep(&list->extra, &list->nextra, key, s);
        }
    }
}

int mods_list_parse(ModsList *list, const char *text, size_t len, const char **why) {
    memset(list, 0, sizeof *list);
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) text += 3, len -= 3;   /* (a byte order mark: editors and .NET write one) */
    Scan s = {text, text + len, NULL, 0};
    if (value(&s)) {
        space(&s);
        if (s.p != s.end) fail(&s, "text after the list");
    }
    if (!s.why) {
        s.p = text;
        space(&s);
        if (*s.p != '{') fail(&s, "it is not a JSON object");
        else read_top(list, &s);
    }
    if (!s.why) return 1;
    mods_list_free(list);
    if (why) *why = s.why;
    return 0;
}

static void free_extra(ModsListExtra *extra, int n) {
    for (int i = 0; i < n; i++) free(extra[i].key), free(extra[i].value);
    free(extra);
}
void mods_list_free(ModsList *list) {
    for (int i = 0; i < list->nmods; i++) free(list->mods[i].id), free_extra(list->mods[i].extra, list->mods[i].nextra);
    for (int i = 0; i < list->ncheats; i++) free(list->cheats[i].key), free(list->cheats[i].value);
    free(list->mods), free(list->cheats), free_extra(list->extra, list->nextra);
    memset(list, 0, sizeof *list);
}

static ModsListCheat *cheat_of(const ModsList *list, const char *key) {   /* key's cheat: the last one with it, or NULL */
    for (int i = list->ncheats; i-- > 0;)
        if (!strcmp(list->cheats[i].key, key)) return &list->cheats[i];
    return NULL;
}
int mods_list_cheat(const ModsList *list, const char *key, int otherwise) {
    const ModsListCheat *c = cheat_of(list, key);
    return c && !c->value ? c->on : otherwise;
}
void mods_list_cheat_set(ModsList *list, const char *key, int on) {
    ModsListCheat *c = cheat_of(list, key);
    if (!c) c = cheat_slot(list, key);
    if (!c) return;
    free(c->value);
    c->value = NULL, c->on = on != 0;
}

/* ---- written: a line a mod, what was not known after what is */
typedef struct { char *p; size_t n, cap; int failed; } Text;

static void put(Text *t, const char *s, size_t n) {
    if (t->failed) return;
    if (t->n + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 512;
        while (t->n + n + 1 > cap) cap *= 2;
        char *p = (char *)realloc(t->p, cap);
        if (!p) {
            t->failed = 1;
            return;
        }
        t->p = p, t->cap = cap;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = 0;
}
static void put_text(Text *t, const char *s) { put(t, s, strlen(s)); }
static void put_string(Text *t, const char *s) {   /* a JSON string of UTF-8 text */
    put(t, "\"", 1);
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        char e[8];
        if (*q == '"' || *q == '\\') e[0] = '\\', e[1] = (char)*q, put(t, e, 2);
        else if (*q < 0x20) put(t, e, (size_t)snprintf(e, sizeof e, "\\u%04x", *q));
        else put(t, (const char *)q, 1);
    }
    put(t, "\"", 1);
}
static void put_extra(Text *t, const ModsListExtra *e) {
    put_string(t, e->key);
    put_text(t, ": ");
    put_text(t, e->value);
}

char *mods_list_write(const ModsList *list, size_t *len) {
    Text t = {NULL, 0, 0, 0};
    put_text(&t, list->nmods ? "{\n  \"mods\": [\n" : "{\n  \"mods\": [");
    for (int i = 0; i < list->nmods; i++) {
        const ModsListMod *m = &list->mods[i];
        put_text(&t, "    { \"id\": ");
        put_string(&t, m->id);
        put_text(&t, m->enabled ? ", \"enabled\": true" : ", \"enabled\": false");
        for (int k = 0; k < m->nextra; k++) put_text(&t, ", "), put_extra(&t, &m->extra[k]);
        put_text(&t, i + 1 < list->nmods ? " },\n" : " }\n");
    }
    put_text(&t, list->nmods ? "  ]" : "]");
    if (list->ncheats || list->has_cheats) {
        put_text(&t, list->ncheats ? ",\n  \"cheats\": {\n" : ",\n  \"cheats\": {");
        for (int i = 0; i < list->ncheats; i++) {
            const ModsListCheat *c = &list->cheats[i];
            put_text(&t, "    ");
            put_string(&t, c->key);
            put_text(&t, ": ");
            put_text(&t, c->value ? c->value : c->on ? "true" : "false");
            put_text(&t, i + 1 < list->ncheats ? ",\n" : "\n");
        }
        put_text(&t, list->ncheats ? "  }" : "}");
    }
    for (int i = 0; i < list->nextra; i++) put_text(&t, ",\n  "), put_extra(&t, &list->extra[i]);
    put_text(&t, "\n}\n");
    if (t.failed) {
        free(t.p);
        return NULL;
    }
    if (len) *len = t.n;
    return t.p;
}
