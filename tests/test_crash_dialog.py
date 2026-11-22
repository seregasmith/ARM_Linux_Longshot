"""The window right after a crash (host/hos_crash_dialog.c, host/hos_crash_dialog.h), with no game and no window.

- The helper's command line (--crash-watch <pid> <run id> <game folder> <log>, --crash-show <record> <game folder> <log>):
  what is taken, what is refused (a bad pid or run id, a record name that is a path, a missing argument), and that the game's
  own command lines are not the helper's.
- Which starts get a helper: none for --prepare-shaders, HOS_HEADLESS, HOS_CRASH_WATCH=0, HOS_PGO, a helper itself, or a run
  with no run id.
- A Windows argument quoted for CreateProcessW comes back whole through CommandLineToArgvW's rules (this file's own copy).
- The was-it-a-crash rule: a crash record shows, after a quit too; a start the log says could not go on shows; an unclean end
  shows unless the player had asked to quit or Windows' exit code is 0; a clean end, a restart says nothing.
- The cause from the record and the log (game files, Vulkan, the window, memory, the graphics driver, ended from outside,
  not known) and the stage (the start, the shaders' preparation, play).
- The log's redaction is the Insert menu's Bug report's (gpu/vk/vk_bug_report.cpp redact, compiled beside it: the same
  output for the same lines); a setting's value is shown only when it is a number, a size or on/off.
- The issue link: the game-problem form (.forgejo/ISSUE_TEMPLATE/1-game-problem.yml, which has the fields it fills), at
  most 2000 characters.
- The wiring in the source: main() hands a helper's command line over first, the helper starts after the crash record and
  before the guest and the renderer, a quit is noted first in rx_close_asked, HOS_CRASH_TEST is not taken from
  longshot_env.txt, the helper gets no console and no handle of the game's.
- On Linux, the real helper with the window, the clipboard and the browser stood in for (-DHOS_CRASH_DIALOG_TEST), against
  real game processes: a crash, a kill, a quit then a kill, a clean end, a restart in the same process, a start that could
  not start Vulkan, the window not shown for an earlier crash at the next start, and Report's text: no saves, game files,
  keys, home folder, user name or private setting values in it.

    python -I -B tests/test_crash_dialog.py [--cxx g++]
"""
import argparse
import calendar
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
from urllib.parse import parse_qs, urlsplit

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
RUN = "0123456789abcdef0123456789abcdef"

FIXTURE = r"""
#include "hos_crash_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    const size_t got = fread(b, 1, (size_t)n, f);
    b[got] = 0;
    fclose(f);
    return b;
}
static void facts_of(const char *path, HosCrashLog *f) {
    memset(f, 0, sizeof *f);
    char *text = strcmp(path, "-") ? slurp(path) : NULL;
    if (!text) return;
    for (char *line = text, *next; *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        hos_crash_log_line(f, line);
    }
    free(text);
}
int main(int argc, char **argv) {
    const char *cmd = argv[1];
    if (!strcmp(cmd, "args")) {
        HosCrashArgs a;
        const int m = hos_crash_args(argc - 2, argv + 2, &a);   /* (argv[2]: main's argv[0]) */
        printf("%d|%llu|%s|%s|%s|%s\n", m, (unsigned long long)a.pid, a.run_id, a.record ? a.record : "", a.dir ? a.dir : "",
               a.log ? a.log : "NULL");
    } else if (!strcmp(cmd, "options")) {   /* (the helper's command line: the run's settings file after the log) */
        HosCrashArgs a;
        const int m = hos_crash_args(argc - 2, argv + 2, &a);   /* (argv[2]: main's argv[0]) */
        printf("%d|%s\n", m, a.options ? a.options : "NULL");
    } else if (!strcmp(cmd, "wanted")) {   /* headless off pgo gamescope run [args...] */
        const char *run = strcmp(argv[6], "none") ? argv[6] : NULL;
        printf("%d\n", hos_crash_watch_wanted(argc - 6, argv + 6, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), run));
    } else if (!strcmp(cmd, "gamescope")) {   /* GAMESCOPE_WAYLAND_DISPLAY XDG_CURRENT_DESKTOP ("-": unset) */
        printf("%d\n", hos_crash_under_gamescope(strcmp(argv[2], "-") ? argv[2] : NULL, strcmp(argv[3], "-") ? argv[3] : NULL));
    } else if (!strcmp(cmd, "cliptools")) {   /* wayland x11 */
        const char *const *t;
        for (int i = 0; (t = hos_crash_clip_tool(atoi(argv[2]), atoi(argv[3]), i)); i++) {
            for (int k = 0; t[k]; k++) printf("%s%s", k ? " " : "", t[k]);
            printf("\n");
        }
    } else if (!strcmp(cmd, "ready")) {   /* copied opened saved-path|- */
        char out[8192];
        hos_crash_ready_text(out, sizeof out, atoi(argv[2]), atoi(argv[3]), strcmp(argv[4], "-") ? argv[4] : NULL,
                             "https://example.invalid/issues/new/choose");
        printf("%s\n", out);
    } else if (!strcmp(cmd, "quote")) {
        char out[512];
        const size_t n = hos_crash_quote_arg(out, (size_t)atoi(argv[2]), argv[3]);
        printf("%s\n", n ? out : "<none>");
    } else if (!strcmp(cmd, "log")) {
        HosCrashLog f;
        facts_of(argv[2], &f);
        printf("map=%d preparing=%d prepared=%d files=%d vulkan=%d window=%d reserve=%d memory=%d low=%d lost=%d stage=%d failed=%d\n",
               f.map, f.preparing, f.prepared, f.files, f.vulkan, f.window, f.reserve, f.memory, f.low_memory, f.device_lost,
               hos_crash_stage(&f), hos_crash_start_failed(&f));
        printf("version=%s\ngpu=%s\ndriver=%s\ndevices=%s\ndlss=%s\nfsr4=%s\n", f.version, f.gpu, f.driver, f.devices, f.dlss, f.fsr4);
        printf("os=%s\ncpu=%s\nrenderer=%s\nplace=%s\ncrash=%s\n", f.os, f.cpu, f.renderer, f.place, f.crash);
        char sent[sizeof f.gpu_report];
        hos_crash_gpu(sent, sizeof sent, &f);
        printf("gpu_sent=%s\n", sent);
    } else if (!strcmp(cmd, "setting")) {   /* a line */
        char key[64], value[32];
        if (hos_crash_setting(argv[2], key, sizeof key, value, sizeof value)) printf("%s|%s\n", key, value);
        else printf("-\n");
    } else if (!strcmp(cmd, "release")) {   /* first-line version, the stamp's source= */
        char version[128], source[64];
        hos_crash_version_split(argv[2], version, sizeof version, source, sizeof source);
        printf("%s|%s|%d|%d\n", version, source, hos_crash_release_version(version), hos_crash_release(version, source, argv[3]));
    } else if (!strcmp(cmd, "verdict")) {   /* record log have_exit exit_code replaced [send] */
        HosCrashRecord r;
        HosCrashLog f;
        char *text = strcmp(argv[2], "-") ? slurp(argv[2]) : NULL;
        hos_crash_record_parse(text ? text : "", &r);
        facts_of(argv[3], &f);
        const int have = atoi(argv[4]);
        const unsigned long code = strtoul(argv[5], NULL, 0);
        const int v = hos_crash_verdict(&r, &f, have, code, atoi(argv[6]));
        /* (no log, "-": the helper's stage when it found none) */
        const int cause = hos_crash_cause(v, &r, &f, have, code), stage = strcmp(argv[3], "-") ? hos_crash_stage(&f) : HOS_STAGE_UNKNOWN;
        char message[1400];
        hos_crash_message(message, sizeof message, v, stage, cause, argc > 7 && atoi(argv[7]));
        printf("%d %d %d\n%s\n%s\n", v, cause, stage, hos_crash_title(v), message);
        free(text);
    } else if (!strcmp(cmd, "redact")) {   /* home user windows file */
        char *text = slurp(argv[5]);
        for (char *line = text, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            next = next ? next + 1 : line + strlen(line);
            char out[8192];
            hos_crash_redact_line(out, sizeof out, line, (size_t)(next - line), argv[2], argv[3], atoi(argv[4]));
            fputs(out, stdout);
        }
        free(text);
    } else if (!strcmp(cmd, "shown")) {
        printf("%d\n", hos_crash_value_shown(strcmp(argv[2], "-") ? argv[2] : NULL, argv[3]));
    } else if (!strcmp(cmd, "url")) {
        char out[4096];
        hos_crash_issue_url(out, sizeof out, argv[2], argv[3], argv[4], argv[5], argv[6], argc > 7 ? argv[7] : "",
                            argc > 8 ? argv[8] : "", argc > 9 ? argv[9] : "");
        printf("%s\n", out);
    } else if (!strcmp(cmd, "epoch")) {
        printf("%lld\n", (long long)hos_crash_epoch(argv[2]));
    } else if (!strcmp(cmd, "atstart")) {   /* name shown-list now */
        printf("%d\n", hos_crash_show_at_start(argv[2], argv[3], strtoll(argv[4], NULL, 10), argc > 5 && atoi(argv[5])));
    } else if (!strcmp(cmd, "cgevents")) {   /* a /proc/<pid>/cgroup file */
        char *text = slurp(argv[2]), out[512];
        printf("%d %s\n", hos_crash_cgroup_events(text ? text : "", out, sizeof out), out);
        free(text);
    } else if (!strcmp(cmd, "oomkills")) {   /* a memory.events file */
        char *text = slurp(argv[2]);
        printf("%ld\n", hos_crash_oom_kills(text ? text : ""));
        free(text);
    } else if (!strcmp(cmd, "recname")) {
        char when[17] = "";
        printf("%d %s\n", hos_crash_record_name(argv[2], when), when);
    } else if (!strcmp(cmd, "address")) {   /* a stamp file, a key */
        char *stamp = slurp(argv[2]), out[600] = "";
        printf("%d|%s\n", stamp ? hos_crash_stamp_address(stamp, argv[3], out, sizeof out) : -1, out);
        free(stamp);
    } else if (!strcmp(cmd, "offered")) {   /* record ("-": none) have_address sent_already */
        printf("%d\n", hos_crash_send_offered(strcmp(argv[2], "-") ? argv[2] : NULL, atoi(argv[3]), atoi(argv[4])));
    } else if (!strcmp(cmd, "autosend")) {   /* a longshot_crash.ini file */
        char *ini = slurp(argv[2]);
        printf("%d\n", hos_crash_auto_send(ini ? ini : ""));
        free(ini);
    } else if (!strcmp(cmd, "answer")) {   /* status body */
        char out[1000];
        int again = 0;
        const int sent = hos_crash_answer(out, sizeof out, atoi(argv[2]), argv[3], &again);
        printf("%d|%d|%s\n", sent, again, out);
    } else if (!strcmp(cmd, "status")) {   /* curl's --dump-header file */
        char *h = slurp(argv[2]);
        printf("%d\n", hos_crash_http_status(h ? h : ""));
        free(h);
    } else if (!strcmp(cmd, "crashline")) {   /* a log line */
        char out[600];
        const int k = hos_crash_report_line(out, sizeof out, argv[2]);
        printf("%d|%s\n", k, k ? out : "");
    } else if (!strcmp(cmd, "clean")) {   /* text limit home user windows [device] */
        char buf[4096];
        const char *dev[1] = {argc > 7 ? argv[7] : ""};
        snprintf(buf, sizeof buf, "%s", argv[2]);
        hos_crash_clean(buf, sizeof buf, (size_t)atoi(argv[3]), argv[4], argv[5], atoi(argv[6]), dev, argc > 7);
        printf("%s\n", buf);
    } else if (!strcmp(cmd, "cleancap")) {   /* text cap device: in a buffer of exactly cap bytes, with a guard after them */
        const size_t cap = (size_t)atoi(argv[3]);
        unsigned char *buf = (unsigned char *)malloc(cap + 64);
        memset(buf, 0x5a, cap + 64);
        snprintf((char *)buf, cap, "%s", argv[2]);
        const char *dev[1] = {argv[4]};
        hos_crash_clean((char *)buf, cap, 300, "/h", "u", 0, dev, 1);
        int kept = 1;
        for (size_t i = cap; i < cap + 64; i++) kept &= buf[i] == 0x5a;
        printf("%s|%s\n", kept && strlen((char *)buf) < cap ? "ok" : "overrun", (char *)buf);
        free(buf);
    } else if (!strcmp(cmd, "json")) {
        char out[600];
        hos_crash_json(out, sizeof out, argv[2]);
        printf("%s\n", out);
    } else {
        return 2;
    }
    return 0;
}
"""

MENU_REDACT = r"""
#include "vk_bug_report.h"
#include <cstdio>
#include <fstream>
#include <string>
int main(int argc, char **argv) {
    (void)argc;
    std::ifstream f(argv[4], std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        const std::string out = mkwvk::bug_report::redact(line + "\n", argv[1], argv[2], argv[3][0] == '1');
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
}
"""

DRIVER = r"""
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hos_crash_dialog.h"
#include "hos_crash_record.h"
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int hos_crash_helper_main(int argc, char **argv);
void hos_crash_watch_start(int argc, char **argv);
void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
void hos_log_flush(void) {}
/* (host/options_path.c: the run's settings file; none here, so the helper reads the game folder's, as a helper given none) */
const char *hos_options_file(void) { return ""; }
static void note(const char *kind, const char *text) {
    const char *out = getenv("CRASH_TEST_OUT");
    FILE *f = out ? fopen(out, "ab") : NULL;
    if (!f) return;
    fprintf(f, "%s\n%s\n\x1e\n", kind, text);
    fclose(f);
}
int hos_crash_test_box(const char *title, const char *text, const char *const *buttons, int n) {
    static int asked;
    char all[4096];
    snprintf(all, sizeof all, "%s\n%s\n", title, text);
    for (int i = 0; i < n; i++) strncat(all, buttons[i], sizeof all - strlen(all) - 2), strcat(all, "|");
    note("BOX", all);
    /* (CRASH_TEST_BUTTONS=1,0,1: the buttons clicked, in turn; then the last, Close or Done) */
    const char *script = getenv("CRASH_TEST_BUTTONS");
    int pick = n - 1, k = 0;
    for (const char *p = script; p && *p; k++) {
        if (k == asked) {
            pick = atoi(p);
            break;
        }
        p = strchr(p, ',');
        p = p ? p + 1 : NULL;
    }
    asked++;
    return pick < n ? pick : n - 1;
}
int hos_crash_test_clipboard(const char *text) {
    note("CLIP", text);
    const char *clip = getenv("CRASH_TEST_CLIP");   /* (0: no clipboard took it) */
    return clip && *clip ? atoi(clip) : 1;
}
int hos_crash_test_open(const char *what) {
    note("OPEN", what);
    return 1;
}
int main(int argc, char **argv) {
    const int helper = hos_crash_helper_main(argc, argv);
    if (helper >= 0) {
        note("END", argc > 1 ? argv[1] : "");
        return helper;
    }
    /* the game: driver <game folder> <how it ends> [log lines...] */
    const char *dir = argv[1], *mode = argv[2];
    char logs[4096], log[4200];
    snprintf(logs, sizeof logs, "%s/logs", dir);
    mkdir(logs, 0700);
    snprintf(log, sizeof log, "%s/longshot_log.txt", logs);
    hos_crash_record_start(dir, getenv("CRASH_TEST_RECORD_VERSION") ? getenv("CRASH_TEST_RECORD_VERSION") : "vtest-1", log);
    /* (the exec'd run after a Restart now: stderr and the log are the same open file the run before left; the game
       does not start its log over, it appends where that run stopped) */
    FILE *f = getenv("RESTARTED") ? fopen(log, "a") : fopen(log, "w");
    fprintf(f, "[hos host] longshot %s\n[hos host] run_id: %s\n",
            getenv("CRASH_TEST_LOG_VERSION") ? getenv("CRASH_TEST_LOG_VERSION") : "vtest-1", hos_crash_record_run());
    for (int i = 3; i < argc; i++) fprintf(f, "%s\n", argv[i]);
    /* (the restarted run's own lines: only it writes these, the way the real game keeps logging after a restart) */
    if (getenv("RESTARTED") && getenv("CRASH_TEST_RESTARTED_LOG")) fprintf(f, "%s\n", getenv("CRASH_TEST_RESTARTED_LOG"));
    fclose(f);
    char *prepare[] = {argv[0], (char *)"--prepare-shaders", NULL};
    if (!strcmp(mode, "prepare")) hos_crash_watch_start(2, prepare);
    else hos_crash_watch_start(argc, argv);
    if (!strcmp(mode, "relaunch")) {   /* (Linux's Restart now: the same process again, a new run) */
        hos_crash_quit_asked();
        hos_crash_clean_end();
        const char *to = getenv("CRASH_TEST_RELAUNCH_TO");   /* (what the restarted run does: default "clean") */
        setenv("RESTARTED", "1", 1);   /* (the exec sees the environment: the restarted run keeps the open log) */
        char *again[] = {argv[0], argv[1], (char *)(to && *to ? to : "clean"), NULL};
        execv("/proc/self/exe", again);
        return 9;
    }
    if (!strcmp(mode, "quit-kill") || !strcmp(mode, "quit-crash")) hos_crash_quit_asked();
    if (!strcmp(mode, "kill") || !strcmp(mode, "quit-kill")) raise(SIGKILL);
    if (!strcmp(mode, "crash") || !strcmp(mode, "quit-crash")) {
        HosCrashContext c;
        memset(&c, 0, sizeof c);
        c.reason = "unhandled exception";
        c.thread = "host";
        c.code = 0xC0000005u;
        c.pc = (uintptr_t)main;
        c.sp = (uintptr_t)&c;
        c.fp = (uintptr_t)__builtin_frame_address(0);
        hos_crash_record_write(&c);
        raise(SIGKILL);
    }
    hos_crash_clean_end();
    return 0;
}
"""


def argv_of(line):
    """CommandLineToArgvW's rules for the arguments after the program's name (2n backslashes and a quote: n and a quote's
    toggle; 2n+1 and a quote: n and a literal quote; backslashes before anything else stay)"""
    out, cur, quoted, have, i = [], [], False, False, 0
    while i < len(line):
        c = line[i]
        if c == "\\":
            n = 0
            while i < len(line) and line[i] == "\\":
                n, i = n + 1, i + 1
            if i < len(line) and line[i] == '"':
                cur.append("\\" * (n // 2))
                if n % 2:
                    cur.append('"')
                    i += 1
                have = True
                continue
            cur.append("\\" * n)
            have = True
            continue
        if c == '"':
            if quoted and i + 1 < len(line) and line[i + 1] == '"':
                cur.append('"')
                i += 2
                continue
            quoted, have, i = not quoted, True, i + 1
            continue
        if c in " \t" and not quoted:
            if have:
                out.append("".join(cur))
            cur, have, i = [], False, i + 1
            continue
        cur.append(c)
        have, i = True, i + 1
    if have:
        out.append("".join(cur))
    return out


def source_checks():
    main = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    dialog = (ROOT / "host/hos_crash_dialog.c").read_text(encoding="utf-8")
    backend = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    # (the crash report's renderer is logged before the back end's start(), which prepares the shaders: a crash in there has one)
    start = backend[backend.index("int rx_start(void *hwnd) {"):]
    start = start[:start.index("\n}\n")]
    assert start.index('hos_log("report: renderer Vulkan")') < start.index("r->start(hwnd)"), \
        "report: renderer is logged before the Vulkan back end starts, so a crash while it prepares the shaders names it"
    first = "const int helper = hos_crash_helper_main(argc, argv);\n        if (helper >= 0) return helper;"
    assert main.count(first) == 2, "both main()s hand a helper's command line over"
    win = main[main.index("int main(int argc, char **argv) {\n#else"):]
    assert win.index(first) < win.index("narrow_paths_ok(argc, argv)") < win.index("hos_game_layout();"), \
        "Windows: the helper before the folder check, the layout and longshot_env.txt"
    linux = main[main.rindex("int main(int argc, char **argv) {"):]
    assert linux.index(first) < linux.index("hos_game_layout();") < linux.index("hos_display_init();"), \
        "Linux: the helper before the layout and the window"
    start = main.index("hos_crash_watch_start(argc, argv);")
    assert main.index("hos_crash_record_prepare(path);") < start and main.index('hos_log("run_id: %s",') < start, \
        "the helper starts once the crash record (and its run id) is set up"
    for later in ("if (!reserve_guest()) {", "hos_display_start();", "hos_boot_main_thread(", "hos_mods_init(image);"):
        assert start < main.index(later), "the helper starts before " + later
    arm = main.index("hos_crash_test_arm();")
    assert main.index("if (!reserve_guest()) {") < arm < main.index("hos_display_start();"), \
        "HOS_CRASH_TEST crashes once the crash handlers are set up (reserve_guest), before the renderer"
    allowed = main[main.index("static int env_line_allowed("):main.index("static void load_longshot_env(")]
    assert '"HOS_CRASH_TEST"' in allowed, "HOS_CRASH_TEST is not taken from a shared longshot_env.txt"
    close = backend[backend.index("void rx_close_asked(void) {"):]
    close = close[:close.index("\n}\n")]
    assert close.index("hos_crash_quit_asked();") < close.index("if (!vk_module) return;"), \
        "a quit is noted first, before the renderer's close that can take seconds"
    own = dialog[dialog.index("static int log_is_run(const char *path, const char *run_id, uint64_t *at) {"):]
    own = own[:own.index("\n}\n")]
    assert "read_text" not in own and "fgets" in own and "*at =" in own and "banner_at" in own, \
        "the log-ownership probe streams the file for its run_id line AND says where it sits: a head window " \
        "(read_text) misses the run after a posix Restart now, whose line sits where the run before stopped (issue #1374)"
    rdl = dialog[dialog.index("static void read_log(RunLog *r, const Who *who) {"):]
    rdl = rdl[:rdl.index("\n}\n")]
    assert "line_at >= r->from" in rdl, \
        "the facts the report reads come from the located run_id line on: a shared log holds the run before too, " \
        "and its map must not describe the restarted run (issue #1374, review round two)"
    spawn = dialog[dialog.index("static int spawn_exe(const char *exe_path, const char *const *args, int n, int wait_ms, int *code) {"):]
    spawn = spawn[:spawn.index("\n}\n")]
    assert "CREATE_NO_WINDOW" in spawn and re.search(r"CreateProcessW\(program, cmd, NULL, NULL, FALSE,", spawn), \
        "Windows: no console, no handle of the game's inherited"
    assert spawn.count('"/dev/null"') == 3 and '"/proc/self/exe"' in spawn, "Linux: the helper's output to nowhere"
    entry = dialog[dialog.index("int hos_crash_helper_main(int argc, char **argv) {"):]
    entry = entry[:entry.index("\n}\n")]
    assert entry.index("close_inherited();") < entry.index("helper(&a)"), \
        "Linux: the helper lets go of the fds it inherited without close-on-exec (a launcher's pipe) before it waits"
    helper_fn = dialog[dialog.index("static int helper(const HosCrashArgs *a) {"):]
    helper_fn = helper_fn[:helper_fn.index("\n}\n")]
    assert helper_fn.index("GetSystemMetrics(SM_SHUTTINGDOWN)") < helper_fn.index("show(&s);") and \
        helper_fn.index("GetSystemMetrics(SM_SHUTTINGDOWN)") < helper_fn.index("ledger_add("), \
        "Windows: no window at a logoff or shutdown (it would hold it up), and the record not listed as shown"
    assert "HOS_STAGE_UNKNOWN" in helper_fn, "no log found: the stage is not known"
    ledger = dialog[dialog.index("static int ledger_add("):]
    assert "replace_text(path," in ledger[:ledger.index("\n}\n")], "the ledger is replaced whole, never read half-written"
    report_fn = dialog[dialog.index("static int make_report(const Shown *s, Made *out) {"):]
    assert "prune_reports(pending);" in report_fn[:report_fn.index("\n}\n")], "the saved reports are pruned"
    post_fn = dialog[dialog.index("static int post_report(const Shown *s, const char *payload, char **body) {"):]
    post_fn = post_fn[:post_fn.index("\n}\n")]
    assert '"=https"' in post_fn and "--location" not in post_fn and "s->reports}" in post_fn, \
        "the report goes over https to the stamp's address, no redirect followed"
    assert 'args[] = {"--disable", ' in post_fn, "curl's --disable first: no .curlrc of the player's changes where it goes"
    spawn_fn = dialog[dialog.index("static int spawn_exe(const char *exe_path, const char *const *args, int n, int wait_ms, int *code) {"):]
    spawn_fn = spawn_fn[:spawn_fn.index("\n}\n")]
    assert "char **env = exe_path ? system_env() : NULL;" in spawn_fn and "env ? env : environ" in spawn_fn, \
        "Linux: curl gets the environment without the game's LD_LIBRARY_PATH and LD_PRELOAD, as busctl does"
    send_fn = dialog[dialog.index("static int send_report(const Shown *s) {"):]
    send_fn = send_fn[:send_fn.index("\n}\n")]
    assert send_fn.index("if (hit != 0) break;") < send_fn.index("post_report("), "posted only once Send is pressed"
    show_fn = dialog[dialog.index("static void show(Shown *s) {"):]
    assert "find_addresses(s);" in show_fn and "sender" not in show_fn and "spawn_exe" not in send_fn, \
        "the game sends its report itself: no launcher"
    page_fn = dialog[dialog.index("static int ui_page("):]
    page_fn = page_fn[:page_fn.index("\n}\n")]
    assert page_fn.index("window_page(") < page_fn.index("sdl_box("), "the crash window first, SDL's message box after it"
    record = (ROOT / "host/hos_crash_record.c").read_text(encoding="utf-8")
    quit_fn = record[record.index("void hos_crash_quit_asked(void) {"):]
    quit_fn = quit_fn[:quit_fn.index("\n}\n")]
    assert quit_fn.index("__atomic_add_fetch(&quit_busy") < quit_fn.index("&ready") < quit_fn.index("__atomic_sub_fetch(&quit_busy"), \
        "the quit note counts itself busy before it reads ready"
    clean = record[record.index("void hos_crash_clean_end(void) {"):]
    clean = clean[:clean.index("\n}\n")]
    assert clean.index("__atomic_store_n(&ready, 0") < clean.index("&quit_busy") < clean.index("close_file(marker)"), \
        "a clean end waits for a quit note being written before it closes the marker"
    watch = dialog[dialog.index("void hos_crash_watch_start(int argc, char **argv) {"):]
    watch = watch[:watch.index("\n}\n")]
    assert re.search(r'hos_crash_under_gamescope\(getenv\("GAMESCOPE_WAYLAND_DISPLAY"\), getenv\("XDG_CURRENT_DESKTOP"\)\)', watch) and \
        watch.index("gamescope, run)") < watch.index("HOS_CRASH_SHOW_ARG"), \
        "under gamescope neither the helper nor the next start's box of an earlier crash"
    clip = dialog[dialog.index("static int clipboard(const char *text) {\n#ifdef HOS_CRASH_DIALOG_TEST\n    return hos_crash_test_clipboard(text);\n#endif\n    const char *wl"):]
    clip = clip[:clip.index("\n}\n")]
    assert clip.index("hos_crash_clip_tool(") < clip.index("SetClipboardText") and "if (!x11 ||" in clip, \
        "Linux: a clipboard tool first, SDL's only on X11"
    # nx2/make_game.py's update: a windowless copy is ended, but not a waiting helper of a game still open
    import sys
    sys.path.insert(0, str(ROOT / "nx2"))
    import make_game
    assert make_game.stale_copies([10, 11, 12, 13], {10: 1, 11: 10, 12: 99, 13: 10}, {10, 13}) == [12], \
        "the update ends a stray copy, not the game (10), its waiting helper (11) or a helper showing its window (13)"
    folder = os.path.join("C:" + os.sep, "Games", "Longshot")
    roots = [os.path.normcase(f) + os.sep for f in (os.path.join(folder, "bin"), folder)]
    for name, copy in (("longshot.exe", True), (os.path.join("bin", "longshot.exe"), True),
                       ("LongshotLauncher-v1.0.0-rc.9-windows-x64-setup.exe", False), (os.path.join("bin", "other.exe"), False),
                       (os.path.join("saves", "longshot.exe"), False)):
        assert make_game.game_copy(os.path.join(folder, name), roots) == copy, \
            "only longshot.exe in the folder or bin\\ is a running game, not the launcher kept there (LongshotLauncher#139): " + name
    for fixture in ("tests/display_startup_failure_test.cpp",):   # (the checks that build a main() cut out of hos_main.c)
        assert "int hos_crash_helper_main(" in (ROOT / fixture).read_text(encoding="utf-8"), fixture + " stands in for the helper"
    code = re.sub(r"/\*.*?\*/|//[^\n]*", "", dialog, flags=re.S)
    assert not re.search(r"\b(?:ExitProcess|_exit|_Exit|TerminateProcess)\s*\(", code), "the helper ends by returning from main"
    for forbidden in ("hos_sdl_", "rx_start", "hos_display", "hos_boot"):
        assert forbidden not in code, "the helper starts nothing of the game's: " + forbidden
    # (the crash window takes a press, never input left over: no key repeats, nothing in a page's first moments, a key down
    # then only once let go)
    window = (ROOT / "gpu/vk/vk_crash_window.cpp").read_text(encoding="utf-8")
    assert "if (type == 0x300 && raw[37]) break;" in window and "if (type == 0x300 && raw[13]) break;" in window, \
        "a key repeat (SDL3's, SDL2's) presses no button"
    assert "in.quiet = std::chrono::steady_clock::now() - shown < kQuiet;" in window and \
        "if (in.quiet) in.held->set((size_t)code);" in window and "if (type == 0x401 && in.quiet) break;" in window, \
        "a key or click in a page's first moments presses nothing, and a key held then only after it is let go"
    # (of the back end's module, only the crash window: gpu/vk/vk_crash_window.cpp, SDL's renderer, never the renderer itself)
    assert set(re.findall(r"\bmkwvk_\w+", code)) == {"mkwvk_crash_page", "mkwvk_crash_page_fn", "mkwvk_crash_close",
                                                      "mkwvk_crash_close_fn"}, set(re.findall(r"\bmkwvk_\w+", code))
    switches = (ROOT / "gpu/vk/switches.def").read_text(encoding="utf-8")
    for switch in ("HOS_CRASH_WATCH", "HOS_CRASH_TEST"):
        assert "\nSW(%s," % switch in switches, switch + " has its row in gpu/vk/switches.def"
    assert "--crash-watch" in (ROOT / "docs/dev/reference/crash-window.md").read_text(encoding="utf-8"), \
        "--crash-watch is documented in docs/dev/reference/crash-window.md"
    template = (ROOT / ".forgejo/ISSUE_TEMPLATE/1-game-problem.yml").read_text(encoding="utf-8")
    for field in ("version", "track", "kind", "gpu", "system", "where", "what", "settings", "log"):
        assert re.search(r"^\s+id: " + field + r"$", template, re.M), "the form has the field " + field
    menu = (ROOT / "gpu/vk/vk_bug_report.cpp").read_text(encoding="utf-8")
    form = re.search(r'#define HOS_CRASH_ISSUE_FORM "([^"]+)"', (ROOT / "host/hos_crash_dialog.h").read_text(encoding="utf-8")).group(1)
    assert 'kForm = "' + form + '"' in menu, "the crash window links the same form as the menu's Bug report"
    print("PASS: the wiring: helper first in main, started after the record and before the game, quit noted first, no console")
    return form


def run(exe, *args):
    r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=30, encoding="utf-8")
    assert r.returncode == 0, (args, r.returncode, r.stderr)
    return r.stdout


def logic_checks(exe, menu, tmp, form):
    # the command line
    good = ["longshot", "--crash-watch", "1234", RUN, "/g", "/g/logs/longshot_log.txt"]
    assert run(exe, "args", *good).strip() == "1|1234|%s||/g|/g/logs/longshot_log.txt" % RUN
    assert run(exe, "args", *good[:5], "-").strip().endswith("|/g|NULL")
    for bad in (["0"], ["12a"], ["-5"], ["123456789012345678901"]):
        assert run(exe, "args", "longshot", "--crash-watch", *bad, RUN, "/g", "-").startswith("-1|"), bad
    for bad_run in (RUN[:-1], RUN.upper(), RUN + "0", "unknown"):
        assert run(exe, "args", "longshot", "--crash-watch", "7", bad_run, "/g", "-").startswith("-1|"), bad_run
    assert run(exe, "args", "longshot", "--crash-watch", "7", RUN, "/g").startswith("-1|"), "an argument missing"
    assert run(exe, "args", "longshot", "--crash-watch", "7", RUN, "", "-").startswith("-1|"), "no game folder"
    for name in ("crash-20261114T101112Z.txt", "unclean-20261114T101112Z.txt", "crash-20261114T101112Z-4242-3.txt"):
        assert run(exe, "args", "longshot", "--crash-show", name, "/g", "-").strip() == "2|0||%s|/g|NULL" % name
    for name in ("../crash-20261114T101112Z.txt", "crash-20261114T101112Z.txt.bak", "report-20261114T101112Z.txt",
                 "crash-20261114T1011Z.txt", "crash-20261114T101112Z-1-.txt", "/etc/passwd"):
        assert run(exe, "args", "longshot", "--crash-show", name, "/g", "-").startswith("-1|"), name
    for game in (["longshot"], ["longshot", "image.bin"], ["longshot", "--user", "u", "--crash-watchx"]):
        assert run(exe, "args", *game).startswith("0|"), game
    # (the run's settings file, in the user folder (host/options_path.c), after the log; left out, the game folder's)
    opts = "/u/longshot_options.ini"
    assert run(exe, "options", *good).strip() == "1|NULL"
    assert run(exe, "options", *good, opts).strip() == "1|" + opts
    assert run(exe, "options", *good, "-").strip() == "1|NULL"
    assert run(exe, "options", *good, "").startswith("-1|"), "an empty settings file"
    assert run(exe, "options", *good, opts, "x").startswith("-1|"), "one too many"
    assert run(exe, "options", "longshot", "--crash-show", "crash-20261114T101112Z.txt", "/g", "-", opts).strip() == "2|" + opts
    print("PASS: the helper's command line: whole or refused, and never the game's own")
    # which starts get a helper
    assert run(exe, "wanted", 0, 0, 0, 0, RUN).strip() == "1"
    assert run(exe, "wanted", 0, 0, 0, 0, RUN, "image.bin", "--user", "u").strip() == "1"
    for flags, args in (((1, 0, 0, 0), []), ((0, 1, 0, 0), []), ((0, 0, 1, 0), []), ((0, 0, 0, 1), []),
                        ((0, 0, 0, 0), ["--prepare-shaders"]), ((0, 0, 0, 0), ["--crash-watch"]), ((0, 0, 0, 0), ["--crash-show"])):
        assert run(exe, "wanted", *flags, RUN, *args).strip() == "0", (flags, args)
    assert run(exe, "wanted", 0, 0, 0, 0, "none").strip() == "0" and run(exe, "wanted", 0, 0, 0, 0, "unknown").strip() == "0"
    for display, desktop, want in (("gamescope-0", "-", 1), ("-", "gamescope", 1), ("-", "KDE:Gamescope", 1), ("", "-", 0),
                                   ("-", "KDE", 0), ("-", "gamescopex", 0), ("-", "-", 0)):
        assert run(exe, "gamescope", display, desktop).strip() == str(want), (display, desktop)
    print("PASS: no helper for --prepare-shaders, HOS_HEADLESS, HOS_CRASH_WATCH=0, HOS_PGO, gamescope, a helper, or no run id")
    # Linux's clipboard tools, and the box after Report: never "copied" when it was not
    assert run(exe, "cliptools", 1, 1).splitlines() == ["wl-copy", "xclip -selection clipboard -in", "xsel --clipboard --input"]
    assert run(exe, "cliptools", 1, 0).splitlines() == ["wl-copy"], "Wayland without XWayland: wl-copy only"
    assert run(exe, "cliptools", 0, 1).splitlines() == ["xclip -selection clipboard -in", "xsel --clipboard --input"]
    assert run(exe, "cliptools", 0, 0).strip() == ""
    saved = "/home/x/Longshot/bug-reports/pending/report-20261114T101112Z.txt"
    for copied in (1, 2):
        text = run(exe, "ready", copied, 1, saved)
        assert text.startswith("The report is on your clipboard.") and "It is saved as " + saved + "." in text
        assert ("Keep this window open" in text) == (copied == 2), "kept open only when this helper serves the clipboard"
    text = run(exe, "ready", 0, 1, saved)
    assert "on your clipboard" not in text and "could not be put on the clipboard" in text and "Open the report" in text
    assert "It is saved as " + saved + "." in text and "Keep this window open" not in text
    text = run(exe, "ready", 0, 0, "-")
    assert "on your clipboard" not in text and "nor saved" in text and "Open the report" not in text
    assert "https://example.invalid/issues/new/choose" in text, "no browser: the form's address said"
    print("PASS: the clipboard tools in turn; the box after Report never says copied when nothing took it")
    # Windows' quoting, read back by CommandLineToArgvW's rules
    for arg in ("C:\\Games\\Longshot", "C:\\Games\\Long shot\\", 'a"b', "x\\\\", "x\\\"y", "", "ゲーム Zélda 🎮", "--crash-watch"):
        quoted = run(exe, "quote", 500, arg)[:-1]
        assert argv_of("prog " + quoted)[1:] == [arg], (arg, quoted)
    assert run(exe, "quote", 4, "long").strip() == "<none>", "a quoted argument that does not fit is refused, not cut"
    print("PASS: a quoted argument comes back whole through CommandLineToArgvW's rules")
    # record names and times
    assert run(exe, "recname", "crash-20261114T101112Z.txt").split() == ["1", "20261114T101112Z"]
    assert run(exe, "recname", "unclean-20261114T101112Z-99-1.txt").split() == ["2", "20261114T101112Z"]
    for when in ("19700101T000000Z", "20000229T235959Z", "20261114T101112Z", "21000301T000000Z"):
        want = calendar.timegm(time.strptime(when, "%Y%m%dT%H%M%SZ"))
        assert int(run(exe, "epoch", when)) == want, when
    now = calendar.timegm(time.strptime("20261114T120000Z", "%Y%m%dT%H%M%SZ"))
    assert run(exe, "atstart", "crash-20261114T101112Z.txt", "", now).strip() == "1"
    assert run(exe, "atstart", "crash-20261114T101112Z.txt", "crash-20261114T101112Z.txt\n", now).strip() == "0", "shown already"
    assert run(exe, "atstart", "crash-20261114T101112Z.txt", "crash-20261114T101112Z.txt.x\n", now).strip() == "1", "whole names"
    assert run(exe, "atstart", "unclean-20261114T101112Z.txt", "", now).strip() == "0", "an unclean end is not shown at a start"
    assert run(exe, "atstart", "unclean-20261114T101112Z.txt", "", now, 1).strip() == "1", \
        "an out-of-memory end is shown at a start (its helper may have been stopped with the game's scope)"
    assert run(exe, "atstart", "unclean-20261114T101112Z.txt", "unclean-20261114T101112Z.txt\n", now, 1).strip() == "0"
    assert run(exe, "atstart", "crash-20261112T101112Z.txt", "", now).strip() == "0", "older than a day"
    assert run(exe, "atstart", "crash-20271114T101112Z.txt", "", now).strip() == "0", "from the future"
    print("PASS: a start shows a crash record of the last day nobody showed, never an unclean end")
    # Report automatically: the addresses in the stamp, offered for a crash record not sent yet, the report's lines, the answer
    stamp = tmp / "stamp.txt"
    for text, key, want in (("version=v3.1.0\ncrash_reports=https://d.example/telemetry/crash\n", "crash_reports",
                             "1|https://d.example/telemetry/crash"),
                            ("crash_reports=https://d.example/telemetry/crash\r\n", "crash_reports",
                             "1|https://d.example/telemetry/crash"),
                            ("crash_reports=http://d.example/x\n", "crash_reports", "0|"), ("crash_reports=\n", "crash_reports", "0|"),
                            ("crash_reports=https://a b\n", "crash_reports", "0|"), ("x_crash_reports=https://d/x\n", "crash_reports", "0|"),
                            ("version=v3.1.0\n", "crash_reports", "0|")):
        stamp.write_text(text, encoding="utf-8", newline="")
        assert run(exe, "address", stamp, key).strip() == want, (text, run(exe, "address", stamp, key))
    offered = {("crash-20261114T101112Z.txt", 1, 0): 1, ("crash-20261114T101112Z-77-1.txt", 1, 0): 1,
               ("unclean-20261114T101112Z.txt", 1, 0): 0, ("crash-20261114T101112Z.txt", 0, 0): 0,
               ("crash-20261114T101112Z.txt", 1, 1): 0, ("-", 1, 0): 0, ("../crash-20261114T101112Z.txt", 1, 0): 0}
    for (name, have, sent), want in offered.items():
        assert int(run(exe, "offered", name, have, sent)) == want, (name, have, sent)
    # (the launcher's Send crash reports automatically: send=1 on a line of its own in longshot_crash.ini, blanks around it)
    ini = tmp / "longshot_crash.ini"
    for text, want in (("send=1\n", 1), ("  send=1 \r\n", 1), ("x=2\nsend=1", 1), ("send=0\n", 0), ("send=10\n", 0),
                       ("#send=1\n", 0), ("", 0)):
        ini.write_text(text, encoding="utf-8")
        assert int(run(exe, "autosend", ini)) == want, repr(text)
    # (the crash's lines as the relay's signature reads them: folders off the modules, the place's module in its characters,
    # the rows of the game's memory left out)
    for line, want in (("[hos Main] CRASH: exception 0xc0000005 (read 0x8, guest 0x8) at /usr/lib/libc.so.6+0xb5a22",
                        "1|CRASH: exception 0xc0000005 (read 0x8, guest 0x8) at libc.so.6+0xb5a22"),
                       ("[hos Main] CRASH: exception 0xc0000005 (read 0x8) at ?+0x10", "1|CRASH: exception 0xc0000005 (read 0x8) at unknown+0x10"),
                       ("CRASH: host backtrace: C:\\G\\longshot.exe+0x1a ntdll.dll+0x33", "1|CRASH: host backtrace: C:\\G\\longshot.exe+0x1a ntdll.dll+0x33"),
                       ("[x] CRASH:   x0 0000000000000001", "0|"), ("[x] CRASH: [x19+0x10] 00 11 22", "0|"), ("[x] vk: frame", "0|")):
        assert run(exe, "crashline", line).rstrip("\n") == want, (line, run(exe, "crashline", line))
    cleaned = run(exe, "clean", "  audio: output on Jane's AirPods (3 devices) /home/alice/x Alice's PC C:\\Users\\Bob\\y "
                  "Z:\\home\\bob\\z deck My Mic\x01 ", 300, "/home/alice", "alice", 0, "My Mic").rstrip("\n")
    assert cleaned == "audio: output on <device> (3 devices) ~/x <user>'s PC %USERPROFILE%\\y ~\\z deck <device>", cleaned
    assert run(exe, "clean", "é" * 10, 4, "/h", "u", 0).rstrip("\n") == "éééé", "a character is never cut in two"
    # (a device name inside <device> itself is found once, never again in what it became; a name that grows the text
    # past its buffer cuts the end off, never a character in two, and writes nothing past it)
    for dev in ("dev", "vice", "device"):
        assert run(exe, "clean", f"a {dev} b {dev}", 300, "/h", "u", 0, dev).rstrip("\n") == "a <device> b <device>", dev
        assert run(exe, "cleancap", f"{dev} {dev}", 4096, dev).rstrip("\n") == "ok|<device> <device>", dev
    assert run(exe, "cleancap", " ".join(["dev"] * 8), 32, "dev").rstrip("\n") == "ok|<device> <device> <device>"
    assert run(exe, "cleancap", "dev éééé", 15, "dev").rstrip("\n") == "ok|<device> éé"
    assert run(exe, "cleancap", "audio in: Mi open (48000 Hz", 20, "zzz").rstrip("\n") == "ok|audio in: <device>"
    assert run(exe, "json", 'a"b\\c\nd\x02').rstrip("\n") == '"a\\"b\\\\c\\nd\\u0002"'
    # (sent: the team has it, and the Sent page links nowhere: an older relay's comment link is not shown or opened)
    for body in ('{"accepted": true}', '{"comment": "https://g.example/o/r/issues/81#issuecomment-5", "new": true}', ""):
        sent = run(exe, "answer", 201, body).rstrip("\n")
        assert sent == "1|0|Sent. Thank you: the Longshot team has it. Only the team can read it.", sent
    assert run(exe, "answer", 429, "{}").startswith("0|1|Not sent: the crash server is busy")
    assert run(exe, "answer", 400, '{"error": "crash is required"}').rstrip("\n") == \
        "0|0|Not sent: the crash server refused the report (400: crash is required)."
    assert run(exe, "answer", 502, "").startswith("0|1|"), "a server error: try again"
    assert run(exe, "answer", 0, "").startswith("0|1|Not sent: couldn't reach"), "no answer: try again"
    headers = tmp / "headers.txt"
    headers.write_text("HTTP/1.1 301 Moved\r\nLocation: x\r\n\r\nHTTP/2 201 \r\ncontent-type: application/json\r\n", encoding="utf-8")
    assert run(exe, "status", headers).strip() == "201", "the last answer's status"
    headers.write_text("", encoding="utf-8")
    assert run(exe, "status", headers).strip() == "0"
    print("PASS: Report automatically: the stamp's addresses, offered once, the report's lines cleaned, the answer's words")

    # out of memory: the game's cgroup's memory.events, and the helper out of the game's scope
    cg = tmp / "cgroup.txt"
    cg.write_text("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-x-1.scope\n", encoding="utf-8")
    assert run(exe, "cgevents", cg).strip() == \
        "1 /sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-x-1.scope/memory.events"
    cg.write_text("0::/\n", encoding="utf-8")
    assert run(exe, "cgevents", cg).strip() == "1 /sys/fs/cgroup/memory.events", "the root cgroup"
    cg.write_text("12:memory:/x\n", encoding="utf-8")
    assert run(exe, "cgevents", cg).split()[0] == "0", "cgroup v1 only: no count"
    ev = tmp / "memory.events"
    ev.write_text("low 0\nhigh 0\nmax 12\noom 3\noom_kill 2\noom_group_kill 0\n", encoding="utf-8")
    assert run(exe, "oomkills", ev).strip() == "2"
    ev.write_text("low 0\n", encoding="utf-8")
    assert run(exe, "oomkills", ev).strip() == "-1"
    dialog = (ROOT / "host/hos_crash_dialog.c").read_text(encoding="utf-8")
    helper_fn = dialog[dialog.index("static int helper(const HosCrashArgs *a) {"):]
    helper_fn = helper_fn[:helper_fn.index("\n}\n")]
    assert helper_fn.index("oom_before = oom_kills(events);") < helper_fn.index("own_scope();") < helper_fn.index("wait_game("), \
        "the game's OOM count read, then out of its scope, before the wait"
    assert "if (name[0] && !r.oom) ledger_add" in helper_fn and "if (name[0] && r.oom) ledger_add" in helper_fn, \
        "an out-of-memory end is listed as shown only after its window"
    held = helper_fn.index("sigprocmask(SIG_BLOCK, &term, NULL);")
    assert held < helper_fn.index("wait_game(") < helper_fn.index("mark_oom(") < helper_fn.index("sigprocmask(SIG_UNBLOCK, &term, NULL);"), \
        "SIGTERM held back from the wait until the record says out of memory (systemd stops the scope right after the kill)"
    scope_fn = dialog[dialog.index("static void own_scope(void) {"):]
    assert '"StartTransientUnit"' in scope_fn and '"PIDs", "au", "1", pid' in scope_fn and "waited >= 2000" in scope_fn[:scope_fn.index("\n}\n")]
    print("PASS: out of memory: the game's oom_kill count, the record marked, shown at a start, the helper out of the scope")

    # what Report automatically's card says, in the manual report too (build_report's rules)
    for line, want in {"ultrawide=1": "ultrawide|1", "output_res=1920x1080\r": "output_res|1920x1080", "aspect=16,9": "aspect|16,9",
                       "fov=-5": "fov|-5", "key_a=30": "-", "pad_b=1": "-", "auto_for=12": "-", "microphone_device=Mic": "-",
                       "name=abc": "-", "Bad=1": "-", "x=1x": "-", "x=-1x2": "-"}.items():
        assert run(exe, "setting", line).strip() == want, (line, want)
    for line, stamp, want in (("v3.1.0 (source 1234abcd)", "1234abcd", "v3.1.0|1234abcd|1|1"),
                              ("v3.1.0 (source 1234abcd)", "99999999", "v3.1.0|1234abcd|1|0"),
                              ("v3.1.0-rc.2 (source ab)", "ab", "v3.1.0-rc.2|ab|1|1"),
                              ("nightly-170-6c9edffe (source ab)", "ab", "nightly-170-6c9edffe|ab|1|1"),
                              ("v3.0.0-rc.3-105-g6b698e81-dirty (source ab)", "ab", "v3.0.0-rc.3-105-g6b698e81-dirty|ab|0|0"),
                              ("v3.1.0", "", "v3.1.0||1|0")):
        assert run(exe, "release", line, stamp).strip() == want, (line, want)
    print("PASS: the card's fields for the manual report: the settings it counts, the release build")

    # the log: what it says, the stage
    def facts(*lines):
        path = tmp / "facts.txt"
        path.write_text("".join(l + "\n" for l in lines), encoding="utf-8")
        out = run(exe, "log", path).splitlines()
        values = dict(kv.split("=") for kv in out[0].split())
        values.update(line.split("=", 1) for line in out[1:])
        return values
    head = ["[hos host] longshot v3.1.0 (source 1234abcd)", "[hos host] run_id: " + RUN]
    f = facts(*head)
    assert f["stage"] == "0" and f["failed"] == "0" and f["version"] == "v3.1.0 (source 1234abcd)"
    f = facts(*head, "[hos host] vk: precompile: 9916 programs", "[hos host] vk: start-up: shaders 10 of 20, 50% of all")
    assert f["stage"] == "1", "preparing"
    f = facts(*head, "[hos host] vk: start-up: shaders 10 of 20", "[hos host] vk: start-up: the shaders took 4.7 s")
    assert f["stage"] == "0" and f["prepared"] == "1", "prepared: the start again"
    f = facts(*head, "[hos host] vk: start-up: shaders 1 of 2", "[hos Main] map: Kokiri Forest (spot 0)")
    assert f["stage"] == "2", "a map line: play"
    assert f["place"] == "Kokiri Forest (spot 0)", f
    f = facts(*head, "[hos host] report: os CachyOS (Linux 7.3.4), KDE (wayland)", "[hos host] report: cpu Ryzen 7 (16 threads), 15 GB",
              "[hos host] report: renderer Vulkan", "[hos host] vk: start-up: shaders 1 of 2",
              "[hos host] CRASH: exception 0xc0000005 (write 0x10, guest 0xfff) at ~/Games/Longshot/longshot+0x1a5b9928",
              "[hos host] CRASH: exception 0xc0000005 (read 0x0) at other+0x1")
    assert (f["os"], f["cpu"], f["renderer"], f["place"], f["crash"]) == (
        "CachyOS (Linux 7.3.4), KDE (wayland)", "Ryzen 7 (16 threads), 15 GB", "Vulkan", "Preparing shaders",
        "0xc0000005 (write) at longshot+0x1a5b9928"), f
    f = facts(*head, "[hos host] vk: start-up: shaders 1 of 2", "[hos host] vk: start-up: the shaders took 4.7 s")
    assert f["place"] == "Title and intro" and f["crash"] == "", f
    f = facts(*head, "[hos host] vk: device 0: NVIDIA GeForce RTX 3090, Vulkan 1.4, driver 0x9b118180, type 2",
              "[hos host] vk: device 1: llvmpipe (LLVM 23.1.4, 256 bits), Vulkan 1.4, driver 0x6802006, type 4",
              "[hos host] vk: NVIDIA GeForce RTX 3090, Vulkan 1.4, driver 0x9b118180; mesh shaders used, host memory import yes",
              "[hos host] vk: driver: NVIDIA 580.95.05", "[hos host] vk: NVIDIA DLSS: not available: no nvngx_dlss.dll in ~/x",
              "[hos host] [warn] vk: AMD FSR 4: no upscaler on this GPU")
    assert f["gpu"] == "NVIDIA GeForce RTX 3090" and f["driver"] == "NVIDIA 580.95.05"
    # (what Report automatically sends as the card: the renderer's `report: gpu` line, else the device and its driver
    # joined the same way, else the name alone: one value for a card, with its driver)
    assert f["gpu_sent"] == "NVIDIA GeForce RTX 3090, NVIDIA 580.95.05", "no report: gpu line: the device and vk: driver:"
    chosen = "[hos host] vk: NVIDIA GeForce RTX 3090, Vulkan 1.4, driver 0x9b118180; mesh shaders yes, host memory import yes"
    g = facts(*head, chosen, "[hos host] vk: driver: NVIDIA 580.95.05", "[hos host] report: gpu AMD Radeon RX 7800 XT, radv Mesa 26.0")
    assert g["gpu_sent"] == "AMD Radeon RX 7800 XT, radv Mesa 26.0" and g["gpu"] == "NVIDIA GeForce RTX 3090", "report: gpu first"
    g = facts(*head, "[hos host] report: gpu NVIDIA GeForce RTX 3090, NVIDIA 580.95.05", "[hos host] report: gpu ")
    assert g["gpu_sent"] == "", "the last report: gpu line, as the other report: lines; none when the renderer names no device"
    assert facts(*head, chosen)["gpu_sent"] == "NVIDIA GeForce RTX 3090", "no driver line: the name alone"
    assert facts(*head, "[hos host] vk: driver: NVIDIA 580.95.05")["gpu_sent"] == "", "no device in use: nothing"
    assert f["devices"] == "NVIDIA GeForce RTX 3090; llvmpipe (LLVM 23.1.4, 256 bits)", f["devices"]
    assert f["dlss"].startswith("NVIDIA DLSS: not available") and f["fsr4"].startswith("AMD FSR 4: no upscaler")
    starts = {
        "files": ["cannot open /g/image.bin", "usage: ./longshot [image.bin] [--romfs <dir>]",
                  "/g/image.bin: 1 bytes, the lifted code expects 0x5f00000: lift and image do not match",
                  "[hos host] rx: mkwvk_init version mismatch (3 vs 4): WebGPU"],
        "vulkan": ["[hos host] rx: RX_BACKEND=vulkan, but /g/bin/longshot_vk.so did not load (libvulkan.so.1: no such file): nothing is drawn",
                   "[hos host] vk: load_global failed (Vulkan loader library or vkCreateInstance missing)",
                   "[hos host] vk: vkCreateInstance failed", "[hos host] vk: no Vulkan 1.3 GPU", "[hos host] vk: create_device failed",
                   "[hos host] display: the renderer did not start: the window stays black"],
        "window": ["[hos host] display: no SDL: no window", "[hos host] display: SDL_CreateWindow: no device: no window",
                   "[hos host] display: CreateWindowExW failed (error 5)"],
        "reserve": ["cannot reserve the guest window"],
    }
    for kind, lines in starts.items():
        for line in lines:
            f = facts(*head, line)
            assert f[kind] == "1" and f["failed"] == "1", (kind, line)
            f = facts(*head, line, "[hos Main] map: Kokiri Forest")
            assert f["failed"] == "0", "a start that reached play did not fail to start: " + line
    for line in ("[hos host] vk: pipeline cache cannot open the file", "[hos host] mods: cannot open mods/x",
                 "[hos host] display: windowed", "[hos host] rx: renderer: Vulkan (longshot_vk.dll)"):
        f = facts(*head, line)
        assert f["failed"] == "0", line
    assert facts(*head, "[hos host] mem: Windows is out of memory (RAM and page file): close")["memory"] == "1"
    assert facts(*head, "[hos host] mem: commit of 0x10000 bytes at 0x1000 failed (error 1455)")["memory"] == "1"
    assert facts(*head, "[hos host] boot: LOW MEMORY: 7000 MB of memory")["low"] == "1"
    assert facts(*head, "[hos host] [error] vk: FATAL: vkQueueSubmit: VK_ERROR_DEVICE_LOST")["lost"] == "1"
    print("PASS: the log's stage, start failures, memory, device lost, GPU and driver")

    # the verdict and the cause
    def record(kind, **values):
        lines = ["Longshot %s record 1" % kind, "version: v3.1.0", "run_id: " + RUN, "when: 20261114T101112Z"]
        lines += ["%s: %s" % kv for kv in values.items()] + ["record_complete: yes"]
        path = tmp / ("record-%d.txt" % len(list(tmp.glob("record-*.txt"))))
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def log(*lines):
        path = tmp / ("log-%d.txt" % len(list(tmp.glob("log-*.txt"))))
        path.write_text("".join(l + "\n" for l in head + list(lines)), encoding="utf-8")
        return path

    def verdict(rec, lg, have=0, code=0, replaced=0, send=0):
        out = run(exe, "verdict", rec or "-", lg or "-", have, code, replaced, send).split("\n", 3)
        v, cause, stage = map(int, out[0].split())
        return v, cause, stage, out[1], out[2] + "\n" + out[3]
    QUIET, CRASH, UNCLEAN, START = 0, 1, 2, 3
    UNKNOWN, FILES, VULKAN, WINDOW, MEMORY, DRIVER, ENDED = range(7)
    crash = record("crash", reason="unhandled exception", exception="0xc0000005", module_offset="longshot.exe+0x1234")
    plain = log()
    v = verdict(crash, plain)
    assert v[:3] == (CRASH, UNKNOWN, 0) and v[3] == "Longshot closed unexpectedly", v
    assert v[4].startswith("Longshot closed unexpectedly during the start.") and "An unexpected error." in v[4]
    assert "Report manually copies" in v[4] and "Longshot itself sends nothing" in v[4]
    v = verdict(crash, plain, send=1)[4]
    assert "Report automatically shows you what would go" in v and "sends it only when you press Send" in v, v
    assert "Report manually itself sends nothing" in v and "Longshot itself sends nothing" not in v, \
        "with Send offered the window never says Longshot sends nothing"
    assert verdict(crash, log("[hos Main] map: Hyrule Field"))[:3] == (CRASH, UNKNOWN, 2)
    assert "while you were playing" in verdict(crash, log("[hos Main] map: Hyrule Field"))[4]
    assert "while it prepared the shaders" in verdict(crash, log("[hos host] vk: precompile: 1 programs"))[4]
    v = verdict(crash, None)
    assert v[0] == CRASH and v[2] == 3 and v[4].startswith("Longshot closed unexpectedly.\n"), \
        "no log found: nothing said about when (not \"during the start\" for a crash in play)"
    assert verdict(crash, plain, 1, 0)[0] == CRASH, "a crash record shows whatever the exit code"
    assert verdict(crash, plain, 0, 0, 1)[0] == QUIET, "a run another took the place of says nothing"
    quit_crash = record("crash", reason="fatal signal", signal="11", module_offset="longshot+0x10")
    assert verdict(quit_crash, plain)[0] == CRASH, "a crash after a quit still shows (as PR 498 counts it)"
    unclean = record("unclean", reason="ended without a clean exit", crash_handler="not observed")
    assert verdict(unclean, plain)[:2] == (UNCLEAN, ENDED)
    oom = record("unclean", reason="ended without a clean exit", oom_kill="yes")
    assert verdict(oom, plain)[:2] == (UNCLEAN, MEMORY), "the OOM killer's end: out of memory, not ended from outside"
    assert "ran out of memory" in verdict(oom, plain)[4]
    assert "ended without a crash report" in verdict(unclean, plain)[4]
    assert verdict(unclean, log("[hos host] boot: LOW MEMORY: 7000 MB of memory"))[:2] == (UNCLEAN, MEMORY)
    assert verdict(unclean, plain, 1, 1)[0] == UNCLEAN, "Task Manager's End task: exit code 1"
    assert verdict(unclean, plain, 1, 0)[0] == QUIET, "Windows' exit code 0 is no crash"
    quit_unclean = record("unclean", reason="ended without a clean exit", quit_asked="yes")
    assert verdict(quit_unclean, plain)[0] == QUIET, "a quit never counts, however the process ends after it"
    assert verdict(quit_unclean, plain, 1, 1)[0] == QUIET
    assert verdict(None, plain)[0] == QUIET and verdict(None, plain, 1, 0)[0] == QUIET, "a clean end"
    assert verdict(None, None)[0] == QUIET, "a clean end with no log"
    assert verdict(None, plain, 1, 2)[0] == QUIET, "an exit code alone (a typo at a terminal) is no window"
    no_vulkan = log("[hos host] rx: RX_BACKEND=vulkan, but /g/bin/longshot_vk.so did not load (x): nothing is drawn")
    v = verdict(None, no_vulkan)
    assert v[:2] == (START, VULKAN) and v[3] == "Longshot couldn't start" and "Vulkan 1.3" in v[4], v
    assert verdict(quit_unclean, no_vulkan)[0] == START, "a black window closed: still a start that failed"
    assert verdict(None, log("cannot open /g/image.bin"))[:2] == (START, FILES)
    assert "Build the game again" in verdict(None, log("cannot open /g/image.bin"))[4]
    assert verdict(None, log("[hos host] display: no SDL: no window"))[:2] == (START, WINDOW)
    assert verdict(None, log("cannot reserve the guest window"))[:2] == (START, MEMORY)
    assert verdict(crash, no_vulkan)[:2] == (CRASH, VULKAN), "a crash after Vulkan did not start: Vulkan"
    assert verdict(crash, log("[hos host] mem: Windows is out of memory (RAM and page file)"))[1] == MEMORY
    assert verdict(record("crash", reason="unhandled exception", exception="0xc0000017"), plain)[1] == MEMORY
    assert verdict(record("crash", reason="device lost"), plain)[1] == DRIVER
    assert verdict(record("crash", reason="Vulkan fatal"), plain)[1] == DRIVER
    for module in ("nvoglv64.dll+0x1", "amdvlk64.dll+0x2", "igvk64.dll+0x3", "libvulkan_radeon.so+0x4", "libnvidia-glcore.so.580+0x5"):
        assert verdict(record("crash", reason="unhandled exception", module_offset=module), plain)[1] == DRIVER, module
    assert verdict(record("crash", reason="unhandled exception", module_offset="SDL3.dll+0x1"), plain)[1] == UNKNOWN
    assert verdict(crash, log("[hos host] [error] vk: FATAL: x: VK_ERROR_DEVICE_LOST"))[1] == DRIVER
    print("PASS: crash, unclean end, quit, clean end, restart and failed starts; causes from the record and the log")

    # redaction: the menu's own, line for line
    home_l, user_l = "/home/private-user", "private-user"
    lines = [home_l + "/Games/Longshot/logs/longshot_log.txt", "[hos host] vk: pipeline cache " + home_l + "/x.bin (1 KB)",
             "/run/media/private-user/Disk/Longshot", "/home/private-user2/other", "x/home/private-user/y", "/home/private-user",
             "/media/Private-User/x", "a private-user b", "/mnt/private-user.d/x", "ü/home/private-user/ü", "\t" + home_l + "\t"]
    win_home, win_user = "C:\\Users\\Private User", "Private User"
    wlines = ["C:\\Users\\Private User\\Games\\x", "c:/users/private user/Games/x", "D:\\Users\\Private User\\Lib",
              "C:\\Users\\Private Users\\x", "[hos host] vk: pipeline cache C:\\USERS\\PRIVATE USER\\AppData\\x.bin"]
    for home, user, windows, sample in ((home_l, user_l, 0, lines), (win_home, win_user, 1, wlines), (home_l + "/", user_l, 0, lines)):
        path = tmp / "redact.txt"
        path.write_text("".join(l + "\n" for l in sample), encoding="utf-8")
        ours = run(exe, "redact", home, user, windows, path)
        theirs = run(menu, home, user, windows, path)
        assert ours == theirs, (ours, theirs)
        assert "<user>" in ours and ("~" in ours or "%USERPROFILE%" in ours), ours
    print("PASS: the log's redaction is the Insert menu's Bug report's, line for line")
    shown = {("-", "1"): 1, ("-", "-1"): 1, ("-", "1.5"): 1, ("-", "1920x1080"): 1, ("-", "on"): 1, ("-", "OFF"): 1,
             ("-", ""): 1, ("-", "123456789012"): 1, ("-", "1234567890123"): 0, ("-", "-1x2"): 0, ("-", "Private Mic"): 0,
             ("-", "/home/x"): 0, ("-", "1.5.3"): 0, ("-", "0x10"): 1, ("RX_VK_PACE", "1"): 1, ("HOS_SECRET_TOKEN", "1"): 0,
             ("HOS_DISCORD_KEY", "on"): 0, ("HOS_CONTROL", "/tmp/sock"): 0, ("LONGSHOT_USER_DIR", "1"): 0}
    for (name, value), want in shown.items():
        assert int(run(exe, "shown", name, value)) == want, (name, value)
    print("PASS: a setting's value only when it is a number, a size or on/off; never for a secret's name")
    # the issue link
    url = run(exe, "url", "Game: Longshot closed unexpectedly during the start", "v3.1.0 (source 1234abcd)",
              "NVIDIA GeForce RTX 3090, NVIDIA 580.95.05", "Ryzen 7 7700 (16 threads), 32 GB, Windows 11 (build 26100)",
              "Longshot closed unexpectedly during the start. An unexpected error.").strip()
    assert url.startswith(form + "&") and len(url) <= 2000, url
    q = parse_qs(urlsplit(url).query)
    assert q["template"] == [".forgejo/ISSUE_TEMPLATE/1-game-problem.yml"]
    assert (ROOT / q["template"][0]).is_file()
    assert q["field:version"] == ["v3.1.0 (source 1234abcd)"] and q["field:gpu"] == ["NVIDIA GeForce RTX 3090, NVIDIA 580.95.05"]
    assert q["title"] == ["Game: Longshot closed unexpectedly during the start"] and "field:what" in q and "field:system" in q
    assert q["field:kind"] == ["Crash, freeze or black screen"] and "field:track" not in q and "field:where" not in q
    url = run(exe, "url", "T", "v1", "G", "S", "W", "Release", "Kokiri Forest", "ultrawide=1\n").strip()
    q = parse_qs(urlsplit(url).query)
    assert (q["field:track"], q["field:where"], q["field:settings"]) == (["Release"], ["Kokiri Forest"], ["ultrawide=1\n"]), q
    template = (ROOT / ".forgejo/ISSUE_TEMPLATE/1-game-problem.yml").read_text(encoding="utf-8")
    for option in ("Release", "Nightly", "Built from source", "Crash, freeze or black screen"):   # (no release candidates: #1768)
        assert re.search(r"^\s+- " + re.escape(option) + r"$", template, re.M), "the form's dropdowns have " + option
    long_url = run(exe, "url", "T", "v1", "G", "S", "w" * 3000).strip()
    assert len(long_url) <= 2000 and "field:what" not in parse_qs(urlsplit(long_url).query) and "field:version=v1" in long_url, \
        "what does not fit is left out, the rest kept"
    print("PASS: the issue link: the game-problem form, its fields, within 2000 characters")


def entries(out):
    if not out.exists():
        return []
    parts = out.read_text(encoding="utf-8").split("\n\x1e\n")
    return [tuple(p.split("\n", 1)) for p in parts if p.strip()]


def behaviour(cc, tmp):
    exe = tmp / "driver"
    (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
    # (-x c before each file: g++ takes a single -x c for the first file only, and compiles the rest as C++)
    sources = [tmp / "driver.c", ROOT / "host/hos_crash_dialog.c", ROOT / "host/hos_crash_record.c", ROOT / "host/hos_report_run.c",
               ROOT / "host/log_redact.c"]
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-DHOS_CRASH_DIALOG_TEST", "-I", str(ROOT / "host"),
                    *(x for f in sources for x in ("-x", "c", str(f))), "-o", str(exe), "-pthread", "-ldl"], check=True, timeout=180)
    home = tmp / "home" / "private-user"
    home.mkdir(parents=True)
    base = {k: v for k, v in os.environ.items() if not k.startswith(("HOS_", "RX_", "LONGSHOT_"))}
    base.update(HOME=str(home), USER="private-user", HOS_SECRET_TOKEN="abc-secret", RX_VK_PACE="1")
    count = [0]

    def game(mode, *lines, ends=1, buttons="", extra=None, folder=None, wait=True):
        count[0] += 1
        folder = folder or home / ("game-%d" % count[0])
        folder.mkdir(exist_ok=True)
        out = folder / "out.txt"
        env = dict(base, CRASH_TEST_OUT=str(out), CRASH_TEST_BUTTONS=buttons, **(extra or {}))
        # (stderr not a file: hos_crash_record_start takes a regular file there for the game's log, as the game's own stderr is,
        # over the driver's logs/longshot_log.txt, and this check's output may be one)
        subprocess.run([str(exe), str(folder), mode, *lines], env=env, timeout=30, stderr=subprocess.DEVNULL)
        deadline = time.time() + (20 if ends else 2)
        while time.time() < deadline:
            done = [e for e in entries(out) if e[0] == "END"]
            if ends and len(done) >= ends:
                break
            time.sleep(0.1)
        got = entries(out)
        assert len([e for e in got if e[0] == "END"]) == ends, (mode, got)
        return folder, got

    def boxes(got):
        return [e[1].split("\n")[0] for e in got if e[0] == "BOX"]

    _, got = game("clean")
    assert boxes(got) == [], got
    _, got = game("crash")
    assert boxes(got) == ["Longshot closed unexpectedly"] and "during the start" in got[0][1], got
    folder, got = game("crash", "[hos Main] map: Kokiri Forest")
    assert "while you were playing" in got[0][1]
    shown = (folder / "bug-reports/shown.txt").read_text(encoding="utf-8")
    assert re.fullmatch(r"crash-\d{8}T\d{6}Z(-\d+-\d+)?\.txt\n", shown), shown
    _, got = game("kill")
    assert boxes(got) == ["Longshot closed unexpectedly"] and "ended without a crash report" in got[0][1], got
    _, got = game("quit-kill")
    assert boxes(got) == [], "a quit, then the process killed while it closes: nothing (PR 498)"
    _, got = game("quit-crash")
    assert boxes(got) == ["Longshot closed unexpectedly"], "a crash after a quit still shows"
    _, got = game("clean", "[hos host] rx: RX_BACKEND=vulkan, but /g/longshot_vk.so did not load (x): nothing is drawn")
    assert boxes(got) == ["Longshot couldn't start"] and "Vulkan 1.3" in got[0][1], got
    _, got = game("relaunch", ends=2)
    assert boxes(got) == [], "a restart in the same process (execve) says nothing, from the old helper or the new"
    # issue #1374: the run after a Restart now appends to the log the run before left, at that run's offset; any
    # play session is past 8 KB, and the report searched only the log's first 8192 bytes for its run_id line, so its
    # own crash said "the game logged to no file" while its lines sat in the file past the window.
    filler = ["filler line %04d and some more text to reach past the window" % i for i in range(400)]
    folder, got = game("relaunch", *filler, ends=2, buttons="0",
                       extra={"CRASH_TEST_RELAUNCH_TO": "crash",
                              "CRASH_TEST_RESTARTED_LOG": "RESTARTED-TAIL-MARKER"})   # (the restarted run's own line)
    crash_boxes = [e[1] for e in got if e[0] == "BOX" and e[1].startswith("Longshot closed unexpectedly")]
    assert len(crash_boxes) == 1, ("the run after a restart still shows its crash once", got)
    clip = next((e[1] for e in got if e[0] == "CLIP"), "")
    assert clip.startswith("Longshot crash report"), ("Report manually copies the full report", clip[:120])
    assert "Not found: the game logged to no file" not in clip, \
        "the restarted run's report says the game logged to no file: its run_id line sits at %d bytes, past the " \
        "old 8 KB head window, and find_run_log never looked further (issue #1374)" % sum(len(x) + 1 for x in filler)
    assert "RESTARTED-TAIL-MARKER" in clip, "the restarted run's report must carry its own log tail"
    print("PASS: the run after a Restart now: its crash report finds the deep run_id line and carries the log tail")
    # round two (review): the file holds TWO runs now — the facts the report reads (map, stage) must come from this
    # run's own lines, not from the run before it. The run before played in Kokiri Forest; the restarted run crashed
    # at the start, before any map: its report must not blame "while you were playing" with the old run's map.
    folder, got = game("relaunch", *(filler + ["[hos Main] map: Kokiri Forest (spot 0)"]), ends=2, buttons="0",
                       extra={"CRASH_TEST_RELAUNCH_TO": "crash",
                              "CRASH_TEST_RESTARTED_LOG": "RESTARTED-START-MARKER"})
    box = next(e[1] for e in got if e[0] == "BOX" and e[1].startswith("Longshot closed unexpectedly"))
    clip = next((e[1] for e in got if e[0] == "CLIP"), "")
    assert "during the start" in box, \
        "the restarted run's box names the stage from its own run: the run before loaded a map, this one never did"
    assert "Map: Kokiri" not in clip and "Map: Title and intro" in clip, \
        "the restarted run's report keeps the run before's map as its own (issue #1374, review): the facts must be " \
        "read from the located run_id line on, not from byte zero of the shared log"
    assert "RESTARTED-START-MARKER" in clip, "the restarted run's own line is still in its tail"
    print("PASS: the run after a Restart now: facts (map, stage) come from its own lines, not the run before")
    # round three (review): the gate starts at the run's OWN banner, not at its run_id line: the version line sits
    # one line before the run_id, and a gate that starts at run_id always drops the version — the report falls back
    # to the crash record's, or says "not known" (P3). The case makes the two versions distinct on purpose: the
    # report must name the one in the run's own LOG, not the record's — proving the gate reaches the banner line.
    folder, got = game("relaunch", *(filler + ["[hos Main] map: Kokiri Forest (spot 0)"]), ends=2, buttons="0",
                       extra={"CRASH_TEST_RELAUNCH_TO": "crash",
                              "CRASH_TEST_RESTARTED_LOG": "RESTARTED-START-MARKER",
                              "CRASH_TEST_LOG_VERSION": "vlog-9",       # (what the restarted run logs)
                              "CRASH_TEST_RECORD_VERSION": "vrec-7"})   # (what the crash record alone would say)
    clip = next((e[1] for e in got if e[0] == "CLIP"), "")
    assert "Version: vlog-9" in clip and "Version: vrec-7" not in clip, \
        "the restarted run's report lost the version from its own log: the gate started at the run_id line, one " \
        "line after the version banner, and the report fell back to the crash record (issue #1374, review round " \
        "three): %s" % [l for l in clip.splitlines() if l.startswith("Version")][:1]
    assert "Map: Kokiri" not in clip, \
        "reaching back to this run's banner must not reach back past it: the run before's map is still out " \
        "(issue #1374, review round three)"
    print("PASS: the report's version comes from the run's own banner, and the gate still excludes the run before")
    # round three, P2 (Windows): hos_log batches up to 250 ms, and the startup failures write straight to stderr —
    # physically BEFORE the queued banner — so a reader gated at the run's boundary would never see the failure.
    # The shipped game writes its boundary first (hos_log_flush right after run_id). Pinned on the source: Windows
    # can't be built here, and the flush's own writer machinery is the tested logger, not this file's.
    main_c = open(os.path.join(ROOT, "host", "hos_main.c"), encoding="utf-8").read()
    at_banner = main_c.index('hos_log("run_id: %s"')
    assert "hos_log_flush();" in main_c[at_banner:at_banner + 900], \
        "the run's banner and run_id are not on disk before the direct-stderr startup diagnostics can write: on " \
        "Windows the batched logger lands after them and the startup-failure box loses its log (issue #1374, " \
        "review round three)"
    for mode, extra in (("crash", {"HOS_HEADLESS": "1"}), ("crash", {"HOS_CRASH_WATCH": "0"}), ("prepare", {}),
                        ("crash", {"HOS_PGO": str(tmp / "pgo")}), ("crash", {"GAMESCOPE_WAYLAND_DISPLAY": "gamescope-0"}),
                        ("crash", {"XDG_CURRENT_DESKTOP": "gamescope"})):
        _, got = game(mode, ends=0, extra=extra)
        assert got == [], (mode, extra, got)
    print("PASS: real runs: crash, kill, quit then kill, crash after quit, clean, Vulkan missing, restart; no helper when off")

    # the next start: the first one with this window lists what was pending; a crash nobody showed is shown once
    folder = home / "fallback"
    pending = folder / "bug-reports/pending"
    pending.mkdir(parents=True)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(time.time() - 60))
    old = pending / ("crash-%s.txt" % stamp)
    old.write_text("Longshot crash record 1\nversion: vtest-0\nrun_id: %s\nwhen: %s\nreason: hos_fatal\nrecord_complete: yes\n"
                   % ("f" * 32, stamp), encoding="utf-8")
    _, got = game("clean", folder=folder)
    assert boxes(got) == [] and old.name in (folder / "bug-reports/shown.txt").read_text(encoding="utf-8"), \
        "the first start with the window: what was pending is not news"
    (folder / "bug-reports/shown.txt").write_text("", encoding="utf-8")
    (folder / "out.txt").unlink()
    _, got = game("clean", folder=folder, ends=2)
    assert boxes(got) == ["Longshot closed unexpectedly"] and [e[1] for e in got if e[0] == "END"][0] == "--crash-show", got
    (folder / "out.txt").unlink()
    _, got = game("clean", folder=folder)
    assert boxes(got) == [], "shown once"
    print("PASS: a crash no helper showed is shown once at the next start; the first start lists what was pending")

    # Report automatically, the game's own: the report built from the run's log and shown, Show all of it, Send through a
    # stand-in for curl (the server's answers, no network), the answer's page and its link; then the window again, without
    # Send (sent already). A busy server: Try again; a refusal: nothing to try again
    curl = tmp / "curl.sh"
    curl.write_text("""#!/bin/sh
out=; head=; data=; url=
while [ $# -gt 0 ]; do
    case "$1" in
    --output) out="$2"; shift ;;
    --dump-header) head="$2"; shift ;;
    --data-binary) data="${2#@}"; shift ;;
    --max-time|--connect-timeout|--proto|--header|--user-agent) shift ;;
    -*) ;;
    *) url="$1" ;;
    esac
    shift
done
dir=$(dirname "$(dirname "$(dirname "$data")")")
echo "$url" >> "$dir/curl-calls.txt"
cp "$data" "$dir/sent.json"
case "$CURL_ANSWER" in
busy) printf 'HTTP/1.1 429 Too Many Requests\\r\\n\\r\\n' > "$head"; printf '{"error": "slow down"}' > "$out" ;;
refused) printf 'HTTP/1.1 400 Bad Request\\r\\n\\r\\n' > "$head"; printf '{"error": "crash is required"}' > "$out" ;;
*) printf 'HTTP/1.1 201 Created\\r\\n\\r\\n' > "$head"
   printf '{"comment": "https://g.example/o/r/issues/81#issuecomment-5", "new": true}' > "$out" ;;
esac
""", encoding="utf-8")
    curl.chmod(0o755)
    # (crash_issue=: what a build made before the reports went team-only has in its stamp, now ignored)
    stamp = "version=vtest-1\ncrash_reports=https://d.example/telemetry/crash\ncrash_issue=https://g.example/o/r/issues/81\n"
    log = ("[hos host] report: os Arch Linux", "[hos host] report: cpu Ryzen 7 7700 (16 threads), 32 GB",
           "[hos host] vk: NVIDIA GeForce RTX 3090, Vulkan 1.4, driver 0x9b118180; mesh shaders yes, host memory import yes",
           "[hos host] vk: driver: NVIDIA 580.95.05", "[hos host] report: gpu NVIDIA GeForce RTX 3090, NVIDIA 580.95.05",
           "[hos Main] map: Kokiri Forest", "[hos Main] reading %s/secret.txt" % home,
           "[hos host] CRASH: exception 0xc0000005 (read 0x8, guest 0x8) at %s/Games/longshot+0x1a2b" % home,
           "[hos host] CRASH:   x0 0000000000000001", "[hos host] CRASH: host backtrace: longshot+0x1a2b libc.so.6+0x10")
    for answer, buttons in (("sent", "0,1,0,0"), ("busy", "0,0,1"), ("refused", "0,0")):
        folder = home / ("send-" + answer)
        folder.mkdir()
        (folder / "longshot_version.txt").write_text(stamp, encoding="utf-8")
        (folder / "longshot_options.ini").write_text("ultrawide=1\nmicrophone_device=Private Mic\nkey_a=30\n", encoding="utf-8")
        _, got = game("crash", *log, folder=folder, buttons=buttons, extra={"CRASH_TEST_CURL": str(curl), "CURL_ANSWER": answer})
        kinds = [e[0] for e in got]
        calls = (folder / "curl-calls.txt").read_text(encoding="utf-8").splitlines()
        assert calls == ["https://d.example/telemetry/crash"], "posted once, to the stamp's address: %s" % calls
        report = json.loads((folder / "sent.json").read_text(encoding="utf-8"))
        assert sorted(report) == ["client", "cpu", "crash", "gpu", "log_tail", "map", "options", "os", "release", "renderer", "version"]
        assert report["client"] == "Longshot"
        assert (report["version"], report["release"], report["os"], report["map"]) == ("vtest-1", False, "Arch Linux", "Kokiri Forest")
        assert report["gpu"] == "NVIDIA GeForce RTX 3090, NVIDIA 580.95.05", "the card with its driver: %r" % report["gpu"]
        assert report["crash"] == ["CRASH: exception 0xc0000005 (read 0x8, guest 0x8) at longshot+0x1a2b",
                                   "CRASH: host backtrace: longshot+0x1a2b libc.so.6+0x10"], report["crash"]
        assert "[hos Main] reading ~/secret.txt" in report["log_tail"] and "private-user" not in json.dumps(report), report
        assert report["options"] == "ultrawide=1", "the settings, a value only as the menu's rules allow: %r" % report["options"]
        assert got[0][1].split("\n")[-1].startswith("Report automatically|Report manually|Open the log folder|Close"), got[0]
        assert got[1][1].startswith("Longshot: send to the team\n") and "Client: Longshot" in got[1][1] and \
            "Version: vtest-1" in got[1][1] and "Graphics card: NVIDIA GeForce RTX 3090, NVIDIA 580.95.05\n" in got[1][1] and \
            "CRASH: exception 0xc0000005" in got[1][1], got[1]
        assert got[1][1].split("\n")[-1] == "Send|Show all of it|Cancel|", got[1]
        if answer == "sent":
            # (the stamp's old crash_issue= and the answer's comment link are ignored: the Sent page has Done only, opens nothing)
            assert kinds == ["BOX", "BOX", "OPEN", "BOX", "BOX", "BOX", "END"], kinds
            assert got[2][1].endswith(".json") and "sending-crash-" in got[2][1], "Show all of it opens the report as it is sent"
            assert got[4][1].startswith("Longshot: sent\nSent.") and got[4][1].split("\n")[-1] == "Done|", got[4]
            assert "issues/81" not in got[4][1], "no link to the reports: only the team can read them"
            assert not got[5][1].split("\n")[-1].startswith("Report automatically"), "sent: not offered again"
            assert list((folder / "bug-reports/sent").iterdir()), "the answer kept"
        elif answer == "busy":
            assert kinds == ["BOX", "BOX", "BOX", "BOX", "END"], kinds
            assert got[2][1].startswith("Longshot: not sent\nNot sent: the crash server is busy") and \
                got[2][1].split("\n")[-1] == "Try again|Close|", got[2]
            assert got[3][1].split("\n")[-1].startswith("Report automatically|"), "not sent: offered still"
        else:
            assert kinds == ["BOX", "BOX", "BOX", "BOX", "END"], kinds
            assert "refused the report (400: crash is required)" in got[2][1] and got[2][1].split("\n")[-1] == "Close|", got[2]
        assert not list((folder / "bug-reports/pending").glob("sending-*")), "the report's files go once the window is done"
    # (no address in the stamp: not offered)
    folder = home / "send-none"
    folder.mkdir()
    (folder / "longshot_version.txt").write_text("version=vtest-1\n", encoding="utf-8")
    _, got = game("crash", *log, folder=folder, extra={"CRASH_TEST_CURL": str(curl)})
    assert got[0][1].split("\n")[-1].startswith("Report manually|"), got[0]
    print("PASS: Report automatically: the game's own report shown, Show all of it, Send with curl, the answer, offered once")

    # Send crash reports automatically (the launcher's choice, longshot_crash.ini send=1): the same report posted with no
    # window at all; a busy server: the window as always, Report automatically still offered
    for answer in ("sent", "busy"):
        folder = home / ("auto-" + answer)
        folder.mkdir()
        (folder / "longshot_version.txt").write_text(stamp, encoding="utf-8")
        (folder / "longshot_crash.ini").write_text("send=1\n", encoding="utf-8")
        _, got = game("crash", *log, folder=folder, buttons="3", extra={"CRASH_TEST_CURL": str(curl), "CURL_ANSWER": answer})
        calls = (folder / "curl-calls.txt").read_text(encoding="utf-8").splitlines()
        assert calls == ["https://d.example/telemetry/crash"], "posted once, to the stamp's address: %s" % calls
        report = json.loads((folder / "sent.json").read_text(encoding="utf-8"))
        assert report["crash"][0].startswith("CRASH: exception 0xc0000005") and report["version"] == "vtest-1", report
        run_log = (folder / "logs/longshot_log.txt").read_text(encoding="utf-8")
        if answer == "sent":
            assert boxes(got) == [], "sent on its own: no window: %s" % got
            assert list((folder / "bug-reports/sent").iterdir()), "the answer kept: not sent again"
            copies = list((folder / "bug-reports/sent").glob("*.json"))
            assert len(copies) == 1 and json.loads(copies[0].read_text(encoding="utf-8")) == report, \
                "a copy of what was sent kept beside its answer: %s" % copies
            assert "[hos crash] sent on its own (longshot_crash.ini): Sent. Thank you: the Longshot team has it." in run_log, \
                "the answer in the run's log"
        else:
            assert boxes(got) == ["Longshot closed unexpectedly"], got
            assert got[0][1].split("\n")[-1].startswith("Report automatically|"), "not sent: the window offers it still"
            assert "[hos crash] sent on its own (longshot_crash.ini): Not sent: the crash server is busy" in run_log, \
                "not sent: that in the run's log too"
            assert not list((folder / "bug-reports").glob("sent/*.json")), "nothing sent: no copy"
        assert not list((folder / "bug-reports/pending").glob("sending-*")), "the report's files go once it is done"
    print("PASS: Send crash reports automatically: posted with no window; a report that can't be sent shows the window")

    # Report: what it holds, and what it never does
    folder = home / "Longshot report"
    folder.mkdir()
    (folder / "longshot_options.ini").write_text("options_version=5\nultrawide=1\nmicrophone_device=Private Mic\nkey_a=30\n"
                                                 "output_res=1920x1080\n", encoding="utf-8")
    (folder / "user").mkdir()
    (folder / "user/save.bin").write_bytes(b"SAVEDATA")
    (folder / "image.bin").write_bytes(b"GAMEBYTES")
    (folder / "prod.keys").write_text("KEYSECRET\n", encoding="utf-8")
    (folder / "bug-reports/pending").mkdir(parents=True)
    for k in range(11):   # (reports saved before: the newest ten kept, this one among them)
        (folder / ("bug-reports/pending/report-20200101T0000%02dZ.txt" % k)).write_text("old", encoding="utf-8")
    cache = "[hos host] vk: pipeline cache %s/Longshot report/cache/longshot_vk_pipelines.bin (1 KB)" % home
    _, got = game("crash", cache, "[hos host] vk: NVIDIA GeForce RTX 3090, Vulkan 1.4, driver 0x1; mesh shaders yes",
                  "[hos host] vk: driver: NVIDIA 580.95.05", "[hos Main] map: Kokiri Forest", folder=folder, buttons="1,0,1")
    kinds = [e[0] for e in got]
    assert kinds == ["BOX", "OPEN", "BOX", "CLIP", "OPEN", "BOX", "END"], kinds
    assert got[1][1] == str(folder / "logs"), "Open the log folder opens the run's logs folder"
    clip, link = got[3][1], got[4][1]
    for section in ("### What happened", "### Summary (what Report automatically sends, too)", "Release build: No",
                    "Renderer: ", "Map: ", "CPU and RAM: ", "Settings: ", "Crash: ", "### Longshot version", "### Graphics card and driver version",
                    "### CPU, RAM and operating system", "### Changes from the defaults", "### Crash record",
                    "### The end of longshot_log.txt", "### Not in this report", "vtest-1", "NVIDIA GeForce RTX 3090, NVIDIA 580.95.05",
                    "Longshot crash record 1", "run_id: ", "ultrawide=1", "output_res=1920x1080",
                    "microphone_device=(value left out)", "RX_VK_PACE=1", "HOS_SECRET_TOKEN=(value left out)",
                    "~/Longshot report/cache/longshot_vk_pipelines.bin", "while you were playing"):
        assert section in clip, section
    for secret in ("SAVEDATA", "GAMEBYTES", "KEYSECRET", "private-user", str(home), "Private Mic", "abc-secret", "key_a"):
        assert secret not in clip, secret
    reports = sorted((folder / "bug-reports/pending").glob("report-*.txt"))
    assert reports[-1].read_text(encoding="utf-8") == clip, "the report is written beside the records too"
    assert len(reports) == 10 and reports[0].name == "report-20200101T000002Z.txt", ("the newest ten reports kept", reports)
    assert link.startswith(source_form()) and len(link) <= 2000 and "field%3Aversion" not in link and "field:version=vtest-1" in link
    assert "Longshot%20report" not in link and "private-user" not in link
    assert "on your clipboard" in got[5][1] and "Open the report" in got[5][1].split("\n")[-1], got[5][1]
    assert "It is saved as " + str(reports[-1]) + "." in got[5][1], "the box names the saved report's path"
    # nothing took the text: the box says so, names the file and opens it
    (folder / "out.txt").unlink()
    _, got = game("crash", folder=folder, buttons="0,0", extra={"CRASH_TEST_CLIP": "0"})
    kinds = [e[0] for e in got]
    assert kinds == ["BOX", "CLIP", "OPEN", "BOX", "OPEN", "BOX", "END"], kinds
    ready = got[3][1]
    assert "on your clipboard" not in ready and "could not be put on the clipboard" in ready, ready
    newest = sorted((folder / "bug-reports/pending").glob("report-*.txt"))[-1]
    assert "It is saved as " + str(newest) + "." in ready and got[4][1] == str(newest), "Open the report opens the saved file"
    print("PASS: Report: the clipboard's text has the version, GPU, system, settings, record and log end, and nothing private")


def source_form():
    text = (ROOT / "host/hos_crash_dialog.h").read_text(encoding="utf-8")
    return re.search(r'#define HOS_CRASH_ISSUE_FORM "([^"]+)"', text).group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    form = source_checks()
    with tempfile.TemporaryDirectory(prefix="crash-dialog-") as t:
        tmp = Path(t)
        suffix = ".exe" if os.name == "nt" else ""
        exe, menu = tmp / ("fixture" + suffix), tmp / ("menu" + suffix)
        (tmp / "fixture.c").write_text(FIXTURE, encoding="utf-8")
        (tmp / "menu.cpp").write_text(MENU_REDACT, encoding="utf-8")
        subprocess.run([args.cxx, "-x", "c", "-std=c11", "-D_CRT_SECURE_NO_WARNINGS", "-Wall", "-Wextra", "-Werror", "-I",
                        str(ROOT / "host"), str(tmp / "fixture.c"), "-o", str(exe)], check=True, timeout=120)
        subprocess.run([args.cxx, "-std=c++20", "-O1", *([] if os.name == "nt" else ["-pthread"]), "-I", str(ROOT / "gpu/vk"),
                        str(tmp / "menu.cpp"), str(ROOT / "gpu/vk/vk_bug_report.cpp"), "-o", str(menu)], check=True, timeout=180)
        logic_checks(exe, menu, tmp, form)
        if os.name == "nt":
            print("real helper runs: Linux only here (POSIX processes); Windows' path is the same code with its own waits")
        else:
            behaviour(args.cxx, tmp)


if __name__ == "__main__":
    main()
