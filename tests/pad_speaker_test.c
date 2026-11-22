/* host/pad_speaker.c itself against a made-up DualSense: SDL's endpoint calls and the pad's output report are stood in
   for here, and the test plays the mixer, SDL's audio thread and the pads' poll in turn. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int64_t now_ns;
int64_t hos_now_ns(void) { return now_ns; }
void hos_log(const char *fmt, ...) { (void)fmt; }

/* the menu's Controller speaker page: its rows' values, or -1 where the renderer has none yet */
static int menu_volume = -1, menu_sync = -1000, menu_class[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int menu_headphones = -1, menu_headphone_volume = -1;
int rx_option(int id, int otherwise) {
    /* (gpu/vk/vk_api.h's numbers: the volume 140, the classes 141 to 152, the sync 153, the headphones 154 and 155) */
    if (id == 153) return menu_sync == -1000 ? otherwise : menu_sync;   /* (the sync goes below 0: -1000 is "no renderer yet") */
    const int v = id == 140 ? menu_volume : id >= 141 && id < 153 ? menu_class[id - 141]
                : id == 154 ? menu_headphones : id == 155 ? menu_headphone_volume : -2;
    CHECK(v != -2);
    return v < 0 ? otherwise : v;
}
static int pad_headphones = -1;   /* (what the made-up pad's report says: -1 nothing) */
int hos_sdl_pad_headphones(void *pad) { (void)pad; return pad_headphones; }

static int endpoints = 1, endpoint_channels = 4, alive = 1, opened, closed, open_fails;
static void *unmatched_pad;
static const char *endpoint_name = "Speakers (DualSense Wireless Controller)";
static void (*pull)(void *user, uint8_t *out, int len);
static uint8_t report[64];
static int reports, report_size, reject_report;
static void *report_pad;

/* host/hos_sdl.c's playback devices: a sound card, then the pad's (when there) */
int hos_sdl_output_count(void) { return endpoints ? 2 : 1; }
int hos_sdl_output_info(int index, char *id, int id_capacity, char *name, int name_capacity) {
    snprintf(id, (size_t)id_capacity, "%s", index == 0 ? "card" : "pad");
    snprintf(name, (size_t)name_capacity, "%s", index == 0 ? "Speakers (some sound card)" : endpoint_name);
    return index == 0 || (index == 1 && endpoints);
}
int hos_sdl_output_channels(int index) { return index == 0 ? 2 : endpoint_channels; }
int hos_sdl_output_resolve(const char *id) { return !strcmp(id, "pad") ? 9 : 7; }
int hos_sdl_pad_audio_endpoint(void *pad) { return pad && pad != unmatched_pad && endpoints && endpoint_channels >= 4 ? 9 : -1; }
void *hos_sdl_audio_open(unsigned device, int freq, int channels, int s16, int frames, void (*p)(void *user, uint8_t *out, int len), void *user) {
    CHECK(device == 9 && freq == 48000 && channels == 4 && !s16 && !user && frames == (getenv("HOS_PAD_SPEAKER_LOW_LATENCY") ? 0 : 256));
    if (open_fails) return NULL;
    pull = p, opened++;
    return &opened;
}
int hos_sdl_audio_alive(void *stream) { CHECK(stream == &opened); return alive; }
void hos_sdl_audio_close(void *stream) { CHECK(stream == &opened); pull = NULL, closed++; }
/* host/rumble_renhdrum.c: the vibration's stream on a cabled DualSense, none unless a test gives the pad one */
static void *vib_owner;
static int vib_alive = 1;
void *rh_usb_owner(void) { return vib_owner; }
int rh_usb_alive(void) { return vib_alive; }
int rh_usb_restart_audio(void) { return vib_alive; }
static int vib_token = 9;   /* (the vibration's endpoint: the pad's own, 9, unless a test moves it) */
int rh_usb_token(void) { return vib_owner && vib_alive ? vib_token : -1; }
static int vib_want = -1, vib_gain = -1;   /* (what the speaker last asked of the vibration's stream: RenHDrum's to open) */
int rh_usb_speaker(int want, int gain) { vib_want = want, vib_gain = gain; return 1; }
int hos_sdl_pad_effect(void *pad, const void *data, int size) {
    CHECK(size <= (int)sizeof report);
    memcpy(report, data, (size_t)size), report_size = size, report_pad = pad, reports++;
    return !reject_report;
}

#include "pad_speaker.c"
#ifndef _WIN32
void Sleep(DWORD ms) { (void)ms; }   /* (host/posix's in the game) */
#endif

static float out[4800 * 4], in[4800];
static void play(int frames) { CHECK(pull); memset(out, 0x55, sizeof out); pull(NULL, (uint8_t *)out, frames * 4 * (int)sizeof(float)); }
static void second(void) { now_ns += 1000000000; }
/* the mixer: frames of a sine, on from where the last call stopped, in its 240-sample frames */
static double tone_phase;
static void tone(double hz, float amp, int frames) {
    for (int done = 0; done < frames; done += 240) {
        for (int i = 0; i < 240; i++) in[i] = amp * (float)sin(tone_phase), tone_phase += 2 * 3.14159265358979323846 * hz / 48000;
        hos_pad_speaker_write(in, 240);
    }
}
static float peak(int from, int to) {   /* the loudest of the speaker's channel over out[from..to) */
    float p = 0;
    for (int i = from; i < to; i++) p = fabsf(out[i * 4]) > p ? fabsf(out[i * 4]) : p;
    return p;
}
static uint32_t fill_now(void) { return ring_fill(); }

int main(int argc, char **argv) {
    int pad_a, pad_b;
    CHECK(argc == 2);
    if (!strcmp(argv[1], "wrap")) {
        hos_pad_speaker_pad(&pad_a, 1);
        CHECK(hos_pad_speaker_active());
        InterlockedExchange((volatile LONG *)&ring_w, (LONG)(UINT32_MAX - 119));
        InterlockedExchange((volatile LONG *)&ring_r, (LONG)(UINT32_MAX - 119));
        tone(440, 0.5f, 1200);
        CHECK(ring_fill() == 1200);
        play(240);
        CHECK(ring_fill() < 1200 && ring_fill() > 900);
        return 0;
    }
    if (!strcmp(argv[1], "reject")) {
        reject_report = 1;
        hos_pad_speaker_pad(&pad_a, 1);
        CHECK(!hos_pad_speaker_active() && !hos_pad_speaker_headphones_all());
        return 0;
    }
    if (!strcmp(argv[1], "off")) {   /* HOS_PAD_SPEAKER=0 */
        hos_pad_speaker_pad(&pad_a, 1);
        CHECK(!opened && !reports && !hos_pad_speaker_active());
        puts("pad speaker: switched off, a DualSense on a cable is left alone");
        return 0;
    }
    if (!strcmp(argv[1], "modes")) {   /* HOS_PAD_SPEAKER_UI=0, HOS_PAD_SPEAKER_OCARINA=1, HOS_PAD_SPEAKER_AGONY=7 */
        hos_pad_speaker_pad(NULL, 0);
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_NONE) == HOS_PAD_SPEAKER_OFF && hos_pad_speaker_mode(AUDIO_SOURCE_CLASSES) == HOS_PAD_SPEAKER_OFF);
        CHECK(hos_pad_speaker_mode(-3) == HOS_PAD_SPEAKER_OFF);
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_NAVI_VOICE) == HOS_PAD_SPEAKER_ONLY && hos_pad_speaker_mode(AUDIO_SOURCE_NAVI_SOUND) == HOS_PAD_SPEAKER_BOTH);
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_UI) == HOS_PAD_SPEAKER_OFF && hos_pad_speaker_mode(AUDIO_SOURCE_OCARINA) == HOS_PAD_SPEAKER_ONLY);
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_AGONY) == HOS_PAD_SPEAKER_BOTH);   /* (out of range: the nearest) */
        menu_class[AUDIO_SOURCE_NAVI_VOICE - 1] = 2, menu_class[AUDIO_SOURCE_UI - 1] = 2, menu_class[AUDIO_SOURCE_FAIRY - 1] = 9;
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_NAVI_VOICE) == HOS_PAD_SPEAKER_ONLY);   /* (read at the poll, not at every sound) */
        hos_pad_speaker_pad(NULL, 0);
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_NAVI_VOICE) == HOS_PAD_SPEAKER_BOTH);   /* the menu's row */
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_FAIRY) == HOS_PAD_SPEAKER_BOTH);
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_UI) == HOS_PAD_SPEAKER_OFF);   /* the switch, over the menu's */
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_LOW_HEALTH) == HOS_PAD_SPEAKER_ONLY);   /* (the later classes: their own rows) */
        menu_class[AUDIO_SOURCE_BOTTLE - 1] = 2, menu_class[AUDIO_SOURCE_FISHING - 1] = 0;
        hos_pad_speaker_pad(NULL, 0);
        CHECK(hos_pad_speaker_mode(AUDIO_SOURCE_BOTTLE) == HOS_PAD_SPEAKER_BOTH && hos_pad_speaker_mode(AUDIO_SOURCE_FISHING) == HOS_PAD_SPEAKER_OFF);
        puts("pad speaker: each class's mode from the menu's row, its default before, or its switch over both");
        return 0;
    }
    if (!strcmp(argv[1], "menu")) {   /* no switch: the menu's rows alone */
        menu_volume = 60;
        for (int i = 0; i < 12; i++) menu_class[i] = 0;
        hos_pad_speaker_pad(&pad_a, 1);
        CHECK(!opened && !reports);   /* every kind Off: the pad keeps its sound */
        menu_class[AUDIO_SOURCE_OCARINA - 1] = 1; second(); second(); hos_pad_speaker_pad(&pad_a, 1);
        CHECK(opened == 1 && reports == 1 && report[5] == 100 && hos_pad_speaker_mode(AUDIO_SOURCE_OCARINA) == HOS_PAD_SPEAKER_ONLY);
        second(); hos_pad_speaker_pad(&pad_a, 1);
        CHECK(reports == 1 && !test_left);   /* (nothing moved: nothing sent) */
        menu_volume = 30; second(); hos_pad_speaker_pad(&pad_a, 1);
        CHECK(opened == 1 && reports == 1 && test_left > 0);   /* a tone at the new volume (ours: the pad's own stays at its top) */
        InterlockedExchange(&test_left, 0);
        for (int k = 0; k < 20; k++) tone(1000, 0.1f, 240), play(240);
        CHECK(peak(0, 240) > 0.057f && peak(0, 240) < 0.063f);   /* 30%: 0.3 x 2 of the mixer's */
        menu_volume = 200; second(); hos_pad_speaker_pad(&pad_a, 1); InterlockedExchange(&test_left, 0);
        for (int k = 0; k < 20; k++) tone(1000, 0.1f, 240), play(240);
        CHECK(peak(0, 240) > 0.38f && peak(0, 240) < 0.42f);   /* 200%: four times */
        menu_volume = 900; second(); hos_pad_speaker_pad(&pad_a, 1); InterlockedExchange(&test_left, 0);
        CHECK(volume == 200);
        menu_class[AUDIO_SOURCE_OCARINA - 1] = 0; second(); hos_pad_speaker_pad(&pad_a, 1);
        CHECK(closed == 1 && reports == 2 && report[7] == 0 && !hos_pad_speaker_active());   /* the last kind Off: the jack back */
        puts("pad speaker: open while a kind of sound plays there, the menu's volume sent when it moves");
        return 0;
    }
    if (!strcmp(argv[1], "ride_other")) {   /* the vibration found an endpoint by name that is not this pad's own */
        vib_owner = &pad_a, vib_token = 5;
        hos_pad_speaker_pad(&pad_a, 1);
        CHECK(!riding && opened == 1 && vib_want == -1 && hos_pad_speaker_active());   /* (not shared: our own, on the pad's) */
        vib_token = 9; second(); hos_pad_speaker_pad(&pad_a, 1);
        CHECK(riding && closed == 1 && vib_want == RH_SPEAKER_GAME);   /* (matched: shared) */
        puts("pad speaker: the vibration's stream shared only on the endpoint matched to the pad");
        return 0;
    }
    if (!strcmp(argv[1], "ride")) {   /* the vibration's stream on the pad carries the speaker's channels */
        hos_pad_speaker_pad(&pad_a, 1);
        CHECK(opened == 1 && !riding && hos_pad_speaker_active());   /* (no vibration on it: our own) */
        vib_owner = &pad_a; second(); hos_pad_speaker_pad(&pad_a, 1);
        /* ours gone; the speaker opened by RenHDrum's block at our gain, no second report of ours */
        CHECK(closed == 1 && riding && hos_pad_speaker_active() && reports == 1 && !pull && vib_want == RH_SPEAKER_GAME && vib_gain == 4);
        for (int k = 0; k < 10; k++) tone(1000, 0.25f, 240);
        static float buf[480 * 4];
        for (int i = 0; i < 480; i++) buf[i * 4] = 0.0f, buf[i * 4 + 1] = 0.25f, buf[i * 4 + 2] = 0.5f, buf[i * 4 + 3] = -0.5f;
        hos_pad_speaker_fill(buf, 480);
        float p = 0;
        for (int i = 0; i < 480; i++) {   /* (HD Haptics' taps on the second channel kept: the game's added to them) */
            CHECK(buf[i * 4 + 2] == 0.5f && buf[i * 4 + 3] == -0.5f && fabsf(buf[i * 4 + 1] - 0.25f - buf[i * 4]) < 1e-6f);
            p = fabsf(buf[i * 4]) > p ? fabsf(buf[i * 4]) : p;
        }
        CHECK(p > 0.4f);   /* (the game's sounds added, the voice coils' left as the vibration wrote them) */
        for (int k = 0; k < 10; k++) tone(1000, 0.25f, 240);
        for (int i = 0; i < 480; i++) buf[i * 4] = -0.9f, buf[i * 4 + 1] = 0.9f;
        hos_pad_speaker_fill(buf, 480);
        for (int i = 0; i < 480; i++) CHECK(fabsf(buf[i * 4]) <= 1.0f && fabsf(buf[i * 4 + 1]) <= 1.0f);   /* (clamped) */
        for (int i = 0; i < 12; i++) menu_class[i] = 0;
        second(); hos_pad_speaker_pad(&pad_a, 1);
        CHECK(!riding && vib_want == RH_SPEAKER_NONE && reports == 1);   /* nothing there: the speaker RenHDrum's again */
        for (int i = 0; i < 12; i++) menu_class[i] = -1;
        second(); hos_pad_speaker_pad(&pad_a, 1);
        CHECK(riding && vib_want == RH_SPEAKER_GAME);
        vib_alive = 0; second(); second(); hos_pad_speaker_pad(&pad_a, 1);
        CHECK(!riding && opened == 2 && pull && hos_pad_speaker_active());   /* the vibration's stream gone: ours again */
        hos_pad_speaker_fill(buf, 480);   /* (nothing written into a stream that no longer carries it) */
        hos_pad_speaker_shutdown();
        CHECK(closed == 2 && !hos_pad_speaker_active());
        puts("pad speaker: on the vibration's stream while it plays the pad, on its own otherwise");
        return 0;
    }
    if (!strcmp(argv[1], "legacy")) {   /* HOS_PAD_SPEAKER_HPF=0, _LIMITER=0, _LOW_LATENCY=0: each fix turned back for a run */
        hos_pad_speaker_pad(&pad_a, 1);
        CHECK(opened == 1 && target == 48 * 40);   /* (SDL's own pull size, checked by the opener above; the 40 ms ring) */
        for (int k = 0; k < 40; k++) tone(60, 0.25f, 240), play(240);
        CHECK(peak(0, 240) > 0.2f);   /* the bass kept */
        for (int k = 0; k < 40; k++) tone(1000, 3.0f, 240), play(240);
        int flat = 0;
        for (int i = 0; i < 240; i++) flat += fabsf(out[i * 4]) > 0.99f;
        CHECK(flat > 20 && peak(0, 240) <= 1.0f);   /* the earlier curve: the tops pressed flat against full scale */
        for (int k = 0; k < 100; k++) tone(1000, 0.25f, 480), play(481);
        CHECK(underruns > 0 || fill_now() < 48 * 40);   /* (no following of the pad's clock) */
        puts("pad speaker: each fix turned back for a run: the bass, the earlier curve, the 40 ms ring");
        return 0;
    }
    CHECK(!strcmp(argv[1], "play"));   /* HOS_PAD_SPEAKER_VOLUME=50: the mixer's own level (half of twice) */

    /* nothing opens without a pad, on a wireless pad, or while the pad has no four-channel endpoint */
    hos_pad_speaker_pad(NULL, 0); second();
    hos_pad_speaker_pad(&pad_a, 0); second();
    endpoints = 0; hos_pad_speaker_pad(&pad_a, 1); second();
    endpoints = 1; endpoint_channels = 2; hos_pad_speaker_pad(&pad_a, 1); second();
    endpoint_channels = 4; unmatched_pad = &pad_a; hos_pad_speaker_pad(&pad_a, 1); second();
    CHECK(!opened && !reports && !hos_pad_speaker_active());
    for (int i = 0; i < 240; i++) in[i] = 0.5f;
    hos_pad_speaker_write(in, 240);   /* (not open: dropped) */
    unmatched_pad = NULL; open_fails = 1; second(); second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(!opened && !reports);
    open_fails = 0; hos_pad_speaker_pad(&pad_a, 1);
    CHECK(!opened);   /* (asked again only two seconds later) */
    second(); second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(opened == 1 && hos_pad_speaker_active());

    /* the pad was told: audio control and speaker volume, the speaker alone, its volume at the top, gain through the second control */
    CHECK(reports == 1 && report_pad == &pad_a && report_size == 47);
    CHECK(report[0] == 0xa0 && report[1] == 0x80 && report[5] == 100 && report[7] == 0x30 && report[37] == 4);
    for (int i = 0; i < 47; i++) if (i != 0 && i != 1 && i != 5 && i != 7 && i != 37) CHECK(!report[i]);

    /* silence until 20 ms are there, then the mixer's sound on the speaker's two channels and nothing on the coils' */
    play(480);
    for (int i = 0; i < 480 * 4; i++) CHECK(out[i] == 0);
    tone(1000, 0.25f, 720);   /* 15 ms */
    play(240);
    for (int i = 0; i < 240 * 4; i++) CHECK(out[i] == 0);
    tone(1000, 0.25f, 240);   /* 20 ms */
    play(240);
    CHECK(peak(0, 240) > 0.05f);
    for (int k = 0; k < 40; k++) tone(1000, 0.25f, 240), play(240);
    for (int i = 0; i < 240; i++) CHECK(out[i * 4 + 1] == out[i * 4] && out[i * 4 + 2] == 0 && out[i * 4 + 3] == 0);
    CHECK(peak(0, 240) > 0.23f && peak(0, 240) < 0.27f && underruns == 0);   /* (1 kHz passes as it is) */

    /* the bass goes: a 60 Hz rumble comes out at a few percent of itself */
    for (int k = 0; k < 40; k++) tone(60, 0.25f, 240), play(240);
    float bass = 0;
    for (int k = 0; k < 8; k++) tone(60, 0.25f, 240), play(240), bass = peak(0, 240) > bass ? peak(0, 240) : bass;
    CHECK(bass < 0.025f);

    /* far too loud: turned down to under full scale as a whole, never cut flat (no run of samples at the top) */
    for (int k = 0; k < 40; k++) tone(1000, 3.0f, 240), play(240);
    int flat = 0;
    for (int k = 0; k < 8; k++) {
        tone(1000, 3.0f, 240), play(240);
        CHECK(peak(0, 240) > 0.8f && peak(0, 240) <= 0.9f);
        for (int i = 0; i < 240; i++) flat += fabsf(out[i * 4]) >= 0.999f;
    }
    CHECK(flat == 0);
    /* and the gain comes back once it is quiet again (100 ms) */
    for (int k = 0; k < 120; k++) tone(1000, 0.25f, 240), play(240);
    CHECK(peak(0, 240) > 0.23f && peak(0, 240) < 0.27f);

    /* the two clocks apart by 0.2% either way: the ring stays near its 20 ms, nothing dropped, nothing runs dry */
    for (int dir = -1; dir <= 1; dir += 2) {
        for (int k = 0; k < 2000; k++) tone(1000, 0.25f, 480), play(480 + dir);   /* (10 ms at a time, as the two do) */
        CHECK(underruns == 0 && trims == 0);
        CHECK(fill_avg > 960 - 400 && fill_avg < 960 + 400);   /* (within 8 ms of the target, as the pull sees it) */
    }

    /* a stall (the pad stopped pulling): back to the target at once, the newest samples kept */
    tone(1000, 0.25f, 4800);
    play(240);
    CHECK(trims == 1 && fill_now() <= 960 && fill_now() > 960 - 300);
    /* a stopped pull never lets the ring wrap */
    for (int f = 0; f < 200; f++) tone(1000, 0.25f, 240);
    CHECK(ring_w - ring_r <= RING);

    /* the test tone: 0.6 s over whatever plays, on the speaker's channels only, then nothing more */
    while (primed) play(4800);   /* (drained: silence from here) */
    CHECK(hos_pad_speaker_test());
    play(4800);
    int loud = 0;
    for (int i = 0; i < 4800; i++) { CHECK(out[i * 4 + 2] == 0 && out[i * 4 + 3] == 0); loud += out[i * 4] > 0.15f; }
    CHECK(loud > 100);
    for (int i = 0; i < 12; i++) play(4800);
    play(4800);
    for (int i = 0; i < 4800 * 4; i++) CHECK(out[i] == 0);

    /* the menu's Controller sync holds the pad's sound back by that much more: the ring's target with it */
    menu_sync = 50; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(target == 48 * 70);
    menu_sync = 900; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(target == 48 * 170 && hos_pad_speaker_tv_delay() == 0);   /* (150 ms at most) */
    /* below 0 the TV's copy waits instead: the pad's ring at its own 20 ms */
    menu_sync = -30; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(target == 48 * 20 && hos_pad_speaker_tv_delay() == 48 * 30);
    menu_sync = -900; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(hos_pad_speaker_tv_delay() == 48 * 100);   /* (-100 ms at most) */
    menu_sync = 0; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(target == 48 * 20 && hos_pad_speaker_tv_delay() == 0);

    /* the ocarina on the pad: the rumble at half while it plays, and for 200 ms after */
    CHECK(hos_pad_speaker_rumble_percent() == 100);
    hos_pad_speaker_ocarina_seen();
    CHECK(hos_pad_speaker_rumble_percent() == 50);
    now_ns += 150000000;
    CHECK(hos_pad_speaker_rumble_percent() == 50);
    now_ns += 100000000;
    CHECK(hos_pad_speaker_rumble_percent() == 100);

    /* headphones in the jack (All sound, the default): the whole mix to them in stereo, nothing classed, the pad told */
    pad_headphones = 1; reports = 0; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(reports == 1 && report[0] == 0x90 && report[4] == 0x7f * 80 / 100 && report[7] == 0 && report[5] == 0);
    CHECK(hos_pad_speaker_headphones_all() && !hos_pad_speaker_active() && hos_pad_speaker_tv_delay() == 0);
    {
        static int16_t mix[480 * 6];
        for (int i = 0; i < 480; i++) mix[2 * i] = 16384, mix[2 * i + 1] = -8192;
        InterlockedExchange((volatile LONG *)&ring_r, ring_w), InterlockedExchange(&primed, 0);   /* (the ring emptied of what came before) */
        for (int k = 0; k < 6; k++) hos_pad_speaker_write_stereo(mix, 480, 2);
        hos_pad_speaker_write(in, 240);   /* (the classed sounds' mono: not while all of it goes there) */
        play(480);
        for (int i = 0; i < 480; i++) CHECK(fabsf(out[i * 4] - 0.5f) < 0.001f && fabsf(out[i * 4 + 1] + 0.25f) < 0.001f);
        for (int i = 0; i < 480 * 3; i++) mix[i] = 0;
        for (int i = 0; i < 480; i++) mix[6 * i + 2] = 16384;   /* (5.1: the centre to both sides) */
        InterlockedExchange((volatile LONG *)&ring_r, ring_w), InterlockedExchange(&primed, 0);   /* (the ring emptied of what came before) */
        for (int k = 0; k < 6; k++) hos_pad_speaker_write_stereo(mix, 480, 6);
        play(480);
        CHECK(fabsf(out[0] - 0.3535f) < 0.001f && fabsf(out[1] - 0.3535f) < 0.001f);
    }
    CHECK(hos_pad_speaker_rumble_percent() == 100);
    hos_pad_speaker_ocarina_seen();
    CHECK(hos_pad_speaker_rumble_percent() == 100);   /* (no speaker playing: the rumble as it is) */
    /* Unplug while full-mix samples are queued: none may reach the speaker. */
    CHECK(fill_now() > 0);
    pad_headphones = 0; reports = 0; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(hos_pad_speaker_active() && fill_now() == 0 && !primed);
    play(480);
    CHECK(peak(0, 480) == 0);
    pad_headphones = 1; second(); hos_pad_speaker_pad(&pad_a, 1);
    /* the headphone volume moved: told again */
    menu_headphone_volume = 40; reports = 0; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(reports == 1 && report[4] == 0x7f * 40 / 100);
    /* Controller sounds: the classed sounds to the headphones (bass kept), the rest stays on the TV */
    menu_headphones = 1; reports = 0; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(reports == 1 && report[0] == 0x90 && !hos_pad_speaker_headphones_all() && hos_pad_speaker_active());
    InterlockedExchange((volatile LONG *)&ring_r, ring_w), InterlockedExchange(&primed, 0);
    for (int k = 0; k < 40; k++) tone(60, 0.25f, 240), play(240);
    CHECK(peak(0, 240) > 0.2f);   /* (60 Hz: headphones play it, at the mixer's own level) */
    /* pulled out: back to the speaker */
    pad_headphones = 0; reports = 0; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(reports == 1 && report[0] == 0xa0 && report[7] == 0x30 && hos_pad_speaker_active());
    /* Off: headphones in, the speaker plays all the same */
    menu_headphones = 0; pad_headphones = 1; reports = 0; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(reports == 0 && hos_pad_speaker_active() && !hos_pad_speaker_headphones_all());
    pad_headphones = 0; menu_headphones = -1; second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(reports == 0);
    reports = 1; opened = 1; closed = 0;   /* (the current stream, for the counts below) */

    /* the cable is pulled (the pad is on Bluetooth now): the jack gets its sound back and the endpoint closes */
    second(); hos_pad_speaker_pad(&pad_a, 0);
    CHECK(closed == 1 && !hos_pad_speaker_active() && reports == 2 && report[0] == 0x80 && report[7] == 0 && report[5] == 0);
    CHECK(!hos_pad_speaker_test());
    /* back on the cable; then the endpoint dies under it and comes back */
    second(); second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(opened == 2 && reports == 3 && report[7] == 0x30);
    alive = 0; second(); second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(closed == 2 && opened == 3 && hos_pad_speaker_active());
    alive = 1;
    /* another DualSense takes its place */
    second(); second(); hos_pad_speaker_pad(&pad_b, 1);
    CHECK(closed == 3 && opened == 4 && report_pad == &pad_b);
    /* the pad is gone: no report to a pad that is not there */
    reports = 0; second(); hos_pad_speaker_pad(NULL, 0);
    CHECK(closed == 4 && reports == 0 && !hos_pad_speaker_active());

    /* the game closes: the jack back, and nothing opens after */
    second(); second(); hos_pad_speaker_pad(&pad_a, 1);
    CHECK(opened == 5);
    hos_pad_speaker_shutdown();
    CHECK(closed == 5 && reports == 2 && report[7] == 0 && !hos_pad_speaker_active());
    second(); second(); hos_pad_speaker_pad(&pad_a, 1);
    hos_pad_speaker_shutdown();
    CHECK(opened == 5 && closed == 5 && reports == 2);
    puts("pad speaker: opens on a wired DualSense's endpoint, routes the pad, primes, plays without bass or clipping, follows the pad's clock, trims a stall, tests, syncs and closes");
    return 0;
}
