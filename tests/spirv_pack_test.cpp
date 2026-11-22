// The shader store (gpu/vk/spirv_pack.h), on its own: tests/test_spirv_pack.py builds and runs this.
// Synthetic SPIR-V words only. Two Store objects on one folder stand for two Longshots sharing it (each takes the store's lock
// as another process would).
#include "spirv_pack.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) std::printf("FAIL line %d: %s\n", __LINE__, #c), failures++;   \
    } while (0)

static std::vector<uint32_t> words(uint32_t seed, size_t n) {
    std::vector<uint32_t> w(n);
    w[0] = 0x07230203;   // (SPIR-V's magic, as a stage's first word)
    for (size_t i = 1; i < n; i++) w[i] = seed * 2654435761u + (uint32_t)i * 40503u;
    return w;
}
static spirv_pack::Hash hash(const std::vector<uint32_t>& w) { return spirv_pack::hash_words(w.data(), w.size()); }
static bool reads(const spirv_pack::Store& s, int64_t at, const std::vector<uint32_t>& want) {
    std::vector<uint32_t> got(want.size());
    return s.read(at, want.size(), hash(want), got.data()) && got == want;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string dir = argv[1];
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto a = words(1, 1000), b = words(2, 1000), c = words(3, 7);

    // (one Longshot: the same words are one blob, read back by their place)
    spirv_pack::Store one;
    CHECK(one.open(dir));
    CHECK(one.on() && one.id() != 0);
    const int64_t at_a = one.put(a.data(), a.size());
    const int64_t at_b = one.put(b.data(), b.size());
    CHECK(at_a > 0 && at_b > 0 && at_a != at_b);
    CHECK(one.put(a.data(), a.size()) == at_a);   // (once)
    CHECK(one.blob_count() == 2);
    CHECK(reads(one, at_a, a) && reads(one, at_b, b));
    {
        std::vector<uint32_t> got(a.size());
        CHECK(!one.read(at_a, a.size() + 1, hash(a), got.data()));   // (another count than the blob's head: not read)
        CHECK(!one.read(at_a + 4, a.size(), hash(a), got.data()));   // (not a blob's place)
        CHECK(!one.read(0, a.size(), hash(a), got.data()));
        std::vector<uint32_t> got_b(b.size());
        CHECK(!one.read(at_a, b.size(), hash(b), got_b.data()));   // (a's place, asked for b's words: another blob is there)
    }
    const uint32_t id = one.id();

    // (a second Longshot on the same store: it finds the first one's blobs, and each finds what the other adds)
    spirv_pack::Store two;
    CHECK(two.open(dir));
    CHECK(two.id() == id);
    CHECK(two.put(a.data(), a.size()) == at_a);
    const int64_t at_c = two.put(c.data(), c.size());
    CHECK(at_c > at_b);
    CHECK(one.put(c.data(), c.size()) == at_c);   // (read from the index the other wrote, not written again)
    CHECK(reads(one, at_c, c) && reads(two, at_b, b));
    CHECK(one.blob_count() == 3 && two.blob_count() == 3);

    // (the next start: the same store, the same blobs)
    one.close();
    two.close();
    CHECK(!one.on());
    spirv_pack::Store again;
    CHECK(again.open(dir));
    CHECK(again.id() == id && again.blob_count() == 3);
    CHECK(reads(again, at_a, a) && reads(again, at_c, c));

    // (a write cut short: bytes past the last entry, named by nothing; the same words are written again whole, after them)
    {
        FILE* f = std::fopen((dir + "/spirv.pack").c_str(), "ab");
        CHECK(f != nullptr);
        const uint64_t junk[3] = {1, 2, 500};
        std::fwrite(junk, 8, 3, f);
        std::fclose(f);
    }
    const auto d = words(4, 500);
    const int64_t at_d = again.put(d.data(), d.size());
    CHECK(at_d > at_c && reads(again, at_d, d));
    again.close();

    // (an index naming bytes the pack does not have, a pack cut short by hand: not taken)
    {
        const auto size = std::filesystem::file_size(dir + "/spirv.pack");
        std::filesystem::resize_file(dir + "/spirv.pack", size - 8);
        spirv_pack::Store cut;
        CHECK(cut.open(dir));
        CHECK(cut.blob_count() == 3);   // (d's entry left out)
        std::vector<uint32_t> got(d.size());
        CHECK(!cut.read(at_d, d.size(), hash(d), got.data()));
        CHECK(reads(cut, at_a, a));
        CHECK(cut.put(d.data(), d.size()) > at_d);   // (written again: the cache names the new place)
        cut.close();
    }

    // (the store deleted, by the player or the launcher: a new store, with another id, that has none of the old blobs)
    std::filesystem::remove(dir + "/spirv.pack", ec);
    std::filesystem::remove(dir + "/spirv.idx", ec);
    spirv_pack::Store fresh;
    CHECK(fresh.open(dir));
    CHECK(fresh.id() != 0 && fresh.id() != id && fresh.blob_count() == 0);
    {
        std::vector<uint32_t> got(a.size());
        CHECK(!fresh.read(at_a, a.size(), hash(a), got.data()));
    }
    // (one of the two files of another store: a new store too)
    fresh.close();
    std::filesystem::remove(dir + "/spirv.idx", ec);
    spirv_pack::Store half;
    CHECK(half.open(dir));
    CHECK(half.blob_count() == 0);
    half.close();

    // (a blob's words changed on the disk under an intact head: not read, and its place not given out again)
    {
        const std::string sub = dir + "/flip";
        spirv_pack::Store s;
        CHECK(s.open(sub));
        const int64_t at = s.put(a.data(), a.size());
        {   // (read back on a thread of its own: read's handle is the thread's, and this thread's would keep the words it read in
            // its buffer, the C library's, past the change below)
            bool read_back = false;
            std::thread([&] { read_back = reads(s, at, a); }).join();
            CHECK(read_back);
        }
        {
            FILE* f = std::fopen((sub + "/spirv.pack").c_str(), "r+b");
            CHECK(f != nullptr);
            const uint32_t other = 0xDEADBEEF;
            std::fseek(f, (long)(at + 40), SEEK_SET);
            std::fwrite(&other, 4, 1, f);
            std::fclose(f);
        }
        std::vector<uint32_t> got(a.size());
        CHECK(!s.read(at, a.size(), hash(a), got.data()));   // (the words are not the hash's)
        const int64_t again_at = s.put(a.data(), a.size());
        CHECK(again_at > at && reads(s, again_at, a));   // (written again, at a place of its own)
        s.close();
        spirv_pack::Store next;   // (the next start takes the later entry for those words)
        CHECK(next.open(sub));
        CHECK(next.put(a.data(), a.size()) == again_at);
        next.close();
    }

    // (zeros under an entry, as a crash can leave the disk's blocks: the next start writes those words again before any read)
    {
        const std::string sub = dir + "/zeros";
        spirv_pack::Store s;
        CHECK(s.open(sub));
        const int64_t at = s.put(b.data(), b.size());
        s.close();
        {
            FILE* f = std::fopen((sub + "/spirv.pack").c_str(), "r+b");
            CHECK(f != nullptr);
            const uint64_t zero[3] = {};
            std::fseek(f, (long)(at - 24), SEEK_SET);
            std::fwrite(zero, 8, 3, f);
            std::fclose(f);
        }
        CHECK(s.open(sub));
        const int64_t again_at = s.put(b.data(), b.size());
        CHECK(again_at != at && reads(s, again_at, b));
        s.close();
    }

    // (a pack cut short by hand under an entry: the next blob may take x's old place, and a record naming that place for x
    // reads nothing there, never e's words, even of x's length)
    {
        const std::string sub = dir + "/short";
        const auto x = words(5, 500), e = words(6, 500);
        spirv_pack::Store s;
        CHECK(s.open(sub));
        const int64_t at_x = s.put(x.data(), x.size());
        s.close();
        std::filesystem::resize_file(sub + "/spirv.pack", (uintmax_t)(at_x - 24));   // (x gone whole)
        CHECK(s.open(sub));
        const int64_t at_e = s.put(e.data(), e.size());
        CHECK(at_e == at_x);   // (its place again: nothing is written for the entry the pack no longer has)
        s.close();
        spirv_pack::Store next;
        CHECK(next.open(sub));
        std::vector<uint32_t> got(x.size());
        CHECK(!next.read(at_x, x.size(), hash(x), got.data()));   // (e is there, of x's length: not x)
        CHECK(reads(next, at_e, e));
        const int64_t new_x = next.put(x.data(), x.size());
        CHECK(new_x != at_x && reads(next, new_x, x));
        next.close();
    }

    // (an index entry naming a place far past the pack's end, damaged: not taken, and nothing written for it. The next blob
    // goes at the pack's end, not out to that place)
    {
        const std::string sub = dir + "/far";
        const auto y = words(10, 300), z = words(11, 400);
        spirv_pack::Store s;
        CHECK(s.open(sub));
        CHECK(s.put(y.data(), y.size()) > 0);
        s.close();
        {
            FILE* f = std::fopen((sub + "/spirv.idx").c_str(), "ab");
            CHECK(f != nullptr);
            const uint64_t damaged[4] = {21, 22, 1ull << 46, 1000};   // (64 TB in)
            std::fwrite(damaged, 8, 4, f);
            std::fclose(f);
        }
        const auto before = std::filesystem::file_size(sub + "/spirv.pack");
        spirv_pack::Store next;
        CHECK(next.open(sub));
        CHECK(next.blob_count() == 1);
        const int64_t at_z = next.put(z.data(), z.size());
        CHECK(at_z == (int64_t)before + 24 && reads(next, at_z, z));
        CHECK(std::filesystem::file_size(sub + "/spirv.pack") == before + 24 + z.size() * 4);
        next.close();
    }

    // (a store made again while another Longshot reads it, one of its files damaged: the reader keeps reading its blobs.
    // Linux: the store is made as new files in the old ones' place, the reader keeps the old ones. Windows: while the reader
    // has them open they are not replaced, and the other Longshot goes without the store this run)
    {
        const std::string sub = dir + "/live";
        const auto v = words(12, 800);
        spirv_pack::Store reader;
        CHECK(reader.open(sub));
        const int64_t at_v = reader.put(v.data(), v.size());
        CHECK(reads(reader, at_v, v));   // (its read handle open)
        const uint32_t reader_id = reader.id();
        {
            FILE* f = std::fopen((sub + "/spirv.idx").c_str(), "r+b");
            CHECK(f != nullptr);
            const uint32_t junk = 0x12345678;
            std::fwrite(&junk, 4, 1, f);   // (the index's magic damaged)
            std::fclose(f);
        }
        spirv_pack::Store other;
#ifdef _WIN32
        CHECK(!other.open(sub));
#else
        CHECK(other.open(sub));
        CHECK(other.id() != reader_id && other.blob_count() == 0);
        const auto u = words(13, 800);
        CHECK(other.put(u.data(), u.size()) == at_v);   // (its first blob where the reader's is, in the new file)
        other.close();
#endif
        CHECK(reader.id() == reader_id && reads(reader, at_v, v));
        reader.close();
    }

    // (an index entry cut short, by a full disk: the entries after it stay on the grid, for this Longshot and the others)
    {
        const std::string sub = dir + "/idx";
        const auto f1 = words(7, 50), f2 = words(8, 60), f3 = words(9, 70);
        spirv_pack::Store s, other;
        CHECK(s.open(sub) && other.open(sub));
        CHECK(s.put(f1.data(), f1.size()) > 0);
        {
            FILE* f = std::fopen((sub + "/spirv.idx").c_str(), "ab");
            CHECK(f != nullptr);
            const uint64_t half[2] = {11, 12};
            std::fwrite(half, 8, 2, f);
            std::fclose(f);
        }
        const int64_t at2 = s.put(f2.data(), f2.size());
        CHECK(at2 > 0);
        CHECK((std::filesystem::file_size(sub + "/spirv.idx") - 16) % 32 == 0);
        const int64_t at3 = other.put(f3.data(), f3.size());
        CHECK(other.put(f2.data(), f2.size()) == at2);   // (found, not written again)
        CHECK(s.put(f3.data(), f3.size()) == at3);       // (this one still reads the others' entries)
        s.close();
        other.close();
        spirv_pack::Store next;
        CHECK(next.open(sub));
        CHECK(next.blob_count() == 3 && reads(next, at2, f2) && reads(next, at3, f3));
        next.close();
    }

#ifdef _WIN32
    // (a store file held by another program at the start: none this run, and the store kept, not made again)
    {
        const std::string sub = dir + "/held";
        spirv_pack::Store s;
        CHECK(s.open(sub));
        const int64_t at = s.put(a.data(), a.size());
        const uint32_t held_id = s.id();
        s.close();
        const HANDLE h = CreateFileA((sub + "/spirv.idx").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        spirv_pack::Store busy;
        CHECK(!busy.open(sub));
        CloseHandle(h);
        spirv_pack::Store later;
        CHECK(later.open(sub));
        CHECK(later.id() == held_id && reads(later, at, a));
        later.close();
    }
#endif

    // (no folder: none)
    spirv_pack::Store none;
    CHECK(!none.open(""));
    CHECK(none.put(a.data(), a.size()) == -1);

    // (the hash: the words' order and count are in it)
    std::vector<uint32_t> swapped = a;
    std::swap(swapped[5], swapped[6]);
    CHECK(!(spirv_pack::hash_words(a.data(), a.size()) == spirv_pack::hash_words(swapped.data(), swapped.size())));
    CHECK(!(spirv_pack::hash_words(a.data(), a.size()) == spirv_pack::hash_words(a.data(), a.size() - 1)));
    CHECK(spirv_pack::hash_words(a.data(), a.size()) == spirv_pack::hash_words(a.data(), a.size()));

    std::filesystem::remove_all(dir, ec);
    if (failures) return 1;
    std::printf("ok\n");
    return 0;
}
