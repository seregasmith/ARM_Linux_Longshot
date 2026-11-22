// (longshot) The shader store: every program's SPIR-V kept once, by its content, for every game folder of the player.
//
// A game folder's longshot_programs.bin held each stage's SPIR-V itself: 7.5 GB a build, half of it the same SPIR-V again
// (programs sharing a vertex or a fragment stage), and the same again in every other build. Two builds a player kept, stable
// v3.0.0 and nightly-486, with different translator stamps: 47,592 of their 47,798 programs translated to byte-identical
// SPIR-V, 14.7 GB stored for 3.5 GB distinct (Longshot #1649, #1759). So the SPIR-V goes here, once, and a record says where.
//
// Where: %LOCALAPPDATA%\Longshot\shaders\ (Windows), $XDG_CACHE_HOME/longshot/shaders/ else ~/.cache/longshot/shaders/
// (Linux); LONGSHOT_SHADER_STORE names another folder, or 0 for none (each record keeps its SPIR-V, as before the store).
//
//   spirv.pack  a 16-byte head ('LSPK', version, the store's id), then blobs: [hash a u64][hash b u64][words u64][words]
//   spirv.idx   a 16-byte head ('LSPI', version, the same id), then 32 bytes a blob: [hash a][hash b][at i64][words u64]
//
// Only ever appended to. Several Longshots may write at once (a build preparing while another plays): an append takes the
// store's lock (a lock on spirv.lock, which the system lets go when a Longshot ends), first reads what the others added to the
// index since, and writes the blob, then its index entry. A blob is read by its place (the words' offset in spirv.pack) and
// the hash the record naming it keeps: only when its head has that hash and the words as read hash to it. So whatever lies
// at a place now (a blob cut short, zeros where a crash left the disk's blocks unwritten, another blob where a pack cut short
// by hand, or made again, had one) is never SPIR-V given to the driver in its stead: the program is translated again and its
// words written again (the bad place no longer given out). Index entries keep their 32-byte grid when a write of one was cut
// short, so one damaged write spoils no other blob.
//
// The id: a store made again (deleted, by the player or the launcher's Free up space; or one of its files damaged) has
// another, and a game folder's cache written for the old one starts over (vk_progcache.cpp disk_init) rather than name blobs
// that are gone. It is made as new files put in the old ones' place, never the old ones cut short: a Longshot reading them
// keeps them (Linux), and while one has them open they are not replaced (Windows: this run goes without the store). A store
// that cannot be read this run (a file held by another program) is not made again either.
#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spirv_pack {

constexpr uint32_t kPackMagic = 0x4B50534C;   // ("LSPK")
constexpr uint32_t kIdxMagic = 0x4950534C;    // ("LSPI")
constexpr uint32_t kVersion = 1;
constexpr size_t kHead = 16;
constexpr size_t kBlobHead = 24;
constexpr size_t kEntry = 32;
constexpr uint64_t kMostWords = 1ull << 24;   // (64 MB of SPIR-V: no stage comes near it; a larger count is a damaged entry)
constexpr int64_t kMostAt = 1ll << 48;         // (a place past 256 TB: a damaged entry)

struct Hash {
    uint64_t a = 0, b = 0;
    bool operator==(const Hash& o) const { return a == o.a && b == o.b; }
};
struct HashHash {
    size_t operator()(const Hash& h) const { return (size_t)(h.a ^ (h.b * 0x9E3779B97F4A7C15ull)); }
};

/// 128 bits of a stage's words: two independent 64-bit hashes (FNV-1a over the words, and a multiply-rotate mix with
/// murmur's finaliser). The words are the key: two stages with one hash are one blob, so the hash is wide
inline Hash hash_words(const uint32_t* w, size_t n) {
    uint64_t a = 0xCBF29CE484222325ull, b = 0x9E3779B97F4A7C15ull ^ ((uint64_t)n * 0xC2B2AE3D27D4EB4Full);
    for (size_t i = 0; i < n; i++) {
        a = (a ^ w[i]) * 0x100000001B3ull;
        b ^= (uint64_t)w[i] * 0xFF51AFD7ED558CCDull;
        b = ((b << 31) | (b >> 33)) * 0xC4CEB9FE1A85EC53ull;
    }
    b ^= b >> 33, b *= 0xFF51AFD7ED558CCDull, b ^= b >> 33, b *= 0xC4CEB9FE1A85EC53ull, b ^= b >> 33;
    return {a, b};
}

/// The store's folder: LONGSHOT_SHADER_STORE (0: none), else the player's local cache folder. "" for none
inline std::string default_dir() {
    if (const char* s = std::getenv("LONGSHOT_SHADER_STORE"); s && *s) return std::strcmp(s, "0") == 0 ? std::string() : std::string(s);
#ifdef _WIN32
    if (const char* l = std::getenv("LOCALAPPDATA"); l && *l) return std::string(l) + "\\Longshot\\shaders";
    return {};
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x == '/') return std::string(x) + "/longshot/shaders";
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/longshot/shaders";
    return {};
#endif
}

class Store {
public:
    Store() = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    ~Store() { close(); }

    /// Opens (or makes) the store in dir. false: none this run (the records keep their SPIR-V)
    bool open(const std::string& folder) {
        close();
        if (folder.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(folder), ec);
        dir = folder;
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        pack_path = dir + "spirv.pack", idx_path = dir + "spirv.idx";
        if (!lock_init()) return fail();
        bool good = false;
        {
            Held held(*this);
            good = held.ok && open_held();   // (not held a minute after another Longshot took it: none this run)
        }
        if (!good) return fail();
        return true;
    }
    void close() {
        for (FILE** f : {&pack_out, &pack_in, &idx_out, &idx_in})
            if (*f) std::fclose(*f), *f = nullptr;
        blobs.clear();
        {
            std::lock_guard lk(bad_mu);
            bad.clear();
        }
        store_id = 0;
        // (this thread's read handle let go: the files can then be deleted. Another thread's goes at its next read, or its end)
        if (Handle& h = thread_handle(); h.f && h.gen == my_gen) std::fclose(h.f), h.f = nullptr;
        my_gen = 0;
        lock_close();
    }
    bool on() const { return store_id != 0; }
    uint32_t id() const { return store_id; }
    const std::string& folder() const { return dir; }

    /// The place of these words in the store (the offset of the words in spirv.pack), put there unless they are already;
    /// -1 when they could not be written (a full disk): the caller keeps them in its record. h: hash_words(w, n), which the
    /// caller keeps with the place, for read
    int64_t put(const uint32_t* w, size_t n, const Hash& h) {
        if (!on() || !n || n >= kMostWords) return -1;
        Held held(*this);
        if (!held.ok) return -1;
        catch_up();
        // (already there: given out once its head is these words' and no read found the words other than their hash)
        if (const auto it = blobs.find(h); it != blobs.end() && it->second.words == n && !is_bad(it->second.at) &&
                                           head_is(it->second.at, h, n))
            return it->second.at;
        if (std::fseek(pack_out, 0, SEEK_END) != 0) return -1;
        const int64_t end = tell(pack_out);
        if (end < (int64_t)kHead) return -1;
        const uint64_t head[3] = {h.a, h.b, (uint64_t)n};
        bool ok = std::fwrite(head, 8, 3, pack_out) == 3 && std::fwrite(w, 4, n, pack_out) == n && std::fflush(pack_out) == 0;
        if (!ok) return -1;
        const int64_t at = end + (int64_t)kBlobHead;
        // (the entry on the index's 32-byte grid: a write of one cut short, by a full disk, is made a whole entry of zeros
        // first, which nothing reads as a blob, rather than shift every entry after it)
        if (std::fseek(idx_out, 0, SEEK_END) != 0) return -1;
        const int64_t idx_end = tell(idx_out);
        if (idx_end < (int64_t)kHead) return -1;
        if (const int64_t off = (idx_end - (int64_t)kHead) % (int64_t)kEntry; off && !zeros(idx_out, (int64_t)kEntry - off)) return -1;
        const uint64_t e[4] = {h.a, h.b, (uint64_t)at, (uint64_t)n};
        ok = std::fwrite(e, 8, 4, idx_out) == 4 && std::fflush(idx_out) == 0;
        if (!ok) return -1;   // (the blob is there, named by nothing: written again by the next put of these words)
        // (catch_up read every whole entry before this one: the index is read up to its end now)
        idx_read = tell(idx_out);
        blobs[h] = {at, n};
        written_bytes += n * 4;
        return at;
    }

    int64_t put(const uint32_t* w, size_t n) { return put(w, n, hash_words(w, n)); }

    /// The words at at (put's answer), n of them, into out: false when the store does not have them there, whole, with the
    /// hash want (put's). Whatever else is there now is not read; a blob there whose words are not its head's hash is not
    /// given out again (the next put of those words writes them again)
    bool read(int64_t at, uint64_t n, const Hash& want, uint32_t* out) const {
        if (!on() || at < (int64_t)(kHead + kBlobHead) || at >= kMostAt || !n || n >= kMostWords) return false;
        Handle& h = thread_handle();
        if (h.gen != my_gen || !h.f) {   // (a handle of each thread on spirv.pack, of the store opened last)
            if (h.f) std::fclose(h.f);
            h.f = std::fopen(pack_path.c_str(), "rb"), h.gen = my_gen;
            if (!h.f) return false;
        }
        uint64_t head[3];
        if (seek(h.f, at - (int64_t)kBlobHead) || std::fread(head, 8, 3, h.f) != 3) {
            std::clearerr(h.f);
            return false;
        }
        if (head[0] != want.a || head[1] != want.b || head[2] != n) return false;   // (another blob there, or none)
        if (std::fread(out, 4, (size_t)n, h.f) != n) {
            std::clearerr(h.f);
            return false;
        }
        if (const Hash got = hash_words(out, (size_t)n); got.a != head[0] || got.b != head[1]) {
            std::lock_guard lk(bad_mu);
            bad.insert(at);
            return false;
        }
        return true;
    }

    size_t blob_count() const {
        std::lock_guard lk(mu);
        return blobs.size();
    }
    uint64_t written() const { return written_bytes; }

private:
    struct Blob {
        int64_t at;
        uint64_t words;
    };
    enum class Head { missing, bad, ok, unreadable };
    std::string dir, pack_path, idx_path;
    FILE* pack_out = nullptr;
    FILE* pack_in = nullptr;   // (the heads of blobs already there, read under the lock before one is given out again)
    FILE* idx_out = nullptr;
    FILE* idx_in = nullptr;
    int64_t idx_read = kHead;   // (how far into spirv.idx this process has read: entries past it are other processes')
    uint32_t store_id = 0;
    std::atomic<uint64_t> written_bytes{0};
    uint64_t my_gen = 0;   // (this opening's number among every Store's: a thread's read handle is of one opening)
    struct Handle {
        FILE* f = nullptr;
        uint64_t gen = 0;
        ~Handle() {
            if (f) std::fclose(f);
        }
    };
    static Handle& thread_handle() {
        thread_local Handle h;
        return h;
    }
    static std::atomic<uint64_t>& generations() {
        static std::atomic<uint64_t> g{0};
        return g;
    }
    std::unordered_map<Hash, Blob, HashHash> blobs;
    mutable std::mutex mu;   // (this process's threads; the lock on spirv.lock is for the other processes)
    mutable std::mutex bad_mu;
    mutable std::unordered_set<int64_t> bad;   // (places whose words did not hash to their head's: not given out again)
#ifdef _WIN32
    HANDLE os_lock = INVALID_HANDLE_VALUE;
#else
    int os_lock = -1;
#endif

    static int seek(FILE* f, int64_t at) {
#ifdef _WIN32
        return _fseeki64(f, at, SEEK_SET);
#else
        return fseeko(f, (off_t)at, SEEK_SET);
#endif
    }
    static int64_t tell(FILE* f) {
#ifdef _WIN32
        return _ftelli64(f);
#else
        return (int64_t)ftello(f);
#endif
    }
    static int64_t file_size(const std::string& path) {
        std::error_code ec;
        const auto n = std::filesystem::file_size(std::filesystem::path(path), ec);
        return ec ? -1 : (int64_t)n;
    }
    static bool zeros(FILE* f, int64_t n) {   // (n under one entry: an index entry's grid made whole again)
        static const char z[kEntry] = {};
        return n > 0 && n <= (int64_t)kEntry && std::fwrite(z, 1, (size_t)n, f) == (size_t)n;
    }
    static uint32_t new_id() {
        std::random_device rd;
        uint64_t x = ((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        x ^= x >> 29, x *= 0xBF58476D1CE4E5B9ull, x ^= x >> 32;
        const uint32_t id = (uint32_t)x;
        return id ? id : 1;   // (0 is "no store")
    }
    /// missing: no such file; bad: too short, or not a head of this version; unreadable: there, but not opened or read now
    static Head read_head(const std::string& path, uint32_t magic, uint32_t& id) {
        std::error_code ec;
        const bool there = std::filesystem::exists(std::filesystem::path(path), ec);
        if (ec) return Head::unreadable;
        if (!there) return Head::missing;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return Head::unreadable;
        uint32_t h[4]{};
        const size_t got = std::fread(h, 4, 4, f);
        const bool error = std::ferror(f) != 0;
        std::fclose(f);
        if (error) return Head::unreadable;
        if (got != 4 || h[0] != magic || h[1] != kVersion || !h[2]) return Head::bad;
        id = h[2];
        return Head::ok;
    }
    /// The file made new, a head alone: written beside it, then put in its place. Never the old file cut short: a Longshot
    /// still reading it keeps it (Linux), and while one has it open it is not replaced (Windows: false)
    static bool write_head(const std::string& path, uint32_t magic, uint32_t id) {
        const std::string fresh = path + ".new";
        FILE* f = std::fopen(fresh.c_str(), "wb");
        if (!f) return false;
        const uint32_t h[4] = {magic, kVersion, id, 0};
        const bool ok = std::fwrite(h, 4, 4, f) == 4 && std::fflush(f) == 0;
        std::error_code ec;
        if (std::fclose(f) != 0 || !ok) {
            std::filesystem::remove(std::filesystem::path(fresh), ec);
            return false;
        }
        std::filesystem::rename(std::filesystem::path(fresh), std::filesystem::path(path), ec);
        if (ec) {
            std::error_code ignored;
            std::filesystem::remove(std::filesystem::path(fresh), ignored);
        }
        return !ec;
    }
    bool fail() {
        close();
        return false;
    }
    /// (held) the heads read (the store made when there is none), the files opened, the index read
    bool open_held() {
        uint32_t pid = 0, iid = 0;
        const Head ph = read_head(pack_path, kPackMagic, pid), ih = read_head(idx_path, kIdxMagic, iid);
        // (a file there that could not be read now, held by another program: none this run, the store kept as it is)
        if (ph == Head::unreadable || ih == Head::unreadable) return false;
        if (ph != Head::ok || ih != Head::ok || pid != iid) {   // (none yet, one of the two damaged or of another store)
            pid = new_id();
            // (the index first: a crash between the two leaves a store of two ids, made again at the next start)
            if (!write_head(idx_path, kIdxMagic, pid) || !write_head(pack_path, kPackMagic, pid)) return false;
        }
        pack_out = std::fopen(pack_path.c_str(), "ab");
        pack_in = std::fopen(pack_path.c_str(), "rb");
        idx_out = std::fopen(idx_path.c_str(), "ab");
        idx_in = std::fopen(idx_path.c_str(), "rb");
        if (!pack_out || !pack_in || !idx_out || !idx_in) return false;
        store_id = pid;
        my_gen = ++generations();
        idx_read = kHead;
        catch_up();
        return true;
    }
    bool is_bad(int64_t at) const {
        std::lock_guard lk(bad_mu);
        return bad.count(at) != 0;
    }
    /// (held) whether the blob at at has these words' hash and count in its head
    bool head_is(int64_t at, const Hash& h, uint64_t n) {
        uint64_t head[3];
        if (seek(pack_in, at - (int64_t)kBlobHead) || std::fread(head, 8, 3, pack_in) != 3) {
            std::clearerr(pack_in);
            return false;
        }
        return head[0] == h.a && head[1] == h.b && head[2] == n;
    }
    /// (held) the entries other processes added since this one last read, into blobs (a later entry for the same words
    /// replaces an earlier one), a few thousand at a time. An entry naming bytes the pack does not have (a pack cut short by
    /// hand, a damaged entry) is not taken, and nothing is written for it: a record naming its place reads nothing there
    void catch_up() {
        const int64_t size = file_size(idx_path), pack_size = file_size(pack_path);
        if (size < idx_read + (int64_t)kEntry || seek(idx_in, idx_read)) return;
        constexpr size_t kBatch = 4096;
        std::vector<uint64_t> e(kBatch * 4);
        for (int64_t left = (size - idx_read) / (int64_t)kEntry; left > 0;) {
            const size_t want = (size_t)std::min<int64_t>(left, (int64_t)kBatch);
            const size_t got = std::fread(e.data(), kEntry, want, idx_in);
            for (size_t i = 0; i < got; i++) {
                const uint64_t* x = &e[i * 4];
                if (!x[3] || x[3] >= kMostWords || x[2] < kHead + kBlobHead || x[2] >= (uint64_t)kMostAt) continue;
                const int64_t at = (int64_t)x[2], end = at + (int64_t)x[3] * 4;   // (both bounded: no overflow)
                if (end <= pack_size) blobs[{x[0], x[1]}] = {at, x[3]};
            }
            idx_read += (int64_t)(got * kEntry);
            if (got < want) break;
            left -= (int64_t)got;
        }
        std::clearerr(idx_in);
    }

    struct Held {
        Store& s;
        std::unique_lock<std::mutex> lk;
        bool ok = false;
        explicit Held(Store& st) : s(st), lk(st.mu) { ok = s.lock(); }
        ~Held() {
            if (ok) s.unlock();
        }
    };
    // (the lock: on spirv.lock, the store's own file, so two spellings of one folder are one lock, and the system lets it go
    // when the Longshot holding it ends. Waited for a minute at most: then that put keeps its SPIR-V in its record)
    static constexpr int kLockWaitMs = 60000;
#ifdef _WIN32
    bool lock_init() {
        os_lock = CreateFileA((dir + "spirv.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return os_lock != INVALID_HANDLE_VALUE;
    }
    void lock_close() {
        if (os_lock != INVALID_HANDLE_VALUE) CloseHandle(os_lock), os_lock = INVALID_HANDLE_VALUE;
    }
    bool lock() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLockWaitMs);
        for (;;) {
            OVERLAPPED ov{};
            if (LockFileEx(os_lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) return true;
            if (GetLastError() != ERROR_LOCK_VIOLATION || std::chrono::steady_clock::now() >= until) return false;
            Sleep(1);
        }
    }
    void unlock() {
        OVERLAPPED ov{};
        UnlockFileEx(os_lock, 0, 1, 0, &ov);
    }
#else
    bool lock_init() {
        os_lock = ::open((dir + "spirv.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        return os_lock >= 0;
    }
    void lock_close() {
        if (os_lock >= 0) ::close(os_lock), os_lock = -1;
    }
    bool lock() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLockWaitMs);
        for (;;) {
            if (flock(os_lock, LOCK_EX | LOCK_NB) == 0) return true;
            if ((errno != EWOULDBLOCK && errno != EINTR) || std::chrono::steady_clock::now() >= until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void unlock() { flock(os_lock, LOCK_UN); }
#endif
};

}   // namespace spirv_pack
