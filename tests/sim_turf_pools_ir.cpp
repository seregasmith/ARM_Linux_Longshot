// SPDX-License-Identifier: GPL-3.0-or-later
// Simulated turf's lists made larger (ir_opt/sim_turf_pool_pass.h) on synthetic programs shaped as the game's: the blade list's producer
// (d5a9475f2790532e), the pools' producer (3de6d86ec7e6720e) and a mesh program drawing a pool. Each is checked made larger: its
// caps derived from what the bound buffers hold, its record accesses moved to a private descriptor (shader_info.h IsSimTurfStorage),
// placed before the tables, the headers left where the game has them; pool 2's records written at the same place in the new
// buffer as in the game's (the addresses are worked out here, before and after). Programs that differ a little are left exactly
// as they are. And the switch, through vk_shader.cpp's own sim_turf_pools (cut out by tests/test_sim_turf_pools.py): `on` runs the
// pass, `off` (RX_VK_SIM_TURF_POOLS=0) leaves every program as it is.
//
// sim_turf_pools_ir on|off; tests/test_sim_turf_pools.py builds and runs it.
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/ir_opt/sim_turf_pool_pass.h"

using namespace Shader;
namespace GP = Shader::Optimization;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

// (stand-ins for what vk_shader.cpp's sim_turf_pools calls besides the pass)
static std::vector<std::string> logged;
static int dce_runs = 0;
static void sim_turf_log(const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    logged.emplace_back(line);
}
namespace Shader::Optimization {
void DeadCodeEliminationPass(IR::Program&) { dce_runs++; }
}
#define logf sim_turf_log
#include "sim_turf_pools_cut.inc"   // vk_shader.cpp: sim_turf_pools_on, sim_turf_pools
#undef logf

constexpr u32 kMask = 0xffffffc0u;
// (the constant bank 0 words the programs read: the lists' addresses, low and high halves)
const std::map<u32, u32> kC0 = {{0x40, 0x10000000u}, {0x44, 0x2u}, {0x50, 0x10400000u}, {0x54, 0x2u},
                                {0x60, 0x20000000u}, {0x64, 0x3u}, {0x70, 0x30000000u}, {0x74, 0x3u}};
constexpr u32 kCount = 5;   // what every atomic and every other unknown returns, for the addresses worked out below
constexpr u32 kBladeRuntimeCap = 200000, kBladeInputRuntimeCap = 300000;
constexpr u32 kPool2RuntimeCap = 250000, kPool1RuntimeCap = 180000;

struct Prog {
    ObjectPool<IR::Inst> insts{512};
    ObjectPool<IR::Block> blocks{4};
    IR::Program program;
    IR::Block* block = nullptr;
    Prog(Stage stage, std::vector<StorageBufferDescriptor> descs) {
        block = blocks.Create(insts);
        program.blocks.push_back(block);
        program.post_order_blocks.push_back(block);
        program.stage = stage;
        for (const auto& d : descs) program.info.storage_buffers_descriptors.push_back(d);
    }
    IR::Inst* E(IR::Opcode op, std::initializer_list<IR::Value> args) { return &*block->PrependNewInst(block->end(), op, args); }
    static IR::Value V(u32 v) { return IR::Value{v}; }
    static IR::Value V(IR::Inst* i) { return IR::Value{i}; }
    IR::Inst* Cbuf(u32 off) { return E(IR::Opcode::GetCbufU32, {V(0u), V(off)}); }
    IR::Inst* Base(u32 off) { return E(IR::Opcode::BitwiseAnd32, {V(Cbuf(off)), V(kMask)}); }
    /// A header access's offset, as the storage buffer pass writes it: ISub32(c0 word + at, c0 word & mask)
    IR::Inst* Header(u32 off, u32 at) {
        IR::Inst* x = Cbuf(off);
        if (at) x = E(IR::Opcode::IAdd32, {V(x), V(at)});
        return E(IR::Opcode::ISub32, {V(x), V(Base(off))});
    }
    /// A record's: ISub32(c0 word + index (+ extra), c0 word & mask)
    IR::Inst* Record(u32 off, IR::Inst* index, u32 extra) {
        IR::Inst* x = E(IR::Opcode::IAdd32, {V(Cbuf(off)), V(index)});
        if (extra) x = E(IR::Opcode::IAdd32, {V(x), V(extra)});
        return E(IR::Opcode::ISub32, {V(x), V(Base(off))});
    }
    std::vector<IR::Inst*> All(IR::Opcode op) const {
        std::vector<IR::Inst*> r;
        for (IR::Inst& i : block->Instructions())
            if (i.GetOpcode() == op) r.push_back(&i);
        return r;
    }
    const std::vector<StorageBufferDescriptor> Descs() const {
        return {program.info.storage_buffers_descriptors.begin(), program.info.storage_buffers_descriptors.end()};
    }
};

/// The value an instruction computes, for the few opcodes these programs use (an atomic or a load: kCount)
u64 Eval(IR::Value v) {
    v = v.Resolve();
    if (v.IsImmediate()) return v.Type() == IR::Type::U64 ? v.U64() : v.U32();
    const IR::Inst* i = v.InstRecursive();
    const auto a = [i](size_t n) { return Eval(i->Arg(n)); };
    switch (i->GetOpcode()) {
    case IR::Opcode::GetCbufU32: return a(0) == 0 ? kC0.at(static_cast<u32>(a(1))) : kCount;
    case IR::Opcode::GetCbufU32x2: return kC0.at(static_cast<u32>(a(1))) | u64{kC0.at(static_cast<u32>(a(1)) + 4)} << 32;
    case IR::Opcode::CompositeExtractU32x2: return a(1) == 0 ? a(0) & 0xffffffffu : a(0) >> 32;
    case IR::Opcode::CompositeConstructU32x2: return (a(0) & 0xffffffffu) | (a(1) << 32);
    case IR::Opcode::PackUint2x32: return a(0);
    case IR::Opcode::IAdd32: return (a(0) + a(1)) & 0xffffffffu;
    case IR::Opcode::ISub32: return (a(0) - a(1)) & 0xffffffffu;
    case IR::Opcode::IMul32: return (a(0) * a(1)) & 0xffffffffu;
    case IR::Opcode::BitwiseAnd32: return a(0) & a(1);
    case IR::Opcode::UMin32: return std::min(a(0), a(1));
    case IR::Opcode::UMax32: return std::max(a(0), a(1));
    case IR::Opcode::UDiv32: return a(1) ? a(0) / a(1) : 0;
    case IR::Opcode::StorageBufferLength:
        switch (a(0)) {
        case 1: return 64 + kPool2RuntimeCap * 48;      // a mesh reader's private pool
        case 2: return 64 + kBladeRuntimeCap * 8;       // the blade producer's private list
        case 3: return 64 + kBladeInputRuntimeCap * 8;  // the pool producer's private input
        case 4: return 64 + kPool2RuntimeCap * 48;      // the pool producer's first private output
        case 5: return 64 + kPool1RuntimeCap * 48;      // the pool producer's second private output
        default: return 0;
        }
    case IR::Opcode::IAdd64: return a(0) + a(1);
    default: return kCount;
    }
}

bool IsPrivate(const StorageBufferDescriptor& d, u32 orig, bool pool, bool creator) {
    return IsSimTurfStorage(d) && SimTurfStorageOrig(d) == orig && ((d.cbuf_offset & kSimTurfStoragePool) != 0) == pool &&
           ((d.cbuf_offset & kSimTurfStorageCreator) != 0) == creator && d.is_written == creator;
}

// ---- d5a9: the blade list's producer ----
enum class Blade { Game, ThirdWrite, OneGuard, CountNotCapped, TableUsed };
struct BladeProg : Prog {
    IR::Inst *cap, *umax, *bump, *load, *g1, *g2, *w1, *w2;
    explicit BladeProg(Blade s)
        : Prog(Stage::Compute, {{0, 0x40, 1, true}, {0, 0x50, 1, false}, {0, 0x100, 4, false}}) {
        using O = IR::Opcode;
        IR::Inst* demand = E(O::GetCbufU32, {V(3u), V(0x180u)});
        cap = E(O::UMin32, {V(demand), V(GP::kSimTurfBladeCap)});
        IR::Inst* groups = E(O::IAdd32, {V(s == Blade::CountNotCapped ? demand : cap), V(63u)});
        if (s == Blade::CountNotCapped) E(O::IAdd32, {V(cap), V(1u)});
        umax = E(O::StorageAtomicUMax32, {V(0u), V(Header(0x40, 0)), V(groups)});
        bump = E(O::StorageAtomicIAdd32, {V(0u), V(Header(0x40, 4)), V(1u)});
        load = E(O::LoadStorage32, {V(1u), V(Record(0x50, bump, 0)), });
        g1 = E(O::Identity, {V(GP::kSimTurfBladeCap - 1)});
        IR::Inst* i1 = E(O::ISub32, {V(g1), V(bump)});
        w1 = E(O::WriteStorage32, {V(0u), V(Record(0x40, i1, 0)), V(load)});
        g2 = s == Blade::OneGuard ? nullptr : E(O::Identity, {V(GP::kSimTurfBladeCap - 1)});
        IR::Inst* i2 = E(O::ISub32, {V(g2 ? g2 : g1), V(bump)});
        w2 = E(O::WriteStorage32, {V(0u), V(Record(0x40, i2, 4)), V(load)});
        if (s == Blade::ThirdWrite) E(O::WriteStorage32, {V(0u), V(Record(0x40, i2, 8)), V(load)});
        if (s == Blade::TableUsed) E(O::LoadStorage32, {V(2u), V(Record(0x100, bump, 0))});
    }
    bool Untouched() const {
        return Descs().size() == 3 && cap->Arg(1).U32() == GP::kSimTurfBladeCap && g1->Arg(0).U32() == GP::kSimTurfBladeCap - 1 &&
               (!g2 || g2->Arg(0).U32() == GP::kSimTurfBladeCap - 1) && w1->Arg(0).U32() == 0 && w2->Arg(0).U32() == 0;
    }
    bool MadeLarger() const {
        const auto d = Descs();
        return d.size() == 4 && IsPrivate(d[2], 0x40, false, true) && d[3].cbuf_offset == 0x100 && d[3].count == 4 &&
               Eval(cap->Arg(1)) == kBladeRuntimeCap && Eval(g1->Arg(0)) == kBladeRuntimeCap - 1 &&
               Eval(g2->Arg(0)) == kBladeRuntimeCap - 1 && All(IR::Opcode::StorageBufferLength).size() == 3 &&
               w1->Arg(0).U32() == 2 && w2->Arg(0).U32() == 2 &&
               umax->Arg(0).U32() == 0 && bump->Arg(0).U32() == 0 && load->Arg(0).U32() == 1;
    }
};

// ---- 3de6: the pools' producer ----
enum class Pool { Game, NineWrites, GuardAfter, SharedCap, BladeHeaderTwice };
struct PoolProg : Prog {
    IR::Inst *count_in, *blade_a, *blade_b, *in_guard, *cap2, *cap1, *g0, *g1;
    std::vector<u64> pool2_addresses;   // the game's addresses of pool 2's records, in program order
    explicit PoolProg(Pool s) : Prog(Stage::Compute, {{0, 0x40, 1, false}, {0, 0x60, 1, true}, {0, 0x70, 1, true}}) {
        using O = IR::Opcode;
        count_in = E(O::LoadStorage32, {V(0u), V(Header(0x40, 12))});
        if (s == Pool::BladeHeaderTwice) E(O::LoadStorage32, {V(0u), V(Header(0x40, 12))});
        in_guard = E(O::Identity, {V(GP::kSimTurfBladeCap - 1)});
        IR::Inst* i = E(O::ISub32, {V(in_guard), V(E(O::GetCbufU32, {V(3u), V(0u)}))});
        blade_a = E(O::LoadStorage32, {V(0u), V(Record(0x40, i, 0))});
        blade_b = E(O::LoadStorage32, {V(0u), V(Record(0x40, i, 4))});
        IR::Inst* n2 = E(O::StorageAtomicIAdd32, {V(1u), V(Header(0x60, 4)), V(1u)});
        cap2 = E(O::UMin32, {V(n2), V(GP::kSimTurfPoolCap)});
        E(O::StorageAtomicUMax32, {V(1u), V(Header(0x60, 0)), V(E(O::IAdd32, {V(cap2), V(31u)}))});
        IR::Inst* n1 = E(O::StorageAtomicIAdd32, {V(2u), V(Header(0x70, 4)), V(1u)});
        cap1 = E(O::UMin32, {V(n1), V(GP::kSimTurfPoolCap)});
        E(O::StorageAtomicUMax32, {V(2u), V(Header(0x70, 0)), V(E(O::IAdd32, {V(s == Pool::SharedCap ? cap2 : cap1), V(31u)}))});
        if (s != Pool::GuardAfter) g0 = E(O::Identity, {V(GP::kSimTurfPoolCap)});
        // pool 2's ten records: 48 bytes each, the slot from the counter; two through IAdd64(address, #8), one 64-bit
        IR::Inst* slot2 = E(O::IMul32, {V(n2), V(48u)});
        for (u32 k = 0; k < (s == Pool::NineWrites ? 9u : 10u); k++) {
            IR::Inst* at = E(O::IAdd32, {V(slot2), V(k < 8 ? 4 * k : 0u)});
            IR::Inst* lo;
            if (k == 1) lo = E(O::IAdd32, {V(E(O::CompositeExtractU32x2, {V(E(O::GetCbufU32x2, {V(0u), V(0x60u)})), V(0u)})), V(at)});
            else if (k == 5) lo = E(O::IAdd32, {V(at), V(Cbuf(0x60))});
            else lo = E(O::IAdd32, {V(Cbuf(0x60)), V(at)});
            IR::Inst* a = E(O::PackUint2x32, {V(E(O::CompositeConstructU32x2, {V(lo), V(Cbuf(0x64))}))});
            if (k >= 8) a = E(O::IAdd64, {V(a), IR::Value{u64{8 + 4 * (k - 8)}}});
            if (k == 3) E(O::WriteGlobal64, {V(a), V(E(O::CompositeConstructU32x2, {V(blade_a), V(blade_b)}))});
            else E(O::WriteGlobal32, {V(a), V(k & 1 ? blade_b : blade_a)});
            pool2_addresses.push_back(Eval(V(a)));
            if (s == Pool::GuardAfter && k == 0) g0 = E(O::Identity, {V(GP::kSimTurfPoolCap)});
        }
        g1 = E(O::Identity, {V(GP::kSimTurfPoolCap)});
        // pool 1's ten, after its guard
        IR::Inst* slot1 = E(O::IMul32, {V(E(O::ISub32, {V(g1), V(n1)})), V(48u)});
        for (u32 k = 0; k < 10; k++) {
            IR::Inst* lo = E(O::IAdd32, {V(Cbuf(0x70)), V(E(O::IAdd32, {V(slot1), V(4 * k)}))});
            E(O::WriteGlobal32, {V(E(O::PackUint2x32, {V(E(O::CompositeConstructU32x2, {V(lo), V(Cbuf(0x74))}))})), V(blade_a)});
        }
    }
    int GlobalWrites(u32 base) {
        int n = 0;
        for (IR::Opcode op : {IR::Opcode::WriteGlobal32, IR::Opcode::WriteGlobal64})
            for (IR::Inst* w : All(op))
                if ((Eval(w->Arg(0)) >> 32) == kC0.at(base + 4) && (Eval(w->Arg(0)) & 0xf0000000u) == kC0.at(base)) n++;
        return n;
    }
    bool Untouched() {
        return Descs().size() == 3 && cap2->Arg(1).U32() == GP::kSimTurfPoolCap && cap1->Arg(1).U32() == GP::kSimTurfPoolCap &&
               g0->Arg(0).U32() == GP::kSimTurfPoolCap && g1->Arg(0).U32() == GP::kSimTurfPoolCap &&
               in_guard->Arg(0).U32() == GP::kSimTurfBladeCap - 1 && blade_a->Arg(0).U32() == 0 && blade_b->Arg(0).U32() == 0 &&
               GlobalWrites(0x60) == static_cast<int>(pool2_addresses.size()) && GlobalWrites(0x70) == 10 &&
               All(IR::Opcode::WriteStorage32).empty() && All(IR::Opcode::WriteStorage64).empty();
    }
    bool MadeLarger() {
        const auto d = Descs();
        CHECK(d.size() == 6 && IsPrivate(d[3], 0x40, false, false) && IsPrivate(d[4], 0x60, true, true) &&
              IsPrivate(d[5], 0x70, true, true));
        CHECK(blade_a->Arg(0).U32() == 3 && blade_b->Arg(0).U32() == 3 && count_in->Arg(0).U32() == 0);
        CHECK(Eval(cap2->Arg(1)) == kPool2RuntimeCap && Eval(cap1->Arg(1)) == kPool1RuntimeCap);
        CHECK(Eval(g0->Arg(0)) == kPool2RuntimeCap && Eval(g1->Arg(0)) == kPool1RuntimeCap);
        CHECK(Eval(in_guard->Arg(0)) == kBladeInputRuntimeCap - 1);
        CHECK(All(IR::Opcode::StorageBufferLength).size() == 5);
        CHECK(GlobalWrites(0x60) == 0 && GlobalWrites(0x70) == 0);
        // pool 2's records in the private pool, each at its place in the game's: its address less the pool's base
        std::vector<u64> written;
        size_t wide = 0;
        for (IR::Inst& w : block->Instructions()) {
            const IR::Opcode op = w.GetOpcode();
            if (op != IR::Opcode::WriteStorage32 && op != IR::Opcode::WriteStorage64) continue;
            CHECK(w.Arg(0).U32() == 4 || w.Arg(0).U32() == 5);
            const u32 base = w.Arg(0).U32() == 4 ? 0x60 : 0x70;
            written.push_back(Eval(w.Arg(1)) + (u64{kC0.at(base + 4)} << 32 | kC0.at(base)));
            wide += op == IR::Opcode::WriteStorage64;
        }
        CHECK(std::equal(pool2_addresses.begin(), pool2_addresses.end(), written.begin()));
        CHECK(written.size() == 20 && wide == 1);
        return true;
    }
};

// ---- a mesh program drawing a pool ----
enum class Reader { Game, TwoGuards, Writes, NotMesh };
struct ReaderProg : Prog {
    IR::Inst *count, *guard, *r1, *r2;
    explicit ReaderProg(Reader s) : Prog(s == Reader::NotMesh ? Stage::Fragment : Stage::Mesh, {{0, 0x60, 1, false}}) {
        using O = IR::Opcode;
        count = E(O::LoadStorage32, {V(0u), V(Header(0x60, 12))});
        guard = E(O::Identity, {V(GP::kSimTurfPoolCap)});
        if (s == Reader::TwoGuards) E(O::Identity, {V(GP::kSimTurfPoolCap)});
        IR::Inst* slot = E(O::IMul32, {V(E(O::ISub32, {V(guard), V(count)})), V(48u)});
        r1 = E(O::LoadStorage32, {V(0u), V(Record(0x60, slot, 0))});
        r2 = E(O::LoadStorage64, {V(0u), V(Record(0x60, slot, 16))});
        if (s == Reader::Writes) E(O::WriteStorage32, {V(0u), V(Record(0x60, slot, 4)), V(r1)});
    }
    bool Untouched() const {
        return Descs().size() == 1 && guard->Arg(0).U32() == GP::kSimTurfPoolCap && r1->Arg(0).U32() == 0 && r2->Arg(0).U32() == 0;
    }
    bool MadeLarger() const {
        const auto d = Descs();
        return d.size() == 2 && IsPrivate(d[1], 0x60, true, false) && Eval(guard->Arg(0)) == kPool2RuntimeCap &&
               All(IR::Opcode::StorageBufferLength).size() == 1 &&
               r1->Arg(0).U32() == 1 && r2->Arg(0).U32() == 1 && count->Arg(0).U32() == 0;
    }
};

void ThePass() {
    {   // d5a9
        BladeProg p{Blade::Game};
        CHECK(GP::SimTurfPools(p.program, GP::kSimTurfBladeKey) == "blade list made larger");
        CHECK(p.MadeLarger());
    }
    for (const Blade s : {Blade::ThirdWrite, Blade::OneGuard, Blade::CountNotCapped, Blade::TableUsed}) {
        BladeProg p{s};
        CHECK(GP::SimTurfPools(p.program, GP::kSimTurfBladeKey).starts_with("blade list left as it is: "));
        CHECK(p.Untouched());
    }
    {   // another program of the same shape: not simulated turf's
        BladeProg p{Blade::Game};
        CHECK(GP::SimTurfPools(p.program, GP::kSimTurfBladeKey ^ 1).empty());
        CHECK(p.Untouched());
    }
    {   // the other cached phase-0 key uses the same pass
        BladeProg p{Blade::Game};
        CHECK(GP::SimTurfPools(p.program, 0xbdcc9ddf7ca7cabdULL) == "blade list made larger");
        CHECK(p.MadeLarger());
    }
    {   // 3de6
        PoolProg p{Pool::Game};
        CHECK(p.Untouched());
        CHECK(GP::SimTurfPools(p.program, GP::kSimTurfPoolKey) == "both pools made larger");
        CHECK(p.MadeLarger());
    }
    for (const u64 key : {0x64978274654e224cULL, 0x9a6fe9d7a7ce2007ULL, 0xb0cc49ac253104ccULL}) {
        PoolProg p{Pool::Game};
        CHECK(GP::SimTurfPools(p.program, key) == "both pools made larger");
        CHECK(p.MadeLarger());
    }
    for (const Pool s : {Pool::NineWrites, Pool::GuardAfter, Pool::SharedCap, Pool::BladeHeaderTwice}) {
        PoolProg p{s};
        CHECK(GP::SimTurfPools(p.program, GP::kSimTurfPoolKey).starts_with("pools left as they are: "));
        CHECK(p.Untouched());
    }
    {   // the mesh reader
        ReaderProg p{Reader::Game};
        CHECK(GP::SimTurfPools(p.program, 0x1234) == "pool reader made larger");
        CHECK(p.MadeLarger());
    }
    for (const Reader s : {Reader::TwoGuards, Reader::Writes, Reader::NotMesh}) {
        ReaderProg p{s};
        CHECK(GP::SimTurfPools(p.program, 0x1234).empty());
        CHECK(p.Untouched());
    }
    std::printf("pass: both phase-0 keys, all four phase-1 keys and the mesh reader use runtime capacities; 11 programs that differ left as they are\n");
}

/// vk_shader.cpp's sim_turf_pools, the way translation calls it: on, the three made larger and logged; off, all left as they are
void TheSwitch(bool on) {
    BladeProg blade{Blade::Game};
    PoolProg pool{Pool::Game};
    ReaderProg reader{Reader::Game};
    sim_turf_pools(blade.program, GP::kSimTurfBladeKey);
    sim_turf_pools(pool.program, GP::kSimTurfPoolKey);
    sim_turf_pools(reader.program, 0x1234);
    CHECK(sim_turf_pools_on() == on);
    if (on) {
        CHECK(blade.MadeLarger() && pool.MadeLarger() && reader.MadeLarger());
        CHECK(dce_runs == 3 && logged.size() == 3);
        CHECK(logged[0] == "vk: simulated turf: program d5a9475f2790532e: blade list made larger");
        CHECK(logged[1] == "vk: simulated turf: program 3de6d86ec7e6720e: both pools made larger");
        CHECK(logged[2] == "vk: simulated turf: program 0000000000001234: pool reader made larger");
    } else {
        CHECK(blade.Untouched() && pool.Untouched() && reader.Untouched());
        CHECK(dce_runs == 0 && logged.empty());
    }
    std::printf("switch %s: %s\n", on ? "on" : "off (RX_VK_SIM_TURF_POOLS=0)", on ? "made larger, logged" : "left as they are");
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    CHECK(mode == "on" || mode == "off");
    ThePass();
    TheSwitch(mode == "on");
    std::printf("PASS\n");
    return 0;
}
