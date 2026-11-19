// The renderer's memory accountant (gpu/vk/mem_account.h) with made-up pools: tests/test_mem_account.py compiles and runs this.
// With the argument "line" it prints the formatted line and nothing else, for the script's parser (scripts/memtour.py).
#include "../gpu/vk/mem_account.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

using namespace mkwvk::mem_account;

namespace {
const uint64_t KB = 1ull << 10, MB = 1ull << 20;
int asked = 0;   // (how many report functions a collect called)

void textures(Report& r) { asked++, r.held[VRAM] = 3480 * MB; }
void target_pool(Report& r) { asked++, r.held[VRAM] = r.reuse[VRAM] = 512 * MB; }
void staging(Report& r) { asked++, r.held[VRAM] = 128 * MB, r.held[RAM] = 64 * MB; }           // (a pool in both ledgers)
void dead_images(Report& r) { asked++, r.held[VRAM] = r.pending[VRAM] = 300 * KB; }            // (under 1 MB: folded)
void scratch(Report& r) { asked++, r.held[VRAM] = 900 * KB; }                                  // (and this one with it)
void nothing(Report&) { asked++; }                                                             // (holds nothing: not shown)
void imports(Report& r) { asked++, r.held[RAM] = 2100 * MB, r.pending[RAM] = 100 * MB; }
void captures(Report& r) { asked++, r.held[RAM] = 512 * MB; }                                  // (as large as the spares: before them)
void capture_spares(Report& r) { asked++, r.held[RAM] = r.reuse[RAM] = 512 * MB; }
void capture_table(Report& r) { asked++, r.held[RAM] = 512 * KB; }                             // (folded, and under 1 MB in all)

// ---- the arbiter's pools: each writes down that it was asked and for how much, and gives what it has ----
int who_asked[16], asks = 0;
uint64_t want_seen[16];
uint64_t spares_have = 0, staging_has = 0, pool_has = 0;
uint64_t give(int who, uint64_t& has, const Ask& a, bool willing = true) {
    CHECK(asks < 16);
    who_asked[asks] = who, want_seen[asks++] = a.want;
    const uint64_t g = willing ? std::min(has, a.want) : 0;
    has -= g;
    return g;
}
uint64_t give_spares(const Ask& a) { return give(1, spares_have, a, a.entered); }   // (only as the pressure begins)
uint64_t give_staging(const Ask& a) { return give(2, staging_has, a); }
uint64_t give_blocks(const Ask& a) { static uint64_t none = 0; return give(3, none, a); }   // (asked, gives nothing at once)
uint64_t give_pool(const Ask& a) { return give(4, pool_has, a); }
uint64_t give_never(const Ask& a) { static uint64_t much = 1ull << 40; return give(9, much, a); }

Registry givers() {
    Registry r;
    // (registered out of their order)
    CHECK(r.add({"target pool", nothing, 4, give_pool}));
    CHECK(r.add({"textures", textures}));                       // (no give-back: never asked)
    CHECK(r.add({"blocks free", nothing, 3, give_blocks}));
    CHECK(r.add({"not asked", nothing, 0, give_never}));        // (order 0: never asked, whatever it could give)
    CHECK(r.add({"no function", nothing, 1}));
    CHECK(r.add({"capture spares", nothing, 1, give_spares}));
    CHECK(r.add({"staging", nothing, 2, give_staging}));
    return r;
}

void check_arbiter() {
    // the lines: pressure past 90% of the allowance, the count past 97% of it; nothing with no allowance
    const uint64_t A = 10000 * MB;
    CHECK(near_line(A) == 9000 * MB && count_line(A) == 9700 * MB);
    CHECK(level(9000 * MB, 9000 * MB, A) == 0 && level(9000 * MB + 1, 9000 * MB + 1, A) == 1);
    CHECK(level(0, 9700 * MB, A) == 0 && level(0, 9700 * MB + 1, A) == 2 && level(9500 * MB, 9800 * MB, A) == 2);
    CHECK(level(1ull << 40, 1ull << 40, 0) == 0);
    // (the pressure is the count less the free space the textures can take: a full count with room in the blocks is no level 1)
    CHECK(level(8000 * MB, 9600 * MB, A) == 0);
    CHECK(excess(5000 * MB, 5000 * MB, A) == 0 && excess(9100 * MB, 9100 * MB, A) == 100 * MB);
    CHECK(excess(9100 * MB, 9800 * MB, A) == 100 * MB && excess(9000 * MB, 9900 * MB, A) == 200 * MB && excess(1, 1, 0) == 0);

    // a look: the level, what to ask for, and whether the pressure has just begun
    Arbiter ar;
    Ask a = ar.look(5000 * MB, 5000 * MB, A);
    CHECK(a.level == 0 && !a.entered && a.want == 0 && a.ledger == VRAM);
    a = ar.look(9100 * MB, 9200 * MB, A);
    CHECK(a.level == 1 && a.entered && a.want == 100 * MB);
    a = ar.look(9100 * MB, 9200 * MB, A);
    CHECK(a.level == 1 && !a.entered);                           // (still there: not begun again)
    a = ar.look(9100 * MB, 9800 * MB, A);
    CHECK(a.level == 2 && a.entered);                            // (a higher level is entered)
    CHECK(!ar.look(9100 * MB, 9800 * MB, A).entered && !ar.look(9100 * MB, 9200 * MB, A).entered);
    // (a count that sways across the line: calm for less than kCalmLooks looks is the same pressure going on)
    for (int i = 0; i < Arbiter::kCalmLooks - 1; i++) CHECK(ar.look(5000 * MB, 5000 * MB, A).level == 0);
    CHECK(!ar.look(9100 * MB, 9200 * MB, A).entered && !ar.look(9100 * MB, 9800 * MB, A).entered);
    for (int i = 0; i < Arbiter::kCalmLooks; i++) ar.look(5000 * MB, 5000 * MB, A);
    a = ar.look(9100 * MB, 9200 * MB, A);
    CHECK(a.level == 1 && a.entered);                            // (calm that long: it begins anew)
    CHECK(ar.look(9100 * MB, 9800 * MB, A).entered);
    CHECK(Arbiter::kCalmLooks == 60);                            // (600 presents at a look every 10)

    // the asking: in the pools' order whatever order they registered in, each for what is left, until the want is covered
    const Registry r = givers();
    Given gave[kMaxPools];
    size_t n = 99;
    spares_have = 100 * MB, staging_has = 50 * MB, pool_has = 300 * MB;
    CHECK(ask(r, Ask{VRAM, 120 * MB, 1, true}, gave, kMaxPools, &n) == 120 * MB);
    CHECK(asks == 2 && who_asked[0] == 1 && who_asked[1] == 2);  // (covered by the first two: the blocks and the pool not asked)
    CHECK(want_seen[0] == 120 * MB && want_seen[1] == 20 * MB);
    CHECK(n == 2 && !std::strcmp(gave[0].name, "capture spares") && gave[0].bytes == 100 * MB);
    CHECK(!std::strcmp(gave[1].name, "staging") && gave[1].bytes == 20 * MB);
    CHECK(spares_have == 0 && staging_has == 30 * MB && pool_has == 300 * MB);
    // (not covered: everyone asked in order; a pool that gives nothing is not in the list, nor counted)
    asks = 0, spares_have = 100 * MB;
    CHECK(ask(r, Ask{VRAM, 500 * MB, 1, false}, gave, kMaxPools, &n) == 330 * MB);
    CHECK(asks == 4 && who_asked[0] == 1 && who_asked[1] == 2 && who_asked[2] == 3 && who_asked[3] == 4);
    CHECK(want_seen[0] == 500 * MB && want_seen[1] == 500 * MB && want_seen[2] == 470 * MB && want_seen[3] == 470 * MB);
    CHECK(n == 2 && !std::strcmp(gave[0].name, "staging") && gave[0].bytes == 30 * MB);
    CHECK(!std::strcmp(gave[1].name, "target pool") && gave[1].bytes == 300 * MB);
    CHECK(spares_have == 100 * MB);                              // (the spares give only as the pressure begins)
    // (nobody is asked at level 0, nor for nothing)
    asks = 0, staging_has = pool_has = 100 * MB;
    CHECK(ask(r, Ask{VRAM, 500 * MB, 0, false}, gave, kMaxPools, &n) == 0 && n == 0 && asks == 0);
    CHECK(ask(r, Ask{VRAM, 0, 2, true}, gave, kMaxPools, &n) == 0 && n == 0 && asks == 0);
    CHECK(ask(Registry{}, Ask{VRAM, 500 * MB, 2, true}, gave, kMaxPools, &n) == 0 && n == 0);
    // (a list too short for the pools that gave, or none: the bytes are still all counted)
    CHECK(ask(r, Ask{VRAM, 150 * MB, 2, true}, gave, 1, &n) == 150 * MB && n == 1 && !std::strcmp(gave[0].name, "capture spares"));
    staging_has = pool_has = 100 * MB;
    CHECK(ask(r, Ask{VRAM, 150 * MB, 2, false}, nullptr, 0, nullptr) == 150 * MB && staging_has == 0 && pool_has == 50 * MB);

    char line[128];
    CHECK(format_given(line, sizeof line, Given{"target pool", 300 * MB}, 9200 * MB, A) == std::strlen(line));
    CHECK(!std::strcmp(line, "vk: budget: gave back 300 MB from target pool at 92% of the allowance"));
    format_given(line, sizeof line, Given{"staging", 30 * MB}, 9200 * MB, 0);
    CHECK(!std::strcmp(line, "vk: budget: gave back 30 MB from staging at 0% of the allowance"));
    CHECK(format_given(line, 0, Given{"staging", 30 * MB}, 1, 1) == 0);
}

Registry made_up() {
    Registry r;
    // (not in the order of their sizes: the line sorts them)
    CHECK(r.add({"dead images", dead_images}));
    CHECK(r.add({"staging", staging}));
    CHECK(r.add({"captures", captures}));
    CHECK(r.add({"capture spares", capture_spares}));
    CHECK(r.add({"target pool", target_pool}));
    CHECK(r.add({"nothing", nothing}));
    CHECK(r.add({"imports", imports}));
    CHECK(r.add({"textures", textures}));
    CHECK(r.add({"scratch", scratch}));
    CHECK(r.add({"capture table", capture_table}));
    return r;
}

const char kLine[] = "vk: budget: VRAM 4121 of 7802 MB (4500 MB by the driver, 378 MB unaccounted): textures 3480, target pool 512, "
                     "staging 128, others 1; RAM 3188 MB (the process's private memory 18200 MB): imports 2100, captures 512, capture spares 512, "
                     "staging 64, others 0";
}  // namespace

int main(int argc, char** argv) {
    const Registry r = made_up();
    const Totals t = collect(r);
    char line[1024];
    const size_t n = format(line, sizeof line, t, 7802 * MB, 4500 * MB, true, 18200 * MB);
    if (argc > 1 && !std::strcmp(argv[1], "line")) {
        std::printf("%s\n", line);
        return 0;
    }

    // the registry: a pool once, by its name; none without a name or a function; no more than it has room for
    CHECK(r.n == 10);
    Registry full = r;
    CHECK(!full.add({"textures", nothing}));
    CHECK(!full.add({nullptr, nothing}));
    CHECK(!full.add({"no function", nullptr}));
    CHECK(full.n == 10);
    static char names[kMaxPools][24];
    for (size_t i = full.n; i < kMaxPools; i++) {
        std::snprintf(names[i], sizeof names[i], "p%zu", i);
        CHECK(full.add({names[i], nothing}));
    }
    CHECK(full.n == kMaxPools && !full.add({"one more", nothing}));
    // (the renderer's own registry is one object, empty until a pool registers)
    CHECK(registry().n == 0);
    CHECK(add({"textures", textures}) && !add({"textures", textures}) && registry().n == 1);

    // collect: every pool asked once, in the order registered, and the sums by ledger
    CHECK(asked == 10 && t.n == 10);
    CHECK(!std::strcmp(t.name[0], "dead images") && !std::strcmp(t.name[9], "capture table"));
    CHECK(t.pool[1].held[VRAM] == 128 * MB && t.pool[1].held[RAM] == 64 * MB);
    CHECK(t.sum.held[VRAM] == (3480 + 512 + 128) * MB + (300 + 900) * KB);
    CHECK(t.sum.held[RAM] == (64 + 2100 + 512 + 512) * MB + 512 * KB);
    CHECK(t.sum.reuse[VRAM] == 512 * MB && t.sum.reuse[RAM] == 512 * MB);
    CHECK(t.sum.pending[VRAM] == 300 * KB && t.sum.pending[RAM] == 100 * MB);

    // what no pool reports: the driver's count less the sum, below zero when the pools count more than the driver
    CHECK(unaccounted(4500 * MB, t.sum.held[VRAM]) == (int64_t)(380 * MB - 1200 * KB));
    CHECK(unaccounted(4000 * MB, t.sum.held[VRAM]) == -(int64_t)(120 * MB + 1200 * KB));
    CHECK(mb(380 * MB - 1200 * KB) == 378 && mb(-(int64_t)(120 * MB + 1200 * KB)) == -121 && mb(0) == 0);

    // a ledger's pools: the largest first, equal ones as registered, those under 1 MB summed as "others", empty ones left out
    char pools[256];
    CHECK(format_pools(pools, sizeof pools, t, VRAM) == std::strlen(pools));
    CHECK(!std::strcmp(pools, "textures 3480, target pool 512, staging 128, others 1"));
    format_pools(pools, sizeof pools, t, RAM);
    CHECK(!std::strcmp(pools, "imports 2100, captures 512, capture spares 512, staging 64, others 0"));
    CHECK(!std::strstr(line, "nothing") && !std::strstr(line, "dead images") && !std::strstr(line, "scratch"));

    // the line
    CHECK(n == std::strlen(line));
    if (std::strcmp(line, kLine)) {
        std::printf("FAIL: the line is\n%s\nnot\n%s\n", line, kLine);
        return 1;
    }
    format(line, sizeof line, t, 7802 * MB, 4000 * MB, true, 18200 * MB);
    CHECK(std::strstr(line, "(4000 MB by the driver, -121 MB unaccounted)"));
    // (no count from the driver: the allocator's own is printed, and no difference is made of it)
    format(line, sizeof line, t, 7802 * MB, 4000 * MB, false, 18200 * MB);
    CHECK(std::strstr(line, "VRAM 4121 of 7802 MB (4000 MB by the driver, unaccounted n/a): textures 3480,"));
    CHECK(!std::strstr(line, "MB unaccounted"));

    // no pool, or none that holds anything: "none", and sums of 0
    const Registry empty{};
    const Totals none = collect(empty);
    CHECK(none.n == 0 && none.sum.held[VRAM] == 0 && none.sum.held[RAM] == 0);
    format(line, sizeof line, none, 0, 0, true, 5 * MB);
    CHECK(!std::strcmp(line, "vk: budget: VRAM 0 of 0 MB (0 MB by the driver, 0 MB unaccounted): none; RAM 0 MB (the process's private "
                             "memory 5 MB): none"));
    Registry idle;
    CHECK(idle.add({"nothing", nothing}));
    format_pools(pools, sizeof pools, collect(idle), VRAM);
    CHECK(!std::strcmp(pools, "none"));

    // a buffer too small: cut short, ended, and never written past
    char small[32];
    std::memset(small, '#', sizeof small);
    const size_t cut = format(small, 24, t, 7802 * MB, 4500 * MB, true, 18200 * MB);
    CHECK(cut == 23 && std::strlen(small) == 23 && !std::strncmp(small, kLine, 23) && small[24] == '#');
    std::memset(small, '#', sizeof small);
    const size_t few = format_pools(small, 12, t, VRAM);
    CHECK(few == std::strlen(small) && few <= 11 && !std::strncmp(small, "textures 3480, target pool 512", few) && small[12] == '#');
    CHECK(format(small, 0, t, 0, 0, true, 0) == 0);

    check_arbiter();
    std::printf("PASS: the memory accountant's sums, order and line, and the arbiter's lines and order\n");
    return 0;
}
