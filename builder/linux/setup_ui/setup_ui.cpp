// The Longshot setup window: the ARM64 setup AppImage's front end for Game Mode on a handheld (builder/linux/AppRun starts it
// with no arguments when there is a display). It picks the game file (.nup) and the game folder with its own big-row file
// browser, runs the AppImage's own --cli build (nx2/make_game.py), shows its log as it comes and its progress from the builder
// lines (@@stage, @@progress ...), keeps the log next to the AppImage (LONGSHOT_LOG_DIR, AppRun's), and adds the built game
// to Steam. SDL2 and Dear ImGui (third_party/imgui and its SDL2 backends), so a controller works as well as touch, a mouse
// and a keyboard. The decisions without a window are setup_logic.h's.
//
//   longshot-setup                    the window (full screen in Game Mode: gamescope, Steam's gamepad UI)
//   longshot-setup --window           a window even there;  --fullscreen: full screen anywhere
//   longshot-setup --selftest <dir> [--size WxH] [--nup <file> --out <folder>]
//                                     every screen drawn once with made-up state, saved as <dir>/<screen>.bmp, then it
//                                     closes (SDL_VIDEODRIVER=offscreen works: no display needed). With --nup and --out, a
//                                     real build too (LONGSHOT_SETUP_BUILD's program, for a test), drawn until it ends
//
// Environment (AppRun's): LONGSHOT_TOOLS (fonts, VERSION), LONGSHOT_APPRUN (the build: <it> --cli ...; and --launch to
// play), LONGSHOT_LOG_DIR (where the logs go). LONGSHOT_SETUP_BUILD=<program> runs that instead of "AppRun --cli" (tests).
// Its own log: LONGSHOT_SETUP_LOG (AppRun's: Longshot-logs/setup-window.log next to the AppImage), a line for each step of
// its start and what it does, SDL's warnings, and whatever its libraries print. Started where nobody reads its output
// (Steam, Game Mode), AppRun sends all of it there (LONGSHOT_SETUP_LOGGED=1); from a terminal it copies its output there
// itself. LONGSHOT_SETUP_DEBUG=1: SDL's own messages too. SDL_RENDER_DRIVER=software: no GPU for the window.
#include "setup_logic.h"

#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using setup::Event;
using setup::Kind;

namespace {

// ---- small helpers

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

double now_s() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

std::string home_dir() {
    std::string h = env("HOME");
    if (h.empty()) {
        if (const passwd* pw = getpwuid(getuid())) h = pw->pw_dir;
    }
    return h.empty() ? "/" : h;
}

bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

uint64_t file_size(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

bool mkdirs(const std::string& p) {
    if (p.empty() || is_dir(p)) return true;
    if (!mkdirs(setup::parent(p))) return false;
    return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string read_text(const std::string& p) {
    std::string out;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

bool write_text(const std::string& p, const std::string& text) {
    const std::string tmp = p + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) return false;
    return std::rename(tmp.c_str(), p.c_str()) == 0;
}

// The free space where a folder is, or would be (its nearest existing parent); 0 when unknown
uint64_t free_space(std::string p) {
    while (!p.empty() && !is_dir(p)) p = setup::parent(p);
    struct statvfs vs;
    if (p.empty() || statvfs(p.c_str(), &vs) != 0) return 0;
    return (uint64_t)vs.f_bavail * vs.f_frsize;
}

std::string program_on_path(const char* name) {
    std::string path = env("PATH");
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(':', i);
        if (j == std::string::npos) j = path.size();
        const std::string cand = setup::join(path.substr(i, j - i), name);
        if (!path.substr(i, j - i).empty() && access(cand.c_str(), X_OK) == 0) return cand;
        i = j + 1;
    }
    return "";
}

// Runs argv detached (its own session, no output, not waited for: a double fork, so no zombie is left)
bool spawn_detached(const std::vector<std::string>& argv) {
    const pid_t a = fork();
    if (a < 0) return false;
    if (a == 0) {
        setsid();
        if (fork() == 0) {
            const int null = open("/dev/null", O_RDWR);
            if (null >= 0) dup2(null, 0), dup2(null, 1), dup2(null, 2);
            std::vector<char*> args;
            for (auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
            args.push_back(nullptr);
            execvp(args[0], args.data());
            _exit(127);
        }
        _exit(0);
    }
    int st;
    waitpid(a, &st, 0);
    return true;
}

// ---- the window's own log (LONGSHOT_SETUP_LOG): "[seconds since start] what", on stderr, which goes to the log

double T0 = 0;

void note(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[%8.3f] %s\n", now_s() - T0, msg);
}

int saved_stderr = -1;

// From a terminal: stdout and stderr through a pipe, copied to the log file and to the terminal by a thread (whatever the
// libraries print, Mesa's and SDL's, comes along)
void tee_into(const std::string& path) {
    const int log = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log < 0) return;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) {
        close(log);
        return;
    }
    std::fflush(stdout), std::fflush(stderr);
    saved_stderr = fcntl(2, F_DUPFD_CLOEXEC, 3);
    dup2(p[1], 1), dup2(p[1], 2);
    close(p[1]);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::thread([r = p[0], log, out = saved_stderr] {
        char buf[4096];
        for (;;) {
            const ssize_t n = read(r, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            if (write(log, buf, (size_t)n) < 0) {
            }
            if (out >= 0 && write(out, buf, (size_t)n) < 0) {
            }
        }
    }).detach();
}

// Before an exec (Play): the program's output back on the real stderr, not on a pipe nobody reads any more
void untee() {
    if (saved_stderr < 0) return;
    std::fflush(stdout), std::fflush(stderr);
    dup2(saved_stderr, 1), dup2(saved_stderr, 2);
}

// ---- the build: a child in its own process group, its stdout and stderr in one pipe, read by a thread

struct Build {
    pid_t pid = -1;
    int fd = -1;
    std::thread reader;
    std::mutex mu;
    std::vector<std::string> pending;
    std::atomic<bool> finished{false};
    int status = 0;          // (waitpid's)
    double stop_at = -1;     // (when Stop was asked: SIGTERM then; SIGKILL 5 s later)
    bool stopped = false;

    bool running() const { return pid > 0 && !finished; }

    bool start(const std::vector<std::string>& argv, std::string& err) {
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) {
            err = std::string("pipe: ") + std::strerror(errno);
            return false;
        }
        const pid_t child = fork();
        if (child < 0) {
            err = std::string("fork: ") + std::strerror(errno);
            close(p[0]), close(p[1]);
            return false;
        }
        if (child == 0) {
            setpgid(0, 0);
            const int null = open("/dev/null", O_RDONLY);
            if (null >= 0) dup2(null, 0);
            dup2(p[1], 1), dup2(p[1], 2);
            std::vector<char*> args;
            for (auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
            args.push_back(nullptr);
            execv(args[0], args.data());
            std::fprintf(stderr, "could not start %s: %s\n", args[0], std::strerror(errno));
            _exit(127);
        }
        setpgid(child, child);
        close(p[1]);
        pid = child, fd = p[0], finished = false, status = 0, stop_at = -1, stopped = false;
        reader = std::thread([this] { read_all(); });
        return true;
    }

    void read_all() {
        setup::Splitter split;
        char buf[16384];
        std::vector<std::string> got;
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got.clear();
            split.feed(buf, (size_t)n, [&](const std::string& l) { got.push_back(l); });
            std::lock_guard<std::mutex> g(mu);
            for (auto& l : got) pending.push_back(std::move(l));
        }
        split.finish([&](const std::string& l) {
            std::lock_guard<std::mutex> g(mu);
            pending.push_back(l);
        });
        close(fd);
        fd = -1;
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        status = st;
        finished = true;
    }

    std::vector<std::string> take() {
        std::lock_guard<std::mutex> g(mu);
        std::vector<std::string> out;
        out.swap(pending);
        return out;
    }

    void stop() {
        if (!running()) return;
        stopped = true;
        stop_at = now_s();
        kill(-pid, SIGTERM);
    }

    void tick() {   // (SIGKILL for a build that SIGTERM didn't end in 5 s)
        if (running() && stop_at > 0 && now_s() - stop_at > 5) {
            kill(-pid, SIGKILL);
            stop_at = now_s() + 1e9;
        }
    }

    void join() {
        if (reader.joinable()) reader.join();
        pid = -1;
    }

    ~Build() {
        if (running()) {
            kill(-pid, SIGKILL);
        }
        join();
    }
};

// ---- the file browser's folder

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};

std::vector<Entry> list_dir(const std::string& path, bool files, std::string& err, bool zip = false) {
    std::vector<Entry> out;
    err.clear();
    DIR* d = opendir(path.c_str());
    if (!d) {
        err = std::strerror(errno);
        return out;
    }
    while (dirent* de = readdir(d)) {
        const std::string name = de->d_name;
        if (name.empty() || name[0] == '.') continue;
        struct stat st;
        if (stat(setup::join(path, name).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            out.push_back({name, true, 0});
        } else if (files && S_ISREG(st.st_mode) && (zip ? name.size() >= 4 && !strcasecmp(name.c_str() + name.size() - 4, ".zip")
                                                      : setup::is_game_file(name))) {
            out.push_back({name, false, (uint64_t)st.st_size});
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

struct Place {
    std::string label, path;
};

// Home, its Downloads and Games, and the removable media (an SD card): /run/media/<user>/*, /media/<user>/*, /media/*, /mnt/*
std::vector<Place> places() {
    std::vector<Place> out;
    const std::string home = home_dir();
    out.push_back({"Home", home});
    for (const char* sub : {"Downloads", "Games"})
        if (is_dir(setup::join(home, sub))) out.push_back({sub, setup::join(home, sub)});
    std::string user = env("USER");
    if (user.empty()) user = setup::base_name(home);
    std::vector<std::string> roots = {"/run/media/" + user, "/media/" + user, "/media", "/mnt"};
    for (auto& root : roots) {
        std::string err;
        for (auto& e : list_dir(root, false, err)) {
            const std::string p = setup::join(root, e.name);
            if (p == "/media/" + user) continue;
            bool seen = false;
            for (auto& o : out) seen |= o.path == p;
            if (!seen) out.push_back({e.name, p});
        }
    }
    out.push_back({"Whole system", "/"});
    return out;
}

// ---- the window's state

enum class Screen { Home, Browse, Build };
enum class ShaderTask { Export, Inspect, Restore };

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImFont* font = nullptr;
    ImFont* bold = nullptr;
    float scale = 1;
    int width = 1280, height = 720;

    Screen screen = Screen::Home;
    bool focus_pending = true;   // (the screen's first button takes the controller's focus on its first frame)
    bool quit = false, quit_asked = false, quitting = false, stop_asked = false;
    bool reset_scroll = false;   // (the browser's list back at its top: a new folder)
    bool dismiss_popups = false; // (the questions closed, as a No: the self-test between its screens)

    std::string tools, version, apprun, log_dir, settings_path;
    std::string nup, out;
    std::string message;          // (under the buttons: Steam's answer, a problem)
    bool message_bad = false;

    // the browser
    bool browse_files = true;     // (the .nup; else the game folder)
    bool browse_dump_destination = false;
    bool browse_dump_zip = false;
    std::string browse_dir, browse_err;
    std::vector<Entry> entries;
    std::vector<Place> place_list;

    // the build
    Build build;
    setup::Progress progress;
    setup::Lines log;
    std::string log_file, build_error;
    double started = 0, ended = 0;
    bool follow = true;           // (the log keeps its last line in view until scrolled up)
    bool builder_ended = false;   // (the build's process ended and its lines were read)
    Build dump;
    ShaderTask dump_task = ShaderTask::Export;
    double dump_progress = 0;
    std::string dump_result, dump_error;
    std::string shader_zip, shader_driver;

    // ---- settings
    void load_settings() {
        const auto s = setup::parse_settings(read_text(settings_path));
        nup = setup::setting(s, "nup");
        out = setup::setting(s, "out");
        if (out.empty()) out = setup::join(home_dir(), "Games/Longshot");
    }
    void save_settings() {
        mkdirs(setup::parent(settings_path));
        write_text(settings_path, setup::format_settings({{"nup", nup}, {"out", out}}));
    }

    bool built(const std::string& folder) const {
        return access(setup::join(folder, "bin/longshot").c_str(), X_OK) == 0 && is_file(setup::join(folder, "image.bin"));
    }

    void go(Screen s) {
        screen = s;
        focus_pending = true;
    }

    void say(const std::string& text, bool bad = false) {
        message = text;
        message_bad = bad;
    }

    // ---- the browser
    void open_browser(bool files, bool dump_destination = false, bool zip = false) {
        browse_files = files;
        browse_dump_destination = dump_destination;
        browse_dump_zip = zip;
        place_list = places();
        std::string start = files ? (nup.empty() ? "" : setup::parent(nup)) : (out.empty() ? "" : setup::parent(out));
        if (dump_destination) start = setup::parent(env("LONGSHOT_BUILDER_EXE"));
        if (zip) start = setup::parent(shader_zip.empty() ? env("LONGSHOT_BUILDER_EXE") : shader_zip);
        if (start.empty() || !is_dir(start)) {
            const std::string dl = setup::join(home_dir(), "Downloads");
            start = files && is_dir(dl) ? dl : home_dir();
        }
        enter(start);
        go(Screen::Browse);
    }
    void enter(const std::string& dir) {
        std::string err;
        auto list = list_dir(dir, browse_files, err, browse_dump_zip);
        if (!err.empty() && !browse_dir.empty() && screen == Screen::Browse) {
            browse_err = "Can't open " + dir + ": " + err;
            return;
        }
        browse_dir = dir, entries = std::move(list), browse_err = err.empty() ? "" : "Can't open " + dir + ": " + err;
        focus_pending = true;
    }
    void pick_folder(const std::string& folder) {
        if (browse_dump_destination) {
            go(Screen::Home);
            start_dump(folder);
            return;
        }
        out = folder;
        save_settings();
        say("");
        go(Screen::Home);
    }

    void start_dump(const std::string& destination = {}) {
        note("shader dump: starting for %s (destination: %s)", out.c_str(),
             destination.empty() ? "beside the AppImage" : destination.c_str());
        std::string err;
        std::vector<std::string> argv{apprun, "--dump-shaders", out};
        if (!destination.empty()) argv.push_back(destination);
        start_shader_task(ShaderTask::Export, argv);
    }

    void choose_shader_zip(const std::string& path) {
        shader_zip = path;
        shader_driver.clear();
        start_shader_task(ShaderTask::Inspect, {apprun, "--inspect-shader-dump", path});
    }

    void restore_shader_zip() {
        start_shader_task(ShaderTask::Restore, {apprun, "--restore-shaders", out, shader_zip});
    }

    void start_shader_task(ShaderTask task, const std::vector<std::string>& argv) {
        dump_task = task;
        std::string err;
        dump_progress = 0;
        dump_result.clear();
        dump_error.clear();
        if (apprun.empty() || !dump.start(argv, err)) {
            const std::string failure = apprun.empty() ? "Open the Setup AppImage to manage shader dumps."
                                                       : "Could not start the shader dump operation: " + err;
            note("shader dump: %s", failure.c_str());
            say(failure, true);
            return;
        }
        say("");
    }

    void pump_dump() {
        dump.tick();
        const bool finished = dump.pid > 0 && dump.finished;
        for (const auto& line : dump.take()) {
            const auto event = setup::parse_line(line);
            if (event.kind == Kind::Progress) {
                dump_progress = event.a;
                continue;
            }
            note("shader dump: %s", line.c_str());
            if (event.kind == Kind::Done) dump_result = event.text;
            else if (event.kind == Kind::Error || event.kind == Kind::Log) dump_error = event.text;
        }
        if (!finished) return;
        dump.join();
        note("shader dump: ended (status %d%s)", dump.status, dump.stopped ? ", cancelled" : "");
        if (WIFEXITED(dump.status) && WEXITSTATUS(dump.status) == 0 && !dump_result.empty()) {
            if (dump_task == ShaderTask::Inspect) shader_driver = dump_result;
            else if (dump_task == ShaderTask::Restore) say("Shader dump restored. Start the game with Turnip Mesa " + dump_result + ".");
            else say("Shader dump saved to " + dump_result);
        } else if (dump.stopped) say("Shader dump operation cancelled.");
        else say(dump_error.empty() ? "The shader dump operation failed." : dump_error, true);
    }

    // ---- the build
    void start_build() {
        say("");
        if (!is_file(nup)) {
            say("The game file is not there: " + nup, true);
            return;
        }
        if (!mkdirs(out)) {
            say("The game folder can't be made: " + out + " (" + std::strerror(errno) + ")", true);
            return;
        }
        std::string dir = log_dir;
        if (dir.empty() || !mkdirs(dir)) {
            const std::string state = env("XDG_STATE_HOME").empty() ? setup::join(home_dir(), ".local/state") : env("XDG_STATE_HOME");
            dir = setup::join(state, "longshot/logs");
            mkdirs(dir);
        }
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        log_file = setup::join(dir, setup::log_name(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec));
        std::vector<std::string> argv;
        const std::string custom = env("LONGSHOT_SETUP_BUILD");
        if (!custom.empty()) {
            argv = {custom};
        } else if (!apprun.empty()) {
            argv = {apprun, "--cli"};
        } else {
            say("This window runs from the Longshot AppImage (no LONGSHOT_APPRUN)", true);
            return;
        }
        argv.insert(argv.end(), {"--nup", nup, "--out", out, "--log", log_file});
        if (!shader_zip.empty()) argv.insert(argv.end(), {"--shader-dump", shader_zip});
        progress = setup::Progress();
        log = setup::Lines();
        build_error.clear();
        builder_ended = false;
        follow = true;
        std::string err;
        build.join();
        if (!build.start(argv, err)) {
            say("The build could not start: " + err, true);
            return;
        }
        started = now_s(), ended = 0;
        {
            std::string cmd;
            for (auto& a : argv) cmd += (cmd.empty() ? "" : " ") + a;
            note("build: started (pid %d): %s", (int)build.pid, cmd.c_str());
        }
        log.add("== Building into " + out);
        log.add("== The log is saved as " + log_file);
        save_settings();
        go(Screen::Build);
    }

    void feed(const std::string& line) {
        const Event e = setup::parse_line(line);
        progress.feed(e, now_s());
        if (e.kind == Kind::Log) {
            log.add(e.text);
        } else if (e.kind == Kind::Stage) {
            log.add("== " + e.text);
        } else if (e.kind == Kind::Error) {
            log.add("ERROR: " + e.text);
        } else if (e.kind == Kind::Check) {
            log.add(e.id + ": " + e.text);
        }
    }

    void pump() {
        pump_dump();
        for (auto& l : build.take()) feed(l);
        build.tick();
        if (build.pid > 0 && build.finished && !builder_ended) {
            for (auto& l : build.take()) feed(l);
            build.join();
            builder_ended = true;
            ended = now_s();
            const int st = build.status;
            const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
            if (build.stopped) {
                build_error = "You stopped the build.";
            } else if (!ok && !progress.failed) {
                build_error = WIFSIGNALED(st) ? "The build ended on signal " + std::to_string(WTERMSIG(st)) + "."
                                              : "The build stopped (exit code " + std::to_string(WEXITSTATUS(st)) + ").";
            } else if (!ok) {
                build_error = progress.error;
            } else if (!progress.done) {
                progress.done = true;   // (an older make_game.py that said no @@done: its exit status is the answer)
            }
            if (!build_error.empty()) progress.failed = true;
            note("build: ended, %s (status %d, %zu lines)%s%s", ok ? "built" : build.stopped ? "stopped" : "failed", st, log.total,
                 build_error.empty() ? "" : ": ", build_error.c_str());
            if (ok && !build.stopped) log.add("== Done");
            else if (build.stopped) log.add("== " + build_error);
            else if (build_error != progress.error) log.add("ERROR: " + build_error);
            focus_pending = true;
        }
    }

    // ---- Steam and play
    void add_to_steam() {
        const std::string script = setup::join(out, setup::STEAM_NAME);
        if (read_text(script) != setup::steam_script()) {
            if (!write_text(script, setup::steam_script())) {
                say("The Steam entry's script could not be written in " + out + ": " + std::strerror(errno), true);
                return;
            }
        }
        chmod(script.c_str(), 0755);
        // (Steam's launcher writes the running client's pid; a pid file of a process that ended: Steam isn't running)
        bool running = true, found = false;
        for (const char* rel : {".steam/steam.pid", ".var/app/com.valvesoftware.Steam/.steam/steam.pid"}) {
            const std::string pidfile = setup::join(home_dir(), rel);
            if (!is_file(pidfile)) continue;
            if (!found) running = false;
            found = true;
            const long pid = std::atol(read_text(pidfile).c_str());
            if (pid > 0 && is_dir("/proc/" + std::to_string(pid))) running = true;
        }
        if (!running) {
            say("Steam is not running: start Steam, then Add to Steam again.", true);
            return;
        }
        if (FILE* f = std::fopen("/tmp/addnonsteamgamefile", "a")) std::fclose(f);   // (what lets the request through, as SteamOS's)
        std::string tool = program_on_path("steam");
        if (tool.empty()) tool = program_on_path("xdg-open");
        if (tool.empty()) {
            say("Neither steam nor xdg-open is on PATH: add " + script + " to Steam yourself.", true);
            return;
        }
        note("steam: %s %s", tool.c_str(), setup::steam_url(script).c_str());
        if (!spawn_detached({tool, setup::steam_url(script)})) {
            say("Steam could not be asked: " + std::string(std::strerror(errno)), true);
            return;
        }
        say("Steam was asked to add \"" + std::string(setup::STEAM_NAME) + "\" to its library; it appears there within a few seconds.");
    }

    void play() {
        if (!built(out)) {
            say("There is no built game in " + out + " yet.", true);
            return;
        }
        save_settings();
        note("play: %s --launch %s", apprun.empty() ? setup::join(out, "longshot").c_str() : apprun.c_str(), out.c_str());
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        untee();
        if (!apprun.empty()) {
            execl(apprun.c_str(), apprun.c_str(), "--launch", out.c_str(), (char*)nullptr);
        }
        const std::string own = setup::join(out, "longshot");
        execl(own.c_str(), own.c_str(), (char*)nullptr);
        std::fprintf(stderr, "longshot-setup: could not start the game: %s\n", std::strerror(errno));
        _exit(1);
    }
};

// ---- the look: dark, one accent, sizes from the window's height (720 lines: 1.0)

const ImVec4 ACCENT(0.36f, 0.72f, 0.45f, 1.0f);
const ImVec4 ACCENT_DIM(0.22f, 0.45f, 0.29f, 1.0f);
const ImVec4 BAD(0.94f, 0.42f, 0.38f, 1.0f);
const ImVec4 WARN(0.95f, 0.78f, 0.36f, 1.0f);
const ImVec4 DIM(0.62f, 0.64f, 0.66f, 1.0f);
const ImVec4 BG(0.075f, 0.082f, 0.09f, 1.0f);

void apply_style(App& app) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0, s.ChildRounding = 10, s.FrameRounding = 10, s.PopupRounding = 10, s.GrabRounding = 8;
    s.ScrollbarRounding = 8, s.ScrollbarSize = 18;
    s.WindowPadding = ImVec2(28, 22), s.FramePadding = ImVec2(20, 12), s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0, s.ChildBorderSize = 1, s.PopupBorderSize = 1, s.FrameBorderSize = 0;
    s.SelectableTextAlign = ImVec2(0, 0.5f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = BG;
    c[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.115f, 0.125f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.14f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.24f, 0.26f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.95f, 1.0f);
    c[ImGuiCol_TextDisabled] = DIM;
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.19f, 0.21f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.23f, 0.26f, 0.28f, 1.0f);
    c[ImGuiCol_ButtonActive] = ACCENT_DIM;
    c[ImGuiCol_Header] = ImVec4(0.17f, 0.19f, 0.21f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.21f, 0.24f, 0.26f, 1.0f);
    c[ImGuiCol_HeaderActive] = ACCENT_DIM;
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.165f, 0.18f, 1.0f);
    c[ImGuiCol_PlotHistogram] = ACCENT;
    c[ImGuiCol_NavCursor] = ACCENT;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    s.ScaleAllSizes(app.scale);
    s.FontSizeBase = 24.0f * app.scale;
}

void restyle(App& app) {
    SDL_GetRendererOutputSize(app.renderer, &app.width, &app.height);
    float sc = app.height / 720.0f;
    if (app.width / 1280.0f < sc) sc = app.width / 1280.0f;   // (a narrow window: by its width)
    app.scale = sc < 0.6f ? 0.6f : sc > 3.0f ? 3.0f : sc;
    apply_style(app);
}

// ---- drawing helpers

float em(const App& app, float n) { return n * 24.0f * app.scale; }

bool big_button(const App& app, const char* label, bool primary = false, float width = 0) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, ACCENT_DIM);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.55f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ACCENT);
    }
    const bool r = ImGui::Button(label, ImVec2(width, em(app, 2.2f)));
    if (primary) ImGui::PopStyleColor(3);
    return r;
}

void focus_here(App& app) {
    if (app.focus_pending) {
        ImGui::SetKeyboardFocusHere();
        app.focus_pending = false;
    }
}

void heading(App& app, const char* text) {
    ImGui::PushFont(app.bold, em(app, 1.6f));
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void dim_text(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, DIM);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void colored_wrapped(const ImVec4& col, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// The bottom line: what the controller's buttons do
void hints(App& app, const char* text) {
    const float h = ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - h);
    ImGui::PushFont(nullptr, em(app, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, DIM);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

float hints_height(App& app) { return em(app, 0.8f) * 1.6f + ImGui::GetStyle().ItemSpacing.y; }

bool back_pressed() {
    return !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
           (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
}

// A label and its value, on rows: "Game file   /path"   [Choose…]
bool field(App& app, const char* id, const char* label, const std::string& value, const char* empty, const std::string& note,
           bool note_bad, bool take_focus) {
    ImGui::PushID(id);
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, DIM);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const float button_w = em(app, 6.5f);
    const float text_w = ImGui::GetContentRegionAvail().x - button_w - ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
    ImGui::BeginChild("value", ImVec2(text_w, em(app, 2.2f)), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::SetCursorPos(ImVec2(em(app, 0.7f), (em(app, 2.2f) - ImGui::GetTextLineHeight()) / 2));
    if (value.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, DIM);
        ImGui::TextUnformatted(empty);
        ImGui::PopStyleColor();
    } else {
        // (a long path: its end, which says the most)
        std::string shown = value;
        const float room = text_w - em(app, 1.4f);
        size_t cut = 0;
        while (cut < value.size() && ImGui::CalcTextSize(shown.c_str()).x > room) {
            cut++;
            while (cut < value.size() && ((unsigned char)value[cut] & 0xC0) == 0x80) cut++;   // (a whole UTF-8 letter)
            shown = "\u2026" + value.substr(cut);
        }
        ImGui::TextUnformatted(shown.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (take_focus) focus_here(app);
    const bool pressed = big_button(app, "Choose…", false, button_w);
    if (!note.empty()) {
        ImGui::PushFont(nullptr, em(app, 0.85f));
        if (note_bad) colored_wrapped(BAD, note);
        else dim_text(note);
        ImGui::PopFont();
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return pressed;
}

// ---- the screens

void draw_home(App& app) {
    heading(app, "Longshot");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                         ImGui::CalcTextSize(app.version.c_str()).x);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em(app, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, DIM);
    ImGui::TextUnformatted(app.version.c_str());
    ImGui::PopStyleColor();
    dim_text("Builds The Legend of Zelda: Ocarina of Time (Nintendo Switch 2) for this device from your own copy of the game.");
    ImGui::Dummy(ImVec2(0, em(app, 0.4f)));

    const bool dumping = app.dump.pid > 0;
    ImGui::BeginDisabled(dumping);

    std::string nup_note;
    bool nup_bad = false;
    if (!app.nup.empty()) {
        if (is_file(app.nup)) nup_note = setup::size_text(file_size(app.nup));
        else nup_note = "Not there any more: choose it again.", nup_bad = true;
    }
    const bool nup_ok = !app.nup.empty() && is_file(app.nup);
    if (field(app, "nup", "Your game file (.nup)", app.nup, "Not chosen yet", nup_note, nup_bad, !nup_ok))
        app.open_browser(true);
    ImGui::Dummy(ImVec2(0, em(app, 0.2f)));

    std::string out_note;
    const bool built = app.built(app.out);
    if (built) out_note = "A game is built here.";
    else if (is_dir(app.out)) out_note = "The game will be built in this folder.";
    else out_note = "This folder will be made.";
    const uint64_t free_b = free_space(app.out);
    if (free_b) out_note += "  " + setup::size_text(free_b) + " free.";
    if (field(app, "out", "The game folder", app.out, "Not chosen yet", out_note, false, false)) app.open_browser(false);
    ImGui::Dummy(ImVec2(0, em(app, 0.2f)));
    const bool valid_dump = !app.shader_driver.empty() && is_file(app.shader_zip);
    const std::string shader_note = valid_dump ? "Turnip Mesa " + app.shader_driver : "";
    if (field(app, "shader-zip", "Shader dump (.zip, optional)", app.shader_zip, "Choose ZIP", shader_note,
              !app.shader_zip.empty() && !valid_dump && !dumping, false)) app.open_browser(true, false, true);
    if (!app.shader_zip.empty() && big_button(app, "Clear ZIP", false, em(app, 6))) {
        app.shader_zip.clear();
        app.shader_driver.clear();
        app.say("");
    }
    ImGui::Dummy(ImVec2(0, em(app, 0.6f)));

    const bool can_build = nup_ok && !app.out.empty() && (app.shader_zip.empty() || valid_dump);
    ImGui::BeginDisabled(!can_build);
    if (can_build) focus_here(app);
    if (big_button(app, built ? "Build again" : "Build the game", true, em(app, 9))) app.start_build();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!built);
    if (big_button(app, "Play", false, em(app, 5))) app.play();
    ImGui::SameLine();
    if (big_button(app, "Add to Steam", false, em(app, 8))) app.add_to_steam();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (big_button(app, "Quit", false, em(app, 5))) app.quit = true;
    ImGui::EndDisabled();

    if (dumping) {
        const char* action = app.dump_task == ShaderTask::Inspect ? "Checking ZIP" :
                             app.dump_task == ShaderTask::Restore ? "Restoring shader dump" : "Dumping shaders";
        ImGui::ProgressBar((float)std::clamp(app.dump_progress, 0.0, 1.0), ImVec2(-1, 0), action);
        ImGui::BeginDisabled(app.dump.stopped);
        if (big_button(app, "Cancel dump", false, em(app, 8))) app.dump.stop();
        ImGui::EndDisabled();
    } else if (built) {
        if (big_button(app, "Dump Shaders", false, em(app, 9))) app.start_dump();
        ImGui::SameLine();
        if (big_button(app, "Choose dump folder", false, em(app, 11))) app.open_browser(false, true);
        if (valid_dump) {
            ImGui::SameLine();
            if (big_button(app, "Restore shader dump", false, em(app, 11))) app.restore_shader_zip();
        }
    }

    if (!app.message.empty()) {
        ImGui::Dummy(ImVec2(0, em(app, 0.2f)));
        if (app.message_bad) colored_wrapped(BAD, app.message);
        else colored_wrapped(ACCENT, app.message);
    }
    if (!app.log_dir.empty()) {
        ImGui::Dummy(ImVec2(0, em(app, 0.2f)));
        ImGui::PushFont(nullptr, em(app, 0.8f));
        dim_text("Build logs go to " + app.log_dir);
        ImGui::PopFont();
    }
    hints(app, "A  choose      D-pad  move");
}

enum Icon { ICON_FILE, ICON_DIR, ICON_UP };
void draw_icon(ImVec2 p, int kind, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight();
    const float y = p.y + (size - h) / 2 + h * 0.12f, w = h * 1.05f, hh = h * 0.78f;
    const float x = p.x + h * 0.15f;
    if (kind == ICON_UP) {
        const float cx = x + w * 0.5f, t = h * 0.09f;
        dl->AddTriangleFilled(ImVec2(cx, y), ImVec2(cx - w * 0.4f, y + hh * 0.45f), ImVec2(cx + w * 0.4f, y + hh * 0.45f),
                              ImGui::GetColorU32(DIM));
        dl->AddRectFilled(ImVec2(cx - t, y + hh * 0.4f), ImVec2(cx + t, y + hh), ImGui::GetColorU32(DIM));
    } else if (kind == ICON_DIR) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w * 0.45f, y + hh * 0.3f), ImGui::GetColorU32(ACCENT_DIM), h * 0.08f);
        dl->AddRectFilled(ImVec2(x, y + hh * 0.18f), ImVec2(x + w, y + hh), ImGui::GetColorU32(ACCENT), h * 0.1f);
    } else {
        dl->AddRect(ImVec2(x + w * 0.15f, y), ImVec2(x + w * 0.85f, y + hh), ImGui::GetColorU32(ImGuiCol_Text), h * 0.08f, 0,
                    h * 0.08f);
        dl->AddLine(ImVec2(x + w * 0.3f, y + hh * 0.4f), ImVec2(x + w * 0.7f, y + hh * 0.4f), ImGui::GetColorU32(DIM), h * 0.06f);
        dl->AddLine(ImVec2(x + w * 0.3f, y + hh * 0.6f), ImVec2(x + w * 0.7f, y + hh * 0.6f), ImGui::GetColorU32(DIM), h * 0.06f);
    }
}

// A row's name (and its size at the right), drawn over its Selectable: clipped before the size
void row_text(ImVec2 min, ImVec2 max, float indent, const std::string& name, const std::string& right, ImVec4 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight(), pad = h * 0.4f;
    const float y = min.y + (max.y - min.y - h) / 2;
    float clip_x = max.x - pad;
    if (!right.empty()) {
        const float w = ImGui::CalcTextSize(right.c_str()).x;
        dl->AddText(ImVec2(max.x - pad - w, y), ImGui::GetColorU32(DIM), right.c_str());
        clip_x = max.x - pad * 3 - w;
    }
    const ImVec4 clip(min.x, min.y, clip_x, max.y);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + indent, y), ImGui::GetColorU32(col), name.c_str(), nullptr, 0,
                &clip);
}

// Touch: a finger dragged over a list scrolls it, and a drag isn't a tap on the row it ends on
void drag_scroll() {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f))
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
}
bool was_drag() { return ImGui::GetIO().MouseDragMaxDistanceSqr[0] > 12.0f * 12.0f; }

void draw_browse(App& app) {
    heading(app, app.browse_dump_zip ? "Choose a shader dump ZIP" : app.browse_files ? "Choose your game file" : "Choose the game folder");
    ImGui::PushFont(nullptr, em(app, 0.9f));
    dim_text(app.browse_dump_zip ? "Select a ZIP created with Dump Shaders."
             : app.browse_files ? "The .nup file of your copy of Ocarina of Time. Only .nup files and folders are shown."
                              : "The built game goes here: the program, the game's data and its saves.");
    ImGui::PopFont();
    ImGui::PushFont(app.bold, em(app, 1.0f));
    ImGui::TextWrapped("%s", app.browse_dir.c_str());
    ImGui::PopFont();
    if (!app.browse_err.empty()) colored_wrapped(BAD, app.browse_err);

    const float buttons_h = em(app, 2.2f) + ImGui::GetStyle().ItemSpacing.y * 2 + em(app, 0.5f);
    const float list_h = ImGui::GetContentRegionAvail().y - buttons_h - hints_height(app);
    const float side_w = std::max(em(app, 8), ImGui::GetContentRegionAvail().x * 0.24f);
    const float row = em(app, 1.9f);
    std::string go_to;
    std::string picked;

    ImGui::BeginChild("places", ImVec2(side_w, list_h), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    drag_scroll();
    for (size_t i = 0; i < app.place_list.size(); i++) {
        const Place& p = app.place_list[i];
        ImGui::PushID((int)i);
        const bool here = app.browse_dir == p.path;
        if (ImGui::Selectable(p.label.c_str(), here, ImGuiSelectableFlags_None, ImVec2(0, row)) && !was_drag()) go_to = p.path;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("list", ImVec2(0, list_h), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    if (app.reset_scroll) ImGui::SetScrollY(0), app.reset_scroll = false;
    drag_scroll();
    const float icon_w = ImGui::GetTextLineHeight() * 1.6f;
    {
        if (app.browse_dir != "/") {
            focus_here(app);
            if (ImGui::Selectable("##up", false, ImGuiSelectableFlags_None, ImVec2(0, row)) && !was_drag())
                go_to = setup::parent(app.browse_dir);
            draw_icon(ImGui::GetItemRectMin(), ICON_UP, row);
            row_text(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), icon_w, "Up one folder", "", DIM);
        }
        ImGuiListClipper clip;
        clip.Begin((int)app.entries.size(), row + ImGui::GetStyle().ItemSpacing.y);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const Entry& e = app.entries[i];
                ImGui::PushID(i);
                if (app.browse_dir == "/") focus_here(app);
                if (ImGui::Selectable("##e", false, ImGuiSelectableFlags_None, ImVec2(0, row)) && !was_drag()) {
                    if (e.dir) go_to = setup::join(app.browse_dir, e.name);
                    else picked = setup::join(app.browse_dir, e.name);
                }
                draw_icon(ImGui::GetItemRectMin(), e.dir ? ICON_DIR : ICON_FILE, row);
                row_text(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), icon_w, e.name, e.dir ? "" : setup::size_text(e.size),
                         ImGui::GetStyle().Colors[ImGuiCol_Text]);
                ImGui::PopID();
            }
        }
        if (app.entries.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, DIM);
            ImGui::TextWrapped(app.browse_dump_zip ? "No ZIP files or folders here." :
                               app.browse_files ? "No .nup files or folders here." : "No folders here.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.2f));
    if (!app.browse_files) {
        if (big_button(app, "Use this folder", true, em(app, 9))) app.pick_folder(app.browse_dir);
        ImGui::SameLine();
        const std::string sub = setup::join(app.browse_dir, "Longshot");
        if (setup::base_name(app.browse_dir) != "Longshot" && big_button(app, "A new Longshot folder here", false, em(app, 12)))
            app.pick_folder(sub);
        ImGui::SameLine();
    }
    if (big_button(app, "Cancel", false, em(app, 6))) app.go(Screen::Home);
    hints(app, "A  open / choose      B  back      D-pad  move");

    if (!picked.empty()) {
        if (app.browse_dump_zip) {
            app.choose_shader_zip(picked);
        } else {
            app.nup = picked;
            app.save_settings();
            app.say("");
        }
        app.go(Screen::Home);
    } else if (!go_to.empty()) {
        app.enter(go_to);
        app.reset_scroll = true;
    } else if (back_pressed()) {
        app.go(Screen::Home);
    }
}

void stage_rows(App& app) {
    const setup::Progress& p = app.progress;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < setup::STAGE_COUNT; i++) {
        const bool done = p.stages_done & (1 << i), now = p.stage == i && !p.done && !p.failed;
        const bool broke = p.stage == i && p.failed;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight(), r = h * 0.32f;
        const ImVec2 c(at.x + r + 2, at.y + h / 2);
        if (broke) dl->AddCircleFilled(c, r, ImGui::GetColorU32(BAD));
        else if (done) dl->AddCircleFilled(c, r, ImGui::GetColorU32(ACCENT));
        else if (now) dl->AddCircle(c, r, ImGui::GetColorU32(ACCENT), 0, h * 0.12f), dl->AddCircleFilled(c, r * 0.45f, ImGui::GetColorU32(ACCENT));
        else dl->AddCircle(c, r, ImGui::GetColorU32(DIM), 0, h * 0.08f);
        ImGui::SetCursorScreenPos(ImVec2(at.x + r * 2 + h * 0.6f, at.y));
        const std::string text = now && !p.stage_text.empty() ? p.stage_text : setup::STAGES[i].title;
        ImGui::PushStyleColor(ImGuiCol_Text, broke ? BAD : done || now ? ImGui::GetStyle().Colors[ImGuiCol_Text] : DIM);
        if (now) ImGui::PushFont(app.bold, 0.0f);
        ImGui::TextUnformatted(text.c_str());
        if (now) ImGui::PopFont();
        ImGui::PopStyleColor();
    }
}

void draw_log(App& app, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.055f, 0.06f, 1.0f));
    ImGui::BeginChild("log", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(nullptr, em(app, 0.72f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, em(app, 0.12f)));
    drag_scroll();
    const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && (io.MouseWheel > 0 || ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f))) app.follow = false;
    if (at_bottom && io.MouseWheel <= 0) app.follow = true;
    ImGuiListClipper clip;
    clip.Begin((int)app.log.lines.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const std::string& l = app.log.lines[(size_t)i];
            ImVec4 col = ImGui::GetStyle().Colors[ImGuiCol_Text];
            if (l.rfind("== ", 0) == 0) col = ACCENT;
            else if (l.rfind("ERROR", 0) == 0 || l.find("error:") != std::string::npos || l.find("FAILED") != std::string::npos ||
                     l.find("Traceback") != std::string::npos)
                col = BAD;
            else if (l.find("warning") != std::string::npos || l.find("WARNING") != std::string::npos)
                col = WARN;
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (app.follow) ImGui::SetScrollHereY(1.0f);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_build(App& app) {
    setup::Progress& p = app.progress;
    const bool running = app.build.running() || !app.builder_ended;
    const double now = app.ended > 0 ? app.ended : now_s();
    heading(app, running ? "Building the game" : p.failed ? "The build stopped" : "The game is built");
    ImGui::SameLine();
    const std::string elapsed = setup::clock_text(now - app.started);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::CalcTextSize(elapsed.c_str()).x);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em(app, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, DIM);
    ImGui::TextUnformatted(elapsed.c_str());
    ImGui::PopStyleColor();

    ImGui::BeginGroup();
    stage_rows(app);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);
    ImGui::BeginGroup();
    {
        std::string info;
        if (running) {
            if (p.stage == setup::stage_index("compile") && p.plan_total > 0) {
                info = std::to_string(p.files) + " of " + std::to_string(p.plan_changed >= 0 ? p.plan_changed : p.plan_total) +
                       " files";
                if (p.plan_changed >= 0 && p.plan_changed < p.plan_total)
                    info += " (" + std::to_string(p.plan_total - p.plan_changed) + " kept from before)";
            }
            const double left = p.eta_left(now_s());
            if (left >= 0) info += std::string(info.empty() ? "" : "\n") + "About " + setup::duration(left) + " of compiling left";
            if (app.build.stopped) info = "Stopping…";
        } else if (!p.failed) {
            info = "Built in " + setup::duration(p.total_s >= 0 ? p.total_s : now - app.started);
            if (p.previous_s > 0) info += " (the last build took " + setup::duration(p.previous_s) + ")";
        }
        if (!info.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, DIM);
            ImGui::TextWrapped("%s", info.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, em(app, 0.2f)));
    const double f = p.overall();
    char overlay[32];
    std::snprintf(overlay, sizeof overlay, "%d%%", (int)(f * 100 + 1e-6));
    if (p.failed) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, BAD);
    ImGui::ProgressBar((float)f, ImVec2(-FLT_MIN, em(app, 1.4f)), overlay);
    if (p.failed) ImGui::PopStyleColor();

    if (!running) {
        if (p.failed) colored_wrapped(BAD, app.build_error.empty() ? p.error : app.build_error);
        else if (p.shaders_later)
            colored_wrapped(WARN, "The shaders weren't prepared now: the game's first start prepares them, which takes a few minutes.");
        if (!app.message.empty()) colored_wrapped(app.message_bad ? BAD : ACCENT, app.message);
    }

    const float bottom = em(app, 2.2f) + ImGui::GetStyle().ItemSpacing.y * 3 + em(app, 0.8f) * 1.5f + hints_height(app);
    draw_log(app, std::max(em(app, 4), ImGui::GetContentRegionAvail().y - bottom));
    ImGui::PushFont(nullptr, em(app, 0.8f));
    dim_text("Log: " + app.log_file);
    ImGui::PopFont();

    if (running) {
        focus_here(app);
        if (big_button(app, app.build.stopped ? "Stopping…" : "Stop the build", false, em(app, 9)) && !app.build.stopped)
            app.stop_asked = true;
        hints(app, "A  choose      D-pad  move");
    } else if (!p.failed) {
        focus_here(app);
        if (big_button(app, "Play", true, em(app, 6))) app.play();
        ImGui::SameLine();
        if (big_button(app, "Add to Steam", false, em(app, 8))) app.add_to_steam();
        ImGui::SameLine();
        if (big_button(app, "Back", false, em(app, 6))) app.go(Screen::Home);
        if (is_file(setup::join(app.out, "cache/longshot_shader_dump_source.json"))) {
            ImGui::SameLine();
            if (big_button(app, "Dump Shaders", false, em(app, 9))) {
                app.go(Screen::Home);
                app.start_dump();
            }
        }
        hints(app, "A  choose      B  back      D-pad  move");
        if (back_pressed()) app.go(Screen::Home);
    } else {
        focus_here(app);
        if (big_button(app, "Back", false, em(app, 6))) app.go(Screen::Home);
        ImGui::SameLine();
        if (big_button(app, "Try again", true, em(app, 7))) app.start_build();
        hints(app, "A  choose      B  back      D-pad  move");
        if (back_pressed()) app.go(Screen::Home);
    }
}

// A question over the screen: Yes / No; true when answered (yes in *yes). dismiss: no longer a question (the build ended
// meanwhile): closed as a No
bool ask(App& app, const char* id, const char* text, const char* yes_label, const char* no_label, bool* yes, bool dismiss) {
    bool answered = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min((float)app.width * 0.8f, em(app, 26)), 0));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("%s", text);
        ImGui::Dummy(ImVec2(0, em(app, 0.4f)));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (big_button(app, no_label, true, em(app, 9))) *yes = false, answered = true;
        ImGui::SameLine();
        if (big_button(app, yes_label, false, em(app, 9))) *yes = true, answered = true;
        if (dismiss || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            *yes = false, answered = true;
        if (answered) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return answered;
}

void draw(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("Longshot setup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    switch (app.screen) {
    case Screen::Home: draw_home(app); break;
    case Screen::Browse: draw_browse(app); break;
    case Screen::Build: draw_build(app); break;
    }
    if (app.stop_asked && !ImGui::IsPopupOpen("stop")) ImGui::OpenPopup("stop");
    bool yes = false;
    if (ask(app, "stop", "Stop the build? You can build again later.", "Stop it", "Keep building", &yes,
            app.dismiss_popups || app.builder_ended)) {
        app.stop_asked = false;
        if (yes) app.build.stop();
    }
    if (app.quit && app.build.running() && !app.quitting) {
        app.quit = false;
        app.quit_asked = true;
    }
    if (app.quit_asked && !ImGui::IsPopupOpen("quit")) ImGui::OpenPopup("quit");
    if (ask(app, "quit", "A build is running. Stop it and quit?", "Stop and quit", "Keep building", &yes,
            app.dismiss_popups || !app.build.running())) {
        app.quit_asked = false;
        if (yes) {
            app.build.stop();
            app.quit = app.quitting = true;
        }
    }
    app.dismiss_popups = false;
    ImGui::End();
}

void frame(App& app) {
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    draw(app);
    ImGui::Render();
    SDL_SetRenderDrawColor(app.renderer, (Uint8)(BG.x * 255), (Uint8)(BG.y * 255), (Uint8)(BG.z * 255), 255);
    SDL_RenderClear(app.renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), app.renderer);
}

bool game_mode() {
    return !env("GAMESCOPE_WAYLAND_DISPLAY").empty() || env("SteamGamepadUI") == "1" || env("SteamDeck") == "1" ||
           env("XDG_CURRENT_DESKTOP") == "gamescope";
}

bool save_shot(App& app, const std::string& path) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, app.width, app.height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return false;
    const bool ok = SDL_RenderReadPixels(app.renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0 &&
                    SDL_SaveBMP(s, path.c_str()) == 0;
    SDL_FreeSurface(s);
    return ok;
}

// The self-test's own event loop: SDL's events into ImGui (the virtual controller's buttons come through SDL_PollEvent)
void selftest_frames(App& app, int n) {
    for (int i = 0; i < n; i++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) ImGui_ImplSDL2_ProcessEvent(&ev);
        app.pump();
        frame(app);
        SDL_RenderPresent(app.renderer);
        SDL_Delay(5);
    }
}

// The controller's way through, on a virtual one (SDL's virtual joystick, as a game controller): A on the game file's
// Choose (focused while there is none), down to the .nup in the browser, A, and A on Build (focused once there is a game
// file). 1: the build started this way; 0: no virtual controller here (not a failure); -1: the controller went wrong
int controller_walk(App& app, const std::string& demo) {
    const int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    SDL_Joystick* joy = idx >= 0 ? SDL_JoystickOpen(idx) : nullptr;
    if (!joy) {
        std::printf("selftest: no virtual controller here (%s): the build starts without one\n", SDL_GetError());
        return 0;
    }
    auto press = [&](SDL_GameControllerButton b) {
        SDL_JoystickSetVirtualButton(joy, b, 1);
        selftest_frames(app, 3);
        SDL_JoystickSetVirtualButton(joy, b, 0);
        selftest_frames(app, 3);
    };
    auto expect = [&](bool ok, const char* what) {
        if (!ok) std::fprintf(stderr, "selftest: controller: %s (screen %d, nup %s)\n", what, (int)app.screen, app.nup.c_str());
        return ok;
    };
    app.nup = setup::join(demo, "missing.nup");   // (no game file: the Choose button has the focus; the browser opens in demo)
    app.say("");
    app.go(Screen::Home);
    selftest_frames(app, 6);
    press(SDL_CONTROLLER_BUTTON_A);
    if (!expect(app.screen == Screen::Browse && app.browse_files && app.browse_dir == demo, "A did not open the game file's browser"))
        return -1;
    for (int i = 0; i < 3; i++) press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);   // (Up one folder, Downloads, Games, the .nup)
    press(SDL_CONTROLLER_BUTTON_A);
    if (!expect(app.screen == Screen::Home && app.nup == setup::join(demo, "Ocarina of Time.nup"), "A did not pick the .nup"))
        return -1;
    press(SDL_CONTROLLER_BUTTON_B);   // (B on the home screen: nothing)
    if (!expect(app.screen == Screen::Home && !app.quit, "B on the home screen did something")) return -1;
    press(SDL_CONTROLLER_BUTTON_A);
    if (!expect(app.screen == Screen::Build && !app.log_file.empty(), "A did not start the build")) return -1;   // (it may have ended already)
    std::printf("selftest: controller: A, down x3, A, B, A: the game file chosen and the build started\n");
    SDL_JoystickClose(joy);
    return 1;
}

// --selftest: each screen with made-up state, a few frames each (the clipper and the focus settle), saved as BMP
int selftest(App& app, const std::string& dir, const std::string& real_nup, const std::string& real_out) {
    mkdirs(dir);
    const std::string demo = setup::join(dir, "demo");
    mkdirs(setup::join(demo, "Games"));
    mkdirs(setup::join(demo, "Downloads"));
    write_text(setup::join(demo, "Ocarina of Time.nup"), std::string(4096, 'x'));
    write_text(setup::join(demo, "notes.txt"), "not shown");
    app.log_dir = "/run/media/deck/SD/Longshot-logs";
    int failed = 0;
    auto shot = [&](const char* name, const std::function<void()>& set) {
        set();
        for (int i = 0; i < 4; i++) {
            frame(app);
            if (i < 3) SDL_RenderPresent(app.renderer);
        }
        const std::string path = setup::join(dir, std::string(name) + ".bmp");
        if (!save_shot(app, path)) {
            std::fprintf(stderr, "selftest: %s: %s\n", path.c_str(), SDL_GetError());
            failed++;
        } else {
            std::printf("selftest: %s\n", path.c_str());
        }
        SDL_RenderPresent(app.renderer);
    };
    shot("home-empty", [&] { app.nup.clear(), app.out = setup::join(demo, "Games/Longshot"), app.go(Screen::Home); });
    shot("home-ready", [&] { app.nup = setup::join(demo, "Ocarina of Time.nup"), app.go(Screen::Home); });
    shot("browse-file", [&] { app.open_browser(true), app.enter(demo); });
    shot("browse-folder", [&] { app.open_browser(false), app.enter(demo); });
    auto running = [&] {
        app.progress = setup::Progress();
        app.log = setup::Lines();
        app.started = now_s() - 1234, app.ended = 0, app.builder_ended = false, app.follow = true;
        app.log_file = "/run/media/deck/SD/Longshot-logs/build-20261117-132000.log";
        for (const char* l : {"@@stage read Reading your game files", "checked 102,185 files (100%, 51 s)", "@@progress 1",
                              "@@stage lift Translating the game to PC code", "lift: functions in 212s", "@@progress 1",
                              "@@stage compile Compiling", "@@plan 1830 1900", "@@eta 1260"}) {
            app.feed(l);
        }
        for (int i = 0; i < 412; i++) app.feed("@@file code_" + std::to_string(i) + ".c 3.1 new");
        for (int i = 0; i < 60; i++)
            app.feed("  code_" + std::to_string(i) + ".c      " + std::to_string(20 + i % 9) + " MB       3s  (" + std::to_string(600 + i) + "s)");
        app.feed("warning: a made-up warning line, to see its colour");
        app.feed("@@progress 0.31");
        app.go(Screen::Build);
    };
    // (the build's process is not running in the self-test: builder_ended false keeps the running screen)
    shot("build-running", running);
    shot("build-done", [&] {
        running();
        for (const char* l : {"@@progress 1", "@@stage finish Putting the game folder together", "@@progress 1",
                              "the shaders were not prepared now (no Vulkan device): the game's first start prepares them",
                              "@@times 4321 3600 4800", "@@done /home/deck/Games/Longshot"})
            app.feed(l);
        app.builder_ended = true, app.ended = app.started + 4321;
        app.say("");
        app.go(Screen::Build);
    });
    shot("build-failed", [&] {
        running();
        app.feed("Traceback (most recent call last):");
        app.feed("@@error The compiler stopped: clang: error: unsupported option '-mcx16' for target 'aarch64-unknown-linux-gnu'");
        app.builder_ended = true, app.ended = app.started + 900;
        app.progress.failed = true, app.build_error = app.progress.error;
        app.go(Screen::Build);
    });
    shot("build-stop-question", [&] { running(), app.stop_asked = true; });
    app.stop_asked = false, app.dismiss_popups = true;
    frame(app);
    SDL_RenderPresent(app.renderer);
    if (!real_nup.empty()) {
        // (the window's own build path: the program run, its lines read and drawn as they come, until it ends)
        app.nup = real_nup, app.out = real_out, app.log_dir = setup::join(dir, "logs");
        const int walked = controller_walk(app, demo);
        if (walked < 0) return 1;
        if (walked == 0) {
            app.nup = real_nup;
            app.start_build();
        }
        if (app.screen != Screen::Build) {
            std::fprintf(stderr, "selftest: the build did not start: %s\n", app.message.c_str());
            return 1;
        }
        const double give_up = now_s() + 120;
        int frames = 0;
        while (!app.builder_ended && now_s() < give_up) {
            app.pump();
            frame(app);
            SDL_RenderPresent(app.renderer);
            frames++;
            SDL_Delay(10);
        }
        app.pump();
        shot("build-real", [] {});
        std::printf("selftest: real build: ended=%d done=%d failed=%d stage=%d overall=%.3f files=%d lines=%zu frames=%d "
                    "error=%s log=%s\n",
                    (int)app.builder_ended, (int)app.progress.done, (int)app.progress.failed, app.progress.stage,
                    app.progress.overall(), app.progress.files, app.log.total, frames,
                    (app.build_error.empty() ? "-" : app.build_error.c_str()), app.log_file.c_str());
        if (!app.builder_ended) failed++;
    }
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string selftest_dir, real_nup, real_out;
    int want_w = 0, want_h = 0;
    bool force_window = false, force_full = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--selftest" && i + 1 < argc) selftest_dir = argv[++i];
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &want_w, &want_h);
        else if (a == "--nup" && i + 1 < argc) real_nup = argv[++i];
        else if (a == "--out" && i + 1 < argc) real_out = argv[++i];
        else if (a == "--window") force_window = true;
        else if (a == "--fullscreen") force_full = true;
        else {
            std::fprintf(stderr, "usage: longshot-setup [--window|--fullscreen] [--selftest <dir> [--size WxH]]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    T0 = now_s();
    const std::string own_log = env("LONGSHOT_SETUP_LOG");
    if (selftest_dir.empty() && !own_log.empty() && env("LONGSHOT_SETUP_LOGGED").empty()) tee_into(own_log);

    App app;
    app.tools = env("LONGSHOT_TOOLS");
    app.apprun = env("LONGSHOT_APPRUN");
    app.log_dir = env("LONGSHOT_LOG_DIR");
    const std::string config = env("XDG_CONFIG_HOME").empty() ? setup::join(home_dir(), ".config") : env("XDG_CONFIG_HOME");
    app.settings_path = setup::join(config, "longshot/setup.ini");
    app.version = setup::trim_end(read_text(setup::join(app.tools, "VERSION")));
    if (app.version.empty()) app.version = "Longshot";
    if (selftest_dir.empty()) app.load_settings();
    {
        utsname u{};
        uname(&u);
        note("longshot-setup %s, %s %s %s, pid %d", app.version.c_str(), u.sysname, u.release, u.machine, (int)getpid());
        for (const char* name : {"DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "GAMESCOPE_WAYLAND_DISPLAY",
                                 "SteamGameId", "SteamAppId", "SteamGamepadUI", "SteamDeck", "SDL_VIDEODRIVER", "SDL_RENDER_DRIVER",
                                 "LD_PRELOAD", "APPIMAGE", "LONGSHOT_LOG_DIR"})
            if (std::getenv(name)) note("  %s=%s", name, std::getenv(name));
        SDL_version built, linked;
        SDL_VERSION(&built);
        SDL_GetVersion(&linked);
        note("SDL %d.%d.%d (built with %d.%d.%d)", linked.major, linked.minor, linked.patch, built.major, built.minor, built.patch);
    }
    SDL_LogSetOutputFunction([](void*, int, SDL_LogPriority, const char* msg) { note("SDL: %s", msg); }, nullptr);
    SDL_LogSetAllPriority(env("LONGSHOT_SETUP_DEBUG").empty() ? SDL_LOG_PRIORITY_WARN : SDL_LOG_PRIORITY_VERBOSE);

    if (!selftest_dir.empty()) {
        if (env("SDL_VIDEODRIVER").empty()) setenv("SDL_VIDEODRIVER", "offscreen", 1);
        app.settings_path = setup::join(selftest_dir, "setup.ini");
    }
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    {
        std::string drivers;
        for (int i = 0; i < SDL_GetNumVideoDrivers(); i++) drivers += std::string(i ? ", " : "") + SDL_GetVideoDriver(i);
        note("SDL's video drivers here: %s", drivers.c_str());
    }
    note("SDL_Init: video");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        note("longshot-setup: SDL's video: %s", SDL_GetError());
        return 1;
    }
    // (an SDL that can't reach the display falls back to its offscreen driver: a window nobody sees, and Steam's Game Mode
    // waits for it for ever. That was an SDL2 built without X11, which gamescope gives its programs. Then gamescope's own
    // Wayland socket is tried, and else the window gives up, so that Steam goes back to its library and the log says why)
    const std::string driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "";
    if (selftest_dir.empty() && env("SDL_VIDEODRIVER").empty() && (driver == "offscreen" || driver == "dummy")) {
        note("no display: SDL took its %s driver, which shows nothing (DISPLAY=%s, WAYLAND_DISPLAY=%s)", driver.c_str(),
             env("DISPLAY").c_str(), env("WAYLAND_DISPLAY").c_str());
        bool shown = false;
        if (!env("GAMESCOPE_WAYLAND_DISPLAY").empty() && env("WAYLAND_DISPLAY").empty()) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            setenv("WAYLAND_DISPLAY", env("GAMESCOPE_WAYLAND_DISPLAY").c_str(), 1);
            note("trying gamescope's Wayland socket, %s", env("WAYLAND_DISPLAY").c_str());
            setenv("SDL_VIDEODRIVER", "wayland", 1);
            shown = SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
            unsetenv("SDL_VIDEODRIVER");   // (not for the build, nor for the game Play starts)
            if (!shown) note("gamescope's Wayland: %s", SDL_GetError());
        }
        if (!shown) {
            note("longshot-setup: no display this SDL can open: it needs X11 or Wayland (the drivers above)");
            return 1;
        }
    }
    note("SDL_Init: video done, driver %s, %d display(s)", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
         SDL_GetNumVideoDisplays());
    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) note("display 0: %dx%d at %d Hz", mode.w, mode.h, mode.refresh_rate);
    // (controllers apart: a window without them is still a window, and the log says which step a hang is in)
    note("SDL_Init: controllers");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) note("no controllers: %s", SDL_GetError());
    else note("SDL_Init: controllers done, %d joystick(s)", SDL_NumJoysticks());
    const bool full = !selftest_dir.empty() ? false : force_full || (game_mode() && !force_window);
    int w = want_w > 0 ? want_w : 1280, h = want_h > 0 ? want_h : 800;
    SDL_Rect usable;
    if (selftest_dir.empty() && !full && want_w <= 0 && SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        w = std::min(w, usable.w * 9 / 10), h = std::min(h, usable.h * 9 / 10);
    }
    note("window: %dx%d%s (Game Mode: %s)", w, h, full ? ", full screen" : "", game_mode() ? "yes" : "no");
    app.window = SDL_CreateWindow("Longshot setup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!app.window) {
        note("longshot-setup: the window: %s", SDL_GetError());
        return 1;
    }
    {
        std::string drivers;
        for (int i = 0; i < SDL_GetNumRenderDrivers(); i++) {
            SDL_RendererInfo ri;
            if (SDL_GetRenderDriverInfo(i, &ri) == 0) drivers += std::string(drivers.empty() ? "" : ", ") + ri.name;
        }
        note("window: created; renderers here: %s", drivers.c_str());
    }
    if (selftest_dir.empty()) {
        note("renderer: the GPU's (accelerated, vsync)");
        app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!app.renderer) note("renderer: no GPU renderer (%s): the software one", SDL_GetError());
    }
    if (!app.renderer) app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_SOFTWARE);
    if (!app.renderer) {
        note("longshot-setup: the renderer: %s", SDL_GetError());
        return 1;
    }
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(app.renderer, &ri) == 0)
            note("renderer: %s%s%s", ri.name, ri.flags & SDL_RENDERER_ACCELERATED ? ", accelerated" : "",
                 ri.flags & SDL_RENDERER_PRESENTVSYNC ? ", vsync" : "");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavEscapeClearFocusItem = false;
    io.ConfigNavCursorVisibleAlways = true;
    // (the bundled fonts: the AppImage's third_party/fonts, as the game's overlays use; ImGui's own when they're missing)
    const std::string fonts = setup::join(app.tools.empty() ? "." : app.tools, "third_party/fonts/libertinus");
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    if (is_file(setup::join(fonts, "LibertinusSans-Regular.ttf")))
        app.font = io.Fonts->AddFontFromFileTTF(setup::join(fonts, "LibertinusSans-Regular.ttf").c_str(), 24.0f, &cfg);
    if (!app.font) app.font = io.Fonts->AddFontDefault();
    if (is_file(setup::join(fonts, "LibertinusSans-Bold.ttf")))
        app.bold = io.Fonts->AddFontFromFileTTF(setup::join(fonts, "LibertinusSans-Bold.ttf").c_str(), 24.0f, &cfg);
    if (!app.bold) app.bold = app.font;
    io.FontDefault = app.font;
    ImGui_ImplSDL2_InitForSDLRenderer(app.window, app.renderer);
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll);
    ImGui_ImplSDLRenderer2_Init(app.renderer);
    restyle(app);
    note("ImGui: ready, %dx%d pixels, scale %.2f, font %s", app.width, app.height, app.scale,
         app.font && app.font != app.bold ? "Libertinus" : "ImGui's own");

    if (!selftest_dir.empty()) {
        const int r = selftest(app, selftest_dir, real_nup, real_out);
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_Quit();
        return r;
    }

    int held = 0, busy_frames = 3;
    bool exposed = false;
    long frames = 0;
    note("running");
    while (!app.quit || app.build.running() || app.dump.pid > 0) {
        if (app.quitting && app.build.running() && app.build.stop_at < 0) app.build.stop();
        SDL_Event ev;
        // (idle: a few frames a second, woken by input; the build's lines come at least every 100 ms; held buttons and
        // drags keep every frame, for ImGui's key repeat and scrolling)
        const bool wait = held <= 0 && busy_frames <= 0;
        bool got = wait ? SDL_WaitEventTimeout(&ev, app.build.running() || app.dump.running() ? 100 : 250) != 0 : SDL_PollEvent(&ev) != 0;
        while (got) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            switch (ev.type) {
            case SDL_QUIT:
                note("quit asked (window closed, or Steam)");
                if (!app.dump.stopped) app.dump.stop();
                app.quit = true;
                break;
            case SDL_CONTROLLERDEVICEADDED: {
                const char* name = SDL_GameControllerNameForIndex(ev.cdevice.which);
                note("controller: %s", name ? name : "(no name)");
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: note("controller removed"); break;
            case SDL_KEYDOWN:
                if (!ev.key.repeat) held++;
                break;
            case SDL_KEYUP: held--; break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_FINGERDOWN: held++; break;
            case SDL_CONTROLLERBUTTONUP:
            case SDL_MOUSEBUTTONUP:
            case SDL_FINGERUP: held--; break;
            case SDL_CONTROLLERAXISMOTION:
                if (ev.caxis.value > 12000 || ev.caxis.value < -12000) busy_frames = 30;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    restyle(app);
                    note("window: %dx%d pixels, scale %.2f", app.width, app.height, app.scale);
                }
                if (ev.window.event == SDL_WINDOWEVENT_SHOWN) note("window: shown");
                if (ev.window.event == SDL_WINDOWEVENT_EXPOSED && !exposed) note("window: on screen (exposed)"), exposed = true;
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) note("window: focused");
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) note("window: focus lost");
                break;
            default: break;
            }
            if (held < 0) held = 0;
            busy_frames = std::max(busy_frames, 4);
            got = SDL_PollEvent(&ev) != 0;
        }
        app.pump();
        frame(app);
        SDL_RenderPresent(app.renderer);
        if (++frames == 1) note("first frame drawn");
        if (busy_frames > 0) busy_frames--;
        if (app.quit && !app.build.running() && app.dump.pid < 0) break;
    }
    app.build.join();
    note("closed after %ld frames", frames);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
