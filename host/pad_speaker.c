/* (longshot) The controller's speaker: the DualSense's own sound card, its first two channels (host/pad_speaker.h).

   Three threads meet here. The game's mixer (host/nn_audio.c) writes the chosen sounds, mono at 48 kHz, into a ring;
   a stream on the pad's endpoint pulls the speaker's two channels from it: the vibration's (host/rumble_renhdrum.c, its
   voice coils on the other two) when it plays the pad, else our own (its voice coils' two silent); the pads' poll (host/pad_sdl.c) opens the endpoint when
   a DualSense on a cable is there and the speaker is wanted, and lets it go otherwise.

   The pad plays its first two channels through the headphone jack unless told otherwise: an output report (SDL's
   SendGamepadEffect, the DualSense's "effects" block) routes them to the speaker and sets its volume, and the
   opposite one gives the jack back when the endpoint closes. On the vibration's stream that report is RenHDrum's speaker
   block (host/rumble_renhdrum.c rh_usb_speaker), the one that also opens the speaker for its HD Haptics: the game's
   sounds are added to its taps there, never over them.

   The ring holds what the mixer wrote and SDL has not pulled yet: about 20 ms, the two clocks kept together by
   reading a hair faster or slower (see "the ring").

   What plays there is the player's: the menu's Controller speaker page, a row a class (Off, the controller only, the
   controller and the TV), the speaker's volume and its sync (a delay for a TV that plays later). The endpoint is open only while some class plays there.

   HOS_PAD_SPEAKER=0: never opened. HOS_PAD_SPEAKER=1: opened even with every class Off (testing).
   HOS_PAD_SPEAKER_<CLASS>=<0-2>: that class's mode for the run, over the menu's (NAVI_VOICE, NAVI_SOUND, TARGET, OCARINA,
   PICKUP, ITEM, FAIRY, UI, AGONY, LOW_HEALTH, FISHING, BOTTLE). HOS_PAD_SPEAKER_VOLUME=<0-200>: the volume in percent, over the menu's. HOS_PAD_SPEAKER_GAIN=<0-7>: its preamp gain
   (default 4). HOS_PAD_SPEAKER_BUFFER_MS=<5-100>: the ring's target before the sync (default 20).
   Each of the crackle and delay fixes can be turned back for a run: HOS_PAD_SPEAKER_HPF=0 (the bass kept),
   HOS_PAD_SPEAKER_LIMITER=0 (the earlier sample-by-sample curve), HOS_PAD_SPEAKER_LOW_LATENCY=0 (the earlier 40 ms ring). HOS_PAD_SPEAKER_PATH=<0-3>: the pad's output path (default 3: the speaker alone; 2: speaker and jack).
   HOS_PAD_SPEAKER_TEST=1: a tone when the endpoint opens.

   Headphones in the pad's jack (the pad says so in its input report, host/hos_sdl.c hos_sdl_pad_headphones): with the
   menu's Headphones on the controller at All sound (the default, as on the console) the whole game's mix goes to them in
   stereo and the TV plays silence; at Controller sounds the classed sounds go to them instead of the speaker; Off leaves
   them out of it (the speaker plays, as before). HOS_PAD_SPEAKER_HEADPHONES=0 or 1: unplugged or plugged for a run, over
   what the pad says (a pad that does not say: unplugged). HOS_PAD_SPEAKER_HEADPHONE_MODE=<0-2>: the menu's row for a run.
   The Controller sync below 0 holds the TV's copy of the Controller + TV sounds back instead (host/nn_audio.c), for a
   pad that plays later than the TV. HOS_PAD_SPEAKER_OCARINA_RUMBLE=<0-100>: the rumble's strength while an ocarina sound
   plays on the pad, in percent (default 50; 100: as it is). */
#include "pad_speaker.h"
#include "audio_source.h"
#include "hos_sdl.h"
#include "rumble_renhdrum.h"   /* (the vibration's stream on a cabled DualSense, which carries ours when it plays) */
#include "../gpu/vk/vk_api.h"   /* (HOS_OPT_PAD_SPEAKER_*) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void hos_log(const char *fmt, ...);
int64_t hos_now_ns(void);

enum {
    RING = 16384,                                    /* samples (a power of two): 341 ms */
    TEST_FRAMES = HOS_PAD_SPEAKER_RATE * 6 / 10,
    RETRY_NS_PER_S = 1000000000
};

static float ring[RING][2];            /* (left and right: the same on the speaker; the mix's two sides on headphones) */
static volatile uint32_t ring_w, ring_r;   /* (free-running: the mixer moves w, the pull moves r) */
static volatile LONG primed;           /* 0: the pull waits for the target's samples before it plays */
static volatile LONG stream_on;        /* 1 while the endpoint's stream is open */
static volatile LONG test_left;        /* frames of the test tone still to play */
static volatile LONG underruns, trims;
static void *stream;                   /* (the poll thread's: our own stream, when the vibration has none on the pad) */
static volatile LONG riding;           /* 1 while the vibration's stream (host/rumble_renhdrum.c) carries the speaker's channels */
static void *stream_pad;
static void *candidate_pad;            /* the pad selected by the poll, before vibration opens */
static int64_t retry_at;
static double test_phase;
static volatile LONG gate;             /* 1 while the poll or the shutdown holds the endpoint */
static volatile LONG closing;          /* the game is closing: nothing opens again */
static SRWLOCK ring_lock = SRWLOCK_INIT; /* route changes wait for both the writer and the pull */

static int env_int(const char *name, int otherwise, int low, int high) {
    const char *v = getenv(name);
    if (!v || !*v) return otherwise;
    const int n = atoi(v);
    return n < low ? low : n > high ? high : n;
}

/* ---- the classes' modes ---- */
static const char *const class_env[AUDIO_SOURCE_CLASSES] = {NULL, "HOS_PAD_SPEAKER_NAVI_VOICE", "HOS_PAD_SPEAKER_NAVI_SOUND",
    "HOS_PAD_SPEAKER_TARGET", "HOS_PAD_SPEAKER_OCARINA", "HOS_PAD_SPEAKER_PICKUP", "HOS_PAD_SPEAKER_ITEM", "HOS_PAD_SPEAKER_FAIRY",
    "HOS_PAD_SPEAKER_UI", "HOS_PAD_SPEAKER_AGONY", "HOS_PAD_SPEAKER_LOW_HEALTH", "HOS_PAD_SPEAKER_FISHING", "HOS_PAD_SPEAKER_BOTTLE"};
/* without a choice: Navi, targeting, pick-ups, the interface and the Stone of Agony from the pad alone (they are
   "near" sounds), and the low-health alarm, fishing and bottles too (in Link's hands); her flight, the ocarina, items
   and fairies from both (they belong to the world too) */
static const signed char class_default[AUDIO_SOURCE_CLASSES] = {0, HOS_PAD_SPEAKER_ONLY, HOS_PAD_SPEAKER_BOTH, HOS_PAD_SPEAKER_ONLY,
    HOS_PAD_SPEAKER_BOTH, HOS_PAD_SPEAKER_ONLY, HOS_PAD_SPEAKER_BOTH, HOS_PAD_SPEAKER_BOTH, HOS_PAD_SPEAKER_ONLY, HOS_PAD_SPEAKER_ONLY,
    HOS_PAD_SPEAKER_ONLY, HOS_PAD_SPEAKER_ONLY, HOS_PAD_SPEAKER_ONLY};

/* The player's choices: the menu's Controller speaker page (rx_option; the rows' defaults until the renderer is up), a
   switch over each for the run. Read at the pads' poll, once a second. */
_Static_assert(AUDIO_SOURCE_CLASSES - 1 == HOS_PAD_SPEAKER_CLASS_OPTIONS, "a menu row for each class");
static volatile LONG modes;             /* two bits per class, published together */
static volatile LONG route_state;       /* 0 off, 1 classed sounds, 2 all sound */
static LONG atomic_read(volatile LONG *p) { return InterlockedCompareExchange(p, 0, 0); }
static volatile LONG volume = 100, any_class;
static volatile LONG target = HOS_PAD_SPEAKER_RATE * 20 / 1000;   /* (the ring's, in samples: see "the ring") */
static volatile LONG tv_delay;                                     /* (the Controller sync below 0, in samples) */
static volatile LONG hp_mode = HOS_PAD_HEADPHONES_ALL, hp_volume = 80;   /* (the menu's Headphones rows) */
static volatile LONG hp_plugged;                                   /* (what the pad says, or the switch) */
static volatile LONG route_hp;                                     /* 1 while the pad's sound goes to its headphones */
static volatile LONG route_all;                                    /* 1 while all the game's sound goes there */
static volatile LONG ocarina_ms, ocarina_seen;                     /* (the mixer's: an ocarina sound played on the pad, when) */

/* HOS_PAD_SPEAKER_LOW_LATENCY=0: the ring as it was before, for a run: 40 ms, SDL's own 1024 frames a pull, no
   following of the pad's clock (a stall still cut back) */
static int low_latency(void) {
    static int on = -1;
    if (on < 0) on = env_int("HOS_PAD_SPEAKER_LOW_LATENCY", 1, 0, 1);
    return on;
}
int hos_pad_speaker_frames(void) { return low_latency() ? 256 : 0; }

static void settings(void) {
    int rx_option(int id, int otherwise);   /* host/rx_backend.c */
    int any = 0;
    LONG packed = 0;
    for (int i = 1; i < AUDIO_SOURCE_CLASSES; i++) {
        const int menu = rx_option(HOS_OPT_PAD_SPEAKER_OF(i - 1), class_default[i]);
        const int value = env_int(class_env[i], menu < 0 ? 0 : menu > 2 ? 2 : menu, 0, 2);
        packed |= (LONG)value << (2 * (i - 1));
        any |= value != HOS_PAD_SPEAKER_OFF;
    }
    const int v = rx_option(HOS_OPT_PAD_SPEAKER_VOLUME, 100);
    InterlockedExchange(&volume, env_int("HOS_PAD_SPEAKER_VOLUME", v < 0 ? 0 : v > 200 ? 200 : v, 0, 200));
    int sync = rx_option(HOS_OPT_PAD_SPEAKER_SYNC, 0);
    sync = sync < -100 ? -100 : sync > 150 ? 150 : sync;   /* (below 0: the TV waits, above: the pad) */
    const int ms = env_int("HOS_PAD_SPEAKER_BUFFER_MS", low_latency() ? 20 : 40, 5, 100) + (sync > 0 ? sync : 0);
    InterlockedExchange(&target, HOS_PAD_SPEAKER_RATE / 1000 * ms);
    InterlockedExchange(&tv_delay, sync < 0 ? HOS_PAD_SPEAKER_RATE / 1000 * -sync : 0);
    const int hm = rx_option(HOS_OPT_PAD_SPEAKER_HEADPHONES, HOS_PAD_HEADPHONES_ALL);
    InterlockedExchange(&hp_mode, env_int("HOS_PAD_SPEAKER_HEADPHONE_MODE", hm < 0 ? 0 : hm > 2 ? 2 : hm, 0, 2));
    const int hv = rx_option(HOS_OPT_PAD_SPEAKER_HEADPHONE_VOLUME, 80);
    InterlockedExchange(&hp_volume, hv < 0 ? 0 : hv > 100 ? 100 : hv);
    InterlockedExchange(&any_class, any);
    InterlockedExchange(&modes, packed);
}

int hos_pad_speaker_mode(int cls) {
    if (cls <= AUDIO_SOURCE_NONE || cls >= AUDIO_SOURCE_CLASSES) return HOS_PAD_SPEAKER_OFF;
    return (atomic_read(&modes) >> (2 * (cls - 1))) & 3;
}

/* what the pad's first two channels play now: the speaker, the headphones with the classed sounds, or the headphones
   with all the game's sound */
static int hp_route(void) { return atomic_read(&hp_plugged) && atomic_read(&hp_mode) != HOS_PAD_HEADPHONES_OFF; }
static int all_route(void) { return atomic_read(&hp_plugged) && atomic_read(&hp_mode) == HOS_PAD_HEADPHONES_ALL; }

static int wanted(void) {   /* 0 never; unset: while a class plays there (or headphones take it all); 1: with every class Off too (testing) */
    static int on = -2;
    if (on == -2) on = env_int("HOS_PAD_SPEAKER", -1, 0, 1);
    return on < 0 ? (int)atomic_read(&any_class) || all_route() : on;
}

int hos_pad_speaker_share_frames(void *pad, int token) {
    return !atomic_read(&closing) && wanted() && pad && pad == candidate_pad && token >= 0 &&
                   hos_sdl_pad_audio_endpoint(pad) == token ? hos_pad_speaker_frames() : 0;
}

/* ---- how loud, and clean ----
   The pad's small speaker is quiet beside a TV, and the game's sounds leave the mixer well under full scale: the
   menu's 100% is twice the mixer's level and its 200% four times, on top of the pad's own volume at its top and its
   preamp. Two things keep that from crackling (a bomb, an ocarina note at 200%):
   - the bass goes (a high-pass at 250 Hz, two poles): the speaker cannot play it, it only fills the amplifier;
   - a limiter: a peak over 0.89 of full scale turns the whole sound down for a moment (it is seen 1.3 ms ahead, the
     gain falls in 0.5 ms and comes back over 100 ms), the waveform itself is never bent. A hard stop at full scale
     is left only for what still gets past. */
#define SPEAKER_BOOST 2.0f
#define HPF_HZ 250.0
#define LIMIT 0.89f
enum { LOOK = 64 };   /* (samples of look-ahead: 1.3 ms) */
static float speaker_scale(void) { return (float)atomic_read(&volume) / 100.0f * SPEAKER_BOOST; }

typedef struct {   /* the mixer thread's: hos_pad_speaker_write */
    int hpf, limiter;   /* (HOS_PAD_SPEAKER_HPF=0, HOS_PAD_SPEAKER_LIMITER=0: each back to how it was before, for a run) */
    float b0, b1, b2, a1, a2, x1, x2, y1, y2;
    float env, gain, rel, att;
    float look[LOOK];
    int at;
} Shape;
static Shape shape;
static void shape_reset(void) {
    const double w0 = 2.0 * 3.14159265358979323846 * HPF_HZ / HOS_PAD_SPEAKER_RATE, c = cos(w0);
    const double alpha = sin(w0) / (2.0 * 0.70710678118654752), a0 = 1.0 + alpha;
    memset(&shape, 0, sizeof shape);
    shape.b0 = (float)((1.0 + c) / 2.0 / a0), shape.b1 = (float)(-(1.0 + c) / a0), shape.b2 = shape.b0;
    shape.a1 = (float)(-2.0 * c / a0), shape.a2 = (float)((1.0 - alpha) / a0);
    shape.gain = 1.0f;
    shape.hpf = env_int("HOS_PAD_SPEAKER_HPF", 1, 0, 1), shape.limiter = env_int("HOS_PAD_SPEAKER_LIMITER", 1, 0, 1);
    shape.rel = (float)exp(-1.0 / (0.100 * HOS_PAD_SPEAKER_RATE));
    shape.att = (float)(1.0 - exp(-1.0 / (0.0005 * HOS_PAD_SPEAKER_RATE)));
}
static float shape_sample(float x) {
    Shape *s = &shape;
    float y = x;
    if (s->hpf && !atomic_read(&route_hp)) {   /* (headphones play the bass: only the small speaker loses it) */
        y = s->b0 * x + s->b1 * s->x1 + s->b2 * s->x2 - s->a1 * s->y1 - s->a2 * s->y2;
        s->x2 = s->x1, s->x1 = x, s->y2 = s->y1, s->y1 = y;
    }
    if (!s->limiter) {   /* (the earlier way: bent past 0.7 of full scale, sample by sample) */
        const float a = fabsf(y);
        if (y != y) return 0;
        if (a <= 0.7f) return y;
        const float b = 0.7f + 0.3f * tanhf((a - 0.7f) / 0.3f);
        return y < 0 ? -b : b;
    }
    const float a = fabsf(y);
    s->env = a > s->env ? a : s->env * s->rel;   /* (the loudest lately, falling slowly) */
    const float want = s->env > LIMIT ? LIMIT / s->env : 1.0f;
    s->gain = want < s->gain ? s->gain + (want - s->gain) * s->att : want;
    float out = s->look[s->at] * s->gain;
    s->look[s->at] = y, s->at = (s->at + 1) % LOOK;
    if (out != out) out = 0;   /* (NaN) */
    return out > 1.0f ? 1.0f : out < -1.0f ? -1.0f : out;
}

/* ---- the ring ----
   The mixer writes every 5 ms on the PC's clock, the pad reads on its own. The pull keeps the ring near its target
   (HOS_PAD_SPEAKER_BUFFER_MS, 20 ms by default, plus the menu's Controller sync) by reading a hair faster or slower,
   at most 0.4% (a pitch no one hears), instead of dropping samples; only a ring past target + 60 ms (a stall) is cut
   back at once. Under-run plays silence and starts again from the target. */
static double read_frac, fill_avg = -1.0;
static double fill_sum;
static LONG fill_n;
static uint32_t ring_fill(void) { return (uint32_t)ring_w - (uint32_t)ring_r; }

void hos_pad_speaker_write(const float *mono, int frames) {
    AcquireSRWLockExclusive(&ring_lock);
    if (!atomic_read(&stream_on) || atomic_read(&route_all)) { ReleaseSRWLockExclusive(&ring_lock); return; }
    uint32_t w = (uint32_t)ring_w;
    if (w - (uint32_t)ring_r + (uint32_t)frames > RING) { ReleaseSRWLockExclusive(&ring_lock); return; }
    const float scale = atomic_read(&route_hp) ? 1.0f : speaker_scale();   /* (headphones: the mixer's level, the pad's own volume sets it) */
    for (int i = 0; i < frames; i++) {
        float *f = ring[(w + (uint32_t)i) & (RING - 1)];
        f[0] = f[1] = shape_sample(mono[i] * scale);
    }
    InterlockedExchange((volatile LONG *)&ring_w, (LONG)(w + (uint32_t)frames));
    ReleaseSRWLockExclusive(&ring_lock);
}

/* The whole game's mix for the headphones (the output thread, after the player's volume): its two sides, or a 5.1 mix
   folded down as nn_audio.c folds it for stereo speakers. */
void hos_pad_speaker_write_stereo(const int16_t *pcm, int frames, int channels) {
    AcquireSRWLockExclusive(&ring_lock);
    if (!atomic_read(&stream_on) || !atomic_read(&route_all)) { ReleaseSRWLockExclusive(&ring_lock); return; }
    uint32_t w = (uint32_t)ring_w;
    if (w - (uint32_t)ring_r + (uint32_t)frames > RING) { ReleaseSRWLockExclusive(&ring_lock); return; }
    for (int i = 0; i < frames; i++) {
        const int16_t *p = pcm + (size_t)i * channels;
        float l = p[0] / 32768.0f, r = channels > 1 ? p[1] / 32768.0f : l;
        if (channels >= 6) {   /* FL FR FC LFE RL RR */
            const float c = p[2] / 32768.0f * 0.7071f, lfe = p[3] / 32768.0f * 0.5f;
            l += c + lfe + p[4] / 32768.0f * 0.7071f, r += c + lfe + p[5] / 32768.0f * 0.7071f;
        }
        float *f = ring[(w + (uint32_t)i) & (RING - 1)];
        f[0] = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
        f[1] = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
    }
    InterlockedExchange((volatile LONG *)&ring_w, (LONG)(w + (uint32_t)frames));
    ReleaseSRWLockExclusive(&ring_lock);
}

/* A stream's thread: frames x 4 floats, the game's sounds added to the speaker's two channels, never over them (on the
   vibration's stream the second carries HD Haptics' taps; the voice coils' two are left as they are) */
static float add_clamped(float a, float b) {
    const float v = a + b;
    return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}
static void speaker_fill(float *out, int frames) {
    const uint32_t goal = (uint32_t)atomic_read(&target), w = (uint32_t)ring_w;
    uint32_t r = (uint32_t)ring_r, fill = w - r;
    if (!primed) {
        if (fill >= goal) InterlockedExchange(&primed, 1), read_frac = 0, fill_avg = fill;
    } else if (fill > goal + HOS_PAD_SPEAKER_RATE * 60 / 1000) {   /* a stall: back to the target at once */
        r = w - goal, fill = goal, fill_avg = goal, read_frac = 0;
        InterlockedExchange((volatile LONG *)&ring_r, (LONG)r);
        InterlockedIncrement(&trims);
    }
    if (primed) {
        fill_avg = fill_avg < 0 ? fill : fill_avg + (fill - fill_avg) * 0.05;
        double step = 1.0 + (fill_avg - goal) * (0.004 / (HOS_PAD_SPEAKER_RATE * 10 / 1000));
        step = step < 0.996 ? 0.996 : step > 1.004 ? 1.004 : step;
        if (!low_latency()) step = 1.0;
        double pos = read_frac;
        int i = 0;
        for (; i < frames && w - r >= 2; i++) {
            const float *a = ring[(unsigned)r & (RING - 1)], *b = ring[(unsigned)(r + 1) & (RING - 1)];
            out[i * 4] = add_clamped(out[i * 4], a[0] + (b[0] - a[0]) * (float)pos);
            out[i * 4 + 1] = add_clamped(out[i * 4 + 1], a[1] + (b[1] - a[1]) * (float)pos);
            pos += step;
            while (pos >= 1.0) pos -= 1.0, r++;
        }
        read_frac = pos;
        InterlockedExchange((volatile LONG *)&ring_r, (LONG)r);
        fill_sum += fill, fill_n++;
        if (i < frames) {   /* ran dry: silence, and a fresh start once the mixer is ahead again */
            InterlockedExchange(&primed, 0);
            if (i > 0 || fill > 0) InterlockedIncrement(&underruns);
        }
    }
    LONG left = atomic_read(&test_left);
    if (left > 0) {   /* the test: 880 Hz with a short fade at both ends, over whatever plays */
        const int n = left < frames ? (int)left : frames;
        for (int i = 0; i < n; i++) {
            const LONG at = TEST_FRAMES - left + i;
            const float fade = at < 480 ? at / 480.0f : left - i < 480 ? (left - i) / 480.0f : 1.0f;
            const float t = 0.2f * (atomic_read(&route_hp) ? 1.0f : speaker_scale()) * fade * (float)sin(test_phase);
            test_phase += 2.0 * 3.14159265358979323846 * 880.0 / HOS_PAD_SPEAKER_RATE;
            for (int c = 0; c < 2; c++) out[i * 4 + c] = add_clamped(out[i * 4 + c], t);
        }
        InterlockedExchangeAdd(&test_left, -n);
    }
}
/* our own stream's pull: the voice coils' two silent, the speaker's ours alone */
static void speaker_pull(void *user, uint8_t *bytes, int len) {
    (void)user;
    float *out = (float *)bytes;
    const int frames = len / (int)(4 * sizeof(float));
    memset(out, 0, (size_t)frames * 4 * sizeof(float));
    AcquireSRWLockExclusive(&ring_lock);
    if (atomic_read(&stream_on)) speaker_fill(out, frames);
    ReleaseSRWLockExclusive(&ring_lock);
}
/* the vibration's stream (host/rumble_renhdrum.c usb_pull), after it wrote its four channels */
void hos_pad_speaker_fill(float *out, int frames) {
    AcquireSRWLockExclusive(&ring_lock);
    if (atomic_read(&stream_on) && atomic_read(&riding)) speaker_fill(out, frames);
    ReleaseSRWLockExclusive(&ring_lock);
}

/* ---- the pad's output path: its "effects" block, as SDL's PS5 driver sends it (47 bytes) ---- */
enum { DS_ENABLE1 = 0, DS_ENABLE2 = 1, DS_HEADPHONE_VOLUME = 4, DS_SPEAKER_VOLUME = 5, DS_AUDIO_FLAGS = 7, DS_AUDIO_FLAGS2 = 37,
       DS_EFFECTS = 47 };
enum { DS_SPEAKER_FULL = 100, DS_HEADPHONE_FULL = 0x7f };
enum { DS_E1_HEADPHONE_VOLUME = 0x10, DS_E1_SPEAKER_VOLUME = 0x20, DS_E1_AUDIO_CONTROL = 0x80, DS_E2_AUDIO_CONTROL2 = 0x80 };
enum { TO_JACK, TO_SPEAKER, TO_HEADPHONES };   /* (the jack given back as it was; the speaker; the headphones, by us) */

static int stream_to;   /* (TO_SPEAKER or TO_HEADPHONES: what the pad was told while it plays) */

/* the pad told where its first two channels go. On the vibration's stream the speaker is RenHDrum's to open and give back
   (host/rumble_renhdrum.c rh_usb_speaker: one speaker block for HD Haptics and these sounds, the preamp's gain ours while
   they play there); only the headphones are told here. On our own stream, this report does it all. */
static int pad_route_own(void *pad, int to);
static int pad_route(void *pad, int to) {
    if (!atomic_read(&riding)) {
        return pad_route_own(pad, to);
    }
    if (to == TO_JACK && stream_to == TO_HEADPHONES) pad_route_own(pad, TO_JACK);   /* (the headphones' route undone) */
    int sent = rh_usb_speaker(to == TO_SPEAKER ? RH_SPEAKER_GAME : to == TO_HEADPHONES ? RH_SPEAKER_JACK : RH_SPEAKER_NONE,
                   env_int("HOS_PAD_SPEAKER_GAIN", 4, 0, 7));
    if (to == TO_HEADPHONES) sent = pad_route_own(pad, TO_HEADPHONES) && sent;
    else {
        InterlockedExchange(&route_hp, 0);
        hos_log("pad speaker: the pad's sound %s (RenHDrum's speaker block%s)", to == TO_SPEAKER ? "to its speaker" : "off its speaker",
                to == TO_SPEAKER ? ", our gain" : "");
    }
    return sent;
}
static int pad_route_own(void *pad, int to) {
    uint8_t e[DS_EFFECTS] = {0};
    e[DS_ENABLE1] = DS_E1_AUDIO_CONTROL;
    e[DS_AUDIO_FLAGS] = (uint8_t)((to == TO_SPEAKER ? env_int("HOS_PAD_SPEAKER_PATH", 3, 0, 3) : 0) << 4);
    if (to == TO_SPEAKER) {
        e[DS_ENABLE1] |= DS_E1_SPEAKER_VOLUME;
        e[DS_SPEAKER_VOLUME] = DS_SPEAKER_FULL;   /* (the pad at its loudest: the volume is ours, speaker_scale) */
        e[DS_ENABLE2] = DS_E2_AUDIO_CONTROL2;
        e[DS_AUDIO_FLAGS2] = (uint8_t)env_int("HOS_PAD_SPEAKER_GAIN", 4, 0, 7);
    } else if (to == TO_HEADPHONES) {   /* (path 0: both sides to the jack; their volume, the menu's) */
        e[DS_ENABLE1] |= DS_E1_HEADPHONE_VOLUME;
        e[DS_HEADPHONE_VOLUME] = (uint8_t)(DS_HEADPHONE_FULL * atomic_read(&hp_volume) / 100);
    }
    const int sent = hos_sdl_pad_effect(pad, e, sizeof e);
    if (sent) InterlockedExchange(&route_hp, to == TO_HEADPHONES);
    if (to == TO_HEADPHONES)
        hos_log("pad speaker: the %s to its headphones (path 0, headphone volume %d)%s",
                atomic_read(&route_all) ? "game's sound" : "pad's sound", e[DS_HEADPHONE_VOLUME], sent ? "" : ": the pad did not take the report");
    else
        hos_log("pad speaker: the pad's sound %s (path %d, volume %d, gain %d)%s", to == TO_SPEAKER ? "to its speaker" : "back to its jack",
                e[DS_AUDIO_FLAGS] >> 4, e[DS_SPEAKER_VOLUME], e[DS_AUDIO_FLAGS2], sent ? "" : ": the pad did not take the report");
    return sent;
}

/* ---- the endpoint (the pads' poll thread) ----
   A cabled DualSense's four-channel sound card has one stream: the vibration's (host/rumble_renhdrum.c: its voice coils on
   the third and fourth channels) when it plays the pad, and the speaker's first two ride on it (hos_pad_speaker_fill);
   else our own, opened here, its last two silent. When the vibration opens its stream ours goes, and when it lets its go
   ours opens again, within a second. */
/* (the vibration finds its endpoint by name, as before: the speaker shares it only once it is matched to this pad itself,
   checked as the sharing starts and again at each reopen, rh_usb_restart_audio. Where nothing can match a pad to its
   endpoint (Linux for now) the speaker never shares it, and the vibration plays as before) */
static int vibration_stream(void *pad) {
    if (!pad || rh_usb_owner() != pad || !rh_usb_alive()) return 0;
    if (atomic_read(&riding)) return 1;
    const int token = rh_usb_token();
    return token >= 0 && hos_sdl_pad_audio_endpoint(pad) == token;
}

static void close_endpoint(const char *why) {
    if (!stream && !atomic_read(&riding)) return;
    InterlockedExchange(&stream_on, 0);
    InterlockedExchange(&route_state, 0);
    InterlockedExchange(&route_all, 0), InterlockedExchange(&route_hp, 0);
    if (stream) hos_sdl_audio_close(stream);   /* (waits for the pull) */
    stream = NULL;
    InterlockedExchange(&riding, 0);
    AcquireSRWLockExclusive(&ring_lock);
    hos_log("pad speaker: closed (%s); %ld under-runs, %ld trims, %.1f ms buffered on average (target %.1f ms)", why,
            (long)atomic_read(&underruns), (long)atomic_read(&trims), fill_n ? fill_sum / fill_n * 1000.0 / HOS_PAD_SPEAKER_RATE : 0.0,
            atomic_read(&target) * 1000.0 / HOS_PAD_SPEAKER_RATE);
    fill_sum = 0, fill_n = 0;
    ReleaseSRWLockExclusive(&ring_lock);
}

static void speaker_pad(void *pad, int wired);

void hos_pad_speaker_pad(void *pad, int wired) {
    if (atomic_read(&closing) || InterlockedCompareExchange(&gate, 1, 0)) return;   /* (the shutdown has it: next second) */
    if (!atomic_read(&closing)) speaker_pad(pad, wired);
    InterlockedExchange(&gate, 0);
}

void hos_pad_speaker_forget(void *pad) {
    while (InterlockedCompareExchange(&gate, 1, 0)) Sleep(1);
    if (pad && pad == stream_pad) {
        close_endpoint("the pad disconnected");
        stream_pad = NULL;
    }
    InterlockedExchange(&gate, 0);
}

/* Headphones in the jack: what the pad says (-1: it does not say), or the switch over it, each second */
static void headphones(void *pad) {
    int plugged = env_int("HOS_PAD_SPEAKER_HEADPHONES", -1, 0, 1);
    if (plugged < 0) plugged = pad ? hos_sdl_pad_headphones(pad) : 0;
    if (plugged < 0) plugged = 0;
    if (plugged != atomic_read(&hp_plugged)) hos_log("pad speaker: headphones %s the pad's jack", plugged ? "in" : "out of");
    InterlockedExchange(&hp_plugged, plugged);
}

static void start(void *pad, int to, void *own, const char *how) {
    AcquireSRWLockExclusive(&ring_lock);
    InterlockedExchange((volatile LONG *)&ring_r, ring_w);
    InterlockedExchange(&primed, 0);
    shape_reset();   /* (the mixer writes nothing while nothing plays) */
    ReleaseSRWLockExclusive(&ring_lock);
    stream = own, stream_pad = pad;
    InterlockedExchange(&riding, own == NULL);
    InterlockedExchange(&route_all, all_route());
    if (!pad_route(pad, to)) { close_endpoint("output report rejected"); return; }
    stream_to = to;
    InterlockedExchange(&stream_on, 1);
    InterlockedExchange(&route_state, all_route() ? 2 : 1);
    hos_log("pad speaker: playing on the DualSense's endpoint (%s)", how);
    if (env_int("HOS_PAD_SPEAKER_TEST", 0, 0, 1)) hos_pad_speaker_test();
}

static void speaker_pad(void *pad, int wired) {
    candidate_pad = wired != 0 ? pad : NULL;
    const LONG volume_was = atomic_read(&volume), hp_volume_was = atomic_read(&hp_volume);
    settings();
    headphones(wired == 0 ? NULL : pad);
    const int to = hp_route() ? TO_HEADPHONES : TO_SPEAKER;
    const int playing = stream || atomic_read(&riding);
    if (playing && pad && pad == stream_pad && wanted() &&
        (to != stream_to || all_route() != atomic_read(&route_all) || (to == TO_HEADPHONES && atomic_read(&hp_volume) != hp_volume_was))) {
        AcquireSRWLockExclusive(&ring_lock);
        InterlockedExchange(&stream_on, 0);
        InterlockedExchange(&route_state, 0);
        InterlockedExchange((volatile LONG *)&ring_r, ring_w);
        InterlockedExchange(&primed, 0);
        shape_reset();
        InterlockedExchange(&route_all, all_route());   /* (plugged, pulled, or the menu's rows moved: the pad told again) */
        ReleaseSRWLockExclusive(&ring_lock);
        /* Closing the SDL stream also discards samples already pulled from the ring. */
        if (atomic_read(&riding)) {
            if (!rh_usb_restart_audio()) {
                pad_route(pad, to);
                close_endpoint("the vibration stream would not reopen");
                return;
            }
        } else {
            hos_sdl_audio_close(stream);
            stream = NULL;
            const int device = hos_sdl_pad_audio_endpoint(pad);
            if (device >= 0)
                stream = hos_sdl_audio_open((unsigned)device, HOS_PAD_SPEAKER_RATE, 4, 0, hos_pad_speaker_frames(), speaker_pull, NULL);
            if (!stream) { pad_route(pad, to); return; }
        }
        if (!pad_route(pad, to)) { close_endpoint("output report rejected"); return; }
        stream_to = to;
        InterlockedExchange(&stream_on, 1);
        InterlockedExchange(&route_state, all_route() ? 2 : 1);
    } else if (playing && atomic_read(&volume) != volume_was && to == TO_SPEAKER) {
        hos_pad_speaker_test();   /* the menu's volume moved: a tone at the new one */
    }
    if (!wanted() || !pad || wired == 0) {
        if (playing) {
            if (pad && pad == stream_pad) pad_route(pad, TO_JACK);
            close_endpoint(!pad ? "the pad is gone" : wired == 0 ? "the pad is not on a cable" : "no sound is sent there");
        }
        stream_pad = NULL;
        return;
    }
    if (playing && pad != stream_pad) close_endpoint("another DualSense");
    if (stream && !hos_sdl_audio_alive(stream)) close_endpoint("the endpoint went away");
    const int ride = vibration_stream(pad);
    if (atomic_read(&riding) && !ride) {   /* the vibration let its stream go: ours from here */
        InterlockedExchange(&stream_on, 0);
        InterlockedExchange(&route_state, 0);
        InterlockedExchange(&riding, 0);
        hos_log("pad speaker: the vibration's stream is gone: our own");
    }
    if (stream && ride) {   /* the vibration opened its stream: ours goes, its carries the sound */
        InterlockedExchange(&stream_on, 0);
        InterlockedExchange(&route_state, 0);
        void *own = stream;
        stream = NULL;
        hos_sdl_audio_close(own);
        start(pad, to, NULL, "with the vibration's stream");
        return;
    }
    if (stream || atomic_read(&riding)) return;
    if (ride) {
        start(pad, to, NULL, "with the vibration's stream");
        return;
    }
    const int64_t now = hos_now_ns();
    if (now < retry_at) return;
    retry_at = now + 2LL * RETRY_NS_PER_S;
    const int device = hos_sdl_pad_audio_endpoint(pad);
    static int said;
    if (device < 0) {
        if (!said++) hos_log("pad speaker: no matching four-channel audio endpoint for the DualSense");
        return;
    }
    /* 256 frames lets the ring be short; the SDL opener respects an explicit override. */
    void *s = hos_sdl_audio_open(device, HOS_PAD_SPEAKER_RATE, 4, 0, hos_pad_speaker_frames(), speaker_pull, NULL);
    if (!s) {
        if (!said++) hos_log("pad speaker: the DualSense's audio endpoint would not open");
        return;
    }
    said = 0;
    start(pad, to, s, "its own stream");
}

int hos_pad_speaker_active(void) { return atomic_read(&route_state) == 1; }
int hos_pad_speaker_headphones_all(void) { return atomic_read(&route_state) == 2; }
int hos_pad_speaker_tv_delay(void) { return atomic_read(&route_state) == 1 ? (int)atomic_read(&tv_delay) : 0; }

void hos_pad_speaker_ocarina_seen(void) {
    InterlockedExchange(&ocarina_ms, (LONG)(hos_now_ns() / 1000000));
    InterlockedExchange(&ocarina_seen, 1);
}
int hos_pad_speaker_rumble_percent(void) {   /* (the pads' poll: while an ocarina sound played on the pad in the last 200 ms) */
    static int percent = -1;
    if (percent < 0) percent = env_int("HOS_PAD_SPEAKER_OCARINA_RUMBLE", 50, 0, 100);
    const uint32_t since = (uint32_t)(hos_now_ns() / 1000000) - (uint32_t)atomic_read(&ocarina_ms);
    return atomic_read(&route_state) == 1 && !atomic_read(&route_hp) && atomic_read(&ocarina_seen) && since < 200 ? percent : 100;
}
int hos_pad_speaker_pad_rumble_percent(void *pad) {
    if (InterlockedCompareExchange(&gate, 1, 0)) return 100;
    const int percent = pad && pad == stream_pad ? hos_pad_speaker_rumble_percent() : 100;
    InterlockedExchange(&gate, 0);
    return percent;
}

int hos_pad_speaker_test(void) {
    if (!atomic_read(&stream_on)) return 0;
    InterlockedExchange(&test_left, TEST_FRAMES);
    return 1;
}

void hos_pad_speaker_shutdown(void) {
    InterlockedExchange(&closing, 1);
    while (InterlockedCompareExchange(&gate, 1, 0)) Sleep(1);   /* the pad must stay open until the poll finishes */
    if ((stream || atomic_read(&riding)) && stream_pad) pad_route(stream_pad, TO_JACK);
    close_endpoint("the game is closing");
    stream_pad = NULL;
    InterlockedExchange(&gate, 0);
}
