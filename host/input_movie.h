/* (longshot, issue 1113) Input recordings: everything the player's input does to the game, frame by frame, written down and
   given back. The logic here is the file's and the run's alone (no clock, no guest, no file): host/input_movie.c is the glue
   (the switches, the file, the log), and the places that record and play are the ones where input reaches the game:
   - host/nn_hid.c's npad_states, each read of controller No.1 (once a game frame): the button word and the sticks the game
     is given, after the script, the control bridge, the pads, the keys, the mouse as a stick, the toggles and the menus'
     filters; and the flight and free camera cheats' inputs, which the host takes from the sticks for itself;
   - host/nn_hid.c's GetSixAxisSensorStates: the motion readings (the gyroscope) the game is given;
   - host/mouse_native.c's hos_mouse_camera, the player camera's update: the native mouse camera, which turns the camera's
     own angles by the mouse's motion, not by a stick;
   - host/oot_menu.c: the mouse in the game's menus, which hands a button the focus (or the Items page its item) by calling
     the game, where it points.
   Whichever device made it, a replay gives the game the same state at the same point.

   Points are counted from the recording's start, the first read of a game frame: `f` is the game frame since then, in
   every line, by the game clock (+1 a frame at the Per frame step). The game reads controller No.1 twice in most frames,
   from its jobs, so a frame has one pad: a replay gives every read of frame f the recording's pad of frame f, whatever
   count of reads a frame of its own has (each frame's reads are written down, `frames` lines, and a different count is
   counted). The camera's updates, the motion reads and Link's updates are held to the recording by their frame and `i`,
   their call within it (0 the frame's first): one more or one fewer of them in a frame of a replay (the jobs' race
   again) leaves the frames after it where they were, where a count from the start shifted every one after it. Their
   counts from the start (`n` of a cam, view or mark line, `c` of a gyro line) are written too, for people and the log.

   The file is ls-input-recording/1, the format of mooki's ls-workbench (play/pilot/inputlog.py): JSON lines, the first a
   header ({"format":"ls-input-recording/1","fps":60,...}), then one object a line with its `f` and `cmd`. The game writes
   these commands, which that tool's converters pass over:
     {"f":F,"cmd":"pad","frames":N,"bits":"0x...","buttons":"A+ZL","lx":0,"ly":32767,"rx":0,"ry":0}
         the word (NpadButton bits, as the game reads them) and the sticks (-32767..32767) for N frames from frame F;
         `buttons` is only for people (playback reads `bits`). With the flight or free camera cheat on, also "fl" and "fc"
         (the cheats' packed axes, hex) and "fh" (the free camera's height)
     {"f":F,"cmd":"cam","n":M,"i":I,"native":1,"free":1,"held":0,"dyaw":Y,"dpitch":P}
         the native mouse camera at the camera's M-th update, its I-th in frame F: whether it was on, whether the game
         left the camera free, whether a stick turned it (its angles then the mouse's new start), and the degrees the mouse
         turned it by (only when it did). Written when one of the three changed or the mouse moved; a replay takes the last
         line at or before its own (F, I), and the turn only at that (F, I)
     {"f":F,"cmd":"gyro","c":C,"i":I,"live":L,"top":T,"upto":U}
         the C-th motion read, the I-th of frame F: L live readings, the newest numbered T (then T-1, ...), among the
         first U motion lines
     {"f":F,"cmd":"motion","n":N,"dt":D,"v":[18 floats]}
         one motion reading (as host/hos_motion.h's HosMotionSample: acceleration, rates, rotation, direction), once
     {"f":F,"cmd":"focus","screen":"0x...","button":"0x..."}, {"f":F,"cmd":"itemkey","screen":"0x...","key":"..."}
         the menu mouse's focus change, or Items page choice, on that screen, given again on the screen's first update
         in frame F (a focus only to one of that screen's buttons then, an item only one of Key Items')
     {"f":F,"cmd":"frames","count":C,"reads":R,"read":N}
         C game frames from frame F, each of R controller reads, the first of them read N since the start
     {"f":F,"cmd":"view","n":M,"i":I,"yaw":Y,"pitch":P,"w":"<hex>"}
         the player camera as its M-th update (the I-th of frame F) began, written when it changed: its state block, the
         14 words from +0x178
         (the pitch limits, the pitch it eases to at +0x188, the pitch at +0x18c, the yaw at +0x1a4; host/mouse_native.c
         names them) as hex, and the yaw and pitch in degrees for people: the second fingerprint, the camera's, held to
         exactly as the marks are, and what HOS_INPUT_SYNC puts back. "in" (when the object is there): the camera's
         inputs, the 13 words from +0x45c of the object at 0x84c9ede0 that the camera's update makes the pitch it eases
         to from (f_80644c80 reads +0x45c..+0x464 and +0x484..+0x48c; the renderer's sun shadow fits a box at
         +0x478..+0x48c of an object like it each frame, rendering.md): told apart from the camera's own state, and put
         back before the camera reads them
     {"f":F,"cmd":"mark","n":K,"i":I,"pos":[x,y,z]}
         Link's position after his K-th update since the start, the I-th of frame F (written when he moved): the run's
         fingerprint; a replay compares its own update (F, I) with the last mark at or before it, exactly
     {"f":F,"cmd":"action","a":A}
         a Debug tools action the game took at Link's update in frame F (host/hos_main.c oot_update_player_cheats: from
         the Insert menu, HOS_INPUT_LIVE's !cheat or the control bridge): 2 Mark, 3 Return, 100-123 the hour, 200-348 a
         pouch, 999 the age, 1000 + i a teleport, 3000 + an item's edit. A replay takes the live ones no more and gives
         these at their frame
     {"f":F,"cmd":"option","id":I,"v":V}
         a Debug tools row the host reads (MOVIE_OPTION_IDS: heart protection, flight, the free camera, Link's speed,
         damage taken) as it was from frame F: each at frame 0, then at each change. A replay answers rx_option with these
     {"f":F,"cmd":"anchor","n":N,"gap":G}
         Link's first update after G frames without one (a load: a teleport, a door, the age), the N-th since the start,
         in frame F. A replay re-bases its frames at its own N-th anchor so that it is frame F, and waits at frame F when
         its load takes longer: a load that takes more or fewer frames does not move the input after it
     {"f":F,"cmd":"note","text":"..."}
         something the recording does not hold
   Floats are written with %.9g: read back, the value is the same float; a NaN or an infinity (or a number past a float's
   range) refuses the line, as a pitch of NaN put back by HOS_INPUT_SYNC would be. A recording's frames are contiguous
   `pad` runs; a frame no run covers is a neutral pad (no button, sticks centred). */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOVIE_FORMAT "ls-input-recording/1"
#define MOVIE_RUN_MAX 600u   /* (a run is written at least every 600 frames: a crash loses 10 s at most) */
#define MOVIE_MOTION_FLOATS 18
#define MOVIE_CALLS_MAX 1000   /* (a hook's calls in one frame, at most: `i` past it refuses the line) */
#define MOVIE_ANCHOR_GAP 10      /* (frames without one of Link's updates that make the next an anchor: a load) */
#define MOVIE_ANCHOR_SHIFT 900   /* (frames a replay's anchor may be from the recording's and still be it: 15 s of load) */
#define MOVIE_ANCHOR_WAIT 900    /* (frames a replay waits at an anchor's frame for its own load to end) */
/* (the Debug tools rows the host reads that change what the game does: gpu/vk/vk_api.h HOS_OPT_CHEAT_HEARTS 32, _FLIGHT 33,
   _CAMERA 35, _SPEED 36 and HOS_OPT_DAMAGE_TAKEN 55) */
#define MOVIE_OPTION_IDS {32, 33, 35, 36, 55}
#define MOVIE_OPTIONS 5

/* (a point of the run: frame f, the hook's call i within it; in the order they come) */
static inline uint64_t movie_key(uint32_t f, uint32_t i) {
    return (uint64_t)f << 32 | i;
}

/* (a hook's calls counted within their frame: i 0 at each new frame) */
typedef struct {
    uint32_t f, i;
    int have;
} MovieAt;

static inline uint32_t movie_at_step(MovieAt *a, uint32_t f) {
    if (a->have && a->f == f) return ++a->i;
    a->f = f, a->i = 0, a->have = 1;
    return 0;
}

typedef struct {
    uint64_t bits;
    int32_t stick[4];   /* (lx, ly, rx, ry) */
    uint64_t flight;    /* (the flight cheat's packed stick and climb, nn_hid.c flight_input; 0 without it) */
    uint64_t axes;      /* (the free camera cheat's packed sticks, free_camera_axes) */
    int32_t height;     /* (the free camera's height: R up, L down) */
} MoviePad;

static inline int movie_pad_same(const MoviePad *a, const MoviePad *b) {
    return a->bits == b->bits && !memcmp(a->stick, b->stick, sizeof a->stick) && a->flight == b->flight &&
           a->axes == b->axes && a->height == b->height;
}

/* (NpadButton bits 0..15 as host/nn_hid.c's B_ names them; others as b<n>) */
static inline void movie_button_names(uint64_t bits, char *out, size_t cap) {
    static const char *names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP",
                                    "RIGHT", "DOWN"};
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < 64; i++) {
        if (!(bits >> i & 1)) continue;
        char b[8];
        const char *name = i < 16 ? names[i] : (snprintf(b, sizeof b, "b%d", i), b);
        const int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s", n ? "+" : "", name);
        if (w < 0 || (size_t)w >= (cap > n ? cap - n : 0)) break;
        n += (size_t)w;
    }
}

/* the line of a run (newline included); its length, 0 when it does not fit */
static inline int movie_pad_line(char *out, size_t cap, uint32_t f, uint32_t frames, const MoviePad *p) {
    char names[128], cheats[96] = "";
    movie_button_names(p->bits, names, sizeof names);
    if (p->flight || p->axes || p->height)
        snprintf(cheats, sizeof cheats, ",\"fl\":\"0x%llx\",\"fc\":\"0x%llx\",\"fh\":%d", (unsigned long long)p->flight,
                 (unsigned long long)p->axes, (int)p->height);
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"pad\",\"frames\":%u,\"bits\":\"0x%llx\",\"buttons\":\"%s\",\"lx\":%d,"
                           "\"ly\":%d,\"rx\":%d,\"ry\":%d%s}\n", f, frames, (unsigned long long)p->bits, names, (int)p->stick[0],
                           (int)p->stick[1], (int)p->stick[2], (int)p->stick[3], cheats);
    return n > 0 && (size_t)n < cap ? n : 0;
}

static inline int movie_mark_line(char *out, size_t cap, uint32_t f, uint32_t i, uint32_t k, const float pos[3]) {
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"mark\",\"n\":%u,\"i\":%u,\"pos\":[%.9g,%.9g,%.9g]}\n", f, k, i,
                           (double)pos[0], (double)pos[1], (double)pos[2]);
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* (a note's text, or an item key: what JSON needs escaped is dropped, the rest kept) */
static inline void movie_clean(const char *text, char *t, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; text[i] && j + 1 < cap; i++)
        if (text[i] != '"' && text[i] != '\\' && (unsigned char)text[i] >= 0x20) t[j++] = text[i];
    t[j] = 0;
}

static inline int movie_note_line(char *out, size_t cap, uint32_t f, const char *text) {
    char t[200];
    movie_clean(text, t, sizeof t);
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"note\",\"text\":\"%s\"}\n", f, t);
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* ---- recording: one open run, closed when the pad changes or it reaches MOVIE_RUN_MAX reads ---- */

typedef struct {
    MoviePad pad;
    uint32_t f, frames;
    int open;
} MovieRun;

/* frame `f`'s pad; returns 1 and fills *done when the run before it ended */
static inline int movie_rec_step(MovieRun *r, uint32_t f, const MoviePad *p, MovieRun *done) {
    if (r->open && movie_pad_same(&r->pad, p) && r->frames < MOVIE_RUN_MAX && f == r->f + r->frames) {
        r->frames++;
        return 0;
    }
    const int ended = r->open;
    if (ended) *done = *r;
    r->pad = *p, r->f = f, r->frames = 1, r->open = 1;
    return ended;
}

/* ---- the game's frames: their reads, run-length ---- */

typedef struct {
    uint32_t f, n, count, reads;   /* (f: the read frame n began at, the line's "read"; the line's "f" is n) */
} MovieFrames;

typedef struct {
    MovieFrames run;
    int open;
} MovieFramesRec;

/* frame n ended with `reads` reads from read f (since the start); 1 and *done when the run before it ended */
static inline int movie_frames_step(MovieFramesRec *r, uint32_t n, uint32_t f, uint32_t reads, MovieFrames *done) {
    MovieFrames *o = &r->run;
    if (r->open && reads == o->reads && n == o->n + o->count && f == o->f + o->count * o->reads) {
        o->count++;
        return 0;
    }
    const int ended = r->open;
    if (ended) *done = *o;
    o->f = f, o->n = n, o->count = 1, o->reads = reads, r->open = 1;
    return ended;
}

static inline int movie_frames_line(char *out, size_t cap, const MovieFrames *m) {
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"frames\",\"count\":%u,\"reads\":%u,\"read\":%u}\n", m->n, m->count,
                           m->reads, m->f);
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* ---- the native mouse camera ---- */

typedef struct {
    uint32_t n, f;
    uint8_t native, free, held, moved;
    float dyaw, dpitch;   /* (subtracted from the yaw, added to the pitch, as hos_mouse_camera does) */
    uint32_t i;           /* (the camera's update within frame f) */
} MovieCam;

typedef struct {
    MovieCam last;
    int have;
} MovieCamRec;

/* the camera's update m; 1 (and its line) when it is to be written: the first, a change of native, free or held, a turn */
static inline int movie_cam_rec(MovieCamRec *r, const MovieCam *c, char *out, size_t cap, int *len) {
    *len = 0;
    if (r->have && !c->moved && c->native == r->last.native && c->free == r->last.free && c->held == r->last.held) return 0;
    r->last = *c, r->have = 1;
    char turn[64] = "";
    if (c->moved) snprintf(turn, sizeof turn, ",\"dyaw\":%.9g,\"dpitch\":%.9g", (double)c->dyaw, (double)c->dpitch);
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"cam\",\"n\":%u,\"i\":%u,\"native\":%d,\"free\":%d,\"held\":%d%s}\n",
                           c->f, c->n, c->i, c->native, c->free, c->held, turn);
    *len = n > 0 && (size_t)n < cap ? n : 0;
    return 1;
}

#define MOVIE_VIEW_AT 0x178        /* (the camera's state block: from +0x178, 14 words) */
#define MOVIE_VIEW_WORDS 14
#define MOVIE_VIEW_PITCH ((0x18c - MOVIE_VIEW_AT) / 4)
#define MOVIE_VIEW_YAW ((0x1a4 - MOVIE_VIEW_AT) / 4)
#define MOVIE_VIEW_INPUTS 0x84c9ede0ULL   /* (the guest word holding the object the camera's inputs are in) */
#define MOVIE_VIEW_IN_AT 0x45c
#define MOVIE_VIEW_IN_WORDS 13

typedef struct {
    uint32_t n, f, i;   /* (its update since the start, its frame, its update within the frame) */
    float yaw, pitch;
    uint32_t w[MOVIE_VIEW_WORDS];
    int has_w;   /* (a recording from before the block: yaw and pitch only) */
    uint32_t in[MOVIE_VIEW_IN_WORDS];
    int has_in;  /* (the camera's inputs: their object was there) */
} MovieView;

static inline float movie_word_float(uint32_t w) {
    float f;
    memcpy(&f, &w, sizeof f);
    return f;
}

/* the view of a camera block as it is, with its inputs (NULL: not there) */
static inline MovieView movie_view_of(uint32_t n, uint32_t f, const uint32_t w[MOVIE_VIEW_WORDS], const uint32_t *in) {
    MovieView v;
    memset(&v, 0, sizeof v);
    v.n = n, v.f = f, v.has_w = 1;
    memcpy(v.w, w, sizeof v.w);
    if (in) memcpy(v.in, in, sizeof v.in), v.has_in = 1;
    v.yaw = movie_word_float(w[MOVIE_VIEW_YAW]), v.pitch = movie_word_float(w[MOVIE_VIEW_PITCH]);
    return v;
}

/* whether the camera's inputs are the view's (1 when either side has none) */
static inline int movie_view_inputs_same(const MovieView *v, const uint32_t *in) {
    return !v->has_in || !in || !memcmp(v->in, in, sizeof v->in);
}

/* whether a camera block is the view's (its block, or yaw and pitch for an older recording) */
static inline int movie_view_same(const MovieView *v, const uint32_t w[MOVIE_VIEW_WORDS]) {
    if (v->has_w) return !memcmp(v->w, w, sizeof v->w);
    return v->yaw == movie_word_float(w[MOVIE_VIEW_YAW]) && v->pitch == movie_word_float(w[MOVIE_VIEW_PITCH]);
}

/* (a view without its block, has_w 0: the yaw and pitch alone, as a route's views are: no guest words in it) */
static inline int movie_view_line(char *out, size_t cap, const MovieView *v) {
    char hex[MOVIE_VIEW_WORDS * 8 + 16] = "", in[MOVIE_VIEW_IN_WORDS * 8 + 16] = "";
    if (v->has_w) {
        snprintf(hex, sizeof hex, ",\"w\":\"");
        for (int i = 0; i < MOVIE_VIEW_WORDS; i++) snprintf(hex + 6 + 8 * i, 9, "%08x", (unsigned)v->w[i]);
        snprintf(hex + 6 + 8 * MOVIE_VIEW_WORDS, 2, "\"");
    }
    if (v->has_in) {
        snprintf(in, sizeof in, ",\"in\":\"");
        for (int i = 0; i < MOVIE_VIEW_IN_WORDS; i++) snprintf(in + 7 + 8 * i, 9, "%08x", (unsigned)v->in[i]);
        snprintf(in + 7 + 8 * MOVIE_VIEW_IN_WORDS, 2, "\"");
    }
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"view\",\"n\":%u,\"i\":%u,\"yaw\":%.9g,\"pitch\":%.9g%s%s}\n",
                           v->f, v->n, v->i, (double)v->yaw, (double)v->pitch, hex, in);
    return n > 0 && (size_t)n < cap ? n : 0;
}

static inline int movie_json_text(const char *line, const char *key, char *out, size_t cap);
/* (count hex words of a JSON string after "key":, 0 when absent, -1 malformed) */
static inline int movie_json_words(const char *line, const char *key, uint32_t *w, int count) {
    char hex[16 * 8 + 2];
    if (!movie_json_text(line, key, hex, sizeof hex)) return 0;
    if ((int)strlen(hex) != count * 8) return -1;
    for (int i = 0; i < count; i++) {
        char word[9];
        memcpy(word, hex + 8 * i, 8), word[8] = 0;
        char *end;
        w[i] = (uint32_t)strtoul(word, &end, 16);
        if (*end) return -1;
    }
    return 1;
}

/* ---- motion ---- */

typedef struct {
    int64_t n, dt;
    float v[MOVIE_MOTION_FLOATS];
} MovieMotion;

typedef struct {
    uint32_t c, f, live, upto;
    int64_t top;
    uint32_t i;   /* (the motion read within frame f) */
} MovieGyro;

typedef struct {
    int64_t top;    /* (the newest reading written: one numbered above it is new) */
    int have;
    uint32_t written;   /* (motion lines so far) */
} MovieGyroRec;

static inline int movie_motion_line(char *out, size_t cap, uint32_t f, const MovieMotion *m) {
    int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"motion\",\"n\":%lld,\"dt\":%lld,\"v\":[", f, (long long)m->n, (long long)m->dt);
    for (int i = 0; i < MOVIE_MOTION_FLOATS && n > 0 && (size_t)n < cap; i++)
        n += snprintf(out + n, cap - (size_t)n, "%s%.9g", i ? "," : "", (double)m->v[i]);
    if (n > 0 && (size_t)n < cap) n += snprintf(out + n, cap - (size_t)n, "]}\n");
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* the readings of one motion read (newest first) not written yet: how many, from the oldest new one (*from) */
static inline int movie_gyro_new(MovieGyroRec *r, const MovieMotion *h, int live, int *from) {
    int k = 0;
    if (live > 0 && (!r->have || h[0].n < r->top))   /* (the first, or numbers started again: all of them) */
        k = live;
    else
        while (k < live && h[k].n > r->top) k++;
    *from = k - 1;
    if (live > 0) r->top = h[0].n, r->have = 1;
    return k;
}

static inline int movie_gyro_line(char *out, size_t cap, const MovieGyro *g) {
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"gyro\",\"c\":%u,\"i\":%u,\"live\":%u,\"top\":%lld,\"upto\":%u}\n",
                           g->f, g->c, g->i, g->live, (long long)g->top, g->upto);
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* ---- the menu mouse ---- */

typedef struct {
    uint32_t f;
    uint8_t kind, used;   /* (kind 1: focus, 2: item key) */
    uint64_t screen, button;
    char key[64];
} MovieMenu;

static inline int movie_menu_line(char *out, size_t cap, const MovieMenu *m) {
    char key[64];
    movie_clean(m->key, key, sizeof key);
    const int n = m->kind == 1 ?
        snprintf(out, cap, "{\"f\":%u,\"cmd\":\"focus\",\"screen\":\"0x%llx\",\"button\":\"0x%llx\"}\n", m->f,
                 (unsigned long long)m->screen, (unsigned long long)m->button) :
        snprintf(out, cap, "{\"f\":%u,\"cmd\":\"itemkey\",\"screen\":\"0x%llx\",\"key\":\"%s\"}\n", m->f,
                 (unsigned long long)m->screen, key);
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* (what is recorded or played now: host/input_movie.c hos_input_movie_status, for the Insert menu's rows and the control
   bridge's status) */
typedef struct {
    int mode;   /* (0 nothing, 1 recording, 2 playing) */
    int started, held, diverged;
    uint32_t frame, end, link_apart, link_checked, syncs, anchors, anchors_wanted;
    float largest;   /* (metres: Link's largest correction) */
    char path[512];  /* (the file recorded or played; nothing: the last recording made) */
    char why[128];   /* (why the last playback started at run time ended before its start: "" none) */
} HosInputMovieStatus;

/* (a clock for the host's own decisions by time (host/oot_ocb_hold.c's hold, host/oot_tex_compact.c's gap): the wall clock,
   or while a recording is recorded or played the game clock's time (1/60 s a frame), so a decision lands on the same frame
   of the replay as of the recording; kept going on from where it was when it changes from one to the other. One a caller,
   under the caller's lock: host/input_movie.c hos_input_movie_time) */
typedef struct {
    int game;               /* (the last value was the game clock's) */
    int64_t offset, last;   /* (ns: added to the clock to go on from last) */
} HosMovieTime;

static inline int64_t movie_time_step(HosMovieTime *t, int game, int64_t raw) {
    if (game != t->game) t->offset = t->last ? t->last - raw : 0, t->game = game;
    int64_t v = raw + t->offset;
    if (v < t->last) v = t->last;   /* (never back) */
    t->last = v;
    return v;
}

/* ---- actions, options and anchors ---- */

typedef struct {
    uint32_t f;
    int32_t a;
    uint8_t used;
} MovieAction;

typedef struct {
    uint32_t f;
    int32_t id, v;
} MovieOption;

typedef struct {
    uint32_t f, n, gap;
} MovieAnchor;

static inline int movie_action_line(char *out, size_t cap, uint32_t f, int a) {
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"action\",\"a\":%d}\n", f, a);
    return n > 0 && (size_t)n < cap ? n : 0;
}

static inline int movie_option_line(char *out, size_t cap, uint32_t f, int id, int v) {
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"option\",\"id\":%d,\"v\":%d}\n", f, id, v);
    return n > 0 && (size_t)n < cap ? n : 0;
}

static inline int movie_anchor_line(char *out, size_t cap, const MovieAnchor *a) {
    const int n = snprintf(out, cap, "{\"f\":%u,\"cmd\":\"anchor\",\"n\":%u,\"gap\":%u}\n", a->f, a->n, a->gap);
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* (the slot of an option id in MOVIE_OPTION_IDS, -1 for one a recording does not hold) */
static inline int movie_option_slot(int id) {
    static const int ids[MOVIE_OPTIONS] = MOVIE_OPTION_IDS;
    for (int i = 0; i < MOVIE_OPTIONS; i++)
        if (ids[i] == id) return i;
    return -1;
}

/* Where a replay's frame goes at its own anchor: the recording's anchor frame, when the two are within MOVIE_ANCHOR_SHIFT
   of each other and the replay is not past it (1, *to); 0 when the recording has no such anchor, or one too far to be
   this load */
static inline int movie_anchor_rebase(const MovieAnchor *a, size_t na, uint32_t n, uint32_t here, uint32_t *to) {
    if (n >= na) return 0;
    const int64_t shift = (int64_t)a[n].f - (int64_t)here;
    /* (back by at most MOVIE_ANCHOR_GAP: a replay whose frame was taken from the clock before its hold applied (Link's update
       before the frame's first read, or no read during the load) is a few frames past the anchor's; further back is not it) */
    if (shift > MOVIE_ANCHOR_SHIFT || shift < -MOVIE_ANCHOR_GAP) return 0;
    *to = a[n].f;
    return 1;
}

/* Whether a replay at frame `next` has passed the recording's next anchor (number n) without one of its own: its load
   takes longer, and it is to wait at the anchor's frame (1, *at) */
static inline int movie_anchor_hold(const MovieAnchor *a, size_t na, uint32_t n, uint32_t next, uint32_t *at) {
    if (n >= na || next <= a[n].f) return 0;
    *at = a[n].f;
    return 1;
}

/* ---- playback ---- */

typedef struct {
    uint32_t f, frames;
    MoviePad pad;
} MovieEntry;

typedef struct {
    uint32_t k, f;
    float pos[3];
    uint32_t i;   /* (Link's update within frame f) */
} MovieMark;

typedef struct {
    MovieEntry *e;
    size_t ne, cap_e;
    MovieMark *m;
    size_t nm, cap_m;
    MovieCam *cam;
    size_t ncam, cap_cam;
    MovieGyro *gyro;
    size_t ngyro, cap_gyro;
    MovieMotion *motion;
    size_t nmotion, cap_motion;
    MovieMenu *menu;
    size_t nmenu, cap_menu, menu_cursor;
    MovieFrames *frames;
    size_t nframes, cap_frames;
    MovieView *view;
    size_t nview, cap_view;
    MovieAction *action;
    size_t naction, cap_action, action_cursor;
    MovieOption *option;
    size_t noption, cap_option, option_cursor;
    MovieAnchor *anchor;
    size_t nanchor, cap_anchor;
    size_t cursor;       /* (the entry the last lookup ended at: frames come in order) */
    uint32_t end;        /* (one past the last frame any run covers) */
    int header;          /* (the first line was the format's header) */
    char from[16];       /* (the header's "from": where frame 0 is, "boot" or "play") */
    char map[64];        /* (the header's "map": where Link was at the start, "" when not known) */
    char age[16];        /* (the header's "age": "child" or "adult" at the start, "" when not known) */
    uint32_t skipped;    /* (lines of other commands: press, stick, sequence, live, note) */
} MovieTape;

/* (the value after "key": in a one-line JSON object, or NULL) */
static inline const char *movie_json_at(const char *line, const char *key) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static inline int movie_json_long(const char *line, const char *key, long long *v) {
    const char *p = movie_json_at(line, key);
    if (!p) return 0;
    char *end;
    *v = strtoll(p, &end, 10);
    return end != p;
}

static inline int movie_json_float(const char *line, const char *key, float *v) {
    const char *p = movie_json_at(line, key);
    if (!p) return 0;
    char *end;
    *v = strtof(p, &end);
    return end != p && isfinite(*v);   /* (strtof's overflow is an infinity: refused too) */
}

static inline int movie_json_text(const char *line, const char *key, char *out, size_t cap) {
    const char *p = movie_json_at(line, key);
    if (!p || *p != '"' || !cap) return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return *p == '"';
}

static inline int movie_json_hex(const char *line, const char *key, uint64_t *v) {
    char t[24];
    if (!movie_json_text(line, key, t, sizeof t)) return 0;
    char *end;
    *v = strtoull(t, &end, 16);
    return end != t;
}

/* (n floats of a JSON array after "key":) */
static inline int movie_json_floats(const char *line, const char *key, float *v, int n) {
    const char *p = movie_json_at(line, key);
    if (!p || *p != '[') return 0;
    p++;
    for (int i = 0; i < n; i++) {
        char *end;
        v[i] = strtof(p, &end);
        if (end == p || !isfinite(v[i])) return 0;
        p = end;
        while (*p == ' ' || *p == ',') p++;
    }
    return 1;
}

static inline int movie_push(void **arr, size_t *n, size_t *cap, size_t size, const void *item) {
    if (*n == *cap) {
        const size_t c = *cap ? *cap * 2 : 256;
        void *grown = realloc(*arr, c * size);
        if (!grown) return 0;
        *arr = grown, *cap = c;
    }
    memcpy((char *)*arr + *n * size, item, size);
    (*n)++;
    return 1;
}

static inline int movie_long_in(const char *line, const char *key, long long lo, long long hi, long long *v) {
    return movie_json_long(line, key, v) && *v >= lo && *v <= hi;
}

/* (a line's "i", its hook's call within the frame: 0 when it has none; -1 when out of range) */
static inline int movie_call_of(const char *line, uint32_t *i) {
    long long v = 0;
    if (movie_json_at(line, "i") && !movie_long_in(line, "i", 0, MOVIE_CALLS_MAX, &v)) return -1;
    *i = (uint32_t)v;
    return 0;
}

/* one line of the file: 1 taken (the header, a command of the game's, or one passed over), 0 blank, -1 not this format */
static inline int movie_parse_line(MovieTape *t, const char *line) {
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
    if (!*line) return 0;
    if (*line != '{') return -1;
    char text[64];
    if (!t->header) {
        if (!movie_json_text(line, "format", text, sizeof text) || strcmp(text, MOVIE_FORMAT)) return -1;
        t->header = 1;
        if (!movie_json_text(line, "from", t->from, sizeof t->from)) snprintf(t->from, sizeof t->from, "boot");
        if (!movie_json_text(line, "map", t->map, sizeof t->map)) t->map[0] = 0;
        if (!movie_json_text(line, "age", t->age, sizeof t->age)) t->age[0] = 0;
        return 1;
    }
    long long f;
    if (!movie_json_text(line, "cmd", text, sizeof text) || !movie_long_in(line, "f", 0, 0x7fffffff, &f)) return -1;
    if (!strcmp(text, "pad")) {
        long long frames, v[4];
        static const char *axes[4] = {"lx", "ly", "rx", "ry"};
        MovieEntry e;
        memset(&e, 0, sizeof e);
        if (!movie_long_in(line, "frames", 1, 0x7fffffff, &frames) || !movie_json_hex(line, "bits", &e.pad.bits)) return -1;
        e.f = (uint32_t)f, e.frames = (uint32_t)frames;
        for (int i = 0; i < 4; i++) {
            if (!movie_json_long(line, axes[i], &v[i])) v[i] = 0;
            e.pad.stick[i] = (int32_t)(v[i] < -32768 ? -32768 : v[i] > 32767 ? 32767 : v[i]);
        }
        long long h = 0;
        movie_json_hex(line, "fl", &e.pad.flight);
        movie_json_hex(line, "fc", &e.pad.axes);
        if (movie_json_long(line, "fh", &h)) e.pad.height = (int32_t)(h < -1 ? -1 : h > 1 ? 1 : h);
        if (t->ne && e.f < t->e[t->ne - 1].f + t->e[t->ne - 1].frames) return -1;   /* (runs in order, not overlapping) */
        if (!movie_push((void **)&t->e, &t->ne, &t->cap_e, sizeof e, &e)) return -1;
        if (e.f + e.frames > t->end) t->end = e.f + e.frames;
        return 1;
    }
    if (!strcmp(text, "mark")) {
        long long k;
        MovieMark m;
        if (!movie_long_in(line, "n", 0, 0x7fffffff, &k) || !movie_json_floats(line, "pos", m.pos, 3) || movie_call_of(line, &m.i))
            return -1;
        m.k = (uint32_t)k, m.f = (uint32_t)f;
        if (t->nm && (m.k < t->m[t->nm - 1].k || movie_key(m.f, m.i) < movie_key(t->m[t->nm - 1].f, t->m[t->nm - 1].i))) return -1;
        return movie_push((void **)&t->m, &t->nm, &t->cap_m, sizeof m, &m) ? 1 : -1;
    }
    if (!strcmp(text, "cam")) {
        long long n, native, free, held;
        MovieCam c;
        memset(&c, 0, sizeof c);
        if (!movie_long_in(line, "n", 0, 0x7fffffff, &n) || !movie_long_in(line, "native", 0, 1, &native) ||
            !movie_long_in(line, "free", 0, 1, &free) || !movie_long_in(line, "held", 0, 1, &held))
            return -1;
        c.n = (uint32_t)n, c.f = (uint32_t)f, c.native = (uint8_t)native, c.free = (uint8_t)free, c.held = (uint8_t)held;
        const int yaw = movie_json_float(line, "dyaw", &c.dyaw), pitch = movie_json_float(line, "dpitch", &c.dpitch);
        if (yaw != pitch || movie_call_of(line, &c.i)) return -1;
        c.moved = (uint8_t)yaw;
        if (t->ncam && (c.n <= t->cam[t->ncam - 1].n || movie_key(c.f, c.i) < movie_key(t->cam[t->ncam - 1].f, t->cam[t->ncam - 1].i)))
            return -1;
        return movie_push((void **)&t->cam, &t->ncam, &t->cap_cam, sizeof c, &c) ? 1 : -1;
    }
    if (!strcmp(text, "motion")) {
        long long n, dt;
        MovieMotion m;
        if (!movie_json_long(line, "n", &n) || !movie_json_long(line, "dt", &dt) ||
            !movie_json_floats(line, "v", m.v, MOVIE_MOTION_FLOATS))
            return -1;
        m.n = n, m.dt = dt;
        return movie_push((void **)&t->motion, &t->nmotion, &t->cap_motion, sizeof m, &m) ? 1 : -1;
    }
    if (!strcmp(text, "gyro")) {
        long long c, live, top, upto;
        MovieGyro g;
        if (!movie_long_in(line, "c", 0, 0x7fffffff, &c) || !movie_long_in(line, "live", 1, 64, &live) ||
            !movie_json_long(line, "top", &top) || !movie_long_in(line, "upto", 0, 0x7fffffff, &upto) ||
            (size_t)upto > t->nmotion)
            return -1;
        g.c = (uint32_t)c, g.f = (uint32_t)f, g.live = (uint32_t)live, g.top = top, g.upto = (uint32_t)upto;
        if (movie_call_of(line, &g.i)) return -1;
        if (t->ngyro && (g.c <= t->gyro[t->ngyro - 1].c || movie_key(g.f, g.i) < movie_key(t->gyro[t->ngyro - 1].f, t->gyro[t->ngyro - 1].i)))
            return -1;
        return movie_push((void **)&t->gyro, &t->ngyro, &t->cap_gyro, sizeof g, &g) ? 1 : -1;
    }
    if (!strcmp(text, "view")) {
        long long n;
        MovieView v;
        memset(&v, 0, sizeof v);
        if (!movie_long_in(line, "n", 0, 0x7fffffff, &n) || !movie_json_float(line, "yaw", &v.yaw) ||
            !movie_json_float(line, "pitch", &v.pitch))
            return -1;
        v.n = (uint32_t)n, v.f = (uint32_t)f;
        const int w = movie_json_words(line, "w", v.w, MOVIE_VIEW_WORDS), in = movie_json_words(line, "in", v.in, MOVIE_VIEW_IN_WORDS);
        if (w < 0 || in < 0 || movie_call_of(line, &v.i)) return -1;   /* (the block 14 words, the inputs 13: or the line is not this format's) */
        v.has_w = w, v.has_in = in;
        /* (the block's floats HOS_INPUT_SYNC writes back, the pitch limits, the pitch it eases to, the pitch and the yaw:
           finite, as the angles are) */
        static const int floats[] = {0, 1, (0x188 - MOVIE_VIEW_AT) / 4, MOVIE_VIEW_PITCH, MOVIE_VIEW_YAW};
        for (unsigned i = 0; w && i < sizeof floats / sizeof *floats; i++)
            if (!isfinite(movie_word_float(v.w[floats[i]]))) return -1;
        if (t->nview && (v.n <= t->view[t->nview - 1].n || movie_key(v.f, v.i) < movie_key(t->view[t->nview - 1].f, t->view[t->nview - 1].i)))
            return -1;
        return movie_push((void **)&t->view, &t->nview, &t->cap_view, sizeof v, &v) ? 1 : -1;
    }
    if (!strcmp(text, "frames")) {
        long long read, count, reads;
        MovieFrames m;
        if (!movie_long_in(line, "read", 0, 0x7fffffff, &read) || !movie_long_in(line, "count", 1, 0x7fffffff, &count) ||
            !movie_long_in(line, "reads", 0, 1000, &reads))
            return -1;
        m.f = (uint32_t)read, m.n = (uint32_t)f, m.count = (uint32_t)count, m.reads = (uint32_t)reads;
        if (t->nframes && m.n < t->frames[t->nframes - 1].n + t->frames[t->nframes - 1].count) return -1;
        return movie_push((void **)&t->frames, &t->nframes, &t->cap_frames, sizeof m, &m) ? 1 : -1;
    }
    if (!strcmp(text, "focus") || !strcmp(text, "itemkey")) {
        MovieMenu m;
        memset(&m, 0, sizeof m);
        m.kind = !strcmp(text, "focus") ? 1 : 2;
        m.f = (uint32_t)f;
        if (!movie_json_hex(line, "screen", &m.screen)) return -1;
        if (m.kind == 1 ? !movie_json_hex(line, "button", &m.button) : !movie_json_text(line, "key", m.key, sizeof m.key))
            return -1;
        if (t->nmenu && m.f < t->menu[t->nmenu - 1].f) return -1;
        return movie_push((void **)&t->menu, &t->nmenu, &t->cap_menu, sizeof m, &m) ? 1 : -1;
    }
    if (!strcmp(text, "action")) {
        long long a;
        MovieAction m;
        memset(&m, 0, sizeof m);
        if (!movie_long_in(line, "a", 1, 0x7fffffff, &a)) return -1;
        m.f = (uint32_t)f, m.a = (int32_t)a;
        if (t->naction && m.f < t->action[t->naction - 1].f) return -1;
        return movie_push((void **)&t->action, &t->naction, &t->cap_action, sizeof m, &m) ? 1 : -1;
    }
    if (!strcmp(text, "option")) {
        long long id, v;
        MovieOption m;
        if (!movie_long_in(line, "id", 0, 0xffff, &id) || !movie_long_in(line, "v", -0x7fffffff, 0x7fffffff, &v) ||
            movie_option_slot((int)id) < 0)
            return -1;
        m.f = (uint32_t)f, m.id = (int32_t)id, m.v = (int32_t)v;
        if (t->noption && m.f < t->option[t->noption - 1].f) return -1;
        return movie_push((void **)&t->option, &t->noption, &t->cap_option, sizeof m, &m) ? 1 : -1;
    }
    if (!strcmp(text, "anchor")) {
        long long n, gap;
        MovieAnchor m;
        if (!movie_long_in(line, "n", 0, 0x7fffffff, &n) || !movie_long_in(line, "gap", 1, 0x7fffffff, &gap)) return -1;
        m.f = (uint32_t)f, m.n = (uint32_t)n, m.gap = (uint32_t)gap;
        if (m.n != t->nanchor || (t->nanchor && m.f < t->anchor[t->nanchor - 1].f)) return -1;   /* (0, 1, 2, ... in order) */
        return movie_push((void **)&t->anchor, &t->nanchor, &t->cap_anchor, sizeof m, &m) ? 1 : -1;
    }
    t->skipped++;   /* (note, press, stick, sequence, live: none of them the game's input as it reached it) */
    return 1;
}

/* frame f's pad (frames come in order: a cursor); 0 once f is past the recording's end */
static inline int movie_tape_pad(MovieTape *t, uint32_t f, MoviePad *out) {
    if (f >= t->end) return 0;
    if (t->cursor >= t->ne || t->e[t->cursor].f > f) t->cursor = 0;
    while (t->cursor < t->ne && t->e[t->cursor].f + t->e[t->cursor].frames <= f) t->cursor++;
    if (t->cursor < t->ne && t->e[t->cursor].f <= f) *out = t->e[t->cursor].pad;
    else memset(out, 0, sizeof *out);   /* (a gap: a neutral pad) */
    return 1;
}

/* the mark a replay's update (f, i) is held to: the last one at or before it, NULL before the first */
static inline const MovieMark *movie_tape_mark(const MovieTape *t, uint32_t f, uint32_t i) {
    const uint64_t key = movie_key(f, i);
    size_t lo = 0, hi = t->nm;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (movie_key(t->m[mid].f, t->m[mid].i) <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo ? &t->m[lo - 1] : NULL;
}

/* the view the camera's update (f, i) is held to: the last view line at or before it, NULL before the first */
static inline const MovieView *movie_tape_view(const MovieTape *t, uint32_t f, uint32_t i) {
    const uint64_t key = movie_key(f, i);
    size_t lo = 0, hi = t->nview;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (movie_key(t->view[mid].f, t->view[mid].i) <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo ? &t->view[lo - 1] : NULL;
}

/* the camera's update (f, i) as recorded: native, free and held from the last line at or before it, the turn only from a
   line of that update; 0 (the camera not the mouse's) before the first line */
static inline int movie_tape_cam(const MovieTape *t, uint32_t f, uint32_t i, MovieCam *out) {
    const uint64_t key = movie_key(f, i);
    size_t lo = 0, hi = t->ncam;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (movie_key(t->cam[mid].f, t->cam[mid].i) <= key) lo = mid + 1;
        else hi = mid;
    }
    memset(out, 0, sizeof *out);
    out->f = f, out->i = i;
    if (!lo) return 0;
    const MovieCam *c = &t->cam[lo - 1];
    out->native = c->native, out->free = c->free, out->held = c->held;
    if (movie_key(c->f, c->i) == key && c->moved) out->moved = 1, out->dyaw = c->dyaw, out->dpitch = c->dpitch;
    return 1;
}

/* the motion read (f, i) as recorded: its live readings into h (newest first), how many (0: none live then, or no such
   read in the recording) */
static inline int movie_tape_gyro(const MovieTape *t, uint32_t f, uint32_t i, MovieMotion *h, int max) {
    const uint64_t key = movie_key(f, i);
    size_t lo = 0, hi = t->ngyro;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (movie_key(t->gyro[mid].f, t->gyro[mid].i) < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= t->ngyro || movie_key(t->gyro[lo].f, t->gyro[lo].i) != key) return 0;
    const MovieGyro *g = &t->gyro[lo];
    int n = 0;
    for (uint32_t i = 0; i < g->live && n < max; i++) {
        const int64_t want = g->top - (int64_t)i;
        size_t j = g->upto;
        while (j > 0 && t->motion[j - 1].n != want) j--;   /* (the latest reading of that number written before this read) */
        if (!j) break;
        h[n++] = t->motion[j - 1];
    }
    return n;
}

/* the read frame n started at in the recording (1, *f), or 0 when the recording has no word of that frame */
static inline int movie_tape_frame(const MovieTape *t, uint32_t n, uint32_t *f) {
    size_t lo = 0, hi = t->nframes;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (t->frames[mid].n <= n) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return 0;
    const MovieFrames *m = &t->frames[lo - 1];
    if (n >= m->n + m->count) return 0;
    *f = m->f + (n - m->n) * m->reads;
    return 1;
}

/* how many reads frame n had in the recording (1, *reads), or 0 when the recording has no word of that frame */
static inline int movie_tape_frame_reads(const MovieTape *t, uint32_t n, uint32_t *reads) {
    size_t lo = 0, hi = t->nframes;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (t->frames[mid].n <= n) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || n >= t->frames[lo - 1].n + t->frames[lo - 1].count) return 0;
    *reads = t->frames[lo - 1].reads;
    return 1;
}

/* the next menu event for this screen at frame f, once each; events of earlier frames left untaken are counted in *missed */
static inline const MovieMenu *movie_tape_menu(MovieTape *t, uint32_t f, uint64_t screen, uint32_t *missed) {
    while (t->menu_cursor < t->nmenu && t->menu[t->menu_cursor].f < f) {
        if (!t->menu[t->menu_cursor].used) (*missed)++, t->menu[t->menu_cursor].used = 1;
        t->menu_cursor++;
    }
    for (size_t i = t->menu_cursor; i < t->nmenu && t->menu[i].f == f; i++)
        if (!t->menu[i].used && t->menu[i].screen == screen) {
            t->menu[i].used = 1;
            return &t->menu[i];
        }
    return NULL;
}

/* the next recorded action at or before frame f not given yet (once each, in order: one a call), NULL when none */
static inline const MovieAction *movie_tape_action(MovieTape *t, uint32_t f) {
    while (t->action_cursor < t->naction && t->action[t->action_cursor].used) t->action_cursor++;
    if (t->action_cursor >= t->naction || t->action[t->action_cursor].f > f) return NULL;
    MovieAction *a = &t->action[t->action_cursor++];
    a->used = 1;
    return a;
}

/* the options as recorded at frame f: each slot of v[MOVIE_OPTIONS] set from the lines at or before f (frames come in
   order: a cursor); *have gets a bit per slot set */
static inline void movie_tape_options(MovieTape *t, uint32_t f, int32_t v[MOVIE_OPTIONS], uint32_t *have) {
    while (t->option_cursor < t->noption && t->option[t->option_cursor].f <= f) {
        const MovieOption *o = &t->option[t->option_cursor++];
        const int slot = movie_option_slot(o->id);
        if (slot >= 0) v[slot] = o->v, *have |= 1u << slot;
    }
}

static inline void movie_tape_free(MovieTape *t) {
    free(t->e);
    free(t->m);
    free(t->cam);
    free(t->gyro);
    free(t->motion);
    free(t->menu);
    free(t->frames);
    free(t->view);
    free(t->action);
    free(t->option);
    free(t->anchor);
    memset(t, 0, sizeof *t);
}
