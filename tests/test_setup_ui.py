"""The setup window's logic (builder/linux/setup_ui/setup_logic.h), game-free: the ARM64 setup AppImage's window for Game Mode.

  - make_game.py's builder lines (@@stage, @@progress, @@plan, @@file, @@eta, @@times, @@check, @@done, @@error) are read as
    its docstring says, every other line is the log, and a malformed one is neither;
  - the progress bar: the stages' spans are make_game.py's STAGE_SPAN (read from its source), a stage's @@progress fills its
    span, the bar does not go back, a stage that comes again (the lift after a stale kept translation) starts from its
    beginning, @@done fills it; the compile's time left counts down from its @@eta;
  - the pipe's bytes become lines (\\n, \\r\\n, a lone \\r), the log keeps its last lines;
  - the settings file reads back what it wrote; the Steam request's URL is urllib.parse.quote's, and the Steam entry's script
    starts the folder's own longshot with the arguments it was given (run here on Linux);
  - the times, sizes and paths it shows.

    python -I -B tests/test_setup_ui.py [--cxx clang++]

The window itself (setup_ui.cpp: SDL2 and Dear ImGui) is built and drawn by build_docker.sh setup, which runs its --selftest:
every screen, and a real run of a stand-in build to its end.
"""
import argparse
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "the ARM64 Linux setup AppImage's window: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "builder" / "linux" / "setup_ui" / "setup_logic.h"

PATHS = ["/home/deck/Games/Longshot/Longshot - Ocarina of Time", "/run/media/deck/SD card/Игры/Longshot/x",
         "/a&b=c?d#e%f+g~h_i.j-k"]
DRIVER = "/usr/share/armada/turnip/owner's build $(exit 9)/icd.aarch64.json"

FIXTURE = r'''
#include "setup_logic.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
using namespace setup;
static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
static void out_stages() {
    for (int i = 0; i < STAGE_COUNT; i++) std::printf("STAGE %s %.17g %.17g\n", STAGES[i].id, STAGES[i].lo, STAGES[i].hi);
}
int main() {
    out_stages();
    // ---- the lines
    Event e = parse_line("@@stage compile Compiling the game\r\n");
    CHECK(e.kind == Kind::Stage && e.id == "compile" && e.text == "Compiling the game");
    e = parse_line("@@progress 0.25");
    CHECK(e.kind == Kind::Progress && near(e.a, 0.25));
    CHECK(parse_line("@@progress lots").kind == Kind::Other);
    CHECK(parse_line("@@progress").kind == Kind::Other);
    e = parse_line("@@plan 120 1900");
    CHECK(e.kind == Kind::Plan && near(e.a, 120) && near(e.b, 1900));
    e = parse_line("@@file code_12.c 3.5 new");
    CHECK(e.kind == Kind::File && e.id == "code_12.c" && near(e.a, 3.5) && e.text == "new");
    e = parse_line("@@eta 600");
    CHECK(e.kind == Kind::Eta && near(e.a, 600));
    e = parse_line("@@times 4321 3600 -1");
    CHECK(e.kind == Kind::Times && near(e.a, 4321) && near(e.b, 3600) && near(e.c, -1));
    e = parse_line("@@check ok Ocarina of Time 131072");
    CHECK(e.kind == Kind::Check && e.id == "ok" && e.text == "Ocarina of Time 131072");
    e = parse_line("@@done /home/deck/Games/Longshot");
    CHECK(e.kind == Kind::Done && e.text == "/home/deck/Games/Longshot");
    e = parse_line("@@error Not enough space: 12 GB free, 30 GB needed");
    CHECK(e.kind == Kind::Error && e.text == "Not enough space: 12 GB free, 30 GB needed");
    e = parse_line("@@something new");
    CHECK(e.kind == Kind::Other && e.id == "something");
    e = parse_line("  code_1.c   20 MB   3s  (600s)");
    CHECK(e.kind == Kind::Log && e.text == "  code_1.c   20 MB   3s  (600s)");
    CHECK(parse_line("progress 0.5 Compiling").kind == Kind::Log);   // (--prepare's line is not this protocol)
    CHECK(parse_line("").kind == Kind::Log);

    // ---- the progress
    Progress p;
    CHECK(near(p.overall(), 0) && p.stage == -1);
    double t = 1000;
    p.feed(parse_line("@@stage read Reading your game files"), t);
    CHECK(p.stage == 0 && near(p.overall(), STAGES[0].lo));
    p.feed(parse_line("@@progress 0.5"), t);
    CHECK(near(p.overall(), STAGES[0].lo + (STAGES[0].hi - STAGES[0].lo) * 0.5));
    p.feed(parse_line("@@progress 7"), t);   // (held to its stage)
    CHECK(near(p.overall(), STAGES[0].hi));
    p.feed(parse_line("@@stage lift Translating the game to PC code"), t);
    CHECK(p.stage == 1 && (p.stages_done & 1) && p.stage_text == "Translating the game to PC code");
    CHECK(near(p.overall(), STAGES[0].hi));   // (not back to the lift's start, which is the read's end)
    p.feed(parse_line("@@stage compile Compiling"), t);
    p.feed(parse_line("@@plan 1830 1900"), t);
    for (int i = 0; i < 5; i++) p.feed(parse_line("@@file code.c 1 new"), t);
    CHECK(p.files == 5 && p.plan_changed == 1830 && p.plan_total == 1900 && (p.stages_done & 2));
    p.feed(parse_line("@@eta 600"), t);
    CHECK(near(p.eta_left(t + 100), 500) && near(p.eta_left(t + 1000), 0));
    p.feed(parse_line("@@progress 0.5"), t);
    const double mid = STAGES[2].lo + (STAGES[2].hi - STAGES[2].lo) * 0.5;
    CHECK(near(p.overall(), mid));
    p.feed(parse_line("@@progress 0.2"), t);   // (a step's own count going back: the bar stays)
    CHECK(near(p.overall(), mid));
    // (a stale kept translation: the lift and the compile again, from their beginnings)
    p.feed(parse_line("@@stage lift Translating the game to PC code"), t);
    CHECK(p.stage == 1 && !(p.stages_done & 2) && near(p.overall(), STAGES[1].lo));
    p.feed(parse_line("@@stage compile Compiling"), t);
    CHECK(p.files == 0 && (p.stages_done & 4) == 0 && (p.stages_done & 2));   // (the compile's own bit: not done yet)
    p.feed(parse_line("@@stage finish Putting the game folder together"), t);
    p.feed(parse_line("the shaders were not prepared now (no Vulkan device): the game's first start prepares them"), t);
    CHECK(p.shaders_later);
    p.feed(parse_line("@@times 4321 3600 4800"), t);
    p.feed(parse_line("@@done /g"), t);
    CHECK(p.done && !p.failed && p.folder == "/g" && near(p.overall(), 1) && p.stages_done == 15);
    CHECK(near(p.total_s, 4321) && near(p.previous_s, 4800) && p.eta_left(t) < 0);
    Progress q;
    q.feed(parse_line("@@stage check Checking"), t);   // (a stage the window doesn't list: its text, no row)
    CHECK(q.stage == -1 && q.stage_text == "Checking");
    q.feed(parse_line("@@error The compiler stopped"), t);
    CHECK(q.failed && q.error == "The compiler stopped" && q.eta_left(t) < 0);

    // ---- the pipe's bytes, the log
    std::vector<std::string> got;
    Splitter sp;
    auto put = [&](const std::string& l) { got.push_back(l); };
    const char* a = "one\ntw";
    sp.feed(a, std::strlen(a), put);
    const char* b = "o\r\nthree\rfour\n\nfi";
    sp.feed(b, std::strlen(b), put);
    sp.finish(put);
    CHECK(got.size() == 6 && got[0] == "one" && got[1] == "two" && got[2] == "three" && got[3] == "four" && got[4] == "" &&
          got[5] == "fi");
    Lines lines;
    lines.cap = 3;
    for (int i = 0; i < 10; i++) lines.add(std::to_string(i));
    CHECK(lines.lines.size() == 3 && lines.lines.front() == "7" && lines.lines.back() == "9" && lines.total == 10);

    // ---- settings
    const auto s = parse_settings(format_settings({{"nup", "/a b/game.nup"}, {"out", "/g=h"}, {"bad", "x\ny"}}));
    CHECK(setting(s, "nup") == "/a b/game.nup" && setting(s, "out") == "/g=h" && setting(s, "bad") == "");
    CHECK(setting(parse_settings("# c\r\nnup=x\r\n=y\nno equals\n"), "nup") == "x");

    // ---- words and paths
    CHECK(duration(45) == "45 s" && duration(12 * 60 + 10) == "12 min" && duration(3600 + 5 * 60) == "1 h 05 min");
    CHECK(duration(-3) == "0 s" && duration(3599) == "59 min");
    CHECK(clock_text(754) == "12:34" && clock_text(3723) == "1:02:03");
    CHECK(size_text(5600000000ull) == "5.6 GB" && size_text(312000000) == "312 MB" && size_text(4096) == "4 KB" &&
          size_text(12) == "12 B");
    CHECK(parent("/a/b/") == "/a" && parent("/a") == "/" && parent("/") == "/" && base_name("/a/b/") == "b");
    CHECK(join("/a", "b") == "/a/b" && join("/", "b") == "/b" && join("/a/", "b") == "/a/b");
    CHECK(is_game_file("Zelda.NUP") && is_game_file("x.nup") && !is_game_file("x.nup.part") && !is_game_file("nup"));
    CHECK(log_name(2026, 11, 7, 9, 5, 3) == "build-20261107-090503.log");
    CHECK(std::string(STEAM_NAME) != "longshot" && std::string(STEAM_NAME) != "Longshot");

    // ---- for the script's half: the URLs, and the Steam script's text
    for (const char* path : {@PATHS@}) std::printf("URL %s\n", steam_url(path).c_str());
    std::printf("SCRIPT_BEGIN\n%sSCRIPT_END\n", steam_script().c_str());
    std::printf("PINNED_BEGIN\n%sPINNED_END\n", steam_script("@DRIVER@").c_str());
    std::printf("NAME %s\n", STEAM_NAME);
    if (fails) return 1;
    std::puts("PASS: setup_logic.h");
    return 0;
}
'''


def fail(why):
    raise SystemExit("FAIL: " + why)


def make_game_spans():
    """make_game.py's STAGE_SPAN, read from its source (ast: no import, no game)"""
    src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
    for node in ast.parse(src).body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", "") == "STAGE_SPAN" for t in node.targets):
            return ast.literal_eval(node.value), src
    fail("nx2/make_game.py has no STAGE_SPAN")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    a = p.parse_args()
    fixture = FIXTURE.replace("@PATHS@", ", ".join('"%s"' % x.replace("\\", "\\\\").replace('"', '\\"') for x in PATHS))
    fixture = fixture.replace("@DRIVER@", DRIVER)
    with tempfile.TemporaryDirectory(prefix="setup-ui-") as tmp:
        tmp = Path(tmp)
        src, exe = tmp / "fixture.cpp", tmp / ("fixture.exe" if os.name == "nt" else "fixture")
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(HEADER.parent), str(src), "-o", str(exe)],
                       check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, encoding="utf-8")
        print(r.stdout.split("SCRIPT_BEGIN")[0].strip())
        if r.returncode:
            fail("setup_logic.h's checks:\n" + r.stdout + r.stderr)
        out = r.stdout

        # (the bar's spans are make_game.py's, and every stage it says is one the window lists)
        spans, src_text = make_game_spans()
        ours = {m.group(1): (float(m.group(2)), float(m.group(3))) for m in re.finditer(r"^STAGE (\S+) (\S+) (\S+)$", out, re.M)}
        if ours != {k: tuple(v) for k, v in spans.items()}:
            fail("setup_logic.h STAGES %r is not make_game.py's STAGE_SPAN %r" % (ours, spans))
        said = set(re.findall(r'say\("stage", "(\w+)"', src_text))
        if not said or not said <= set(ours):
            fail("make_game.py says stages the window doesn't list: %s" % sorted(said - set(ours)))
        # (the builder lines the window reads are the ones make_game.py's docstring lists)
        listed = set(re.findall(r"^\s+@@(\w+) ", src_text.split('"""')[1], re.M))
        known = {"stage", "progress", "plan", "file", "eta", "times", "check", "done", "error"}
        if listed != known:
            fail("make_game.py's builder lines %s are not the ones the window reads %s" % (sorted(listed), sorted(known)))

        # (Steam's URL, as TotK Recomp's installer/games.py makes it)
        urls = re.findall(r"^URL (.*)$", out, re.M)
        want = ["steam://addnonsteamgame/" + urllib.parse.quote(x, safe="") for x in PATHS]
        if urls != want:
            fail("the Steam URLs %r are not urllib.parse.quote's %r" % (urls, want))

        # (the Steam entry's script: from a game folder, through a link as Steam may keep it, it runs the folder's longshot
        # with the arguments)
        script = out.split("SCRIPT_BEGIN\n")[1].split("SCRIPT_END")[0]
        pinned = out.split("PINNED_BEGIN\n")[1].split("PINNED_END")[0]
        name = re.search(r"^NAME (.*)$", out, re.M).group(1)
        readlink = shutil.which("readlink")
        if readlink and subprocess.run([readlink, "-f", str(tmp)], capture_output=True).returncode == 0:
            game = tmp / "Games" / "Longshot"
            game.mkdir(parents=True)
            (game / name).write_text(script, encoding="utf-8")
            (game / name).chmod(0o755)
            (game / "longshot").write_text('#!/bin/sh\necho "started in $(pwd) with: $*"\necho "$0"\n'
                                           'printf "%s\\n" "$VK_DRIVER_FILES" "$VK_ICD_FILENAMES" "${VK_LOADER_DRIVERS_SELECT:-}"\n',
                                           encoding="utf-8")
            (game / "longshot").chmod(0o755)
            os.symlink(game / name, tmp / "link")
            env = dict(os.environ, VK_DRIVER_FILES="current", VK_ICD_FILENAMES="current", VK_LOADER_DRIVERS_SELECT="current")
            args = [str(tmp / "link"), "--a", "b c"]
            r = subprocess.run(args, capture_output=True, text=True, cwd=tmp, env=env)
            if r.returncode or "with: --a b c" not in r.stdout or str(game / "longshot") not in r.stdout:
                fail("the Steam entry's script did not start the folder's longshot: %r %r" % (r.stdout, r.stderr))
            if r.stdout.splitlines()[-3:] != ["current"] * 3:
                fail("the Steam entry without a driver match must keep Steam's driver environment")
            (game / name).write_text(pinned, encoding="utf-8")
            r = subprocess.run(args, capture_output=True, text=True, cwd=tmp, env=env)
            if r.returncode or r.stdout.splitlines()[-3:] != [DRIVER, DRIVER, ""] or "with: --a b c" not in r.stdout:
                fail("the Steam entry did not pass the selected driver to Longshot: %r %r" % (r.stdout, r.stderr))
    print("PASS: the setup window's logic")


if __name__ == "__main__":
    main()
