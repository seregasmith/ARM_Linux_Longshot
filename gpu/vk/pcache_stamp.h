// (longshot) What the pipeline cache's files were made for (vk_draw.cpp's pcache_init), apart from Vulkan so that
// tests/test_pcache_stamp.py can run it.
//
// longshot_vk_pipelines.bin is checked against the GPU, the driver and the cache UUID only. A new translator (or a switch that changes
// what it makes) gives every program new SPIR-V: the program cache and the prepare record start over (program_cache_stamp), but
// the pipeline cache kept every older translator's pipelines, none of which can be hit again. It only grew: 2.4 GB after a
// few dozen test builds in one folder, read whole at every start and held by the driver, a working set of 18.7 GB in a field
// walk against 14.2 GB with a fresh file (the same build, the same walk). Now the stamp the files were made with is kept beside
// them in longshot_vk_pipelines.stamp, and files of another stamp are dropped before they are read.
//
// But a new translator gives most programs the same SPIR-V (99.6% of them between a stable and a nightly): dropping the file made
// every update a cold compile of every pipeline, hours on AMD, for pipelines the driver still had. So another translator's files
// are carried, within a bound. What is measured is the main file and the play files together, all of which a start hands the driver.
// The base (longshot_vk_pipelines.base) is that size when a precompile had finished for the translator being left (its stamp in
// longshot_pipelines.bin.built, with kBuiltWhole: that record is also left by a start that gave up on the build after two that
// crashed in it, and before a save): only then is it what one whole set of pipelines takes. From then on the cache is carried while it is
// at most kCarryGrowth of its base, and dropped past it (and the base with it). A cache no precompile has finished for is still filling
// in, so its growth says nothing of dead weight: it is carried without a size, at most kUnfinishedCarries times. A Vulkan pipeline
// cache cannot be pruned (no enumeration; vkMergePipelineCaches only adds), so the growth is bounded this way.
// The base file names the stamp it goes with and the one before: it is written first and the stamp file after, so a start cut short
// between them finds the carry half done and finishes it, and a base that could not be written leaves the stamp as it was, for the
// next start to try again. Under any other stamp it is not taken (a build from before it, which drops by the stamp alone, ran in
// between). It is a file of its own so that such a build reads the stamp file as ever.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace pcache_stamp {

inline const char* kStampFile = "longshot_vk_pipelines.stamp";
inline const char* kMainFile = "longshot_vk_pipelines.bin";
inline const char* kPlayPrefix = "longshot_vk_pipelines_play_";
inline const char* kBaseFile = "longshot_vk_pipelines.base";
inline const char* kBuiltFile = "longshot_pipelines.bin.built";   // (vk_draw.cpp precompile_built_write: its first word, the stamp)
/// That record's fourth word (vk_draw.cpp precompile_built_whole): every note was built and the cache's file holds them. A record
/// without it (an older build's, or one that says only "don't build these again") gives no size to judge the cache by
constexpr uint64_t kBuiltWhole = 0x454c4f4857544c42ull;

/// How far the cache may have grown past its base and still be carried to another translator (3/2: half as large again)
constexpr uint64_t kCarryGrowthNum = 3, kCarryGrowthDen = 2;
/// How many translators a cache no precompile has finished for is carried through, having no size to be judged by
constexpr uint64_t kUnfinishedCarries = 8;

/// The stamp kept in dir's stamp file; false: none (no file, or not 8 bytes)
inline bool read(const std::string& dir, uint64_t* stamp) {
    FILE* f = fopen((dir + kStampFile).c_str(), "rb");
    if (!f) return false;
    uint64_t v = 0;
    const bool ok = fread(&v, sizeof v, 1, f) == 1 && fgetc(f) == EOF;
    fclose(f);
    if (ok) *stamp = v;
    return ok;
}

/// stamp into dir's stamp file, through a .tmp renamed over it; whether it is in place
inline bool write(const std::string& dir, uint64_t stamp) {
    const std::string path = dir + kStampFile, tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(&stamp, sizeof stamp, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    return ok && !ec;
}

/// The base file. bytes: the cache's size when a precompile had finished for it (0: not known yet); stamp: the stamp this carry
/// moves to; previous: the one it moves from; carries: the carries made with no size known. Taken beside `stamp`, or beside
/// `previous` when `stamp` is this build's (the carry was cut short before the stamp file moved); otherwise none is known
struct Base { uint64_t bytes = 0, stamp = 0, previous = 0, carries = 0; };
inline Base read_base(const std::string& dir, uint64_t kept, uint64_t now) {
    FILE* f = fopen((dir + kBaseFile).c_str(), "rb");
    if (!f) return {};
    Base b;
    const bool ok = fread(&b, sizeof b, 1, f) == 1 && fgetc(f) == EOF;
    fclose(f);
    return ok && (b.stamp == kept || (b.stamp == now && b.previous == kept)) ? b : Base{};
}

/// b into dir's base file (nothing known, Base{}: the file removed); whether it is so
inline bool write_base(const std::string& dir, Base b) {
    const std::string path = dir + kBaseFile, tmp = path + ".tmp";
    std::error_code ec;
    if (!b.bytes && !b.carries) return std::filesystem::remove(path, ec) || !std::filesystem::exists(path, ec);
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(&b, sizeof b, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    if (ok) std::filesystem::rename(tmp, path, ec);
    const bool placed = ok && !ec;
    if (!placed) std::filesystem::remove(tmp, ec);
    return placed;
}

/// Whether a precompile got to its end for `kept` and its pipelines reached the file: the cache then holds every noted pipeline
inline bool finished_for(const std::string& dir, uint64_t kept) {
    FILE* f = fopen((dir + kBuiltFile).c_str(), "rb");
    if (!f) return false;
    uint64_t v[4] = {};
    const bool ok = fread(v, sizeof *v, 4, f) == 4;
    fclose(f);
    return ok && v[0] == kept && v[3] == kBuiltWhole;
}

/// What a start does with the cache's files. Keep: made for this stamp (or one `also` takes). Carry: another translator's, within
/// kCarryGrowth of its base (all_bytes: the main file and the play files), or with no base yet where a precompile finished for it
/// or it has been carried fewer than kUnfinishedCarries times. Drop: a folder from before the stamp file, no main file, or past those
enum class Verdict { Keep, Carry, Drop };
inline Verdict judge(bool have_stamp, uint64_t kept, uint64_t stamp, bool (*also)(uint64_t), uint64_t main_bytes, uint64_t all_bytes,
                     const Base& base, bool finished) {
    if (!have_stamp) return Verdict::Drop;
    if (kept == stamp || (also && also(kept))) return Verdict::Keep;
    if (!main_bytes) return Verdict::Drop;
    if (base.bytes) return all_bytes * kCarryGrowthDen <= base.bytes * kCarryGrowthNum ? Verdict::Carry : Verdict::Drop;
    return finished || base.carries < kUnfinishedCarries ? Verdict::Carry : Verdict::Drop;
}

/// Whether name is one of the pipeline cache's files: the main one, or a play file (longshot_vk_pipelines_play_<n>.bin, also .tmp)
inline bool is_cache_file(const std::string& name) {
    if (name == kMainFile || name == std::string(kMainFile) + ".tmp") return true;
    const std::string pre = kPlayPrefix;
    if (name.size() <= pre.size() || name.compare(0, pre.size(), pre)) return false;
    const std::string rest = name.substr(pre.size());
    const size_t dot = rest.find('.');
    if (dot == 0 || dot == std::string::npos || rest.substr(0, dot).find_first_not_of("0123456789") != std::string::npos) return false;
    const std::string ext = rest.substr(dot);
    return ext == ".bin" || ext == ".bin.tmp";
}

/// The sizes of the main file and the play files together (their .tmp left out: a start sweeps those)
inline uint64_t cache_bytes(const std::string& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name;
        try {
            name = it->path().filename().string();
        } catch (...) {
            continue;
        }
        if (!is_cache_file(name) || name.size() < 4 || name.compare(name.size() - 4, 4, ".bin")) continue;
        std::error_code size_ec;
        const uintmax_t size = it->file_size(size_ec);
        if (!size_ec) total += (uint64_t)size;
    }
    return total;
}

struct Dropped {
    size_t files = 0;
    uint64_t bytes = 0;
    bool carried = false;                  // (another translator's files, kept: Verdict::Carry)
    uint64_t main = 0, all = 0, base = 0;  // (the main file, it and the play files, and the base after the start: for the log)
};

/// The cache's files in dir judged (judge): kept as they are; carried to this stamp (the base first, then the stamp file); or
/// dropped, the stamp file then saying `stamp` and the base gone. A file that could not be removed (held open), or a main file
/// whose size could not be read, leaves the stamp as it was, for the next start to judge again. A stamp `also` takes is kept
/// too, as it is (vk_draw.cpp: the same translator under an earlier program cache format, pipelines_stamp_ok). What went, and
/// what was carried
inline Dropped check(const std::string& dir, uint64_t stamp, bool (*also)(uint64_t) = nullptr) {
    Dropped d;
    uint64_t kept = 0;
    const bool have = read(dir, &kept);
    if (have && (kept == stamp || (also && also(kept)))) return d;
    std::error_code size_ec;
    const auto type = std::filesystem::status(dir + kMainFile, size_ec).type();
    if (type == std::filesystem::file_type::none || type == std::filesystem::file_type::unknown) return d;
    if (type != std::filesystem::file_type::not_found) {
        const uintmax_t main = std::filesystem::file_size(dir + kMainFile, size_ec);
        if (size_ec) return d;
        d.main = (uint64_t)main;
    }
    d.all = cache_bytes(dir);
    const Base was = have ? read_base(dir, kept, stamp) : Base{};
    const bool finished = have && finished_for(dir, kept);
    const bool resumed = have && was.stamp == stamp && was.previous == kept;   // (this carry, cut short before the stamp moved)
    Verdict v = judge(have, kept, stamp, also, d.main, d.all, was, finished);
    if (resumed && d.main) v = Verdict::Carry;
    if (v == Verdict::Carry) {
        Base next = was;
        if (!resumed && !next.bytes) {
            if (finished) next.bytes = d.all;
            else next.carries++;
        }
        next.stamp = stamp, next.previous = kept;
        d.carried = true;
        d.base = next.bytes;
        if (write_base(dir, next)) write(dir, stamp);
        return d;
    }
    // (nothing here may throw: it runs at every start. The range-for's ++ throws on an error, and path::string() throws on
    // Windows for a name the code page cannot hold, so the iterator is stepped by increment(ec) and such a name, never one of
    // ours, is passed over)
    std::error_code ec;
    bool left = false;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name;
        try {
            name = it->path().filename().string();
        } catch (...) {
            continue;
        }
        if (!is_cache_file(name)) continue;
        std::error_code ec2;
        const uint64_t size = it->file_size(ec2);
        const bool sized = !ec2;
        if (std::filesystem::remove(it->path(), ec2)) d.files++, d.bytes += sized ? size : 0;
        else if (ec2) left = true;
    }
    if (left) return d;   // (held open: still this stamp's to drop at the next start, not to be taken for a new translator's)
    write(dir, stamp);
    write_base(dir, Base{});
    d.base = 0;
    return d;
}

}   // namespace pcache_stamp
