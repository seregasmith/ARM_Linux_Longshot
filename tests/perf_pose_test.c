/* tests/test_perf_pose.py: Link's pose for the performance summary's one-second samples, the host's code run on a made-up guest.
   The pieces of host/hos_main.c (hos_perf_info, oot_perf_pose and what it calls, the map line's refresh) are cut out of the
   source by their text into perf_pose_cut.h and included below; what the game would give them is made here: guest memory (the
   camera manager's chain to the gameplay view, its eye and target, the event manager, Link's place), the clock, the locks, and
   what the save says (the map, loaded or not). Nothing is taken from a dump: the addresses are the ones the host already
   names, the values are made up. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long LONG;
typedef struct Ctx { int dummy; } Ctx;
#define LONGSHOT_VERSION "test"

/* (the locks: none is ever held twice, a shared one never beside an exclusive: what would hang the game) */
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static int bad_lock;
static void exclusive(SRWLOCK *p) { if (*p) bad_lock++; *p = -1; }
static void shared(SRWLOCK *p) { if (*p < 0) bad_lock++; else (*p)++; }
static void release(SRWLOCK *p) { if (*p == 0) bad_lock++; else *p = *p < 0 ? 0 : *p - 1; }
#define AcquireSRWLockExclusive(p) exclusive(p)
#define AcquireSRWLockShared(p) shared(p)
#define ReleaseSRWLockExclusive(p) release(p)
#define ReleaseSRWLockShared(p) release(p)
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG old = *p; *p = v; return old; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG exchange, LONG compare) {
    LONG old = *p;
    if (old == compare) *p = exchange;
    return old;
}

static int checks, failed;
static void check(int ok, const char *what) {
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed++;
}

/* the clock, and the guest's memory: 8 bytes at each address that has been set, 0 anywhere else */
static int64_t now_ns = 10000000000LL;
static int64_t hos_now_ns(void) { return now_ns; }
static struct { uint64_t at, bits; } guest[256];
static int guest_size, guest_reads;
static void put64(uint64_t at, uint64_t bits) {
    for (int i = 0; i < guest_size; i++)
        if (guest[i].at == at) { guest[i].bits = bits; return; }
    guest[guest_size].at = at, guest[guest_size].bits = bits, guest_size++;
}
static void put_double(uint64_t at, double v) { uint64_t b; memcpy(&b, &v, 8); put64(at, b); }
static void put_float(uint64_t at, float v) { uint32_t b; memcpy(&b, &v, 4); put64(at, b); }
static uint64_t recomp_load64(Ctx *c, uint64_t at) {
    (void)c;
    guest_reads++;
    for (int i = 0; i < guest_size; i++)
        if (guest[i].at == at) return guest[i].bits;
    return 0;
}
static uint32_t recomp_load32(Ctx *c, uint64_t at) { return (uint32_t)recomp_load64(c, at); }

/* what the save says: whether one is loaded, and the map Link is in */
static int save_loaded, save_child = 1;
static char save_map[80];
static void hos_log(const char *fmt, ...) { (void)fmt; }
void rx_option_set(int id, int value) { (void)id, (void)value; }
static int oot_item_count(Ctx *c, const char *name) { (void)c, (void)name; return save_loaded ? 0 : -1; }
static int oot_gamedata_bool(Ctx *c, uint32_t hash, int *value) {
    (void)c, (void)hash;
    *value = save_child;
    return save_loaded;
}
static int oot_gamedata_string(Ctx *c, uint32_t hash, char *buf, size_t size) {
    (void)c, (void)hash;
    if (!save_loaded) return 0;
    snprintf(buf, size, "%s", save_map);
    return 1;
}
static int oot_pouch_count(Ctx *c, const char *name) { (void)c, (void)name; return -1; }
static void hos_movie_stats(int64_t out[5]) { memset(out, 0, 5 * sizeof out[0]); }

#include "oot_cheat_items.h"
#include "perf_pose_cut.h"

#define CAMERAS_SLOT 0x84ca6bc0ULL
#define VIEW_VTABLE 0x84c18a88ULL
enum { HOLDER = 0x1000, MANAGER = 0x1100, CAMERAS = 0x1200, CAMERA = 0x1300, VIEW = 0x1400, OTHER_VIEW = 0x1480, EVENT_HOLDER = 0x1500,
       EVENT_MGR = 0x1600, LINK = 0x2000 };

/* (the tap's camera: the bridge's status.game.camera, from oot_state_tap.c's state_camera, the same arithmetic) */
static double bridge_camera(const double eye[3], const double at[3]) {
    const double x = at[0] - eye[0], z = at[2] - eye[2];
    return atan2(x, z) * 57.29577951308232;
}

static void camera(const double eye[3], const double at[3]) {
    for (int i = 0; i < 3; i++) put_double(VIEW + 96 + i * 8, eye[i]), put_double(VIEW + 120 + i * 8, at[i]);
}

/* The camera looking along yaw (radians, 0 toward +z, pi/2 toward +x) and pitch (up positive) from a place */
static void look(double yaw, double pitch) {
    const double eye[3] = {-1200.5, 300.25, 800.75}, flat = cos(pitch) * 10;
    const double at[3] = {eye[0] + sin(yaw) * flat, eye[1] + sin(pitch) * 10, eye[2] + cos(yaw) * flat};
    camera(eye, at);
}

static void world(void) {
    put64(CAMERAS_SLOT, HOLDER), put64(HOLDER, MANAGER), put64(MANAGER + 32, 1), put64(MANAGER + 40, CAMERAS);
    put64(CAMERAS, CAMERA), put64(CAMERA + 120, VIEW), put64(VIEW, VIEW_VTABLE);
    put_float(VIEW + 0x1a4, 0.0f), put_float(VIEW + 0x18c, 0.0f);   /* (the fields the first version read: 0.0 in the game, all run) */
    put64(OOT_EVENTMGR_SLOT, EVENT_HOLDER), put64(EVENT_HOLDER, EVENT_MGR), put64(EVENT_MGR + 104, 0);
    put_float(LINK + 796, 250.0f), put_float(LINK + 800, 10.0f), put_float(LINK + 804, -30.0f);
    look(0, 0);
}

/* A Link update: the pose at most 4 times a second, from where the time is. Then what the renderer is given */
static Ctx ctx;
static void update(int64_t advance_ns) {
    now_ns += advance_ns;
    oot_perf_pose(&ctx, LINK);
}
static int ask(float pose[5], char *map) {
    int has = -1;
    int64_t movie[5];
    char text[120];
    hos_perf_info(text, sizeof text, movie, pose, &has);
    if (map) snprintf(map, sizeof text, "%s", text);
    return has;
}
static int has_pose(void) {
    float pose[5];
    return ask(pose, NULL) == 1;
}
static void refresh(void) {   /* (the map line's own clock: twice a second) */
    now_ns += 600000000LL;
    oot_inventory_refresh(&ctx, now_ns);
}
#define MS 1000000LL

int main(void) {
    const double pi = 3.14159265358979323846;
    float pose[5];
    char map[120];
    world();
    save_loaded = 1, snprintf(save_map, sizeof save_map, "Kokiri Forest");
    refresh();   /* the map line is set: "Kokiri Forest (child)" */

    /* off until the launcher's tick is on: no pose, and not one guest read */
    guest_reads = 0;
    update(300 * MS);
    check(guest_reads == 0 && !has_pose(), "off: no pose, no guest memory read");
    hos_perf_tracking(1);

    /* the pose follows the camera, in radians: yaw 0 toward +z and pi/2 toward +x, pitch up positive */
    look(0, 0), update(300 * MS);
    check(ask(pose, map) == 1 && pose[0] == 250.0f && pose[1] == 10.0f && pose[2] == -30.0f, "a pose: Link's place");
    check(fabs(pose[3]) < 1e-6 && fabs(pose[4]) < 1e-6, "looking along +z: yaw 0, pitch 0, though the view's +0x1a4 and +0x18c read 0.0");
    check(!strcmp(map, "Kokiri Forest (child)"), "and the map");
    look(pi / 2, 0), update(300 * MS);
    check(ask(pose, NULL) == 1 && fabs(pose[3] - pi / 2) < 1e-6, "looking toward +x: yaw pi/2");
    look(-pi / 2, 0), update(300 * MS);
    check(ask(pose, NULL) == 1 && fabs(pose[3] + pi / 2) < 1e-6, "looking toward -x: yaw -pi/2");
    look(0.5, pi / 4), update(300 * MS);
    check(ask(pose, NULL) == 1 && fabs(pose[3] - 0.5) < 1e-6 && fabs(pose[4] - pi / 4) < 1e-6, "looking up 45 degrees: pitch +pi/4, yaw kept");
    look(-2.0, -pi / 3), update(300 * MS);
    check(ask(pose, NULL) == 1 && fabs(pose[3] + 2.0) < 1e-6 && fabs(pose[4] + pi / 3) < 1e-6, "looking down 60 degrees: pitch -pi/3");

    /* in step with the bridge's camera (degrees, the same arithmetic) over a whole turn, at three heights */
    {
        int off_by = 0, count = 0;
        for (int pitch = -60; pitch <= 60; pitch += 60)
            for (int deg = -180; deg < 180; deg += 5) {
                look(deg * pi / 180, pitch * pi / 180);
                update(300 * MS);
                double eye[3], at[3];
                for (int i = 0; i < 3; i++) {
                    const uint64_t e = recomp_load64(&ctx, VIEW + 96 + i * 8), a = recomp_load64(&ctx, VIEW + 120 + i * 8);
                    memcpy(&eye[i], &e, 8), memcpy(&at[i], &a, 8);
                }
                count++;
                off_by += !(ask(pose, NULL) == 1 && fabs(pose[3] * 180 / pi - bridge_camera(eye, at)) < 1e-3);
            }
        check(off_by == 0 && count == 216, "yaw follows the bridge's camera (the degrees of status.game.camera) all round, at three pitches");
    }

    /* the relay's range, for every direction: |yaw| <= pi, |pitch| <= pi/2, as numbers, behind the camera and straight up included */
    {
        int out_of_range = 0, posed = 0, none = 0;
        for (int i = 0; i <= 3600; i++)
            for (int j = -90; j <= 90; j += 3) {
                look(-pi + i * (2 * pi / 3600), j * pi / 180);
                update(300 * MS);
                if (ask(pose, NULL) != 1) { none++; continue; }
                posed++;
                out_of_range += !(fabs((double)pose[3]) <= pi && fabs((double)pose[4]) <= pi / 2);
            }
        check(out_of_range == 0 && posed > 200000, "every direction gives yaw within pi and pitch within pi/2");
        check(none == 3601 * 2, "only straight up and straight down have no pose");
    }
    {   /* (looking straight behind, from either side of the axis: yaw is +pi or -pi, and is not past it) */
        const double eye[3] = {0, 0, 0}, behind[3] = {0, 0, -10}, behind_minus[3] = {-0.0, 0, -10};
        camera(eye, behind), update(300 * MS);
        check(ask(pose, NULL) == 1 && (double)pose[3] <= pi && (double)pose[3] > 3.14, "straight behind: yaw pi, not a hair over");
        camera(eye, behind_minus), update(300 * MS);
        check(ask(pose, NULL) == 1 && (double)pose[3] >= -pi && (double)pose[3] < -3.14, "and the other way round: yaw -pi");
    }
    {   /* (straight up and down: the yaw is not defined; a camera that does not move: none either; a stray value: none) */
        const double eye[3] = {10, 20, 30}, up[3] = {10, 25, 30}, still[3] = {10, 20, 30};
        camera(eye, up), update(300 * MS);
        check(!has_pose(), "looking straight up: no pose");
        camera(eye, still), update(300 * MS);
        check(!has_pose(), "a camera with its eye on its target: no pose");
        const double nan_eye[3] = {NAN, 0, 0}, far[3] = {INFINITY, 0, 1}, ok_at[3] = {0, 0, 10}, zero[3] = {0, 0, 0};
        camera(nan_eye, ok_at), update(300 * MS);
        check(!has_pose(), "an eye that is not a number: no pose");
        camera(zero, far), update(300 * MS);
        check(!has_pose(), "a target at infinity: no pose");
    }
    look(0.3, 0.1), update(300 * MS);
    check(has_pose(), "a normal view again: the pose is back");
    put_float(LINK + 796, 2e6f), update(300 * MS);
    check(!has_pose(), "a place no map has (a stray read): no pose");
    put_float(LINK + 796, 250.0f), update(300 * MS);
    check(has_pose(), "and back");

    /* the camera object is the gameplay view only while it is that type: a cutscene's is another object, whose floats mean nothing */
    put64(VIEW, 0x12345), update(300 * MS);
    check(!has_pose(), "a camera zero that is another kind of object: no pose");
    put64(VIEW, VIEW_VTABLE), update(300 * MS);
    check(has_pose(), "the gameplay camera again: the pose");

    /* a cutscene: an event is playing, whatever camera zero is: no pose at once, not a second later */
    update(300 * MS);
    check(has_pose(), "before the event: a pose");
    put64(EVENT_MGR + 104, 0x7000), update(300 * MS);
    check(!has_pose(), "an event is playing: no pose (the pose before it is not kept for its second)");
    update(300 * MS), update(300 * MS);
    check(!has_pose(), "and none all through it");
    put64(EVENT_MGR + 104, 0), update(300 * MS);
    check(has_pose(), "the event is over: the pose is back");

    /* the clock: Link's update reads at most 4 times a second; a pose not set for a second is none (the pause, a load) */
    look(0.3, 0.1), update(300 * MS);
    look(1.0, 0.1), update(100 * MS);
    check(ask(pose, NULL) == 1 && fabs(pose[3] - 0.3) < 1e-6, "an update 100 ms after the last reads nothing: the pose is the last one's");
    update(160 * MS);
    check(ask(pose, NULL) == 1 && fabs(pose[3] - 1.0) < 1e-6, "260 ms after it: read again");
    now_ns += 999 * MS;
    check(has_pose(), "999 ms after Link's last update: a pose");
    now_ns += 2 * MS;
    check(!has_pose(), "1001 ms after it: none (the pause, a load, the title)");
    update(300 * MS);
    check(has_pose(), "Link updates again: a pose");

    /* a map change: the pose of the map before is not the new one's, and not for a sample of it */
    snprintf(save_map, sizeof save_map, "Hyrule Field");
    refresh();
    check(ask(pose, map) == 0 && !strcmp(map, "Hyrule Field (child)"), "the map line changes: no pose until Link's update gives the new map's");
    update(300 * MS);
    check(ask(pose, map) == 1 && !strcmp(map, "Hyrule Field (child)"), "and the next update gives it, under the new map's name");
    refresh();
    check(ask(pose, NULL) == 1, "a refresh that finds the same map leaves the pose alone");

    /* the title (or the file select): no save loaded. The map is empty and so is the pose, even for Link's update just after
       the clear (the one that cleared it is Link's own), which a second of staleness would have served */
    save_loaded = 0;
    refresh();
    check(ask(pose, map) == 0 && map[0] == 0, "back at the title: no map, no pose");
    update(300 * MS);
    check(ask(pose, map) == 0 && map[0] == 0, "Link updated just after: still no pose with no map (never 'Title and intro' at a place)");
    now_ns += 400 * MS;
    check(ask(pose, map) == 0, "and a third of a second later");
    save_loaded = 1, snprintf(save_map, sizeof save_map, "Kokiri Forest");
    refresh(), update(300 * MS);
    check(ask(pose, map) == 1 && !strcmp(map, "Kokiri Forest (child)"), "a save is loaded again: map and pose");

    /* untick: nothing kept */
    hos_perf_tracking(0);
    check(!has_pose(), "unticked: no pose");
    guest_reads = 0;
    update(300 * MS);
    check(guest_reads == 0 && !has_pose(), "and no read of the guest while unticked");

    check(bad_lock == 0, "no lock taken twice, none released that was not held");
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
