/* Authored memory, clock and API spies; input_actual.inc contains the unmodified production path. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#include "input_constants.inc"
#include "../host/hold_toggle.h"
#include "../host/input_movie.h"   /* (MoviePad: npad_states hands each read to the input recording, host/input_movie.c) */
static int toggle_flags, toggle_reset, sprint_reset_pending;
static int controls_case, mouse_mode, native_free = 1, mouse_pointing;
static int mouse_let_go, game_menu_mouse, side_binds;   /* (the game's menus and the mouse's side buttons: menu_side_case) */
static int held_keys[256], walk_mode, walk_speed;
static int player_stamps;   /* (host/nn_hid.c player_used: the input the game menus' cursor hides by; held_input_case) */
static uint64_t physical_buttons;
static int physical_deck_builtin;   /* (the Deck's built-in controls give gameplay input: its mouse must not) */
static int32_t physical_rstick;     /* (the right stick they give, which Steam's Desktop layout also drives the mouse with) */
static int InterlockedExchange(int *p, int v) { int old = *p; *p = v; return old; }
/* API spies must not collide with Windows SDK/import-library names. */
#define GetCurrentProcessId fixture_GetCurrentProcessId
#define InterlockedCompareExchange64 fixture_InterlockedCompareExchange64

typedef void *HWND;
typedef uint32_t DWORD;
typedef intptr_t LONG_PTR;
typedef struct { uint64_t x[32]; } Ctx;
enum { GWL_EXSTYLE = -20, WS_EX_TOOLWINDOW = 0x80, WS_EX_NOACTIVATE = 0x8000000,
       VK_MENU = 0x12, VK_CAPITAL = 0x14, VK_RETURN = 0x0d, VK_LEFT = 0x25, VK_UP, VK_RIGHT, VK_DOWN,
       VK_LBUTTON = 1, VK_RBUTTON = 2, VK_MBUTTON = 4, VK_XBUTTON1 = 5, VK_XBUTTON2 = 6 };
static int front, menu_background, menu_open, bridge_active, bootstrap_polls, pad_merges, key_queries, mouse_queries, focus_queries, commands;
static int focus_gain_after_query;
static uint64_t bridge_buttons;
static int bridge_up;
typedef int64_t LONG64;
volatile LONG64 hos_present_count = 1000;
static int frame_reads;
LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 value, LONG64 expected) {
    const LONG64 old = *p;
    frame_reads++;
    if (old == expected) *p = value;
    return old;
}
static int64_t now_ns = 10000000000LL, sampling;
typedef long LONG;   /* (the note layer's words: production gets it from windows.h) */
static int key_bind[HOS_KEY_N][2], pad_raw_now;   /* (the keys-only controls after the Switch's buttons) */
typedef int SRWLOCK;   /* (host/nn_hid.c guards the pad table with these since #1990) */
#define SRWLOCK_INIT 0
static void AcquireSRWLockShared(SRWLOCK *p) { (void)p; }
static void ReleaseSRWLockShared(SRWLOCK *p) { (void)p; }
static void AcquireSRWLockExclusive(SRWLOCK *p) { (void)p; }   /* (hos_hid_tap's) */
static void ReleaseSRWLockExclusive(SRWLOCK *p) { (void)p; }
static int pad_bind[HOS_KEY_N][2] = { [HOS_KEY_TOGGLE_HUD] = {-1, -1}, [HOS_KEY_SPRINT] = {-1, -1} };
static LONG hud_bind_generation;
static SRWLOCK pad_bind_lock = SRWLOCK_INIT;
static SRWLOCK input_bind_lock = SRWLOCK_INIT;
static _Thread_local int sprint_frame, sprint_toggle_frame, sprint_excluded, input_in_front, input_snapshot_active;
static _Thread_local int input_keys[HOS_KEY_N][2], input_pads[HOS_KEY_N][2];
static int sampled_key(int i, int k) { return input_snapshot_active ? input_keys[i][k] : key_bind[i][k]; }
static int sampled_pad(int i, int k) { return input_snapshot_active ? input_pads[i][k] : pad_bind[i][k]; }
static uint64_t pad_after_menu, pad_raw_game;   /* (pad_raw_now stays 0 here: no pad input waits after the menu) */
static uint64_t bound_pad_buttons(uint64_t raw) { (void)raw; CHECK(!"a pad input waited after the menu"); return 0; }
static LONG oc_consumed_keys[256];
static uint64_t oc_consumed_pads;
volatile uint64_t hos_oc_note_bits; volatile int hos_oc_wait; volatile int hos_oc_last_kind; volatile unsigned hos_oc_beat;
static unsigned char *memory;
static char last_log[256];
static int origin_logs, script_logs;
static long long logged_origin;
volatile int64_t hos_hid_last_read;
volatile uint64_t hos_hid_buttons;   /* (host/cheats.c reads it) */
volatile int hos_nvn_sync_log_on;
static int32_t bridge_stick[4];   /* (the bridge's sticks: the stick-directions case) */
int hos_control_input(uint64_t *buttons, int32_t stick[4]) {
    if (bridge_up) stick[1] = 32767;
    *buttons |= bridge_buttons;
    for (int i = 0; i < 4; i++)
        if (bridge_stick[i]) stick[i] = bridge_stick[i];
    return bridge_active;
}
/* (no recording and no playback here: every read is the player's, and each one is offered to the recording once) */
static int movie_begins, movie_ends;
int hos_input_movie_begin(MoviePad *given) { (void)given; movie_begins++; return 0; }
void hos_input_movie_end(const MoviePad *p) { (void)p; movie_ends++; CHECK(movie_ends == movie_begins); }
static void *at(uint64_t address) { CHECK(address <= 512 - 8); return memory + address; }
#define GP(a) at(a)
static void *hos_guest_range(uint64_t a, uint64_t n) { return a <= 512 && n <= 512 - a ? memory + a : NULL; }
#define G32(a) (*(uint32_t *)at(a))
#define G64(a) (*(uint64_t *)at(a))
#define ARG(n) (c->x[n])
#define RET(value) (c->x[0] = (uint64_t)(value))

int rx_option(int id, int otherwise) {
    return id == HOS_OPT_HOLD_TOGGLE ? toggle_flags : id == HOS_OPT_WALK_MODE ? walk_mode :
           id == HOS_OPT_WALK_SPEED ? walk_speed : id == 6 ? mouse_mode : id == 4 ? menu_background : id == 1 ? menu_open : otherwise;
}
int64_t hos_now_ns(void) { return now_ns; }
void hos_prompts_live(void) {}   /* (host/prompts.c: the button prompts follow the controls) */
volatile int hos_prompts_row = -1;   /* (host/prompts.c: "!prompts" in the live input) */
void hos_bind(int set, int button, int slot, int code) { (void)set, (void)button, (void)slot, (void)code; }   /* ("!bind") */
void hos_log(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(last_log, sizeof last_log, format, args); va_end(args);
    if (!strncmp(last_log, "hid: input script of ", 21)) {
        const char *stamp = strstr(last_log, "; origin_ns=");
        long long origin;
        CHECK(stamp && sscanf(stamp, "; origin_ns=%lld", &origin) == 1);
        CHECK(origin == now_ns);
        CHECK(++script_logs == 1);
    }
    if (!strncmp(last_log, "hid: input frame clock origin=", 29)) {
        origin_logs++;
        CHECK(sscanf(last_log, "hid: input frame clock origin=%lld native presents; rate=60", &logged_origin) == 1);
        CHECK(logged_origin == 1000);
        CHECK(!strcmp(last_log, "hid: input frame clock origin=1000 native presents; rate=60"));
    }
}
HWND GetForegroundWindow(void) {
    const HWND result = (HWND)(uintptr_t)(front ? 1 : 2);
    if (++focus_queries == focus_gain_after_query) front = 1;
    return result;
}
DWORD GetWindowThreadProcessId(HWND window, DWORD *pid) { *pid = window == (HWND)(uintptr_t)1 ? 10 : 20; return 1; }
DWORD GetCurrentProcessId(void) { return 10; }
LONG_PTR GetWindowLongPtrA(HWND window, int index) { (void)window; (void)index; return 0; }
int GetClassNameA(HWND window, char *text, int size) { (void)window; return snprintf(text, (size_t)size, "fixture"); }
short GetAsyncKeyState(int key) {
    key_queries++;
    return (controls_case ? held_keys[key & 255] : key == 'P' || key == 'D') ? (short)-32768 : 0;
}
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4], int *deck_builtin_input) {
    bootstrap_polls++; *buttons = 1ULL << B_Y; stick[0] = -31000;
    if (deck_builtin_input) *deck_builtin_input = 0;
    return 1;
}
void pads_read(uint64_t *buttons, int32_t stick[4], int *deck_builtin_input) {
    /* The physical backend boundary is a spy; actual routing merges its result with the script below. */
    if (deck_builtin_input) *deck_builtin_input = physical_deck_builtin;
    if (controls_case) { *buttons |= physical_buttons; stick[2] = physical_rstick; return; }
    pad_merges++; *buttons |= 1ULL << B_B; stick[0] = -12000; stick[2] = 8000;
}
void binds_load(void) {
    key_bind[B_X][0] = 'P';
    pad_bind[HOS_KEY_SPRINT][0] = pad_bind[HOS_KEY_SPRINT][1] = -1;
    /* (the left stick's and the camera's keys are bindings too: the defaults, WASD and the arrows) */
    key_bind[HOS_KEY_MOVE_UP][0] = 'W', key_bind[HOS_KEY_MOVE_DOWN][0] = 'S', key_bind[HOS_KEY_MOVE_LEFT][0] = 'A';
    key_bind[HOS_KEY_MOVE_RIGHT][0] = 'D', key_bind[HOS_KEY_CAM_UP][0] = VK_UP, key_bind[HOS_KEY_CAM_DOWN][0] = VK_DOWN;
    key_bind[HOS_KEY_CAM_LEFT][0] = VK_LEFT, key_bind[HOS_KEY_CAM_RIGHT][0] = VK_RIGHT;
    key_bind[HOS_KEY_WALK][0] = VK_CAPITAL;
    key_bind[B_ZL][0] = 'Z';
    if (side_binds) key_bind[B_A][1] = VK_XBUTTON1, key_bind[B_Y][1] = VK_LBUTTON, key_bind[HOS_KEY_WALK][1] = VK_XBUTTON2;
}
/* the native mouse look (no camera found: the motion goes to mouse_stick) and the mouse's sensitivity, which
   read_input calls */
int hos_mouse_native(void) { return controls_case ? (mouse_let_go ? 0 : native_free ? 1 : -1) : -1; }
/* (host/display.c: one of the game's menus has the mouse, the Insert menu closed) */
int hos_mouse_in_game_menu(void) { return game_menu_mouse; }
void hos_mouse_native_free(int free) { native_free = free; }
void hos_mouse_native_stick(int held) { (void)held; }
static void mouse_setup(void) {}
static void mouse_sens_keys(void) {}
static int mouse_motion[2];   /* (mouse_stick's: Steam's Desktop layout drives the mouse with the right stick) */
int mouse_stick(int32_t *x, int32_t *y, int held) {
    mouse_pointing = held; mouse_queries++;
    *x = mouse_motion[0]; *y = mouse_motion[1];
    return mouse_motion[0] || mouse_motion[1];   /* (the pointer taken in whenever it moved) */
}
void rx_request_frame_dump(void) { commands++; }
void rx_set_screenshot_ms(int64_t ms) { (void)ms; commands++; }
void rx_request_rdoc_capture(int frames) { (void)frames; commands++; }
void rx_flash_dark_arm(void) { commands++; }

#include "input_actual.inc"

static void packet4(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    static int64_t expected_sampling;
    Ctx context = {{0}};
    context.x[0] = 64; context.x[1] = 2; context.x[2] = 16; context.x[30] = 0x1234;
    G32(16) = NPAD_NO1;
    memset(memory + 64, 0xa5, 2 * STATE_SIZE + 8);
    npad_states(&context);
    CHECK(context.x[0] == 2);
    expected_sampling++;
    /* (the buttons alone, whatever the sticks: no direction bits 16-23. The game's own controllers read bits 16-19 as the
       D-pad, so a stick pushed forward with them held the D-pad and A pressed then did not roll) */
    for (int i = 0; i < 2; i++) {
        const uint64_t address = 64 + (uint64_t)i * STATE_SIZE;
        CHECK(G64(address) == (uint64_t)(expected_sampling - i));
        CHECK(G64(address + 8) == buttons);
        CHECK((int32_t)G32(address + 16) == lx && (int32_t)G32(address + 20) == ly);
        CHECK((int32_t)G32(address + 24) == rx && (int32_t)G32(address + 28) == ry);
        CHECK(G32(address + 32) == ATTR_CONNECTED && G32(address + 36) == 0);
    }
    for (int i = 0; i < 8; i++) CHECK(memory[64 + 2 * STATE_SIZE + i] == 0xa5);
    CHECK(commands == 0);
}
static void packet(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx) { packet4(buttons, lx, ly, rx, 0); }
/* (stick-directions) Each stick pushed all the way in each direction, and both diagonals of each, with A held, then the
   D-pad's four with the left stick forward: the state carries the buttons alone, and a D-pad press still reaches it. With
   the sticks' direction bits (16-23) the left stick forward reached the game's controllers as a held D-pad bit, and A
   pressed while running forward did not roll */
static void stick_directions_case(void) {
    static const int32_t d[8][2] = {{0, 32767}, {0, -32767}, {-32767, 0}, {32767, 0},
                                    {-23170, 23170}, {23170, 23170}, {-23170, -23170}, {23170, -23170}};
    const uint64_t a = 1ULL << B_A;
    controls_case = front = bridge_active = 1;   /* (no pad or key of the player's held: the bridge's input alone, then none) */
    for (int stick = 0; stick < 2; stick++)
        for (int k = 0; k < 8; k++) {
            memset(bridge_stick, 0, sizeof bridge_stick);
            bridge_stick[2 * stick] = d[k][0], bridge_stick[2 * stick + 1] = d[k][1];
            bridge_buttons = a;
            packet4(a, bridge_stick[0], bridge_stick[1], bridge_stick[2], bridge_stick[3]);
        }
    static const int dpad[4] = {B_UP, B_DOWN, B_LEFT, B_RIGHT};
    memset(bridge_stick, 0, sizeof bridge_stick);
    bridge_stick[1] = 32767;
    for (int k = 0; k < 4; k++) {
        bridge_buttons = a | 1ULL << dpad[k];
        packet4(bridge_buttons, 0, 32767, 0, 0);
    }
    memset(bridge_stick, 0, sizeof bridge_stick);
    bridge_buttons = 0, bridge_active = 0;
    packet(0, 0, 0, 0);
}

/* The real Npad writer, with the control socket boundary providing exclusive logical presses. */
static void hold_toggle_case(void) {
    const uint64_t r = 1ULL << B_R, plus = 1ULL << B_PLUS;
    front = bridge_active = 1;
    bridge_buttons = r; packet(r, 0, 0, 0);   /* Off: ordinary hold/release */
    bridge_buttons = 0; packet(0, 0, 0, 0);
    toggle_flags = 1 << B_R;
    bridge_buttons = r; packet(r, 0, 0, 0);
    packet(r, 0, 0, 0);   /* repeat */
    bridge_buttons = 0; packet(r, 0, 0, 0);
    bridge_buttons = r; packet(0, 0, 0, 0);
    bridge_buttons = 0; packet(0, 0, 0, 0);
    bridge_buttons = r; packet(r, 0, 0, 0);
    menu_open = 1; packet(0, 0, 0, 0);
    bridge_buttons = 0; packet(0, 0, 0, 0);
    bridge_buttons = r; packet(0, 0, 0, 0);   /* never latches within Insert */
    menu_open = 0; packet(0, 0, 0, 0);   /* must release after it closes */
    bridge_buttons = 0; packet(0, 0, 0, 0);
    bridge_buttons = r; packet(r, 0, 0, 0);
    bridge_buttons = 0; packet(r, 0, 0, 0);
    toggle_reset = 1; packet(0, 0, 0, 0);   /* display event during a polling gap, or rebinding */
    packet(0, 0, 0, 0);
    bridge_buttons = r; packet(r, 0, 0, 0);
    front = 0; packet(0, 0, 0, 0);
    front = 1; packet(0, 0, 0, 0);
    bridge_buttons = 0; packet(0, 0, 0, 0);
    bridge_buttons = r; packet(r, 0, 0, 0);
    bridge_buttons = plus; packet(plus, 0, 0, 0);   /* pause opens and drops the latch */
    bridge_buttons = 0; packet(0, 0, 0, 0);
    toggle_flags |= 1 << B_PLUS;
    bridge_buttons = plus; packet(plus, 0, 0, 0);   /* pause itself remains usable */
    packet(plus, 0, 0, 0); /* Plus stays an ordinary hold, even with a hand-written flag. */
    bridge_buttons = 0; packet(0, 0, 0, 0);
}

/* Sprint's input as npad_states hands it to host/oot_sprint.c (a spy here): Sprint never presses A, so the game's buttons
   are the player's own; held, toggled, a toggled sprint held (never ended) by Insert, Plus and the focus lost, a bridge's
   exclusive input keeping the player's Sprint out, and a bridge's SPRINT counted as its key, behind other windows too */
#define SPY(on, down, toggle, reset, blocked) \
    CHECK(spy_sprint_on == (on) && spy_sprint_down == (down) && spy_sprint_toggle == (toggle) && \
          spy_sprint_reset == (reset) && spy_sprint_blocked == (blocked) && !spy_sprint_l3)
static void sprint_toggle_case(void) {
    const uint64_t a = 1ULL << B_A;
    controls_case = front = 1;
    sprint_toggle_mode = 1;
    key_bind[HOS_KEY_SPRINT][0] = 'G';
    held_keys['W'] = 1;
    packet(0, 0, 32767, 0);
    SPY(1, 0, 1, 0, 0);
    held_keys['G'] = 1; packet(0, 0, 32767, 0);   /* (no A from Sprint: the game's run comes from Link's flags) */
    SPY(1, 1, 1, 0, 0);
    menu_open = 1; packet(0, 0, 0, 0);            /* Insert open: no press is Sprint's, a toggled sprint is held */
    CHECK(!spy_sprint_reset && spy_sprint_blocked == (SPRINT_BLOCKED | SPRINT_AWAY));   /* (the keys may go to the menu) */
    menu_open = 0; packet(0, 0, 32767, 0);
    SPY(1, 1, 1, 0, 0);
    held_keys['G'] = 0; packet(0, 0, 32767, 0);
    SPY(1, 0, 1, 0, 0);
    physical_buttons = a; packet(a, 0, 32767, 0); /* the player's own A: theirs, untouched */
    physical_buttons = 0; packet(0, 0, 32767, 0);
    physical_buttons = 1ULL << B_PLUS; packet(1ULL << B_PLUS, 0, 32767, 0);   /* the game's pause: nothing ended, */
    SPY(1, 0, 1, 0, SPRINT_PAUSE);                /* (its press said, for the hold while the game stops under it) */
    packet(1ULL << B_PLUS, 0, 32767, 0);          /* (held: said once) */
    SPY(1, 0, 1, 0, 0);
    physical_buttons = 0; packet(0, 0, 32767, 0);
    front = 0; packet(0, 0, 0, 0);                /* the focus lost: away, a toggled sprint held, nothing ended */
    SPY(1, 0, 1, 0, SPRINT_AWAY);
    front = 1; packet(0, 0, 32767, 0);
    SPY(1, 0, 1, 0, 0);
    sprint_toggle_mode = 0;                       /* Toggle off: Sprint while held */
    held_keys['G'] = 1; packet(0, 0, 32767, 0);
    SPY(1, 1, 0, 0, 0);
    front = 0; packet(0, 0, 0, 0);                /* (held: the focus lost is not away, as before) */
    SPY(1, 0, 0, 0, 0);
    front = 1; packet(0, 0, 32767, 0);
    held_keys['G'] = 0;
    bridge_up = 1; bridge_active = 1; bridge_buttons = 1ULL << B_SPRINT; packet(0, 0, 32767, 0);
    CHECK(spy_sprint_down && !spy_sprint_blocked);   /* a bridge's SPRINT is Sprint's key, even exclusive */
    bridge_buttons = 0; packet(0, 0, 32767, 0);
    CHECK(!spy_sprint_down && spy_sprint_blocked);   /* an exclusive bridge input keeps the player's Sprint out */
    bridge_active = 0; packet(0, 0, 32767, 0);
}
#undef SPY

/* The game's menus' mouse clicks (host/oot_menu.c), through the real hos_hid_tap and host_taps: a click is a short
   press of the host's own, which a Toggle on that button must not latch; the player's presses of it still do. */
static void hold_taps_case(void) {
    front = controls_case = 1;
    const uint64_t buttons[2] = {1ULL << B_R, 1ULL << B_A};
    const char *const taps[2] = {"0:R/75", "0:A/75"};
    for (int k = 0; k < 2; k++) {
        const uint64_t b = buttons[k];
        toggle_flags = (int)b;
        physical_buttons = 0; packet(0, 0, 0, 0);
        for (int click = 0; click < 2; click++) {   /* (each click: the button while it lasts, then nothing) */
            hos_hid_tap(taps[k]); packet(b, 0, 0, 0);
            now_ns += 80000000; packet(0, 0, 0, 0);
        }
        physical_buttons = b; packet(b, 0, 0, 0);   /* the player's press latches, */
        hos_hid_tap(taps[k]); packet(b, 0, 0, 0);   /* a click while it is held changes nothing, */
        now_ns += 80000000; packet(b, 0, 0, 0);     /* nor does the click's end, */
        physical_buttons = 0; packet(b, 0, 0, 0);
        hos_hid_tap(taps[k]); packet(b, 0, 0, 0);   /* nor a click while it is latched, */
        now_ns += 80000000; packet(b, 0, 0, 0);
        physical_buttons = b; packet(0, 0, 0, 0);   /* and the player's next press lets it go */
        physical_buttons = 0; packet(0, 0, 0, 0);
    }
}

/* Real read_input and Npad serialization: physical release must retain mouse routing. */
static void hold_mouse_case(void) {
    front = controls_case = 1;
    for (int mode = 0; mode <= 2; mode++) {
        mouse_mode = mode;
        for (int b = B_L; b <= B_ZR; b++) {
            toggle_flags = 1 << b;
            physical_buttons = 0; packet(0, 0, 0, 0);
            physical_buttons = 1ULL << b; packet(physical_buttons, 0, 0, 0);
            physical_buttons = 0; packet(1ULL << b, 0, 0, 0);
            if (mode == 1) CHECK(native_free == (b == B_ZL));
            if (mode == 0 || (mode == 1 && b != B_ZL)) CHECK(mouse_pointing == (b == B_L));
            physical_buttons = 1ULL << b; packet(0, 0, 0, 0);
            physical_buttons = 0; packet(0, 0, 0, 0);
        }
    }
}

static void hold_background_case(void) {
    controls_case = 1;
    front = 0; menu_background = 1;
    toggle_flags = 1 << B_ZL;
    physical_buttons = 1ULL << B_ZL; packet(physical_buttons, 0, 0, 0);
    physical_buttons = 0; packet(1ULL << B_ZL, 0, 0, 0);
    physical_buttons = 1ULL << B_ZL; packet(0, 0, 0, 0);
    physical_buttons = 0; packet(0, 0, 0, 0);
}

/* One of the game's menus takes the mouse: its capture ends, and the left, right and middle buttons are that menu's
   clicks (host/display.c game_menu_press). The side buttons are no menu's, so they stay their controls' there, as a
   key does: Mouse 4 on A confirms a choice of the text box (Longshot #1306). Through the real read_input. */
static void menu_side_case(void) {
    const uint64_t a = 1ULL << B_A;
    const int tilt = 32767 * 35 / 100;
    front = controls_case = side_binds = 1;
    mouse_mode = 1;   /* (Native: the fake camera has the capture until mouse_let_go) */
    walk_speed = 35;
    packet(0, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 1; packet(a, 0, 0, 0);   /* in play, the mouse captured: Mouse 4 is A */
    held_keys[VK_XBUTTON1] = 0; packet(0, 0, 0, 0);
    mouse_let_go = game_menu_mouse = 1; packet(0, 0, 0, 0);   /* a menu of the game's takes the mouse */
    held_keys[VK_XBUTTON1] = 1; packet(a, 0, 0, 0);   /* Mouse 4 is still A there */
    packet(a, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 0; packet(0, 0, 0, 0);
    held_keys[VK_LBUTTON] = 1; packet(0, 0, 0, 0);    /* the left button is the menu's click, not Y */
    held_keys[VK_LBUTTON] = 0; packet(0, 0, 0, 0);
    held_keys['W'] = held_keys[VK_XBUTTON2] = 1; packet(0, 0, tilt, 0);   /* a keys-only control on Mouse 5: Walk */
    held_keys[VK_XBUTTON2] = 0; packet(0, 0, 32767, 0);
    held_keys['W'] = 0; packet(0, 0, 0, 0);
    /* held from the play into the menu and back: one press, as a key's */
    mouse_let_go = game_menu_mouse = 0; packet(0, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 1; packet(a, 0, 0, 0);
    mouse_let_go = game_menu_mouse = 1; packet(a, 0, 0, 0);
    mouse_let_go = game_menu_mouse = 0; packet(a, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 0; packet(0, 0, 0, 0);
    /* the mouse free in play (Esc): no button of it is a control, and one held as a menu opens waits to be let go */
    mouse_let_go = 1; packet(0, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 1; packet(0, 0, 0, 0);
    game_menu_mouse = 1; packet(0, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 0; packet(0, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 1; packet(a, 0, 0, 0);
    game_menu_mouse = 0; packet(0, 0, 0, 0);          /* the menu closed, the mouse still free: no longer A */
    held_keys[VK_XBUTTON1] = 0; packet(0, 0, 0, 0);
    /* Mouse camera Off: the mouse is never the game's, in its menus neither */
    mouse_mode = 2; game_menu_mouse = 1; packet(0, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 1; packet(0, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 0; packet(0, 0, 0, 0);
    /* As the stick: the same as Native (the fake stick never has the capture) */
    mouse_mode = 0; packet(0, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 1; packet(a, 0, 0, 0);
    held_keys[VK_XBUTTON1] = 0; packet(0, 0, 0, 0);
}

/* The Steam Deck's built-in controls expose a mouse interface as well as the pad (in its desktop layout R2 is a left
   click, L2 a right one), so a gameplay control bound to a mouse button fired twice: once from the pad, once from
   that click. While those controls give gameplay input the mouse's gameplay bindings must not count; its pointer and
   its clicks in one of the game's menus are not touched. Through the real read_input. */
static void deck_mouse_case(void) {
    const uint64_t a = 1ULL << B_A, y = 1ULL << B_Y;
    front = controls_case = side_binds = 1;
    mouse_mode = 1;   /* (Native: the fake camera holds the capture, so the mouse's buttons can be controls) */
    packet(0, 0, 0, 0);
    /* an ordinary pad: the left button is Y, as its binding says */
    held_keys[VK_LBUTTON] = 1; packet(y, 0, 0, 0);
    /* the Deck's built-in controls give gameplay input: the same click is not Y, and the pad's own press counts */
    physical_buttons = a; physical_deck_builtin = 1;
    packet(a, 0, 0, 0);
    /* let go and pressed again while they still give input: still nothing from the mouse */
    held_keys[VK_LBUTTON] = 0; packet(a, 0, 0, 0);
    held_keys[VK_LBUTTON] = 1; packet(a, 0, 0, 0);
    /* the controls go, the button held through: it must be let go before it is Y again */
    physical_buttons = 0; physical_deck_builtin = 0;
    packet(0, 0, 0, 0);
    held_keys[VK_LBUTTON] = 0; packet(0, 0, 0, 0);
    held_keys[VK_LBUTTON] = 1; packet(y, 0, 0, 0);
    held_keys[VK_LBUTTON] = 0; packet(0, 0, 0, 0);
    /* one of the game's menus has the mouse: its pointer and left click are the menu's either way */
    mouse_let_go = game_menu_mouse = 1;
    physical_deck_builtin = 1; packet(0, 0, 0, 0);
    held_keys[VK_LBUTTON] = 1; packet(0, 0, 0, 0);
    held_keys[VK_LBUTTON] = 0; packet(0, 0, 0, 0);
    physical_deck_builtin = 0; packet(0, 0, 0, 0);
    mouse_let_go = game_menu_mouse = 0; packet(0, 0, 0, 0);
    /* the mouse free in play (Esc): no button of it is a control, the built-in controls or not */
    mouse_let_go = 1;
    physical_buttons = a; physical_deck_builtin = 1;
    held_keys[VK_LBUTTON] = 1; packet(a, 0, 0, 0);
    held_keys[VK_LBUTTON] = 0; physical_buttons = 0; physical_deck_builtin = 0; packet(0, 0, 0, 0);
}

/* Steam's Desktop layout drives the Deck's mouse with the right stick as well as its buttons: with the built-in
   controls giving gameplay input the camera takes the pad's stick once, not the mouse's motion as well, and the
   pointer is still taken in so it does not fly across the window. Through the real read_input. */
static void deck_motion_case(void) {
    front = controls_case = 1;
    mouse_mode = 0;   /* (As the stick: the mouse's motion would be the right stick's) */
    physical_rstick = 8000;
    mouse_motion[0] = 20000;
    packet(0, 0, 0, 20000);   /* an ordinary mouse: its motion is the camera's, over the stick's */
    physical_deck_builtin = 1;
    const int before = mouse_queries;
    packet(0, 0, 0, 8000);    /* the built-in controls: the pad's stick alone */
    CHECK(mouse_queries > before);   /* (the pointer still taken in: captured and held at the center) */
    mouse_mode = 1;           /* (Native too: the camera hook is not asked for, the motion dropped) */
    packet(0, 0, 0, 8000);
    mouse_mode = 2;           /* (Off too: the built-in controls still take the pointer, its motion counts for nothing) */
    packet(0, 0, 0, 8000);
    physical_deck_builtin = 0; mouse_mode = 0;
    packet(0, 0, 0, 20000);   /* (the controls go: the mouse's motion is the camera's again) */
    mouse_motion[0] = mouse_motion[1] = 0; physical_rstick = 0;
}

/* The player's input hides the game menus' cursor until the mouse moves (host/display.c gm_cursor_shown), so an input
   counts when it goes down, not at every read while it stays down: the ocarina's song sheet is open while ZL is held,
   and a ZL held on a pad or the Z key counted at every read, so the cursor the mouse had just shown was hidden again
   before the menu used it and its hover failed (Longshot #1253). Through the real read_input. */
static void held_input_case(void) {
    const uint64_t zl = 1ULL << B_ZL, l = 1ULL << B_L;
    front = controls_case = 1;
    packet(0, 0, 0, 0); CHECK(player_stamps == 0);
    held_keys['Z'] = 1; packet(zl, 0, 0, 0); CHECK(player_stamps == 1);   /* a key goes down: the player's input */
    for (int i = 0; i < 5; i++) packet(zl, 0, 0, 0);
    CHECK(player_stamps == 1);                                            /* held: not at every read */
    held_keys['Z'] = 0; packet(0, 0, 0, 0); CHECK(player_stamps == 1);    /* let go: not an input */
    held_keys['Z'] = 1; packet(zl, 0, 0, 0); CHECK(player_stamps == 2);   /* down again: one more */
    held_keys['Z'] = 0; packet(0, 0, 0, 0);
    physical_buttons = zl; packet(zl, 0, 0, 0); CHECK(player_stamps == 3);   /* a pad's button the same */
    for (int i = 0; i < 5; i++) packet(zl, 0, 0, 0);
    CHECK(player_stamps == 3);
    physical_buttons = zl | l; packet(zl | l, 0, 0, 0); CHECK(player_stamps == 4);   /* a second one, the first held */
    physical_buttons = l; packet(l, 0, 0, 0); CHECK(player_stamps == 4);
    physical_buttons = 0; packet(0, 0, 0, 0); CHECK(player_stamps == 4);
}

/* Walk reads its raw key before the Npad button latch. Neither toggle drives the other. */
static void walk_toggle_case(void) {
    const uint64_t zl = 1ULL << B_ZL;
    const int tilt = 32767 * 35 / 100;
    front = controls_case = 1;
    walk_speed = 35;
    for (int mode = 0; mode <= 1; mode++) {
        walk_mode = 0; packet(0, 0, 0, 0);   /* clear Walk's own state before this mode */
        walk_mode = mode;
        toggle_flags = 1 << B_ZL;
        held_keys['W'] = held_keys[VK_CAPITAL] = held_keys['Z'] = 1;
        packet(zl, 0, tilt, 0);
        packet(zl, 0, tilt, 0);   /* held/repeated reads change neither latch */
        held_keys[VK_CAPITAL] = held_keys['Z'] = 0;
        packet(zl, 0, mode ? tilt : 32767, 0);
        held_keys['Z'] = 1; packet(0, 0, mode ? tilt : 32767, 0);
        held_keys['Z'] = 0; packet(0, 0, mode ? tilt : 32767, 0);
        held_keys[VK_CAPITAL] = 1; packet(0, 0, mode ? 32767 : tilt, 0);
        memset(held_keys, 0, sizeof held_keys);
        packet(0, 0, 0, 0);
    }
    walk_mode = 1;
    held_keys['W'] = held_keys[VK_CAPITAL] = held_keys['Z'] = 1;
    packet(zl, 0, tilt, 0);
    held_keys[VK_CAPITAL] = held_keys['Z'] = 0; packet(zl, 0, tilt, 0);
    /* Insert clears Switch-button latches, but preserves Walk's chosen walking state. */
    menu_open = 1; packet(0, 0, 0, 0);
    held_keys[VK_CAPITAL] = held_keys['Z'] = 1; packet(0, 0, 0, 0);
    menu_open = 0; packet(0, 0, tilt, 0);   /* the closing press rearms neither */
    held_keys[VK_CAPITAL] = held_keys['Z'] = 0; packet(0, 0, tilt, 0);
    held_keys['Z'] = 1; packet(zl, 0, tilt, 0);
    held_keys['Z'] = 0; packet(zl, 0, tilt, 0);
    /* Focus loss has the same distinction; only a fresh Walk press changes its mode. */
    front = 0; packet(0, 0, 0, 0);
    held_keys[VK_CAPITAL] = held_keys['Z'] = 1;
    front = 1; packet(0, 0, tilt, 0);
    held_keys[VK_CAPITAL] = held_keys['Z'] = 0; packet(0, 0, tilt, 0);
    held_keys['Z'] = 1; packet(zl, 0, tilt, 0);
    held_keys['Z'] = 0; packet(zl, 0, tilt, 0);
    /* Plus/Minus and rebinding reset Switch buttons, not the separate Walk state. */
    for (int b = B_PLUS; b <= B_MINUS; b++) {
        physical_buttons = 1ULL << b; packet(physical_buttons, 0, tilt, 0);
        physical_buttons = 0; packet(0, 0, tilt, 0);
        held_keys['Z'] = 1; packet(zl, 0, tilt, 0);
        held_keys['Z'] = 0; packet(zl, 0, tilt, 0);
    }
    toggle_reset = 1; packet(0, 0, tilt, 0);
    packet(0, 0, tilt, 0);
    held_keys['Z'] = 1; packet(zl, 0, tilt, 0);
    held_keys['Z'] = 0; packet(zl, 0, tilt, 0);
    toggle_flags = 0; packet(0, 0, tilt, 0);
    held_keys[VK_CAPITAL] = 1; packet(0, 0, 32767, 0);
    CHECK(oc_consumed_pads == 0);
}

static void append_live(const char *text) {
    const char *path = getenv("HOS_INPUT_LIVE"); CHECK(path);
    FILE *file = fopen(path, "ab"); CHECK(file);
    CHECK(fputs(text, file) >= 0 && fclose(file) == 0);
}

static void frame_clock_case(const char *name) {
    const int hold = !strcmp(name, "frame-hold");
    const int live = !strcmp(name, "live-frame");
    const int reuse = !strcmp(name, "reuse-frame");
    const int wall = !strcmp(name, "wall") || !strcmp(name, "zero") || !strcmp(name, "empty");
    CHECK(hold || live || reuse || wall);
    front = hold || wall;
    packet(hold || wall ? 1ULL << B_A : 0, 0, hold || wall ? 32767 : 0, 0);
    if (hold) {
        now_ns += 7000000000LL; /* no native present returns during a compile */
        packet(1ULL << B_A, 0, 32767, 0);
        CHECK(hos_hid_last_read == now_ns); /* HID diagnostics stay wall-clock */
        hos_present_count = 1011;
        packet(1ULL << B_A, 0, 32767, 0);
        hos_present_count = 1012; /* exactly 200 virtual ms, relative to initial frame 1000 */
        packet(0, 0, 0, 0);
        CHECK(pad_merges == 0 && key_queries == 0 && mouse_queries == 0);
        hos_present_count = 1311;
        packet(0, 0, 0, 0); /* exclusive margin still held at 5183 ms */
        hos_present_count = 1312;
        packet((1ULL << B_B) | (1ULL << B_X), 32767, 0, 8000);
        CHECK(pad_merges > 0 && frame_reads > 0);
    } else if (wall) {
        now_ns += 7000000000LL;
        packet((1ULL << B_B) | (1ULL << B_X), 32767, 0, 8000);
        CHECK(pad_merges > 0 && frame_reads == 0);
    } else if (live) {
        append_live("0:A+LUP/200"); now_ns += 30000000;
        packet(0, 0, 0, 0); /* incomplete live line remains unconsumed */
        hos_present_count = 1060;
        append_live("\n"); now_ns += 30000000;
        packet(1ULL << B_A, 0, 32767, 0);
        CHECK(presses[0].start_ms == 1000 && presses[0].end_ms == 1200);
        append_live("0:B/200\n"); now_ns += 5000000;
        packet(1ULL << B_A, 0, 32767, 0); /* file polling stays on the 30 ms wall clock */
        now_ns += 30000000;
        packet((1ULL << B_A) | (1ULL << B_B), 0, 32767, 0);
        CHECK(presses[1].start_ms == 1000);
        now_ns += 7000000000LL;
        packet((1ULL << B_A) | (1ULL << B_B), 0, 32767, 0);
        hos_present_count = 1072;
        packet(0, 0, 0, 0);
    } else {
        for (int i = 0; i < MAX_PRESSES; i++) CHECK(parse_presses("0:A/200", 0, 1) == 1);
        now_ns += 7000000000LL;
        CHECK(parse_presses("0:B/200", 0, 1) == 0);
        hos_present_count = 1012;
        CHECK(parse_presses("0:B/200", 200, 1) == 0); /* original strict reuse boundary */
        hos_present_count = 1013;
        CHECK(parse_presses("0:B/200", 216, 1) == 1);
        packet(1ULL << B_B, 0, 0, 0);
        CHECK(npresses == MAX_PRESSES);
    }
    CHECK(script_logs == (getenv("HOS_INPUT_SCRIPT") && *getenv("HOS_INPUT_SCRIPT") ? 1 : 0));
    CHECK(bootstrap_polls == 1);
    CHECK(origin_logs == (wall ? 0 : 1));
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(!pads_in_background(NULL, 0));   /* out of the box: the window in front only */
    CHECK(pads_in_background(NULL, 1));    /* the menu's row */
    CHECK(pads_in_background("1", 0));     /* HOS_INPUT_BACKGROUND=1 without the menu */
    CHECK(!pads_in_background("0", 1));    /* HOS_INPUT_BACKGROUND=0 wins over the menu */
    memory = calloc(512, 1); CHECK(memory);
    const char *name = argv[1];
    if (!strcmp(name, "sprint-toggle")) {
        sprint_toggle_case();
        free(memory); return 0;
    }
    if (!strcmp(name, "walk-toggle")) {
        walk_toggle_case();
        puts("PASS: Walk Hold/Toggle beside button Toggle; independent edges, speed and menu/focus/pause resets");
        free(memory); return 0;
    }
    if (!strcmp(name, "hold-mouse") || !strcmp(name, "hold-background")) {
        if (!strcmp(name, "hold-mouse")) hold_mouse_case();
        else hold_background_case();
        puts("PASS: latch mouse/background routing");
        free(memory); return 0;
    }
    if (!strcmp(name, "menu-side-buttons")) {
        menu_side_case();
        puts("PASS: the mouse's side buttons stay their controls' while one of the game's menus has the mouse");
        free(memory); return 0;
    }
    if (!strcmp(name, "deck-mouse")) {
        deck_mouse_case();
        puts("PASS: the Deck's built-in controls give their actions once, not twice through the mouse interface they also expose");
        free(memory); return 0;
    }
    if (!strcmp(name, "deck-motion")) {
        deck_motion_case();
        puts("PASS: the Deck's built-in controls' right stick is the camera once, not the mouse's motion as well; the pointer is still taken in");
        free(memory); return 0;
    }
    if (!strcmp(name, "stick-directions")) {
        stick_directions_case();
        puts("PASS: the Npad states carry the buttons alone, whatever the sticks; the D-pad still reaches them");
        free(memory); return 0;
    }
    if (!strcmp(name, "held-input")) {
        held_input_case();
        puts("PASS: an input held down is the player's input when it goes down, not at every read");
        free(memory); return 0;
    }
    if (!strcmp(name, "hold-taps")) {
        hold_taps_case();
        puts("PASS: a Toggle button does not latch the host's clicks; the player's presses still latch");
        free(memory);
        return 0;
    }
    if (!strcmp(name, "hold-toggle")) {
        hold_toggle_case();
        puts("PASS: actual Npad hold-to-toggle, menu, focus, binding reset and pause");
        free(memory);
        return 0;
    }
    if (!strcmp(name, "frame-hold") || !strcmp(name, "live-frame") || !strcmp(name, "reuse-frame") ||
        !strcmp(name, "wall") || !strcmp(name, "zero") || !strcmp(name, "empty")) {
        frame_clock_case(name);
        printf("PASS: actual input frame clock %s\n", name);
        free(memory);
        return 0;
    }
    front = !strcmp(name, "foreground") || !strcmp(name, "exclusive-foreground");
    menu_background = !strcmp(name, "pads-menu") || !strcmp(name, "pads-off");
    const int exclusive = !strncmp(name, "exclusive-", 10);
    const int pads = !exclusive && (front || !strcmp(name, "pads-env") || !strcmp(name, "pads-menu"));
    const uint64_t physical = pads ? (1ULL << B_B) | (front ? 1ULL << B_X : 0) : 0;
    if (!strcmp(name, "live-background")) {
        packet(0, 0, 0, 0);
        append_live("0:A+LUP/200"); now_ns += 30000000;
        packet(0, 0, 0, 0); /* incomplete authored line is not consumed */
        append_live("\n"); now_ns += 30000000;
        packet(1ULL << B_A, 0, 32767, 0);
        now_ns += 200000000;
        packet(0, 0, 0, 0); /* consumed once, then released at the actual hold boundary */
    } else if (!strcmp(name, "no-script-background")) {
        packet(0, 0, 0, 0);
    } else {
        packet((1ULL << B_A) | physical, pads ? (front ? 32767 : -12000) : 0, 32767, pads ? 8000 : 0);
        now_ns += 201000000;
        packet(physical, pads ? (front ? 32767 : -12000) : 0, 0, pads ? 8000 : 0);
        if (exclusive) {
            CHECK(pad_merges == 0 && key_queries == 0 && mouse_queries == 0);
            now_ns += 5100000000LL;
            packet(front ? (1ULL << B_B) | (1ULL << B_X) : 0, front ? 32767 : 0, 0, front ? 8000 : 0);
        }
    }
    CHECK(script_logs == (getenv("HOS_INPUT_SCRIPT") && *getenv("HOS_INPUT_SCRIPT") ? 1 : 0));
    CHECK(bootstrap_polls == 1); /* production startup poll is allowed, but its Y/stick result is discarded */
    if (!front) {
        CHECK(key_queries == 0 && mouse_queries == 0);
        if (!pads) CHECK(pad_merges == 0);
    }
    if (!strcmp(name, "script-background")) CHECK(strstr(last_log, "game window not in front"));
    if (front) CHECK(pad_merges > 0 && key_queries > 0 && mouse_queries > 0);
    CHECK(focus_queries > 0);
    CHECK(oc_consumed_pads == 0);   /* hos_oc_wait stays 0: the ocarina note layer took no pad button */
    printf("PASS: actual scripted parse/merge/Npad route %s\n", name);
    free(memory);
    return 0;
}
