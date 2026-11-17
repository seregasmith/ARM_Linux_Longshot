// The setup window's logic, without a window: what nx2/make_game.py says and how far the build is, the log's lines, the
// settings file, the Steam entry, and the wording of times and sizes. builder/linux/setup_ui/setup_ui.cpp draws it;
// tests/test_setup_ui.py compiles this header alone and checks it (and that STAGES agrees with make_game.py's STAGE_SPAN).
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace setup {

// ---- the build's lines: make_game.py's protocol for a builder app ("@@stage <id> <text>" and the others, its docstring),
// every other line is the log

struct Stage {
    const char* id;
    const char* title;
    double lo, hi;   // (the share of the whole build it takes: make_game.py STAGE_SPAN)
};
inline constexpr Stage STAGES[] = {
    {"read", "Reading your game files", 0.0, 0.15},
    {"lift", "Translating the game", 0.15, 0.26},
    {"compile", "Compiling", 0.26, 0.87},
    {"finish", "Putting the game folder together", 0.87, 1.0},
};
inline constexpr int STAGE_COUNT = (int)(sizeof STAGES / sizeof STAGES[0]);

inline int stage_index(const std::string& id) {
    for (int i = 0; i < STAGE_COUNT; i++)
        if (id == STAGES[i].id) return i;
    return -1;
}

enum class Kind { Log, Stage, Progress, Plan, File, Eta, Times, Check, Done, Error, Other };

struct Event {
    Kind kind = Kind::Log;
    std::string id;     // (Stage: its id; File: the module's name; Check: ok or bad)
    std::string text;   // (Log: the line; Stage, Check, Error: the text; Done: the folder)
    double a = 0, b = 0, c = 0;
};

inline std::string trim_end(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// (the first word of s from i, and i past it and the spaces after)
inline std::string word(const std::string& s, size_t& i) {
    while (i < s.size() && s[i] == ' ') i++;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') j++;
    std::string w = s.substr(i, j - i);
    i = j;
    while (i < s.size() && s[i] == ' ') i++;
    return w;
}

inline bool number(const std::string& w, double& out) {
    if (w.empty()) return false;
    char* end = nullptr;
    out = std::strtod(w.c_str(), &end);
    return end && *end == 0 && std::isfinite(out);
}

inline Event parse_line(const std::string& raw) {
    Event e;
    std::string line = trim_end(raw);
    if (line.rfind("@@", 0) != 0) {
        e.kind = Kind::Log;
        e.text = line;
        return e;
    }
    size_t i = 2;
    const std::string key = word(line, i);
    const std::string rest = line.substr(i);
    e.text = rest;
    size_t k = 0;
    if (key == "stage") {
        e.kind = Kind::Stage;
        e.id = word(rest, k);
        e.text = rest.substr(k);
    } else if (key == "progress") {
        e.kind = number(word(rest, k), e.a) ? Kind::Progress : Kind::Other;
    } else if (key == "plan") {
        e.kind = number(word(rest, k), e.a) && number(word(rest, k), e.b) ? Kind::Plan : Kind::Other;
    } else if (key == "file") {
        e.kind = Kind::File;
        e.id = word(rest, k);
        number(word(rest, k), e.a);
        e.text = word(rest, k);
    } else if (key == "eta") {
        e.kind = number(word(rest, k), e.a) ? Kind::Eta : Kind::Other;
    } else if (key == "times") {
        e.kind = number(word(rest, k), e.a) && number(word(rest, k), e.b) && number(word(rest, k), e.c) ? Kind::Times
                                                                                                      : Kind::Other;
    } else if (key == "check") {
        e.kind = Kind::Check;
        e.id = word(rest, k);
        e.text = rest.substr(k);
    } else if (key == "done") {
        e.kind = Kind::Done;
    } else if (key == "error") {
        e.kind = Kind::Error;
    } else {
        e.kind = Kind::Other;
        e.id = key;
    }
    return e;
}

// What the build has said so far, for the window: the stage, how far, the files, the time left, how it ended
struct Progress {
    int stage = -1;             // (STAGES' index; -1: none yet, or one the window doesn't know)
    std::string stage_text;     // (what make_game.py calls it)
    double step = 0;            // (how far the stage is, 0..1)
    double best = 0;            // (the most the bar has shown: it doesn't go back when a step's own count starts again)
    int plan_changed = -1, plan_total = -1, files = 0;
    double eta = -1, eta_at = 0;
    double total_s = -1, compile_s = -1, previous_s = -1;
    bool done = false, failed = false;
    std::string folder, error;
    bool shaders_later = false;  // ("the shaders were not prepared now (...)": the game's first start prepares them)
    int stages_done = 0;         // (a bit per STAGES' index: the stages already ended)

    void feed(const Event& e, double now) {
        switch (e.kind) {
        case Kind::Stage: {
            const int i = stage_index(e.id);
            if (stage >= 0 && i != stage) stages_done |= 1 << stage;
            if (i >= 0) stages_done &= ~(1 << i);   // (a stage again: make_game.py translates again after a stale kept lift)
            stage = i, stage_text = e.text, step = 0;
            if (i >= 0 && STAGES[i].lo < best) best = STAGES[i].lo;   // (a stage again starts the bar from its beginning)
            if (e.id == "compile") files = 0;
            break;
        }
        case Kind::Progress:
            step = e.a < 0 ? 0 : e.a > 1 ? 1 : e.a;
            break;
        case Kind::Plan:
            plan_changed = (int)e.a, plan_total = (int)e.b;
            break;
        case Kind::File:
            files++;
            break;
        case Kind::Eta:
            eta = e.a, eta_at = now;
            break;
        case Kind::Times:
            total_s = e.a, compile_s = e.b, previous_s = e.c;
            break;
        case Kind::Done:
            done = true, folder = e.text, step = 1;
            if (stage >= 0) stages_done |= 1 << stage;
            break;
        case Kind::Error:
            failed = true, error = e.text;
            break;
        case Kind::Log:
            if (e.text.rfind("the shaders were not prepared now", 0) == 0) shaders_later = true;
            break;
        default:
            break;
        }
    }

    // The whole build's share done, 0..1: the stage's span, and the step's part of it
    double overall() {
        if (done) return best = 1;
        double f = best;
        if (stage >= 0) f = STAGES[stage].lo + (STAGES[stage].hi - STAGES[stage].lo) * step;
        if (f > best) best = f;
        return best;
    }

    // The time left the build said (@@eta, for the compile), counted down since it was said; -1 when unknown
    double eta_left(double now) const {
        if (eta < 0 || done || failed) return -1;
        const double left = eta - (now - eta_at);
        return left > 0 ? left : 0;
    }
};

// The log the window shows: the last lines, at most `cap`, and how many there were
struct Lines {
    std::deque<std::string> lines;
    size_t cap = 4000, total = 0;
    void add(std::string s) {
        lines.push_back(std::move(s));
        total++;
        while (lines.size() > cap) lines.pop_front();
    }
};

// Splits bytes as they come into lines (\n; a \r alone ends one too, for a tool that redraws its line)
struct Splitter {
    std::string part;
    template <class F> void feed(const char* p, size_t n, F&& emit) {
        for (size_t i = 0; i < n; i++) {
            const char ch = p[i];
            if (ch == '\n' || ch == '\r') {
                if (ch == '\n' && part.empty() && i > 0 && p[i - 1] == '\r') continue;   // (\r\n is one line end)
                emit(part);
                part.clear();
            } else {
                part += ch;
            }
        }
    }
    template <class F> void finish(F&& emit) {
        if (!part.empty()) emit(part);
        part.clear();
    }
};

// ---- the settings file: key=value lines (~/.config/longshot/setup.ini)

inline std::vector<std::pair<std::string, std::string>> parse_settings(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string line = trim_end(text.substr(i, j - i));
        i = j + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

inline std::string setting(const std::vector<std::pair<std::string, std::string>>& s, const std::string& key) {
    for (auto& kv : s)
        if (kv.first == key) return kv.second;
    return "";
}

inline std::string format_settings(const std::vector<std::pair<std::string, std::string>>& s) {
    std::string out = "# The Longshot setup window's choices (builder/linux/setup_ui)\n";
    for (auto& kv : s) {
        if (kv.second.find('\n') != std::string::npos) continue;
        out += kv.first + "=" + kv.second + "\n";
    }
    return out;
}

// ---- Steam: "Add to Steam" asks the running Steam client to add the game as a non-Steam game (steam://addnonsteamgame,
// as SteamOS's own "Add to Steam" does; TotK Recomp's installer/games.py). Steam names the entry after the file, so a short
// script with the game's name goes in the game folder: it starts the folder's own longshot (builder/linux/longshot-folder),
// which starts the game through the AppImage that built it. Not "Longshot": on a FAT or exFAT card that is the same name as
// the folder's longshot.

inline constexpr const char* STEAM_NAME = "Longshot - Ocarina of Time";

inline std::string steam_script() {
    return "#!/bin/sh\n"
           "# Starts the game in this folder; Steam names its library entry after this file (the setup window's Add to Steam,\n"
           "# builder/linux/setup_ui).\n"
           "here=$(dirname \"$(readlink -f \"$0\")\")\n"
           "exec \"$here/longshot\" \"$@\"\n";
}

// urllib.parse.quote(s, safe=''): every byte but letters, digits and _.-~ as %XX
inline std::string url_quote(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '_' || ch == '.' || ch == '-' || ch == '~') {
            out += (char)ch;
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 15];
        }
    }
    return out;
}

inline std::string steam_url(const std::string& path) { return "steam://addnonsteamgame/" + url_quote(path); }

// ---- paths and words

inline std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

inline std::string parent(const std::string& path) {
    if (path.empty() || path == "/") return "/";
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const size_t slash = p.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

inline std::string base_name(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

inline bool ends_with_ci(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)tail[i])) return false;
    return true;
}

// The game file the window offers: the .nup (make_game.py --nup)
inline bool is_game_file(const std::string& name) { return ends_with_ci(name, ".nup"); }

// "1 h 05 min", "12 min", "45 s"
inline std::string duration(double s) {
    if (s < 0) s = 0;
    const long t = (long)(s + 0.5);
    char b[64];
    if (t >= 3600)
        std::snprintf(b, sizeof b, "%ld h %02ld min", t / 3600, (t % 3600) / 60);
    else if (t >= 60)
        std::snprintf(b, sizeof b, "%ld min", (t + 30) / 60 > 59 ? 59 : (t + 30) / 60);
    else
        std::snprintf(b, sizeof b, "%ld s", t);
    return b;
}

// "12:34" or "1:02:03": the time since the build began
inline std::string clock_text(double s) {
    if (s < 0) s = 0;
    const long t = (long)s;
    char b[32];
    if (t >= 3600)
        std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, (t % 3600) / 60, t % 60);
    else
        std::snprintf(b, sizeof b, "%ld:%02ld", t / 60, t % 60);
    return b;
}

// "5.6 GB", "312 MB", "4 KB"
inline std::string size_text(uint64_t n) {
    char b[32];
    if (n >= 1000ull * 1000 * 1000)
        std::snprintf(b, sizeof b, "%.1f GB", n / 1e9);
    else if (n >= 1000ull * 1000)
        std::snprintf(b, sizeof b, "%.0f MB", n / 1e6);
    else if (n >= 1000)
        std::snprintf(b, sizeof b, "%.0f KB", n / 1e3);
    else
        std::snprintf(b, sizeof b, "%llu B", (unsigned long long)n);
    return b;
}

// The log file's name: build-<date>-<time>.log, as builder/linux/AppRun names the command line's
inline std::string log_name(int y, int mo, int d, int h, int mi, int s) {
    char b[64];
    std::snprintf(b, sizeof b, "build-%04d%02d%02d-%02d%02d%02d.log", y, mo, d, h, mi, s);
    return b;
}

}  // namespace setup
