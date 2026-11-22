/* (longshot) RenHDrum's ways of playing the game's vibration (third_party/renhdrum, MIT OR Apache-2.0), for the pads
   host/vibration.h gives them: a DualSense's voice coils over its cable (its own four-channel sound card) or over
   Bluetooth (HID report 0x36, or 0x32 with HD Haptics off), with its HD Haptics (its speaker and its L2/R2 triggers
   from the same vibration), an Xbox One or Series pad's four motors, a Switch pad's HD rumble packets, a Steam
   Controller (2026)'s grips (HID report 0x88).

   RenHDrum is loaded at run time from beside the game (renhdrum.dll, renhdrum.so on Linux: scripts/renhdrum.py builds
   them, the Builder carries them); without it every pad keeps what it had, and rh_load says why.

   Threads: host/pad_sdl.c's (the pads' poll) opens, closes and ticks everything here; the game's (host/nn_hid.c)
   pushes the values; the sound card's callback and the Bluetooth sender read the DSPs. One lock holds them all, never
   across a call that waits for one of those threads (a stream's close, the sender's join). */
#pragma once
#include <stdint.h>

int rh_load(void);                 /* 1 when RenHDrum is loaded (tried once) */
const char *rh_why(void);          /* why it is not, when it is not */

void rh_push(int side, const float value[4]);    /* (the game's thread) {amp_low, freq_low, amp_high, freq_high} */
/* each poll: a test's values, a hand gone quiet, the strength, a DualSense's HD Haptics (1 on: the Controls page's) */
void rh_tick(int64_t now, int testing, int percent, int usb_percent, int hd_haptics);
/* each poll, for the DualSense whose voice coils play (cable or Bluetooth): its triggers' pull (0 to 255) and whether
   another button is held, for HD Haptics' trigger kick-back. Sent from the pad thread only */
void rh_ds_triggers(void *pad, int64_t now, int left_depth, int right_depth, int other_held);

/* A DualSense on a cable: its four-channel sound card, found by its name (as before the controller speaker, on every
   system), opened for one pad */
int rh_usb_find(void *pad);        /* endpoint token, or -1 */
int rh_usb_open(void *pad, int token); /* 1 when it plays */
int rh_usb_alive(void);
int rh_usb_token(void);            /* the open stream's endpoint token, or -1 */
/* (longshot) discard the shared stream's queued audio: closed and opened again, only on the endpoint matched to the pad
   (hos_sdl_pad_audio_endpoint) at that moment; 0, the stream given up, when it no longer is */
int rh_usb_restart_audio(void);
void rh_usb_close(void);
void *rh_usb_owner(void);
/* (longshot) the controller speaker (host/pad_speaker.c) on the cable's stream: RH_SPEAKER_GAME, the game's sounds play on
   the speaker too, so it is opened (RenHDrum's speaker block, with HD Haptics off as well) at that preamp gain (0 to 7);
   RH_SPEAKER_JACK, they go to headphones in the jack, so the speaker is given back and HD Haptics' taps are silent;
   RH_SPEAKER_NONE, as without it. The pads' poll thread; forgotten when the stream closes. */
enum { RH_SPEAKER_NONE = 0, RH_SPEAKER_GAME = 1, RH_SPEAKER_JACK = 2 };
int rh_usb_speaker(int want, int gain);

/* A DualSense over Bluetooth: report 0x36 (or 0x32) every 10.7 ms through a second handle on its HID device */
int rh_bt_open(void *pad, const char *hid_path);
int rh_bt_alive(void);
void rh_bt_close(void);
void *rh_bt_owner(void);

/* A Steam Controller (2026)'s grips: report 0x88 through a second handle on its HID device (cable: 1 its 8 kHz format,
   0 Bluetooth's or its dongle's 4 kHz) */
int rh_steam_open(void *pad, const char *hid_path, int cable);
int rh_steam_alive(void);
void rh_steam_close(void);
void *rh_steam_owner(void);

/* An Xbox pad's four motors, a Switch pad's packets: what to send now, when there is something (1) */
int rh_pad_open(void *pad, int backend, int bluetooth, int pro);   /* backend: VIB_RH_XBOX or VIB_RH_SWITCH */
void rh_pad_close(void *pad);
int rh_xbox_poll(void *pad, int64_t now, uint16_t motors[4]);      /* left grip, right grip, left trigger, right */
int rh_switch_poll(void *pad, int64_t now, uint8_t packet[10]);

void rh_shutdown(void);            /* the game is over: everything closed, nothing opens again */
