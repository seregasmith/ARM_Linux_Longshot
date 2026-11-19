// (longshot) The performance summary (perf_report.h): on only when the launcher's "Send performance statistics" tick wrote
// send=1 to the game folder's longshot_perf.ini. Then the FPS counter's Breakdown measures every frame whatever level the
// counter shows (vk_fps.h fps_breakdown_on), and each frame is added here, on the thread that presents, to its map's totals:
// a bucket, a few sums and the renderer's counters read since the frame before. No allocation, no file and no lookup a
// frame: the map is asked of the host about once a second, the memory once a summary, and a summary's JSON is made and
// written on a thread of its own every `period` seconds (RX_VK_PERF_INTERVAL) and at exit. Off, the only cost is fps_presented's look at perf_measure.
#include "perf_report.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <iterator>

#include "vk_bug_report.h"
#include "vk_draw.h"
#include "vk_fps.h"
#include "vk_framegen.h"
#include "vk_internal.h"
#include "vk_menu.h"
#include "vk_paths.h"
#include "vk_shader.h"
#include "vk_texture.h"
#include "upscaler_rows.h"

#include "perf_curl.h"

#ifdef _WIN32
#include <psapi.h>   // (PROCESS_MEMORY_COUNTERS_EX, K32GetProcessMemoryInfo: ram_used_gb)
#endif

namespace mkwvk {

std::atomic<bool> perf_measure{false};
double menu_target_ms();   // (vk_menu.cpp fps_target_ms: the frame time the game aims for, 0 uncapped)
int menu_fps_cap();        // (vk_menu.cpp fps_cap_now: the cap in effect, the same rule)
bool shaders_preparing();  // (vk_main.cpp: the start-up screen's shader preparation)

namespace {

namespace pr = perf_report;
static_assert(PROF_COUNT == pr::kPhases, "perf_report's phases must name the renderer's PROF_* in the same order");
namespace fs = fps_stats;

double period = pr::kPeriodDefault;   // (seconds of play between summaries: RX_VK_PERF_INTERVAL, perf_report_init)
constexpr double kMapEvery = 1;      // (seconds between asking the host where Link is)
constexpr int kMaxPending = 48;      // (summaries waiting in perf_pending at most, about four hours of play: a player who never
                                     // starts the launcher, or is offline, doesn't fill the disk; the newest go unwritten)
constexpr int kDrainPerSummary = 12; // (older summaries retried each period, the launcher sender's default: a full queue
                                     // drains after a relay recovers instead of dropping every new summary)

std::mutex mu;   // (the presenting thread's frames, against the exit's and a lost device's last summary)
std::map<std::string, pr::MapTotals> maps;
std::vector<pr::PositionSample> samples;
std::map<std::string, bool> seen_maps;   // (maps this session has sent: the relay counts a session on a map once)
pr::MapTotals* current = nullptr;
std::string current_map;
float latest_pose[5] = {};
bool latest_pose_valid = false;
pr::PositionSample collecting;
bool collecting_pose_valid = false;
pr::Machine machine;
std::string send_url;                 // perf_reports= from the game folder's longshot_version.txt (the launcher stamps it)
std::string stamp_source, game_source; // (the release flag, the launcher's Python sender used to fill in)
bool keep_sent = true;                // keep_sent=0 in longshot_perf.ini turns off the copies (the launcher's setting)
std::mutex send_mu;                   // one curl at a time: a start's retry can't meet a short interval's send
std::atomic<bool> shutting_down{false};  // (exit or lost device: never queue behind a curl, leave the last summary waiting)
std::filesystem::path folder;   // (the game folder: vk_paths.h, wide on Windows)
std::atomic<int64_t> events[pr::kEvents];
int64_t started_ns = 0, last_movie[5] = {};
double since_map = 0, since_summary = 0, avg_ms = 0, last_ms = 0;
bool first = true;
int seq = 0;
int64_t l_failed = 0;   // (the renderer's counts at the summary before: at the start, perf_report_init)
uint64_t l_swap = 0;

bool ticked(const std::filesystem::path& dir) {
    std::ifstream f(dir / "longshot_perf.ini");
    bool on = false;
    for (std::string line; std::getline(f, line);)
        if (!line.compare(0, 5, "send=")) on = atoi(line.c_str() + 5) == 1;
    return on;
}

double ram_used_gb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) return pm.PrivateUsage / 1073741824.0;
#else
    FILE* f = fopen("/proc/self/statm", "r");
    long pages = 0, resident = 0;
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
        return resident * (double)sysconf(_SC_PAGE_SIZE) / 1073741824.0;
    }
#endif
    return 0;
}

double vram_total_gb() {
    if (!dev.physical) return 0;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &mp);
    uint64_t most = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) most = std::max<uint64_t>(most, mp.memoryHeaps[i].size);
    return most / 1073741824.0;
}

const char* upscaler_name(int v) {
    // (upscaler_rows.h's order: GAME .. XESS_AA)
    static const char* const t[] = {"Game network", "DLSS", "DLAA", "FSR 3.1", "FSR 4", "FSR 4 AA", "Off", "XeSS", "XeSS AA"};
    static_assert(sizeof t / sizeof t[0] == upscaler_rows::kUpscalers, "a name for each upscaler");
    return v >= 0 && v < upscaler_rows::kUpscalers ? t[v] : "other";
}
const char* preset_name(int v) {
    static const char* const t[] = {"Custom", "Minimum", "Low", "Balanced", "High", "Ultra"};
    return v >= 0 && v < 6 ? t[v] : "Auto";
}

/// The machine, once: the GPU and its driver from Vulkan, the rest from the Bug report's system line (vk_bug_report.cpp
/// system_text: "<cpu> (<n> threads), <ram> GB, <os> (<kernel>)...")
void read_machine(const char* version) {
    const std::string full = version ? version : "";
    machine.version = full.substr(0, full.find(" ("));   // ("<version> (source <hash>)": the version)
    const size_t at = full.find("(source ");
    if (at != std::string::npos) {
        const size_t from = at + 8, end = full.find(')', from);
        game_source = end == std::string::npos ? full.substr(from) : full.substr(from, end - from);
    }
    machine.release = pr::is_release(machine.version, game_source, stamp_source);
    machine.gpu = dev.props.deviceName;
    machine.driver = driver_text();
    machine.vram_gb = vram_total_gb();
    const std::string sys = bug_report::system_text();
    const size_t threads = sys.find(" ("), gb = sys.find(" GB, ");
    machine.cpu = sys.substr(0, threads);
    if (gb != std::string::npos) {
        const size_t from = sys.rfind(", ", gb);
        machine.ram_gb = atof(sys.c_str() + (from == std::string::npos ? 0 : from + 2));
        const std::string os = sys.substr(gb + 5);
        machine.os = os.substr(0, os.find(" ("));
        machine.kernel = pr::kernel_of(os);
    }
}

/// The settings a summary says: read at each one (the player may change them in play)
void read_settings() {
    machine.upscaler = upscaler_name(opts.dlss);
    machine.preset = preset_name(opts.preset);
    machine.fps_cap = menu_fps_cap();
    // (version 2 labels: the report's finding that nothing said how a frame was presented)
    switch (dev.swap_mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: machine.present_mode = "immediate"; break;
        case VK_PRESENT_MODE_MAILBOX_KHR: machine.present_mode = "mailbox"; break;
        case VK_PRESENT_MODE_FIFO_KHR: machine.present_mode = "fifo"; break;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: machine.present_mode = "fifo_relaxed"; break;
        default: machine.present_mode = "other"; break;
    }
    static const char* const v[] = {"auto", "on", "off"};
    const int setting = menu_vsync();
    machine.vsync = setting >= 0 && setting < 3 ? v[setting] : "other";
    machine.framegen = framegen_on() ? "on" : "off";
}

/// The map from the host, and the movies' totals since the last ask, onto the map they played on. True when Link is on a map
/// the summary has no room for (pr::kMaxMaps): it stays on the one before, for a summary to make room
bool where_now() {
    char map[120] = "";
    int64_t movie[5] = {};
    int has_pose = 0;
    float pose[5] = {};
    const char* version = host->perf_info ? host->perf_info(map, sizeof map, movie, pose, &has_pose) : nullptr;
    latest_pose_valid = has_pose != 0;
    if (latest_pose_valid) memcpy(latest_pose, pose, sizeof latest_pose);
    if (machine.gpu.empty()) read_machine(version);
    // (before a map: the start-up screen while the shaders are prepared, then the intro, the title and the file select)
    const std::string name = map[0] ? map : shaders_preparing() ? pr::kPreparingShaders : pr::kTitleAndIntro;
    const bool full = current && name != current_map && !maps.count(name) && maps.size() >= (size_t)pr::kMaxMaps;
    if (!full && (!current || name != current_map)) {
        current_map = name;
        current = &maps[name];
        current->fresh = !seen_maps.count(name);
    }
    current->movie_shown += movie[0] - last_movie[0];
    current->movie_dropped += movie[1] - last_movie[1];
    current->movie_late += movie[2] - last_movie[2];
    current->movie_decode_ms += (movie[3] - last_movie[3]) / 1e6;
    current->movie_fallbacks += movie[4] - last_movie[4];
    memcpy(last_movie, movie, sizeof last_movie);
    return full;
}

void finish_sample() {
    if (collecting_pose_valid && collecting.frames > 0) samples.push_back(collecting);
    collecting = {};
    collecting_pose_valid = false;
}

void begin_sample() {
    collecting = {};
    collecting.map = current_map;
    collecting_pose_valid = latest_pose_valid;
    if (collecting_pose_valid) {
        collecting.x = latest_pose[0], collecting.y = latest_pose[1], collecting.z = latest_pose[2];
        collecting.yaw = latest_pose[3], collecting.pitch = latest_pose[4];
    }
}

/// Post one summary itself, as the game's side of the statistics (the launcher only configured the address and the tick):
/// keep a copy of what the relay took in perf_sent/, drop only what it refused, and leave a busy or unreachable answer for
/// the next summary. --disable first so a player's .curlrc can't add a redirect, a proxy or an insecure option. At exit
/// another curl may still hold send_mu: don't queue a second wait behind it, leave this summary for the next start
pr::SendResult send_summary(const std::string& name) {
    namespace fsys = std::filesystem;
    std::unique_lock<std::mutex> lk(send_mu, std::defer_lock);
    if (shutting_down.load(std::memory_order_relaxed)) {
        if (!lk.try_lock()) {
            logf("vk: performance summary %s left in perf_pending: a send is still in flight at exit", name.c_str());
            return pr::SendResult::None;
        }
    } else {
        lk.lock();
    }
    if (send_url.empty()) return pr::SendResult::None;
    const auto pending = folder / "perf_pending";
    const auto path = pending / (name + ".json"), headers = pending / (name + ".headers.txt"),
               answer = pending / (name + ".answer.txt");
    std::error_code ec;
    fsys::remove(headers, ec), fsys::remove(answer, ec);
    const int status = perf_curl::post(path, headers, answer, send_url);
    const pr::SendResult result = pr::send_result(status);
    fsys::remove(headers, ec), fsys::remove(answer, ec);
    if (!pr::drop_after(result)) return result;   // (busy or no answer: kept)
    if (result == pr::SendResult::Sent && keep_sent) {
        const auto sent = folder / "perf_sent";
        fsys::create_directory(sent, ec);
        fsys::rename(path, sent / (name + ".json"), ec);   // (the copy is the file, exactly as it was sent)
        if (!ec) return result;
    }
    fsys::remove(path, ec);
    return result;
}

/// Files a send or a write left behind when the game was killed or couldn't close cleanly: a .tmp summary cut short, and
/// curl's header/answer files. The .json summaries stay: send_pending() retries them. Called at start and at exit
void clean_pending() {
    namespace fsys = std::filesystem;
    std::unique_lock<std::mutex> lk(send_mu, std::try_to_lock);
    if (!lk.owns_lock()) return;   // (a curl still has its header/answer files: clean them at the next start)
    const auto dir = folder / "perf_pending";
    const auto ends = [](const std::string& s, const char* suffix) {
        const std::string t = suffix;
        return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
    };
    std::error_code ec;
    for (fsys::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (ends(name, ".tmp") || ends(name, ".headers.txt") || ends(name, ".answer.txt")) fsys::remove(it->path(), ec);
    }
}

/// Summaries left waiting (the relay was busy or unreachable), oldest first: at start all of them, then a bounded few
/// from each period so a full queue can drain after the relay recovers. Stops on the first busy/unreachable answer,
/// and skips a summary the caller has just sent itself. On the summary thread, never on a frame's
void send_pending(int limit = kMaxPending, const std::string& skip = "") {
    namespace fsys = std::filesystem;
    if (send_url.empty()) return;
    const auto dir = folder / "perf_pending";
    std::vector<std::string> names;
    std::error_code ec;
    for (fsys::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".json") names.push_back(it->path().stem().string());
    std::sort(names.begin(), names.end());
    int tried = 0;
    for (const auto& n : names) {
        if (n == skip) continue;
        const pr::SendResult r = send_summary(n);
        if (!pr::drop_after(r)) break;   // (busy or unreachable: the rest wait for a later pass, as the launcher's sender did)
        if (++tried >= limit) break;
    }
}

void write_summary(const std::filesystem::path& dir, const std::string& name, const std::string& json) {
    namespace fsys = std::filesystem;
    const fsys::path pending = dir / "perf_pending";
    std::error_code ec;
    {
        int waiting = 0;
        for (fsys::directory_iterator it(pending, ec), end; !ec && it != end; it.increment(ec))
            waiting += it->path().extension() == ".json";
        if (waiting >= kMaxPending) {
            send_pending(kDrainPerSummary);   // (make room for this one when the relay has recovered)
            waiting = 0;
            for (fsys::directory_iterator it(pending, ec), end; !ec && it != end; it.increment(ec))
                waiting += it->path().extension() == ".json";
            if (waiting >= kMaxPending) return;
        }
    }
    fsys::create_directory(pending, ec);
    const fsys::path tmp = pending / (name + ".tmp"), path = pending / (name + ".json");
    bool ok;
    {
        std::ofstream f(tmp, std::ios::binary);
        ok = f && f.write(json.data(), (std::streamsize)json.size()) && f.flush();
    }
    if (ok) fsys::rename(tmp, path, ec);
    if (!ok || ec) fsys::remove(tmp, ec);   // (whole or nothing: never send half a summary)
    else if (pr::drop_after(send_summary(name)))
        send_pending(kDrainPerSummary, name);   // (drain the older ones too, once what this period wrote is on its way)
}

/// The summary of what came in since the one before (under mu), written on a thread of its own unless `now` (the exit, a
/// lost device: the process ends next). The second being sampled carries on into the next summary, so the frames just
/// after this one are sampled too; the last summary takes it as it is
void summarize(bool now) {
    if (!current) return;
    if (now) finish_sample();
    if (!ticked(folder)) {   // (unticked in the launcher while the game ran: nothing more measured or written)
        if (host->perf_tracking) host->perf_tracking(0);
        perf_measure.store(false, std::memory_order_relaxed);
        maps.clear(), samples.clear(), current = nullptr;
        collecting = {}, collecting_pose_valid = false;
        logf("vk: performance statistics off (unticked in the launcher): nothing more is written");
        return;
    }
    where_now();
    read_settings();
    {   // (the renderer's events, onto the map they ended on: there are few)
        int64_t translated = 0, failed = 0;
        shader_stats(&translated, &failed);
        current->event[pr::kProgramFailed] += failed - l_failed, l_failed = failed;
        const uint64_t swap = dev.swapchain_generation;
        current->event[pr::kSwapchainRecreated] += (int64_t)(swap - l_swap);
        l_swap = swap;
        for (int e = 0; e < pr::kEvents; e++) current->event[e] += events[e].exchange(0);
    }
    {   // (the presentation clock: one sample per frame the driver confirmed as shown, drained into this summary)
        double shown_sum = 0;
        int64_t shown_n = 0;
        take_shown_samples(shown_sum, shown_n);
        current->shown_sum_ms += shown_sum, current->shown_count += shown_n;
    }
    bool any = false;
    for (const auto& [n, t] : maps) any = any || t.any();
    if (!any) return;   // (nothing since the last: the exit right after a lost device's summary)
    pr::Memory mem;
    uint64_t used = 0, allowed = 0;
    if (vram_usage_now(&used, &allowed)) mem.vram_used_gb = used / 1073741824.0, mem.vram_allowed_gb = allowed / 1073741824.0;
    mem.ram_used_gb = ram_used_gb();
    const double session_s = (fps_clock() - started_ns) / 1e9;
    std::string json = pr::summary_json(machine, first, session_s, current_map, &mem, maps, samples);
    char name[48];
    snprintf(name, sizeof name, "%lld-%d", (long long)std::time(nullptr), seq++);
    for (auto& [n, t] : maps)
        if (t.any()) seen_maps[n] = true;
    maps.clear(), samples.clear();
    current = &maps[current_map];
    current->fresh = !seen_maps.count(current_map);   // (a map entered just before this summary, with nothing yet: still new)
    first = false;
    if (now) return write_summary(folder, name, json);
    std::thread([dir = folder, n = std::string(name), j = std::move(json)] { write_summary(dir, n, j); }).detach();
}

// (an exit that runs atexit: Windows' ExitProcess, exit(). The window's close writes it first, mkwvk_close_asked; then
// this finds nothing new. Only tried: ExitProcess has already ended the other threads, and one may have held mu)
void at_exit() {
    std::unique_lock lk(mu, std::try_to_lock);
    if (lk) {
        shutting_down = true;
        summarize(true);
        clean_pending();   // (the last summary's curl files, and any .tmp an exit cut short)
    }
}

}   // namespace

bool perf_report_init() {
    folder = game_dir_path();
    if (!ticked(folder)) return false;
    {   // (what the launcher configured: the relay's address, the source fingerprint and the copies setting)
        std::ifstream stamp_file(folder / "longshot_version.txt");
        const std::string stamp((std::istreambuf_iterator<char>(stamp_file)), std::istreambuf_iterator<char>());
        if (!pr::stamp_address(stamp, "perf_reports", send_url)) send_url.clear();
        size_t at = 0;
        while (at < stamp.size()) {
            size_t end = stamp.find('\n', at);
            if (end == std::string::npos) end = stamp.size();
            std::string line = stamp.substr(at, end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 7, "source=") == 0) stamp_source = line.substr(7);
            at = end + 1;
        }
        std::ifstream choice(folder / "longshot_perf.ini");
        for (std::string line; std::getline(choice, line);)
            if (line == "keep_sent=0") keep_sent = false;
        if (send_url.empty()) logf("vk: performance statistics on, but no perf_reports= in longshot_version.txt: summaries stay in perf_pending");
    }
    // (send what a previous session left first, then clear the files a crash or an unstoppable close left behind; on one
    // thread so a curl that's still running never has its header/answer files removed under it, and never on a frame's)
    std::thread([] { send_pending(); clean_pending(); }).detach();
    period = pr::summary_period(getenv("RX_VK_PERF_INTERVAL"));
    if (host->perf_tracking) host->perf_tracking(1);
    started_ns = fps_clock();
    int64_t translated = 0;
    shader_stats(&translated, &l_failed);   // (the renderer's counts so far: only what comes after is counted)
    l_swap = dev.swapchain_generation;
    std::atexit(at_exit);
    logf("vk: performance statistics on (the launcher's Send performance statistics): a summary every %.0f s into perf_pending",
         period);
    perf_measure.store(true, std::memory_order_relaxed);
    return true;
}

void perf_event(int e) {
    if (e >= 0 && e < pr::kEvents && perf_measure.load(std::memory_order_relaxed)) events[e].fetch_add(1, std::memory_order_relaxed);
}

void perf_frame(const fs::Frame& f, double wait_ms, int64_t pipe_waits) {
    // (the renderer's counters since the frame before: what the slow frames waited on. wait_ms: draw_stats3's, read by
    // fps_presented, which calls this only while perf_measure holds)
    static int64_t l_prof[PROF_COUNT], l_readback;
    static double l_wait_ms, l_translate;
    static bool primed = false;   // (the first frame only takes the counters: they hold everything since the game started)
    const double translate = shader_translate_wait_ms();   // (only what a draw waited for: the jobs' run behind the game)
    const int64_t readback = texture_readback_wait_ns();
    // (the per-cause counters, all cumulative: their differences are this frame's, as the profiler's columns are)
    static int64_t l_late, l_tex, l_uploads, l_upload_bytes, l_gone, l_pipe_waits, l_main[3];
    const int64_t late = memory_late_wait_ns(), tex = texture_frame_ns(), gone = (int64_t)texture_destroy_generation();
    int64_t uploads = 0, astc = 0, upload_bytes = 0;
    texture_stats(&uploads, &astc, &upload_bytes);
    int64_t main[3] = {0, 0, 0};
    if (host->main_thread_times) host->main_thread_times(main);
    const auto delta = [](int64_t a, int64_t b) { return std::max<int64_t>(a - b, 0); };
    const int64_t d_late = delta(late, l_late), d_tex = delta(tex, l_tex), d_gone = delta(gone, l_gone),
                  d_uploads = delta(uploads, l_uploads), d_bytes = delta(upload_bytes, l_upload_bytes),
                  d_pipe = delta(pipe_waits, l_pipe_waits);
    const int64_t d_main[3] = {delta(main[0], l_main[0]), delta(main[1], l_main[1]), delta(main[2], l_main[2])};
    l_late = late, l_tex = tex, l_gone = gone, l_uploads = uploads, l_upload_bytes = upload_bytes, l_pipe_waits = pipe_waits;
    memcpy(l_main, main, sizeof l_main);
    const auto dp = [](int i) { return (prof_ns[i] - l_prof[i]) / 1e6; };
    double phase_ms[pr::kPhases] = {};
    for (int p = 0; p < pr::kPhases; p++) phase_ms[p] = dp(p);   // (this frame's, before l_prof moves up below)
    pr::Waits w;
    // (draw_stats3's waits are draws waiting for a program's translation; program_pick's own translation, in place, is
    // timed inside PROF_PROGRAM too: each counted once, as a translation)
    const double in_place = translate - l_translate;
    w.translate = wait_ms - l_wait_ms + in_place;
    w.pipeline = std::max(0.0, dp(PROF_PIPE) + dp(PROF_PROGRAM) - in_place);
    w.upload = dp(PROF_UPLOAD);
    w.memsync = dp(PROF_MEMSYNC);
    w.readback = (readback - l_readback) / 1e6;
    memcpy(l_prof, prof_ns, sizeof l_prof);
    l_wait_ms = wait_ms, l_translate = translate, l_readback = readback;
    if (!primed) w = pr::Waits{}, primed = true;

    std::lock_guard lk(mu);
    if (!current || since_map >= kMapEvery) {   // (each second: where Link is, and the next one-second sample)
        finish_sample();
        // (a 17th map in one period: that summary now, with the 16 it has, then the new map in the next)
        if (where_now()) summarize(false), since_summary = 0, where_now();
        since_map = 0;
        begin_sample();
    }
    // (the target: the cap's, else uncapped the recent average. A hitch is twice the longer of the target and the recent
    // average, before this frame: a frame twice the usual, never every frame of a game the GPU holds under its cap)
    const double usual = avg_ms ? avg_ms : f.ms;
    avg_ms = usual * 0.98 + f.ms * 0.02;
    const double cap_ms = menu_target_ms();
    const double target = cap_ms > 0 ? cap_ms : usual;
    // (the render thread's time in each phase, the same intervals as the waits above, summed for the summary)
    if (primed) {   // (one frame's intervals: phases, waits and per-cause detail together)
        for (int p = 0; p < pr::kPhases; p++) current->phase[p] += phase_ms[p];
        current->wait[pr::kWaitTranslate] += w.translate;
        current->wait[pr::kWaitPipeline] += w.pipeline;
        current->wait[pr::kWaitUpload] += w.upload;
        current->wait[pr::kWaitMemsync] += w.memsync;
        current->wait[pr::kWaitReadback] += w.readback;
        current->main_cpu_ms += d_main[2] / 1e6;
        current->main_blocked_ms += d_main[1] / 1e6;
        current->main_vcore_ms += d_main[0] / 1e6;
        current->late_wait_ms += d_late / 1e6;
        current->tex_frame_ms += d_tex / 1e6;
        current->upload_mb += d_bytes / 1048576.0;
        current->uploads += d_uploads;
        current->pipe_waits += d_pipe;
        current->textures_gone += d_gone;
    }
    const bool hitch = pr::add_frame(*current, f, w, target, last_ms, usual);
    if (collecting_pose_valid) {
        collecting.frames++;
        collecting.frame_ms += f.ms;
        if (f.gpu_ms > 0) collecting.gpu_frames++, collecting.gpu_ms += f.gpu_ms;
        collecting.hitches += hitch;
    }
    last_ms = f.ms;
    since_map += f.ms / 1000.0, since_summary += f.ms / 1000.0;
    if (since_summary >= period) summarize(false), since_summary = 0;
}

void perf_report_final(int event) {
    if (!perf_measure.load(std::memory_order_relaxed)) return;
    perf_event(event);
    std::lock_guard lk(mu);
    shutting_down = true;
    summarize(true);
    clean_pending();   // (as at_exit: leave perf_pending/ with only summaries still waiting to be sent)
    if (host->perf_tracking) host->perf_tracking(0);
}

}   // namespace mkwvk
