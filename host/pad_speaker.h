/* (longshot) The controller's speaker (host/pad_speaker.c): a DualSense on a cable is a four-channel 48 kHz sound card
   as well as a pad, and its first two channels can play through the speaker in the pad. The game's mixer hands the
   sounds of the classes the player chose (host/audio_source.h: Navi's voice, targeting, the ocarina, ...) to
   hos_pad_speaker_write, and they come out of the pad instead of, or as well as, the game's output.

   No pad on a cable, no endpoint, an SDL that cannot open one (SDL2): hos_pad_speaker_active is 0 and every sound stays
   on the game's output, as without this. */
#pragma once
#include <stdint.h>

enum { HOS_PAD_SPEAKER_RATE = 48000 };

/* what a class of sounds does (the menu's rows; HOS_PAD_SPEAKER_<CLASS> over them) */
enum { HOS_PAD_SPEAKER_OFF = 0, HOS_PAD_SPEAKER_ONLY = 1, HOS_PAD_SPEAKER_BOTH = 2 };

/* host/pad_sdl.c, once a second: the DualSense that is open now (NULL: none), and whether it is on a cable (1 wired,
   0 wireless, -1 when SDL cannot say: tried as wired). Opens, checks and closes the pad's endpoint as needed. */
void hos_pad_speaker_pad(void *pad, int wired);
void hos_pad_speaker_forget(void *pad);
int hos_pad_speaker_active(void);                          /* 1 while the classed sounds written here are heard from the pad */
int hos_pad_speaker_mode(int cls);                         /* HOS_PAD_SPEAKER_* for a class of audio_source.h (0 none: off) */
void hos_pad_speaker_write(const float *mono, int frames); /* the mixer thread: 48 kHz samples, -1 to 1 */
int hos_pad_speaker_test(void);                            /* a short tone from the pad; 1 when it started */
void hos_pad_speaker_shutdown(void);

/* Headphones in the pad's jack, as on the console (the menu's Headphones on the controller: Off, the controller's sounds,
   all sound): while all the game's sound goes to them, hos_pad_speaker_headphones_all is 1, hos_pad_speaker_active 0
   (no sound is classed: they all go there), and the output thread hands the whole mix to hos_pad_speaker_write_stereo
   and plays silence on the TV. */
enum { HOS_PAD_HEADPHONES_OFF = 0, HOS_PAD_HEADPHONES_SOUNDS = 1, HOS_PAD_HEADPHONES_ALL = 2 };
int hos_pad_speaker_headphones_all(void);
void hos_pad_speaker_write_stereo(const int16_t *pcm, int frames, int channels);   /* the device's mix, 2 or 6 channels */

/* The Controller sync below 0: the TV's copy of the sounds set to Controller + TV held back by this many samples, so that
   it plays with the pad (0: none). The mixer thread asks once a frame. */
int hos_pad_speaker_tv_delay(void);

/* The ocarina on the pad shakes its speaker when the pad vibrates too: the mixer says when an ocarina sound plays there,
   and the pads' poll scales the rumble by hos_pad_speaker_rumble_percent (100: as it is). */
void hos_pad_speaker_ocarina_seen(void);
int hos_pad_speaker_rumble_percent(void);

/* The vibration's stream on a cabled DualSense (host/rumble_renhdrum.c), once its voice coils' channels are written: the
   speaker's two written into it (frames x 4 floats), while it carries them */
void hos_pad_speaker_fill(float *out, int frames);
int hos_pad_speaker_frames(void); /* requested SDL buffer size, or 0 for the default */
int hos_pad_speaker_share_frames(void *pad, int token);
int hos_pad_speaker_pad_rumble_percent(void *pad);
