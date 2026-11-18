"""Mesa's shader cache is one folder for every start of the game, and the driver's environment is in the game's log
(host/hos_main.c driver_cache_shared, driver_env_note).

Steam gives each entry of its library a folder of its own for Mesa's cache (MESA_SHADER_CACHE_DIR=.../steamapps/shadercache/
<app id>): the Builder's preparation, started from the setup's entry, filled one folder and the game's entry another (on an
AYN Odin 3, Turnip, Game Mode: 1736 MB in the setup's, 2 MB in the game's), and what the preparation left in the driver's cache
alone was compiled again in play. And the two entries ran the driver with other options (mesh shaders off in one, on in the
other), which the logs did not show. This cuts the two functions out of hos_main.c and runs them:
 - Steam's folder, or none, or an empty value, becomes the game folder's cache/driver (bin/longshot's folder's parent when
   the game is in bin/), and the game's log says so and what it replaced;
 - a folder the player named is kept, and nothing is said;
 - the driver's variables (TU_, MESA_, VK_, ENABLE_VK_ and the others, LD_LIBRARY_PATH) are listed for the log, values cut,
   others (HOME) left out, and a line says when none is set;
and checks main runs them after longshot_env.txt is read and before the window may load Vulkan, the list after every function
that changes the environment, and that game_main's driver_env_log says both once its log is open.
python -I -B tests/test_driver_cache_shared.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "the Linux main's code (setenv, environ)"}

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef unsigned int DWORD;
/* (host/hos_paths.h's, standing in: the game's own file as TEST_GAME_PATH names it) */
static DWORD hos_game_path_a(char *buf, DWORD size) {
    const char *p = getenv("TEST_GAME_PATH");
    if (!p) return 0;
    snprintf(buf, size, "%%s", p);
    return (DWORD)strlen(buf);
}
static void hos_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("LOG=");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
%s
int main(void) {
    driver_cache_shared();
    driver_env_note();
    const char *v = getenv("MESA_SHADER_CACHE_DIR");
    printf("MESA_SHADER_CACHE_DIR=%%s\n", v ? v : "(unset)");
    driver_env_log();
    return 0;
}
"""


def function(source, signature):
    start = source.index(signature)
    first = source.index("{", start)
    depth, at = 1, first + 1
    while depth:
        depth += {"{": 1, "}": -1}.get(source[at], 0)
        at += 1
    return source[start:at]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    cc = shutil.which(args.cc)
    if not cc:
        raise SystemExit("C compiler not found: " + args.cc)
    src = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    for name in ("driver_cache_shared", "driver_env_note"):
        if "static void %s(void) {" % name not in src:
            raise SystemExit("FAIL: host/hos_main.c has no %s: Steam's per-entry Mesa cache folders split the preparation's "
                             "driver cache from the game's, and the driver's environment is not in the log" % name)
    problems = []
    main_fn = src[src.rindex("int main(int argc, char **argv) {"):]
    order = ["load_longshot_env();", "driver_cache_shared();", "driver_env_choose();", "driver_env_note();", "hos_display_init()"]
    if any(o not in main_fn for o in order):
        problems.append("the Linux main does not call " + ", ".join(o for o in order if o not in main_fn))
    elif [main_fn.index(o) for o in order] != sorted(main_fn.index(o) for o in order):
        problems.append("main does not run driver_cache_shared after longshot_env.txt is read, driver_env_note after the "
                        "environment's last change, and both before the window may load Vulkan")
    game = function(src, "static int game_main(int argc, char **argv) {")
    if "driver_env_log();" not in game or game.index("driver_env_log();") < game.index("freopen(path"):
        problems.append("game_main does not say the driver's environment once its log is open")
    log_fn = function(src, "static void driver_env_log(void) {")
    buffers = "\n".join(line for line in src.splitlines()
                        if line.startswith(("static char driver_env_said[", "static char driver_cache_said[",
                                            "static char driver_env_seen[")))
    code = "\n".join([buffers, "extern char **environ;", function(src, "static void driver_cache_shared(void) {"),
                      function(src, "static void driver_env_note(void) {"), log_fn])
    with tempfile.TemporaryDirectory(prefix="longshot-driver-cache-") as folder:
        tmp = Path(folder)
        (tmp / "t.c").write_text(HARNESS % code, encoding="utf-8")
        r = subprocess.run([cc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", str(tmp / "t.c"), "-o", str(tmp / "t")],
                           capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit("FAIL: the functions do not compile\n" + r.stdout + r.stderr)

        def run(env):
            out = subprocess.run([str(tmp / "t")], env=env, capture_output=True, text=True, timeout=30)
            if out.returncode:
                raise SystemExit("FAIL: the harness ended with %d\n%s" % (out.returncode, out.stderr))
            got = {"LOG": []}
            for line in out.stdout.splitlines():
                key, _, value = line.partition("=")
                if key == "LOG":
                    got["LOG"].append(value)
                else:
                    got[key] = value
            return got

        drivers = ("TU_", "FD_", "MESA_", "RADV_", "ACO_", "ANV_", "NVK_", "VK_", "ENABLE_VK_", "DISABLE_VK_", "__GL_",
                   "LD_LIBRARY_PATH", "LD_PRELOAD")
        base = {k: v for k, v in os.environ.items() if not k.startswith(drivers)}
        base["TEST_GAME_PATH"] = "/games/Longshot/longshot"
        want = "/games/Longshot/cache/driver"
        steam_dir = "/home/deck/.local/share/Steam/steamapps/shadercache/3333777733"
        for name, env, said in (("Steam's folder", dict(base, MESA_SHADER_CACHE_DIR=steam_dir), steam_dir),
                                ("no folder named", base, "none was named"),
                                ("an empty value", dict(base, MESA_SHADER_CACHE_DIR=""), "none was named")):
            got = run(env)
            if got["MESA_SHADER_CACHE_DIR"] != want:
                problems.append("%s: Mesa's cache is %r, not the game folder's %r" % (name, got["MESA_SHADER_CACHE_DIR"], want))
            cache_lines = [line for line in got["LOG"] if "Mesa's shader cache" in line]
            if len(cache_lines) != 1 or want not in cache_lines[0] or said not in cache_lines[0]:
                problems.append("%s: the game's log does not say where Mesa's cache is and what it replaced: %r" % (name, got["LOG"]))
        got = run(dict(base, TEST_GAME_PATH="/games/Longshot/bin/longshot", MESA_SHADER_CACHE_DIR=steam_dir))
        if got["MESA_SHADER_CACHE_DIR"] != "/games/Longshot/bin/cache/driver":
            problems.append("the folder is not the one hos_game_path_a answers for: %r" % got["MESA_SHADER_CACHE_DIR"])
        mine = "/home/deck/my-mesa-cache"
        got = run(dict(base, MESA_SHADER_CACHE_DIR=mine))
        if got["MESA_SHADER_CACHE_DIR"] != mine or any("Mesa's shader cache" in line for line in got["LOG"]):
            problems.append("a folder the player named is not kept as it is, or is said to be changed: %r" % got)
        got = run(dict(base, MESA_SHADER_CACHE_DIR=steam_dir, TU_DEBUG="noubwc", VK_ICD_FILENAMES="/usr/share/vulkan/icd.d/freedreno_icd.json",
                       ENABLE_VK_LAYER_VALVE_steam_fossilize_1="1", LD_LIBRARY_PATH="/tmp/.mount_x/usr/lib",
                       FD_DEV_FEATURES="x" * 600, HOME="/home/deck"))
        env_lines = [line for line in got["LOG"] if line.startswith("vk: the driver's environment:")]
        if len(env_lines) != 1:
            problems.append("the driver's environment is not one line of the log: %r" % got["LOG"])
        else:
            line = env_lines[0]
            for part in ("TU_DEBUG=noubwc", "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/freedreno_icd.json",
                         "ENABLE_VK_LAYER_VALVE_steam_fossilize_1=1", "LD_LIBRARY_PATH=/tmp/.mount_x/usr/lib",
                         "MESA_SHADER_CACHE_DIR=" + want, "MESA_SHADER_CACHE_MAX_SIZE" if False else "TU_DEBUG"):
                if part not in line:
                    problems.append("the driver's environment line leaves out %s: %r" % (part, line))
            if "HOME=" in line:
                problems.append("the driver's environment line names variables no driver reads: %r" % line)
            if "x" * 300 in line or "FD_DEV_FEATURES=" + "x" * 200 not in line:
                problems.append("a long value is not cut in the driver's environment line")
        got = run(dict(base, MESA_SHADER_CACHE_DIR=mine))
        env_lines = [line for line in got["LOG"] if line.startswith("vk: the driver's environment:")]
        if len(env_lines) != 1 or "MESA_SHADER_CACHE_DIR=" + mine not in env_lines[0]:
            problems.append("the environment line does not list what is set: %r" % got["LOG"])
        none = {k: v for k, v in base.items()}
        got = run(none)
        env_lines = [line for line in got["LOG"] if line.startswith("vk: the driver's environment:")]
        # (driver_cache_shared sets MESA_SHADER_CACHE_DIR, so the list is never empty after it: with it kept, unset by hand)
        if len(env_lines) != 1 or "MESA_SHADER_CACHE_DIR=" + want not in env_lines[0]:
            problems.append("with nothing set, the environment line does not show the folder chosen: %r" % got["LOG"])
    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    print("PASS: Mesa's shader cache is the game folder's for every start (Steam's per-entry folder replaced, the player's kept), "
          "and the driver's environment is in the game's log")


if __name__ == "__main__":
    main()
