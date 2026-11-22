/* (longshot, issue 1113) Input recordings for repeatable test runs and benchmarks: the glue of host/input_movie.h (what a
   recording is, and the format) to the switches, the menu and bridge controls, the file and the log. The places that call
   here are the ones where input reaches the game: host/nn_hid.c (each read of controller No.1, and of the motion sensors),
   host/mouse_native.c (the native mouse camera, in the player camera's update), host/oot_menu.c (the mouse in the game's
   menus), host/hos_main.c (Link's update: the marks, the anchors and the Debug tools' actions) and host/rx_backend.c
   (rx_option: the Debug tools' rows a replay answers).

   A recording starts from the switches at the game's start (HOS_INPUT_RECORD, HOS_INPUT_PLAY), or at run time from the
   Insert menu's Debug tab or the control bridge (hos_input_record_start, hos_input_play_start); it ends at its stop, the
   end of the file it plays, or the game's close.

   HOS_INPUT_RECORD=<file>: writes what the game was given. HOS_INPUT_FROM=play (the default) starts at the first read of
   the game frame after Link's first update, so a load that takes more or fewer frames on another run or PC does not move
   the inputs against the game; =boot starts at the first read of the game (the title, its menus and the load are in the
   recording). The unit is the game frame, by the game clock (hos_step_probe's +0xd58: +1 a frame at the Per frame step):
   the game reads controller No.1 from its job graphs, twice in most frames, once or three times in some, while Link's
   update runs on another worker. An input that changed between a frame's two reads reached Link or not as the race went,
   so replays given the same reads left the recording at the first stick change (2026-11-18: the same update, 1348, in
   every pair of runs). So a frame has one pad: recording, its first read takes the input and its later reads are given
   the same; a replay gives every read of frame n the recording's pad of frame n. The reads each frame had are written
   down (frames lines) for a look, not used.
   Loads after the start (a teleport, a door, the age switch) are anchors: Link's first update after MOVIE_ANCHOR_GAP
   frames without one. A replay re-bases its frames at each of its own anchors to the recording's, and when its load takes
   longer it waits at the anchor's frame (the pad given again) until its own: the input after a load is where it was
   against the game, however long the load took.
   The Debug tools' actions (teleports, the age, items, Mark and Return, the hour) are recorded where the game takes them,
   and the Debug tools' rows that change what the game does (heart protection, flight, the free camera, Link's speed,
   damage taken) at the start and at each change: a replay takes no live action (the menu's, HOS_INPUT_LIVE's or the
   bridge's), gives the recorded ones at their frame, and answers rx_option for those rows with the recording's values,
   so the player's own menu does not change the replay, nor the replay the player's menu.
   HOS_INPUT_PLAY=<file>: gives the game the recording from the same point (the header's "from"), and nothing of the player's
   devices, the script or the bridge until it ends; then the game's input is the player's again. Each of Link's updates is
   held to the recording's marks: the first that differs is logged with the frame and how far apart, and the end says how
   many did. Both set: the replay is recorded too (to HOS_INPUT_RECORD): its own marks, views and frames beside the
   recording's, for analysis/ or a test to set side by side (the same pads, written again), and a result line at its end.
   Not to the file it plays (the same name, a link or another spelling of it): its start would open that for writing and
   wipe it, so it is refused and the playback goes on unrecorded.
   Neither set (a player's game): every hook returns after one atomic load (movie_mode_now), and its callers read and copy
   nothing for it (hos_input_movie_active).
   HOS_INPUT_SYNC=<updates> (with a playback; 1 by default, 0 off): where the replay leaves a fingerprint it is put back,
   at every <updates>-th update: the camera's state block (the view line's 14 words) at the start of its update, its
   inputs first, and Link at the mark's position through the game's own move of his character controller
   (host/hos_main.c). Only while it is close: Link further than HOS_INPUT_SYNC_MAX metres (1 by default) from the mark is
   a replay that went another way (a hit taken, a door missed), not float noise: it is said once ("diverged") and nothing
   is put back after it, so a test fails on it rather than being steered back. The input stays the recording's; what else
   the game has (velocities, other actors, timers) is not put back, so a corrected replay follows the recording without
   proving that it would have: HOS_INPUT_SYNC=0 is the exact test. Every correction is counted, and the end says how
   many, the largest and the mean.
   A recording made for assets/routes/ (HOS_INPUT_ROUTE=1, or the bridge's record route): no guest words and no addresses,
   so it can be committed: views with the yaw and pitch alone, no menu mouse lines (a route uses the controller), and the
   save it starts from in its header (HOS_INPUT_SAVE).

   A replay is exact only where the game is: the 60 cap with the Per frame game speed (HOS_REALTIME_STEP=0), the same clock
   (HOS_FIXED_TIME), the same save and the same settings. The log says at the start when the step is the wall clock's: a
   cap above 60 or Real time, where the frames are the game clock's sixtieths of a second, several of the game's frames to
   one, and a replay follows the recording by its time (corrected at each update), not frame for frame. A run that records
   or plays from its start also takes the host's own time-based choices by the game clock (hos_input_movie_time), and has
   no movie pacing (host/nn_vi.c). */
#include "hos.h"
#include "input_movie.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>   /* (SRWLOCK, LONG64; on Linux host/posix's) */
#ifndef _WIN32
#include <sys/stat.h>   /* (movie_same_file) */
#endif

extern volatile LONG64 hos_present_count;          /* (host/nvn_native.c: the game's presents) */
extern volatile uint32_t hos_frame_seconds_bits;   /* (host/nn_vi.c: 0 while the step is the game's own) */
extern volatile uint64_t hos_step_probe;           /* (host/nn_vi.c: the game's step object, its +0xd58 the game clock) */
int rx_option(int id, int otherwise);              /* (host/rx_backend.c: 0, the Game speed row: 1 Real time) */
const char *hos_version(void);                     /* (host/hos_main.c) */
int hos_oot_current_map(char *out, size_t cap);    /* (host/hos_main.c: the map Link was last seen in) */
int hos_oot_state(int *event, int *loading, int *child);   /* (host/hos_main.c: an event playing, a load, Link the child) */
int hos_oot_destination(int index, const char **child, const char **adult);   /* (the Debug tab's teleports' maps) */
int hos_oot_teleport_ask(int index);               /* (host/hos_main.c: the bridge's teleport, taken at Link's update) */
void hos_oot_teleport_cancel(int ticket);          /* (host/hos_main.c: an ask not taken yet, dropped) */
int hos_oot_age_ask(int adult);                    /* (likewise the age switch) */
extern const char *hos_user_dir;                   /* (host/nn_fs.c: the player's folder) */

enum { MOVIE_OFF, MOVIE_RECORD, MOVIE_PLAY };
#define MOVIE_SETTLE 30   /* (Link's updates a run-time playback holds him to the recording from its start: half a second) */

static SRWLOCK movie_lock = SRWLOCK_INIT;
/* (-1 until the first hook decides from the switches; then OFF, RECORD or PLAY, changed only under movie_lock (a start, a
   stop, an end) and read without it, so a player's game, with nothing recorded or played, returns from every hook before
   any lock, guest read or copy: movie_mode_now) */
static int movie_mode = -1;

/* (one recording or playback: started at a start, cleared at its end (movie_reset); under movie_lock) */
typedef struct {
    int from_play, started, over;
    int rec;                     /* (a recording is written: alone, or a replay's) */
    int route;                   /* (a route's: no guest words, no addresses) */
    int runtime;                 /* (started from the menu or the bridge: Link put at the recording's start first) */
    uint32_t reads;              /* (controller reads since the start) */
    uint32_t updates;            /* (Link's updates seen, all of them; k0 the count at the start) */
    uint32_t k0;
    uint32_t cams, gyros;        /* (the camera's updates and the motion reads since the start) */
    uint32_t cams_before;        /* (the camera's updates before the start: what a start shares with another) */
    FILE *file;
    MovieRun run;
    MovieAt cam_at, gyro_at, link_at;   /* (each hook's calls within the frame: the points' i) */
    MovieCamRec cam_rec;
    MovieGyroRec gyro_rec;
    float last_pos[3];
    int have_pos;
    MovieTape tape;
    uint32_t drifts, checked, menu_missed, menu_given;
    double last_clock, link_clock;      /* (the game clock at the last read, at Link's first update) */
    double last_update_clock;           /* (at Link's last update: a gap past MOVIE_ANCHOR_GAP makes the next an anchor) */
    double clock0;                      /* (frame 0's clock; a replay moves it at each anchor) */
    int by_clock;                       /* (frames are the clock's: the Per frame step) */
    uint32_t frame, frame_first, resyncs;   /* (the frame since the start, the read it began at) */
    MoviePad frame_pad;                 /* (the frame's pad: taken at its first read, or the recording's) */
    int64_t pad_frame;                  /* (the frame frame_pad is for) */
    MovieFramesRec frames_rec;
    MovieView last_view;
    int have_view;
    uint32_t view_drifts, view_checked, input_drifts;
    int sync;                           /* (HOS_INPUT_SYNC: put back at every sync-th update; 0 never) */
    float sync_max;                     /* (HOS_INPUT_SYNC_MAX: metres past which Link is not put back: diverged) */
    uint32_t syncs_input, syncs_view, syncs_link;
    float sync_largest;                 /* (metres, Link's largest correction) */
    double sync_total;
    int diverged;
    uint32_t anchors;                   /* (anchors seen since the start: the next one's n) */
    uint32_t anchors_met;
    int held;                           /* (a replay waiting at an anchor's frame for its own load to end) */
    uint32_t held_frames;
    double held_clock;                  /* (the game clock when the wait began) */
    int32_t opt_rec[MOVIE_OPTIONS];     /* (recording: the options last written) */
    uint32_t opt_rec_have;
    uint32_t actions_given, actions_dropped;
    /* (a playback started at run time, before its start: Link taken to the recording's age and map first, through the
       Debug tab's age switch and teleport (movie_prep_step); prep 1 while that goes on) */
    int prep, want_child, want_dest, prep_tries, prep_step, prep_ticket;   /* (prep_step: 1 the age switch, 2 the teleport) */
    int64_t prep_at;
    char path[1024], rec_path[1024], map[64];
} MovieState;
static MovieState S;

/* (a replay's Debug tools rows, as rx_option answers them: written under movie_lock, read without it from any thread) */
static int movie_opt_on;
static int32_t movie_opt_v[MOVIE_OPTIONS];
static uint32_t movie_opt_have;
static char movie_last_path[1024];   /* (the last recording written: the menu's Last recording) */

static void movie_reset(void) {   /* (under movie_lock: nothing recorded or played) */
    movie_tape_free(&S.tape);
    memset(&S, 0, sizeof S);
    S.from_play = 1, S.pad_frame = -1, S.sync = 1, S.sync_max = 1.0f;
    S.last_clock = S.link_clock = S.last_update_clock = -1.0;
}

/* the game clock (1/60 s, +1 a frame at the 60 cap's Per frame step), or -1 in a build without the step's hook */
static double movie_clock(void) {
    const uint64_t step = __atomic_load_n(&hos_step_probe, __ATOMIC_RELAXED);
    if (!step) return -1.0;
    double v;
    memcpy(&v, GP(step + 0xd58), sizeof v);
    return v;
}

static int movie_on(void) {   /* (started and not over: under movie_lock) */
    return movie_mode > MOVIE_OFF && S.started && !S.over;
}

/* (under movie_lock, started) The frame a hook runs in, by the game clock as the pads' frames are (else the pads' frame):
   from the camera's, the motion's and Link's own threads, so not the frame the last controller read began. A replay waiting
   at an anchor stays at its frame */
static uint32_t movie_hook_frame(void) {
    if (!S.by_clock || S.held) return S.frame;
    const double d = movie_clock() - S.clock0;
    return d > 0 ? (uint32_t)(d + 0.5) : 0;
}

static void movie_write(const char *line, int n) {
    if (!S.file || n <= 0) return;
    if (fwrite(line, 1, (size_t)n, S.file) != (size_t)n || fflush(S.file)) {
        hos_log("input: writing the recording %s failed: it stops here", S.rec_path);
        fclose(S.file);
        S.file = NULL;
    }
}

static void movie_summary(const char *how) {
    hos_log("input: playback %s at frame %u of %u: %u of %u updates of Link's apart from the recording; %u menu choices given, "
            "%u not met; %u of %u camera updates apart from it (its inputs at %u); %u frames had another count of reads", how,
            S.frame, S.tape.end, S.drifts, S.checked, S.menu_given, S.menu_missed, S.view_drifts, S.view_checked,
            S.input_drifts, S.resyncs);
    if (S.sync)
        hos_log("input: HOS_INPUT_SYNC: put back the camera's inputs at %u updates, the camera at %u and Link at %u (the "
                "largest %.4f m, the mean %.4f m)%s", S.syncs_input, S.syncs_view, S.syncs_link, (double)S.sync_largest,
                S.syncs_link ? S.sync_total / S.syncs_link : 0.0, S.diverged ? "; diverged" : "");
    size_t untaken = 0;
    for (size_t i = 0; i < S.tape.naction; i++) untaken += !S.tape.action[i].used;
    if (S.tape.nanchor || S.tape.naction || S.actions_dropped)
        hos_log("input: %u of %zu anchors met; %u of %zu actions given (%zu not); %u live actions not taken", S.anchors_met,
                S.tape.nanchor, S.actions_given, S.tape.naction, untaken, S.actions_dropped);
}

/* (under movie_lock) The replay went another way: said once, and nothing put back from here */
static void movie_diverge(uint32_t fr, const char *why) {
    if (S.diverged) return;
    S.diverged = 1;
    hos_log("input: the replay diverged at frame %u: %s; nothing is put back from here", fr, why);
}

/* (under movie_lock) The end of a recording or a playback, by its stop, its file's end or the game's close: the last
   lines written, the file closed, the playback's count said; then nothing recorded or played */
static void movie_list_changed(void);   /* (below: the recordings folder listed again) */

static void movie_finish(const char *how) {
    const int mode = movie_mode;
    if (S.rec && S.file) {
        char line[512];
        MovieFrames done;
        if (S.started && S.reads > S.frame_first && !S.held &&   /* (the last frame, if it had reads: a replay ends at one that had none) */
            movie_frames_step(&S.frames_rec, S.frame, S.frame_first, S.reads - S.frame_first, &done))
            movie_write(line, movie_frames_line(line, sizeof line, &done));
        if (S.frames_rec.open) movie_write(line, movie_frames_line(line, sizeof line, &S.frames_rec.run));
        S.frames_rec.open = 0;
        if (S.run.open) movie_write(line, movie_pad_line(line, sizeof line, S.run.f, S.run.frames, &S.run.pad));
        S.run.open = 0;
        if (mode == MOVIE_PLAY && S.started) {   /* (a replay's own recording: how it went, for a test or dev.py ab) */
            const int n = snprintf(line, sizeof line, "{\"f\":%u,\"cmd\":\"result\",\"link_apart\":%u,\"link_checked\":%u,"
                                   "\"camera_apart\":%u,\"camera_checked\":%u,\"syncs\":%u,\"largest\":%.4f,\"diverged\":%d,"
                                   "\"anchors\":%u,\"anchors_wanted\":%zu,\"ended\":\"%s\"}\n", S.frame, S.drifts, S.checked,
                                   S.view_drifts, S.view_checked, S.syncs_link, (double)S.sync_largest, S.diverged,
                                   S.anchors_met, S.tape.nanchor, how);
            movie_write(line, n > 0 && (size_t)n < sizeof line ? n : 0);
        }
        if (S.file) fclose(S.file);
        S.file = NULL;
        hos_log("input: recorded %u frames (%u reads), %u camera updates and %u motion reads to %s", S.frame + 1, S.reads,
                S.cams, S.gyros, S.rec_path);
        if (mode == MOVIE_RECORD) snprintf(movie_last_path, sizeof movie_last_path, "%s", S.rec_path);
        movie_list_changed();   /* (a new file in the folder, maybe: listed at once) */
    }
    if (mode == MOVIE_PLAY && S.started && !S.over) {
        S.over = 1;
        movie_summary(how);
    }
    __atomic_store_n(&movie_opt_on, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&movie_mode, MOVIE_OFF, __ATOMIC_RELEASE);
    movie_reset();
}

/* (the game's close work, host/hos_relaunch.c hos_close_finish, and atexit for any other way out: ExitProcess runs no atexit
   handler of the exe's. Once: the last run written, the file closed, the playback's count said) */
void hos_input_movie_close(void) {
    AcquireSRWLockExclusive(&movie_lock);
    static int closed;
    if (!closed) {
        closed = 1;
        if (movie_mode > MOVIE_OFF) movie_finish("stopped");
    }
    ReleaseSRWLockExclusive(&movie_lock);
}
static void movie_at_exit(void) { hos_input_movie_close(); }

static int movie_load(const char *path) {   /* (into S.tape) */
    FILE *f = fopen(path, "rb");
    if (!f) {
        hos_log("input: playback: cannot open %s: no playback", path);
        return 0;
    }
    static char line[8192];
    unsigned n = 0;
    int ok = 1;
    while (fgets(line, sizeof line, f)) {
        n++;
        if (movie_parse_line(&S.tape, line) < 0) {
            hos_log("input: playback: line %u of %s is not %s: no playback", n, path, MOVIE_FORMAT);
            ok = 0;
            break;
        }
    }
    fclose(f);
    if (ok && !S.tape.header) {
        hos_log("input: playback: %s has no %s header: no playback", path, MOVIE_FORMAT);
        ok = 0;
    }
    if (!ok) {
        movie_tape_free(&S.tape);
        return 0;
    }
    S.from_play = !strcmp(S.tape.from, "play");
    hos_log("input: playing %s: %u frames in %zu runs, %zu camera lines, %zu motion reads, %zu menu choices, %zu marks, "
            "%zu actions, %zu anchors, from %s%s", path, S.tape.end, S.tape.ne, S.tape.ncam, S.tape.ngyro, S.tape.nmenu,
            S.tape.nm, S.tape.naction, S.tape.nanchor, S.from_play ? "Link's first update" : "the game's first read",
            S.tape.skipped ? " (other commands passed over)" : "");
    return 1;
}

/* Whether two paths name one file: the same name, or (a link, another spelling, a hard link) the same file on the same
   volume. A path that does not name a file yet is no other file. (Paths here are UTF-8 strings: the exe's manifest makes
   the process's code page UTF-8, so getenv, fopen and the A functions take them as they are: nx2/win_manifest.py) */
#ifdef _WIN32
static int movie_file_id(const char *path, BY_HANDLE_FILE_INFORMATION *id) {
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    const int ok = GetFileInformationByHandle(h, id) != 0;
    CloseHandle(h);
    return ok;
}
static int movie_same_file(const char *a, const char *b) {
    if (!_stricmp(a, b)) return 1;
    BY_HANDLE_FILE_INFORMATION x, y;
    return movie_file_id(a, &x) && movie_file_id(b, &y) && x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
           x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}
#else
static int movie_same_file(const char *a, const char *b) {
    if (!strcmp(a, b)) return 1;
    struct stat x, y;
    return !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
#endif

/* (HOS_INPUT_SYNC and HOS_INPUT_SYNC_MAX, as a playback started from the switches takes them) */
static void movie_sync_switches(int *sync, float *sync_max) {
    const char *s = getenv("HOS_INPUT_SYNC"), *m = getenv("HOS_INPUT_SYNC_MAX");
    *sync = 1, *sync_max = 1.0f;
    if (s && *s) {
        char *end;
        const long v = strtol(s, &end, 10);
        if (*end || v < 0 || v > 1000000) hos_log("input: HOS_INPUT_SYNC=%s is not a count of updates: 1 (every update)", s);
        else *sync = (int)v;
    }
    if (m && *m) {
        const float v = strtof(m, NULL);
        if (isfinite(v) && v > 0) *sync_max = v;
        else hos_log("input: HOS_INPUT_SYNC_MAX=%s is not a distance in metres: 1", m);
    }
}

/* (under movie_lock, S reset: a playback of path set up, mode PLAY; 0 when the file does not load) */
static int movie_play_setup(const char *path, int sync, float sync_max) {
    snprintf(S.path, sizeof S.path, "%s", path);
    if (!movie_load(path)) return 0;
    S.sync = sync, S.sync_max = sync_max;
    if (S.sync)
        hos_log("input: HOS_INPUT_SYNC=%d: Link and the camera are put back where the replay leaves the recording, at every "
                "%d-th update, while Link is within %.2f m (HOS_INPUT_SYNC_MAX)", S.sync, S.sync, (double)S.sync_max);
    return 1;
}

/* (once, at the first hook: what this run does, from the switches; under movie_lock) */
static void movie_decide(void) {
    movie_reset();
    const char *rec = getenv("HOS_INPUT_RECORD"), *play = getenv("HOS_INPUT_PLAY");
    int mode = MOVIE_OFF;
    if (rec && *rec) S.rec = 1, snprintf(S.rec_path, sizeof S.rec_path, "%s", rec);
    if (play && *play) {
        int sync;
        float sync_max;
        movie_sync_switches(&sync, &sync_max);
        if (movie_play_setup(play, sync, sync_max)) mode = MOVIE_PLAY;
        /* (the replay recorded to the file it plays: its start would open that with "wb" and wipe the recording. Refused,
           the playback kept: "record a replay too" names another file) */
        if (S.rec && mode == MOVIE_PLAY && movie_same_file(rec, play)) {
            hos_log("input: HOS_INPUT_RECORD names the recording HOS_INPUT_PLAY plays (%s): the replay is not recorded, so the "
                    "recording is kept; name another file to record the replay", rec);
            S.rec = 0;
        }
        if (S.rec && mode == MOVIE_PLAY) hos_log("input: the replay is recorded too, to %s", rec);
        if (mode != MOVIE_PLAY) movie_reset();   /* (a playback that cannot play records nothing: it is no replay) */
    } else if (rec && *rec) {
        const char *from = getenv("HOS_INPUT_FROM");
        S.from_play = !(from && !strcmp(from, "boot"));
        if (from && *from && strcmp(from, "boot") && strcmp(from, "play"))
            hos_log("input: HOS_INPUT_FROM=%s is neither boot nor play: from play", from);
        const char *route = getenv("HOS_INPUT_ROUTE");
        S.route = route && *route && strcmp(route, "0");
        mode = MOVIE_RECORD;
        hos_log("input: recording to %s from %s%s", rec, S.from_play ? "Link's first update" : "the game's first read",
                S.route ? " (a route: no guest words)" : "");
    }
    static int at_exit;
    if (!at_exit) at_exit = 1, atexit(movie_at_exit);
    __atomic_store_n(&movie_mode, mode, __ATOMIC_RELEASE);   /* (last: a hook that sees it sees the rest decided) */
}

/* The run's mode, decided at the first call: after that one atomic load, so every hook of a player's game (nothing
   recorded or played) returns at once */
static int movie_mode_now(void) {
    int mode = __atomic_load_n(&movie_mode, __ATOMIC_ACQUIRE);
    if (mode >= 0) return mode;
    AcquireSRWLockExclusive(&movie_lock);
    if (movie_mode < 0) movie_decide();
    mode = movie_mode;
    ReleaseSRWLockExclusive(&movie_lock);
    return mode;
}

/* (the hooks' callers: whether to read and copy what a recording takes at all; 0 with nothing recorded or played) */
int hos_input_movie_active(void) {
    return movie_mode_now() > MOVIE_OFF;
}

/* (under movie_lock, recording: the Debug tools rows that changed since the last frame, or all of them at the start) */
static void movie_options_record(uint32_t fr, int all) {
    static const int ids[MOVIE_OPTIONS] = MOVIE_OPTION_IDS;
    for (int i = 0; i < MOVIE_OPTIONS; i++) {
        const int v = rx_option(ids[i], 0);
        if (!all && (S.opt_rec_have >> i & 1) && S.opt_rec[i] == v) continue;
        S.opt_rec[i] = v, S.opt_rec_have |= 1u << i;
        char line[96];
        movie_write(line, movie_option_line(line, sizeof line, fr, ids[i], v));
    }
}

/* (under movie_lock, playing: the recording's Debug tools rows at frame fr, for rx_option) */
static void movie_options_play(uint32_t fr) {
    int32_t v[MOVIE_OPTIONS];
    uint32_t have = movie_opt_have;
    for (int i = 0; i < MOVIE_OPTIONS; i++) v[i] = __atomic_load_n(&movie_opt_v[i], __ATOMIC_RELAXED);
    movie_tape_options(&S.tape, fr, v, &have);
    for (int i = 0; i < MOVIE_OPTIONS; i++) __atomic_store_n(&movie_opt_v[i], v[i], __ATOMIC_RELAXED);
    __atomic_store_n(&movie_opt_have, have, __ATOMIC_RELEASE);
}

/* (rx_option, host/rx_backend.c, before it asks the menu: a replay's answer for the Debug tools rows it holds (1, *v)) */
int hos_input_movie_option(int id, int *v) {
    if (!__atomic_load_n(&movie_opt_on, __ATOMIC_ACQUIRE)) return 0;
    const int slot = movie_option_slot(id);
    if (slot < 0 || !(__atomic_load_n(&movie_opt_have, __ATOMIC_ACQUIRE) >> slot & 1)) return 0;
    *v = __atomic_load_n(&movie_opt_v[slot], __ATOMIC_RELAXED);
    return 1;
}

/* (the start: the header, and what makes a replay differ from the run said once) */
static void movie_start(double clock) {
    S.started = 1, S.reads = 0, S.k0 = S.updates, S.cams = S.gyros = 0;
    const double clock_from = clock - S.link_clock;
    const int realtime = rx_option(0, 1) != 0, timed = __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0;
    /* (frames are the game clock's sixtieths of a second, whatever the step: at the 60 cap's Per frame step one a frame; at
       a cap above 60 or Real time several of the game's frames share one, so a route recorded at 60 plays at its own speed
       in a run measured uncapped (dev.py ab --route), Link and the camera put back at each update. No clock: each change
       of it a frame) */
    S.clock0 = clock, S.by_clock = clock >= 0;
    if (realtime || timed)
        hos_log("input: the game's step follows the wall clock here (%s): a %s follows the recording by the game clock's time, "
                "not frame for frame; the 60 cap and HOS_REALTIME_STEP=0 make it the game's own", timed ? "a cap above 60" :
                "Game speed Real time", movie_mode == MOVIE_RECORD ? "replay of this recording" : "replay");
    const long long present = (long long)__atomic_load_n(&hos_present_count, __ATOMIC_RELAXED);
    if (!hos_oot_current_map(S.map, sizeof S.map)) S.map[0] = 0;
    int event, loading, child;
    const char *age = hos_oot_state(&event, &loading, &child) ? (child ? "child" : "adult") : "";
    if (S.rec) {
        S.file = fopen(S.rec_path, "wb");
        if (!S.file) hos_log("input: recording: cannot write %s: no recording", S.rec_path);
    }
    if (S.file) {
        const char *fixed = getenv("HOS_FIXED_TIME"), *save = getenv("HOS_INPUT_SAVE");
        char line[768], map[64], saved[64];
        movie_clean(S.map, map, sizeof map);
        movie_clean(save ? save : "", saved, sizeof saved);
        const int n = snprintf(line, sizeof line, "{\"format\":\"%s\",\"fps\":60,\"start\":%lld,\"source\":\"longshot\","
                               "\"from\":\"%s\",\"build\":\"%s\",\"fixed_time\":\"%s\",\"step\":\"%s\",\"camera_before\":%u,"
                               "\"link_before\":%u,\"clock\":%.1f,\"map\":\"%s\",\"age\":\"%s\",\"profile\":\"%s\","
                               "\"save\":\"%s\"}\n", MOVIE_FORMAT, present, S.from_play ? "play" : "boot", hos_version(),
                               fixed ? fixed : "", realtime || timed ? "wall clock" : "per frame", S.cams_before, S.updates,
                               clock, map, age, S.route ? "route" : "full", saved);
        movie_write(line, n > 0 && (size_t)n < sizeof line ? n : 0);
    }
    if (movie_mode == MOVIE_PLAY) {   /* (first: a replay that is recorded too writes the recording's rows, as rx_option answers) */
        movie_options_play(0);
        __atomic_store_n(&movie_opt_on, 1, __ATOMIC_RELEASE);
    }
    if (S.file) movie_options_record(0, 1);
    hos_log("input: %s from frame 0 at present %lld, game clock %.1f (%.1f after Link's first update), after %u camera updates and "
            "%u of Link's", movie_mode == MOVIE_RECORD ? "recording" : "playback", present, clock, clock_from, S.cams_before,
            S.updates);
}

/* (under movie_lock, playing: the next frame's number, held at the recording's next anchor when the replay passed it
   without its own (a longer load), and given up when the wait passes MOVIE_ANCHOR_WAIT) */
static uint32_t movie_play_next(uint32_t next, double clock) {
    uint32_t at;
    if (S.held) {   /* (the wait by the game clock's time, as the frames are: 15 s at any frame rate) */
        S.held_frames++;
        if (clock - S.held_clock > MOVIE_ANCHOR_WAIT || (clock < 0 && S.held_frames > MOVIE_ANCHOR_WAIT)) {
            char why[96];
            snprintf(why, sizeof why, "its load did not end within %d frames of the recording's (anchor %u)", MOVIE_ANCHOR_WAIT,
                     S.anchors);
            movie_diverge(S.frame, why);
            S.over = 1;
            movie_summary("over");
            hos_log("input: the player's input from here");
        }
        return S.frame;
    }
    if (movie_anchor_hold(S.tape.anchor, S.tape.nanchor, S.anchors, next, &at)) {
        S.held = 1, S.held_frames = 0, S.held_clock = clock;
        hos_log("input: playback waits at frame %u for its load to end (the recording's anchor %u)", at, S.anchors);
        return at;
    }
    return next;
}

/* ---- the controller (nn_hid.c npad_states) ---- */

/* Before each read of controller No.1: 0 when the read is to be made (the merge runs, and recording, hos_input_movie_end
   takes its pad as the frame's); 1 when *given is the read's pad: a replay's (the recording's for this frame), or
   recording, the frame's pad taken at its first read. */
int hos_input_movie_begin(MoviePad *given) {
    if (movie_mode_now() == MOVIE_OFF) return 0;   /* (a player's game: no lock, no guest read) */
    AcquireSRWLockExclusive(&movie_lock);
    const double clock = movie_clock();
    const int clocked = clock >= 0, new_frame = !clocked || clock != S.last_clock;   /* (no clock: each read a frame) */
    S.last_clock = clock;
    if (movie_mode != MOVIE_OFF && !S.started) {
        /* (a frame's first read: from play, the first frame after the one Link was first updated in) */
        if (new_frame && (!S.from_play || (S.updates > 0 && (!clocked || clock > S.link_clock)))) {
            movie_start(clock);
            S.frame = S.frame_first = 0;
        }
    } else if (movie_on() && new_frame) {
        /* (the game's frame that begins is in which of the recording's: above 60 fps several of the game's share one, and
           the frame before ends only when the recording's changes) */
        const int was_held = S.held;
        const double d = clock - S.clock0;
        uint32_t next = S.by_clock ? (d > 0 ? (uint32_t)(d + 0.5) : 0) : S.frame + 1;   /* (a frame with no read is still counted) */
        if (movie_mode == MOVIE_PLAY) next = movie_play_next(next, clock);
        if (next != S.frame || was_held) {
            if (S.rec && !was_held) {   /* (the frame before ended: its reads) */
                MovieFrames done;
                char line[128];
                if (movie_frames_step(&S.frames_rec, S.frame, S.frame_first, S.reads - S.frame_first, &done))
                    movie_write(line, movie_frames_line(line, sizeof line, &done));
            }
            uint32_t had;
            if (movie_mode == MOVIE_PLAY && !was_held && movie_tape_frame_reads(&S.tape, S.frame, &had) &&
                had != S.reads - S.frame_first) {
                if (!S.resyncs)
                    hos_log("input: frame %u had %u reads in the recording, %u here (each given the frame's pad)", S.frame, had,
                            S.reads - S.frame_first);
                S.resyncs++;
            }
            S.frame = next;
            S.frame_first = S.reads;
            if (!S.over) {
                if (movie_mode == MOVIE_PLAY) movie_options_play(S.frame);
                if (S.rec) movie_options_record(S.frame, 0);   /* (a replay's: the recording's rows, rx_option answering) */
            }
        }
    }
    int r = 0;
    if (movie_on()) {
        if (movie_mode == MOVIE_PLAY && S.frame >= S.tape.end) {
            S.over = 1;   /* (this read is the player's again: the recording ended with the frame before) */
            movie_summary("over");
            hos_log("input: the player's input from here");
        } else if (movie_mode == MOVIE_PLAY) {
            if (S.pad_frame != S.frame) {
                movie_tape_pad(&S.tape, S.frame, &S.frame_pad);
                S.pad_frame = S.frame;
                MovieRun ended;
                char line[256];
                if (S.rec && movie_rec_step(&S.run, S.frame, &S.frame_pad, &ended))
                    movie_write(line, movie_pad_line(line, sizeof line, ended.f, ended.frames, &ended.pad));
            }
            *given = S.frame_pad, r = 1;
        } else if (S.pad_frame == S.frame) {
            *given = S.frame_pad, r = 1;   /* (recording: a later read of the frame gets its first read's pad) */
        }
        if (!S.over) S.reads++;
    }
    if (movie_mode == MOVIE_PLAY && S.started && S.over) movie_finish("over");   /* (the end: the replay's file closed) */
    ReleaseSRWLockExclusive(&movie_lock);
    return r;
}

/* recording: the pad a frame's first read made (after the merge), the frame's pad: recorded, and given to its later reads */
void hos_input_movie_end(const MoviePad *p) {
    if (__atomic_load_n(&movie_mode, __ATOMIC_ACQUIRE) != MOVIE_RECORD) return;   /* (decided by the read's begin) */
    AcquireSRWLockExclusive(&movie_lock);
    if (movie_on() && movie_mode == MOVIE_RECORD && S.pad_frame != S.frame) {
        S.frame_pad = *p, S.pad_frame = S.frame;
        MovieRun ended;
        char line[256];
        if (movie_rec_step(&S.run, S.frame, p, &ended))
            movie_write(line, movie_pad_line(line, sizeof line, ended.f, ended.frames, &ended.pad));
    }
    ReleaseSRWLockExclusive(&movie_lock);
}

/* (whether the recording has the game's input now: the motion sensors and the menu mouse then the recording's alone) */
int hos_input_movie_playing(void) {
    return __atomic_load_n(&movie_mode, __ATOMIC_RELAXED) == MOVIE_PLAY && __atomic_load_n(&S.started, __ATOMIC_RELAXED) &&
           !__atomic_load_n(&S.over, __ATOMIC_RELAXED);
}

/* ---- the motion sensors (nn_hid.c GetSixAxisSensorStates) ---- */

/* each motion read: recording, h holds the live readings (newest first) and their count is returned; playing, h is
   filled with the recording's and their count returned */
int hos_input_movie_gyro(MovieMotion *h, int max, int live) {
    if (movie_mode_now() == MOVIE_OFF) return live;
    AcquireSRWLockExclusive(&movie_lock);
    if (!movie_on()) {
        ReleaseSRWLockExclusive(&movie_lock);
        return live;
    }
    if (S.held) {   /* (waiting for a load: the pad at rest, nothing recorded) */
        ReleaseSRWLockExclusive(&movie_lock);
        return 0;
    }
    const uint32_t c = S.gyros++, fr = movie_hook_frame(), at = movie_at_step(&S.gyro_at, fr);
    if (movie_mode == MOVIE_PLAY) live = movie_tape_gyro(&S.tape, fr, at, h, max);
    if (S.rec && live > 0) {
        int from;
        const int k = movie_gyro_new(&S.gyro_rec, h, live, &from);
        char line[640];
        for (int i = from; i > from - k; i--) {   /* (the new ones, oldest first) */
            movie_write(line, movie_motion_line(line, sizeof line, fr, &h[i]));
            S.gyro_rec.written++;
        }
        MovieGyro g = {c, fr, (uint32_t)live, S.gyro_rec.written, h[0].n, at};
        movie_write(line, movie_gyro_line(line, sizeof line, &g));
    }
    ReleaseSRWLockExclusive(&movie_lock);
    return live;
}

/* ---- the native mouse camera (mouse_native.c hos_mouse_camera) ---- */

/* At each of the camera's updates, before it decides, with its state block then (block: MOVIE_VIEW_WORDS words from
   +0x178) and its inputs (MOVIE_VIEW_IN_WORDS words, NULL when their object is not there): the view, recorded, or held to
   the recording's, and with HOS_INPUT_SYNC put back where it differs, the inputs first. -1 nothing recorded or played; 0
   recording (c->n set: fill the rest and give it to hos_input_movie_cam_end); 1 playing (c is the recording's: on or not,
   free, held and the turn) */
int hos_input_movie_cam_begin(MovieCam *c, uint32_t *block, uint32_t *inputs) {
    if (movie_mode_now() == MOVIE_OFF) return -1;
    AcquireSRWLockExclusive(&movie_lock);
    if (!movie_on()) {
        if (!S.started) S.cams_before++;
        ReleaseSRWLockExclusive(&movie_lock);
        return -1;
    }
    if (S.held) {   /* (waiting for a load: the recording's camera then, no turn, nothing checked or recorded) */
        movie_tape_cam(&S.tape, S.frame, MOVIE_CALLS_MAX, c);
        c->moved = 0;
        ReleaseSRWLockExclusive(&movie_lock);
        return 1;
    }
    const uint32_t m = S.cams++, fr = movie_hook_frame(), at = movie_at_step(&S.cam_at, fr);
    int r = 0;
    MovieView v = movie_view_of(m, fr, block, inputs);
    v.i = at;
    if (movie_mode == MOVIE_PLAY) {
        movie_tape_cam(&S.tape, fr, at, c);
        r = 1;
        c->n = m;
        const MovieView *w = movie_tape_view(&S.tape, fr, at);
        /* (HOS_INPUT_SYNC: this update put back; a run-time playback's camera held to the recording from its start, as Link
           is (MOVIE_SETTLE), with or without it: not counted as corrections) */
        const int settle = S.runtime && !S.diverged && m < MOVIE_SETTLE;
        const int put = S.sync && !S.diverged && m % (uint32_t)S.sync == 0;
        if (w) {
            S.view_checked++;
            if (settle) {   /* (held to the recording from the start, as Link: not counted) */
                if (!movie_view_inputs_same(w, inputs) && inputs) memcpy(inputs, w->in, sizeof w->in);
                if (!movie_view_same(w, block)) {
                    if (w->has_w) memcpy(block, w->w, sizeof w->w);
                    else memcpy(&block[MOVIE_VIEW_YAW], &w->yaw, 4), memcpy(&block[MOVIE_VIEW_PITCH], &w->pitch, 4);
                }
            } else if (!movie_view_inputs_same(w, inputs)) {   /* (what the camera eases to is made from: apart before the camera is) */
                if (!S.input_drifts)
                    hos_log("input: playback's camera inputs left the recording at its update %u (frame %u)", m, fr);
                S.input_drifts++;
                if (put && inputs) memcpy(inputs, w->in, sizeof w->in), S.syncs_input++;
            }
            if (!movie_view_same(w, block)) {
                if (!S.view_drifts)
                    hos_log("input: playback's camera left the recording at its update %u (frame %u): yaw %.9g pitch %.9g, the "
                            "recording's %.9g %.9g", m, fr, (double)v.yaw, (double)v.pitch, (double)w->yaw,
                            (double)w->pitch);
                S.view_drifts++;
                if (put) {   /* (put back: the block, or the yaw and pitch of a route or an older recording) */
                    if (w->has_w) memcpy(block, w->w, sizeof w->w);
                    else memcpy(&block[MOVIE_VIEW_YAW], &w->yaw, 4), memcpy(&block[MOVIE_VIEW_PITCH], &w->pitch, 4);
                    S.syncs_view++;
                }
            }
        }
    } else {
        memset(c, 0, sizeof *c);
        c->n = m;
    }
    if (S.route) v.has_w = 0, v.has_in = 0;   /* (a route: the angles alone, no guest words) */
    if (S.rec && (!S.have_view || (S.route ? S.last_view.yaw != v.yaw || S.last_view.pitch != v.pitch :
                                   memcmp(S.last_view.w, v.w, sizeof v.w) || S.last_view.has_in != v.has_in ||
                                   memcmp(S.last_view.in, v.in, sizeof v.in)))) {
        char line[448];   /* (the block's 112 hex digits and the inputs' 104 besides) */
        movie_write(line, movie_view_line(line, sizeof line, &v));
        S.last_view = v, S.have_view = 1;
    }
    c->f = fr, c->i = at;
    ReleaseSRWLockExclusive(&movie_lock);
    return r;
}

void hos_input_movie_cam_end(const MovieCam *c) {
    if (!__atomic_load_n(&S.rec, __ATOMIC_RELAXED)) return;   /* (only a recording takes it) */
    AcquireSRWLockExclusive(&movie_lock);
    if (S.rec && movie_on() && !S.held) {
        char line[200];
        int n;
        if (movie_cam_rec(&S.cam_rec, c, line, sizeof line, &n)) movie_write(line, n);
    }
    ReleaseSRWLockExclusive(&movie_lock);
}

/* ---- the menu mouse (oot_menu.c) ---- */

/* recording: a focus change (kind 1, the button) or an Items page choice (kind 2, the key) the mouse made on this screen */
void hos_input_movie_menu(int kind, uint64_t screen, uint64_t button, const char *key) {
    if (!__atomic_load_n(&S.rec, __ATOMIC_RELAXED)) return;
    AcquireSRWLockExclusive(&movie_lock);
    if (movie_on() && !S.route) {   /* (a route has no addresses: it uses the controller in the menus) */
        MovieMenu m;
        memset(&m, 0, sizeof m);
        m.f = movie_hook_frame(), m.kind = (uint8_t)kind, m.screen = screen, m.button = button;
        if (key) snprintf(m.key, sizeof m.key, "%s", key);
        char line[256];
        movie_write(line, movie_menu_line(line, sizeof line, &m));
    }
    ReleaseSRWLockExclusive(&movie_lock);
}

/* playing: the next of the recording's menu choices for this screen at this frame (1, *out filled), else 0. The caller
   says with hos_input_movie_menu_done whether the game took it: the file's button is checked against the screen first */
int hos_input_movie_menu_take(uint64_t screen, MovieMenu *out) {
    if (!hos_input_movie_playing()) return 0;
    AcquireSRWLockExclusive(&movie_lock);
    const MovieMenu *m = movie_on() && !S.held ? movie_tape_menu(&S.tape, movie_hook_frame(), screen, &S.menu_missed) : NULL;
    if (m) *out = *m;
    ReleaseSRWLockExclusive(&movie_lock);
    return m != NULL;
}

/* (a menu choice taken: given (met), or not met: its button not one of the screen's, its item not the page's, or the
   game's setter refused it) */
void hos_input_movie_menu_done(int met) {
    AcquireSRWLockExclusive(&movie_lock);
    if (met) S.menu_given++;
    else S.menu_missed++;
    ReleaseSRWLockExclusive(&movie_lock);
}

/* ---- Link's update (hos_main.c): the Debug tools' actions, the anchors and the marks ---- */

/* Before the Debug tools act at Link's update, with the action the game would take (live: the menu's, HOS_INPUT_LIVE's or
   the bridge's; 0 none): the action to take. Recording or nothing played: live. Playing: the recording's for this frame,
   and the live one not taken (*dropped 1: the caller answers the bridge) */
int hos_input_movie_action(int live, int *dropped) {
    *dropped = 0;
    if (movie_mode_now() != MOVIE_PLAY) return live;
    AcquireSRWLockExclusive(&movie_lock);
    int r = live;
    if (movie_on()) {
        r = 0;
        if (live) {
            if (!S.actions_dropped)
                hos_log("input: playback takes no Debug tools action of the player's (%d not taken): the recording's alone", live);
            S.actions_dropped++, *dropped = 1;
        }
        const MovieAction *a = S.held ? NULL : movie_tape_action(&S.tape, movie_hook_frame());
        if (a) r = a->a, S.actions_given++;
    }
    ReleaseSRWLockExclusive(&movie_lock);
    return r;
}

/* (after the Debug tools acted: the action the game took, recorded; 0 none) */
void hos_input_movie_action_taken(int a) {
    if (!a || !__atomic_load_n(&S.rec, __ATOMIC_RELAXED)) return;
    AcquireSRWLockExclusive(&movie_lock);
    if (S.rec && movie_on() && !S.held) {
        char line[96];
        movie_write(line, movie_action_line(line, sizeof line, movie_hook_frame(), a));
    }
    ReleaseSRWLockExclusive(&movie_lock);
}

/* (under movie_lock, started: Link's first update after a load, gap frames without one) */
static void movie_anchor(int gap) {
    const uint32_t n = S.anchors++;
    uint32_t fr = movie_hook_frame();
    if (movie_mode == MOVIE_PLAY) {
        uint32_t to;
        if (movie_anchor_rebase(S.tape.anchor, S.tape.nanchor, n, fr, &to)) {
            if (to != fr) hos_log("input: playback's anchor %u (a load, %d frames) at frame %u: frame %u as in the recording", n,
                                  gap, fr, to);
            S.clock0 = movie_clock() - (double)to;   /* (the frames from here are the recording's) */
            S.frame = to, S.held = 0, S.anchors_met++;
            S.frame_first = S.reads;
            fr = to;
        } else {
            /* (a load the recording does not have here (a pause it had not, a stall): said, and the recording's anchor kept
               for the replay's next, so one more load does not misnumber every one after it) */
            char why[96];
            snprintf(why, sizeof why, "a load (at frame %u) the recording does not have there", fr);
            movie_diverge(fr, why);
            S.anchors--;
            return;
        }
    }
    if (S.rec) {
        const MovieAnchor a = {fr, n, (uint32_t)gap};
        char line[96];
        movie_write(line, movie_anchor_line(line, sizeof line, &a));
    }
}

/* (under movie_lock, at Link's update: a playback started at run time takes Link to the recording's age and map before it
   starts, the Debug tab's age switch and then its teleport, each asked again after 12 s and given up after the third; once
   he is there and nothing loads or plays, the start is the frame after his next update, as from a save's load) */
static char movie_why[128];   /* (why the last run-time playback was given up before its start: the menu's line) */
static void movie_prep_step(void) {
    int event = 0, loading = 0, child = 0;
    char map[64] = "";
    if (!hos_oot_state(&event, &loading, &child) || event || loading) return;   /* (a load or an event: wait for its end) */
    const int have_map = hos_oot_current_map(map, sizeof map);
    const int64_t now = hos_now_ns();
    const int step = S.want_child >= 0 && child != S.want_child ? 1 : have_map && strcmp(map, S.tape.map) ? 2 : 0;
    if (step != S.prep_step) S.prep_step = step, S.prep_tries = 0;   /* (each step its own tries, asked at once) */
    const int asking = S.prep_tries && now - S.prep_at < 12000000000LL;
    if (!step) {
        S.prep = 0, S.updates = 0, S.prep_ticket = 0;   /* (there: the start after his next update) */
        hos_log("input: playback: Link is in %s, the recording's start: from his next update", map);
        return;
    }
    if (asking) return;
    if (S.prep_tries++ < 3 && (step == 1 || S.want_dest >= 0)) {
        if (step == 1) {
            hos_log("input: playback: Link is the %s and the recording starts with the %s: the age switch first",
                    child ? "child" : "adult", S.want_child ? "child" : "adult");
            S.prep_ticket = hos_oot_age_ask(!S.want_child);
        } else {
            hos_log("input: playback: Link is in %s and the recording starts in %s: the teleport there first", map, S.tape.map);
            S.prep_ticket = hos_oot_teleport_ask(S.want_dest);
        }
        S.prep_at = now;
        return;
    }
    if (S.prep_ticket) hos_oot_teleport_cancel(S.prep_ticket);
    snprintf(movie_why, sizeof movie_why, "Link could not be taken to %s (%s): not played.", S.tape.map,
             step == 1 ? "the age switch did not take" : "the teleport did not take");
    hos_log("input: playback: %s", movie_why);
    __atomic_store_n(&movie_mode, MOVIE_OFF, __ATOMIC_RELEASE);
    movie_reset();
}

/* Link's update, his position after it: recorded as a mark, or held to the recording's; 1 (and *to) when HOS_INPUT_SYNC asks
   for him to be put back there */
int hos_input_movie_link(const float pos[3], float to[3]) {
    if (movie_mode_now() == MOVIE_OFF) return 0;
    int put = 0;
    AcquireSRWLockExclusive(&movie_lock);
    if (S.prep && movie_mode == MOVIE_PLAY && !S.started) {   /* (to the recording's start first: no update counted yet) */
        movie_prep_step();
        ReleaseSRWLockExclusive(&movie_lock);
        return 0;
    }
    const double now = movie_clock();
    if (!S.updates) S.link_clock = now;   /* (the frame he was first updated in: the start is the next) */
    const int gap = S.last_update_clock >= 0 && now >= 0 ? (int)(now - S.last_update_clock + 0.5) : 0;
    S.last_update_clock = now;
    const uint32_t k = S.updates++ - S.k0;
    if (!movie_on()) {
        ReleaseSRWLockExclusive(&movie_lock);
        return 0;
    }
    if (gap >= MOVIE_ANCHOR_GAP) movie_anchor(gap);
    if (S.held) {   /* (an update while waiting for a load that is not one: nothing to hold him to) */
        ReleaseSRWLockExclusive(&movie_lock);
        return 0;
    }
    const uint32_t fr = movie_hook_frame(), at = movie_at_step(&S.link_at, fr);
    if (S.rec && (!S.have_pos || memcmp(pos, S.last_pos, sizeof S.last_pos))) {
        char line[160];
        movie_write(line, movie_mark_line(line, sizeof line, fr, at, k, pos));
        memcpy(S.last_pos, pos, sizeof S.last_pos);
        S.have_pos = 1;
    }
    if (movie_mode == MOVIE_PLAY) {
        const MovieMark *m = movie_tape_mark(&S.tape, fr, at);
        if (m) {
            S.checked++;
            /* (a playback started at run time puts Link at the recording's start first, wherever he stands on its map,
               with or without drift correction, and holds him to the recording for its first MOVIE_SETTLE updates: the
               game's move of his controller lands an update late, so the update after the start still finds him where he
               stood. Those are not counted: not apart, not corrections, not a divergence) */
            const int first = S.runtime && S.checked <= MOVIE_SETTLE;
            if (first && memcmp(pos, m->pos, sizeof m->pos) && !S.diverged) {
                memcpy(to, m->pos, sizeof m->pos);
                put = 1;
            } else if (memcmp(pos, m->pos, sizeof m->pos)) {
                const float dx = pos[0] - m->pos[0], dy = pos[1] - m->pos[1], dz = pos[2] - m->pos[2];
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (!S.drifts) {
                    hos_log("input: playback left the recording at Link's update %u (frame %u): at %.9g %.9g %.9g, the recording "
                            "%.9g %.9g %.9g (%.4f m apart)", k, fr, (double)pos[0], (double)pos[1], (double)pos[2],
                            (double)m->pos[0], (double)m->pos[1], (double)m->pos[2], (double)d);
                }
                S.drifts++;
                /* (past HOS_INPUT_SYNC_MAX the replay has diverged, said with or without the correction, so an exact test
                   (HOS_INPUT_SYNC=0) fails on it too) */
                if (!S.diverged) {
                    if (d > S.sync_max) {
                        char why[96];
                        snprintf(why, sizeof why, "Link %.3f m from the recording, past HOS_INPUT_SYNC_MAX (%.2f m)", (double)d,
                                 (double)S.sync_max);
                        movie_diverge(fr, why);
                    } else if (S.sync && k % (uint32_t)S.sync == 0) {
                        memcpy(to, m->pos, sizeof m->pos);
                        put = 1, S.syncs_link++, S.sync_total += d;
                        if (d > S.sync_largest) S.sync_largest = d;
                    }
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&movie_lock);
    return put;
}

/* ---- started and stopped at run time: the Insert menu's Debug tab (host/rx_backend.c) and the control bridge
   (host/control.c) ---- */

/* (a recording's file when none is named: <user>/recordings/<date>-<time>-<map>.jsonl) */
static void movie_default_path(char *out, size_t cap) {
    char dir[900], map[64] = "", clean[64];
    snprintf(dir, sizeof dir, "%s/recordings", hos_user_dir ? hos_user_dir : "user");
    CreateDirectoryA(dir, NULL);
    if (!hos_oot_current_map(map, sizeof map)) snprintf(map, sizeof map, "game");
    size_t j = 0;
    for (size_t i = 0; map[i] && j + 1 < sizeof clean; i++) {
        const char ch = map[i];
        clean[j++] = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ? ch : '_';
    }
    clean[j] = 0;
    const time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    snprintf(out, cap, "%s/%04d%02d%02d-%02d%02d%02d-%s.jsonl", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, clean);
}

/* (the player's recordings folder, newest first (the names begin with the date and time, so the greatest is the newest): scanned at most once a second (or after a recording ends), so the menu's
   drawing and the bridge's status read it without a scan each; MOVIE_LISTED newest kept however many the folder holds) */
#define MOVIE_LISTED 64
static SRWLOCK movie_list_lock = SRWLOCK_INIT;
static struct {
    char dir[900], names[MOVIE_LISTED][260];
    int n, dirty;
    int64_t at;
} movie_list;
static void movie_list_scan(void) {   /* (under movie_list_lock, exclusive) */
    const int64_t now = hos_now_ns();
    if (movie_list.at && !movie_list.dirty && now - movie_list.at < 1000000000LL) return;
    movie_list.at = now, movie_list.dirty = 0, movie_list.n = 0;
    snprintf(movie_list.dir, sizeof movie_list.dir, "%s/recordings", hos_user_dir ? hos_user_dir : "user");
    char pattern[920];
    snprintf(pattern, sizeof pattern, "%s/*.jsonl", movie_list.dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {   /* (kept newest first: each name put in its place, the oldest falling off past MOVIE_LISTED) */
        const size_t len = strlen(fd.cFileName);
        if (len <= 6 || len >= sizeof movie_list.names[0] || strcmp(fd.cFileName + len - 6, ".jsonl")) continue;
        int at = movie_list.n;
        while (at > 0 && strcmp(fd.cFileName, movie_list.names[at - 1]) > 0) at--;
        if (at >= MOVIE_LISTED) continue;
        const int last = movie_list.n < MOVIE_LISTED ? movie_list.n : MOVIE_LISTED - 1;
        memmove(movie_list.names[at + 1], movie_list.names[at], (size_t)(last - at) * sizeof movie_list.names[0]);
        snprintf(movie_list.names[at], sizeof movie_list.names[0], "%s", fd.cFileName);
        if (movie_list.n < MOVIE_LISTED) movie_list.n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static void movie_list_changed(void) {
    AcquireSRWLockExclusive(&movie_list_lock);
    movie_list.dirty = 1;
    ReleaseSRWLockExclusive(&movie_list_lock);
}

/* The i-th newest recording of the player's recordings folder (its path, 1), 0 past the last: the Insert menu's Play
   recording page lists them */
int hos_input_recording_at(int i, char *out, size_t cap) {
    AcquireSRWLockExclusive(&movie_list_lock);
    movie_list_scan();
    const int have = i >= 0 && i < movie_list.n;
    if (have) snprintf(out, cap, "%s/%s", movie_list.dir, movie_list.names[i]);
    ReleaseSRWLockExclusive(&movie_list_lock);
    return have;
}

/* (the folder's newest, for Last recording after a restart; 0 none) */
static int movie_newest_recording(char *out, size_t cap) { return hos_input_recording_at(0, out, cap); }

/* A recording from the next frame (after Link's next update), to path (NULL: movie_default_path); route: no guest words.
   0 when something is recorded or played already */
int hos_input_record_start(const char *path, int route) {
    movie_mode_now();
    AcquireSRWLockExclusive(&movie_lock);
    if (movie_mode != MOVIE_OFF) {
        ReleaseSRWLockExclusive(&movie_lock);
        return 0;
    }
    movie_reset();
    if (path && *path) snprintf(S.rec_path, sizeof S.rec_path, "%s", path);
    else movie_default_path(S.rec_path, sizeof S.rec_path);
    S.rec = 1, S.route = route != 0, S.runtime = 1;
    hos_log("input: recording to %s from Link's next update%s", S.rec_path, S.route ? " (a route: no guest words)" : "");
    __atomic_store_n(&movie_mode, MOVIE_RECORD, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&movie_lock);
    return 1;
}

void hos_input_record_stop(void) {
    AcquireSRWLockExclusive(&movie_lock);
    if (movie_mode == MOVIE_RECORD) movie_finish("stopped");
    ReleaseSRWLockExclusive(&movie_lock);
}

/* A playback of path (NULL: the last recording made) from Link's next update, put back at every sync-th update while
   within sync_max metres (sync 0: never). 1 started; 0 something recorded or played already; -1 no such file, or not a
   recording; -2 the recording starts on a map no teleport goes to */
int hos_input_play_start(const char *path, int sync, float sync_max) {
    movie_why[0] = 0;
    movie_mode_now();
    AcquireSRWLockExclusive(&movie_lock);
    if (movie_mode != MOVIE_OFF) {
        ReleaseSRWLockExclusive(&movie_lock);
        return 0;
    }
    movie_reset();
    char file[1024];
    snprintf(file, sizeof file, "%s", path && *path ? path : movie_last_path);
    if (!file[0] && !movie_newest_recording(file, sizeof file)) file[0] = 0;   /* (after a restart: the folder's newest) */
    int r = 1;
    if (!file[0] || !movie_play_setup(file, sync < 0 ? 0 : sync, isfinite(sync_max) && sync_max > 0 ? sync_max : 1.0f)) {
        if (!file[0]) hos_log("input: playback: no recording made yet to play");
        r = -1;
    } else if (strcmp(S.tape.from, "play")) {
        hos_log("input: playback: %s starts at the game's first read (HOS_INPUT_FROM=boot): only from the switches", file);
        r = -1;
    } else {
        /* (Link taken to the recording's age and map first, when they are not his: the map's destination of the Debug tab's
           teleports, for the recording's age (either when not known)) */
        S.want_child = !strcmp(S.tape.age, "child") ? 1 : !strcmp(S.tape.age, "adult") ? 0 : -1;
        S.want_dest = -1;
        const char *child, *adult;
        for (int i = 0; S.tape.map[0] && hos_oot_destination(i, &child, &adult); i++)
            if ((S.want_child != 0 && child && !strcmp(child, S.tape.map)) || (S.want_child != 1 && adult && !strcmp(adult, S.tape.map))) {
                S.want_dest = i;
                if (S.want_child < 0) {   /* (a recording with no age (an older one): the age whose map it is, if only one's) */
                    const int as_child = child && !strcmp(child, S.tape.map), as_adult = adult && !strcmp(adult, S.tape.map);
                    if (as_child != as_adult) S.want_child = as_child;
                }
                break;
            }
        char here[64];
        int event, loading, now_child;
        const int other_map = S.tape.map[0] && hos_oot_current_map(here, sizeof here) && strcmp(here, S.tape.map);
        const int other_age = S.want_child >= 0 && hos_oot_state(&event, &loading, &now_child) && now_child != S.want_child;
        if (other_map && S.want_dest < 0) {
            hos_log("input: playback: %s starts in %s, which no teleport goes to, and Link is in %s: not played", file,
                    S.tape.map, here);
            r = -2;
        } else if (other_map || other_age) {
            S.prep = 1;
        }
    }
    if (r == 1) {
        S.runtime = 1;
        hos_log("input: playback of %s from Link's next update", file);
        __atomic_store_n(&movie_mode, MOVIE_PLAY, __ATOMIC_RELEASE);
    } else {
        movie_reset();
    }
    ReleaseSRWLockExclusive(&movie_lock);
    return r;
}

void hos_input_play_stop(void) {
    AcquireSRWLockExclusive(&movie_lock);
    if (movie_mode == MOVIE_PLAY) {
        if (S.started) movie_finish("stopped");
        else {   /* (stopped while Link is taken there: the age switch or teleport asked and not taken yet, not taken later) */
            if (S.prep_ticket) hos_oot_teleport_cancel(S.prep_ticket);
            __atomic_store_n(&movie_mode, MOVIE_OFF, __ATOMIC_RELEASE), movie_reset();
        }
    }
    ReleaseSRWLockExclusive(&movie_lock);
}

/* The host's clock for its own decisions by time (host/oot_ocb_hold.c, host/oot_tex_compact.c): wall_ns with nothing
   recorded or played; while a recording is recorded or played (or one is wanted from the start), the game clock's time, so
   the decision falls on the same game frame in a replay as in the recording. t is the caller's, under its lock */
int64_t hos_input_movie_time(HosMovieTime *t, int64_t wall_ns) {
    const double c = movie_mode_now() > MOVIE_OFF ? movie_clock() : -1.0;
    return c >= 0 ? movie_time_step(t, 1, (int64_t)(c * (1e9 / 60.0))) : movie_time_step(t, 0, wall_ns);
}

/* what is recorded or played now, for the menu's rows and the bridge's status */
void hos_input_movie_status(HosInputMovieStatus *out) {
    memset(out, 0, sizeof *out);
    const int mode = movie_mode_now();
    AcquireSRWLockExclusive(&movie_lock);
    out->mode = mode == MOVIE_RECORD ? 1 : mode == MOVIE_PLAY ? 2 : 0;
    out->started = S.started, out->held = S.held, out->diverged = S.diverged;
    out->frame = S.frame, out->end = S.tape.end;
    out->link_apart = S.drifts, out->link_checked = S.checked, out->syncs = S.syncs_link;
    out->largest = S.sync_largest;
    out->anchors = S.anchors_met, out->anchors_wanted = (uint32_t)S.tape.nanchor;
    snprintf(out->path, sizeof out->path, "%s", mode == MOVIE_PLAY ? S.path : mode == MOVIE_RECORD ? S.rec_path : movie_last_path);
    snprintf(out->why, sizeof out->why, "%s", movie_why);
    ReleaseSRWLockExclusive(&movie_lock);
    if (mode == MOVIE_OFF && !out->path[0]) {   /* (none made this run: the folder's newest, as Last recording plays; the
                                                   listing's, scanned at most once a second) */
        char newest[1024];
        if (movie_newest_recording(newest, sizeof newest)) snprintf(out->path, sizeof out->path, "%s", newest);
    }
}
