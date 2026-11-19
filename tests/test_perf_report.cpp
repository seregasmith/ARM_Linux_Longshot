// tests/test_perf_report.py: gpu/vk/perf_report.h, the performance summary: its interval, its buckets, what a hitch is and what it waited on,
// the pacing misses, the JSON the relay takes, the cells and bins a sample is sent as, and what a frame costs to add.
// Given a folder (argv[1]) it also writes what the Python side judges with the relay's own rules: every summary the size
// cases make (size-*.json: the sender re-encodes them) and the exact places and what was sent for them (pairs.tsv)
#include "../gpu/vk/perf_report.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace pr = mkwvk::perf_report;
namespace fs = mkwvk::fps_stats;

static fs::Frame frame(float ms, float game, float render, float gpu_wait, float pace) {
    fs::Frame f;
    f.ms = ms;
    f.part[fs::kGame] = game, f.part[fs::kRender] = render, f.part[fs::kGpuWait] = gpu_wait, f.part[fs::kPace] = pace;
    return f;
}

/// A sample at a place and view, and one number of what is sent for it (the JSON's, as the relay reads it)
static pr::PositionSample at(double x, double y, double z, double yaw = 0, double pitch = 0) {
    pr::PositionSample s;
    s.map = "Kokiri Forest (child)", s.x = x, s.y = y, s.z = z, s.yaw = yaw, s.pitch = pitch, s.frames = 1;
    return s;
}
static double sent(const pr::PositionSample& s, const char* key) {
    const std::string j = pr::sample_json(s), k = std::string("\"") + key + "\":";
    return std::strtod(j.c_str() + j.find(k) + k.size(), nullptr);
}
/// The relay's cell for a position (Longshot-api's telemetry/perf/totals.go addPosition: math.Round(v / 100) * 100, a half
/// away from zero), written here from its public rule and not from the header under test
static double nearest_100(double v) { return std::round(v / 100) * 100; }

// The size cases: the biggest summaries there can be (16 maps with long names and every count set, a sample each second of
// the longest period, the numbers play gives and the widest ones there are), names of 1, 2, 3 and 4 UTF-8 bytes a
// character, and the usual one map. The sender refuses one over its MAX_BODY, and a refused summary is deleted with its map
// totals; tests/test_perf_report.py re-encodes what is written here the way the sender does
static std::map<std::string, pr::MapTotals> every_count_set(const std::vector<std::string>& names) {
    std::map<std::string, pr::MapTotals> full;
    for (size_t k = 0; k < names.size(); k++) {
        auto& t = full[names[k]];
        t.fresh = k % 2, t.seconds = 52.517 + (double)k;
        for (auto& v : t.frame) v = 123456;
        for (auto& v : t.gpu) v = 123456;
        for (auto& v : t.hitch) v = 12345;
        for (auto& v : t.event) v = 1234;
        for (auto& v : t.part) v = 123456.789;
        t.frame_sum = t.gpu_sum = 987654.321, t.pacing_misses = t.left_out = 123456;
        t.movie_shown = t.movie_dropped = t.movie_late = t.movie_fallbacks = 12345, t.movie_decode_ms = 12345.678;
    }
    return full;
}

static void write_size_cases(const std::string& folder) {
    pr::Machine mc;
    mc.version = "v1-189-g0123abc (source 99)", mc.release = true, mc.os = "Windows 11", mc.kernel = "10.0.26100";
    mc.gpu = "NVIDIA GeForce RTX 4070", mc.driver = "621.10", mc.cpu = "13th Gen Intel(R) Core(TM) i7-13700F";
    mc.upscaler = "DLSS", mc.preset = "Balanced", mc.vram_gb = 12, mc.ram_gb = 32, mc.fps_cap = 60;
    const pr::Memory mem{5.25, 15.5, 7.75};
    struct Case {
        const char* file;
        int bytes;   // (a name's bytes a character: 0 the ASCII dungeon names, -1 the one usual map)
        int maps, samples;
        bool widest;
    };
    const Case cases[] = {{"ascii16", 0, 16, pr::kPeriodMax, false}, {"two-byte16", 2, 16, pr::kPeriodMax, false},
                          {"three-byte16", 3, 16, pr::kPeriodMax, false}, {"four-byte16", 4, 16, pr::kPeriodMax, false},
                          {"long-names16", 1, 16, pr::kPeriodMax, false}, {"widest-numbers16", 0, 16, pr::kPeriodMax, true},
                          {"one-map-five-minutes", -1, 1, pr::kPeriodDefault, false}, {"one-map-longest", -1, 1, pr::kPeriodMax, false}};
    for (const Case& cs : cases) {
        std::vector<std::string> names;
        for (int k = 0; k < cs.maps; k++) {
            char b[160];
            std::string n;
            if (cs.bytes == -1) n = "Kokiri Forest (child)";
            else if (cs.bytes == 0) snprintf(b, sizeof b, "Dungeon_%02d_LowerFloor_BossRoom_Main (adult) X", k), n = b;
            else {
                snprintf(b, sizeof b, "%02d", k), n = b;
                const char* one[] = {"", "M", "\xc3\xa9", "\xe4\xb8\xad", "\xf0\x9f\x98\x80"};   // (1 to 4 bytes a character)
                while (n.size() + strlen(one[cs.bytes]) <= 120) n += one[cs.bytes];
            }
            names.push_back(n);
        }
        const auto maps = every_count_set(names);
        std::vector<pr::PositionSample> samples;
        for (int i = 0; i < cs.samples; i++) {
            pr::PositionSample s = {names[(size_t)i % names.size()], -12345.678 + i * 7.123, 1234.567, 9876.543 - i, -3.14159 + i * 0.007,
                                    -1.23456 + i * 0.002, 61, 60, 3, 1016.667, 823.456};
            if (cs.widest)
                s.x = -999999.5 + i, s.y = s.z = -999999.5, s.frames = s.gpu_frames = s.hitches = 99999, s.frame_ms = s.gpu_ms = 99999999.999;
            samples.push_back(s);
        }
        std::ofstream(folder + "/size-" + cs.file + ".json", std::ios::binary) << pr::summary_json(mc, false, 840.25, names[0], &mem, maps, samples);
    }
}

// The exact places and views a player has, and the JSON the game makes of each: the relay's rules judge the pairs (the
// Python side). The places are the floats the host reads (so the ties of the relay's rounding, the odd multiples of 50, are
// exact), the angles are from the range the host gives: yaw -pi to pi and pitch -pi/2 to pi/2, their edges included
static void write_pairs(const std::string& folder) {
    std::ofstream out(folder + "/pairs.tsv");
    const double pi = 3.14159265358979323846;
    uint32_t seed = 12345;
    const auto random = [&seed](double low, double high) {
        seed = seed * 1664525u + 1013904223u;
        return low + (high - low) * ((seed >> 8) / 16777216.0);
    };
    std::vector<pr::PositionSample> all;
    for (int k = -80; k <= 80; k++)   // (every multiple of 25: the cells' middles, their edges and their ties)
        all.push_back(at((float)(k * 25.0), (float)(-k * 25.0 + 12.5), (float)(k * 25.0 - 12.5)));
    for (int i = 0; i < 20000; i++)
        all.push_back(at((float)random(-3000, 3000), (float)random(-3000, 3000), (float)random(-3000, 3000), (float)random(-pi, pi),
                         (float)random(-pi / 2, pi / 2)));
    for (int k = -32; k <= 32; k++)   // (every edge of the yaw bins and the pitch bins, and the floats beside each)
        for (int j = -1; j <= 1; j++) {
            const double yaw = k * pi / 32 + j * 1e-6, pitch = k * pi / 64 + j * 1e-6;
            all.push_back(at(0, 0, 0, std::clamp(yaw, -pi, pi), std::clamp(pitch, -pi / 2, pi / 2)));
        }
    for (const double yaw : {-pi, pi, (double)std::nextafterf((float)pi, 0.0f), -(double)std::nextafterf((float)pi, 0.0f)})
        all.push_back(at(0, 0, 0, yaw, 0));
    for (const double pitch : {-pi / 2, pi / 2, (double)std::nextafterf((float)(pi / 2), 0.0f)})
        all.push_back(at(0, 0, 0, 0, pitch));
    char line[256];
    for (const auto& s : all) {
        snprintf(line, sizeof line, "%.17g\t%.17g\t%.17g\t%.17g\t%.17g\t", s.x, s.y, s.z, s.yaw, s.pitch);
        out << line << pr::sample_json(s) << "\n";
    }
}

int main(int argc, char** argv) {
    // (first: the Python side judges what is written even when a check below fails)
    if (argc > 1) write_size_cases(argv[1]), write_pairs(argv[1]);

    // the buckets: the relay's lists, an upper bound each, +Inf last
    CHECK("counts", pr::kFrameBucketCount == 13 && pr::kGpuBucketCount == 11 && pr::kFineBucketCount == 19);
    CHECK("a cut never splits a character", pr::json_text("ab\xc3\xa9", 3) == "\"ab\"" && pr::json_text("ab\xc3\xa9", 4) == "\"ab\xc3\xa9\"");
    CHECK("a 60 fps frame", pr::bucket_of(16.6, pr::kFrameBuckets) == 4);
    CHECK("on the bound", pr::bucket_of(16.7, pr::kFrameBuckets) == 4);
    CHECK("past every bound", pr::bucket_of(5000, pr::kFrameBuckets) == pr::kFrameBucketCount - 1);

    // the interval between summaries: RX_VK_PERF_INTERVAL, 60 to 840 s (the relay's 900 s, a minute left for the last frame)
    CHECK("unset: 5 minutes", pr::summary_period(nullptr) == 300 && pr::summary_period("") == 300);
    CHECK("10 minutes", pr::summary_period("600") == 600);
    CHECK("at least a minute", pr::summary_period("10") == 60 && pr::summary_period("-5") == 60);
    CHECK("at most 14 minutes", pr::summary_period("900") == 840 && pr::summary_period("99999999999") == 840);
    CHECK("not a number: the default", pr::summary_period("soon") == 300 && pr::summary_period("600s") == 300);

    // a hitch: over twice the target and at least 25 ms
    CHECK("60 fps, 40 ms", pr::is_hitch(40, 16.7));
    CHECK("60 fps, 30 ms", !pr::is_hitch(30, 16.7));
    CHECK("240 fps, 12 ms is no stutter", !pr::is_hitch(12, 4.17));
    CHECK("30 fps, 70 ms", pr::is_hitch(70, 33.3));
    {   // (a 60 cap the GPU holds at 20 fps: its 50 ms frames are slow, not hitches; a 120 ms one among them is)
        pr::MapTotals m;
        for (int i = 0; i < 50; i++) pr::add_frame(m, frame(50, 10, 35, 3, 2), pr::Waits{}, 16.7, 50, 50);
        int64_t hitches = 0;
        for (int64_t h : m.hitch) hitches += h;
        CHECK("held at 20 fps under a 60 cap: no hitch", hitches == 0);
        pr::add_frame(m, frame(120, 10, 100, 3, 2), pr::Waits{}, 16.7, 50, 50);
        for (int64_t h : m.hitch) hitches += h;
        CHECK("a 120 ms frame among them: one", hitches == 1);
    }

    // what it waited on: the biggest of the renderer's waits and the parts, "other" under a quarter of the frame
    {
        pr::Waits w;
        w.pipeline = 30;
        CHECK("pipelines", pr::cause_of(frame(50, 10, 35, 2, 3), w) == pr::kPipelineWait);
        CHECK("the GPU", pr::cause_of(frame(50, 5, 5, 38, 2), pr::Waits{}) == pr::kGpuBound);
        CHECK("the game", pr::cause_of(frame(50, 40, 5, 3, 2), pr::Waits{}) == pr::kGameCpu);
        w = {};
        w.upload = 20;
        CHECK("uploads", pr::cause_of(frame(60, 10, 25, 10, 5), w) == pr::kTextureUpload);
        CHECK("nothing stands out", pr::cause_of(frame(100, 20, 20, 20, 20), pr::Waits{}) == pr::kOther);
        CHECK("names", !std::strcmp(pr::cause_name(pr::kGpuBound), "gpu_bound") && !std::strcmp(pr::cause_name(pr::kPacing), "pace") &&
                           !std::strcmp(pr::event_name(pr::kDeviceLost), "device_lost"));
    }

    // pacing: a quarter of the target from the frame before; never the first frame
    CHECK("even", !pr::is_pacing_miss(16.9, 16.5, 16.7));
    CHECK("uneven", pr::is_pacing_miss(22, 16.5, 16.7));
    CHECK("first", !pr::is_pacing_miss(22, 0, 16.7));

    // a map's totals, and the summary as the relay takes it
    {
        std::map<std::string, pr::MapTotals> maps;
        auto& m = maps["Kokiri Forest (child)"];
        double last = 0;
        for (int i = 0; i < 100; i++) pr::add_frame(m, frame(16.6f, 10, 4, 2, 0.6f), pr::Waits{}, 16.7, last), last = 16.6;
        pr::Waits w;
        w.pipeline = 40;
        CHECK("add_frame says a hitch", pr::add_frame(m, frame(60, 10, 45, 3, 2), w, 16.7, last));
        {
            pr::MapTotals other;
            CHECK("nor an ordinary frame", !pr::add_frame(other, frame(16.6f, 10, 4, 2, 0.6f), pr::Waits{}, 16.7, 16.6));
        }
        CHECK("frames", m.frame[4] == 100 && m.frame[9] == 1);
        CHECK("seconds", m.seconds > 1.71 && m.seconds < 1.73);
        CHECK("the hitch", m.hitch[pr::kPipelineWait] == 1 && m.pacing_misses == 0);
        CHECK("an empty map is left out", !maps["Hyrule Field (adult)"].any());
        pr::Machine mc;
        mc.version = "2.4.1 (source 1234abcd)", mc.os = "Arch Linux", mc.kernel = "6.12.3-arch1-1", mc.gpu = "AMD \"Radeon\" RX 7800 XT";
        mc.driver = "radv Mesa 25.2.4", mc.cpu = "AMD Ryzen 7 7800X3D", mc.upscaler = "FSR 3.1", mc.preset = "High";
        mc.vram_gb = 16, mc.ram_gb = 32, mc.fps_cap = 60;
        pr::Memory mem{5.25, 15.5, 7.75};
        std::vector<pr::PositionSample> samples(1);
        samples[0] = {"Kokiri Forest (child)", -123.5, 4, 98.25, -1.2, 0.25, 60, 59, 2, 1000, 800};
        const std::string j = pr::summary_json(mc, true, 312.5, "Kokiri Forest (child)", &mem, maps, samples);
        CHECK("version 2", j.rfind("{\"v\":2,", 0) == 0);
        {   // the fine histogram is a separate field, so the relay's coarse frame_ms stays byte-for-byte for v1
            const size_t a = j.find("\"fine_ms\":["), b = j.find("]", a);
            const std::string list = j.substr(a, b - a);
            CHECK("fine histogram", a != std::string::npos && std::count(list.begin(), list.end(), ',') == pr::kFineBucketCount - 1 &&
                                       j.find("\"frame_ms\":[") != std::string::npos);
        }
        CHECK("client identity before the game version", j.find("\"client\":\"Longshot\",\"version\":") != std::string::npos);
        CHECK("quotes escaped", j.find("\"AMD \\\"Radeon\\\" RX 7800 XT\"") != std::string::npos);
        CHECK("the map", j.find("\"Kokiri Forest (child)\":{\"new\":true,") != std::string::npos);
        CHECK("not the empty one", j.find("Hyrule Field") == std::string::npos);
        CHECK("buckets", j.find("\"frame_ms\":[0,0,0,0,100,0,0,0,0,1,0,0,0]") != std::string::npos);
        CHECK("hitches", j.find("\"hitches\":{\"pipeline_wait\":1,") != std::string::npos);
        CHECK("memory", j.find("\"memory\":{\"vram_used_gb\":5.25,\"vram_allowed_gb\":15.5,\"ram_used_gb\":7.75}") != std::string::npos);
        CHECK("numbers", j.find("\"session_s\":312.5,") != std::string::npos && j.find("\"fps_cap\":60,") != std::string::npos);
        // (a sample is sent as its cell, the nearest multiple of 100 as the relay rounds a place (-123.5 to -100, 4 to 0,
        // 98.25 to 100), and the middle of its 45-degree yaw and 30-degree pitch bin (-1.2 is in [-pi/2, -pi/4), 0.25 in
        // [0, pi/6)), never the exact place)
        CHECK("per-second position and view, as cells and bins", j.find("\"samples\":[{\"map\":\"Kokiri Forest (child)\",\"x\":-100,\"y\":0,\"z\":100,\"yaw\":-1.178,\"pitch\":0.262,\"frames\":60,\"frame_ms\":1000,\"gpu_frames\":59,\"gpu_ms\":800,\"hitches\":2}") != std::string::npos);
        CHECK("no exact place", j.find("-123.5") == std::string::npos && j.find("98.25") == std::string::npos);
        CHECK("closed", j.back() == '}' && std::count(j.begin(), j.end(), '{') == std::count(j.begin(), j.end(), '}'));

        // the cells: a place is sent as its nearest multiple of 100, which is how the relay's math.Round(v / 100) * 100 files an
        // exact place (a half goes away from zero): it files the sent value under the same cell, and rounds it onto itself
        CHECK("40 is in cell 0", sent(at(40, 0, 0), "x") == 0 && sent(at(-40, 0, 0), "x") == 0 && sent(at(0, 0, 0), "x") == 0);
        CHECK("49.99 is in cell 0, 50.01 in 100", sent(at(49.99, 0, 0), "x") == 0 && sent(at(50.01, 0, 0), "x") == 100);
        CHECK("a half goes away from zero: 50 is 100, -50 is -100", sent(at(50, 0, 0), "x") == 100 && sent(at(-50, 0, 0), "x") == -100);
        CHECK("150 is 200, -150 is -200, 149.99 is 100", sent(at(150, 0, 0), "x") == 200 && sent(at(-150, 0, 0), "x") == -200 &&
                                                              sent(at(149.99, 0, 0), "x") == 100);
        CHECK("every axis", sent(at(60, -140, 260), "x") == 100 && sent(at(60, -140, 260), "y") == -100 && sent(at(60, -140, 260), "z") == 300);
        CHECK("never a negative zero", pr::sample_json(at(-0.4, -0.0, -49)).find("-0") == std::string::npos);
        CHECK("a cell sent is its own cell", sent(at(sent(at(-123.5, 0, 0), "x"), 0, 0), "x") == -100 && sent(at(0, 0, 0), "x") == 0);
        for (double v = -2000; v <= 2000; v += 12.5)   // (every multiple of 12.5: the cells' middles, their ties and their edges among them)
            CHECK("every cell sent is a multiple of 100 and the nearest", sent(at(v, 0, 0), "x") == nearest_100(v));

        // the angles: the middle of the bin, and never out of the relay's range (|yaw| <= pi, |pitch| <= pi/2: one sample past
        // it and the relay refuses the whole summary, which the sender then deletes). Yaw pi is the relay's first bin again
        // (it wraps the angle round), and the middle of the last pitch bin (above 60 degrees) takes straight up too
        const double pi = 3.14159265358979323846;
        CHECK("yaw 0 is in the bin from 0", std::fabs(sent(at(0, 0, 0, 0.1), "yaw") - 0.393) < 1e-9 && std::fabs(sent(at(0, 0, 0, -0.1), "yaw") + 0.393) < 1e-9);
        CHECK("yaw edges", std::fabs(sent(at(0, 0, 0, pi / 4 - 1e-4), "yaw") - 0.393) < 1e-9 && std::fabs(sent(at(0, 0, 0, pi / 4), "yaw") - 1.178) < 1e-9);
        CHECK("yaw pi is the first bin: -7/8 pi", std::fabs(sent(at(0, 0, 0, pi), "yaw") + 2.749) < 1e-9 && std::fabs(sent(at(0, 0, 0, -pi), "yaw") + 2.749) < 1e-9);
        CHECK("yaw just under pi is the last bin: 7/8 pi", std::fabs(sent(at(0, 0, 0, pi - 1e-4), "yaw") - 2.749) < 1e-9);
        CHECK("pitch 0 is in the bin from 0", std::fabs(sent(at(0, 0, 0, 0, 0.1), "pitch") - 0.262) < 1e-9 && std::fabs(sent(at(0, 0, 0, 0, -0.1), "pitch") + 0.262) < 1e-9);
        CHECK("pitch up to straight up is the last bin", std::fabs(sent(at(0, 0, 0, 0, pi / 2), "pitch") - 1.309) < 1e-9 &&
                                                              std::fabs(sent(at(0, 0, 0, 0, 1.2), "pitch") - 1.309) < 1e-9);
        CHECK("pitch straight down is the first bin", std::fabs(sent(at(0, 0, 0, 0, -pi / 2), "pitch") + 1.309) < 1e-9);
        for (int i = 0; i <= 3600; i++) {
            const double yaw = -pi + i * (2 * pi / 3600), pitch = -pi / 2 + i * (pi / 3600);
            CHECK("every yaw sent is in the relay's range", std::fabs(sent(at(0, 0, 0, yaw, pitch), "yaw")) <= pi);
            CHECK("every pitch sent is in the relay's range", std::fabs(sent(at(0, 0, 0, yaw, pitch), "pitch")) <= pi / 2);
        }

        std::vector<pr::PositionSample> five_minutes(pr::kPeriodDefault, samples[0]);
        const std::string batch = pr::summary_json(mc, true, 312.5, "Kokiri Forest (child)", &mem, maps, five_minutes);
        const auto sample_count = [](const std::string& s) {
            size_t n = 0;
            for (size_t at = s.find("\"pitch\":"); at != std::string::npos; at = s.find("\"pitch\":", at + 1)) n++;
            return n;
        };
        CHECK("five minutes on one map: every sample", sample_count(batch) == (size_t)pr::kPeriodDefault);
        CHECK("five minutes on one map fits", pr::sent_size(batch) <= pr::kMaxBody);

        // the biggest summary there can be: 16 maps with long names and every count set, and a sample each second of the
        // longest period, with the numbers play gives (positions and times with decimals, large counts). The sender
        // refuses one over its MAX_BODY, and a refused summary is deleted, its map totals with it
        std::vector<std::string> names;
        for (int k = 0; k < pr::kMaxMaps; k++) {
            char name[96];
            snprintf(name, sizeof name, "Dungeon_%02d_LowerFloor_BossRoom_Main (adult) \xc3\xa9", k);   // ("é": escaped by the sender)
            names.push_back(name);
        }
        const auto full = every_count_set(names);
        std::vector<pr::PositionSample> longest;
        for (int i = 0; i < pr::kPeriodMax; i++)
            longest.push_back({names[i % names.size()], -12345.678 + i * 7.123, 1234.567, 9876.543 - i, -3.14159 + i * 0.007,
                               -1.23456 + i * 0.002, 61, 60, 3, 1016.667, 823.456});
        const std::string big = pr::summary_json(mc, false, 840.25, names[0], &mem, full, longest);
        CHECK("the biggest summary fits the sender's limit", pr::sent_size(big) <= pr::kMaxBody);
        bool every_map = true;
        for (const auto& n : names) every_map = every_map && big.find(pr::json_text(n) + ":{\"new\":") != std::string::npos;
        CHECK("every map's totals kept", every_map);
        const size_t kept = sample_count(big);
        CHECK("the samples thinned, not dropped", kept > 100 && kept < (size_t)pr::kPeriodMax);
        CHECK("an even spread, from the period's first second", big.find("\"samples\":[{\"map\":" + pr::json_text(names[0]) +
                                                                          ",\"x\":-12300,") != std::string::npos);
        CHECK("a spread over the whole period", big.find("\"x\":" + pr::json_signed_num(nearest_100(-12345.678 + 800 * 7.123)) + ",") !=
                                                     std::string::npos);
        CHECK("closed when thinned", big.substr(big.size() - 2) == "]}" &&
                                         std::count(big.begin(), big.end(), '{') == std::count(big.begin(), big.end(), '}'));
    }

    // the render thread's replay phases: a separate object, so the relay's strict parts_ms stays as it is
    CHECK("phases", pr::kPhases == 13 && std::string(pr::phase_name(pr::kSubmit)) == "submit" &&
                        std::string(pr::phase_name(pr::kDesc)) == "descriptors" &&
                        std::string(pr::phase_name(pr::kSlotWait)) == "slot_wait" &&
                        std::string(pr::phase_name(pr::kPhases)).empty());
    {
        pr::MapTotals m;
        m.seconds = 1;
        for (int p = 0; p < pr::kPhases; p++) m.phase[p] = p + 1;
        pr::Machine mc;
        mc.version = "v3.0.0";
        mc.present_mode = "mailbox", mc.vsync = "off", mc.framegen = "on";
        const std::string j = pr::summary_json(mc, false, 1, "Kokiri Forest (child)", nullptr, {{"Kokiri Forest (child)", m}});
        CHECK("phases_ms", j.find("\"phases_ms\":{\"submit\":1,\"present\":2,\"draw\":3,\"descriptors\":4,") != std::string::npos &&
                               j.find("\"slot_wait\":13}") != std::string::npos);
        CHECK("present labels", j.find("\"fps_cap\":0,\"present_mode\":\"mailbox\",\"vsync\":\"off\",\"framegen\":\"on\"") !=
                                    std::string::npos);
        CHECK("presentation clock", j.find("\"shown_ms\":{\"sum\":0,\"count\":0}") != std::string::npos);
        CHECK("renderer waits", j.find("\"waits_ms\":{\"shader_translate\":") != std::string::npos &&
                                    j.find("\"readback\":") != std::string::npos);
        CHECK("per-cause detail", j.find("\"causes\":{\"main_cpu_ms\":") != std::string::npos &&
                                      j.find("\"pipe_waits\":") != std::string::npos &&
                                      j.find("\"textures_gone\":") != std::string::npos);
    }

    // the kernel out of the Bug report's system line
    CHECK("linux", pr::kernel_of("Arch Linux (Linux 6.12.3-arch1-1), KDE (wayland)") == "6.12.3-arch1-1");
    CHECK("windows", pr::kernel_of("Windows 11 (build 26100)") == "10.0.26100");
    CHECK("unknown", pr::kernel_of("Linux").empty());

    // the relay's address, stamped by the launcher into the game folder (the crash helper's rule)
    {
        std::string url;
        const std::string stamp = "version=nightly-419-257c5491\nsource=5bd71d51\n"
                                  "crash_reports=https://data-services.magicbean.dev/telemetry/crash\n"
                                  "perf_reports=https://data-services.magicbean.dev/telemetry/perf\n";
        CHECK("perf address", pr::stamp_address(stamp, "perf_reports", url) &&
                                  url == "https://data-services.magicbean.dev/telemetry/perf");
        CHECK("crash address", pr::stamp_address(stamp, "crash_reports", url) &&
                                   url == "https://data-services.magicbean.dev/telemetry/crash");
        CHECK("no key", !pr::stamp_address(stamp, "build_reports", url));
        CHECK("http refused", !pr::stamp_address("perf_reports=http://local/telemetry/perf\n", "perf_reports", url));
        CHECK("space refused", !pr::stamp_address("perf_reports=https://x/y more\n", "perf_reports", url));
        CHECK("empty refused", !pr::stamp_address("perf_reports=\n", "perf_reports", url));
    }

    // the release flag the launcher used to fill in, now the game's own
    CHECK("stable", pr::is_release_version("v3.0.0") && pr::is_release_version("v3.0.0-rc.7"));
    CHECK("nightly", pr::is_release_version("nightly-419-257c5491"));
    CHECK("not a release", !pr::is_release_version("v3.0.0-rc.7-2") && !pr::is_release_version("nightly-419-257c549") &&
                               !pr::is_release_version(""));
    CHECK("release", pr::is_release("v3.0.0", "5bd71d51", "5bd71d51"));
    CHECK("source mismatch", !pr::is_release("v3.0.0", "5bd71d51", "other"));
    CHECK("no source", !pr::is_release("v3.0.0", "", ""));

    // curl's answer: only a definite refusal drops a summary (issue 1265)
    CHECK("status", pr::http_status("HTTP/1.1 200 OK\r\nContent-Length: 15\r\n\r\n") == 200);
    CHECK("last status", pr::http_status("HTTP/1.1 301 Moved\r\nHTTP/1.1 429 Too Many\r\n\r\n") == 429);
    CHECK("no status", pr::http_status("") == 0);
    CHECK("sent", pr::send_result(200) == pr::SendResult::Sent && pr::drop_after(pr::send_result(200)));
    CHECK("refused", pr::send_result(400) == pr::SendResult::Refused && pr::drop_after(pr::send_result(413)));
    CHECK("busy", pr::send_result(404) == pr::SendResult::Busy && pr::send_result(429) == pr::SendResult::Busy &&
                     pr::send_result(503) == pr::SendResult::Busy && pr::send_result(301) == pr::SendResult::Busy);
    CHECK("kept", !pr::drop_after(pr::send_result(0)) && !pr::drop_after(pr::send_result(404)));

    // what a frame costs (a player's): well under a microsecond, against a 16.7 ms frame
    {
        pr::MapTotals m;
        const auto f = frame(16.6f, 10, 4, 2, 0.6f);
        const auto t0 = std::chrono::steady_clock::now();
        const int n = 1000000;
        for (int i = 0; i < n; i++) pr::add_frame(m, f, pr::Waits{}, 16.7, 16.5);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("add_frame: %.1f ns a frame\n", ns);
        CHECK("cheap", ns < 1000 && m.frame[4] == n);
    }
    return 0;
}
