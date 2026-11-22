// The Insert menu's Save my pipeline list (#1633), first in the Debug tab's Developer section: the pipelines this play built
// (shader hashes and draw state, no game data: longshot_pipeline_states.bin) with the "Submit your pipelines" issue's template
// filled in from this run (pipeline_share.h; the builders do the same with nx2/pipeline_share.py), zipped into the Downloads
// folder; then the report on the clipboard, the folder shown and the issue opened in the browser, on a worker so the present
// is not held (vk_menu.cpp's menu_tick finishes it: menu/pipeline_list.h). Fenced from a layout-only build (MENU_LAYOUT_ONLY).
#include "menu_module.h"

#include "menu/pipeline_list.h"

#ifndef MENU_LAYOUT_ONLY
#include "menu/framegen.h"   // (tx_framegen: the template's frame generation)
#include "menu/output.h"     // (the template's output)
#include "menu/upscaler.h"   // (the template's upscaler and the 3D's size)
#include "pipeline_share.h"
#include "vk_draw.h"         // (state_list_snapshot: the pipelines built)
#include "vk_internal.h"
#include "vk_menu_i18n.h"    // (i18n::tr)
#include "vk_paths.h"        // (game_module_path: the game folder, its mods)
#ifdef _WIN32
#include <windows.h>
#else   // (the desktop's programs, run on the worker: as vk_menu.cpp had them before the module)
#include <fcntl.h>
#include <spawn.h>
#include <csignal>
#include <cerrno>
#include <pthread.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace mkwvk {

#ifndef MENU_LAYOUT_ONLY
#ifdef _WIN32
HWND app_window();   // (vk_main.cpp: the game's window)
#endif
namespace ps = pipeline_share;
std::string share_said;    // (the help box's line after a save: where the zip went and what to do, or why there is none)
std::string share_value;   // (the row's value after a save: Saved, or Not saved)
constexpr const char* kShareSaidLongest = "Saved longshot-pipelines-2026-12-31.zip (123456 pipelines) in Downloads. The report "
    "is in the zip. Open git.magicbean.dev/MagicBeanDev/Longshot/issues/14 and attach the zip to a comment, then paste the report and say "
    "where you played.";   // (measure_help: the help box's room)
std::string share_saved;   // (the first sentence: the zip's name, its pipelines and its folder)
std::atomic<bool> share_busy{false};   // (the worker is copying and opening: presses wait for it)
std::atomic<int> share_result{-1};     // (the worker's answer: 1 copied, 2 the issue opened; -1 none waiting)
/// (menu_tick, each frame) the worker's answer into the help box's line
/// How the help box names the zip's folder: share_downloads' words for it, in the menu's language, else the folder's own name
/// alone (no user name on screen). (The help box's lines are put together from pieces, each looked up as it goes in: ui::para
/// looks the whole line up, which no table has)
std::string share_folder_name(std::string dir, const std::string& where) {
    if (!where.empty()) return i18n::tr(where.c_str());
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    return dir.substr(dir.find_last_of("\\/") + 1);
}
/// The help box's first sentence after a save: the zip's name, its pipelines and its folder (share_folder_name)
std::string share_saved_line(const std::string& name, long long n, const std::string& folder) {
    return i18n::tr("Saved ") + name + " (" + std::to_string(n) + i18n::tr(" pipelines) in ") + folder + ". ";
}
/// The rest of it, once the worker has answered (share_result: 1 copied, 2 the issue opened)
std::string share_done_line(int r) {
    return std::string(i18n::tr((r & 1) ? "The report is copied. " : "The report is in the zip. ")) +
           i18n::tr((r & 2) ? "Issue 14 is open in your browser: attach" : "Open git.magicbean.dev/MagicBeanDev/Longshot/issues/14 and attach") +
           i18n::tr(" the zip to a comment, then paste the report and say where you played.");
}
/// The help box's line when the zip could not be written in that folder (share_folder_name)
std::string share_not_written_line(const std::string& folder) { return i18n::tr("The zip could not be written in ") + folder + "."; }
void share_finish() {
    const int r = share_result.exchange(-1);
    if (r < 0) return;
    share_said = share_saved + share_done_line(r);
    share_busy.store(false);
    menu_redraw();
}
std::string game_folder() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1);
}
bool is_dir(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
/// The mods the game loaded, by host/mods.c's rule: under the game folder's mods (or HOS_MODS), a folder holding romfs or exefs,
/// up to four folders deep; names starting with '-' or '_' are off
void share_find_mods(const std::string& dir, const std::string& name, int depth, std::vector<std::string>& out) {
    if (depth > 0 && (is_dir(dir + "/romfs") || is_dir(dir + "/exefs"))) {
        out.push_back(name);
        return;
    }
    if (depth >= 4) return;
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || fd.cFileName[0] == '-' ||
            fd.cFileName[0] == '_')
            continue;
        share_find_mods(dir + "/" + fd.cFileName, name.empty() ? fd.cFileName : name + "/" + fd.cFileName, depth + 1, out);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}
std::string share_os() {
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW v{};
    v.dwOSVersionInfoSize = sizeof v;
    const auto get = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (!get || get(&v) != 0) return "Windows";
    // (Windows 11 is still 10.0, from build 22000; a server says so instead: wProductType VER_NT_WORKSTATION is 1)
    const char* name = v.wProductType != 1 ? "Windows Server" : v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "Windows 11"
                       : v.dwMajorVersion == 10 ? "Windows 10" : "Windows";
    char b[80];
    snprintf(b, sizeof b, "%s (%lu.%lu.%lu)", name, v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return b;
#else
    std::string pretty;
    for (const char* path : {"/etc/os-release", "/usr/lib/os-release"}) {
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                pretty = line + 12;
                while (!pretty.empty() && (pretty.back() == '\n' || pretty.back() == '"' || pretty.back() == '\r')) pretty.pop_back();
                if (!pretty.empty() && pretty[0] == '"') pretty.erase(0, 1);
            }
        fclose(f);
        break;
    }
    utsname u{};
    uname(&u);
    return std::string(u.sysname) + " (" + (pretty.empty() ? "" : pretty + ", ") + "kernel " + u.release + ")";
#endif
}
#ifdef _WIN32
/// A function of shell32 or ole32, loaded at its first use: longshot_vk.dll imports only kernel32, user32, gdi32, advapi32 and the
/// UCRT (the DLSS workflow's import check), and these two are needed only here
FARPROC share_proc(const wchar_t* dll, const char* name) {
    HMODULE m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? GetProcAddress(m, name) : nullptr;
}
#endif
/// The Downloads folder (where a browser's file picker opens), else the home folder, else the game folder. *words: how the menu
/// names it: the folder's own name for Downloads, never the home folder's (that is the user's name)
std::string share_downloads(std::string* words) {
    *words = "";
#ifdef _WIN32
    using KnownFolderFn = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, wchar_t**);
    using FreeFn = void(WINAPI*)(void*);
    static const auto known = (KnownFolderFn)(void*)share_proc(L"shell32.dll", "SHGetKnownFolderPath");
    static const auto free_mem = (FreeFn)(void*)share_proc(L"ole32.dll", "CoTaskMemFree");
    wchar_t* w = nullptr;
    std::string out;
    static const GUID downloads = {0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}};   // (FOLDERID_Downloads)
    if (known && SUCCEEDED(known(downloads, 0, nullptr, &w)) && w) {
        char b[MAX_PATH * 4];
        if (WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr) > 0) out = b;
    }
    if (w && free_mem) free_mem(w);
    if (!out.empty() && is_dir(out)) return out;
    if (const char* home = getenv("USERPROFILE"); home && is_dir(home)) return *words = "your home folder", home;
#else
    const char* home = getenv("HOME");
    if (home && *home) {
        const char* conf = getenv("XDG_CONFIG_HOME");
        const std::string path = (conf && *conf ? std::string(conf) : std::string(home) + "/.config") + "/user-dirs.dirs";
        std::string text;
        if (FILE* f = fopen(path.c_str(), "r")) {
            char b[4096];
            text.assign(b, fread(b, 1, sizeof b, f));
            fclose(f);
        }
        const std::string d = ps::xdg_download_dir(text, home);
        if (!d.empty() && is_dir(d)) return d;
        if (is_dir(std::string(home) + "/Downloads")) return std::string(home) + "/Downloads";
        if (is_dir(home)) return *words = "your home folder", home;
    }
#endif
    *words = "the game folder";
    return game_folder();
}
#ifndef _WIN32
/// A program of the desktop's (xdg-open, wl-copy, ...), not waited for: false when it cannot start. input: its standard input
bool share_spawn(std::vector<std::string> argv, const std::string* input = nullptr) {
    int fds[2] = {-1, -1};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (input) {
        if (pipe2(fds, O_CLOEXEC)) return posix_spawn_file_actions_destroy(&fa), false;   // (no other child holds it open)
        posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
        posix_spawn_file_actions_addclose(&fa, fds[1]);
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);   // (a browser or file manager keeps none of the game's files, nor a clipboard pipe)
    std::vector<char*> args;
    for (std::string& a : argv) args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    const int err = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (input) close(fds[0]);
    if (err) {
        if (input) close(fds[1]);
        return false;
    }
    if (!input) {   // (xdg-open: reaped whenever it ends)
        std::thread([pid] {
            int status = 0;
            waitpid(pid, &status, 0);
        }).detach();
        return true;
    }
    // (the clipboard's programs: given the text, and their exit waited for (they fork to keep the clipboard, the parent ends at once),
    // so one that fails (wl-copy with a stale WAYLAND_DISPLAY) counts as failed and the next is tried. On the share worker. SIGPIPE
    // is blocked on this thread while it writes: a program that exits before reading would otherwise end the game; the write
    // then fails with EPIPE, and the signal it left pending is taken before the mask is put back)
    sigset_t pipe_set, old;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    bool wrote = true;
    for (size_t at = 0; at < input->size();) {
        const ssize_t n = write(fds[1], input->data() + at, input->size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            wrote = false;
            break;
        }
        at += (size_t)n;
    }
    close(fds[1]);
    const timespec now{0, 0};
    while (sigtimedwait(&pipe_set, nullptr, &now) == SIGPIPE) {}
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return wrote && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif
bool share_clipboard(const std::string& text) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    bool open = false;   // (another program can hold the clipboard a moment: a few tries)
    for (int i = 0; n > 0 && !open && i < 5; i++) open = OpenClipboard(app_window()) || (Sleep(20), false);
    if (!open) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t))) {
        if (auto* w = (wchar_t*)GlobalLock(g)) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w, n);
            GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
        }
        if (!ok) GlobalFree(g);
    }
    CloseClipboard();
    return ok;
#else
    const char* wl = getenv("WAYLAND_DISPLAY");
    const char* x = getenv("DISPLAY");
    if (wl && *wl && share_spawn({"wl-copy"}, &text)) return true;
    return x && *x && (share_spawn({"xclip", "-selection", "clipboard"}, &text) || share_spawn({"xsel", "-ib"}, &text));
#endif
}
bool share_open(const std::string& what, bool select = false) {
#ifdef _WIN32
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, what.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, what.c_str(), -1, w.data(), (int)w.size());
    w.resize(wcslen(w.c_str()));
    using ShellExecuteFn = HINSTANCE(WINAPI*)(HWND, const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, INT);
    static const auto shell_execute = (ShellExecuteFn)(void*)share_proc(L"shell32.dll", "ShellExecuteW");
    if (!shell_execute) return false;
    if (select)   // (the folder, the zip chosen in it)
    {
        wchar_t windir[MAX_PATH];   // (%WINDIR%\explorer.exe, not whichever explorer.exe the search path finds first)
        const DWORD n = GetEnvironmentVariableW(L"WINDIR", windir, MAX_PATH);
        const std::wstring explorer = n && n < MAX_PATH ? std::wstring(windir) + L"\\explorer.exe" : L"explorer.exe";
        return (INT_PTR)shell_execute(nullptr, L"open", explorer.c_str(), (L"/select,\"" + w + L"\"").c_str(), nullptr, SW_SHOWNORMAL) > 32;
    }
    return (INT_PTR)shell_execute(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
#else
    (void)select;
    return share_spawn({"xdg-open", what});
#endif
}
/// The zip at path, through <path>.new made fresh (never one that was there, nor a link's target) and moved over what was there
bool share_write(const std::string& path, const std::string& data) {
    const std::string part = path + ".new";
#ifdef _WIN32
    auto wide = [](const std::string& u) {
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, w.data(), (int)w.size());
        w.resize(wcslen(w.c_str()));
        return w;
    };
    const std::wstring wpart = wide(part), wpath = wide(path);
    DeleteFileW(wpart.c_str());   // (a run that stopped halfway)
    HANDLE h = CreateFileW(wpart.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD done = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &done, nullptr) && done == data.size();
    ok = CloseHandle(h) && ok;
    ok = ok && MoveFileExW(wpart.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    unlink(part.c_str());   // (a run that stopped halfway; a link is removed, not followed)
    const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t at = 0; ok && at < data.size();) {
        const ssize_t n = write(fd, data.data() + at, data.size() - at);
        ok = n > 0, at += ok ? (size_t)n : 0;
    }
    ok = close(fd) == 0 && ok;
    ok = ok && rename(part.c_str(), path.c_str()) == 0;
#endif
    if (!ok) {
#ifdef _WIN32
        DeleteFileW(wpart.c_str());
#else
        unlink(part.c_str());
#endif
    }
    return ok;
}
/// The template's values from this run: the version, the OS, the GPU and its driver, the output, the upscaler and the 3D's size,
/// frame generation and HDR as the menu has them now, and the mods
std::vector<std::string> share_values(long long pipelines, bool v2, long long computes) {
    std::vector<std::string> v(ps::kFieldCount);
    v[0] = menu_version();   // (nx2/build.py gives the version to gpu/vk/vk_version.cpp alone)
    v[1] = share_os() + "; " + dev.props.deviceName + ", driver " + driver_text();
    const Out o = shown_output();
    v[2] = ps::output_text(o.w, o.h, opts.handheld != 0, output_choice(opts.output_res) == 0, aspect_follows_host(opts.ultrawide));
    v[3] = std::string(tx_upscaler(opts.dlss)) + ", " + tx_resolution(opts.resolution);
    v[4] = std::string(tx_framegen(std::clamp(opts.framegen, 0, 8))) + " / " + tx_hdr(opts.hdr);
    std::vector<std::string> mods;
    const char* env = getenv("HOS_MODS");
    share_find_mods(env && *env ? std::string(env) : game_folder() + "mods", "", 0, mods);
    std::sort(mods.begin(), mods.end(), [](const std::string& a, const std::string& b) {   // (case-insensitive, as the builders sort)
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                            [](char x, char y) { return tolower((unsigned char)x) < tolower((unsigned char)y); });
    });
    v[5] = ps::mods_text(mods);
    v[7] = ps::pipelines_text(pipelines, v2, computes);
    return v;
}
/// Save my pipeline list: the zip, then the clipboard, the folder and the issue (share_said says what happened)
void share_pipeline_list() {
    if (share_busy.load()) return;   // (the last press is still opening things)
    bool present = false;
    const std::string list = state_list_snapshot(&present);
    std::string why;
    const long long n = ps::check_list(list, present, &why);
    if (n < 0) {
        share_said = why, share_value = "Not saved";
        logf("menu: Save my pipeline list: %s", why.c_str());
        return;
    }
    const std::string lines = ps::template_lines(share_values(n, !list.compare(0, 8, "HKSTATE2"), ps::compute_count(list)));
    const time_t now = time(nullptr);
    tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    const std::string zip = ps::share_zip(list, lines, (uint16_t)((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday),
                                          (uint16_t)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2));
    std::string where;
    const std::string dir = share_downloads(&where), name = ps::zip_name(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
#ifdef _WIN32
    const char* sep = "\\";   // (Explorer's /select takes back slashes only)
#else
    const char* sep = "/";
#endif
    const std::string path = dir + (dir.back() == '/' || dir.back() == '\\' ? "" : sep) + name;
    const bool written = share_write(path, zip);
    const std::string folder = share_folder_name(dir, where);
    if (!written) {
        share_said = share_not_written_line(folder), share_value = "Not saved";
        logf("menu: Save my pipeline list: %s could not be written", name.c_str());
        return;
    }
    share_value = "Saved";
    share_saved = share_saved_line(name, n, folder);
    logf("menu: Save my pipeline list: %s, %lld pipelines, in %s", name.c_str(), n,
         (where.empty() ? folder : where).c_str());   // (the log in English: where's own words, not the help box's)
    if (env_on("HOS_HEADLESS")) {   // (a run without a window: nothing to show the browser or the folder on)
        share_result.store(0);
        share_finish();
        return;
    }
    // (the clipboard, the folder and the browser on a worker of their own: ShellExecuteW and xdg-open can take hundreds of ms, and
    // this is the present thread. Presses are ignored until it is done: share_busy)
    share_busy.store(true);
    share_said = share_saved + i18n::tr("Opening the issue...");
    try {
        std::thread([lines, path, dir] {
#ifdef _WIN32
            using CoInitFn = HRESULT(WINAPI*)(void*, DWORD);
            using CoUninitFn = void(WINAPI*)();
            static const auto co_init = (CoInitFn)(void*)share_proc(L"ole32.dll", "CoInitializeEx");
            static const auto co_uninit = (CoUninitFn)(void*)share_proc(L"ole32.dll", "CoUninitialize");
            // (ShellExecute wants COM on its thread: apartment-threaded, no OLE1 DDE)
            const bool com = co_init && co_uninit && SUCCEEDED(co_init(nullptr, 0x2 /*COINIT_APARTMENTTHREADED*/ | 0x4 /*COINIT_DISABLE_OLE1DDE*/));
#endif
            const bool copied = share_clipboard(lines);
#ifdef _WIN32
            share_open(path, true);
#else
            (void)path;
            share_open(dir);
#endif
            const bool opened = share_open(ps::kIssueUrl);
#ifdef _WIN32
            if (com) co_uninit();
#endif
            share_result.store((copied ? 1 : 0) | (opened ? 2 : 0));
        }).detach();
    } catch (const std::system_error&) {   // (no thread to be had: the zip is saved; say so, and the next press works)
        share_result.store(0);
        share_finish();
    }
}

/// A row's new value v, as the arrows set it (dir: 1 up, -1 down, for the rows that skip or wrap), saved unless !keep
/// (a slider's drag: saved once it is let go)
#endif

namespace {
#ifndef MENU_LAYOUT_ONLY
/// (its line: what the last save did (-1); every one it can say: the longest a save says (0), and the one now (1), when longer)
const char* hn_share(int, int variant) {
    if (variant < 0 || variant == 1) return share_said.empty() ? nullptr : share_said.c_str();
    return variant == 0 ? kShareSaidLongest : nullptr;
}
#endif
} // namespace

MENU_MODULE(pipeline_share) {
    menu::tab("debug")
        .section("developer")   // (the core's Developer heading: this row its first)
        .row({.key = "save_pipeline_list",
              .label = "Save my pipeline list",
              .help = "Helps remove first-load stutter for everyone: saves the pipelines your play built (shader hashes and draw state, no "
                      "game data) with a filled-in report as a zip in Downloads, copies the report and opens the \"Submit your "
                      "pipelines\" issue. Attach the zip there and paste the report. Nothing is uploaded for you.",
              .kind = menu::Kind::Action,
              .choices = {"Save", "Saved", "Not saved"},   // (its cell's texts: the Debug tab's cell as wide as the widest)
              .text = MENU_HOOK([](int) { return share_value.empty() ? std::string("Save") : share_value; }),
              .hint = MENU_HOOK(hn_share),
              .on_activate = MENU_HOOK(share_pipeline_list),
              .verb = "Save"}, menu::first());
}

} // namespace mkwvk
