/* (longshot) The HUD laid out for the picture's shape (hos_wide_ui_tick below): at its 16:9 size, what sits at a side or
   corner of its 16:9 canvas moved out (or in) to the screen's, as a layout made for that shape would place it; panes
   spanning the canvas drawn the screen's width. The pane planner is ozwaldorf's ("Extended" ultrawide menus,
   https://github.com/NotRexed/Hookshot/pull/2843), here for the HUD's layouts on every shape, by
   default, both ways and up and down. The menus are not moved (on a narrower screen they are drawn smaller to fit), except
   with the Ultrawide menus row at Extended (HOS_OPT_WIDE_MENUS, 102) on a wider screen: their layouts are planned as the
   HUD's are, by the screen's whole extra width, with the pause menu's pages spaced wider apart and its background drawn in
   three pieces (hos_wide_ui_bg), and the lists kept at the screen's width widened as they are built (hos_wide_ui_built).

   The game's 2D layouts keep their panes in an array (the layout's +0x100, 0xb0 bytes each). f_8154cab0 makes a pane's
   final 3x4 transform (+0x28, the translation at +0x4c) from its own position, scale and rotation and its parent's
   transform, only when the pane is marked for it (dirty bit 6, f_8154caa0). nx2/oot_wide.py calls in here:

   - hos_wide_ui_parent: the parent f_8154cab0 multiplies by (0 for none), on the thread that is making the transform
   - hos_wide_ui_placed: the transform just stored. A layout's root pane is noted (its kind, by its layout's name), and
     so is every pane of the HUD's trees, the loading fade's and the held layouts' (its parent, and its place on the canvas
     as the game made it, without the moves): a pane made from one noted in such a tree. A menu's other panes are not (the plan never looks
     at them). If the plan moves a pane, its translation is moved, and its children follow, since theirs are made from it.
     A menu's root is made smaller here on a narrower screen
   - hos_wide_ui_size: a pane's size as drawn (f_8154ac30): a pane the plan widens is drawn the screen's width
   - hos_wide_ui_force (read in f_8154caa0): while set, every pane remakes its transform: after a change of the screen's
     shape, or of the plan

   The plan (hos_wide_ui_tick, each present) walks the HUD's noted panes as a tree, parts' own layouts included, with how
   far each one's contents reach across (as the ultrawide UI romfs mod's rules.py does from the layout files): a pane
   spanning the canvas is widened; one touching an edge, or the panes on one side of their parent together when they reach
   an edge or sit far out, move to that side; a container across the centre is looked into; what is off the canvas or
   wider than it (a scrolling strip) is left alone. Up and down the same, when the HUD is drawn smaller (visit_y).

   The Insert menu's HUD layout row (HOS_OPT_HUD_LAYOUT, 71) at Middle 16:9 keeps the HUD in the middle 16:9 as before (on
   a narrower screen fitted to its width, as the menus); HOS_WIDE_UI=0 or 1, when set, stands over the row;
   HOS_WIDE_UI_LOG=<n> logs the first n changes of the plan, each root layout, and the plan's time. */
#include "hos.h"
#include "../gpu/vk/vk_api.h"
#ifndef LONGSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/longshot_env.h"
#endif
#include "../gpu/vk/vk_api.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

extern volatile float hos_wide_ratio;

enum {
    LAYOUT_RES = 0xf8, RES_NAME = 0x18, RES_PANE_NAMES = 0x20,   /* (the layout file's UIL header: its name, its panes') */
    LAYOUT_PANES = 0x100, LAYOUT_COUNT = 0x13c, PANE_SIZE = 0xb0,
    PANE_LAYOUT = 0x18, PANE_XX = 0x28, PANE_YY = 0x38, PANE_X = 0x4c, PANE_Y = 0x50, PANE_W = 0x58, PANE_H = 0x5c,
    PANE_FLAGS = 0xa8,
    PANE_TYPE_NULL = 0, PANE_TYPE_SCISSOR = 4, PANE_TYPE_CAPTURE = 5, PANE_TYPE_PART = 6, PANE_TYPE_HOST = 7,   /* (7: holds other layouts, a list) */
    PANE_DIRTY_RECT = 4,
};
static const float CANVAS_HALF = 1920.0f;   /* (every layout's canvas is 3840 x 2160) */
static const float CANVAS_HALF_Y = 1080.0f;
static const float SIDE_Y = 620.0f;         /* (a group centred further up or down than this sits at the top or bottom) */
static const float EDGE_NEAR = 150.0f;      /* (a pane within this of an edge is anchored to it) */
static const float EDGE_TOL = 40.0f;        /* (a pane within this of both edges spans the canvas) */
static const float SIDE = 1100.0f;          /* (a group centred further out than this sits at a side) */
static const float CROSS = 0.25f;           /* (a pane with less than this share across the centre belongs to one side) */
static const float SIZE_CAP = 960.0f;       /* (wider panes count by their origin only: glows, hit boxes, backdrops) */
static const float PAGE_STRIDE = 4280.0f;   /* (the pause menu's pages along their strips) */

volatile uint32_t hos_wide_ui_force;   /* (read by nx2/oot_wide.py's site in f_8154caa0) */
static volatile int ui_on, ui_custom;
uint32_t hos_gameplay_hud_hidden;
static int hud_minimap_placement = HOS_HUD_DEFAULT_PLACEMENT;

void hos_gameplay_hud_toggle(void) {
    const uint32_t was = __atomic_fetch_xor(&hos_gameplay_hud_hidden, 1u, __ATOMIC_RELAXED);
    hos_log("hud: gameplay HUD %s", was ? "visible" : "hidden");
}

enum { HUD_HEALTH, HUD_ITEMS, HUD_ACTION, HUD_GROUPS };
typedef struct { float scale, x, y; } HudTransform;
static const HudTransform identity = {1.0f, 0.0f, 0.0f};
enum { MAP_UNSCOPED, MAP_MINIMAP, MAP_PAGE };
static _Thread_local uint8_t map_scope;
static _Thread_local HudTransform minimap_transform;
static HudTransform group_plan[HUD_GROUPS];
static atomic_flag group_plan_lock = ATOMIC_FLAG_INIT;

static void lock_group_plan(void) {
    while (atomic_flag_test_and_set_explicit(&group_plan_lock, memory_order_acquire)) {}
}

static void unlock_group_plan(void) { atomic_flag_clear_explicit(&group_plan_lock, memory_order_release); }
static int group_placement[HUD_GROUPS];
static float viewport_x = 1920.0f, viewport_y = 1080.0f;

/* Negative offsets move left or down. Clamp the scaled group to the usable picture whenever it fits. */
static float place_axis(float offset, float scale, float lo, float hi, float half) {
    const float center = (lo + hi) * 0.5f;
    const float half_group = (hi - lo) * 0.5f * scale;
    const float limit = fmaxf(0.0f, half * 0.99f - half_group);
    return fminf(limit, fmaxf(-limit, center + 2.0f * half * offset)) - scale * center;
}

static HudTransform hud_transform(int packed, float x0, float x1, float y0, float y1, float half_x, float half_y) {
    if (packed == HOS_HUD_DEFAULT_PLACEMENT || !isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1)) return identity;
    const float x = (float)((packed & 63) - 16) / 100.0f;
    const float y = (float)(((packed >> 6) & 63) - 16) / 100.0f;
    const float scale = (float)(packed >> 12) / 100.0f;
    if (fabsf(x) > 0.15f || fabsf(y) > 0.15f || scale < 0.75f || scale > 1.25f) return identity;
    return (HudTransform){scale, place_axis(x, scale, x0, x1, half_x), place_axis(y, scale, y0, y1, half_y)};
}

/* The plan and all node fields are shared with the layout threads. */
static unsigned ui_lock_word;
static void ui_lock(void) { while (__atomic_exchange_n(&ui_lock_word, 1, __ATOMIC_ACQUIRE)) {} }
static void ui_unlock(void) { __atomic_store_n(&ui_lock_word, 0, __ATOMIC_RELEASE); }

/* Every 2D screen not kept whole (nx2/oot_wide.py HUD) draws with the UI projection's across and up rows times hos_ui_qx,
   hos_ui_qy (0 before the first present: hos_wide_squeeze across then). The HUD's layouts, by their root layout's name
   (HUD_LAYOUTS), are placed for the screen's shape: ui_ex the canvas units a side pane moves out (less than 0: in, on a
   screen narrower than the HUD's 16:9 canvas), ui_ey the same up and down, ui_wx a spanning pane's width factor. Every
   other layout is a menu's: not moved, and on a screen narrower than 16:9 drawn smaller from its root pane, by ui_fit, to
   fit the screen's width */
volatile float hos_ui_qx, hos_ui_qy;
static volatile float ui_ex, ui_ey, ui_wx = 1.0f, ui_fit = 1.0f;
/* The menus on a wider screen with the Ultrawide menus row at Extended (HOS_OPT_WIDE_MENUS): their layouts are placed as the
   HUD's, ui_mex canvas units a side pane moves out (the screen's whole extra half width), ui_mwx a spanning pane's width factor */
static volatile float ui_mex, ui_mwx = 1.0f;
static volatile int ui_menus, ui_overlay;   /* (ui_overlay: the system overlay's layout planned as a menu's) */
static int menus_fast;   /* (avoid taking the planner lock in menu-only hooks while off) */
static unsigned menu_generation;
/* (the loading fade's pane that spans its canvas, drawn the whole picture: ui_fill_x wider (a wider screen, its layout in the
   middle 16:9), ui_fill_y taller (a narrower one, its layout at the HUD's size). Longshot #82,
   https://github.com/NotRexed/Hookshot/issues/2220: in the centred Ultrawide menus modes the fade covered the middle 16:9
   alone, the world still showing beside it. While its screen is not kept whole (fade_planned) on a narrower screen
   (fade_icon_planned), the rest of its layout, the loading icon, is placed as the HUD's: at the screen's corner, at the HUD's
   size. Left where the 16:9 canvas has it, the icon was cut off at the right there, where the canvas at the HUD's size is
   wider than the screen. On a wider screen it stays in the middle 16:9: the centred modes' blurred or black sides
   (gpu/vk/vk_framegen.cpp wide_sides, fg_scale.comp) cover what is beside it once the world is not drawn, as while loading) */
static volatile float ui_fill_x = 1.0f, ui_fill_y = 1.0f;
static volatile int fade_planned, fade_icon_planned;

/* A pane seen, by its guest address: its parent, kind, and place across the canvas without the moves (x0, x1 its rectangle,
   ox its origin); plan, how far the plan moves it (its own move);
   widen, the plan draws it the screen's width; moved, how far its stored transform was moved, its own and its ancestors'
   together, pushed the side moves among them. Open addressing; released slots are reused. The game's threads write their
   panes; the plan reads them all */
typedef struct {
    uint64_t key, parent, layout, lname, pname;   /* (lname, pname: its layout's name and its own, guest strings) */
    float x0, x1, ox, plan, space, moved, pushed;   /* (space: how far its page moves along a pause menu strip) */
    float y0, y1, oy, plany, movedy, pushedy;   /* (the same up and down: y up, as the canvas) */
    float size_game, size_set;   /* (a list's own width: the game's, and the wider one written over it) */
    float wide;            /* (its drawn width times this: 0 or 1, as the game draws it) */
    float tall;            /* (its drawn height times this, the same) */
    float from;            /* (its parent's ox when it was placed, NAN if its parent was not noted: its place in it) */
    HudTransform placed;   /* (the custom affine transform already inherited by this pane's children) */
    uint32_t type;
    uint8_t custom_group, action_layout, minimap_group;  /* group is zero or HUD_* + 1 */
    uint16_t lkind, rule;  /* (its layout's kind and its own rule, by name: LK_ and RULE_ below) */
    uint32_t in_hud;       /* (its root layout is the HUD's, by name: as the plan last saw it) */
    uint32_t in_menu;      /* (its root layout is a menu's and the menus are placed: Extended) */
    uint8_t hud, whole;    /* (its own layout is one of HUD_LAYOUTS; one drawn over the whole picture, never made smaller) */
    uint8_t fade;          /* (its layout is the loading fade's) */
    uint8_t overlay, map_layout, map_mode, reclassify;
    uint8_t tree;          /* (the panes made from it are noted: a HUD tree's or the loading fade's) */
    uint8_t as_menu;       /* (a HUD root drawn as a menu's: the map page, MapMenu_00 showing it. Set by the plan) */
    uint32_t dead;         /* (its layout let it go: its slot is taken by the next new pane) */
} Node;

static int whole_layout(const Node *n) {
    return n->whole || (n->overlay && !ui_overlay);
}

enum { NODES = 1 << 16 };
static Node nodes[NODES];
static volatile int nodes_used;

static uint32_t node_slot(uint64_t key) { return (uint32_t)((key >> 4) * 0x9E3779B1u) & (NODES - 1); }

static Node *node_find(uint64_t key) {
    for (uint32_t i = node_slot(key), n = 0; n < 64; i = (i + 1) & (NODES - 1), n++) {
        const uint64_t k = __atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE);
        if (k == key) return &nodes[i];
        if (!k) return NULL;
    }
    return NULL;
}

/* (a bit for each slot ever taken since the table was emptied: the plan walks these, not the whole table, whose 8 MB
   the presenting thread otherwise read each present) */
static uint64_t used_bits[NODES / 64];

static void node_fresh(Node *n) { memset((char *)n + sizeof n->key, 0, sizeof *n - sizeof n->key); }

/* A pane let go: drawn as the game makes it, its slot free for another */
static void node_drop(Node *n) {
    n->plan = n->plany = 0.0f, n->wide = n->tall = 1.0f, n->layout = 0;
    __atomic_store_n(&n->dead, 1, __ATOMIC_RELEASE);
}

static int log_each, log_left;
static void guest_copy(char *out, int cap, uint64_t g);

/* The pane's node: its own, else an empty slot or one a let-go pane had (a menu opened again puts its panes at new
   addresses: the old ones' slots are used again, so the table does not fill with them) */
static Node *node_take(uint64_t key) {
    Node *own = node_find(key);
    if (own) {
        if (own->dead) node_fresh(own);
        return own;
    }
    for (uint32_t i = node_slot(key), n = 0; n < 64; i = (i + 1) & (NODES - 1), n++) {
        uint64_t k = __atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE);
        if (!k && __atomic_compare_exchange_n(&nodes[i].key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_or(&used_bits[i / 64], 1ull << (i % 64), __ATOMIC_RELEASE);
            __atomic_add_fetch(&nodes_used, 1, __ATOMIC_RELAXED);
            return &nodes[i];
        }
        if (k && __atomic_load_n(&nodes[i].dead, __ATOMIC_ACQUIRE) &&
            __atomic_compare_exchange_n(&nodes[i].key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            node_fresh(&nodes[i]);
            return &nodes[i];
        }
    }
    static int told;
    if (!told) told = 1, hos_log("wide ui: no slot for pane %llx (64 taken in a row)", (unsigned long long)key);
    return NULL;
}

/* ---- names: the layouts and panes the ultrawide UI romfs mod's rules.py and menu.py treat by hand ---- */

enum { LK_PLAIN, LK_WORLD, LK_WHOLE, LK_HELD, LK_PAGE, LK_CANVAS };   /* (a layout placed by code from the world; on a screen kept
                                                                        whole; held; a pause page: by hand; its own canvas) */
enum { RULE_NONE, RULE_KEEP, RULE_KEEP_LOOK, RULE_WIDEN, RULE_LEFT, RULE_RIGHT, RULE_SLIDE, RULE_KEEP_ORIGIN, RULE_HELD_RIGHT,
       RULE_KEEP_WIDE, RULE_GROW, RULE_KEEP_TAB, RULE_STRIP, RULE_PAGES, RULE_SPLIT };
       /* (RULE_SLIDE: a pane the game slides in from beside its parent, planned where it comes to rest. RULE_HELD_RIGHT: a
          held layout's group at its canvas's right edge, moved in to the screen's right side where the HUD's canvas is
          wider than the screen (ui_ex < 0: 16:10 and narrower), so it keeps 16:9's distance from it; on a wider screen held
          beside its box with the rest. The menus' (Extended): a strip of pages; a pane in one whose children are its pages'
          too; the pages' background, drawn in pieces) */

/* These siblings use one horizontal delta, independent of their visibility or bounds. */
static int live_map_anchor(const Node *n) {
    return n->map_mode == MAP_MINIMAP && (n->minimap_group || n->rule == RULE_KEEP_ORIGIN);
}

static const char *const WORLD_LAYOUTS[] = {"PlayerStatus3D_00", "Pointer_00", "BalloonMessage_00"};
static const char *const PAGE_LAYOUTS[] = {"PaCollectMenu_00", "PaItemMenu_00", "PaEquipmentMenu_00"};
static const char *const CANVAS_LAYOUTS[] = {"PaSystemMenu_00"};
/* (layouts of screens nx2/oot_wide.py keeps whole (KEEP_VTABLES): their canvas already spans the screen, so nothing in
   them moves or widens. The item wheel's background picture widened to 5160 units mapped its capture over 3840 / 5160
   of the screen, black at the sides) */
static const char *const WHOLE_LAYOUTS[] = {"ShortCutBG_00", "EventFade_00"};
/* (held where the game puts them, at the HUD's size: never made smaller, and moved only by name (RULE_HELD_RIGHT). The text
   box and its choices: fitted as a menu's on a narrower screen the choices came out at another scale than the box. The
   choices' column (N_BtnNum_00) reaches the 16:9 canvas's right edge: left there on a 16:10 screen, where the canvas at
   the HUD's size is wider than the screen, the boxes' right ends and their longer texts would be off it) */
static const char *const HELD_LAYOUTS[] = {"MessageDialog_00"};
/* (the HUD's root layouts: the hearts, magic, rupees and buttons; the place's name; the minimap (the pause menu's map page is
   the same layout: drawn as a menu's while it shows, as_menu); the text box and its button guide ("Hide", "Conversation
   Log"), which stands where the minimap was; and the ones placed from the world, never moved. Kept the size of a picture
   over the whole screen: the fades and the system overlay, kept whole by their screens) */
static const char *const HUD_LAYOUTS[] = {"PlayerStatus_00", "Location_00", "MapMenu_00", "Talk_00", "MessageLog_01", "PlayerStatus3D_00",
                                          "Pointer_00", "BalloonMessage_00"};
static const char *const FULL_LAYOUTS[] = {"LoadingFade_00"};
typedef struct { const char *layout, *pane; uint16_t rule; } PaneRule;
static const PaneRule RULES[] = {
    /* (the HUD: the left gauges are moved by name, their group stays where the pause menu places part of it) */
    {"PlayerStatus_00", "N_ScaleL_00", RULE_KEEP_LOOK}, {"PlayerStatus_00", "N_ScaleL_01", RULE_KEEP_LOOK},
    {"PlayerStatus_00", "Pa_HeartGauge_00", RULE_LEFT}, {"PlayerStatus_00", "Pa_MpGauge_00", RULE_LEFT},
    {"PlayerStatus_00", "Pa_AgonyStone_00", RULE_LEFT},
    /* (the pause menu: the page strips and the item preview's panels by hand. The game view's fades while a page changes,
       a capture each and the picture drawing it, kept at their size: a capture's wider transform does not widen what it
       captures, so its picture grown drew the same view 1.8 times as wide) */
    {"PlusMenu_00", "N_MenuChange_00", RULE_STRIP}, {"PlusMenu_00", "N_MenuChange_01", RULE_STRIP},
    {"PlusMenu_00", "N_DreamInfo_00", RULE_PAGES}, {"PlusMenu_00", "P_MenuBg_00", RULE_SPLIT},
    {"PlusMenu_00", "N_3DFade_00", RULE_KEEP}, {"PlusMenu_00", "C_3DFade_00", RULE_KEEP},
    {"PlusMenu_00", "C_3DFade_01", RULE_KEEP}, {"PlusMenu_00", "P_3DFade_01", RULE_KEEP},
    {"PlusMenu_00", "P_3DFade_02", RULE_KEEP}, {"PlusMenu_00", "P_InfoBg_00", RULE_GROW},
    {"PlusMenu_00", "P_InfoBg_01", RULE_GROW}, {"PlusMenu_00", "P_InfoBg_02", RULE_GROW},
    {"PlusMenu_00", "P_InfoBg_03", RULE_RIGHT}, {"PlusMenu_00", "P_MenuBgAmiibo_00", RULE_LEFT},
    {"PlusMenu_00", "P_MenuBgArrow_00", RULE_LEFT}, {"PlusMenu_00", "P_SystemBg_00", RULE_LEFT},
    {"PaCollectMenu_00", "N_ItemAll_00", RULE_RIGHT}, {"PaCollectMenu_00", "N_Arrow_00", RULE_RIGHT},
    {"PaItemMenu_00", "N_ItemAll_00", RULE_LEFT}, {"PaItemMenu_00", "N_Arrow_00", RULE_LEFT},
    {"PaEquipmentMenu_00", "Pa_HeadEquip_00", RULE_LEFT}, {"PaEquipmentMenu_00", "Pa_HeadEquip_01", RULE_LEFT},
    {"PaEquipmentMenu_00", "Pa_HeadEquip_02", RULE_LEFT}, {"PaEquipmentMenu_00", "Pa_HeadEquip_03", RULE_LEFT},
    {"PaEquipmentMenu_00", "Pa_FootEquip_00", RULE_LEFT}, {"PaEquipmentMenu_00", "Pa_FootEquip_01", RULE_LEFT},
    {"PaEquipmentMenu_00", "P_AmiiboIcon_00", RULE_LEFT}, {"PaEquipmentMenu_00", "N_Arrow_00", RULE_LEFT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_00", RULE_RIGHT}, {"PaEquipmentMenu_00", "Pa_Epuipment_01", RULE_RIGHT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_02", RULE_RIGHT}, {"PaEquipmentMenu_00", "Pa_Epuipment_03", RULE_RIGHT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_04", RULE_RIGHT}, {"PaEquipmentMenu_00", "Pa_Epuipment_05", RULE_RIGHT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_06", RULE_RIGHT}, {"PaEquipmentMenu_00", "Pa_Epuipment_07", RULE_RIGHT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_08", RULE_RIGHT}, {"PaEquipmentMenu_00", "N_Arrow_01", RULE_RIGHT},
    /* (the System page's controls sheet, right of the centre: labels on both sides of the controller, its backdrop to the
       canvas's right edge. The mod never looks into a part off the origin: kept whole) */
    {"PaSystemMenu_00", "Pa_SystemController_00", RULE_KEEP},
    /* (the item quick select's carousel: its items scroll through the whole canvas, kept together) */
    {"PaShortCutSlideList_00", "N_SlideList_00", RULE_KEEP_WIDE},   /* (its own width: where its items are cut off) */
    /* (an illustrated board, kept whole; a backdrop and the area name's frame corners to the screen's width) */
    {"ImportantMenu_01", "W_Window_00", RULE_KEEP}, {"ChallengeMenu_01", "P_TapestryMask_00", RULE_WIDEN},
    /* (the Threads of Time and Map pages slide 20 to 80 units as they change: planned at rest, else their vignettes span
       the canvas, and are widened, only at rest) */
    {"ChallengeMenu_01", "N_In_00", RULE_SLIDE},
    {"MapMenu_00", "N_MenuIn_00", RULE_SLIDE}, {"MapMenu_00", "N_InChallenge_00", RULE_SLIDE},
    {"MapMenu_00", "T_MapTitle_00", RULE_KEEP_TAB}, {"MapMenu_00", "P_PicField_00", RULE_KEEP_TAB},
    {"MapMenu_00", "N_Field_01", RULE_KEEP_TAB}, {"MapMenu_00", "Pa_Guide_01", RULE_KEEP_TAB},
    /* (the dungeon map page's floor list at its left and its key, compass and map icons at its right: beside the map, as the title is) */
    {"MapMenu_00", "N_FloorTab_00", RULE_KEEP_TAB}, {"MapMenu_00", "N_ItemIcon_00", RULE_KEEP_TAB},
    /* (the map's icons, placed by code over the map, and the world map, wider than the screen with its location panels on
       it: kept whole with the map under them) */
    /* (MapMenu_00 is also the HUD's minimap: there the icons' group sits over the minimap and moves out with it. At the
       canvas's origin, where the map page puts it, it tells the map page: the layout is then drawn as a menu's (as_menu).
       The icons' part itself is never looked into: its icons move as one with their group) */
    {"MapMenu_00", "N_MapIcon_00", RULE_KEEP_ORIGIN}, {"MapMenu_00", "Pa_MapIcon_00", RULE_KEEP},
    {"MapMenu_00", "Pa_WorldMap_00", RULE_KEEP},
    {"Location_00", "P_DecoFrameLT_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameLTSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameRT_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameRTSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameLB_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameLBSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameRB_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameRBSh_00", RULE_WIDEN},
    /* (the text box's choices: their column, at the canvas's right edge) */
    {"MessageDialog_00", "N_BtnNum_00", RULE_HELD_RIGHT},
};

/* A guest string is s (64 characters at most) */
static int guest_is(uint64_t g, const char *s) {
    if (!g) return 0;
    for (int i = 0; i < 64; i++) {
        const char ch = (char)recomp_load8(NULL, g + (uint64_t)i);
        if (ch != s[i]) return 0;
        if (!ch) return 1;
    }
    return 0;
}

static int guest_in(uint64_t g, const char *const *list, int n) {
    for (int i = 0; i < n; i++)
        if (guest_is(g, list[i])) return 1;
    return 0;
}

/* A pane's layout kind and rule, from its layout's name and its own (the layout file's UIL header) */
static void classify(Node *n, uint64_t pane, uint64_t layout) {
    const uint64_t res = layout ? recomp_load64(NULL, layout + LAYOUT_RES) : 0;
    const uint64_t lname = res ? recomp_load64(NULL, res + RES_NAME) : 0;
    const uint64_t names = res ? recomp_load64(NULL, res + RES_PANE_NAMES) : 0;
    const uint64_t base = layout ? recomp_load64(NULL, layout + LAYOUT_PANES) : 0;
    const uint64_t pname = names && pane >= base ? recomp_load64(NULL, names + (pane - base) / PANE_SIZE * 8) : 0;
#define COUNT(a) (int)(sizeof a / sizeof a[0])
    n->lkind = guest_in(lname, WORLD_LAYOUTS, COUNT(WORLD_LAYOUTS)) ? LK_WORLD
             : guest_in(lname, PAGE_LAYOUTS, COUNT(PAGE_LAYOUTS)) ? LK_PAGE
             : guest_in(lname, CANVAS_LAYOUTS, COUNT(CANVAS_LAYOUTS)) ? LK_CANVAS
             : guest_in(lname, WHOLE_LAYOUTS, COUNT(WHOLE_LAYOUTS)) ? LK_WHOLE
             : guest_in(lname, HELD_LAYOUTS, COUNT(HELD_LAYOUTS)) ? LK_HELD : LK_PLAIN;
    n->lname = lname, n->pname = pname, n->rule = RULE_NONE;
    n->hud = (uint8_t)guest_in(lname, HUD_LAYOUTS, COUNT(HUD_LAYOUTS));
    n->whole = (uint8_t)(n->lkind == LK_WHOLE || guest_in(lname, FULL_LAYOUTS, COUNT(FULL_LAYOUTS)));
    n->overlay = (uint8_t)guest_is(lname, "SystemOverlay_00");
    n->map_layout = (uint8_t)guest_is(lname, "MapMenu_00");
    n->reclassify = 0;
    n->fade = (uint8_t)guest_is(lname, "LoadingFade_00");
    n->custom_group = 0;
    n->action_layout = (uint8_t)guest_is(lname, "DoCommand_00");
    /* (the visible map, markers and frame, including the visible map's scissor. The capture roots are its siblings.) */
    n->minimap_group = (uint8_t)(n->map_layout &&
                                 (guest_is(pname, "N_InOut_00") || guest_is(pname, "Pa_KeySmall_00")));
    if (guest_is(lname, "PlayerStatus_00")) {
        if (guest_is(pname, "Pa_HeartGauge_00") || guest_is(pname, "Pa_MpGauge_00")) n->custom_group = HUD_HEALTH + 1;
        else if (guest_is(pname, "Pa_SelectItem_00")) n->custom_group = HUD_ITEMS + 1;
    } else if (n->action_layout && guest_is(pname, "N_PosType_00")) n->custom_group = HUD_ACTION + 1;
    for (int i = 0; i < COUNT(RULES) && pname; i++)
        if (guest_is(lname, RULES[i].layout) && guest_is(pname, RULES[i].pane)) n->rule = RULES[i].rule;
#undef COUNT
}

/* HOS_WIDE_UI_LOG=<n>: the first n changes of the plan after each change of the shape, to tell what a pane that moves
   wrongly is (log_each, log_left above) */

static void guest_copy(char *out, int cap, uint64_t g) {
    int i = 0;
    for (; g && i < cap - 1; i++)
        if (!(out[i] = (char)recomp_load8(NULL, g + (uint64_t)i))) break;
    out[i] = 0;
}

static void decided(const Node *n, float plan, float widen, float plany) {
    if (log_left <= 0) return;
    log_left--;
    char ln[40], pn[40];
    guest_copy(ln, sizeof ln, n->lname), guest_copy(pn, sizeof pn, n->pname);
    hos_log("wide ui: %s:%s (pane %llx parent %llx, %s) type %u x %.1f [%.1f %.1f] y %.1f [%.1f %.1f] rule %u: %s %.1f, up %.1f, "
            "width x%.3f", ln, pn, (unsigned long long)n->key, (unsigned long long)n->parent,
            n->in_hud ? "HUD" : "menu", n->type, (double)n->ox, (double)n->x0, (double)n->x1, (double)n->oy, (double)n->y0,
            (double)n->y1, n->rule, plan < 0.0f ? "left" : plan > 0.0f ? "right" : "stays", (double)plan, (double)plany,
            (double)widen);
}

/* ---- the plan: the presenting thread, once a present ---- */

static int32_t first_child[NODES], next_sibling[NODES], parent_of[NODES];
static float space_new[NODES];
static float scissor_move[NODES];   /* (a scissor: how far what is in it moves, one way; scissor_mixed: both ways) */
static uint8_t scissor_mixed[NODES];
static float ext_lo[NODES], ext_hi[NODES], plan_new[NODES], wide_new[NODES], tall_new[NODES], plany_new[NODES];
static float ext_ylo[NODES], ext_yhi[NODES];
static uint8_t ext_ydone[NODES], as_menu_new[NODES];
static int32_t used[NODES];   /* (the slots in use as the plan began, used_n of them) */
static int used_n;
#define FOR_USED(i) for (int u_ = 0, i = 0; u_ < used_n && (i = used[u_], 1); u_++)
static float rest[NODES];   /* (how far a pane is from where the plan sees it: its slide, or its page's along a strip) */
static uint8_t ext_done[NODES], rest_done[NODES];

static int drawn(const Node *n) { return n->type != PANE_TYPE_NULL && n->type != PANE_TYPE_PART; }

/* A pane noted is still one of its layout's: a layout let go (a menu closed) leaves its panes noted, and their addresses
   are used again by others */
static int alive(const Node *n) {
    if (!n->layout || recomp_load64(NULL, n->key + PANE_LAYOUT) != n->layout) return 0;
    const uint64_t base = recomp_load64(NULL, n->layout + LAYOUT_PANES);
    const uint32_t count = (uint32_t)recomp_load32(NULL, n->layout + LAYOUT_COUNT) & 0xffff;
    return n->key >= base && (n->key - base) % PANE_SIZE == 0 && (n->key - base) / PANE_SIZE < count;
}

/* A page's own canvas: its root, the pane whose parent is in another layout */
static int canvas_root(int i) {
    const Node *n = &nodes[i];
    return n->lkind == LK_CANVAS && parent_of[i] >= 0 && nodes[parent_of[i]].layout != n->layout;
}

/* How far a pane is from its place at rest: a page's canvas root at the canvas's origin (the strip sliding it in, or a
   page away), a sliding pane where its parent is, what is in them with them. The plan is the same throughout a slide.
   Each pane counts from its parent as they were when it was placed: the game's threads place a frame's panes while the
   plan reads them, and a page root from one frame with its panes from the last put them a frame's slide apart */
static float rest_of(int i, int depth) {
    if (rest_done[i] || depth > 64) return rest[i];
    const Node *n = &nodes[i];
    const int p = parent_of[i];
    float at = n->ox;   /* (its place at rest) */
    if (canvas_root(i)) at = 0.0f;
    else if (p >= 0) {
        const float up = nodes[p].ox - rest_of(p, depth + 1);
        at = n->rule == RULE_SLIDE ? up : up + n->ox - (isnan(n->from) ? nodes[p].ox : n->from);
    }
    rest[i] = n->ox - at;
    rest_done[i] = 1;
    return rest[i];
}

/* How far a pane and what is in it reach across: its origin, its own rectangle when it is drawn and not wider than
   SIZE_CAP, and its children's (as rules.py Extents) */
static void extents(int i, int depth) {
    if (ext_done[i]) return;
    const Node *n = &nodes[i];
    const float r = rest[i];
    float lo = n->ox - r, hi = n->ox - r;
    if (drawn(n) && n->x1 - n->x0 < SIZE_CAP) lo = fminf(lo, n->x0 - r), hi = fmaxf(hi, n->x1 - r);
    if (depth < 48)
        for (int c = first_child[i]; c >= 0; c = next_sibling[c]) {
            extents(c, depth + 1);
            lo = fminf(lo, ext_lo[c]), hi = fmaxf(hi, ext_hi[c]);
        }
    ext_lo[i] = lo, ext_hi[i] = hi, ext_done[i] = 1;
}

static void visit(int p, int depth);

/* The layouts showing their map page: one of their RULE_KEEP_ORIGIN panes at the canvas's origin. Their root is drawn as a
   menu's (as_menu): not moved, and on a narrower screen fitted with the rest of the pause menu */
enum { TAB_LAYOUTS = 16 };
static uint64_t tab_layouts[TAB_LAYOUTS];
static int tab_count;

static int in_tab(uint64_t layout) {
    for (int i = 0; i < tab_count; i++)
        if (tab_layouts[i] == layout) return 1;
    return 0;
}

static void push(int i, int side) { plan_new[i] = (float)side * (nodes[i].in_menu ? ui_mex : ui_ex); }

/* A pane spanning the canvas: drawn the screen's width; what is in it is placed as usual */
static void widen(int i, int depth) {
    const float mid = (nodes[i].x0 + nodes[i].x1) * 0.5f - rest[i];
    if (drawn(&nodes[i]) && mid * mid < 1.0f) wide_new[i] = nodes[i].in_menu ? ui_mwx : ui_wx;
    visit(i, depth + 1);
}

/* rules.py plan_layout's visit: each child of p by how far it and its contents reach */
static void visit(int p, int depth) {
    if (depth > 48) return;
    const float E = CANVAS_HALF;
    int group[2][64], count[2] = {0, 0};
    float glo[2] = {INFINITY, INFINITY}, ghi[2] = {-INFINITY, -INFINITY};
    for (int c = first_child[p]; c >= 0; c = next_sibling[c]) {
        const Node *n = &nodes[c];
        if (live_map_anchor(n)) continue;
        const float ox = n->ox - rest[c], x0 = n->x0 - rest[c], x1 = n->x1 - rest[c];
        if (n->rule == RULE_KEEP_WIDE) wide_new[c] = ui_mwx;   /* (only in a menu) */
        if ((n->rule == RULE_KEEP_ORIGIN && n->in_menu && fabsf(ox) < 1.0f) || (n->rule == RULE_KEEP_TAB && (n->map_mode == MAP_PAGE ||
                (n->map_mode == MAP_UNSCOPED && in_tab(n->layout)))))
            continue;   /* (the map page's: kept over the map) */
        if (n->lkind == LK_WORLD || n->lkind == LK_WHOLE || n->lkind == LK_HELD || n->lkind == LK_PAGE || n->rule == RULE_KEEP ||
            n->rule == RULE_KEEP_WIDE || n->rule == RULE_GROW || n->rule == RULE_LEFT || n->rule == RULE_RIGHT ||
            n->rule == RULE_STRIP || n->rule == RULE_PAGES || n->rule == RULE_SPLIT)
            continue;   /* (placed by code, by hand, or by name: below) */
        if (n->rule == RULE_KEEP_LOOK) {
            visit(c, depth + 1);
            continue;
        }
        if (n->rule == RULE_WIDEN) {
            widen(c, depth);
            continue;
        }
        if (n->type == PANE_TYPE_PART && fabsf(ox) < 1.0f) {   /* (a part at the canvas's origin: its own layout's panes) */
            visit(c, depth + 1);
            continue;
        }
        extents(c, 0);
        float lo = ext_lo[c], hi = ext_hi[c];
        if (drawn(n)) {
            if (x0 <= -E + EDGE_TOL && x1 >= E - EDGE_TOL) {
                widen(c, depth);
                continue;
            }
            if (first_child[c] < 0) lo = fminf(lo, x0), hi = fmaxf(hi, x1);
        }
        if (lo >= E || hi <= -E || (hi - lo > 2.5f * E && fabsf(ox) > 1.0f)) continue;   /* (off the canvas, a strip) */
        lo = fmaxf(lo, -E - EDGE_NEAR), hi = fminf(hi, E + EDGE_NEAR);
        if (lo <= -E + EDGE_TOL && hi >= E - EDGE_TOL) {
            visit(c, depth + 1);
            continue;
        }
        if (lo < 0.0f && hi > 0.0f && fminf(-lo, hi) > CROSS * (hi - lo)) {
            const int near_hi = hi >= E - EDGE_NEAR, near_lo = lo <= -E + EDGE_NEAR;
            if (near_hi != near_lo && (first_child[c] < 0 || hi - lo < E)) push(c, near_hi ? 1 : -1);
            else visit(c, depth + 1);
            continue;
        }
        if (lo == 0.0f || hi == 0.0f ||
            (lo < 0.0f && hi > 0.0f && (fabsf(lo + hi) * 0.5f < EDGE_NEAR || (first_child[c] >= 0 && fabsf(ox) < 1.0f)))) {
            visit(c, depth + 1);   /* (a centred container) */
            continue;
        }
        const int s = lo + hi >= 0.0f;
        if (count[s] < 64) group[s][count[s]++] = c;
        glo[s] = fminf(glo[s], lo), ghi[s] = fmaxf(ghi[s], hi);
    }
    for (int s = 0; s < 2; s++) {
        if (!count[s]) continue;
        const float outer = s ? ghi[s] : -glo[s];
        const int out = outer >= E - EDGE_NEAR || fabsf(glo[s] + ghi[s]) * 0.5f >= SIDE;
        for (int k = 0; k < count[s]; k++) {
            if (out) push(group[s][k], s ? 1 : -1);
            else visit(group[s][k], depth + 1);
        }
    }
}

/* How far a pane and what is in it reach up and down (as extents across, without the slides: those are across) */
static void extents_y(int i, int depth) {
    if (ext_ydone[i]) return;
    const Node *n = &nodes[i];
    float lo = n->oy, hi = n->oy;
    if (drawn(n) && n->y1 - n->y0 < SIZE_CAP) lo = fminf(lo, n->y0), hi = fmaxf(hi, n->y1);
    if (depth < 48)
        for (int c = first_child[i]; c >= 0; c = next_sibling[c]) {
            extents_y(c, depth + 1);
            lo = fminf(lo, ext_ylo[c]), hi = fmaxf(hi, ext_yhi[c]);
        }
    ext_ylo[i] = lo, ext_yhi[i] = hi, ext_ydone[i] = 1;
}

/* Up and down, for a HUD drawn smaller than its 16:9 size (a screen too narrow for it at that size: hud_scale): what sits at
   the top or bottom of the canvas, or a group far up or down, moves out to the screen's top or bottom; a container across
   the middle is looked into */
static void visit_y(int p, int depth) {
    if (depth > 48) return;
    const float E = CANVAS_HALF_Y;
    for (int c = first_child[p]; c >= 0; c = next_sibling[c]) {
        const Node *n = &nodes[c];
        if (n->lkind != LK_PLAIN || n->rule == RULE_KEEP) continue;
        if (n->type == PANE_TYPE_PART && fabsf(n->oy) < 1.0f) {
            visit_y(c, depth + 1);
            continue;
        }
        extents_y(c, 0);
        if (ext_ylo[c] >= E || ext_yhi[c] <= -E) continue;   /* (off the canvas) */
        const float lo = fmaxf(ext_ylo[c], -E - EDGE_NEAR), hi = fminf(ext_yhi[c], E + EDGE_NEAR);
        if ((lo <= -E + EDGE_TOL && hi >= E - EDGE_TOL) || (lo < 0.0f && hi > 0.0f && fminf(-lo, hi) > CROSS * (hi - lo))) {
            visit_y(c, depth + 1);
            continue;
        }
        const int up = lo + hi >= 0.0f;
        if ((up ? hi : -lo) >= E - EDGE_NEAR || fabsf(lo + hi) * 0.5f >= SIDE_Y) plany_new[c] = (up ? 1.0f : -1.0f) * ui_ey;
        else visit_y(c, depth + 1);
    }
}

static int custom_groups(void) {
    HudTransform next[HUD_GROUPS];
    float left[HUD_GROUPS], right[HUD_GROUPS], bottom[HUD_GROUPS], top[HUD_GROUPS];
    for (int g = 0; g < HUD_GROUPS; g++) {
        left[g] = bottom[g] = INFINITY;
        right[g] = top[g] = -INFINITY;
    }
    FOR_USED(i) {
        const Node *n = &nodes[i];
        if (!n->key || !n->layout || !n->custom_group) continue;
        const int g = n->custom_group - 1;
        if (group_placement[g] == HOS_HUD_DEFAULT_PLACEMENT) continue;
        extents(i, 0);
        extents_y(i, 0);
        float dx = 0.0f, dy = 0.0f;
        for (int a = i, d = 0; a >= 0 && d < 64; a = parent_of[a], d++) {
            if (plan_new[a] != 0.0f) dx = plan_new[a];
            if (plany_new[a] != 0.0f) dy = plany_new[a];
        }
        left[g] = fminf(left[g], ext_lo[i] + dx);
        right[g] = fmaxf(right[g], ext_hi[i] + dx);
        bottom[g] = fminf(bottom[g], ext_ylo[i] + dy);
        top[g] = fmaxf(top[g], ext_yhi[i] + dy);
    }
    for (int g = 0; g < HUD_GROUPS; g++)
        next[g] = hud_transform(group_placement[g], left[g], right[g], bottom[g], top[g], viewport_x, viewport_y);
    int changed = 0;
    lock_group_plan();
    for (int g = 0; g < HUD_GROUPS; g++) {
        /* (sub-pixel animation in the bounds needn't dirty every layout on every present) */
        HudTransform *old = &group_plan[g];
        if (next[g].scale != old->scale || fabsf(next[g].x - old->x) >= 0.5f || fabsf(next[g].y - old->y) >= 0.5f) {
            *old = next[g];
            changed = 1;
        }
    }
    unlock_group_plan();
    return changed;
}

static void plan(int custom) {
    const int on = ui_on;
    /* (the slots in use, once: every step walks only them, and only their entries are reset; walking the whole table
       took about 0.7 ms a present on the presenting thread) */
    used_n = 0;
    for (int w = 0; w < NODES / 64; w++)
        for (uint64_t b = __atomic_load_n(&used_bits[w], __ATOMIC_ACQUIRE); b; b &= b - 1) {
            const int i = w * 64 + __builtin_ctzll(b);
            if (__atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE)) used[used_n++] = i;
        }
    FOR_USED(i) {
        first_child[i] = next_sibling[i] = parent_of[i] = -1;
        ext_done[i] = rest_done[i] = ext_ydone[i] = as_menu_new[i] = 0;
        scissor_move[i] = 0.0f, scissor_mixed[i] = 0;
        rest[i] = plan_new[i] = plany_new[i] = space_new[i] = 0.0f, wide_new[i] = tall_new[i] = 1.0f;
    }
    if (on) {
        FOR_USED(i) {
            if (!alive(&nodes[i])) {
                nodes[i].layout = 0;   /* (gone: not part of any tree; its slot free for another pane) */
                __atomic_store_n(&nodes[i].dead, 1, __ATOMIC_RELEASE);
                continue;
            }
            const Node *pn = nodes[i].parent ? node_find(nodes[i].parent) : NULL;
            const int p = pn && pn->layout ? (int)(pn - nodes) : -1;
            if (p < 0 || p == i) continue;
            parent_of[i] = p, next_sibling[i] = first_child[p], first_child[p] = i;
        }
        FOR_USED(i)
            if (nodes[i].key && nodes[i].layout) rest_of(i, 0);
        tab_count = 0;
        FOR_USED(i)
            if (nodes[i].key && nodes[i].layout && nodes[i].map_mode == MAP_UNSCOPED &&
                nodes[i].rule == RULE_KEEP_ORIGIN && fabsf(nodes[i].ox - rest[i]) < 1.0f &&
                !in_tab(nodes[i].layout) && tab_count < TAB_LAYOUTS)
                tab_layouts[tab_count++] = nodes[i].layout;
        FOR_USED(i) {   /* (a HUD's pane: its root layout is one of HUD_LAYOUTS, not showing the map page) */
            if (!nodes[i].key || !nodes[i].layout) continue;
            int top = i;
            for (int d = 0; parent_of[top] >= 0 && d < 64; d++) top = parent_of[top];
            const Node *root = &nodes[top];
            as_menu_new[top] = (uint8_t)(root->map_layout && (root->map_mode == MAP_PAGE ||
                (root->map_mode == MAP_UNSCOPED && in_tab(root->layout))));
            nodes[i].in_hud = (nodes[top].hud && !as_menu_new[top]) || (nodes[top].fade && fade_icon_planned);
            /* (a menu's pane, Extended: its root is not the HUD's, held, placed from the world or kept whole. The loading fade's layout is: its
               screen is squeezed with the menus, its icon goes to the screen's corner and its cover spans the screen) */
            nodes[i].in_menu = ui_menus && !nodes[i].in_hud && (!whole_layout(root) || root->fade) &&
                               nodes[top].lkind != LK_HELD && nodes[top].lkind != LK_WORLD && (!nodes[top].hud || as_menu_new[top]);
        }
        /* (the roots: a HUD layout's root pane, each walked as the canvas's root; the map page's, drawn as a menu's, is
           not, unless the menus are placed (Extended), as the pause menu's) */
        FOR_USED(i)
            if (nodes[i].key && nodes[i].layout && (nodes[i].in_hud || nodes[i].in_menu) && parent_of[i] < 0 &&
                nodes[i].lkind != LK_WORLD && nodes[i].lkind != LK_WHOLE) {
                visit(i, 0);
                if (ui_ey != 0.0f && nodes[i].in_hud) visit_y(i, 0);
            }
        /* (a page's own canvas, from its root) */
        if (ui_menus) FOR_USED(i)
            if (nodes[i].key && nodes[i].layout && nodes[i].in_menu && canvas_root(i)) visit(i, 0);
        /* (by name: moved out, unless an ancestor already is; preview panels grown by the screen's extra width) */
        FOR_USED(i) {
            const Node *n = &nodes[i];
            if (!n->key || !n->layout || !(n->in_hud || n->in_menu)) continue;
            if (n->rule == RULE_GROW && n->x1 > n->x0) wide_new[i] = (n->x1 - n->x0 + 2.0f * ui_mex) / (n->x1 - n->x0);
            if (n->rule != RULE_LEFT && n->rule != RULE_RIGHT) continue;
            int moved = 0;
            for (int a = parent_of[i], d = 0; a >= 0 && d < 64 && !moved; a = parent_of[a], d++) moved = plan_new[a] != 0.0f;
            if (!moved) push(i, n->rule == RULE_LEFT ? -1 : 1);
        }
        /* (a scissor clips what is in it: it moves with its contents, when they move out one way, else they are cut off at its
           old edge. What is made from it is then moved with it, the contents' own moves left out at their placing) */
        FOR_USED(i) {
            if (plan_new[i] == 0.0f) continue;
            for (int a = parent_of[i], d = 0; a >= 0 && d < 64; a = parent_of[a], d++) {
                if (plan_new[a] != 0.0f) break;
                if (nodes[a].type != PANE_TYPE_SCISSOR) continue;
                if (scissor_move[a] == 0.0f) scissor_move[a] = plan_new[i];
                else if (scissor_move[a] != plan_new[i]) scissor_mixed[a] = 1;
                break;
            }
        }
        FOR_USED(i)
            if (scissor_move[i] != 0.0f && !scissor_mixed[i] && nodes[i].layout && (nodes[i].in_hud || nodes[i].in_menu)) plan_new[i] = scissor_move[i];
        /* (the pause menu's pages spaced wider: a pane along a strip, or in a pane of one whose children are pages' too, by
           its page, the nearest to its place along the strip, as menu.py plan_plus_menu) */
        if (ui_menus) FOR_USED(i) {
            const int p = parent_of[i];
            if (!nodes[i].key || !nodes[i].layout || !nodes[i].in_menu || p < 0) continue;
            const int s = nodes[p].rule == RULE_STRIP ? p
                        : nodes[p].rule == RULE_PAGES && parent_of[p] >= 0 && nodes[parent_of[p]].rule == RULE_STRIP ? parent_of[p] : -1;
            if (s >= 0) space_new[i] = roundf((nodes[i].ox - nodes[s].ox) / PAGE_STRIDE) * 2.0f * ui_mex;
        }
        /* (a held layout's group at the canvas's right edge: in to the screen's side only where that is inside the canvas) */
        if (ui_ex < 0.0f)
            FOR_USED(i)
                if (nodes[i].key && nodes[i].layout && nodes[i].lkind == LK_HELD && nodes[i].rule == RULE_HELD_RIGHT) push(i, 1);
        /* (the loading fade: its pane across the whole canvas, drawn over the whole picture. Kept whole (Full width), the
           game's projection already stretches it over the picture: left as it is) */
        FOR_USED(i) {
            const Node *n = &nodes[i];
            if (fade_planned && n->key && n->layout && n->fade && drawn(n) && n->x0 <= -CANVAS_HALF + EDGE_TOL && n->x1 >= CANVAS_HALF - EDGE_TOL)
                wide_new[i] = ui_fill_x, tall_new[i] = ui_fill_y;
        }
    }
    int changed = custom && custom_groups();
    FOR_USED(i) {
        Node *n = &nodes[i];
        if (!n->key || (n->plan == plan_new[i] && n->space == space_new[i] && n->wide == wide_new[i] && n->tall == tall_new[i] &&
                        n->plany == plany_new[i] && n->as_menu == as_menu_new[i]))
            continue;
        decided(n, plan_new[i], wide_new[i], plany_new[i]);
        n->plan = plan_new[i], n->space = space_new[i], n->wide = wide_new[i], n->tall = tall_new[i], n->plany = plany_new[i], n->as_menu = as_menu_new[i];
        changed = 1;
    }
    if (changed && hos_wide_ui_force < 2) hos_wide_ui_force = 2;
}

/* The HUD's layout for the picture's shape (hos_wide_ratio: 0 at 16:9, the game's own). G is 16:9, r the picture's ratio.

   - The HUD keeps its 16:9 size, the size the game made it for: scaled with the picture's height as at 16:9, never
     stretched. What sits at a side or corner of its 16:9 canvas moves out to the screen's side (a wider screen) or in to it
     (a narrower one), as a layout made for that shape would place it; what spans the canvas is drawn the screen's width.
   - Wider than HUD_MAX (12:5, the widest "21:9" screens: 3840x1600) the side moves stop there, so on a 32:9 screen the
     hearts and the minimap stay where the eyes are; at 48:9, three 16:9 screens, the HUD stays on the middle one, as before.
   - Narrower than HUD_FULL (3:2, a 4:3 or 5:4 screen) the 16:9 HUD would take too much of the screen's width: it is drawn
     smaller, by the picture's ratio over HUD_FULL, and what sits at the top or bottom moves out to the screen's top or bottom.
   - The menus (every layout whose root is not the HUD's) keep their 16:9 shape as before on a wider screen (the Ultrawide
     menus row: in the middle, or stretched); on a narrower one they fit its width, in the middle, never stretched.
     The HUD layout row at Middle 16:9, or HOS_WIDE_UI=0 over it, turns the HUD's moves off.

   HOS_HUD_MAX_RATIO=<ratio> (0: the screen's edges at any width) and HOS_HUD_FULL_RATIO=<ratio> stand over the two limits. */
static const float GAME = 16.0f / 9.0f, HUD_MAX = 12.0f / 5.0f, HUD_FULL = 1.5f, SURROUND = 48.0f / 9.0f;

static float env_ratio(const char *name, float otherwise) {
    const char *e = getenv(name);
    return e && *e ? (float)atof(e) : otherwise;
}

/* The HUD's width reach (the ratio its side panes reach out to) on a picture of ratio r */
float hos_hud_reach(float r, float max) {
    if (r >= SURROUND * 0.99f && max == HUD_MAX) return GAME;   /* (three screens: the middle one's) */
    return max > 0.0f && r > max ? (max > GAME ? max : GAME) : r;
}

/* The HUD's size against 16:9's (1: the same) on a picture of ratio r */
float hos_hud_scale(float r, float full) { return r > 0.0f && full > 0.0f && r < full ? r / full : 1.0f; }

/* Each present (host/rx_backend.c host_wide_menus): the scales and moves for the picture's shape, and the plan */
void hos_wide_ui_tick(void) {
    static const char *env;
    static int looked, keep_forced;
    static float hud_max, hud_full, last[13];
    static int last_map_placement = HOS_HUD_DEFAULT_PLACEMENT;
    if (!looked) {
        looked = 1, env = getenv("HOS_WIDE_UI"), keep_forced = getenv("HOS_WIDE_KEEP") != NULL;
        const char *e = getenv("HOS_WIDE_UI_LOG");
        log_each = e ? atoi(e) : 0;
        hud_max = env_ratio("HOS_HUD_MAX_RATIO", HUD_MAX), hud_full = env_ratio("HOS_HUD_FULL_RATIO", HUD_FULL);
        for (int i = 0; i < 13; i++) last[i] = NAN;
    }
    ui_lock();
    const float r = hos_wide_ratio;
    /* (the HUD's moves: HOS_WIDE_UI when set, else the Insert menu's HUD layout, read at each present so a change shows at
       once: 0 Screen's edges, 1 Middle 16:9, HOS_OPT_HUD_LAYOUT) */
    int rx_option(int id, int otherwise);
    const int layout = env && *env ? (env_value_off(env) ? 1 : 0) : rx_option(HOS_OPT_HUD_LAYOUT, 0);
    const int custom = layout == 2;
    const int hud = layout != 1;
    float qx = 0.0f, qy = 0.0f, fit = 1.0f, ex = 0.0f, ey = 0.0f;
    if (r > GAME * 1.001f) {   /* (wider: the middle 16:9 at the height's scale, as before) */
        qx = GAME / r, qy = 1.0f;
        if (hud) ex = CANVAS_HALF * (hos_hud_reach(r, hud_max) / GAME - 1.0f);
    } else if (r > 0.0f && r < GAME * 0.999f) {   /* (narrower: never stretched) */
        const float k = hud ? hos_hud_scale(r, hud_full) : r / GAME;   /* (the HUD's size: off, the width's, as the menus) */
        qx = k * GAME / r, qy = k, fit = r / GAME / k;
        if (hud) ex = CANVAS_HALF * (r / (k * GAME) - 1.0f), ey = CANVAS_HALF_Y * (1.0f / k - 1.0f);
    }
    /* (the Ultrawide menus row at Extended, on a wider screen: the menus placed as the HUD is, by the whole extra width;
       HOS_WIDE_UI is about the HUD's moves only) */
    const int menus = r > GAME * 1.001f && rx_option(HOS_OPT_WIDE_MENUS, 0) == HOS_WIDE_MENUS_EXTENDED;
    const float mex = menus ? CANVAS_HALF * (r / GAME - 1.0f) : 0.0f;
    /* (bit 4, the system overlay's screen, is squeezed with the others where the menus are placed; a mask forced by
       HOS_WIDE_KEEP stands, and the overlay's layout is planned as its bit 4 says) */
    extern volatile uint32_t hos_wide_keep_mask;
    if (menus && !keep_forced) hos_wide_keep_mask &= ~0x10u;
    const int overlay = menus && !(hos_wide_keep_mask & 0x10u);
    hos_ui_qx = qx, hos_ui_qy = qy;
    const float fill_x = qx > 0.0f && qx < 1.0f ? 1.0f / qx : 1.0f, fill_y = qy > 0.0f && qy < 1.0f ? 1.0f / qy : 1.0f;
    /* (bit 3: the loading fade's screen kept whole, the Full width row) */
    const int fade_free = !(hos_wide_keep_mask & 8u);
    if (custom) {
        viewport_x = qx > 0.0f ? CANVAS_HALF / qx : CANVAS_HALF;
        viewport_y = qy > 0.0f ? CANVAS_HALF_Y / qy : CANVAS_HALF_Y;
        group_placement[HUD_HEALTH] = rx_option(HOS_OPT_HUD_HEALTH, HOS_HUD_DEFAULT_PLACEMENT);
        group_placement[HUD_ITEMS] = group_placement[HUD_ACTION] = rx_option(HOS_OPT_HUD_BUTTONS, HOS_HUD_DEFAULT_PLACEMENT);
    }
    const int map_placement = custom ? rx_option(HOS_OPT_HUD_MINIMAP, HOS_HUD_DEFAULT_PLACEMENT) : HOS_HUD_DEFAULT_PLACEMENT;
    __atomic_store_n(&hud_minimap_placement, map_placement, __ATOMIC_RELAXED);
    const int place_groups = custom && (group_placement[HUD_HEALTH] != HOS_HUD_DEFAULT_PLACEMENT ||
                                        group_placement[HUD_ITEMS] != HOS_HUD_DEFAULT_PLACEMENT);
    const int transform = place_groups || map_placement != HOS_HUD_DEFAULT_PLACEMENT;
    const int on = transform || ex != 0.0f || ey != 0.0f || fit != 1.0f || fill_x != 1.0f || fill_y != 1.0f || menus;
    const float now[13] = {ex, ey, fit, (float)on, r, fill_x, fill_y, (float)fade_free, (float)transform, (float)place_groups,
                           (float)menus, mex, (float)overlay};
    if (memcmp(now, last, sizeof now)) {
        if (ui_menus != menus) menu_generation++;
        ui_ex = ex, ui_ey = ey, ui_wx = 1.0f + ex / CANVAS_HALF, ui_fit = fit, ui_fill_x = fill_x, ui_fill_y = fill_y, ui_mex = mex, ui_mwx = 1.0f + mex / CANVAS_HALF, ui_menus = menus, ui_overlay = overlay, ui_on = on;
        __atomic_store_n(&menus_fast, menus, __ATOMIC_RELEASE);
        fade_planned = fade_free, fade_icon_planned = fade_free && r > 0.0f && r < GAME * 0.999f;   /* (narrower only) */
        lock_group_plan();
        for (int g = 0; g < HUD_GROUPS; g++) group_plan[g] = identity;
        unlock_group_plan();
        ui_custom = transform;
        /* Keep list bookkeeping until its layout thread can restore the width. */
        for (int i = 0; i < NODES; i++) {
            Node *n = &nodes[i];
            n->plan = n->space = n->plany = 0.0f;
            n->wide = n->tall = 1.0f;
            n->in_menu = n->in_hud = n->as_menu = 0;
            n->reclassify = 1;
        }
        hos_wide_ui_force = 3;   /* (a few frames: the layouts not drawn this frame catch up when they are) */
        log_left = log_each;
        hos_log("wide ui: picture %.4f: the HUD at %.0f%% of its 16:9 size, its sides moved %.0f and its top and bottom %.0f canvas "
                "units; the menus at %.0f%%", (double)r, (double)(qy > 0.0f ? qy * 100.0f : 100.0f), (double)ex, (double)ey,
                (double)(qy > 0.0f ? qy * fit * 100.0f : 100.0f));
        memcpy(last, now, sizeof last);
    } else if (hos_wide_ui_force) {
        hos_wide_ui_force--;
    }
    if (map_placement != last_map_placement) {
        last_map_placement = map_placement;
        hos_wide_ui_force = 3;
    }
    if (!on) { ui_unlock(); return; }
    const int64_t t0 = log_each > 0 ? hos_now_ns() : 0;
    plan(place_groups);
    if (log_each > 0) {   /* (the plan's time a present, every 600 presents) */
        static int64_t spent;
        static int count;
        spent += hos_now_ns() - t0;
        if (++count == 600) {
            hos_log("wide ui: the plan took %.3f ms a present over the last 600", (double)spent / 600.0 / 1e6);
            spent = 0, count = 0;
        }
    }
    ui_unlock();
}

/* ---- the game's threads ---- */

static _Thread_local uint64_t ui_parent;

void hos_wide_ui_parent(uint64_t parent) { ui_parent = parent; }

static float load_f(Ctx *c, uint64_t a) {
    const uint32_t u = (uint32_t)recomp_load32(c, a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void store_f(Ctx *c, uint64_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    recomp_store32(c, a, u);
}

/* Only the layout thread touches the live width and dirty flags. */
static void restore_width(Ctx *c, Node *n, uint64_t pane) {
    if (!n || !n->size_set) return;
    if (n->layout == recomp_load64(c, pane + PANE_LAYOUT) && load_f(c, pane + PANE_W) == n->size_set) {
        store_f(c, pane + PANE_W, n->size_game);
        recomp_store32(c, pane, (uint32_t)recomp_load32(c, pane) | PANE_DIRTY_RECT);
    }
    n->size_set = 0.0f;
}

void hos_wide_ui_placed(Ctx *c, uint64_t pane) {
    const uint64_t parent = ui_parent;
    ui_parent = 0;
    ui_lock();
    Node *previous = node_find(pane);
    if (previous && (!ui_menus || !previous->in_menu || previous->wide <= 1.0f)) restore_width(c, previous, pane);
    if (!ui_on) { ui_unlock(); return; }   /* (off: the transforms are the game's) */
    const Node *pn = parent ? node_find(parent) : NULL;
    if (parent && !(pn && pn->layout && !pn->dead && pn->tree)) {   /* (a menu's pane, or a HUD part in one: not noted. A pane
                                                                       noted before at its address no longer counts) */
        Node *old = node_find(pane);
        if (old && old->layout) { restore_width(c, old, pane); node_drop(old); }
        ui_unlock(); return;
    }
    Node *n = node_take(pane);
    if (!n) { ui_unlock(); return; }
    const float inherited = pn ? pn->moved : 0.0f, pushed = pn ? pn->pushed : 0.0f;
    const float inherited_y = pn ? pn->movedy : 0.0f, pushed_y = pn ? pn->pushedy : 0.0f;
    const uint32_t flags = (uint32_t)recomp_load32(c, pane + PANE_FLAGS);
    const int custom = ui_custom;
    const HudTransform placed = custom && pn && pn->placed.scale > 0.0f ? pn->placed : identity;
    const float stored_x = load_f(c, pane + PANE_X), stored_y = load_f(c, pane + PANE_Y);
    const float inverse_scale = placed.scale != 1.0f ? 1.0f / placed.scale : 1.0f;
    const float x = (stored_x - placed.x) * inverse_scale - inherited;
    const float y = (stored_y - placed.y) * inverse_scale - inherited_y;
    const float w = load_f(c, pane + PANE_W) * fabsf(load_f(c, pane + PANE_XX)) * inverse_scale;
    const float h = load_f(c, pane + PANE_H) * fabsf(load_f(c, pane + PANE_YY)) * inverse_scale;
    const uint32_t ox = ((flags >> 11) & 0x7f) % 3;   /* (0 left, 1 centre, 2 right: f_8154a820) */
    const uint32_t oy = ((flags >> 11) & 0x7f) / 3 % 3;   /* (0 top, 1 centre, 2 bottom) */
    const uint64_t layout = recomp_load64(c, pane + PANE_LAYOUT);
    const int layout_changed = n->layout != layout;
    if (layout_changed || n->reclassify) classify(n, pane, layout);
    const uint8_t mode = n->map_layout && map_scope ? map_scope
                       : pn ? pn->map_mode
                       : n->map_layout && !layout_changed ? n->map_mode : MAP_UNSCOPED;
    if (mode != n->map_mode) {
        /* The same pane can be drawn by the other map before the next present. */
        restore_width(c, n, pane);
        n->map_mode = mode;
        n->plan = n->space = n->plany = 0.0f;
        n->wide = n->tall = 1.0f;
        n->in_menu = n->in_hud = n->as_menu = 0;
    }
    /* (a held layout's panes too, for its groups moved by name; the plan walks none of them) */
    /* (Extended: a menu's root too, and so every pane made from it) */
    n->tree = (uint8_t)(((n->lkind == LK_PLAIN || n->lkind == LK_PAGE || n->lkind == LK_CANVAS) &&
                         (parent || n->hud || n->fade || (custom && n->action_layout) || (ui_menus && !whole_layout(n)))) || n->lkind == LK_HELD);
    n->parent = parent, n->layout = layout, n->type = (flags >> 22) & 0xf, n->ox = x, n->from = pn ? pn->ox : NAN;
    n->x0 = x - (ox == 1 ? w * 0.5f : ox == 2 ? w : 0.0f), n->x1 = n->x0 + w;
    n->oy = y, n->y1 = y + (oy == 1 ? h * 0.5f : oy == 2 ? h : 0.0f), n->y0 = n->y1 - h;
    const float side = pushed != 0.0f ? 0.0f : live_map_anchor(n) ? ui_ex : n->plan;   /* (made from a parent moved out: already moved with it) */
    float own = side + n->space;
    if (n->rule == RULE_STRIP && parent)   /* (a strip: the page it shows, or between, at the wider spacing) */
        own += (x + inherited - load_f(c, parent + PANE_X)) * (2.0f * ui_mex / PAGE_STRIDE);
    n->moved = inherited + own, n->pushed = pushed + side;
    if (own != 0.0f) store_f(c, pane + PANE_X, stored_x + own * placed.scale);
    if (n->in_menu && n->wide > 1.0f && (n->type == PANE_TYPE_HOST || n->type == PANE_TYPE_SCISSOR)) {   /* (a menu's list or scissor: its own width,
                                                                                         which what is in it is cut off at) */
        const float now = load_f(c, pane + PANE_W);
        if (now != n->size_set) n->size_game = now;
        n->size_set = n->size_game * n->wide;
        if (now != n->size_set) {
            store_f(c, pane + PANE_W, n->size_set);
            recomp_store32(c, pane, (uint32_t)recomp_load32(c, pane) | PANE_DIRTY_RECT);
        }
    }
    const float up = pushed_y != 0.0f ? 0.0f : n->plany;
    n->movedy = inherited_y + up, n->pushedy = pushed_y + up;
    if (up != 0.0f) store_f(c, pane + PANE_Y, stored_y + up * placed.scale);
    n->placed = placed;
    if (custom && (n->custom_group || n->minimap_group)) {
        HudTransform adjust = minimap_transform;
        if (n->custom_group) {
            lock_group_plan();
            adjust = group_plan[n->custom_group - 1];
            unlock_group_plan();
        }
        if (adjust.scale > 0.0f && (adjust.scale != 1.0f || adjust.x != 0.0f || adjust.y != 0.0f)) {
            for (int k = 0; k < 9; k++) {
                const uint64_t at = pane + PANE_XX + 4 * (uint64_t)k;
                store_f(c, at, load_f(c, at) * adjust.scale);
            }
            store_f(c, pane + PANE_X, load_f(c, pane + PANE_X) * adjust.scale + adjust.x);
            store_f(c, pane + PANE_Y, load_f(c, pane + PANE_Y) * adjust.scale + adjust.y);
            n->placed = (HudTransform){placed.scale * adjust.scale,
                                      placed.x * adjust.scale + adjust.x, placed.y * adjust.scale + adjust.y};
        }
    }
    const int map_page = n->map_layout && (n->map_mode == MAP_PAGE || (n->map_mode == MAP_UNSCOPED && n->as_menu));
    if (!parent && (!n->hud || map_page) && !whole_layout(n) && n->lkind != LK_HELD && ui_fit != 1.0f) {
        /* (a menu's root on a narrower screen: smaller to fit its width, about the canvas's middle; what is in it is made
           from it) */
        for (int k = 0; k < 9; k++) store_f(c, pane + PANE_XX + 4 * (uint64_t)k, load_f(c, pane + PANE_XX + 4 * (uint64_t)k) * ui_fit);
        store_f(c, pane + PANE_X, load_f(c, pane + PANE_X) * ui_fit), store_f(c, pane + PANE_Y, load_f(c, pane + PANE_Y) * ui_fit);
    }
    if (!parent && log_each > 0) {   /* (each root layout, once: the HUD's or a menu's) */
        static uint64_t told[64];
        static int told_n;
        int known = 0;
        for (int i = 0; i < told_n && !known; i++) known = told[i] == n->lname;
        if (!known && told_n < 64) {
            char name[40];
            told[told_n++] = n->lname;
            guest_copy(name, sizeof name, n->lname);
            hos_log("wide ui: root layout %s: %s", name, n->hud ? "the HUD's" : whole_layout(n) ? "over the whole picture"
                    : n->lkind == LK_HELD ? "held at the HUD's size" : "a menu's");
        }
    }
    if (n->wide != 1.0f && n->wide > 0.0f && n->type == PANE_TYPE_CAPTURE) {   /* (a capture is not drawn by f_8154ac30: its
                                                                               across axis, wider or narrower) */
        for (int k = 0; k < 3; k++) store_f(c, pane + PANE_XX + 4 * (uint64_t)k, load_f(c, pane + PANE_XX + 4 * (uint64_t)k) * n->wide);
    }
    ui_unlock();
}

/* The update also draws. Collapse the two canvas projections for this draw. The second matrix flips Y.
   Restore the game's values after the update so animation and input keep working and the next draw can show the HUD. */
static void draw_hidden_hud(Ctx *c, A64Fn update, uint64_t screen) {
    const uint64_t projection = screen + 0x1b8 + 8;
    const float mx = load_f(c, projection), my = load_f(c, projection + 20);
    if (!(mx > 0.0f && isfinite(mx) && my > 0.0f && isfinite(my) &&
          mx == load_f(c, projection + 64) && my == -load_f(c, projection + 84))) {
        update(c);
        return;
    }
    static const unsigned indices[] = {0, 5, 16, 21};
    uint32_t saved[4];
    for (unsigned i = 0; i < 4; i++) {
        const uint64_t at = projection + 4 * indices[i];
        saved[i] = (uint32_t)recomp_load32(c, at);
        recomp_store32(c, at, 0);
    }
    update(c);
    for (unsigned i = 0; i < 4; i++) recomp_store32(c, projection + 4 * indices[i], saved[i]);
}

static int gameplay_hud_screen(uint64_t vtable) {
    switch (vtable) {
    case 0x84c0b710ULL:   /* UIPlayerStatusScreen: hearts, magic, and items */
    case 0x84c0afe0ULL:   /* PlayerStatus3D_00: dive gauge */
    case 0x84c05a50ULL:   /* UIDoCommandScreen: action prompts */
    case 0x84c02d48ULL:   /* UIActionGuideScreen */
    case 0x84c05318ULL:   /* UICtrlGuideScreen */
    case 0x84c07400ULL:   /* UIHorseGaugeScreen */
        return 1;
    default:
        return 0;
    }
}

/* One thread may update distinct minimap and pause-map screens in the same frame. Force their
   cached pane transforms on a mode change without making both screens dirty on every draw. */
static void refresh_map_mode(uint64_t screen, uint8_t mode) {
    static _Thread_local struct { uint64_t screen; uint8_t mode; } previous[4];
    static _Thread_local unsigned next;
    for (unsigned i = 0; i < sizeof previous / sizeof *previous; i++) {
        if (previous[i].screen != screen) continue;
        if (previous[i].mode != mode) hos_wide_ui_force = 3;
        previous[i].mode = mode;
        return;
    }
    previous[next].screen = screen;
    previous[next].mode = mode;
    next = (next + 1) % (sizeof previous / sizeof *previous);
    hos_wide_ui_force = 3;
}

/* UIMapMenuScreen runs both maps. IsMiniMap scopes the live minimap's clip, contents and small-key counter to their
   shared transform. The full map's Boss Key and small-key badges stay in its equipment strip. */
void hos_gameplay_hud_screen(Ctx *c, A64Fn update) {
    const int hidden = __atomic_load_n(&hos_gameplay_hud_hidden, __ATOMIC_RELAXED) != 0;
    const int packed = __atomic_load_n(&hud_minimap_placement, __ATOMIC_RELAXED);
    const uint64_t screen = c->x[0];
    if (!screen) {
        update(c);
        return;
    }
    const uint64_t vtable = recomp_load64(c, screen);
    if (vtable != 0x84c08300ULL) {
        if (hidden && gameplay_hud_screen(vtable)) draw_hidden_hud(c, update, screen);
        else update(c);
        return;
    }
    const int mini = recomp_load8(c, screen + 0x2350) == 1;
    const int place = mini && packed != HOS_HUD_DEFAULT_PLACEMENT;
    const uint8_t previous_scope = map_scope;
    const HudTransform previous = minimap_transform;
    map_scope = mini ? MAP_MINIMAP : MAP_PAGE;
    refresh_map_mode(screen, map_scope);
    if (place) {
        /* (the map's frame is centred at (1536, -690) at 16:9, with its labels and direction marker just outside it) */
        const float ex = ui_ex, ey = ui_ey;
        const float qx = hos_ui_qx, qy = hos_ui_qy;
        minimap_transform = hud_transform(packed, 1200.0f + ex, 1860.0f + ex, -1040.0f - ey, -360.0f - ey,
                                          CANVAS_HALF / (qx > 0.0f ? qx : 1.0f), CANVAS_HALF_Y / (qy > 0.0f ? qy : 1.0f));
    } else {
        minimap_transform = identity;
    }
    if (mini && hidden) draw_hidden_hud(c, update, screen);
    else update(c);
    minimap_transform = previous;
    map_scope = previous_scope;
}

/* The drawn size of a pane, its width in the low lane of v0 and its height in v1's (f_8154ac30: ldp s0, s1) */
void hos_wide_ui_size(Ctx *c, uint64_t pane) {
    ui_lock();
    if (!ui_on) { ui_unlock(); return; }
    const Node *n = node_find(pane);
    if (!n || n->dead || n->type == PANE_TYPE_CAPTURE) { ui_unlock(); return; }   /* (a capture: widened above) */
    const int own_width = n->in_menu && n->wide > 1.0f && (n->type == PANE_TYPE_HOST || n->type == PANE_TYPE_SCISSOR);   /* (a list: widened above) */
    float v;
    if (n->wide != 1.0f && n->wide > 0.0f && !own_width) memcpy(&v, c->vreg[0], 4), v *= n->wide, memcpy(c->vreg[0], &v, 4);
    if (n->tall != 1.0f && n->tall > 0.0f) memcpy(&v, c->vreg[1], 4), v *= n->tall, memcpy(c->vreg[1], &v, 4);
    ui_unlock();
}

/* A layout's panes just built (f_81545d60 returned, the layout x19): a list kept at the screen's width is widened now. It
   lays out its items by its width as it opens, before it is first placed: widened only then, its items stayed where the
   16:9 width put them, one item off the selection, until the selection moved */
void hos_wide_ui_built(Ctx *c, uint64_t layout) {
    if (!__atomic_load_n(&menus_fast, __ATOMIC_ACQUIRE)) return;
    ui_lock();
    if (!ui_menus) { ui_unlock(); return; }
    const uint64_t base = recomp_load64(c, layout + LAYOUT_PANES);
    const uint32_t count = (uint32_t)recomp_load32(c, layout + LAYOUT_COUNT) & 0xffff;
    for (uint32_t i = 0; base && i < count; i++) {
        const uint64_t pane = base + (uint64_t)i * PANE_SIZE;
        if ((((uint32_t)recomp_load32(c, pane + PANE_FLAGS) >> 22) & 0xf) != PANE_TYPE_HOST) continue;
        Node probe = {0};
        classify(&probe, pane, layout);
        if (probe.rule != RULE_KEEP_WIDE) continue;
        Node *n = node_take(pane);
        if (!n) { ui_unlock(); return; }
        restore_width(c, n, pane);
        classify(n, pane, layout);
        n->layout = layout, n->type = PANE_TYPE_HOST, n->wide = ui_mwx;
        n->size_game = load_f(c, pane + PANE_W), n->size_set = n->size_game * n->wide;
        store_f(c, pane + PANE_W, n->size_set);
    }
    ui_unlock();
}

/* ---- the pause menu's background in pieces ----

   A picture's draw is made in two steps. f_815505b0 fills a record on its stack (sp + 0x54: its 3x4 matrix by rows at +0x40,
   the translation the last of each row; its size at +0xb0; each texture's corner coordinates, top left, top right, bottom
   left, bottom right, at REC_UV, their count in the low bits of +0x630) and packs it into the frame's constants
   (f_8154f1c0(record, *(info + 0x18), *(info + 0x20)), returning {address, ...} through x8, at sp + 8), which the drawer
   keeps at +0x48. f_81550720 draws the drawer with them later, its quad from -0.5 to 0.5 of the size across.

   For P_MenuBg_00 the record is packed three times, each with a piece of the picture: its left half (texture and quad) the
   extra further left, the column at its centre over the 2 x extra between, its right half the extra further right. The
   drawer keeps the left half's; the other two are drawn just before it, with the drawer's +0x48 set to theirs for each */
enum { REC_AT = 0x54, REC_OUT = 8, REC_MTX = 0x40, REC_SIZE = 0xb0, REC_TEXTURES = 0x630, SPLITS = 8 };
static const uint16_t REC_UV[6] = {0xc0, 0x110, 0x150, 0x1a0, 0x1e0, 0x230};   /* (f_8154f510's table at 0x83e80630) */
static const uint64_t SPLIT_PACK = 0x8154f1c0ull, SPLIT_DRAW = 0x81550720ull;
static const float QUAD_LEFT = -0.5f;   /* (the quad's left side, in widths from the translation) */
static const float SPLIT_AT = 0.5f;     /* (the cut, in widths from the picture's left: its centre, the Equipment page's) */

/* Slots and drawer use are serialized, including packing and the two draws. */
typedef struct {
    uint64_t drawer, pane, layout;
    uint64_t left[3], out[2][3];
    unsigned generation;
    int drawn;
} Split;
static Split splits[SPLITS];
static unsigned split_lock_word;
static void split_lock(void) { while (__atomic_exchange_n(&split_lock_word, 1, __ATOMIC_ACQUIRE)) {} }
static void split_unlock(void) { __atomic_store_n(&split_lock_word, 0, __ATOMIC_RELEASE); }

static Split *split_find(uint64_t drawer) {
    for (int i = 0; i < SPLITS; i++)
        if (splits[i].drawer == drawer) return &splits[i];
    return NULL;
}

/* Called with both locks held. Keep a completed draw for repeated passes until its slot is needed. */
static Split *split_slot(uint64_t drawer, unsigned generation) {
    Split *own = split_find(drawer);
    if (own) return own;
    Split *completed = NULL;
    for (int i = 0; i < SPLITS; i++) {
        Split *slot = &splits[i];
        if (!slot->drawer || slot->generation != generation) return slot;
        const Node *owner = node_find(slot->pane);
        if (!owner || owner->dead || owner->layout != slot->layout) return slot;
        if (slot->drawn && !completed) completed = slot;
    }
    return completed;
}

/* The record as the game filled it: what a piece changes */
typedef struct { float w, m0[3], t[3], uv[6][8]; int count; } Whole;

/* The piece from t0 to t1 of the picture's width, drawn wide (local units) and moved d across the canvas */
static void split_piece(Ctx *c, uint64_t rec, const Whole *o, float t0, float t1, float wide, float d) {
    const float at = QUAD_LEFT * (o->w - wide) + t0 * o->w;   /* (its left side where t0 was) */
    for (int r = 0; r < 3; r++) store_f(c, rec + REC_MTX + 16 * (uint64_t)r + 12, o->t[r] + o->m0[r] * at + (r ? 0.0f : d));
    store_f(c, rec + REC_SIZE, wide);
    for (int i = 0; i < o->count; i++) {
        const float *q = o->uv[i];   /* (top left, top right, bottom left, bottom right; u, v each) */
        for (int k = 0; k < 2; k++) {
            const float *a = q + 4 * k, *b = a + 2;
            for (int j = 0; j < 2; j++) {
                store_f(c, rec + REC_UV[i] + 16 * (uint64_t)k + 4 * (uint64_t)j, a[j] + (b[j] - a[j]) * t0);
                store_f(c, rec + REC_UV[i] + 16 * (uint64_t)k + 8 + 4 * (uint64_t)j, a[j] + (b[j] - a[j]) * t1);
            }
        }
    }
}

/* f_8154f1c0 on the record into out, on the game's thread */
static int split_pack(Ctx *c, uint64_t rec, uint64_t info, uint64_t out) {
    const A64Fn fn = a64_lookup(SPLIT_PACK);
    if (!fn) return 0;
    const Ctx saved = *c;
    c->x[0] = rec, c->x[1] = recomp_load64(c, info + 0x18), c->x[2] = recomp_load64(c, info + 0x20), c->x[8] = out, c->x[30] = 0;
    fn(c);
    *c = saved;
    return 1;
}

static void split_drop(Split *s) {
    if (!s) return;
    s->drawer = 0;
}

/* After f_815505b0 packed a picture's record (x19 the drawer, x20 the pane, x23 what it draws with), before the drawer keeps
   what it returned (in x8, x9 and at sp + 0x18) */
void hos_wide_ui_bg(Ctx *c) {
    if (!__atomic_load_n(&menus_fast, __ATOMIC_ACQUIRE)) return;
    const uint64_t drawer = c->x[19], pane = c->x[20];
    const uint64_t rec = c->x[31] + REC_AT, out = c->x[31] + REC_OUT, info = c->x[23];
    split_lock();
    ui_lock();
    const Node *n = ui_menus ? node_find(pane) : NULL;
    const float e = n && n->rule == RULE_SPLIT && n->space > 0.0f ? ui_mex : 0.0f;
    const uint64_t layout = n ? n->layout : 0;
    const unsigned generation = menu_generation;
    Split *s = e > 0.0f ? split_slot(drawer, generation) : split_find(drawer);
    ui_unlock();
    if (e <= 0.0f) {   /* (spaced as a page: the pages are apart) */
        split_drop(s);
        split_unlock(); return;
    }
    Whole o;
    o.w = load_f(c, rec + REC_SIZE);
    for (int r = 0; r < 3; r++)
        o.m0[r] = load_f(c, rec + REC_MTX + 16 * (uint64_t)r), o.t[r] = load_f(c, rec + REC_MTX + 16 * (uint64_t)r + 12);
    o.count = (int)((uint32_t)recomp_load32(c, rec + REC_TEXTURES) & 7);
    if (o.count > 6) o.count = 6;
    for (int i = 0; i < o.count; i++)
        for (int k = 0; k < 8; k++) o.uv[i][k] = load_f(c, rec + REC_UV[i] + 4 * (uint64_t)k);
    if (!s || !(o.w > 1.0f) || !(o.m0[0] > 1e-3f)) {
        split_drop(s);
        split_unlock(); return;
    }
    const float half = o.w * SPLIT_AT, gap = 2.0f * e / o.m0[0];
    uint64_t made[2][3];
    split_piece(c, rec, &o, SPLIT_AT, 1.0f, o.w - half, e);   /* (the right half) */
    if (!split_pack(c, rec, info, out)) {
        split_drop(s);
        split_unlock(); return;
    }
    for (int k = 0; k < 3; k++) made[0][k] = recomp_load64(c, out + 8 * (uint64_t)k);
    split_piece(c, rec, &o, SPLIT_AT, SPLIT_AT, gap, -e);    /* (the centre column, between) */
    split_pack(c, rec, info, out);
    for (int k = 0; k < 3; k++) made[1][k] = recomp_load64(c, out + 8 * (uint64_t)k);
    split_piece(c, rec, &o, 0.0f, SPLIT_AT, half, -e);       /* (the left half, the drawer's own) */
    split_pack(c, rec, info, out);
    c->x[8] = recomp_load64(c, out), c->x[9] = recomp_load64(c, out + 8);
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 3; k++) s->out[p][k] = made[p][k];
    for (int k = 0; k < 3; k++) s->left[k] = recomp_load64(c, out + 8 * (uint64_t)k);
    s->drawer = drawer;
    s->pane = pane;
    s->layout = layout;
    s->generation = generation;
    s->drawn = 0;
    static int told;
    if (!told) told = 1, hos_log("wide ui: the pause menu's background drawn in three pieces, %.0f units apart", (double)(2.0f * e));
    split_unlock();
}

/* At the start of f_81550720 (x0 the drawer, x1 what it draws with): the pieces before the drawer's own */
void hos_wide_ui_bg_draw(Ctx *c) {
    static _Thread_local int inside;
    const uint64_t drawer = c->x[0];
    if (inside || !__atomic_load_n(&menus_fast, __ATOMIC_ACQUIRE)) return;
    ui_lock();
    const int menus = ui_menus;
    const unsigned generation = menu_generation;
    ui_unlock();
    if (!menus) return;
    split_lock();
    Split *s = split_find(drawer);
    if (!s || s->generation != generation) { split_unlock(); return; }
    uint64_t own[3];
    for (int k = 0; k < 3; k++) own[k] = recomp_load64(c, drawer + 0x48 + 8 * (uint64_t)k);
    /* The drawer can have been destroyed and rebuilt at the same address. */
    if (memcmp(s->left, own, sizeof own) != 0) {
        split_drop(s);
        split_unlock();
        return;
    }
    const A64Fn fn = a64_lookup(SPLIT_DRAW);
    if (!fn) { split_unlock(); return; }
    inside = 1;
    for (int p = 0; p < 2; p++) {
        for (int k = 0; k < 3; k++) recomp_store64(c, drawer + 0x48 + 8 * (uint64_t)k, s->out[p][k]);
        const Ctx saved = *c;
        c->x[0] = drawer, c->x[1] = saved.x[1], c->x[30] = 0;
        fn(c);
        *c = saved;
    }
    inside = 0;
    for (int k = 0; k < 3; k++) recomp_store64(c, drawer + 0x48 + 8 * (uint64_t)k, own[k]);
    s->drawn = 1;
    split_unlock();
}
