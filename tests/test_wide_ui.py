"""The HUD's pane planner (ozwaldorf's Extended ultrawide menus,
https://github.com/NotRexed/Hookshot/pull/2843) moves a HUD layout's side panes out
to a wider screen's edges, and only while the picture is not 16:9.

nx2/oot_wide.py calls host/hos_wide_ui.c from f_8154cab0, where the game makes a pane's final transform: the parent it is
made from, then the transform stored (the translation at +0x4c). A pane at one side of the 3840-wide canvas whose ancestors
were not moved has its x moved out by the screen's extra half width; its children keep the move they were made with; a
pane spanning the canvas is drawn the screen's width instead; the HUD's gauges move by name, a pointer placed from the world
stays, the minimap's icons move with it and the map page keeps its own. A menu's layout is not moved. A change of the shape
marks every pane to remake its transform.

python -I -B tests/test_wide_ui.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/longshot_env.h)
ENV_ON_H = str(ROOT / "gpu" / "vk" / "compat" / "longshot_env.h")
sys.path.insert(0, str(ROOT / "nx2"))
import oot_wide  # noqa: E402

FIXTURE_HEAD = '#include "%s"\n' % (ROOT / "gpu/vk/vk_api.h").as_posix() + r'''#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
#define BASE 0x10000000ull
static unsigned char mem[0x40000];
volatile float hos_wide_squeeze, hos_wide_ratio;
static uint64_t recomp_load8(Ctx *c, uint64_t a) { (void)c; if (a < BASE || a >= BASE + sizeof mem) return 0; return mem[a - BASE]; }
static uint64_t recomp_load32(Ctx *c, uint64_t a) { uint32_t v; (void)c; if (a < BASE || a + 4 > BASE + sizeof mem) return 0; memcpy(&v, mem + (a - BASE), 4); return v; }
static uint64_t recomp_load64(Ctx *c, uint64_t a) { uint64_t v; (void)c; if (a < BASE || a + 8 > BASE + sizeof mem) return 0; memcpy(&v, mem + (a - BASE), 8); return v; }
static void recomp_store32(Ctx *c, uint64_t a, uint32_t v) { (void)c; memcpy(mem + (a - BASE), &v, 4); }
static void recomp_store64(Ctx *c, uint64_t a, uint64_t v) { (void)c; memcpy(mem + (a - BASE), &v, 8); }
typedef void (*A64Fn)(Ctx *);
A64Fn a64_lookup(uint64_t addr);
static void hos_log(const char *f, ...) { (void)f; }
static int64_t hos_now_ns(void) { return 0; }
/* (the Insert menu's rows, host/rx_backend.c rx_option: the Graphics tab's HUD layout, 71, as fixture_hud_layout says) */
int fixture_hud_layout, fixture_wide_menus;
int fixture_hud_health = HOS_HUD_DEFAULT_PLACEMENT;
int fixture_hud_map = HOS_HUD_DEFAULT_PLACEMENT;
int rx_option(int id, int otherwise) {
    if (id == HOS_OPT_HUD_LAYOUT) return fixture_hud_layout;
    if (id == HOS_OPT_HUD_HEALTH) return fixture_hud_health;
    if (id == HOS_OPT_HUD_MINIMAP) return fixture_hud_map;
    if (id == HOS_OPT_WIDE_MENUS) return fixture_wide_menus;
    return otherwise;
}
'''

FIXTURE_MAIN = r'''
static void putf(uint64_t a, float v) { memcpy(mem + (a - BASE), &v, 4); }
static void put32(uint64_t a, uint32_t v) { memcpy(mem + (a - BASE), &v, 4); }
static void put64(uint64_t a, uint64_t v) { memcpy(mem + (a - BASE), &v, 8); }
static float getf(uint64_t a) { float v; memcpy(&v, mem + (a - BASE), 4); return v; }
static int near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }
static Ctx c;
volatile uint32_t hos_wide_keep_mask;
static float packed_width[3], packed_x[3], packed_uv[3][4];
static uint64_t drawn_constants[2];
static int pack_count, draw_count;
static void fixture_pack(Ctx *ctx) {
    const int i = pack_count++;
    CHECK(i < 3);
    packed_width[i] = getf(ctx->x[0] + REC_SIZE);
    packed_x[i] = getf(ctx->x[0] + REC_MTX + 12);
    for (int k = 0; k < 4; k++) packed_uv[i][k] = getf(ctx->x[0] + REC_UV[0] + 4 * k);
    for (int k = 0; k < 3; k++) put64(ctx->x[8] + 8 * k, 100 + 10 * i + k);
}
static void fixture_draw(Ctx *ctx) {
    CHECK(draw_count < 2);
    drawn_constants[draw_count++] = recomp_load64(ctx, ctx->x[0] + 0x48);
}
A64Fn a64_lookup(uint64_t addr) { return addr == SPLIT_PACK ? fixture_pack : addr == SPLIT_DRAW ? fixture_draw : NULL; }
#define LAYOUT (BASE + 0x100)
#define WORLD (BASE + 0x300)
#define STATUS (BASE + 0x500)
#define PANES (BASE + 0x1000)
#define NAMES (BASE + 0x8000)
#define STRINGS (BASE + 0x9000)
static uint64_t text_at = STRINGS;
static uint64_t text(const char *s) { const uint64_t a = text_at; memcpy(mem + (a - BASE), s, strlen(s) + 1); text_at += 64; return a; }
/* a layout object and its UIL header: its name, the pane names (one table for every test layout) */
static void layout_named(uint64_t l, const char *name) {
    const uint64_t res = l + 0x180;
    memcpy(mem + (l + 0x100 - BASE), &(uint64_t){PANES}, 8), memcpy(mem + (l + 0x13c - BASE), &(uint32_t){64}, 4);
    memcpy(mem + (l + 0xf8 - BASE), &res, 8);
    memcpy(mem + (res + 0x18 - BASE), &(uint64_t){text(name)}, 8), memcpy(mem + (res + 0x20 - BASE), &(uint64_t){NAMES}, 8);
}
/* pane i of the layout: x scale 1, its x and width; origin across 0 left, 1 centre; kind 0 none, 1 picture, 6 part */
static uint64_t pane_in(uint64_t layout, int i, const char *name, float x, float w, unsigned origin, unsigned type) {
    const uint64_t p = PANES + (uint64_t)i * 0xb0;
    put64(p + 0x18, layout), put64(NAMES + (uint64_t)i * 8, text(name));
    putf(p + 0x28, 1.0f), putf(p + 0x4c, x), putf(p + 0x58, w);
    put32(p + 0xa8, ((origin + 3) << 11) | (type << 22) | 1);
    return p;
}
static uint64_t pane(int i, float x, float w, unsigned origin, unsigned type) { return pane_in(LAYOUT, i, "P_Test_00", x, w, origin, type); }
/* the game making p's transform from parent (0: none); x as the game stores it (its parent's move in it) */
static float place(uint64_t p, uint64_t parent, float x) {
    putf(p + 0x4c, x);
    hos_wide_ui_parent(parent);
    hos_wide_ui_placed(&c, p);
    return getf(p + 0x4c);
}
static float drawn_width(uint64_t p) {
    const float w = getf(p + 0x58);
    memcpy(c.vreg[0], &w, 4);
    hos_wide_ui_size(&c, p);
    float out;
    memcpy(&out, c.vreg[0], 4);
    return out;
}
/* a run with a switch set (the planner reads them once): Extended on 3440x1440, the row's Centred mask handed in.
   "hud-off", HOS_WIDE_UI=0: the HUD stays, the menus are placed all the same. "keep", HOS_WIDE_KEEP set: the mask stands,
   and the system overlay, kept whole by its bit 4, is the game's */
static int switch_case(const char *which) {
    const int keep = !strcmp(which, "keep");
    layout_named(STATUS, "PlayerStatus_00"), layout_named(BASE + 0x4400, "Test_Menu_00"), layout_named(BASE + 0x4a00, "SystemOverlay_00");
    hos_wide_squeeze = (16.0f / 9.0f) / (3440.0f / 1440.0f), hos_wide_ratio = 3440.0f / 1440.0f;
    const float extra = 1920.0f * (1.0f / hos_wide_squeeze - 1.0f);
    const uint64_t sroot = pane_in(STATUS, 20, "RootPane", 0, 0, 1, 0), gauge = pane_in(STATUS, 22, "Pa_HeartGauge_00", -1140, 1300, 1, 6),
                   mroot = pane_in(BASE + 0x4400, 30, "RootPane", 0, 0, 1, 0), mleft = pane_in(BASE + 0x4400, 31, "P_Column_00", -1920, 400, 0, 1),
                   oroot = pane_in(BASE + 0x4a00, 44, "RootPane", 0, 0, 1, 0), oback = pane_in(BASE + 0x4a00, 45, "N_Back_00", 1626, 240, 1, 1);
    fixture_wide_menus = HOS_WIDE_MENUS_EXTENDED;
    for (int i = 0; i < 4; i++) {
        hos_wide_keep_mask = 0x15;
        hos_wide_ui_tick();
        place(sroot, 0, 0); place(gauge, sroot, -1140); place(mroot, 0, 0); place(mleft, mroot, -1920);
        place(oroot, 0, 0); place(oback, oroot, 1626);
    }
    CHECK(near(getf(mleft + 0x4c), -1920 - extra));
    if (keep) {
        CHECK(hos_wide_keep_mask == 0x15 && getf(oback + 0x4c) == 1626);
        CHECK(near(getf(gauge + 0x4c), -1140 - extra));
    } else {
        CHECK(getf(gauge + 0x4c) == -1140);
        CHECK(!(hos_wide_keep_mask & 0x10u) && near(getf(oback + 0x4c), 1626 + extra));
    }
    printf("ok: %s\n", which);
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1) return switch_case(argv[1]);
    /* (Talk_00, one of the HUD's layouts, for the planner's general cases; Test_Menu_00 a menu's, never moved) */
    layout_named(LAYOUT, "Talk_00"), layout_named(WORLD, "Pointer_00"), layout_named(STATUS, "PlayerStatus_00");
    hos_wide_squeeze = (16.0f / 9.0f) / (3440.0f / 1440.0f);   /* (3440x1440) */
    const float extra = 1920.0f * (1.0f / hos_wide_squeeze - 1.0f);
    const uint64_t root = pane(0, 0, 0, 1, 0), all = pane(1, 0, 200, 1, 0), left = pane(2, -1920, 60, 1, 0),
                   heart = pane(3, -1140, 1300, 1, 6), right = pane(4, 1626, 240, 1, 1), back = pane(5, 0, 3840, 1, 1),
                   page = pane(6, 3840, 0, 1, 0), cell = pane(7, 3840, 200, 1, 1), grid = pane(10, 0, 0, 1, 0),
                   col0 = pane(11, -1750, 200, 1, 1), col1 = pane(12, -900, 200, 1, 1), preview = pane(13, 700, 900, 1, 1),
                   edge = pane(14, -1920, 400, 0, 1);
    /* the HUD by name: the left group stays (the pause menu places part of it), its gauges move; a pointer placed from the
       world stays wherever it is */
    const uint64_t sroot = pane_in(STATUS, 20, "RootPane", 0, 0, 1, 0), scale = pane_in(STATUS, 21, "N_ScaleL_00", -1920, 60, 1, 0),
                   gauge = pane_in(STATUS, 22, "Pa_HeartGauge_00", -1140, 1300, 1, 6),
                   step = pane_in(STATUS, 23, "N_ScaleL_01", 6, 60, 1, 0), mp = pane_in(STATUS, 24, "Pa_MpGauge_00", -1560, 1256, 1, 6),
                   wroot = pane_in(WORLD, 25, "RootPane", 0, 0, 1, 0), arrow = pane_in(WORLD, 26, "P_Arrow_00", -1800, 100, 1, 1);
    /* the game's frame: parents before children; dx the move the plan gave each parent */
    float moved_left = 0, moved_grid0 = 0, moved_grid1 = 0;
#define FRAME() do { \
        place(root, 0, 0); place(all, root, 0); moved_left = place(left, all, -1920) + 1920; \
        place(heart, left, -1140 + moved_left); place(right, all, 1626); place(back, root, 0); \
        place(page, root, 3840); place(cell, page, 3840); place(grid, root, 0); \
        moved_grid0 = place(col0, grid, -1750) + 1750; moved_grid1 = place(col1, grid, -900) + 900; \
        place(preview, grid, 700); place(edge, root, -1920); \
        place(sroot, 0, 0); place(scale, sroot, -1920); place(gauge, scale, -1140); place(step, scale, 6); \
        place(mp, step, -1560); place(wroot, 0, 0); place(arrow, wroot, -1800); \
    } while (0)

    hos_wide_ui_tick();   /* a 16:9 picture: nothing moves */
    FRAME();
    CHECK(moved_left == 0 && getf(right + 0x4c) == 1626 && drawn_width(back) == 3840);

    hos_wide_ratio = 3440.0f / 1440.0f;   /* 3440x1440: noted on the first frame, planned at the present, moved on the next */
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);
    FRAME();
    CHECK(moved_left == 0);
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);   /* (the plan changed: every pane placed again) */
    FRAME();
    CHECK(near(moved_left, -extra));                       /* (the HUD's left side out, at the left edge) */
    CHECK(near(getf(heart + 0x4c), -1140 - extra));        /* (made from it: moved once) */
    CHECK(near(getf(right + 0x4c), 1626 + extra));         /* (far out on the right: out) */
    CHECK(near(drawn_width(back), 3840 / hos_wide_squeeze) && drawn_width(right) == 240);   /* (spans: wider) */
    CHECK(getf(page + 0x4c) == 3840 && getf(cell + 0x4c) == 3840);   /* (off the canvas: left alone) */
    CHECK(near(moved_grid0, -extra) && near(moved_grid1, -extra));   /* (a grid's columns out together) */
    CHECK(getf(preview + 0x4c) == 700);                    /* (its other side stays) */
    CHECK(near(getf(edge + 0x4c), -1920 - extra));         /* (left origin, at the left edge) */
    CHECK(getf(scale + 0x4c) == -1920 && getf(step + 0x4c) == 6);   /* (the HUD's left group: kept) */
    CHECK(near(getf(gauge + 0x4c), -1140 - extra) && near(getf(mp + 0x4c), -1560 - extra));   /* (its gauges: out) */
    CHECK(getf(arrow + 0x4c) == -1800);                    /* (placed from the world: stays) */
    for (int i = 0; i < 4; i++) { hos_wide_ui_tick(); FRAME(); }   /* (stable: the plan no longer changes) */
    CHECK(hos_wide_ui_force == 0 && near(moved_left, -extra) && near(getf(heart + 0x4c), -1140 - extra));

    /* a menu's layout: not moved, even at the canvas's edge, and only its root noted (the plan never walks a menu's panes);
       a HUD part in a menu (the hearts in the pause menu's bar) stays with the menu */
    {
#define MENU (BASE + 0x700)
        layout_named(MENU, "Test_Menu_00");
        const uint64_t mroot = pane_in(MENU, 30, "RootPane", 0, 0, 1, 0), mleft = pane_in(MENU, 31, "P_Column_00", -1920, 400, 0, 1),
                       mheart = pane_in(STATUS, 32, "Pa_HeartGauge_00", -1140, 1300, 1, 6);
        const int noted = nodes_used;
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); place(mroot, 0, 0); place(mleft, mroot, -1920); place(mheart, mroot, -1140); }
        CHECK(getf(mleft + 0x4c) == -1920 && near(moved_left, -extra) && getf(mheart + 0x4c) == -1140);
        CHECK(nodes_used == noted + 1);
    }

    /* the Map page (its icons' group at the canvas's origin): drawn as a menu's, nothing in it moved, its title and its frame
       at the edge included; its icons (placed by code, one by the right edge) and the world map (wider than the screen) as
       they are. The same layout is the HUD's minimap: there its icons are not at the origin and move out with it, and the
       title and the frame are planned */
    {
#define MAPMENU (BASE + 0x4200)
#define MAPICON (BASE + 0x4400)
#define MAPWORLD (BASE + 0x4600)
        layout_named(MAPMENU, "MapMenu_00");
        layout_named(MAPICON, "PaMapIcon_00"), layout_named(MAPWORLD, "PaMapWorld_00");
        const uint64_t mroot2 = pane_in(MAPMENU, 56, "RootPane", 0, 0, 1, 0), title = pane_in(MAPMENU, 57, "T_MapTitle_00", -1702, 1030, 0, 1),
                       icons = pane_in(MAPMENU, 58, "N_MapIcon_00", 0, 60, 1, 0), world = pane_in(MAPMENU, 59, "Pa_WorldMap_00", 0, 9920, 1, 6),
                       iroot = pane_in(MAPICON, 60, "RootPane", 0, 0, 1, 0), icon = pane_in(MAPICON, 61, "P_Icon_00", 1800, 60, 1, 1),
                       wmroot = pane_in(MAPWORLD, 62, "RootPane", 0, 0, 1, 0), wmap = pane_in(MAPWORLD, 63, "P_WorldMap_00", 0, 8448, 1, 1),
                       mframe = pane_in(MAPMENU, 33, "P_Frame_00", -1700, 400, 1, 1),
                       floortab = pane_in(MAPMENU, 52, "N_FloorTab_00", -1770, 480, 1, 1), vignette = pane_in(MAPMENU, 53, "P_Vignette_L_00", -2040, 1200, 0, 1);
        float ix = 0;
        float root_x = 0;   /* (where the icons' layout puts its root: the dungeon's, on its icons) */
#define PAGES_FRAME() do { \
            place(mroot2, 0, 0); place(title, mroot2, -1702); place(icons, mroot2, ix); \
            place(iroot, icons, getf(icons + 0x4c) + root_x); place(icon, iroot, getf(iroot + 0x4c) - root_x + 1800 - ix); place(world, mroot2, 0); place(wmroot, world, 0); place(wmap, wmroot, 0); \
            place(mframe, mroot2, -1700); place(floortab, mroot2, -1770); place(vignette, mroot2, -2040); \
        } while (0)
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(getf(title + 0x4c) == -1702 && getf(mframe + 0x4c) == -1700);
        CHECK(getf(icon + 0x4c) == 1800 && getf(wmap + 0x4c) == 0 && drawn_width(wmap) == 8448);
        ix = 1490;   /* the minimap: its icons over it, by the right edge */
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(near(getf(icons + 0x4c), 1490 + extra));
        CHECK(near(getf(title + 0x4c), -1702 - extra) && near(getf(mframe + 0x4c), -1700 - extra));
        /* the dungeon's minimap: its icon group at the origin as the page has it, its icons on the right over the small map. While the
           frames draw the world it is the HUD's, the icons moved out with the map; the map page keeps its own layout */
        map_scope = MAP_MINIMAP, ix = 0, root_x = 1790;
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(near(getf(icon + 0x4c), 1800 + extra) && near(getf(title + 0x4c), -1702 - extra));
        map_scope = MAP_PAGE;
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(near(getf(icon + 0x4c), 1800) && getf(title + 0x4c) == -1702);
        /* the page with the Ultrawide menus row at Extended: a side pane moves out, but the floor list, as the title, stays beside the map */
        fixture_wide_menus = HOS_WIDE_MENUS_EXTENDED, ix = 0, root_x = 0;
        for (int i = 0; i < 4; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(near(getf(vignette + 0x4c), -2040 - extra) && getf(floortab + 0x4c) == -1770 && getf(title + 0x4c) == -1702);
        fixture_wide_menus = 0;
        map_scope = MAP_UNSCOPED;
    }

    /* the Ultrawide menus row at Extended: a menu's layout is placed as the HUD's is, by the screen's whole extra width; the row
       back, or a 16:9 picture, and it is the game's again */
    {
        const uint64_t mroot = pane_in(MENU, 30, "RootPane", 0, 0, 1, 0), mleft = pane_in(MENU, 31, "P_Column_00", -1920, 400, 0, 1),
                       mback = pane_in(MENU, 34, "P_Back_00", 0, 3840, 1, 1);
#define LFADE (BASE + 0x4c00)
        layout_named(LFADE, "LoadingFade_00");
        /* (the loading fade: its cover spans the canvas, its icon sits in the canvas's corner) */
        const uint64_t lroot = pane_in(LFADE, 49, "RootPane", 0, 0, 1, 0), lcover = pane_in(LFADE, 50, "P_Fade_00", 0, 3840, 1, 1),
                       licon = pane_in(LFADE, 51, "N_IconIn_00", 1778, 200, 1, 0);
#define SYSOV (BASE + 0x4a00)
        layout_named(SYSOV, "SystemOverlay_00");
        /* (the system overlay: kept whole by its screen, planned as a menu's only with the menus placed) */
        const uint64_t oroot = pane_in(SYSOV, 44, "RootPane", 0, 0, 1, 0), oback = pane_in(SYSOV, 45, "N_Back_00", 1626, 240, 1, 1);
#define PLUS (BASE + 0x4800)
        layout_named(PLUS, "PlusMenu_00");
        /* (the pause menu: its pages along a strip, 4280 apart, spaced wider by twice the extra width) */
        const uint64_t proot = pane_in(PLUS, 40, "RootPane", 0, 0, 1, 0), strip = pane_in(PLUS, 41, "N_MenuChange_00", 0, 0, 1, 0),
                       page0 = pane_in(PLUS, 42, "N_Page_00", 0, 0, 1, 0), page1 = pane_in(PLUS, 43, "N_Page_01", 4280, 0, 1, 0),
                       split = pane_in(PLUS, 54, "P_MenuBg_00", 4280, 3840, 1, 1);
#define LIST (BASE + 0x4e00)
        layout_named(LIST, "PaShortCutSlideList_00");
        const uint64_t list = pane_in(LIST, 55, "N_SlideList_00", 0, 3840, 1, 7);
        /* (a list inside a scissor: the scissor moves with the list, which is then moved once, not cut off at the scissor's old edge) */
        const uint64_t sc = pane_in(MENU, 46, "S_List_00", -1060, 1600, 1, 4), grp = pane_in(MENU, 47, "N_List_00", -1060, 0, 1, 0),
                       bg = pane_in(MENU, 48, "P_ListBg_00", -1060, 1600, 1, 1);
        float msc = 0, mbg = 0;
        for (int pass = 0; pass < 3; pass++) {
            fixture_wide_menus = pass == 1 ? HOS_WIDE_MENUS_EXTENDED : 0;
            hos_wide_keep_mask = 0x15;   /* (the row's Centred choices' mask: the system overlay's bit 4 kept) */
            for (int i = 0; i < 4; i++) { hos_wide_ui_tick(); FRAME(); place(mroot, 0, 0); place(mleft, mroot, -1920); place(mback, mroot, 0);
                place(oroot, 0, 0); place(oback, oroot, 1626);
                place(lroot, 0, 0); place(lcover, lroot, 0); place(licon, lroot, 1778);
                msc = place(sc, mroot, -1060) + 1060, place(grp, sc, -1060 + msc), mbg = place(bg, grp, -1060 + msc) + 1060;
                place(proot, 0, 0); place(strip, proot, 0); place(page0, strip, 0); place(page1, strip, 4280); place(split, strip, 4280); }
            if (pass == 1) {
                hos_wide_ui_built(&c, LIST);
                CHECK(near(getf(list + PANE_W), 3840 / hos_wide_squeeze));
                hos_wide_ui_built(&c, LIST);   /* construction called twice must not compound the width */
                CHECK(near(getf(list + PANE_W), 3840 / hos_wide_squeeze));
                const uint64_t stack = BASE + 0x20000, drawer = BASE + 0x21000, info = BASE + 0x22000, rec = stack + REC_AT;
                putf(rec + REC_SIZE, 3840), putf(rec + REC_MTX, 1);
                put32(rec + REC_TEXTURES, 1);
                putf(rec + REC_UV[0], 0), putf(rec + REC_UV[0] + 4, 0);
                putf(rec + REC_UV[0] + 8, 1), putf(rec + REC_UV[0] + 12, 0);
                c.x[19] = drawer, c.x[20] = split, c.x[23] = info, c.x[31] = stack;
                hos_wide_ui_bg(&c);
                CHECK(pack_count == 3 && near(packed_width[0], 1920) && near(packed_width[1], 2 * extra) && near(packed_width[2], 1920));
                CHECK(near(packed_x[0], 960 + extra) && near(packed_x[1], 0) && near(packed_x[2], -960 - extra));
                CHECK(near(packed_uv[0][0], 0.5f) && near(packed_uv[0][2], 1) && near(packed_uv[1][0], 0.5f) && near(packed_uv[1][2], 0.5f) && near(packed_uv[2][0], 0) && near(packed_uv[2][2], 0.5f));
                put64(drawer + 0x48, 120), put64(drawer + 0x50, 121), put64(drawer + 0x58, 122);
                c.x[0] = drawer;
                hos_wide_ui_bg_draw(&c);
                CHECK(draw_count == 2 && drawn_constants[0] == 100 && drawn_constants[1] == 110 && recomp_load64(&c, drawer + 0x48) == 120);
                CHECK(getf(page0 + 0x4c) == 0 && near(getf(page1 + 0x4c), 4280 + 2 * extra));
                CHECK(near(getf(oback + 0x4c), 1626 + extra) && !(hos_wide_keep_mask & 0x10u));
                draw_count = 0;
                hos_wide_ui_bg_draw(&c);
                CHECK(draw_count == 2);   /* Another pass still uses the same packed draw. */

                /* Reusing a drawer's address with a different packed draw cannot read its old pieces. */
                put64(drawer + 0x48, 300);
                draw_count = 0;
                hos_wide_ui_bg_draw(&c);
                CHECK(draw_count == 0);

                /* Reopen at more drawer addresses than the split ring can retain. */
                for (int reopen = 1; reopen <= 10; reopen++) {
                    const uint64_t reopened = drawer + 0x80 * (uint64_t)reopen;
                    pack_count = 0, draw_count = 0;
                    putf(rec + REC_SIZE, 3840);
                    putf(rec + REC_MTX + 12, 0);
                    putf(rec + REC_UV[0], 0), putf(rec + REC_UV[0] + 8, 1);
                    c.x[19] = reopened;
                    hos_wide_ui_bg(&c);
                    CHECK(pack_count == 3);
                    put64(reopened + 0x48, 120), put64(reopened + 0x50, 121), put64(reopened + 0x58, 122);
                    c.x[0] = reopened;
                    hos_wide_ui_bg_draw(&c);
                    CHECK(draw_count == 2 && drawn_constants[0] == 100 && drawn_constants[1] == 110);
                    CHECK(recomp_load64(&c, reopened + 0x48) == 120);
                }
                CHECK(near(msc, -extra) && near(mbg, -extra));
                CHECK(near(getf(licon + 0x4c), 1778 + extra) && near(drawn_width(lcover), 3840 / hos_wide_squeeze));
                CHECK(near(getf(mleft + 0x4c), -1920 - extra));
                CHECK(near(drawn_width(mback), 3840 / hos_wide_squeeze));
            } else {
                place(list, 0, 0);
                CHECK(near(getf(list + PANE_W), 3840));
                CHECK(getf(mleft + 0x4c) == -1920 && drawn_width(mback) == 3840 && getf(page1 + 0x4c) == 4280);
                CHECK(getf(oback + 0x4c) == 1626);   /* (kept whole: the game's) */
                CHECK(msc == 0 && mbg == 0);
                CHECK(getf(licon + 0x4c) == 1778);   /* (the centred choices: the cover spans the picture as before, the icon stays in the middle 16:9) */
            }
        }
        hos_wide_ui_force = 0;
    }

    /* a layout let go: its panes are not planned with the ones that take their addresses */
    put32(LAYOUT + 0x13c, 12);
    hos_wide_ui_tick();
    FRAME();
    CHECK(getf(edge + 0x4c) == -1920);
    put32(LAYOUT + 0x13c, 64);

    hos_wide_ratio = 0;   /* back to 16:9: every pane remade as the game makes it */
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);
    FRAME();
    CHECK(moved_left == 0 && getf(right + 0x4c) == 1626 && drawn_width(back) == 3840);

    hos_wide_ratio = 16.0f / 9.0f;   /* a 16:9 screen's own ratio: nothing moves */
    for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); }
    CHECK(moved_left == 0);

    /* the force site: bit 6 of the dirty bits set while forced */
    {
        Ctx *const site = &c;
        site->x[8] = 0x1;
        hos_wide_ui_force = 2;
        { Ctx *c = site; FORCE_SITE }
        CHECK(site->x[8] == 0x41);
        hos_wide_ui_force = 0;
        site->x[8] = 0x1;
        { Ctx *c = site; FORCE_SITE }
        CHECK(site->x[8] == 0x1);
    }
    (void)moved_grid0, (void)moved_grid1;
    printf("PASS: the HUD's planner moves its layouts' side panes out to a wider screen's edges, and back\n");
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    host = (ROOT / "host" / "hos_wide_ui.c").read_text(encoding="utf-8")
    backend = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")

    check("hos_wide_ui_tick();" in backend, "the host does not plan the HUD each present")
    check('getenv("HOS_WIDE_UI")' in host, "HOS_WIDE_UI no longer turns the HUD's moves off")

    words = {off: word for off, word, _ in oot_wide.UI_SITES}
    check(words == {0x152a730: 0xb9400008, 0x152caa0: 0xb9400008, 0x152cd98: 0xad4207e0, 0x152cdd4: 0xb9403008, 0x152ce64: 0xfc04c263,
                    0x152ac40: 0x2d4b0400, 0x15306f4: 0xa940a7e8, 0x1530740: 0xaa0003f6, 0x1525ca0: 0x91004260},
          "the pane transform sites moved")
    check(all(site in oot_wide.SITES for site in oot_wide.UI_SITES), "the pane sites are not installed")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    opt = re.search(r"HOS_OPT_WIDE_MENUS = (\d+), HOS_WIDE_MENUS_EXTENDED = (\d+)", api)
    check(opt is not None, "vk_api.h no longer names the Ultrawide menus option")
    check(opt.groups() == ("102", "3"), "the Ultrawide menus option or its Extended value moved: update FIXTURE_HEAD")
    for name in ("hos_wide_ui_parent(uint64_t parent)", "hos_wide_ui_placed(Ctx *c, uint64_t pane)",
                 "hos_wide_ui_size(Ctx *c, uint64_t pane)"):
        check(("void " + name) in host, "the host's %s has another signature" % name.split("(")[0])

    body = "".join(line for line in host.splitlines(keepends=True) if not line.startswith("#include"))
    for decl in ("extern volatile float hos_wide_ratio;\n",):
        check(decl in body, "the host file no longer declares " + decl.strip())
        body = body.replace(decl, "", 1)
    src = FIXTURE_HEAD + body + FIXTURE_MAIN.replace("FORCE_SITE", oot_wide.UI_FORCE)
    with tempfile.TemporaryDirectory(prefix="wide-ui-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / ("fixture.exe" if sys.platform == "win32" else "fixture")
        c.write_text(src, encoding="utf-8")
        cc = subprocess.run([a.cc, "-std=c11", "-O1", "-include", ENV_ON_H, "-Wall", "-Wno-unused-function", "-o", str(exe), str(c), "-lm"],
                            capture_output=True, text=True)
        check(cc.returncode == 0, "fixture did not compile:\n" + cc.stdout + cc.stderr)
        env = {k: v for k, v in os.environ.items() if k not in ("HOS_WIDE_UI", "HOS_WIDE_KEEP", "HOS_WIDE_UI_LOG",
                                                                  "HOS_HUD_MAX_RATIO", "HOS_HUD_FULL_RATIO")}
        for case, extra in (((), {}), (("hud-off",), {"HOS_WIDE_UI": "0"}), (("keep",), {"HOS_WIDE_KEEP": "15"})):
            run = subprocess.run([str(exe), *case], capture_output=True, text=True, env={**env, **extra})
            check(run.returncode == 0, "fixture %s failed:\n" % " ".join(case) + run.stdout + run.stderr)
            print(run.stdout.strip())


if __name__ == "__main__":
    main()
