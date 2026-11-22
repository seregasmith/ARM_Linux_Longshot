"""Input recordings (issue 1113): host/input_movie.h, host/input_movie.c and the places that call them, without the game.

- The format: a recording's runs are written when the pad changes (the flight and free camera cheats' inputs with it) and
  at least every 600 reads; read back, every read gets the pad it was recorded with, a gap a neutral pad, the end
  nothing. The native mouse camera's lines are written when it is turned on or off, freed or held, or turned, and read
  back give every camera update its state and only the turned ones their turn. Motion readings are written once each,
  also when their numbers start again, and each motion read gets its own live readings back. Menu choices are given once,
  on their screen at their read; the ones left are counted. Floats keep their value exactly. A file of another format,
  lines out of order and a bad line are refused; the commands of mooki's ls-input-recording/1 that are not the game's input
  as it reached it (press, stick, sequence, live, note) are passed over and counted.
- The glue, in two processes as a test run would be: one records (from Link's first update: what came before it is not in
  the file), one plays the file back while the "player" gives something else entirely: the game gets the recording's
  pad, cheats' inputs, camera turns, motion readings and menu choices from the same point on, then the player's input
  once it ends; Link's positions held to the marks: none apart when they match, the first one apart logged with its update
  and frame when one differs. A replay with one more camera update, motion read and Link update in a frame and none in
  another (the jobs' race) keeps every other frame the recording's: they are held by frame and call, not counted. The file reads as mooki's inputlog.py reads it (a header with format and fps, then lines
  with f and cmd). With neither switch set every hook returns after the one decision: no lock taken again, no guest
  read. A replay recorded to the file it plays (the same name, another spelling of it, a hard link to it) is refused and
  the recording's bytes kept, the playback going on; menu choices the game did not take are counted as not met. A NaN,
  an infinity or a number past a float's range refuses its line.
- The wiring: npad_states asks before the merge and skips it for a played read, gives the game what the recording took or
  gave (the cheats' inputs too); the motion read and the native mouse camera's update go through the recording, the
  camera's on/off, free and held from it while it plays, and the script's mouse motion reaches the camera; the menu
  mouse records its focus and item choices, and while a recording plays gives the recorded ones instead of the mouse's;
  Link's update gives its position under the cheats' lock; the game's close work ends the recording (ExitProcess runs no
  atexit handler); the switches have rows. With neither switch set (a player's game) each hook's first line returns, and
  the callers read the guest and copy the motion readings only while a recording is active.

    python -I -B tests/test_input_movie.py [--cc gcc]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

HEADER_FIXTURE = r"""
#include "input_movie.h"
#include <math.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static MoviePad pad(uint64_t bits, int lx, int ly) { MoviePad p; memset(&p, 0, sizeof p); p.bits = bits; p.stick[0] = lx; p.stick[1] = ly; return p; }
static char file[1 << 18];
static size_t used;
static void put(const char *line, int n) { CHECK(n > 0); memcpy(file + used, line, (size_t)n); used += (size_t)n; }
static MovieTape load(void) {
    MovieTape t;
    memset(&t, 0, sizeof t);
    static char copy[1 << 18];
    memcpy(copy, file, used + 1);
    char *line = strtok(copy, "\n");
    int bad = 0;
    while (line) { if (movie_parse_line(&t, line) < 0) bad++; line = strtok(NULL, "\n"); }
    CHECK(!bad);
    return t;
}
int main(void) {
    char line[700];
    put(line, snprintf(line, sizeof line, "{\"format\":\"%s\",\"fps\":60,\"start\":7,\"from\":\"play\"}\n", MOVIE_FORMAT));
    /* recording: 10 reads of A, 5 of nothing, 1300 of the stick up (split at 600), 4 of the flight cheat's input, then B */
    MovieRun r;
    memset(&r, 0, sizeof r);
    static MoviePad want[1400];
    unsigned runs = 0;
    for (uint32_t f = 0; f < 1320; f++) {
        MoviePad p = f < 10 ? pad(1, 0, 0) : f < 15 ? pad(0, 0, 0) : f < 1315 ? pad(0, 0, 32767) : pad(2, -5, 9);
        if (f >= 1315 && f < 1319) p.flight = 0x100007fffULL, p.height = -1;
        want[f] = p;
        MovieRun done;
        if (movie_rec_step(&r, f, &p, &done)) runs++, put(line, movie_pad_line(line, sizeof line, done.f, done.frames, &done.pad));
    }
    CHECK(r.open && r.frames == 1);
    put(line, movie_pad_line(line, sizeof line, r.f, r.frames, &r.pad));
    CHECK(runs == 6);   /* (A, nothing, 600 + 600 + 100 of the stick, the cheat's 4: the last B still open above) */
    const float pos[3] = {1.0f / 3.0f, -123456.789f, 1e-7f}, pos2[3] = {2.5f, 0.0f, -0.0f};
    put(line, movie_mark_line(line, sizeof line, 3, 0, 0, pos));
    put(line, movie_mark_line(line, sizeof line, 40, 0, 30, pos2));
    /* the camera: on from update 2, free except 6-7, held at 9, turned at 4 and 10 */
    MovieCamRec cr;
    memset(&cr, 0, sizeof cr);
    unsigned camlines = 0;
    for (uint32_t m = 0; m < 14; m++) {
        MovieCam c;
        memset(&c, 0, sizeof c);
        c.n = m, c.f = m + 1, c.native = m >= 2, c.free = !(m == 6 || m == 7), c.held = m == 9;
        if (m == 4 || m == 10) c.moved = 1, c.dyaw = 0.1f * (float)m, c.dpitch = -1.0f / 3.0f;
        int n;
        if (movie_cam_rec(&cr, &c, line, sizeof line, &n)) camlines++, put(line, n);
    }
    CHECK(camlines == 7);   /* (0; on at 2; turned at 4; unfree at 6; free at 8; held at 9; turned and let go at 10) */
    /* motion: three reads of 2, 3 and 3 new readings (numbered 10..17), then the numbers start again at 1 */
    MovieGyroRec gr;
    memset(&gr, 0, sizeof gr);
    MovieMotion h[8];
    int64_t tops[4] = {11, 14, 17, 2};
    int lives[4] = {2, 5, 8, 2};
    unsigned news[4];
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < lives[c]; i++) {
            h[i].n = tops[c] - i, h[i].dt = 5000000 + h[i].n;   /* (a reading's own: the time since the one before it) */
            for (int k = 0; k < MOVIE_MOTION_FLOATS; k++) h[i].v[k] = (float)h[i].n * 0.125f + (c == 3 ? 100.0f : 0.0f) + (float)k / 7.0f;
        }
        int from;
        news[c] = (unsigned)movie_gyro_new(&gr, h, lives[c], &from);
        for (int i = from; i > from - (int)news[c]; i--) put(line, movie_motion_line(line, sizeof line, 50, &h[i])), gr.written++;
        MovieGyro g = {(uint32_t)c * 2, 50, (uint32_t)lives[c], gr.written, h[0].n, (uint32_t)c * 2};   /* (all in frame 50) */
        put(line, movie_gyro_line(line, sizeof line, &g));
    }
    CHECK(news[0] == 2 && news[1] == 3 && news[2] == 3 && news[3] == 2);
    /* menu choices */
    MovieMenu mm;
    memset(&mm, 0, sizeof mm);
    mm.f = 20, mm.kind = 1, mm.screen = 0xabc, mm.button = 0xdef;
    put(line, movie_menu_line(line, sizeof line, &mm));
    mm.kind = 2, mm.button = 0;
    snprintf(mm.key, sizeof mm.key, "Item_\"Bow\"");
    put(line, movie_menu_line(line, sizeof line, &mm));
    mm.f = 21, mm.kind = 1, mm.screen = 0x777, mm.button = 5;
    put(line, movie_menu_line(line, sizeof line, &mm));
    /* frames: 0-9 of 2 reads, 10 of 3, 11-12 of 2 */
    MovieFramesRec fr;
    memset(&fr, 0, sizeof fr);
    unsigned frame_lines = 0;
    uint32_t first = 0;
    for (uint32_t n = 0; n < 13; n++) {
        const uint32_t reads = n == 10 ? 3 : 2;
        MovieFrames done;
        if (movie_frames_step(&fr, n, first, reads, &done)) frame_lines++, put(line, movie_frames_line(line, sizeof line, &done));
        first += reads;
    }
    CHECK(fr.open && fr.run.n == 11 && fr.run.count == 2 && fr.run.f == 23);
    put(line, movie_frames_line(line, sizeof line, &fr.run));
    CHECK(frame_lines == 2);
    put(line, movie_note_line(line, sizeof line, 41, "a \"quoted\" \\ note"));
    put(line, snprintf(line, sizeof line, "{\"f\":50,\"cmd\":\"press\",\"buttons\":\"A\",\"frames\":8}\n\n"));
    file[used] = 0;

    MovieTape t = load();
    CHECK(t.header && !strcmp(t.from, "play") && t.ne == 7 && t.nm == 2 && t.skipped == 2 && t.end == 1320);
    CHECK(t.ncam == camlines && t.ngyro == 4 && t.nmotion == 10 && t.nmenu == 3 && t.nframes == 3);
    {
        uint32_t f = 99;
        CHECK(movie_tape_frame(&t, 0, &f) && f == 0 && movie_tape_frame(&t, 9, &f) && f == 18);
        CHECK(movie_tape_frame(&t, 10, &f) && f == 20 && movie_tape_frame(&t, 11, &f) && f == 23 && movie_tape_frame(&t, 12, &f) && f == 25);
        CHECK(!movie_tape_frame(&t, 13, &f));
    }
    for (uint32_t f = 0; f < 1320; f++) {
        MoviePad p;
        CHECK(movie_tape_pad(&t, f, &p) && movie_pad_same(&p, &want[f]));
    }
    MoviePad p;
    CHECK(!movie_tape_pad(&t, 1320, &p));
    CHECK(movie_tape_pad(&t, 3, &p) && p.bits == 1);   /* (backwards: the cursor starts again) */
    CHECK(movie_tape_pad(&t, 1316, &p) && p.flight == 0x100007fffULL && p.height == -1 && p.axes == 0);
    /* (marks by frame and call: the last at or before it) */
    CHECK(!movie_tape_mark(&t, 2, 7) && movie_tape_mark(&t, 3, 0) && !memcmp(movie_tape_mark(&t, 3, 0)->pos, pos, sizeof pos));
    CHECK(movie_tape_mark(&t, 39, 5) == movie_tape_mark(&t, 3, 0) && movie_tape_mark(&t, 40, 0)->k == 30 && movie_tape_mark(&t, 99, 0)->k == 30);
    CHECK(!memcmp(movie_tape_mark(&t, 40, 0)->pos, pos2, sizeof pos2));   /* (-0.0 kept as -0.0) */
    for (uint32_t m = 0; m < 20; m++) {   /* (update m was frame m + 1's first) */
        MovieCam c;
        CHECK(movie_tape_cam(&t, m + 1, 0, &c));
        CHECK(c.native == (m >= 2) && c.free == !(m == 6 || m == 7) && c.held == (m == 9));
        CHECK(c.moved == (m == 4 || m == 10));
        if (c.moved) CHECK(c.dyaw == 0.1f * (float)m && c.dpitch == -1.0f / 3.0f);
        /* (a second update in that frame, none in the recording: the first's state, and no turn of its) */
        CHECK(movie_tape_cam(&t, m + 1, 1, &c) && c.native == (m >= 2) && !c.moved);
    }
    {
        MovieCam c;
        CHECK(!movie_tape_cam(&t, 0, 0, &c) && !c.native);   /* (before the first line: not the mouse's) */
    }
    for (int c = 0; c < 4; c++) {
        MovieMotion got[8];
        CHECK(movie_tape_gyro(&t, 50, (uint32_t)c * 2, got, 8) == lives[c]);
        CHECK(!movie_tape_gyro(&t, 50, (uint32_t)c * 2 + 1, got, 8) && !movie_tape_gyro(&t, 51, (uint32_t)c * 2, got, 8));
        movie_tape_gyro(&t, 50, (uint32_t)c * 2, got, 8);
        for (int i = 0; i < lives[c]; i++)
            CHECK(got[i].n == tops[c] - i && got[i].dt == 5000000 + got[i].n &&
                  got[i].v[3] == (float)got[i].n * 0.125f + (c == 3 ? 100.0f : 0.0f) + 3.0f / 7.0f);
    }
    uint32_t missed = 0;
    const MovieMenu *e = movie_tape_menu(&t, 20, 0xabc, &missed);
    CHECK(e && e->kind == 1 && e->button == 0xdef);
    e = movie_tape_menu(&t, 20, 0xabc, &missed);
    CHECK(e && e->kind == 2 && !strcmp(e->key, "Item_Bow"));
    CHECK(!movie_tape_menu(&t, 20, 0xabc, &missed) && !movie_tape_menu(&t, 21, 0xabc, &missed) && !missed);
    CHECK(!movie_tape_menu(&t, 22, 0x777, &missed) && missed == 1);   /* (21's on 0x777 was never asked for) */
    movie_tape_free(&t);

    /* a gap between runs is a neutral pad; refusals */
    memset(&t, 0, sizeof t);
    CHECK(movie_parse_line(&t, "{\"format\":\"ls-input-recording/1\",\"fps\":60}") == 1 && !strcmp(t.from, "boot"));
    CHECK(movie_parse_line(&t, "{\"f\": 0, \"cmd\": \"pad\", \"frames\": 2, \"bits\": \"0x10\", \"lx\": 100}") == 1);
    CHECK(movie_parse_line(&t, "{\"f\":5,\"cmd\":\"pad\",\"frames\":1,\"bits\":\"0x1\"}") == 1);
    CHECK(movie_tape_pad(&t, 1, &p) && p.bits == 0x10 && p.stick[0] == 100);
    CHECK(movie_tape_pad(&t, 3, &p) && p.bits == 0 && p.stick[0] == 0);
    CHECK(movie_tape_pad(&t, 5, &p) && p.bits == 1 && !movie_tape_pad(&t, 6, &p));
    CHECK(movie_parse_line(&t, "{\"f\":5,\"cmd\":\"pad\",\"frames\":1,\"bits\":\"0x1\"}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":9,\"cmd\":\"pad\",\"frames\":0,\"bits\":\"0x1\"}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":9,\"cmd\":\"pad\",\"frames\":2}") == -1);
    CHECK(movie_parse_line(&t, "{\"cmd\":\"pad\"}") == -1);
    CHECK(movie_parse_line(&t, "nonsense") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"cam\",\"n\":3,\"native\":1,\"free\":1,\"held\":0,\"dyaw\":1}") == -1);   /* (a half turn) */
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"cam\",\"n\":3,\"native\":2,\"free\":1,\"held\":0}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"gyro\",\"c\":0,\"live\":1,\"top\":1,\"upto\":1}") == -1);   /* (no reading yet) */
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"motion\",\"n\":1,\"dt\":1,\"v\":[1,2]}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":0,\"cmd\":\"frames\",\"count\":0,\"reads\":2,\"read\":0}") == -1);   /* (no frames) */
    CHECK(movie_parse_line(&t, "{\"f\":0,\"cmd\":\"frames\",\"count\":2,\"reads\":2}") == -1);   /* (no first read) */
    /* (a NaN or an infinity, or a number past a float's range, refuses its line: a mark of NaN put back by HOS_INPUT_SNAP
       would put Link there) */
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"mark\",\"n\":1,\"pos\":[nan,0,0]}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"mark\",\"n\":1,\"pos\":[0,-inf,0]}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"mark\",\"n\":1,\"pos\":[0,0,1e39]}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"cam\",\"n\":4,\"native\":1,\"free\":1,\"held\":0,\"dyaw\":NaN,\"dpitch\":0}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"motion\",\"n\":1,\"dt\":1,\"v\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,inf]}") == -1);
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"mark\",\"n\":1,\"pos\":[0,0,1e38]}") == 1);   /* (a float's own) */
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"mark\",\"n\":2,\"i\":3,\"pos\":[0,0,1]}") == 1);
    CHECK(movie_parse_line(&t, "{\"f\":1,\"cmd\":\"mark\",\"n\":3,\"i\":1,\"pos\":[0,0,1]}") == -1);   /* (a call before the last) */
    CHECK(movie_parse_line(&t, "{\"f\":2,\"cmd\":\"mark\",\"n\":3,\"i\":1001,\"pos\":[0,0,1]}") == -1);   /* (past MOVIE_CALLS_MAX) */
    movie_tape_free(&t);
    memset(&t, 0, sizeof t);
    CHECK(movie_parse_line(&t, "{\"format\":\"ls-input-recording/2\"}") == -1);
    {   /* a view's block round trip; an older line (yaw and pitch only) still reads */
        uint32_t w[MOVIE_VIEW_WORDS];
        for (int i = 0; i < MOVIE_VIEW_WORDS; i++) w[i] = 0x40a00000u + (uint32_t)i * 0x1001u;
        uint32_t in[MOVIE_VIEW_IN_WORDS];
        for (int i = 0; i < MOVIE_VIEW_IN_WORDS; i++) in[i] = 0x3f800000u + (uint32_t)i;
        const MovieView v = movie_view_of(7, 3, w, in);
        CHECK(v.yaw == movie_word_float(w[MOVIE_VIEW_YAW]) && v.pitch == movie_word_float(w[MOVIE_VIEW_PITCH]));
        char vl[448];   /* (as the glue's: the block's 112 hex digits and the inputs' 104) */
        CHECK(movie_view_line(vl, sizeof vl, &v) > 0);
        MovieTape t2;
        memset(&t2, 0, sizeof t2);
        CHECK(movie_parse_line(&t2, "{\"format\":\"ls-input-recording/1\"}") == 1 && movie_parse_line(&t2, vl) == 1);
        CHECK(t2.nview == 1 && t2.view[0].has_w && !memcmp(t2.view[0].w, w, sizeof w) && movie_view_same(&t2.view[0], w));
        CHECK(t2.view[0].has_in && !memcmp(t2.view[0].in, in, sizeof in) && movie_view_inputs_same(&t2.view[0], in) &&
              movie_view_inputs_same(&t2.view[0], NULL));
        in[12] ^= 1;
        CHECK(!movie_view_inputs_same(&t2.view[0], in));
        w[3] ^= 1;
        CHECK(!movie_view_same(&t2.view[0], w));
        CHECK(movie_parse_line(&t2, "{\"f\":4,\"cmd\":\"view\",\"n\":8,\"yaw\":1.5,\"pitch\":2}") == 1 && !t2.view[1].has_w &&
              !t2.view[1].has_in);
        CHECK(movie_parse_line(&t2, "{\"f\":5,\"cmd\":\"view\",\"n\":9,\"yaw\":1,\"pitch\":2,\"w\":\"12\"}") == -1);
        CHECK(movie_parse_line(&t2, "{\"f\":5,\"cmd\":\"view\",\"n\":9,\"yaw\":nan,\"pitch\":2}") == -1);
        /* (the block's floats HOS_INPUT_SNAP writes back: a NaN yaw word refuses it, the same block finite reads) */
        for (int bad = 1; bad >= 0; bad--) {
            char hex[MOVIE_VIEW_WORDS * 8 + 1];
            for (int i = 0; i < MOVIE_VIEW_WORDS; i++)
                snprintf(hex + 8 * i, 9, "%08x", bad && i == MOVIE_VIEW_YAW ? 0x7fc00000u : 0x40a00000u);
            snprintf(vl, sizeof vl, "{\"f\":6,\"cmd\":\"view\",\"n\":12,\"yaw\":5,\"pitch\":5,\"w\":\"%s\"}", hex);
            CHECK(movie_parse_line(&t2, vl) == (bad ? -1 : 1));
        }
        movie_tape_free(&t2);
    }
    char names[128];
    movie_button_names((1ULL << 0) | (1ULL << 8) | (1ULL << 15) | (1ULL << 40), names, sizeof names);
    CHECK(!strcmp(names, "A+ZL+DOWN+b40"));
    {   /* (the host's decisions' clock: the wall's, then the game clock's from a recording's start, and back: going on from
           where it was each time, never back) */
        HosMovieTime t;
        memset(&t, 0, sizeof t);
        CHECK(movie_time_step(&t, 0, 5000) == 5000 && movie_time_step(&t, 0, 6000) == 6000);
        CHECK(movie_time_step(&t, 1, 100) == 6000 && movie_time_step(&t, 1, 350) == 6250);   /* (game: +250 from there) */
        CHECK(movie_time_step(&t, 0, 9000) == 6250 && movie_time_step(&t, 0, 9100) == 6350);   /* (wall again: +100) */
        CHECK(movie_time_step(&t, 0, 1000) == 6350);   /* (never back) */
    }
    {   /* (a replay's anchor: the recording's when within MOVIE_ANCHOR_SHIFT ahead, or a few frames past it (its frame taken
           from the clock before the hold applied); further back is a load the recording does not have) */
        const MovieAnchor a[1] = {{500, 0, 20}};
        uint32_t to = 0;
        CHECK(movie_anchor_rebase(a, 1, 0, 400, &to) && to == 500);   /* (a faster load: forward) */
        CHECK(movie_anchor_rebase(a, 1, 0, 500 + MOVIE_ANCHOR_GAP, &to) && to == 500);   /* (just past it: back to it) */
        CHECK(!movie_anchor_rebase(a, 1, 0, 500 + MOVIE_ANCHOR_GAP + 1, &to));
        CHECK(!movie_anchor_rebase(a, 1, 0, 500 - MOVIE_ANCHOR_SHIFT - 1, &to));
        CHECK(!movie_anchor_rebase(a, 1, 1, 500, &to));   /* (past the recording's last anchor) */
    }
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""

# (the glue with its platform stubbed: host/input_movie.c's own code, the game's side a script of reads and updates)
GLUE_HEAD = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* (what the glue costs: every lock it takes, and every guest read (GP), counted) */
static int lock_takes, guest_reads;
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define AcquireSRWLockExclusive(p) (lock_takes++, AcquireSRWLockExclusive(p))
#else
#include <pthread.h>
typedef long long LONG64;
typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define AcquireSRWLockExclusive(p) (lock_takes++, (void)pthread_mutex_lock(p))
#define ReleaseSRWLockExclusive(p) ((void)pthread_mutex_unlock(p))
#endif
static void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("LOG ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
volatile LONG64 hos_present_count = 100;
volatile uint32_t hos_frame_seconds_bits;
/* (the game's step object: its +0xd58 the game clock, one a frame; guest addresses are host addresses here) */
#define GP(a) (guest_reads++, (void *)(uintptr_t)(a))
static unsigned char fake_step[0xd60];
volatile uint64_t hos_step_probe;
static void game_clock(double v) { memcpy(fake_step + 0xd58, &v, sizeof v); }
/* (rx_option as host/rx_backend.c answers it: a replay's Debug tools rows first. FAKEOPT=<id>:<v>@<frame>: that row is v
   from the game's frame on, 0 before; the game's frame is game_fr) */
static int game_fr;
int hos_input_movie_option(int id, int *v);
int rx_option(int id, int otherwise) {
    int v;
    (void)otherwise;
    if (hos_input_movie_option(id, &v)) return v;
    if (id == 0) return atoi(getenv("FAKE_REALTIME") ? getenv("FAKE_REALTIME") : "0");
    const char *o = getenv("FAKEOPT");
    int oid, ov, of;
    if (o && sscanf(o, "%d:%d@%d", &oid, &ov, &of) == 3 && oid == id) return game_fr >= of ? ov : 0;
    return 0;
}
const char *hos_version(void) { return "test-1"; }
/* (the map Link is in: FAKEMAP at the start, unknown without it; the Debug tab's teleports: destination 0 Field (the
   adult's Field_A), 1 Market; one asked is taken at the frame after, and its load keeps Link from his updates 15 frames) */
static char cur_map[64];
static int tp_pending = -1, tp_until = -1, child_now = 1, tp_cancels;
int hos_oot_current_map(char *out, size_t cap) {
    if (!cur_map[0]) return 0;
    snprintf(out, cap, "%s", cur_map);
    return 1;
}
int hos_oot_state(int *event, int *loading, int *child) {
    if (!cur_map[0]) return 0;
    *event = 0, *loading = tp_until > game_fr, *child = child_now;
    return 1;
}
int hos_oot_destination(int index, const char **child, const char **adult) {
    static const char *const c[] = {"Field", "Market"}, *const a[] = {"Field_A", "Market_A"};
    if (index < 0 || index > 1) return 0;
    *child = c[index], *adult = a[index];
    return 1;
}
int hos_oot_teleport_ask(int index) { tp_pending = index; return 1; }
int hos_oot_age_ask(int adult) { child_now = !adult; return 1; }
void hos_oot_teleport_cancel(int ticket) { (void)ticket; tp_cancels++; }
int64_t hos_now_ns(void) { return (int64_t)game_fr * 16666667; }
const char *hos_user_dir = ".";
#ifndef _WIN32
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
static int CreateDirectoryA(const char *path, void *attr) { (void)attr; return mkdir(path, 0700) == 0; }
/* (the folder's listing as host/posix's windows.h gives it: a pattern, the folder and then a name with a star) */
typedef struct { char cFileName[260]; } WIN32_FIND_DATAA;
typedef struct { DIR *d; char pat[64]; } FakeFind;
typedef FakeFind *HANDLE;
#define INVALID_HANDLE_VALUE NULL
static int fake_next(FakeFind *f, WIN32_FIND_DATAA *fd) {
    struct dirent *e;
    while ((e = readdir(f->d)))
        if (!fnmatch(f->pat, e->d_name, 0)) return snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name), 1;
    return 0;
}
static HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd) {
    static FakeFind f;
    char dir[900];
    const char *slash = strrchr(pattern, '/');
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - pattern), pattern);
    snprintf(f.pat, sizeof f.pat, "%s", slash + 1);
    if (!(f.d = opendir(dir))) return NULL;
    if (!fake_next(&f, fd)) return closedir(f.d), NULL;
    return &f;
}
static int FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) { return fake_next(h, fd); }
static void FindClose(HANDLE h) { closedir(h->d); }
#endif
"""

GLUE_MAIN = r"""
/* the game: 20 frames before Link's first update (the title), then a read and an update a frame. The player's input is what
   the frame number makes it (SCRAMBLE=1: something else entirely, as a player who does not play along): A on frames 30-39,
   the stick up on 40-99, B and a tilt after, the flight cheat's input on 110-119. Each frame also has a motion read
   (readings on 50-59), a camera update (the mouse camera on from 30, unfree when fr % 40 >= 35, held at 45, turned on
   60-69) and menu choices (80: a focus, 81: an item key, on screen 0xabc). A played read must come back with the recorded
   value whatever the player gives. Link walks 0.25 m a frame from frame 40 (DRIFT=<frame>: bumped half a metre then, and
   so on from there unless HOS_INPUT_SNAP puts him back) */
/* TWOREADS=1: the game reads the controller twice a frame, Link's first update (frame 20) after both reads (LINKMID unset)
   or between them (LINKMID=1); EXTRA=<frame>: that frame has a third read. Each read's word is 0x100 * frame + its read in
   the frame (SCRAMBLE: 0x8000 + that), so a replay's read says which recorded one it was given */
static int two_reads(int frames) {
    const int mid = getenv("LINKMID") != NULL, extra = getenv("EXTRA") ? atoi(getenv("EXTRA")) : -1;
    const int scramble = getenv("SCRAMBLE") != NULL;
    for (int fr = 0; fr < frames; fr++) {
        game_clock((double)fr);
        const int reads = fr == extra ? 3 : 2;
        for (int j = 0; j < reads; j++) {
            MoviePad given;
            const int played = hos_input_movie_begin(&given);
            MoviePad p;
            memset(&p, 0, sizeof p);
            p.bits = (uint64_t)((scramble ? 0x8000 : 0) + 0x100 * fr + j);
            if (played) p = given;   /* (as npad_states: none of the merge) */
            else hos_input_movie_end(&p);
            printf("READ2 %d %d %d %llx\n", fr, j, played, (unsigned long long)p.bits);
            if (fr >= 20 && ((mid && j == 0) || (!mid && j == reads - 1))) {
                const float pos[3] = {(float)fr, 0, 0};
                float to[3];
                hos_input_movie_link(pos, to);
            }
        }
        hos_present_count++;
    }
    return 0;
}

int main(int argc, char **argv) {
    hos_step_probe = (uint64_t)(uintptr_t)fake_step;
    if (getenv("TWOREADS")) return two_reads(argc > 1 ? atoi(argv[1]) : 60);
    const int frames = argc > 1 ? atoi(argv[1]) : 150, drift = getenv("DRIFT") ? atoi(getenv("DRIFT")) : -1;
    const int scramble = getenv("SCRAMBLE") != NULL;
    const uint64_t menu_screen = getenv("MENUSCREEN") ? strtoull(getenv("MENUSCREEN"), NULL, 16) : 0xabc;
    const int vdrift = getenv("VIEWDRIFT") ? atoi(getenv("VIEWDRIFT")) : -1;
    const int indrift = getenv("INDRIFT") ? atoi(getenv("INDRIFT")) : -1;
    /* (the jobs' race: one more camera update, motion read and Link update in frame EXTRAHOOKS, none in SKIPHOOKS; only
       a frame's first is printed) */
    const int extra_hooks = getenv("EXTRAHOOKS") ? atoi(getenv("EXTRAHOOKS")) : -1;
    const int skip_hooks = getenv("SKIPHOOKS") ? atoi(getenv("SKIPHOOKS")) : -1;
    /* (a load: LOAD=<frame>:<frames>, Link not updated for that long from that frame, as a teleport's or a door's;
       ACTIONAT=<frame>: a Debug tools action (1005, a teleport) taken at Link's update then, by the player: 3 frames
       later in a SCRAMBLE run; RUNREC=<file> and RUNPLAY=<file>: a recording or a playback started at frame 25 from the
       menu or the bridge, a recording stopped at frame 100) */
    int load_at = -1, load_len = 0;
    if (getenv("LOAD")) sscanf(getenv("LOAD"), "%d:%d", &load_at, &load_len);
    const int action_at = getenv("ACTIONAT") ? atoi(getenv("ACTIONAT")) : -1;
    float x = 0;
    /* (CLOCKSTEP=<x>: the game clock goes on by x a frame, as above 60 fps: 0.5 at 120) */
    const double clock_step = getenv("CLOCKSTEP") ? atof(getenv("CLOCKSTEP")) : 1.0;
    if (getenv("FAKEMAP")) snprintf(cur_map, sizeof cur_map, "%s", getenv("FAKEMAP"));
    for (int fr = 0; fr < frames; fr++) {
        game_fr = fr;
        game_clock((double)fr * clock_step);
        if (tp_pending >= 0) {   /* (a teleport asked at the update before: its map, and its load) */
            const char *child, *adult;
            hos_oot_destination(tp_pending, &child, &adult);
            snprintf(cur_map, sizeof cur_map, "%s", child_now ? child : adult);
            tp_until = fr + 15, tp_pending = -1;
            printf("TELEPORT %d %s\n", fr, cur_map);
        }
        if (fr == 25 && getenv("RUNREC")) printf("RECSTART %d %d\n", fr, hos_input_record_start(getenv("RUNREC"), getenv("RUNROUTE") != NULL));
        if (fr == 25 && getenv("RUNPLAY")) printf("PLAYSTART %d %d\n", fr, hos_input_play_start(getenv("RUNPLAY"), 1, 1.0f));
        if (fr == 100 && getenv("RUNREC")) hos_input_record_stop();
        MoviePad given;
        const int played = hos_input_movie_begin(&given);
        MoviePad p;
        memset(&p, 0, sizeof p);
        p.bits = scramble ? 0x8000 : fr >= 30 && fr < 40 ? 1 : fr >= 100 ? 2 : 0;
        p.stick[1] = scramble ? -32767 : fr >= 40 && fr < 100 ? 32767 : fr >= 100 ? 1234 : 0;
        if (!scramble && fr >= 110 && fr < 120) p.flight = 0x10002, p.height = 1;
        if (scramble) p.axes = 0x5555;
        if (played) p = given;   /* (as npad_states: none of the merge) */
        else hos_input_movie_end(&p);
        printf("READ %d %d %llx %d %d %llx %llx %d\n", fr, played, (unsigned long long)p.bits, (int)p.stick[0], (int)p.stick[1],
               (unsigned long long)p.flight, (unsigned long long)p.axes, (int)p.height);
        printf("OPT %d %d\n", fr, rx_option(33, 0));   /* (the flight row, as the host reads it after the frame's first read) */
        /* motion */
        MovieMotion h[16];
        memset(h, 0, sizeof h);
        int live = 0;
        if (scramble && fr >= 40 && fr < 70) {
            live = 2;
            for (int i = 0; i < 2; i++) { memset(&h[i], 0, sizeof h[i]); h[i].n = 9000 - i; h[i].v[0] = -1; }
        } else if (!scramble && fr >= 50 && fr < 60) {
            const int top = 100 + 3 * (fr - 49);
            live = 3 * (fr - 49) < 8 ? 3 * (fr - 49) : 8;
            for (int i = 0; i < live; i++) {
                h[i].n = top - i, h[i].dt = 5000000;
                for (int k = 0; k < MOVIE_MOTION_FLOATS; k++) h[i].v[k] = (float)h[i].n * 0.5f + (float)k;
            }
        }
        const int calls = fr == extra_hooks ? 2 : fr == skip_hooks ? 0 : 1;
        for (int call = 0; call < calls; call++) {
            MovieMotion hh[16];
            memcpy(hh, h, sizeof hh);
            const int got = hos_input_movie_gyro(hh, 16, live);
            if (!call) printf("GYRO %d %d %lld %g\n", fr, got, got ? (long long)hh[0].n : 0LL, got ? (double)hh[0].v[0] : 0.0);
        }
        /* the camera */
        for (int call = 0; call < calls; call++) {
        MovieCam mc;
        /* (the camera's angles: 0.25 degrees a frame, VIEWDRIFT=<frame> a degree more from then) */
        const float yaw = 0.25f * (float)fr + (vdrift >= 0 && fr >= vdrift ? 1.0f : 0.0f), pitch = fr >= 100 ? 5.0f : 0.0f;
        uint32_t block[MOVIE_VIEW_WORDS] = {0xc2aa0000u, 0x42b20000u};   /* (the camera's state block: the limits, -85 and 89) */
        memcpy(&block[MOVIE_VIEW_YAW], &yaw, 4), memcpy(&block[MOVIE_VIEW_PITCH], &pitch, 4);
        /* (the camera's inputs: one word a frame's, INDRIFT=<frame> one more from then) */
        uint32_t inputs[MOVIE_VIEW_IN_WORDS] = {0};
        inputs[5] = 0x40000000u + (uint32_t)fr + (indrift >= 0 && fr >= indrift ? 1u : 0u);
        const int cam = hos_input_movie_cam_begin(&mc, block, inputs);
        if (cam == 1 && !call)   /* (after a snap: the recording's) */
            printf("VIEW %d %g %x\n", fr, (double)movie_word_float(block[MOVIE_VIEW_YAW]), (unsigned)inputs[5]);
        if (cam == 0) {
            mc.native = scramble ? 0 : fr >= 30;
            mc.free = scramble ? 1 : fr % 40 < 35;
            mc.held = !scramble && fr == 45;
            if (!scramble && fr >= 60 && fr < 70) mc.moved = 1, mc.dyaw = 0.5f * (float)(fr - 59), mc.dpitch = -0.25f;
            hos_input_movie_cam_end(&mc);
        } else if (cam == 1) {
            if (!call)
                printf("CAM %d %d %d %d %d %g %g\n", fr, mc.native, mc.free, mc.held, mc.moved, (double)mc.dyaw, (double)mc.dpitch);
            hos_input_movie_cam_end(&mc);   /* (as hos_mouse_camera: recorded too when the replay is) */
        }
        }
        /* the menu */
        if (!played && !hos_input_movie_playing()) {
            if (fr == 80) hos_input_movie_menu(1, 0xabc, 0xdef, NULL);
            if (fr == 81) hos_input_movie_menu(2, 0xabc, 0, "Item_Bow");
        } else {
            MovieMenu m;
            while (hos_input_movie_menu_take(menu_screen, &m)) {
                printf("MENU %d %d %llx %s\n", fr, m.kind, (unsigned long long)m.button, m.key);
                if (getenv("MENUREFUSE")) {   /* (as hos_oot_menu_tick: no such button on the screen, or the setter refused) */
                    hos_input_movie_menu_done(0);
                    continue;
                }
                hos_input_movie_menu(m.kind, menu_screen, m.button, m.key);   /* (as set_focus and set_item_key do) */
                hos_input_movie_menu_done(1);
            }
        }
        hos_present_count++;
        if (fr >= 20 && !(fr >= load_at && fr < load_at + load_len) && fr >= tp_until) {
            if (fr >= 40) x += 0.25f;
            if (fr == drift) x += 0.5f;   /* (a bump: he stays half a metre off unless put back) */
            const float pos[3] = {x, 2.0f, -3.0f};
            float to[3];
            int dropped = 0;   /* (the Debug tools, before the marks: as oot_update_player_cheats before the link hook) */
            const int live = fr == action_at + (scramble ? 3 : 0) ? 1005 : 0;
            const int act = hos_input_movie_action(live, &dropped);
            if (act || dropped) printf("ACTION %d %d %d\n", fr, act, dropped);
            hos_input_movie_action_taken(act);
            for (int call = 0; call < calls; call++)
                if (hos_input_movie_link(pos, to)) {
                    printf("SNAP %d %g\n", fr, (double)to[0]);
                    x = to[0];   /* (put back, as the game's move of his controller does) */
                }
        }
    }
    printf("COUNTS 0 %d %d\n", lock_takes, guest_reads);
    return 0;
}
"""


def check(ok, message):
    if not ok:
        raise SystemExit("FAIL: " + message)


def wiring():
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    main_c = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    mouse = (ROOT / "host" / "mouse_native.c").read_text(encoding="utf-8")
    menu = (ROOT / "host" / "oot_menu.c").read_text(encoding="utf-8")
    table = (ROOT / "gpu" / "vk" / "switches.def").read_text(encoding="utf-8")
    npad = block(hid, "static void npad_states(Ctx *c) {")
    begin, merge = npad.find("const int played = connected && hos_input_movie_begin(&given);"), npad.find("read_input(&buttons, &taps, stick);")
    check(0 <= begin < merge, "npad_states does not ask the recording before its merge")
    check("if (connected && !played) {" in npad[begin:merge + 1], "a played read still runs the merge")
    end, write = npad.find("hos_input_movie_end(&pad);"), npad.find("G64(e + 8) = word;")
    check(merge < end < write, "the word given to the game is not the one the recording took or gave")
    check("const MoviePad pad = {word, {stick[0], stick[1], stick[2], stick[3]}, flight, axes, height};" in npad,
          "the cheats' inputs are not in the recording")
    given = npad.find("word = given.bits;")
    check(merge < given < write and "__atomic_store_n(&flight_input, given.flight, __ATOMIC_RELAXED);" in npad[given:end] and
          "__atomic_store_n(&free_camera_axes, given.axes, __ATOMIC_RELAXED);" in npad[given:end],
          "a frame's pad (and the cheats' inputs) not given to a played or later read")
    check("oc_pad_word(buttons)" in npad[merge:end] and npad.count("oc_pad_word(") == 1,
          "the note buttons' word is made somewhere else than before the recording")
    check(npad.count("hos_title_input(buttons)") == 1 and "if (connected && !played) buttons = hos_title_input(buttons);" in npad,
          "the title's filter runs on a played read")
    six = block(hid, "HLE(_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE) {")
    check("&& !hos_input_movie_playing();" in six and "live = hos_input_movie_gyro(mm, count, live);" in six,
          "motion does not go through the recording")
    check(0 <= six.find("if (hos_input_movie_active()) {") < six.find("MovieMotion mm[HOS_MOTION_HISTORY];"),
          "the motion readings are copied for a recording with none")
    # (with neither switch set, a player's game: each of the glue's hooks returns at its first line, before a lock or a guest
    # read; the camera's end and the menu's recording take no lock without a recording)
    movie_c = (ROOT / "host" / "input_movie.c").read_text(encoding="utf-8")
    for head, first in (("int hos_input_movie_begin(MoviePad *given) {", "if (movie_mode_now() == MOVIE_OFF) return 0;"),
                        ("void hos_input_movie_end(const MoviePad *p) {",
                         "if (__atomic_load_n(&movie_mode, __ATOMIC_ACQUIRE) != MOVIE_RECORD) return;"),
                        ("int hos_input_movie_gyro(MovieMotion *h, int max, int live) {", "if (movie_mode_now() == MOVIE_OFF) return live;"),
                        ("int hos_input_movie_cam_begin(MovieCam *c, uint32_t *block, uint32_t *inputs) {",
                         "if (movie_mode_now() == MOVIE_OFF) return -1;"),
                        ("void hos_input_movie_cam_end(const MovieCam *c) {", "if (!__atomic_load_n(&S.rec, __ATOMIC_RELAXED)) return;"),
                        ("void hos_input_movie_menu(int kind, uint64_t screen, uint64_t button, const char *key) {",
                         "if (!__atomic_load_n(&S.rec, __ATOMIC_RELAXED)) return;"),
                        ("int hos_input_movie_action(int live, int *dropped) {", "*dropped = 0;"),
                        ("void hos_input_movie_action_taken(int a) {", "if (!a || !__atomic_load_n(&S.rec, __ATOMIC_RELAXED)) return;"),
                        ("int hos_input_movie_option(int id, int *v) {", "if (!__atomic_load_n(&movie_opt_on, __ATOMIC_ACQUIRE)) return 0;"),
                        ("int hos_input_movie_link(const float pos[3], float to[3]) {", "if (movie_mode_now() == MOVIE_OFF) return 0;")):
        body = block(movie_c, head).split("\n")
        check(len(body) > 1 and body[1].strip().startswith(first), "%s does not return at once with neither switch set" % head)
    check(six.index("live = hos_input_movie_gyro(") < six.index("const int n = live ? live : count;"),
          "the motion read hands out readings the recording did not see")
    script = block(hid, "static void scripted_input(uint64_t *buttons, uint64_t *taps, int32_t stick[4]) {")
    check("script_mouse[0]" in script and "p.mouse[s[1] == 'Y']" in hid, "the script's MX= and MY= do not move the mouse")
    hook = block(mouse, "void hos_mouse_camera(uint64_t cam) {")
    gate, guest = hook.find("const int recorded = hos_input_movie_active();"), hook.find("G64(MOVIE_VIEW_INPUTS)")
    ask = hook.find("movie = hos_input_movie_cam_begin(&mc, (uint32_t *)GP(cam + MOVIE_VIEW_AT), inputs);")
    check(0 <= gate < hook.find("looks_like_camera(cam)") < guest < ask and "int native = recorded ? -1 : native_on();" in hook and
          "if (recorded) {" in hook[gate:guest],
          "the camera reads the guest with no recording (or its native row unasked first), or its block is not given")
    check("if (native < 0) native = movie == 1 ? mc.native : native_on();" in hook and "movie == 1 ? mc.free : free_look()" in hook and
          "movie == 1 ? mc.held : stick_held != 0" in hook, "a replay's camera decides by the mouse and the window, not the recording")
    check(hook.count("hos_input_movie_cam_end(&mc);") == 4, "a way out of the camera's update is not recorded")
    check("hos_script_mouse_take(&sx, &sy)" in hook and "moved = mc.moved, dyaw = mc.dyaw, dpitch = mc.dpitch;" in hook,
          "the turn is not the recording's or the script's")
    tick = block(menu, "void hos_oot_menu_tick(Ctx *c, uint64_t screen) {\n    if (!screen) return;")   # (Windows': Linux has a stub)
    check("if (hos_input_movie_playing()) {" in tick and "hos_input_movie_menu_take(screen, &m)" in tick and
          "} else if (mine) {" in tick, "the menu mouse plays the player's mouse during a replay")
    check("hos_input_movie_menu(1, screen, button, NULL);" in block(menu, "static int set_focus(Ctx *c, uint64_t screen, uint64_t button) {") and
          "hos_input_movie_menu(2, screen, 0, key);" in block(menu, "static int set_item_key(Ctx *c, uint64_t screen, const char *key) {"),
          "the menu mouse's choices are not recorded")
    lift = block(main_c, "void hos_oot_lift_player(Ctx *c, uint64_t actor) {")
    check(0 <= lift.find("if (hos_input_movie_active()) {") < lift.find("recomp_load32(c, actor + 0x31c"),
          "Link's position is read for a recording with none")
    check("if (hos_input_movie_link(pos, to) && !oot_put_link(c, components, to))" in lift and
          "A64Fn change_position = a64_lookup(0x806d2530ULL);" in block(main_c, "static int oot_put_link("),
          "HOS_INPUT_SNAP does not put Link back through his controller")
    took, call, gave = (lift.find("TryAcquireSRWLockExclusive(&cheats_lock)"), lift.find("hos_input_movie_link(pos, to)"),
                        lift.rfind("ReleaseSRWLockExclusive(&cheats_lock)"))
    check(0 <= took < call < gave, "Link's update does not give the recording his position under the cheats' lock")
    check("actor + 0x31c" in lift and "#define STATE_LINK_POS 0x31c" in (ROOT / "host" / "oot_state_tap.c").read_text(encoding="utf-8"),
          "the recording's position is not the state tap's")
    close = block((ROOT / "host" / "hos_relaunch.c").read_text(encoding="utf-8"), "void hos_close_finish(void) {")
    check("hos_input_movie_close();" in close, "the game's close work does not end the recording (ExitProcess runs no atexit)")
    for name in ("HOS_INPUT_RECORD", "HOS_INPUT_PLAY", "HOS_INPUT_FROM", "HOS_INPUT_SYNC", "HOS_INPUT_SYNC_MAX", "HOS_INPUT_ROUTE",
                 "HOS_INPUT_SAVE"):
        check("SW(%s," % name in table, "no switch row for " + name)
    # (the Debug tools: the action the game takes goes through the recording before it is taken, and is recorded after; a
    # bridge's teleport a playback does not take is answered)
    cheats = block(main_c, "static void oot_update_player_cheats(Ctx *c, uint64_t actor, uint64_t components) {")
    take, ask = cheats.find("action = hos_input_movie_action(live, &dropped);"), cheats.find("oot_bridge_teleport_take(&bridge_ticket)")
    acted, taken = cheats.find("oot_teleport(c, actor, (unsigned)(action - OOT_TELEPORT_ACTION))"), cheats.find("hos_input_movie_action_taken(taken);")
    check(0 <= ask < take < acted < taken, "the Debug tools' action does not go through the recording before it is taken, or "
          "is not recorded after")
    check("oot_bridge_teleport_done(bridge_ticket, 0);" in cheats[take:acted], "a bridge teleport a playback does not take is not answered")
    backend = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")
    option = block(backend, "int rx_option(int id, int otherwise) {")
    check(0 <= option.find("if (hos_input_movie_option(id, &v)) return v;") < option.find("vk->option(id)"),
          "rx_option asks the menu before a playback's rows")
    print("PASS: wiring")


def compile_c(cc, src, exe, extra):
    libraries = [] if os.name == "nt" else ["-lpthread"]
    r = subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation"] + extra +
                       [str(src), "-o", str(exe), "-lm"] + libraries, text=True, capture_output=True)
    check(r.returncode == 0, "compile failed:\n" + r.stdout + r.stderr)


def run(exe, env_extra, frames=150):
    env = dict(os.environ)
    for k in ("HOS_INPUT_RECORD", "HOS_INPUT_PLAY", "HOS_INPUT_FROM", "DRIFT", "SCRAMBLE", "FAKE_REALTIME", "MENUSCREEN", "TWOREADS",
              "LINKMID", "EXTRA", "VIEWDRIFT", "HOS_INPUT_SYNC", "HOS_INPUT_SYNC_MAX", "INDRIFT", "MENUREFUSE", "EXTRAHOOKS",
              "SKIPHOOKS", "LOAD", "ACTIONAT", "CLOCKSTEP", "FAKEOPT", "FAKEMAP", "RUNREC", "RUNPLAY", "RUNROUTE", "HOS_INPUT_ROUTE",
              "HOS_INPUT_SAVE"):
        env.pop(k, None)
    env.update(env_extra)
    r = subprocess.run([str(exe), str(frames)], text=True, capture_output=True, env=env, timeout=60)
    check(r.returncode == 0, "fixture failed:\n" + r.stdout + r.stderr)
    out = {"READ": {}, "GYRO": {}, "CAM": {}, "MENU": [], "READ2": {}, "VIEW": {}, "SNAP": {}, "COUNTS": {}, "OPT": {},
           "ACTION": {}, "RECSTART": {}, "PLAYSTART": {}, "TELEPORT": {}}
    logs = []
    for ln in r.stdout.splitlines():
        kind, _, rest = ln.partition(" ")
        if kind == "LOG":
            logs.append(rest)
        elif kind == "MENU":
            out["MENU"].append(rest.split())
        elif kind == "READ2":
            v = rest.split()
            out[kind][(int(v[0]), int(v[1]))] = v[2:]
        elif kind in out:
            v = rest.split()
            out[kind][int(v[0])] = v[1:]
    return out, logs


def glue(cc, tmp):
    src = (ROOT / "host" / "input_movie.c").read_text(encoding="utf-8")
    body = "".join(ln for ln in src.splitlines(keepends=True)
                   if not ln.startswith('#include "hos.h"') and not ln.startswith("#include <windows.h>"))
    check(body.count("extern volatile LONG64 hos_present_count;") == 1, "the glue's present count changed its declaration")
    for decl in ("extern volatile LONG64 hos_present_count;", "extern volatile uint32_t hos_frame_seconds_bits;",
                 "extern volatile uint64_t hos_step_probe;",
                 "int rx_option(int id, int otherwise);", "const char *hos_version(void);",
                 "int hos_oot_current_map(char *out, size_t cap);", "extern const char *hos_user_dir;",
                 "int hos_oot_state(int *event, int *loading, int *child);",
                 "int hos_oot_destination(int index, const char **child, const char **adult);",
                 "int hos_oot_teleport_ask(int index);", "int hos_oot_age_ask(int adult);",
                 "void hos_oot_teleport_cancel(int ticket);"):
        body = body.replace(decl, "")
    (tmp / "glue.c").write_text(GLUE_HEAD + body + GLUE_MAIN, encoding="utf-8")
    exe = tmp / ("glue.exe" if os.name == "nt" else "glue")
    compile_c(cc, tmp / "glue.c", exe, ["-I", str(ROOT / "host")])
    rec = tmp / "run.jsonl"

    # neither switch set (a player's game): the one decision takes the lock once, then no hook takes it or reads the guest
    got, logs = run(exe, {})
    check(got["COUNTS"][0] == ["1", "0"] and not logs, "the hooks' cost with neither switch set: %s locks and %s guest reads "
          "(1 and 0 wanted), %s" % (got["COUNTS"][0][0], got["COUNTS"][0][1], logs))
    check(all(v[0] == "0" for v in got["READ"].values()), "a read played with neither switch set")
    print("PASS: neither switch set: one decision, then no lock and no guest read in 150 frames of hooks")

    # record from Link's first update (frame 20 is his first; the read after it, 21, is read 0)
    got, logs = run(exe, {"HOS_INPUT_RECORD": str(rec)})
    check(any("recording to" in ln for ln in logs) and
          any("recorded 129 frames (129 reads), 129 camera updates and 129 motion reads" in ln for ln in logs),
          "recording log: %s" % logs)
    lines = [json.loads(ln) for ln in rec.read_text(encoding="utf-8").splitlines() if ln.strip()]
    head, events = lines[0], lines[1:]
    check(head["format"] == "ls-input-recording/1" and head["fps"] == 60 and head["from"] == "play" and
          head["build"] == "test-1" and head["step"] == "per frame", "header: %s" % head)
    check(all(isinstance(e.get("f"), int) and isinstance(e.get("cmd"), str) for e in events), "a line without f or cmd")
    pads = [e for e in events if e["cmd"] == "pad"]
    check([(e["f"], e["frames"], e["bits"], e["ly"], e.get("fl")) for e in pads] ==
          [(0, 9, "0x0", 0, None), (9, 10, "0x1", 0, None), (19, 60, "0x0", 32767, None), (79, 10, "0x2", 1234, None),
           (89, 10, "0x2", 1234, "0x10002"), (99, 30, "0x2", 1234, None)], "runs: %s" % pads)
    check(pads[1]["buttons"] == "A" and pads[4]["fh"] == 1 and pads[4]["fc"] == "0x0", "run fields: %s" % pads)
    cams = [e for e in events if e["cmd"] == "cam"]
    turned = [e for e in cams if "dyaw" in e]
    check(len(turned) == 10 and turned[0]["n"] == 39 and turned[0]["dyaw"] == 0.5 and turned[0]["dpitch"] == -0.25,
          "camera turns: %s" % turned[:2])
    check(cams[0]["n"] == 0 and cams[0]["native"] == 0 and any(e["native"] == 1 and e["n"] == 9 for e in cams),
          "camera lines: %s" % cams[:3])
    motions, gyros = [e for e in events if e["cmd"] == "motion"], [e for e in events if e["cmd"] == "gyro"]
    check(len(gyros) == 10 and len(motions) == 30 and gyros[0]["c"] == 29 and gyros[0]["top"] == 103 and
          gyros[-1]["live"] == 8 and gyros[-1]["upto"] == 30, "motion: %s %s" % (gyros[:1], len(motions)))
    menus = [e for e in events if e["cmd"] in ("focus", "itemkey")]
    check([(e["f"], e["cmd"]) for e in menus] == [(59, "focus"), (60, "itemkey")] and menus[1]["key"] == "Item_Bow",
          "menu: %s" % menus)
    marks = [e for e in events if e["cmd"] == "mark"]
    check(marks[0]["n"] == 0 and marks[0]["pos"] == [0, 2, -3] and len(marks) == 1 + 110, "marks: %d, %s" % (len(marks), marks[:2]))
    check(not any(e["cmd"] == "note" for e in events), "a note in a recording that holds everything")
    views = [e for e in events if e["cmd"] == "view"]
    check(len(views) == 129 and {k: views[0][k] for k in ("f", "cmd", "n", "yaw", "pitch")} ==
          {"f": 0, "cmd": "view", "n": 0, "yaw": 5.25, "pitch": 0} and len(views[0]["w"]) == 112 and
          views[0]["w"].startswith("c2aa000042b20000") and len(views[0]["in"]) == 104 and views[-1]["pitch"] == 5,
          "views: %d %s" % (len(views), views[:1]))
    frames = [e for e in events if e["cmd"] == "frames"]
    check(frames == [{"f": 0, "cmd": "frames", "count": 129, "reads": 1, "read": 0}], "frames: %s" % frames)
    print("PASS: recording (%d runs, %d camera lines, %d motion reads, %d marks)" % (len(pads), len(cams), len(gyros), len(marks)))

    # mooki's reader takes it (ls-workbench play/pilot/inputlog.py read_recording: a header with its format, then lines)
    check(lines[0].get("format") == "ls-input-recording/1" and all("f" in e for e in events), "not inputlog's shape")

    # playback: the player gives something else entirely; the game gets the recording, then the player again
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "SCRAMBLE": "1"}, 170)
    reads = got["READ"]
    check(all(reads[fr][0] == "0" for fr in range(21)) and all(reads[fr][0] == "1" for fr in range(21, 150)), "played reads")

    def want_pad(fr):
        bits = "1" if 30 <= fr < 40 else "2" if fr >= 100 else "0"
        ly = "1234" if fr >= 100 else "32767" if fr >= 40 else "0"
        return [bits, "0", ly, "10002" if 110 <= fr < 120 else "0", "0", "1" if 110 <= fr < 120 else "0"]
    check(all(reads[fr][1:] == want_pad(fr) for fr in range(21, 150)), "a played read's pad differs: %s" %
          [(fr, reads[fr]) for fr in range(21, 150) if reads[fr][1:] != want_pad(fr)][:3])
    check(all(reads[fr][1:] == ["8000", "0", "-32767", "0", "5555", "0"] for fr in range(150, 170)), "after the end the player's")
    gyro = got["GYRO"]
    check(all(gyro[fr] == (["%d" % min(8, 3 * (fr - 49)), "%d" % (100 + 3 * (fr - 49)), "%g" % ((100 + 3 * (fr - 49)) * 0.5)]
                           if 50 <= fr < 60 else ["0", "0", "0"]) for fr in range(21, 150)), "played motion: %s" %
          [(fr, gyro[fr]) for fr in range(40, 70)][:12])
    cam = got["CAM"]
    for fr in range(21, 150):
        want = ["%d" % (fr >= 30), "%d" % (fr % 40 < 35), "%d" % (fr == 45), "%d" % (60 <= fr < 70),
                "%g" % (0.5 * (fr - 59) if 60 <= fr < 70 else 0), "%g" % (-0.25 if 60 <= fr < 70 else 0)]
        check(cam.get(fr) == want, "played camera at %d: %s, not %s" % (fr, cam.get(fr), want))
    check(got["MENU"] == [["80", "1", "def"], ["81", "2", "0", "Item_Bow"]], "played menu: %s" % got["MENU"])
    check(any("playback over at frame 129 of 129: 0 of 129 updates of Link's apart from the recording; 2 menu choices given, "
              "0 not met; 0 of 129 camera updates apart from it (its inputs at 0); 0 frames had another count of reads" in ln
              for ln in logs), "end log: %s" % logs)
    check(not any("left the recording" in ln for ln in logs), "a drift with the same positions: %s" % logs)
    print("PASS: playback (129 reads, the camera, motion and menu choices the recording's; 0 apart)")

    # the jobs' race: one more camera update, motion read and Link update in frame 50 of the replay (29 of the recording's)
    # and none in frame 75. Held by frame and call, every other frame is the recording's still, the turns of 60-69 too,
    # and nothing is apart; counted from the start, every one after 50 was given its neighbour's
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "SCRAMBLE": "1", "EXTRAHOOKS": "50", "SKIPHOOKS": "75"}, 170)
    for fr in [fr for fr in range(21, 150) if fr not in (50, 75)]:
        want = ["%d" % (fr >= 30), "%d" % (fr % 40 < 35), "%d" % (fr == 45), "%d" % (60 <= fr < 70),
                "%g" % (0.5 * (fr - 59) if 60 <= fr < 70 else 0), "%g" % (-0.25 if 60 <= fr < 70 else 0)]
        check(got["CAM"].get(fr) == want, "one more update in frame 50: the camera at %d %s, not %s" % (fr, got["CAM"].get(fr), want))
        want = (["%d" % min(8, 3 * (fr - 49)), "%d" % (100 + 3 * (fr - 49)), "%g" % ((100 + 3 * (fr - 49)) * 0.5)]
                if 50 <= fr < 60 else ["0", "0", "0"])
        check(got["GYRO"][fr] == want, "one more motion read in frame 50: frame %d %s, not %s" % (fr, got["GYRO"][fr], want))
    check(any("playback over at frame 129 of 129: 0 of 129 updates of Link's apart from the recording; 2 menu choices given, "
              "0 not met; 0 of 129 camera updates apart from it (its inputs at 0)" in ln for ln in logs),
          "one more and one fewer update: %s" % logs[-3:])
    print("PASS: one more camera update, motion read and Link update in a frame, and none in another: the rest the recording's")

    # a replay recorded too (HOS_INPUT_PLAY and HOS_INPUT_RECORD): it writes the recording's lines again, all but the header
    again = tmp / "again.jsonl"
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "HOS_INPUT_RECORD": str(again), "SCRAMBLE": "1"}, 170)
    check(any("the replay is recorded too" in ln for ln in logs), "no word of the replay's recording: %s" % logs)
    mine = [json.loads(ln) for ln in again.read_text(encoding="utf-8").splitlines() if ln.strip()]
    result = [e for e in mine if e.get("cmd") == "result"]
    mine = [e for e in mine if e.get("cmd") != "result"]
    check(mine[0]["from"] == "play" and mine[1:] == events, "the replay's own recording differs: %s" %
          [(a, b) for a, b in zip(mine[1:], events) if a != b][:3])
    check(len(result) == 1 and result[0]["link_apart"] == 0 and result[0]["diverged"] == 0 and result[0]["ended"] == "over" and
          result[0]["link_checked"] == 129, "the replay's result line: %s" % result)
    print("PASS: a replay recorded too writes the recording's %d lines again, and its result" % len(events))

    # a replay recorded to the file it plays: by its name, another spelling of it and a hard link to it. Refused, the
    # recording's bytes kept, the playback going on as without HOS_INPUT_RECORD. (Stopped at frame 100: a whole replay
    # recorded writes the same bytes again, so only a cut one would show the file wiped)
    source = rec.read_bytes()
    hard = tmp / "hard.jsonl"
    os.link(rec, hard)
    for name in (str(rec), str(rec.parent) + os.sep + "." + os.sep + rec.name, str(hard)):
        got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "HOS_INPUT_RECORD": name, "SCRAMBLE": "1"}, 100)
        check(rec.read_bytes() == source, "recording a replay to %s changed the recording" % name)
        check(any("names the recording HOS_INPUT_PLAY plays" in ln for ln in logs) and
              not any("the replay is recorded too" in ln for ln in logs), "%s: no refusal: %s" % (name, logs))
        check(all(got["READ"][fr][0] == "1" for fr in range(21, 100)) and
              any("playback stopped at frame 78 of 129" in ln for ln in logs), "%s: the playback did not go on: %s" % (name, logs[-2:]))
    print("PASS: a replay recorded to the file it plays (its name, another spelling, a hard link): refused, the recording kept")

    # menu choices the game did not take (no such button on the screen, the setter refused): not met
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "SCRAMBLE": "1", "MENUREFUSE": "1"}, 170)
    check(any("0 menu choices given, 2 not met" in ln for ln in logs), "refused menu choices: %s" % logs[-3:])
    print("PASS: menu choices the game did not take counted as not met")

    # a drift: the first one logged with its update and read, the count at the end; menu choices on no screen: not met
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "DRIFT": "90", "MENUSCREEN": "111", "VIEWDRIFT": "100", "INDRIFT": "110",
                          "HOS_INPUT_SYNC": "0"}, 150)
    check(not got["SNAP"], "HOS_INPUT_SYNC=0 put Link back: %s" % got["SNAP"])
    left = [ln for ln in logs if "left the recording at Link's" in ln]
    check(len(left) == 1 and "update 69 (frame 69)" in left[0] and "0.5000 m apart" in left[0], "drift log: %s" % left)
    check(any("playback stopped at frame 128 of 129: 60 of 129 updates of Link's apart from the recording; 0 menu choices given, "
              "2 not met" in ln for ln in logs), "stop log: %s" % logs)
    view = [ln for ln in logs if "camera left the recording" in ln]
    check(len(view) == 1 and "its update 79 (frame 79): yaw 26 pitch 5, the recording's 25 5" in view[0] and
          any("50 of 129 camera updates apart from it (its inputs at 40)" in ln for ln in logs) and
          any("camera inputs left the recording at its update 89 (frame 89)" in ln for ln in logs), "camera drift: %s" % view)
    print("PASS: drift found at its update, the camera's too, menu choices not met counted")

    # HOS_INPUT_SYNC (on by default, every update): the same drifts put back: Link once (then on the recording again), the
    # camera at each update (its yaw then the recording's), each counted
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "DRIFT": "90", "VIEWDRIFT": "100", "INDRIFT": "110"}, 150)
    check(got["SNAP"] == {90: ["12.75"]}, "Link's corrections: %s" % got["SNAP"])
    check(all(got["VIEW"][fr] == ["%g" % (0.25 * fr), "%x" % (0x40000000 + fr)] for fr in range(21, 150)),
          "a corrected camera's yaw or inputs not the recording's: %s" % [(fr, got["VIEW"][fr]) for fr in range(108, 112)])
    check(any("playback stopped at frame 128 of 129: 1 of 129 updates of Link's apart" in ln for ln in logs) and
          any("put back the camera's inputs at 40 updates, the camera at 50 and Link at 1 (the largest 0.5000 m" in ln
              for ln in logs) and not any("diverged" in ln for ln in logs), "correction logs: %s" % logs[-4:])
    print("PASS: HOS_INPUT_SYNC puts the camera and Link back by default, and counts it")

    # HOS_INPUT_SYNC=4: only every fourth update put back (Link's bump at update 69 waits for update 72)
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "DRIFT": "90", "HOS_INPUT_SYNC": "4"}, 150)
    check(got["SNAP"] == {93: ["13.5"]}, "every fourth update: Link's corrections %s" % got["SNAP"])
    print("PASS: HOS_INPUT_SYNC=4 puts Link back at every fourth update")

    # past HOS_INPUT_SYNC_MAX: the replay went another way. Said once, nothing put back after it (a test fails on it)
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "DRIFT": "90", "VIEWDRIFT": "100", "HOS_INPUT_SYNC_MAX": "0.1"}, 150)
    div = [ln for ln in logs if "diverged at frame" in ln]
    check(not got["SNAP"] and len(div) == 1 and "diverged at frame 69: Link 0.500 m from the recording, past HOS_INPUT_SYNC_MAX "
          "(0.10 m)" in div[0], "divergence: %s %s" % (got["SNAP"], div))
    check(any("; diverged" in ln for ln in logs) and any(got["VIEW"][fr][0] != "%g" % (0.25 * fr) for fr in range(100, 150)),
          "after the divergence the camera is still put back, or the end does not say it: %s" % logs[-3:])
    print("PASS: Link past HOS_INPUT_SYNC_MAX: diverged, said once, nothing put back after it")

    # the Debug tools: a teleport taken at frame 50 recorded; the replay gives it at 50 and takes not the player's at 53. The
    # flight row turned on at frame 60: the replay answers rx_option with it from 60, and the player's (none) after the end
    acts = tmp / "acts.jsonl"
    run(exe, {"HOS_INPUT_RECORD": str(acts), "ACTIONAT": "50", "FAKEOPT": "33:1@60"}, 150)
    lines = [json.loads(ln) for ln in acts.read_text(encoding="utf-8").splitlines() if ln.strip()]
    check([e for e in lines if e.get("cmd") == "action"] == [{"f": 29, "cmd": "action", "a": 1005}],
          "the action line: %s" % [e for e in lines if e.get("cmd") == "action"])
    opts = [(e["f"], e["id"], e["v"]) for e in lines if e.get("cmd") == "option"]
    check(opts == [(0, 32, 0), (0, 33, 0), (0, 35, 0), (0, 36, 0), (0, 55, 0), (39, 33, 1)], "the option lines: %s" % opts)
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(acts), "SCRAMBLE": "1", "ACTIONAT": "50"}, 170)
    check(got["ACTION"] == {50: ["1005", "0"], 53: ["0", "1"]}, "the replay's actions: %s" % got["ACTION"])
    check(any("1 of 1 actions given (0 not); 1 live actions not taken" in ln for ln in logs), "actions' end: %s" % logs[-3:])
    check(got["OPT"][59] == ["0"] and all(got["OPT"][fr] == ["1"] for fr in range(60, 150)) and got["OPT"][160] == ["0"],
          "the replay's flight row: %s" % [(fr, got["OPT"][fr]) for fr in (59, 60, 149, 150, 160)])
    again2 = tmp / "acts-again.jsonl"   # (a replay recorded too writes the recording's rows, not the player's)
    run(exe, {"HOS_INPUT_PLAY": str(acts), "HOS_INPUT_RECORD": str(again2), "SCRAMBLE": "1", "FAKEOPT": "33:7@0"}, 170)
    mine = [json.loads(ln) for ln in again2.read_text(encoding="utf-8").splitlines() if ln.strip()]
    check([(e["f"], e["id"], e["v"]) for e in mine if e.get("cmd") == "option"] == opts,
          "a recorded replay's rows: %s" % [(e["f"], e["id"], e["v"]) for e in mine if e.get("cmd") == "option"])
    print("PASS: Debug tools actions given at their frame, the player's not taken; the rows the recording's while it plays")

    # a load (a teleport): Link not updated on frames 60-79 of the recording. A replay whose load takes 30 frames waits at
    # the anchor and goes on from it; one whose load takes 12 jumps to it. Either way the pads after the load are the
    # recording's after its load (B from the recording's frame 100: 10 frames later, or 8 earlier), Link on his marks
    loads = tmp / "load.jsonl"
    run(exe, {"HOS_INPUT_RECORD": str(loads), "LOAD": "60:20"}, 150)
    lines = [json.loads(ln) for ln in loads.read_text(encoding="utf-8").splitlines() if ln.strip()]
    check([e for e in lines if e.get("cmd") == "anchor"] == [{"f": 59, "cmd": "anchor", "n": 0, "gap": 21}],
          "the anchor line: %s" % [e for e in lines if e.get("cmd") == "anchor"])
    for length, shift in ((30, 10), (12, -8)):
        got, logs = run(exe, {"HOS_INPUT_PLAY": str(loads), "SCRAMBLE": "1", "LOAD": "60:%d" % length, "HOS_INPUT_SYNC": "0"}, 175)
        reads = got["READ"]

        def bits_ly(fr):   # (the recording's pad at its frame fr: B and the tilt from 100, the stick up before)
            return ["2", "1234"] if fr >= 100 else ["0", "32767"]
        after = range(60 + length + 1, 150 + shift)
        wrong = [(fr, reads[fr]) for fr in after if [reads[fr][1], reads[fr][3]] != bits_ly(fr - shift)]
        check(not wrong, "a %d-frame load: pads after it not the recording's: %s" % (length, wrong[:4]))
        check(any("1 of 1 anchors met" in ln for ln in logs) and
              any("playback over at frame 129 of 129: 0 of" in ln for ln in logs), "a %d-frame load: %s" % (length, logs[-4:]))
        if length > 20:
            check(any("waits at frame 59 for its load to end" in ln for ln in logs) and
                  all([reads[fr][1], reads[fr][3]] == ["0", "32767"] for fr in range(81, 91)),
                  "the wait: %s" % [reads[fr] for fr in range(80, 92)])
    print("PASS: a load longer or shorter than the recording's: the replay re-based at its anchor, Link on his marks")

    # started at run time (the menu's Record and Play rows, the bridge): a recording from Link's update after frame 25 to
    # frame 100, played the same way in another run; a recording on another map than Link is on is not played
    runtime = tmp / "runtime.jsonl"
    got, logs = run(exe, {"RUNREC": str(runtime), "FAKEMAP": "Field"}, 150)
    lines = [json.loads(ln) for ln in runtime.read_text(encoding="utf-8").splitlines() if ln.strip()]
    check(got["RECSTART"] == {25: ["1"]} and lines[0]["map"] == "Field" and lines[0]["profile"] == "full" and
          any("recorded 74 frames" in ln for ln in logs), "run-time recording: %s %s" % (lines[0], logs[-2:]))
    got, logs = run(exe, {"RUNPLAY": str(runtime), "SCRAMBLE": "1", "FAKEMAP": "Field"}, 150)
    reads = got["READ"]
    check(got["PLAYSTART"] == {25: ["1"]} and all(reads[fr][0] == "1" for fr in range(26, 100)) and reads[100][0] == "0" and
          all([reads[fr][1], reads[fr][3]] == (["1", "0"] if 30 <= fr < 40 else ["0", "32767" if fr >= 40 else "0"])
              for fr in range(26, 100)),
          "run-time playback: %s" % [(fr, reads[fr]) for fr in range(24, 42)])
    # Link in another map: the Debug tab's teleport to the recording's first (asked at his update, taken at the frame
    # after, a load of 15 frames), then the start after his next update there, and Link put at the recording's start
    got, logs = run(exe, {"RUNPLAY": str(runtime), "SCRAMBLE": "1", "FAKEMAP": "Market"}, 150)
    reads = got["READ"]
    check(got["PLAYSTART"] == {25: ["1"]} and got["TELEPORT"] == {26: ["Field"]} and
          any("Link is in Market and the recording starts in Field: the teleport there first" in ln for ln in logs) and
          all(reads[fr][0] == "0" for fr in range(43)) and all(reads[fr][0] == "1" for fr in range(43, 117)) and
          all([reads[fr][1], reads[fr][3]] == (["1", "0"] if 30 <= fr - 17 < 40 else ["0", "32767" if fr - 17 >= 40 else "0"])
              for fr in range(43, 117)) and 43 in got["SNAP"],
          "another map: %s %s %s" % (got["TELEPORT"], logs[-4:], [(fr, reads[fr]) for fr in range(40, 46)]))
    norec = tmp / "nowhere.jsonl"
    run(exe, {"RUNREC": str(norec), "FAKEMAP": "Nowhere"}, 110)
    got, logs = run(exe, {"RUNPLAY": str(norec), "FAKEMAP": "Market"}, 60)
    check(got["PLAYSTART"] == {25: ["-2"]} and all(v[0] == "0" for v in got["READ"].values()) and
          any("starts in Nowhere, which no teleport goes to" in ln for ln in logs), "a map no teleport goes to: %s" % logs)
    print("PASS: recorded and played from the run's middle (the menu, the bridge); from another map, the teleport there "
          "first; a map no teleport goes to, not played")

    # a recording made above 60 fps (several of the game's frames to one of its own) plays: its frames lines in order
    half = tmp / "half.jsonl"
    run(exe, {"HOS_INPUT_RECORD": str(half), "CLOCKSTEP": "0.5", "FAKE_REALTIME": "1"}, 300)
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(half), "SCRAMBLE": "1", "CLOCKSTEP": "0.5", "FAKE_REALTIME": "1"}, 320)
    check(not any("is not ls-input-recording" in ln for ln in logs) and any("playback over" in ln for ln in logs),
          "a recording made above 60 fps: %s" % logs[:4])
    print("PASS: a recording made above 60 fps plays back")

    # a load the recording does not have: said, and not counted, so the recording's later anchors still match
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "SCRAMBLE": "1", "LOAD": "60:20", "HOS_INPUT_SYNC": "0"}, 170)
    check(sum("the recording does not have there" in ln for ln in logs) == 1, "an unmatched load: %s" % logs[-4:])
    print("PASS: a load the recording does not have: a divergence, not an anchor")

    # a route (HOS_INPUT_ROUTE=1): no guest words or addresses (the camera's angles alone, no menu mouse), its save named
    route = tmp / "route.jsonl"
    run(exe, {"HOS_INPUT_RECORD": str(route), "HOS_INPUT_ROUTE": "1", "HOS_INPUT_SAVE": "kokiri-start"}, 150)
    lines = [json.loads(ln) for ln in route.read_text(encoding="utf-8").splitlines() if ln.strip()]
    check(lines[0]["profile"] == "route" and lines[0]["save"] == "kokiri-start" and
          not any("w" in e or "in" in e or e.get("cmd") in ("focus", "itemkey") for e in lines[1:]) and
          any(e.get("cmd") == "view" and "yaw" in e for e in lines), "a route's lines: %s" % lines[:3])
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(route), "SCRAMBLE": "1", "VIEWDRIFT": "100"}, 170)
    check(all(got["VIEW"][fr][0] == "%g" % (0.25 * fr) for fr in range(21, 150)) and
          any("playback over at frame 129 of 129: 0 of 129 updates of Link's apart" in ln for ln in logs),
          "a route's replay: the camera's angles not put back, or Link apart: %s" % logs[-3:])
    print("PASS: a route holds no guest words; its replay puts the camera's angles back")

    # a run above 60 fps (dev.py ab --route measures uncapped): the game clock half a frame a frame. The frames are the
    # clock's sixtieths of a second: each read gets the recording's pad of the frame the clock is in, the route at its own
    # speed (two of the game's frames to each of the recording's)
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(rec), "SCRAMBLE": "1", "CLOCKSTEP": "0.5", "FAKE_REALTIME": "1"}, 300)

    def rec_bits(n):   # (the recording's button word at its frame n: its game frame n + 21)
        fr = n + 21
        return "1" if 30 <= fr < 40 else "2" if fr >= 100 else "0"
    played = [(fr, v) for fr, v in got["READ"].items() if v[0] == "1"]
    wrong = [(fr, v[1], rec_bits(int(0.5 * fr - 10.5 + 0.5))) for fr, v in played if v[1] != rec_bits(int(0.5 * fr - 10.5 + 0.5))]
    check(len(played) > 250 and not wrong and any("follows the recording by the game clock's time" in ln for ln in logs),
          "above 60 fps: %d reads played, wrong %s; %s" % (len(played), wrong[:4], logs[:3]))
    print("PASS: above 60 fps the recording plays at its own speed, by the game clock's time")

    # from the boot: read 0 is the game's first; the wall-clock step is said
    rec2 = tmp / "boot.jsonl"
    got, logs = run(exe, {"HOS_INPUT_RECORD": str(rec2), "HOS_INPUT_FROM": "boot", "FAKE_REALTIME": "1"}, 50)
    head = json.loads(rec2.read_text(encoding="utf-8").splitlines()[0])
    check(head["from"] == "boot" and head["step"] == "wall clock" and head["start"] == 100, "boot header: %s" % head)
    check(any("follows the wall clock" in ln for ln in logs), "no word of the wall-clock step: %s" % logs)
    bad = tmp / "bad.jsonl"
    bad.write_text('{"format":"something/1"}\n', encoding="utf-8")
    got, logs = run(exe, {"HOS_INPUT_PLAY": str(bad)}, 30)
    check(all(v[0] == "0" for v in got["READ"].values()) and any("is not ls-input-recording/1" in ln for ln in logs),
          "a bad file played: %s" % logs)
    print("PASS: from boot, wall clock, a bad file")

    # two reads a frame: recorded with Link's first update after a frame's reads, played with it between them (the start
    # then came half a frame apart: every read given its neighbour's), and with a frame of three reads
    rec3 = tmp / "two.jsonl"
    got, logs = run(exe, {"TWOREADS": "1", "HOS_INPUT_RECORD": str(rec3)}, 60)
    recorded = {k: v for k, v in got["READ2"].items() if k[0] >= 21}
    lines = [json.loads(ln) for ln in rec3.read_text(encoding="utf-8").splitlines() if ln.strip()]
    check([e for e in lines if e.get("cmd") == "frames"] == [{"f": 0, "cmd": "frames", "count": 39, "reads": 2, "read": 0}],
          "two reads' frames: %s" % [e for e in lines if e.get("cmd") == "frames"])
    for env, what in (({"LINKMID": "1"}, "Link's first update between the reads"), ({"EXTRA": "30"}, "a frame of three reads")):
        got, logs = run(exe, dict(env, TWOREADS="1", SCRAMBLE="1", HOS_INPUT_PLAY=str(rec3)), 60)
        played = got["READ2"]
        check(all(played[(fr, j)][0] == "0" for fr in range(21) for j in range(2)), "%s: a read before the start played" % what)
        check(all(recorded[(fr, 1)] == ["1", "%x" % (0x100 * fr)] for fr in range(21, 60)),
              "recording: a frame's second read not given its first read's pad")
        wrong = [(k, played[k]) for k in played if k[0] >= 21 and played[k] != ["1", "%x" % (0x100 * k[0])]]
        check(not wrong, "%s: reads given another frame's pad: %s" % (what, wrong[:4]))
        if "EXTRA" in env:
            check(played[(30, 2)] == ["1", "1e00"] and any("frame 9 had 2 reads in the recording, 3 here" in ln for ln in logs) and
                  any("1 frames had another count of reads" in ln for ln in logs), "the extra read: %s %s" % (played[(30, 2)], logs))
        else:
            check(any("0 frames had another count of reads" in ln for ln in logs), "%s: %s" % (what, logs))
    print("PASS: two reads a frame: the start a frame's first read, every read of a frame its pad, a frame of three reads too")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="gcc")
    a = ap.parse_args()
    wiring()
    cc = shutil.which(a.cc)
    if not cc:
        print("SKIP: no %s" % a.cc)
        return 0
    with tempfile.TemporaryDirectory(prefix="longshot-input-movie-") as d:
        tmp = Path(d)
        (tmp / "t.c").write_text(HEADER_FIXTURE, encoding="utf-8")
        exe = tmp / ("t.exe" if os.name == "nt" else "t")
        compile_c(cc, tmp / "t.c", exe, ["-I", str(ROOT / "host")])
        r = subprocess.run([str(exe)], text=True, capture_output=True, timeout=60)
        check(r.returncode == 0, "format fixture:\n" + r.stdout + r.stderr)
        print("PASS: format")
        glue(cc, tmp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
