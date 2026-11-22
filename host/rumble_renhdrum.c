/* (longshot) RenHDrum's backends: host/rumble_renhdrum.h says what they are and who calls what. */
#include "hos.h"
#include "rumble_renhdrum.h"
#include "pad_speaker.h"   /* (the speaker's two channels on this stream) */
#include "hos_sdl.h"
#include "audio_output_devices.h"   /* (HOS_OUTPUT_ID_CAP, HOS_OUTPUT_NAME_CAP) */
#include "vibration.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../third_party/renhdrum/include/renhdrum.h"

#define RH_ABI 0x000500u            /* include/renhdrum.h's RENHDRUM_VERSION: any 0.5.x keeps its ABI */
#define RH_STALE_NS 250000000LL     /* a side the game stopped sending to goes silent: the DSPs hold their last value */
#define RH_TEST_AMP 0.5f
#define RH_FREQ_LOW 160.0f          /* the game's own when it sends none */
#define RH_FREQ_HIGH 320.0f
#define RH_PADS 8
#define RH_EFFECTS RENHDRUM_DS5_EFFECTS_SIZE   /* a DualSense's effects block: SDL's SendGamepadEffect sends it */

/* ---- RenHDrum, loaded from beside the game by its full path (no folder of ours or PATH searched) ---- */
#define RH_FUNCS(X)                                                                                                    \
    X(uint32_t, version, (void))                                                                                       \
    X(void, set_nx2_mode, (int32_t))                                                                                   \
    X(RenHdcDualSenseDsp *, dualsense_dsp_create, (int32_t, bool))                                                    \
    X(void, dualsense_dsp_destroy, (RenHdcDualSenseDsp *))                                                             \
    X(int32_t, dualsense_dsp_set_half_target, (RenHdcDualSenseDsp *, bool, float, float, float, float))               \
    X(int32_t, dualsense_dsp_fill_f32, (RenHdcDualSenseDsp *, float *, size_t))                                       \
    X(int32_t, dualsense_dsp_build_bt_report, (RenHdcDualSenseDsp *, uint8_t, uint8_t *))                             \
    X(int32_t, dualsense_dsp_set_speaker, (RenHdcDualSenseDsp *, int32_t))                                            \
    X(int32_t, dualsense_dsp_build_bt36_report, (RenHdcDualSenseDsp *, const uint8_t *, uint8_t, uint8_t *))          \
    X(RenHdcTriTrigger *, tri_trigger_create, (void))                                                                  \
    X(void, tri_trigger_destroy, (RenHdcTriTrigger *))                                                                 \
    X(int32_t, tri_trigger_update, (RenHdcTriTrigger *, uint32_t, uint8_t, float, float, float, float, bool, uint8_t *)) \
    X(void, tri_trigger_note_action, (RenHdcTriTrigger *, uint32_t))                                                   \
    X(int32_t, hd_haptics_speaker_block, (int32_t, uint8_t *))                                                         \
    X(int32_t, hd_haptics_trigger_block, (int32_t, const uint8_t *, uint8_t *))                                        \
    X(RenHdcDualSenseDsp *, valve_grip_dsp_create_ex, (int32_t, bool, bool))                                           \
    X(int32_t, valve_grip_build_report, (RenHdcDualSenseDsp *, uint8_t, uint8_t *))                                    \
    X(int32_t, valve_grip_mode_tick, (RenHdcDualSenseDsp *, uint8_t, uint64_t, bool, uint8_t *))                       \
    X(int32_t, valve_grip_mode_off, (uint8_t, uint8_t *))                                                              \
    X(RenHdcXboxRumble *, xbox_rumble_create, (bool))                                                                  \
    X(void, xbox_rumble_destroy, (RenHdcXboxRumble *))                                                                 \
    X(int32_t, xbox_rumble_push_half_sample, (RenHdcXboxRumble *, int64_t, bool, float, float, float, float))         \
    X(int32_t, xbox_rumble_tick, (RenHdcXboxRumble *, int64_t, uint16_t *))                                           \
    X(RenHdcEngine *, engine_create, (bool, bool))                                                                     \
    X(void, engine_destroy, (RenHdcEngine *))                                                                          \
    X(int32_t, engine_push_half_sample, (RenHdcEngine *, int64_t, bool, float, float, float, float))                  \
    X(int32_t, engine_tick, (RenHdcEngine *, int64_t, uint8_t *))
static struct {
#define X(ret, name, args) ret(*name) args;
    RH_FUNCS(X)
#undef X
} rh;
static int rh_tried, rh_ok;
static char rh_reason[256];

static void why(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rh_reason, sizeof rh_reason, fmt, ap);
    va_end(ap);
    hos_log("vibration: RenHDrum: %s", rh_reason);
}
const char *rh_why(void) { return rh_reason; }

static HMODULE open_library(void) {
#ifdef _WIN32
    wchar_t path[1024];
    DWORD n = GetModuleFileNameW(NULL, path, (DWORD)(sizeof path / sizeof path[0]));
    if (!n || n >= sizeof path / sizeof path[0]) return why("the game's own folder could not be read"), (HMODULE)NULL;
    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    static const wchar_t file[] = L"renhdrum.dll";
    if (n + sizeof file / sizeof file[0] > sizeof path / sizeof path[0]) return why("the game's folder is too long"), (HMODULE)NULL;
    memcpy(path + n, file, sizeof file);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return why("renhdrum.dll is not beside the game"), (HMODULE)NULL;
    /* (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR: its own folder, for what it imports too, and System32; no PATH, no current folder) */
    /* (SEM_FAILCRITICALERRORS: a file that is not a usable DLL fails here, and its controllers keep their motors, rather
       than Windows stopping the game's start with a "Bad Image" box: a release once shipped a Linux library under this
       name, 0xc000012f. The thread's own mode, put back after) */
    DWORD mode = 0;
    BOOL quiet = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &mode);
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD err = m ? 0 : GetLastError();
    if (quiet) SetThreadErrorMode(mode, NULL);
    if (!m) why("renhdrum.dll would not load (error %lu)", (unsigned long)err);
    return m;
#else
    char path[4096];
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (!n || n >= sizeof path) return why("the game's own folder could not be read"), (HMODULE)NULL;
    while (n && path[n - 1] != '/') n--;
    if (n + sizeof "renhdrum.so" > sizeof path) return why("the game's folder is too long"), (HMODULE)NULL;
    memcpy(path + n, "renhdrum.so", sizeof "renhdrum.so");
    FILE *f = fopen(path, "rb");
    if (!f) return why("renhdrum.so is not beside the game"), (HMODULE)NULL;
    fclose(f);
    HMODULE m = LoadLibraryA(path);
    if (!m) why("renhdrum.so would not load (%s)", hos_load_error());
    return m;
#endif
}

int rh_load(void) {
    if (rh_tried) return rh_ok;
    rh_tried = 1;
    HMODULE m = open_library();
    if (!m) return 0;
#define X(ret, name, args)                                                                                             \
    if (!(*(FARPROC *)&rh.name = GetProcAddress(m, "renhdrum_" #name))) {                                              \
        why("it has no renhdrum_" #name ": the plain rumble only");                                                   \
        FreeLibrary(m);                                                                                                \
        return 0;                                                                                                      \
    }
    RH_FUNCS(X)
#undef X
    const uint32_t v = rh.version();
    if ((v & 0xFFFF00u) != (RH_ABI & 0xFFFF00u)) {
        why("it is version %u.%u.%u, this build wants %u.%u", (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF,
            (RH_ABI >> 16) & 0xFF, (RH_ABI >> 8) & 0xFF);
        FreeLibrary(m);
        return 0;
    }
    rh.set_nx2_mode(2);   /* (a Switch 2 game: RenHDrum's Mode 2 curves, for every pad it plays) */
    hos_log("vibration: RenHDrum %u.%u.%u loaded", (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    rh_reason[0] = 0;
    return rh_ok = 1;
}

/* ---- what the game sends, and everything that plays it (lk) ---- */
static SRWLOCK lk = SRWLOCK_INIT;
static SRWLOCK life = SRWLOCK_INIT;   /* (opening and closing: the pad thread's, and the game's end; never inside lk) */
static int closed;                    /* (the game is over: nothing opens again) */
static int64_t at[2];                 /* when each side last sent (0 never) */
static int quiet[2] = {1, 1};
static int testing;
static volatile LONG gain_percent = 100;
static volatile LONG usb_gain_percent = 100;
static RenHdcDualSenseDsp *usb_dsp, *bt_dsp, *sv_dsp;   /* (sv: a Steam Controller (2026)'s grips) */
static void *usb_pad, *bt_pad, *sv_pad;
/* (a DualSense's HD Haptics, the Controls page's row, on by default: its speaker taps and its L2/R2 kick back from the
   same vibration, the motors' heavy hits a little deeper. Off: it plays as RenHDrum 0.4 did, the speaker at 0 and the
   Bluetooth report the plain 0x32) */
static volatile LONG hd_on = 1;
static float last[2][4] = {{0.0f, RH_FREQ_LOW, 0.0f, RH_FREQ_HIGH}, {0.0f, RH_FREQ_LOW, 0.0f, RH_FREQ_HIGH}};
static uint32_t fed[2];               /* (each side's values sent so far: a trigger's "fresh") */
/* (each path's two triggers, left then right, made while HD Haptics plays on it; the pad thread updates them) */
static RenHdcTriTrigger *usb_tri[2], *bt_tri[2];
static uint32_t usb_seen[2], bt_seen[2];
static int usb_hd;                    /* (the cable's speaker opened and the triggers in use: the pad thread's) */
static uint8_t bt_state[RH_EFFECTS];  /* (both triggers' effects as report 0x36 carries them every time) */
static struct {
    void *pad;
    int backend;
    RenHdcXboxRumble *xbox;
    RenHdcEngine *engine;
} pads[RH_PADS];

static int64_t now_us(void) { return hos_now_ns() / 1000; }

static void feed(int side, float al, float fl, float ah, float fh) {   /* (lk held) */
    const bool right = side == 1;
    const int64_t us = now_us();
    last[side][0] = al, last[side][1] = fl, last[side][2] = ah, last[side][3] = fh, fed[side]++;
    if (usb_dsp) rh.dualsense_dsp_set_half_target(usb_dsp, right, al, fl, ah, fh);
    if (bt_dsp) rh.dualsense_dsp_set_half_target(bt_dsp, right, al, fl, ah, fh);
    if (sv_dsp) rh.dualsense_dsp_set_half_target(sv_dsp, right, al, fl, ah, fh);
    /* (the Switch engine has no gain of its own: the strength scales what it is given, for testers' runs) */
    const float g = vibration_gain((int)gain_percent);
    const float sal = al * g > 1.0f ? 1.0f : al * g, sah = ah * g > 1.0f ? 1.0f : ah * g;
    for (int i = 0; i < RH_PADS; i++) {
        if (pads[i].xbox) rh.xbox_rumble_push_half_sample(pads[i].xbox, us, right, al, fl, ah, fh);
        if (pads[i].engine) rh.engine_push_half_sample(pads[i].engine, us, right, sal, fl, sah, fh);
    }
}

void rh_push(int side, const float v[4]) {
    if (!rh_ok || side < 0 || side > 1 || !v) return;
    float al = v[0], ah = v[2];
    al = al > 0.0f ? (al < 1.0f ? al : 1.0f) : 0.0f;   /* (also a NaN: 0) */
    ah = ah > 0.0f ? (ah < 1.0f ? ah : 1.0f) : 0.0f;
    const float fl = v[1] > 0.0f ? v[1] : RH_FREQ_LOW, fh = v[3] > 0.0f ? v[3] : RH_FREQ_HIGH;
    AcquireSRWLockExclusive(&lk);
    at[side] = hos_now_ns(), quiet[side] = 0;
    if (!testing) feed(side, al, fl, ah, fh);
    ReleaseSRWLockExclusive(&lk);
}

static void recreate_bt_dsp(int percent);
static void usb_hd_set(int on);
void rh_tick(int64_t now, int test, int percent, int usb_percent, int hd) {
    if (!rh_ok) return;
    const int was_percent = (int)InterlockedExchange(&gain_percent, percent);
    InterlockedExchange(&usb_gain_percent, usb_percent);
    const int was_hd = (int)InterlockedExchange(&hd_on, hd != 0);
    AcquireSRWLockExclusive(&lk);
    if (test) {   /* (the Test vibration row, or a strength just changed: both bands, half the game's top) */
        testing = 1;
        for (int s = 0; s < 2; s++) feed(s, RH_TEST_AMP, 180.0f, RH_TEST_AMP, RH_FREQ_HIGH), quiet[s] = 0, at[s] = now;
    } else if (testing) {
        testing = 0;
        for (int s = 0; s < 2; s++) feed(s, 0.0f, RH_FREQ_LOW, 0.0f, RH_FREQ_HIGH), quiet[s] = 1, at[s] = 0;
    }
    for (int s = 0; s < 2; s++)   /* (a side gone quiet: the game stopped sending, and the DSP would hold its value) */
        if (at[s] && !quiet[s] && now - at[s] > RH_STALE_NS) feed(s, 0.0f, RH_FREQ_LOW, 0.0f, RH_FREQ_HIGH), quiet[s] = 1;
    ReleaseSRWLockExclusive(&lk);
    if (percent != was_percent && (bt_dsp || sv_dsp)) recreate_bt_dsp(percent);
    if ((hd != 0) != was_hd) {
        hos_log("vibration: a DualSense's HD Haptics (speaker and triggers) %s", hd ? "on" : "off");
        AcquireSRWLockExclusive(&life);   /* (as the cable's open and close: one of them at a time) */
        usb_hd_set(hd != 0);              /* (Bluetooth's sender sees hd_on at its next report) */
        ReleaseSRWLockExclusive(&life);
    }
}

/* ---- a DualSense's HD Haptics: its speaker and its triggers, from the same vibration ---- */
static void send_effects(void *pad, const uint8_t block[RH_EFFECTS]) {   /* (never inside lk: SDL's own lock) */
    if (pad && hos_sdl_pad_has_effect()) hos_sdl_pad_effect(pad, block, RH_EFFECTS);
}
static void tri_pair(RenHdcTriTrigger *t[2], int make) {   /* (lk held) */
    for (int s = 0; s < 2; s++) {
        if (make && !t[s]) t[s] = rh.tri_trigger_create();
        if (!make && t[s]) rh.tri_trigger_destroy(t[s]), t[s] = NULL;
    }
}
static void bt_state_off(void) {   /* (lk held) both triggers Off, as 0x36 carries them */
    uint8_t one[RH_EFFECTS];
    memset(bt_state, 0, sizeof bt_state);
    for (int s = 0; s < 2; s++) {
        rh.hd_haptics_trigger_block(s, NULL, one);
        const int at = s ? 10 : 21;
        bt_state[0] |= one[0];
        memcpy(bt_state + at, one + at, 11);
    }
}
/* (longshot) the controller speaker's wish for the cable's speaker (rh_usb_speaker), and what the pad was last told: the
   speaker is opened and given back here only, by RenHDrum's speaker block, for HD Haptics and the game's sounds alike */
static int usb_spk_want, usb_spk_gain;
static int usb_spk_open, usb_spk_told_gain = -1;
static int usb_speaker_sync(void) {   /* (life held) */
    void *pad = usb_pad;
    if (!pad) return 0;
    const int jack = usb_spk_want == RH_SPEAKER_JACK, game = usb_spk_want == RH_SPEAKER_GAME;
    const int open = !jack && (usb_hd || game);
    const int gain = open && game ? usb_spk_gain : -1;   /* (-1: the block's own) */
    AcquireSRWLockExclusive(&lk);   /* (HD Haptics' taps: silent while the jack has the sound) */
    if (usb_dsp) rh.dualsense_dsp_set_speaker(usb_dsp, usb_hd && !jack ? RENHDRUM_HD_HAPTICS_SPEAKER_PERCENT : 0);
    ReleaseSRWLockExclusive(&lk);
    if (open == usb_spk_open && (!open || gain == usb_spk_told_gain)) return 1;
    uint8_t block[RH_EFFECTS];
    rh.hd_haptics_speaker_block(open, block);
    if (gain >= 0) block[37] = (uint8_t)((block[37] & ~7) | (gain & 7));   /* (the preamp's gain: its low three bits) */
    if (!hos_sdl_pad_has_effect() || !hos_sdl_pad_effect(pad, block, RH_EFFECTS)) return 0;
    usb_spk_open = open, usb_spk_told_gain = gain;
    return 1;
}
/* The cable's HD Haptics on or off: its speaker opened (or given back), the DSP's speaker level, and both triggers
   released when it goes off. Under life: the pad thread's (rh_tick, rh_usb_open, rh_usb_close) and the game's end */
static void usb_hd_set(int on) {
    void *pad = usb_pad;
    if (!pad || on == usb_hd) return;
    uint8_t block[RH_EFFECTS];
    AcquireSRWLockExclusive(&lk);
    tri_pair(usb_tri, on);
    usb_seen[0] = fed[0], usb_seen[1] = fed[1];
    ReleaseSRWLockExclusive(&lk);
    usb_hd = on;
    usb_speaker_sync();   /* (its speaker level, and the speaker opened or given back) */
    if (!on)
        for (int s = 0; s < 2; s++) rh.hd_haptics_trigger_block(s, NULL, block), send_effects(pad, block);
}
int rh_usb_speaker(int want, int gain) {
    if (!rh_ok) return 0;
    AcquireSRWLockExclusive(&life);
    usb_spk_want = want, usb_spk_gain = gain;
    int sent = usb_speaker_sync();
    ReleaseSRWLockExclusive(&life);
    return sent;
}
void rh_ds_triggers(void *pad, int64_t now, int left_depth, int right_depth, int other_held) {
    if (!rh_ok || !pad || !hd_on || (pad != usb_pad && pad != bt_pad)) return;
    const int usb = pad == usb_pad;
    const uint32_t ms = (uint32_t)(now / 1000000);
    const int depth[2] = {left_depth < 0 ? 0 : left_depth > 255 ? 255 : left_depth,
                          right_depth < 0 ? 0 : right_depth > 255 ? 255 : right_depth};
    uint8_t send[2][RH_EFFECTS];
    int sends = 0;
    AcquireSRWLockExclusive(&lk);
    RenHdcTriTrigger **tri = usb ? usb_tri : bt_tri;
    uint32_t *seen = usb ? usb_seen : bt_seen;
    for (int s = 0; s < 2; s++) {
        if (!tri[s]) continue;
        if (other_held) rh.tri_trigger_note_action(tri[s], ms);   /* (another button held: a posture, or muted) */
        const bool fresh = fed[s] != seen[s];
        seen[s] = fed[s];
        uint8_t effect[11];
        if (rh.tri_trigger_update(tri[s], ms, (uint8_t)depth[s], last[s][0], last[s][1], last[s][2], last[s][3], fresh,
                                  effect) != 1)
            continue;
        if (usb) {
            rh.hd_haptics_trigger_block(s, effect, send[sends++]);
        } else {   /* (Bluetooth: into the state each 0x36 carries) */
            const int at = s ? 10 : 21;
            bt_state[0] |= s ? 0x04 : 0x08;
            memcpy(bt_state + at, effect, 11);
        }
    }
    ReleaseSRWLockExclusive(&lk);
    for (int i = 0; i < sends; i++) send_effects(pad, send[i]);
}

/* ---- a DualSense on a cable: its sound card's third and fourth channels are the voice coils ---- */
static void *usb_stream;
static int usb_token = -1;
static void usb_pull(void *user, uint8_t *out, int len) {   /* (the sound card's thread: lk, for one fill) */
    (void)user;
    float *f = (float *)out;
    const int frames = len / (int)(4 * sizeof(float));
    int filled = 0;
    AcquireSRWLockExclusive(&lk);
    if (usb_dsp) filled = rh.dualsense_dsp_fill_f32(usb_dsp, f, (size_t)frames) >= 0;
    ReleaseSRWLockExclusive(&lk);
    if (!filled) {
        memset(out, 0, (size_t)len);   /* (nothing to play: silence, never a held waveform) */
        hos_pad_speaker_fill(f, frames);   /* (longshot) the game's sounds added to the speaker's, host/pad_speaker.c */
        return;
    }
    /* the Vibration strength, once, on the samples the DSP made: its law stays the core's (a gain before it would go
       through its curve, and past 100 % its normalising held the output still) */
    const float g = vibration_gain((int)InterlockedCompareExchange(&usb_gain_percent, 0, 0));
    for (int i = 0; i < frames; i++)
        for (int ch = 1; ch < 4; ch++) {   /* (1: HD Haptics' speaker; 2 and 3: the voice coils) */
            const float v = f[i * 4 + ch] * g;
            f[i * 4 + ch] = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
        }
    /* (longshot) the game's sounds added to channels 0 and 1, HD Haptics' taps on 1 kept: host/pad_speaker.c, after the
       strength above, which is the vibration's and not theirs */
    hos_pad_speaker_fill(f, frames);
}
/* The vibration's endpoint, found by its name as before the controller speaker: on every system, Linux and the Steam Deck
   too. Only the speaker's sharing of the stream asks more (host/pad_speaker.c: the endpoint matched to the pad itself,
   hos_sdl_pad_audio_endpoint) */
int rh_usb_find(void *pad) {
    (void)pad;
    if (!rh_ok) return -1;
    int found = -1, named = 0;
    char id[HOS_OUTPUT_ID_CAP], name[HOS_OUTPUT_NAME_CAP];
    const int n = hos_sdl_output_count();
    for (int i = 0; i < n; i++) {
        if (!hos_sdl_output_info(i, id, (int)sizeof id, name, (int)sizeof name)) continue;
        if (!strstr(name, "DualSense") && !strstr(name, "Wireless Controller")) continue;
        named++;
        if (found < 0 && hos_sdl_output_channels(i) >= 4) found = hos_sdl_output_resolve(id);
    }
    static int said = -2;   /* (said when it changes, not at every look) */
    if (found != said) {
        said = found;
        hos_log(found >= 0 ? "vibration: the DualSense's four-channel sound card is there"
                           : "vibration: %d playback devices, %d named for a DualSense, none with four channels", n, named);
    }
    return found;
}
int rh_usb_open(void *pad, int token) {
    if (!rh_ok || token < 0) return 0;
    AcquireSRWLockExclusive(&life);
    int ok = 0;
    if (!closed && !usb_pad) {
        RenHdcDualSenseDsp *dsp = rh.dualsense_dsp_create(100, false);   /* (its full gain: the strength is ours) */
        if (dsp) {
            AcquireSRWLockExclusive(&lk);
            usb_dsp = dsp;
            ReleaseSRWLockExclusive(&lk);
            usb_stream = hos_sdl_audio_open((unsigned)token, 48000, 4, 0, hos_pad_speaker_share_frames(pad, token), usb_pull, NULL);
            if (usb_stream) {
                usb_pad = pad, usb_token = token, ok = 1;
                hos_log("vibration: the DualSense's voice coils on its cable (48 kHz, four channels)");
                usb_spk_want = RH_SPEAKER_NONE, usb_spk_open = 0, usb_spk_told_gain = -1;   /* (the speaker asks again) */
                if (hd_on) usb_hd_set(1);
            } else {
                AcquireSRWLockExclusive(&lk);
                usb_dsp = NULL;
                ReleaseSRWLockExclusive(&lk);
                rh.dualsense_dsp_destroy(dsp);
                hos_log("vibration: the DualSense's sound card would not open");
            }
        }
    }
    ReleaseSRWLockExclusive(&life);
    return ok;
}
int rh_usb_alive(void) { return usb_stream && hos_sdl_audio_alive(usb_stream); }
static void usb_close_locked(void);
int rh_usb_token(void) { return usb_stream ? usb_token : -1; }
/* (the controller speaker's route change: the stream closed and opened again, its queued samples gone. The endpoint is
   matched to the pad again first: one that changed under the same name is refused, and the stream given up, so the
   pads' poll finds the pad's own again) */
int rh_usb_restart_audio(void) {
    AcquireSRWLockExclusive(&life);
    int ok = 0;
    if (usb_stream && usb_pad && hos_sdl_pad_audio_endpoint(usb_pad) == usb_token) {
        hos_sdl_audio_close(usb_stream);
        usb_stream = hos_sdl_audio_open((unsigned)usb_token, 48000, 4, 0, hos_pad_speaker_share_frames(usb_pad, usb_token), usb_pull, NULL);
        ok = usb_stream != NULL;
    } else if (usb_stream) {
        hos_log("vibration: the DualSense's sound card no longer matches the pad: let go");
    }
    if (!ok && usb_pad) usb_close_locked();   /* (none open, or not the pad's: rh_usb_alive says so to the pads' poll) */
    ReleaseSRWLockExclusive(&life);
    return ok;
}
static void usb_close_locked(void) {   /* (life held) */
    usb_spk_want = RH_SPEAKER_NONE;   /* (longshot: the controller speaker's wish ends with the stream) */
    usb_hd_set(0);   /* (the speaker given back and the triggers released, while the pad is still open) */
    usb_speaker_sync();   /* (given back also when only the controller speaker had it open) */
    void *stream = usb_stream;
    usb_stream = NULL, usb_pad = NULL, usb_token = -1;
    if (stream) hos_sdl_audio_close(stream);   /* (waits for its callback, which takes lk: lk is not held here) */
    AcquireSRWLockExclusive(&lk);
    RenHdcDualSenseDsp *dsp = usb_dsp;
    usb_dsp = NULL;
    ReleaseSRWLockExclusive(&lk);
    if (dsp) rh.dualsense_dsp_destroy(dsp);
}
void rh_usb_close(void) {
    AcquireSRWLockExclusive(&life);
    usb_close_locked();
    ReleaseSRWLockExclusive(&life);
}
void *rh_usb_owner(void) { return usb_pad; }

/* ---- a DualSense over Bluetooth: report 0x36 every 10.7 ms with HD Haptics (the voice coils, the speaker in Opus and
   the triggers), else the plain 0x32; written beside SDL's own reports ---- */
static void *bt_hid;
static HANDLE bt_thread;
static volatile LONG bt_run, bt_failed;
static DWORD WINAPI bt_main(LPVOID unused) {
    (void)unused;
    void hos_host_thread_register_self(const char *name);   /* (mkw_glue.c) */
    hos_host_thread_register_self("vibration (DualSense, Bluetooth)");
    uint8_t report[RENHDRUM_DS5_BT36_REPORT_SIZE > RENHDRUM_DS5_BT_REPORT_SIZE ? RENHDRUM_DS5_BT36_REPORT_SIZE
                                                                              : RENHDRUM_DS5_BT_REPORT_SIZE];
    int64_t next = hos_now_ns(), loud_at = 0;
    int fails = 0, hd = -1;
    RenHdcDualSenseDsp *set_for = NULL;   /* (the DSP whose speaker level is set: a new strength makes a new one) */
    while (bt_run) {
        next += (int64_t)RENHDRUM_DS5_BT_PERIOD_NS;
        int r = -1, size = 0, send = 0;
        AcquireSRWLockExclusive(&lk);
        const int want = (int)hd_on;
        if (bt_dsp && (want != hd || bt_dsp != set_for)) {   /* (HD Haptics turned on or off, or a new DSP) */
            rh.dualsense_dsp_set_speaker(bt_dsp, want ? RENHDRUM_HD_HAPTICS_SPEAKER_PERCENT : 0);
            if (want != hd) tri_pair(bt_tri, want), bt_seen[0] = fed[0], bt_seen[1] = fed[1], bt_state_off();
            /* (going off: one last 0x36 with both triggers Off, then the plain 0x32) */
            if (!want && hd == 1) r = rh.dualsense_dsp_build_bt36_report(bt_dsp, bt_state, 0, report), size = RENHDRUM_DS5_BT36_REPORT_SIZE;
            hd = want, set_for = bt_dsp;
        }
        if (bt_dsp && !size) {
            if (hd) r = rh.dualsense_dsp_build_bt36_report(bt_dsp, bt_state, 0, report), size = RENHDRUM_DS5_BT36_REPORT_SIZE;
            else r = rh.dualsense_dsp_build_bt_report(bt_dsp, RENHDRUM_DS5_BT_BUFFER_LENGTH_DEFAULT, report),
                 size = RENHDRUM_DS5_BT_REPORT_SIZE;
        }
        ReleaseSRWLockExclusive(&lk);
        const int64_t now = hos_now_ns();
        if (size == RENHDRUM_DS5_BT36_REPORT_SIZE) {
            send = r == 1;   /* (0x36: 1 to send; RenHDrum stops by itself two seconds after the last of it) */
        } else {
            if (r == 1) loud_at = now;
            /* (sent while something plays and two seconds after: the radio stays free for the pad's input otherwise) */
            send = r >= 0 && loud_at && now - loud_at < 2000000000LL;
        }
        if (send) {
            if (hos_sdl_hid_write(bt_hid, report, size) < 0) {
                if (++fails >= 50) {
                    hos_log("vibration: the DualSense's Bluetooth reports keep failing: its motors instead");
                    InterlockedExchange(&bt_failed, 1);
                    break;
                }
            } else {
                fails = 0;
            }
        }
        if (now > next + 4 * (int64_t)RENHDRUM_DS5_BT_PERIOD_NS) next = now;   /* (far behind: start again from now) */
        if (next > now) Sleep((DWORD)((next - now + 999999) / 1000000));
    }
    if (hd == 1 && !bt_failed) {   /* (closing: both triggers Off, so none stays stiff; the speaker stops with the reports) */
        int r = -1;
        AcquireSRWLockExclusive(&lk);
        bt_state_off();
        if (bt_dsp) r = rh.dualsense_dsp_build_bt36_report(bt_dsp, bt_state, 0, report);
        ReleaseSRWLockExclusive(&lk);
        if (r == 1) hos_sdl_hid_write(bt_hid, report, RENHDRUM_DS5_BT36_REPORT_SIZE);
    }
    AcquireSRWLockExclusive(&lk);
    tri_pair(bt_tri, 0);
    ReleaseSRWLockExclusive(&lk);
    return 0;
}
static void recreate_bt_dsp(int percent) {
    /* (Bluetooth's samples, and a Steam Controller's, are made inside the report: the strength is the DSP's own volume, 10
       to 100 %, which RenHDrum sets only when one is made. Above 100 % stays at 100 %) */
    const int v = percent < 10 ? 10 : percent > 100 ? 100 : percent;
    AcquireSRWLockExclusive(&life);
    if (sv_dsp) {
        RenHdcDualSenseDsp *dsp = rh.valve_grip_dsp_create_ex(v, true, true), *old;
        if (dsp) {
            AcquireSRWLockExclusive(&lk);
            old = sv_dsp, sv_dsp = dsp;
            ReleaseSRWLockExclusive(&lk);
            rh.dualsense_dsp_destroy(old);
        }
    }
    if (bt_dsp) {
        RenHdcDualSenseDsp *dsp = rh.dualsense_dsp_create(v, false), *old;
        if (dsp) {
            AcquireSRWLockExclusive(&lk);
            old = bt_dsp, bt_dsp = dsp;
            ReleaseSRWLockExclusive(&lk);
            rh.dualsense_dsp_destroy(old);
        }
    }
    ReleaseSRWLockExclusive(&life);
}
int rh_bt_open(void *pad, const char *hid_path) {
    if (!rh_ok || !hid_path || !*hid_path) return 0;
    AcquireSRWLockExclusive(&life);
    int ok = 0;
    if (!closed && !bt_pad) {
        void *hid = hos_sdl_hid_open(hid_path);
        const int p = (int)gain_percent;
        RenHdcDualSenseDsp *dsp = hid ? rh.dualsense_dsp_create(p < 10 ? 10 : p > 100 ? 100 : p, false) : NULL;
        if (dsp) {
            AcquireSRWLockExclusive(&lk);
            bt_dsp = dsp;
            ReleaseSRWLockExclusive(&lk);
            bt_hid = hid, bt_pad = pad;
            InterlockedExchange(&bt_run, 1), InterlockedExchange(&bt_failed, 0);
            bt_thread = CreateThread(NULL, 0, bt_main, NULL, 0, NULL);
            ok = bt_thread != NULL;
        }
        if (!ok) {
            hos_log("vibration: the DualSense's HID device would not open for its Bluetooth haptics: its motors instead");
            AcquireSRWLockExclusive(&lk);
            bt_dsp = NULL;
            ReleaseSRWLockExclusive(&lk);
            if (dsp) rh.dualsense_dsp_destroy(dsp);
            if (hid) hos_sdl_hid_close(hid);
            bt_hid = NULL, bt_pad = NULL;
        } else {
            hos_log("vibration: the DualSense's voice coils over Bluetooth (report %s)", hd_on ? "0x36, HD Haptics" : "0x32");
        }
    }
    ReleaseSRWLockExclusive(&life);
    return ok;
}
int rh_bt_alive(void) { return bt_thread && !bt_failed; }
static void bt_close_locked(void) {   /* (life held) */
    HANDLE thread = bt_thread;
    bt_thread = NULL;
    InterlockedExchange(&bt_run, 0);
    if (thread) WaitForSingleObject(thread, 2000), CloseHandle(thread);   /* (it takes lk only, never life) */
    if (bt_hid) hos_sdl_hid_close(bt_hid);
    bt_hid = NULL, bt_pad = NULL;
    AcquireSRWLockExclusive(&lk);
    RenHdcDualSenseDsp *dsp = bt_dsp;
    bt_dsp = NULL;
    ReleaseSRWLockExclusive(&lk);
    if (dsp) rh.dualsense_dsp_destroy(dsp);
}
void rh_bt_close(void) {
    AcquireSRWLockExclusive(&life);
    bt_close_locked();
    ReleaseSRWLockExclusive(&life);
}
void *rh_bt_owner(void) { return bt_pad; }

/* ---- a Steam Controller (2026)'s grips: report 0x88 every 3.875 ms on its cable, 7.75 ms over Bluetooth or its dongle,
   and the 0x86 reports that keep it in that mode, written beside SDL's own. RenHDrum's experimental: built from one
   person's measurements of the pad (RenHDrum#5) ---- */
#define RH_SV_TAIL_NS 100000000LL   /* (silent reports sent this long after the last that carried vibration) */
static void *sv_hid;
static HANDLE sv_thread;
static volatile LONG sv_run, sv_failed;
static uint8_t sv_format;
static void sv_wait(int64_t ns) {   /* (the periods are under the system's 1 ms sleep: a high-resolution timer, as nn_vi.c's) */
    static _Thread_local HANDLE timer;
    static _Thread_local int tried;
    if (!tried) tried = 1, timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* HIGH_RESOLUTION */, 0x1F0003 /* ALL_ACCESS */);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100 > 0 ? ns / 100 : 1);
        if (SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
}
static DWORD WINAPI sv_main(LPVOID unused) {
    (void)unused;
    void hos_host_thread_register_self(const char *name);   /* (mkw_glue.c) */
    hos_host_thread_register_self("vibration (Steam Controller)");
    const int64_t period = (int64_t)(sv_format == RENHDRUM_VALVE_GRIP_FORMAT_MULAW_8K ? RENHDRUM_VALVE_GRIP_PERIOD_NS_8K
                                                                                      : RENHDRUM_VALVE_GRIP_PERIOD_NS_4K);
    uint8_t report[RENHDRUM_VALVE_GRIP_REPORT_SIZE], modes[RENHDRUM_VALVE_GRIP_MODE_REPORTS_SIZE];
    int64_t next = hos_now_ns(), loud_at = 0;
    int fails = 0, stalled = 0;
    while (sv_run) {
        next += period;
        int r = -1, m = 0;
        AcquireSRWLockExclusive(&lk);
        if (sv_dsp) {
            r = rh.valve_grip_build_report(sv_dsp, sv_format, report);
            m = rh.valve_grip_mode_tick(sv_dsp, sv_format, (uint64_t)hos_now_ns(), stalled != 0, modes);
        }
        ReleaseSRWLockExclusive(&lk);
        stalled = 0;
        const int64_t now = hos_now_ns();
        /* (its mode: on at the first vibration and every 3 s, at once after a stalled write, off after 100 ms quiet) */
        if (m == RENHDRUM_VALVE_GRIP_MODE_REPORT_COUNT)
            for (int k = 0; k < m; k++)
                if (hos_sdl_hid_write(sv_hid, modes + k * RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE,
                                      RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE) < 0)
                    stalled = 1;
        if (r == 1) loud_at = now;
        /* (2: RenHDrum skips one for its clock's drift; silent ones only just after the last that played) */
        if (r >= 0 && r != RENHDRUM_VALVE_GRIP_SKIP && loud_at && now - loud_at < RH_SV_TAIL_NS) {
            if (hos_sdl_hid_write(sv_hid, report, (int)sizeof report) < 0) {
                stalled = 1;
                if (++fails >= 100) {
                    hos_log("vibration: the Steam Controller's grip reports keep failing: its motors instead");
                    InterlockedExchange(&sv_failed, 1);
                    break;
                }
            } else {
                fails = 0;
            }
        }
        if (now > next + 4 * period) next = now;   /* (far behind: start again from now) */
        if (next > now) sv_wait(next - now);
    }
    /* (every way out: its mode off, so it is not left waiting for grip audio) */
    if (rh.valve_grip_mode_off(sv_format, modes) == RENHDRUM_VALVE_GRIP_MODE_REPORT_COUNT)
        for (int k = 0; k < RENHDRUM_VALVE_GRIP_MODE_REPORT_COUNT; k++)
            hos_sdl_hid_write(sv_hid, modes + k * RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE, RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE);
    return 0;
}
int rh_steam_open(void *pad, const char *hid_path, int cable) {
    if (!rh_ok || !hid_path || !*hid_path) return 0;
    AcquireSRWLockExclusive(&life);
    int ok = 0;
    if (!closed && !sv_pad) {
        void *hid = hos_sdl_hid_open(hid_path);
        const int p = (int)gain_percent;
        /* (crisp notes on, and RenHDrum's 40 ppm drift compensation, as its guide says) */
        RenHdcDualSenseDsp *dsp = hid ? rh.valve_grip_dsp_create_ex(p < 10 ? 10 : p > 100 ? 100 : p, true, true) : NULL;
        if (dsp) {
            AcquireSRWLockExclusive(&lk);
            sv_dsp = dsp;
            ReleaseSRWLockExclusive(&lk);
            sv_hid = hid, sv_pad = pad;
            sv_format = cable ? RENHDRUM_VALVE_GRIP_FORMAT_MULAW_8K : RENHDRUM_VALVE_GRIP_FORMAT_MULAW_4K;
            InterlockedExchange(&sv_run, 1), InterlockedExchange(&sv_failed, 0);
            sv_thread = CreateThread(NULL, 0, sv_main, NULL, 0, NULL);
            ok = sv_thread != NULL;
        }
        if (!ok) {
            hos_log("vibration: the Steam Controller's HID device would not open for its grips: its motors instead");
            AcquireSRWLockExclusive(&lk);
            sv_dsp = NULL;
            ReleaseSRWLockExclusive(&lk);
            if (dsp) rh.dualsense_dsp_destroy(dsp);
            if (hid) hos_sdl_hid_close(hid);
            sv_hid = NULL, sv_pad = NULL;
        } else {
            hos_log("vibration: the Steam Controller's grips (report 0x88, %s; RenHDrum's experimental)",
                    cable ? "8 kHz on its cable" : "4 kHz without one");
        }
    }
    ReleaseSRWLockExclusive(&life);
    return ok;
}
int rh_steam_alive(void) { return sv_thread && !sv_failed; }
static void sv_close_locked(void) {   /* (life held) */
    HANDLE thread = sv_thread;
    sv_thread = NULL;
    InterlockedExchange(&sv_run, 0);
    if (thread) WaitForSingleObject(thread, 2000), CloseHandle(thread);   /* (it takes lk only, never life) */
    if (sv_hid) hos_sdl_hid_close(sv_hid);
    sv_hid = NULL, sv_pad = NULL;
    AcquireSRWLockExclusive(&lk);
    RenHdcDualSenseDsp *dsp = sv_dsp;
    sv_dsp = NULL;
    ReleaseSRWLockExclusive(&lk);
    if (dsp) rh.dualsense_dsp_destroy(dsp);
}
void rh_steam_close(void) {
    AcquireSRWLockExclusive(&life);
    sv_close_locked();
    ReleaseSRWLockExclusive(&life);
}
void *rh_steam_owner(void) { return sv_pad; }

/* ---- an Xbox pad's four motors, a Switch pad's packets ---- */
int rh_pad_open(void *pad, int backend, int bluetooth, int pro) {
    if (!rh_ok || !pad) return 0;
    AcquireSRWLockExclusive(&life);
    int ok = 0;
    for (int i = 0; i < RH_PADS && !closed && !ok; i++) {
        if (pads[i].pad) continue;
        RenHdcXboxRumble *x = backend == VIB_RH_XBOX ? rh.xbox_rumble_create(bluetooth != 0) : NULL;
        RenHdcEngine *e = backend == VIB_RH_SWITCH ? rh.engine_create(bluetooth != 0, pro != 0) : NULL;
        if (!x && !e) break;
        AcquireSRWLockExclusive(&lk);
        pads[i].pad = pad, pads[i].backend = backend, pads[i].xbox = x, pads[i].engine = e;
        ReleaseSRWLockExclusive(&lk);
        ok = 1;
    }
    ReleaseSRWLockExclusive(&life);
    return ok;
}
void rh_pad_close(void *pad) {
    AcquireSRWLockExclusive(&life);
    for (int i = 0; i < RH_PADS; i++) {
        if (!pad || pads[i].pad != pad) continue;
        AcquireSRWLockExclusive(&lk);
        RenHdcXboxRumble *x = pads[i].xbox;
        RenHdcEngine *e = pads[i].engine;
        memset(&pads[i], 0, sizeof pads[i]);
        ReleaseSRWLockExclusive(&lk);
        if (x) rh.xbox_rumble_destroy(x);   /* (out of the list first: nothing else reaches it now) */
        if (e) rh.engine_destroy(e);
    }
    ReleaseSRWLockExclusive(&life);
}
int rh_xbox_poll(void *pad, int64_t now, uint16_t motors[4]) {
    int due = 0;
    AcquireSRWLockShared(&lk);
    for (int i = 0; i < RH_PADS; i++)
        if (pads[i].pad == pad && pads[i].xbox) due = rh.xbox_rumble_tick(pads[i].xbox, now / 1000, motors) == 1;
    ReleaseSRWLockShared(&lk);
    if (due) {   /* (the Vibration strength on each motor's speed) */
        const float g = vibration_gain((int)gain_percent);
        for (int m = 0; m < 4; m++) {
            const float v = motors[m] * g;
            motors[m] = (uint16_t)(v > 65535.0f ? 65535.0f : v);
        }
    }
    return due;
}
int rh_switch_poll(void *pad, int64_t now, uint8_t packet[10]) {
    int due = 0;
    AcquireSRWLockShared(&lk);
    for (int i = 0; i < RH_PADS; i++)
        if (pads[i].pad == pad && pads[i].engine) due = rh.engine_tick(pads[i].engine, now / 1000, packet) == 1;
    ReleaseSRWLockShared(&lk);
    return due;
}

void rh_shutdown(void) {
    if (!rh_ok) return;
    AcquireSRWLockExclusive(&life);
    closed = 1;
    usb_close_locked();
    bt_close_locked();
    sv_close_locked();
    ReleaseSRWLockExclusive(&life);
    for (int i = 0; i < RH_PADS; i++)
        if (pads[i].pad) rh_pad_close(pads[i].pad);
}
