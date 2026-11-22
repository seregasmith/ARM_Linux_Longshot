// tests/test_pcache_stamp.py compiles and runs this: gpu/vk/pcache_stamp.h, what the pipeline cache's files were made for.
#include "../gpu/vk/pcache_stamp.h"

#include <cstdio>
#include <filesystem>
#include <string>

static int failed;
#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            std::printf("FAILED line %d: %s\n", __LINE__, #x);  \
            failed++;                                           \
        }                                                       \
    } while (0)

static void put(const std::string& path, size_t bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    for (size_t i = 0; i < bytes; i++) fputc('x', f);
    fclose(f);
}
static bool there(const std::string& path) { return std::filesystem::exists(path); }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string dir = std::string(argv[1]) + "/";
    using namespace pcache_stamp;

    // the names it takes for the cache's: the main file and the play files, with their .tmp; nothing else in the folder
    CHECK(is_cache_file("longshot_vk_pipelines.bin") && is_cache_file("longshot_vk_pipelines.bin.tmp"));
    CHECK(is_cache_file("longshot_vk_pipelines_play_1.bin") && is_cache_file("longshot_vk_pipelines_play_123.bin.tmp"));
    CHECK(!is_cache_file("longshot_vk_pipelines.stamp") && !is_cache_file("longshot_programs.bin") && !is_cache_file("longshot_pipelines.bin"));
    CHECK(!is_cache_file("longshot_vk_pipelines_play_.bin") && !is_cache_file("longshot_vk_pipelines_play_x.bin") &&
          !is_cache_file("longshot_vk_pipelines_play_1.txt") && !is_cache_file("longshot_vk_pipelines_play_1"));
    CHECK(!is_cache_file("old_longshot_vk_pipelines.bin") && !is_cache_file("save.bin"));
    CHECK(!is_cache_file(kBaseFile));   // (the base goes by write_base, not with the cache's files)

    // a folder from before the stamp file: its cache files go, the others stay, and the stamp is written
    put(dir + "longshot_vk_pipelines.bin", 3000);
    put(dir + "longshot_vk_pipelines_play_1.bin", 200);
    put(dir + "longshot_vk_pipelines_play_7.bin", 100);
    put(dir + "longshot_programs.bin", 50);
    put(dir + "save.bin", 10);
    Dropped d = check(dir, 0x1234);
    CHECK(d.files == 3 && d.bytes == 3300);
    CHECK(!there(dir + "longshot_vk_pipelines.bin") && !there(dir + "longshot_vk_pipelines_play_1.bin") && !there(dir + "longshot_vk_pipelines_play_7.bin"));
    CHECK(there(dir + "longshot_programs.bin") && there(dir + "save.bin"));
    uint64_t s = 0;
    CHECK(read(dir, &s) && s == 0x1234);

    // the same stamp: nothing goes
    put(dir + "longshot_vk_pipelines.bin", 4000);
    put(dir + "longshot_vk_pipelines_play_2.bin", 300);
    d = check(dir, 0x1234);
    CHECK(d.files == 0 && there(dir + "longshot_vk_pipelines.bin") && there(dir + "longshot_vk_pipelines_play_2.bin"));

    // another translator, no precompile finished for this cache: carried with no size to judge it by, one such carry counted
    const auto base_of = [&](uint64_t st) { return read_base(dir, st, st); };
    const auto built = [&](uint64_t st) {
        FILE* f = fopen((dir + kBuiltFile).c_str(), "wb");
        const uint64_t rec[4] = {st, 1, 1, kBuiltWhole};
        fwrite(rec, sizeof rec, 1, f);
        fclose(f);
    };
    d = check(dir, 0x5678);
    CHECK(d.files == 0 && d.carried && d.main == 4000 && d.all == 4300 && d.base == 0 && there(dir + "longshot_vk_pipelines.bin") &&
          there(dir + "longshot_vk_pipelines_play_2.bin"));
    CHECK(read(dir, &s) && s == 0x5678 && base_of(0x5678).bytes == 0 && base_of(0x5678).carries == 1);
    // the same translator again: kept, nothing written
    d = check(dir, 0x5678);
    CHECK(d.files == 0 && !d.carried && base_of(0x5678).carries == 1);
    // it fills in and its precompile finishes (far past the size it was carried at): carried whole, that size the base. This is
    // a preparation stopped early and finished under the next translator: all of it is live
    put(dir + "longshot_vk_pipelines.bin", 6000);
    built(0x5678);
    d = check(dir, 0x9999);
    CHECK(d.carried && d.files == 0 && d.base == 6300 && read(dir, &s) && s == 0x9999 && base_of(0x9999).bytes == 6300);
    // within half as large again of the base, the play files counted (9150 + 300 = 9450 of 6300): carried, the base kept
    put(dir + "longshot_vk_pipelines.bin", 9150);
    d = check(dir, 0xa000);
    CHECK(d.carried && d.files == 0 && read(dir, &s) && s == 0xa000 && base_of(0xa000).bytes == 6300);
    // past it by what the play files hold, the main file alone within it: dropped, the stamp the new one's and no base
    put(dir + "longshot_vk_pipelines.bin", 9151);
    d = check(dir, 0xaaaa);
    CHECK(!d.carried && d.files == 2 && d.bytes == 9451 && !there(dir + "longshot_vk_pipelines.bin") && !there(dir + kBaseFile));
    CHECK(read(dir, &s) && s == 0xaaaa && base_of(0xaaaa).bytes == 0);

    // a record that does not say its pipelines reached the file (a start that gave up on building them after two crashed in it,
    // or a build from before the word): no size to take as the base, so what is built later is not taken for dead weight
    put(dir + "longshot_vk_pipelines.bin", 1000);
    write(dir, 0xb800);
    {
        FILE* f = fopen((dir + kBuiltFile).c_str(), "wb");
        const uint64_t rec[3] = {0xb800, 1, 1};
        fwrite(rec, sizeof rec, 1, f);
        fclose(f);
    }
    CHECK(!finished_for(dir, 0xb800));
    {
        FILE* f = fopen((dir + kBuiltFile).c_str(), "wb");
        const uint64_t rec[4] = {0xb800, 1, 1, 0};   // (a fourth word, not the one that says so)
        fwrite(rec, sizeof rec, 1, f);
        fclose(f);
    }
    CHECK(!finished_for(dir, 0xb800));
    d = check(dir, 0xb801);
    CHECK(d.carried && d.base == 0 && base_of(0xb801).bytes == 0 && base_of(0xb801).carries == 1);
    built(0xb801);
    CHECK(finished_for(dir, 0xb801) && !finished_for(dir, 0xb802));
    std::filesystem::remove(dir + kBaseFile);

    // never finished: carried kUnfinishedCarries times, then dropped, as nothing else bounds it
    std::filesystem::remove(dir + kBuiltFile);
    put(dir + "longshot_vk_pipelines.bin", 1000);
    uint64_t st = 0xb000;
    write(dir, st);
    for (uint64_t i = 1; i <= kUnfinishedCarries; i++) {
        d = check(dir, st + i);
        CHECK(d.carried && base_of(st + i).carries == i);
    }
    d = check(dir, st + kUnfinishedCarries + 1);
    CHECK(!d.carried && d.files == 1 && !there(dir + kBaseFile));

    // a start cut short between the base and the stamp: the next finishes that carry, not counting it twice
    put(dir + "longshot_vk_pipelines.bin", 1000);
    write(dir, 0xc000);
    write_base(dir, Base{0, 0xc001, 0xc000, kUnfinishedCarries});
    CHECK(read_base(dir, 0xc000, 0xc001).carries == kUnfinishedCarries && read_base(dir, 0xc000, 0xc002).carries == 0);
    d = check(dir, 0xc001);
    CHECK(d.carried && read(dir, &s) && s == 0xc001 && base_of(0xc001).carries == kUnfinishedCarries);

    // the base cannot be written: carried for this start, the stamp left, so the next start carries it with the base it knew
    std::filesystem::remove(dir + kBaseFile);
    std::filesystem::create_directory(dir + kBaseFile);
    write(dir, 0xc100);
    d = check(dir, 0xc101);
    CHECK(d.carried && read(dir, &s) && s == 0xc100 && !there(dir + kBaseFile + ".tmp"));
    std::filesystem::remove(dir + kBaseFile);
    d = check(dir, 0xc101);
    CHECK(d.carried && read(dir, &s) && s == 0xc101 && base_of(0xc101).carries == 1);

    // a file of the cache that will not go (a folder here, as a file held open): the stamp stays, and the next start drops it
    const std::string stuck = dir + "longshot_vk_pipelines_play_9.bin";
    std::filesystem::create_directory(stuck);
    put(stuck + "/x", 1);
    write(dir, 0xc200);
    write_base(dir, Base{100, 0xc200, 0, 0});
    put(dir + "longshot_vk_pipelines.bin", 1000);
    d = check(dir, 0xc201);
    CHECK(!d.carried && read(dir, &s) && s == 0xc200 && there(dir + kBaseFile));
    std::filesystem::remove_all(stuck);
    d = check(dir, 0xc201);
    CHECK(!d.carried && read(dir, &s) && s == 0xc201 && !there(dir + kBaseFile));

    // a base beside another stamp than the stamp file's (a build from before the base dropped and wrote its own stamp in between):
    // not taken
    write_base(dir, Base{3000, 0xbbbb, 0xbbba, 0});
    write(dir, 0xcccc);
    put(dir + "longshot_vk_pipelines.bin", 9000);   // (9000 of the old base 3000: dropped, were that base taken)
    CHECK(read_base(dir, 0xcccc, 0xdddd).bytes == 0);
    d = check(dir, 0xdddd);
    CHECK(d.carried && base_of(0xdddd).bytes == 0 && base_of(0xdddd).carries == 1);
    // one that names this build's stamp but moved from another than the stamp file's (an older build ran since): not taken either
    write_base(dir, Base{3000, 0xd001, 0xd000, 0});
    write(dir, 0xd005);
    CHECK(read_base(dir, 0xd005, 0xd001).bytes == 0);
    d = check(dir, 0xd001);
    CHECK(d.carried && base_of(0xd001).bytes == 0 && base_of(0xd001).carries == 1);

    // another translator with no main file: nothing to carry, the play files go
    std::filesystem::remove(dir + "longshot_vk_pipelines.bin");
    put(dir + "longshot_vk_pipelines_play_3.bin", 100);
    d = check(dir, 0xeeee);
    CHECK(!d.carried && d.files == 1 && !there(dir + "longshot_vk_pipelines_play_3.bin") && read(dir, &s) && s == 0xeeee &&
          !there(dir + kBaseFile));

    // the verdict alone, at its edges
    const Base measured{100, 2, 0, 0}, none{}, spent{0, 2, 0, kUnfinishedCarries};
    CHECK(judge(false, 0, 1, nullptr, 100, 100, measured, true) == Verdict::Drop);   // (no stamp file)
    CHECK(judge(true, 1, 1, nullptr, 1u << 30, 1u << 30, measured, false) == Verdict::Keep);   // (the same stamp, whatever its size)
    CHECK(judge(true, 2, 1, nullptr, 100, 150, measured, false) == Verdict::Carry);
    CHECK(judge(true, 2, 1, nullptr, 100, 151, measured, true) == Verdict::Drop);   // (the play files count; finished moves no base)
    CHECK(judge(true, 2, 1, nullptr, 0, 100, measured, true) == Verdict::Drop);   // (nothing to carry)
    CHECK(judge(true, 2, 1, nullptr, 1u << 30, 1u << 30, none, false) == Verdict::Carry);   // (no base yet)
    CHECK(judge(true, 2, 1, nullptr, 100, 100, spent, false) == Verdict::Drop && judge(true, 2, 1, nullptr, 100, 100, spent, true) == Verdict::Carry);
    CHECK(judge(true, 2, 1, [](uint64_t k) { return k == 2; }, 1u << 30, 1u << 30, measured, false) == Verdict::Keep);   // (`also`)

    // an empty folder: nothing to drop, the stamp written; a damaged stamp file counts as none, and its files go
    std::filesystem::remove(dir + kBaseFile);
    d = check(dir, 0x5678);
    CHECK(d.files == 0);
    put(dir + "longshot_vk_pipelines.stamp", 3);
    CHECK(!read(dir, &s));
    put(dir + "longshot_vk_pipelines.bin", 10);
    d = check(dir, 0x5678);
    CHECK(d.files == 1 && read(dir, &s) && s == 0x5678);
    put(dir + "longshot_vk_pipelines.stamp", 9);   // (a byte too many)
    CHECK(!read(dir, &s));
    put(dir + kBaseFile, 5);   // (a damaged base file: none known)
    CHECK(read_base(dir, 0x5678, 0x5678).bytes == 0);

    // a stamp `also` takes (the same translator under an earlier program cache format): kept, the stamp file as it was
    write(dir, 0x5678);
    std::filesystem::remove(dir + kBaseFile);
    put(dir + "longshot_vk_pipelines.bin", 500);
    d = check(dir, 0x9abc, [](uint64_t kept) { return kept == 0x5678; });
    CHECK(d.files == 0 && !d.carried && there(dir + "longshot_vk_pipelines.bin") && read(dir, &s) && s == 0x5678);
    // one it does not take is another translator's: carried
    d = check(dir, 0x9abc, [](uint64_t kept) { return kept == 0x1111; });
    CHECK(d.carried && there(dir + "longshot_vk_pipelines.bin") && read(dir, &s) && s == 0x9abc && base_of(0x9abc).carries == 1);

    if (failed) return 1;
    std::printf("pcache_stamp: ok\n");
    return 0;
}
