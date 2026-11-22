// SPDX-License-Identifier: GPL-3.0-or-later
// (longshot) The large-scale simulated turf's lists moved into renderer-owned buffers so high-demand area settings do
// not overflow the game's fixed record storage. Grass distance (#1382) only changes reach/fade uniforms for separate
// short decorative blades and never binds these lists; flowers, shrubs and other foliage use other paths again.
//
// The game builds Hyrule Field's simulated turf on the GPU each frame: a compute program (d5a9475f2790532e) lists the blades in reach,
// 143360 at most, 8 bytes each; a second one (3de6d86ec7e6720e) sorts them into two pools of 138240 records of 48 bytes, which
// mesh programs draw. Past 2x, more blades are in reach than the lists hold: which ones make it in changes from frame to frame
// (the atomic counter's order), and whole turf patches come and go, even standing still. The lists are the game's own
// buffers, sized for its caps: raising a cap writes past them, over whatever comes next.
//
// So the records go to adaptive buffers of their own on the GPU. Each list's 64-byte header (its counters, the indirect
// arguments) stays where the game has it: the game resets it and the draws read their counts from it. Only the records move:
//  - the producers' record writes go through a new storage descriptor, a "private" one: the back end binds a buffer of its own
//    for it, found by the guest address the original descriptor names (vk_draw.cpp sim_turf_private_binding);
//  - the readers' record loads go through another, which the back end binds to that buffer when there is one, and to the
//    game's own otherwise;
//  - every cap comes from the descriptor's runtime length, so allocation failure and live resize stay memory-safe.
// After a producer dispatch, the back end copies the stock-sized compatible window into the game's record storage. A reader
// variant this pass cannot patch therefore still sees a valid bounded list instead of records written only to the private one.
// The producers are found by key; the mesh readers by what they do (their keys mix in the draw's state: vk_shader.cpp mesh_key),
// including both cached phase-0 keys and all four observed phase-1 keys.
// Every edit is checked first; a program that does not look exactly as expected is left as it is.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::Optimization {

inline constexpr u64 kSimTurfBladeKey = 0xd5a9475f2790532eULL;   // the blade list's producer
inline constexpr u64 kSimTurfPoolKey = 0x3de6d86ec7e6720eULL;    // the pools' producer, the blade list's reader
inline constexpr u64 kSimTurfBladeKeys[] = {kSimTurfBladeKey, 0xbdcc9ddf7ca7cabdULL};
inline constexpr u64 kSimTurfPoolKeys[] = {kSimTurfPoolKey, 0x64978274654e224cULL, 0x9a6fe9d7a7ce2007ULL,
                                         0xb0cc49ac253104ccULL};
inline constexpr u32 kSimTurfBladeCap = 143360;   // records of 8 bytes
inline constexpr u32 kSimTurfPoolCap = 138240;    // records of 48 bytes
inline constexpr u32 kSimTurfListHeader = 64;                                 // bytes before a list's records

namespace sim_turf_detail {

/// The constant-bank word an address's low half comes from: c0+offset, through GetCbufU32 or GetCbufU32x2's first half
inline std::optional<u32> CbufLo(IR::Value v) {
    v = v.Resolve();
    if (v.IsImmediate()) return std::nullopt;
    const IR::Inst* inst = v.InstRecursive();
    const auto c0_imm = [](const IR::Inst* g) -> std::optional<u32> {
        const IR::Value bank = g->Arg(0).Resolve(), off = g->Arg(1).Resolve();
        if (!bank.IsImmediate() || bank.U32() != 0 || !off.IsImmediate()) return std::nullopt;
        return off.U32();
    };
    if (inst->GetOpcode() == IR::Opcode::GetCbufU32) return c0_imm(inst);
    if (inst->GetOpcode() == IR::Opcode::CompositeExtractU32x2) {
        const IR::Value idx = inst->Arg(1).Resolve();
        const IR::Value g = inst->Arg(0).Resolve();
        if (!idx.IsImmediate() || idx.U32() != 0 || g.IsImmediate()) return std::nullopt;
        if (g.InstRecursive()->GetOpcode() == IR::Opcode::GetCbufU32x2) return c0_imm(g.InstRecursive());
    }
    return std::nullopt;
}

/// A storage access's offset, as the storage buffer pass writes it: ISub32(address low half, BitwiseAnd32(c0 word, mask)).
/// A header access's address is the c0 word plus constants; a record's anything else
struct Offset {
    bool ok = false;
    u32 orig = 0;      // the descriptor's c0 offset the base comes from
    u32 mask = 0;
    bool header = false;
    u32 header_at = 0;
};
inline Offset ClassifyOffset(IR::Value v) {
    Offset o;
    v = v.Resolve();
    if (v.IsImmediate()) return o;
    const IR::Inst* sub = v.InstRecursive();
    if (sub->GetOpcode() != IR::Opcode::ISub32) return o;
    const IR::Value m = sub->Arg(1).Resolve();
    if (m.IsImmediate() || m.InstRecursive()->GetOpcode() != IR::Opcode::BitwiseAnd32) return o;
    const IR::Inst* and_inst = m.InstRecursive();
    const IR::Value mask = and_inst->Arg(1).Resolve();
    const std::optional<u32> orig = CbufLo(and_inst->Arg(0));
    if (!mask.IsImmediate() || !orig) return o;
    IR::Value x = sub->Arg(0).Resolve();
    u32 acc = 0;
    while (!x.IsImmediate() && x.InstRecursive()->GetOpcode() == IR::Opcode::IAdd32 &&
           x.InstRecursive()->Arg(1).Resolve().IsImmediate()) {
        acc += x.InstRecursive()->Arg(1).Resolve().U32();
        x = x.InstRecursive()->Arg(0).Resolve();
    }
    const std::optional<u32> lo = CbufLo(x);
    o.ok = true, o.orig = *orig, o.mask = mask.U32();
    o.header = lo && *lo == *orig;
    o.header_at = o.header ? acc : 0;
    return o;
}

enum class Kind { Load, Write, Atomic, Other };
inline Kind StorageKind(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::LoadStorageU8: case IR::Opcode::LoadStorageS8: case IR::Opcode::LoadStorageU16:
    case IR::Opcode::LoadStorageS16: case IR::Opcode::LoadStorage32: case IR::Opcode::LoadStorage64:
    case IR::Opcode::LoadStorage128:
        return Kind::Load;
    case IR::Opcode::WriteStorageU8: case IR::Opcode::WriteStorageS8: case IR::Opcode::WriteStorageU16:
    case IR::Opcode::WriteStorageS16: case IR::Opcode::WriteStorage32: case IR::Opcode::WriteStorage64:
    case IR::Opcode::WriteStorage128:
        return Kind::Write;
    default:
        break;
    }
    const std::string_view name{IR::NameOf(op)};
    if (name.starts_with("StorageAtomic")) return Kind::Atomic;
    return Kind::Other;
}
inline IR::Type StorageType(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::LoadStorage64: case IR::Opcode::WriteStorage64: return IR::Type::U32x2;
    case IR::Opcode::LoadStorage128: case IR::Opcode::WriteStorage128: return IR::Type::U32x4;
    default: return IR::Type::U32;
    }
}

/// What a program does through one storage binding
struct Binding {
    std::vector<IR::Inst*> record_loads, record_writes;
    std::vector<IR::Inst*> header_loads, header_atomics;
    int header12_loads = 0;
    bool odd = false;   // an access of another form, or a record atomic, or another base
    IR::Inst* umax = nullptr;   // the header's group count (StorageAtomicUMax32 at +0)
};
inline std::vector<Binding> Bindings(IR::Program& program, const Info& info) {
    std::vector<Binding> b(info.storage_buffers_descriptors.size());
    for (IR::Block* block : program.blocks)
        for (IR::Inst& inst : block->Instructions()) {
            const Kind kind = StorageKind(inst.GetOpcode());
            if (kind == Kind::Other) continue;
            const IR::Value bv = inst.Arg(0).Resolve();
            if (!bv.IsImmediate() || bv.U32() >= b.size()) {
                for (Binding& x : b) x.odd = true;   // (a binding not known here: touch nothing)
                continue;
            }
            Binding& x = b[bv.U32()];
            const Offset o = ClassifyOffset(inst.Arg(1));
            if (!o.ok || o.orig != info.storage_buffers_descriptors[bv.U32()].cbuf_offset) {
                x.odd = true;
                continue;
            }
            if (o.header) {
                if (kind == Kind::Load) {
                    x.header_loads.push_back(&inst);
                    if (o.header_at == 12 && inst.GetOpcode() == IR::Opcode::LoadStorage32) x.header12_loads++;
                } else if (kind == Kind::Atomic) {
                    x.header_atomics.push_back(&inst);
                    if (inst.GetOpcode() == IR::Opcode::StorageAtomicUMax32 && o.header_at == 0) x.umax = &inst;
                } else {
                    x.odd = true;   // (a header write: not seen in these programs)
                }
            } else if (kind == Kind::Load) {
                x.record_loads.push_back(&inst);
            } else if (kind == Kind::Write) {
                x.record_writes.push_back(&inst);
            } else {
                x.odd = true;
            }
        }
    return b;
}

/// The instructions `Identity #imm` and `UMin32 x, #imm`, in program order
inline std::vector<IR::Inst*> Identities(IR::Program& program, u32 imm) {
    std::vector<IR::Inst*> r;
    for (IR::Block* block : program.blocks)
        for (IR::Inst& inst : block->Instructions())
            if (inst.GetOpcode() == IR::Opcode::Identity && inst.Arg(0).IsImmediate() && inst.Arg(0).U32() == imm)
                r.push_back(&inst);
    return r;
}
inline std::vector<IR::Inst*> UMins(IR::Program& program, u32 imm) {
    std::vector<IR::Inst*> r;
    for (IR::Block* block : program.blocks)
        for (IR::Inst& inst : block->Instructions())
            if (inst.GetOpcode() == IR::Opcode::UMin32 && inst.Arg(1).IsImmediate() && inst.Arg(1).U32() == imm) r.push_back(&inst);
    return r;
}
/// Whether `from`'s value is computed from `target` (through at most `depth` instructions, not through a phi)
inline bool ComesFrom(IR::Value from, const IR::Inst* target, int depth, std::vector<const IR::Inst*>& seen) {
    from = from.Resolve();
    if (from.IsImmediate()) return false;
    const IR::Inst* inst = from.InstRecursive();
    if (inst == target) return true;
    if (depth == 0 || inst->GetOpcode() == IR::Opcode::Phi) return false;
    if (std::find(seen.begin(), seen.end(), inst) != seen.end()) return false;
    seen.push_back(inst);
    for (size_t i = 0; i < inst->NumArgs(); i++)
        if (ComesFrom(inst->Arg(i), target, depth - 1, seen)) return true;
    return false;
}
inline bool ComesFrom(IR::Value from, const IR::Inst* target, int depth) {
    std::vector<const IR::Inst*> seen;
    return ComesFrom(from, target, depth, seen);
}

/// Index of each instruction in program order
inline size_t OrderOf(IR::Program& program, const IR::Inst* target) {
    size_t n = 0;
    for (IR::Block* block : program.blocks)
        for (IR::Inst& inst : block->Instructions()) {
            if (&inst == target) return n;
            n++;
        }
    return SIZE_MAX;
}

/// A new private descriptor (shader_info.h IsSimTurfStorage), before the tables (count > 1): the SPIR-V back end numbers its
/// single buffers by their place among the single ones (spirv_emit_context.cpp DefineSsbos), the IR by their place in the list,
/// which agree while the tables come last. Its index, or nullopt when the list is full or a binding past it is in use
inline std::optional<u32> AddPrivate(Info& info, u32 orig, bool pool, bool creator, u32 used_bindings) {
    auto& descs = info.storage_buffers_descriptors;
    if (descs.size() >= Info::MAX_SSBOS) return std::nullopt;
    size_t at = 0;
    while (at < descs.size() && descs[at].count <= 1) at++;
    if (used_bindings > at) return std::nullopt;
    descs.insert(descs.begin() + static_cast<std::ptrdiff_t>(at),
                 StorageBufferDescriptor{
                     .cbuf_index = 0,
                     .cbuf_offset = SimTurfStorageOffset(orig, pool, creator),
                     .count = 1,
                     .is_written = creator,
                 });
    return static_cast<u32>(at);
}
inline u32 UsedBindings(IR::Program& program) {
    u32 n = 0;
    for (IR::Block* block : program.blocks)
        for (IR::Inst& inst : block->Instructions())
            if (StorageKind(inst.GetOpcode()) != Kind::Other && inst.Arg(0).IsImmediate()) n = std::max(n, inst.Arg(0).U32() + 1);
    return n;
}
inline void Redirect(Info& info, const std::vector<IR::Inst*>& insts, u32 binding) {
    for (IR::Inst* inst : insts) {
        inst->SetArg(0, IR::Value{binding});
        info.used_storage_buffer_types |= StorageType(inst->GetOpcode());
    }
}

/// The number of complete records in a bound private descriptor. Its range includes the guest address's offset from the
/// storage-aligned base and the 64-byte header before the records; subtract both at run time. A fallback descriptor for the
/// game's own buffer therefore restores its real cap, while a renderer-owned descriptor reports exactly what was allocated.
inline IR::Value RuntimeCapacity(IR::Program& program, IR::Inst* before, u32 binding, u32 orig, u32 mask, u32 stride,
                                 bool minus_one = false) {
    IR::Block* owner = nullptr;
    for (IR::Block* block : program.blocks)
        for (IR::Inst& inst : block->Instructions())
            if (&inst == before) owner = block;
    if (!owner) return IR::Value{0u};
    const IR::Block::iterator at = owner->Instructions().iterator_to(*before);
    const auto add = [&](IR::Opcode op, std::initializer_list<IR::Value> args) -> IR::Value {
        return IR::Value{&*owner->PrependNewInst(at, op, args)};
    };
    const IR::Value length = add(IR::Opcode::StorageBufferLength, {IR::Value{binding}});
    const IR::Value low = add(IR::Opcode::GetCbufU32, {IR::Value{0u}, IR::Value{orig}});
    const IR::Value unaligned = add(IR::Opcode::BitwiseAnd32, {low, IR::Value{~mask}});
    const IR::Value prefix = add(IR::Opcode::IAdd32, {unaligned, IR::Value{kSimTurfListHeader}});
    const IR::Value at_least_prefix = add(IR::Opcode::UMax32, {length, prefix});
    const IR::Value bytes = add(IR::Opcode::ISub32, {at_least_prefix, prefix});
    IR::Value cap = add(IR::Opcode::UDiv32, {bytes, IR::Value{stride}});
    if (minus_one) {
        cap = add(IR::Opcode::UMax32, {cap, IR::Value{1u}});
        cap = add(IR::Opcode::ISub32, {cap, IR::Value{1u}});
    }
    program.info.used_storage_buffer_types |= IR::Type::U32;
    return cap;
}

/// Phase 0: its two record writes go to a private list whose descriptor length supplies the cap.
inline std::string PatchBladeProducer(IR::Program& program) {
    Info& info = program.info;
    std::vector<Binding> b = Bindings(program, info);
    int list = -1;
    for (size_t i = 0; i < b.size(); i++)
        if (!b[i].record_writes.empty()) {
            if (list != -1) return "two bindings written";
            list = static_cast<int>(i);
        }
    if (list < 0) return "no records written";
    const Binding& x = b[list];
    if (x.odd || x.record_writes.size() != 2 || !x.record_loads.empty() || !x.umax) return "the list's accesses are not as expected";
    const std::vector<IR::Inst*> umin = UMins(program, kSimTurfBladeCap);
    const std::vector<IR::Inst*> guards = Identities(program, kSimTurfBladeCap - 1);
    if (umin.size() != 1 || guards.size() != 2) return "caps not as expected";
    if (!ComesFrom(x.umax->Arg(2), umin[0], 16)) return "the group count does not come from the cap";
    const Offset records = ClassifyOffset(x.record_writes[0]->Arg(1));
    if (!records.ok) return "the list's record offset is not as expected";
    const auto at = AddPrivate(info, info.storage_buffers_descriptors[list].cbuf_offset, false, true, UsedBindings(program));
    if (!at) return "no room for a descriptor";
    Redirect(info, x.record_writes, *at);
    umin[0]->SetArg(1, RuntimeCapacity(program, umin[0], *at, records.orig, records.mask, 8));
    for (IR::Inst* g : guards)
        g->SetArg(0, RuntimeCapacity(program, g, *at, records.orig, records.mask, 8, true));
    return {};
}

/// Phase 1 reads the blade list and writes two independent record pools (ten WriteGlobal operations through c0+0x60 and
/// c0+0x70). Both get private descriptors; their runtime lengths become independent caps, so only a saturated pool grows.
inline std::string PatchPoolProducer(IR::Program& program) {
    Info& info = program.info;
    const auto original_descs = info.storage_buffers_descriptors;
    std::vector<Binding> b = Bindings(program, info);
    int input = -1;
    for (size_t i = 0; i < b.size(); i++)
        if (!b[i].record_loads.empty()) {
            if (input != -1) return "two bindings read";
            input = static_cast<int>(i);
        }
    if (input < 0) return "no records read";
    if (b[input].odd || !b[input].record_writes.empty() || !b[input].header_atomics.empty() || b[input].header12_loads != 1)
        return "the blade list's accesses are not as expected";
    // the pools: the bindings with header atomics, at c0+0x60 (pool 2) and c0+0x70 (pool 1)
    int pool2 = -1, pool1 = -1;
    for (size_t i = 0; i < b.size(); i++) {
        const u32 off = info.storage_buffers_descriptors[i].cbuf_offset;
        if (info.storage_buffers_descriptors[i].cbuf_index != 0 || info.storage_buffers_descriptors[i].count != 1) continue;
        if (off == 0x60) pool2 = static_cast<int>(i);
        if (off == 0x70) pool1 = static_cast<int>(i);
    }
    if (pool2 < 0 || pool1 < 0) return "pools not found";
    const Binding& p2 = b[pool2];
    const Binding& p1 = b[pool1];
    if (p2.odd || !p2.record_loads.empty() || !p2.record_writes.empty() || !p2.umax) return "pool 2's header not as expected";
    if (p1.odd || !p1.record_loads.empty() || !p1.record_writes.empty() || !p1.umax) return "pool 1's header not as expected";
    // the mask the storage buffer pass took for pool 2's base
    u32 mask = 0;
    for (IR::Inst* a : p2.header_atomics) mask = ClassifyOffset(a->Arg(1)).mask;
    if (!mask) return "no mask";
    // pool 2's record writes: WriteGlobal through PackUint2x32(CompositeConstructU32x2(lo, hi)), lo = IAdd32(c0+0x60's low, x),
    // two of the ten through IAdd64(that, #8): `extra` is that constant
    struct GlobalWrite {
        IR::Block* block;
        IR::Inst* inst;
        IR::Value lo;
        u32 extra;
    };
    std::vector<GlobalWrite> writes2, writes1;
    int other_writes = 0;
    for (IR::Block* block : program.blocks)
        for (IR::Inst& inst : block->Instructions()) {
            const IR::Opcode op = inst.GetOpcode();
            if (op != IR::Opcode::WriteGlobal32 && op != IR::Opcode::WriteGlobal64) continue;
            IR::Value a = inst.Arg(0).Resolve();
            u32 extra = 0;
            if (!a.IsImmediate() && a.InstRecursive()->GetOpcode() == IR::Opcode::IAdd64) {
                const IR::Value k = a.InstRecursive()->Arg(1).Resolve();
                if (k.IsImmediate() && k.Type() == IR::Type::U64 && k.U64() < 48) {   // (inside one 48-byte record)
                    extra = static_cast<u32>(k.U64());
                    a = a.InstRecursive()->Arg(0).Resolve();
                } else {
                    other_writes++;
                    continue;
                }
            }
            std::optional<u32> base;
            IR::Value lo;
            if (!a.IsImmediate() && a.InstRecursive()->GetOpcode() == IR::Opcode::PackUint2x32) {
                const IR::Value c = a.InstRecursive()->Arg(0).Resolve();
                if (!c.IsImmediate() && c.InstRecursive()->GetOpcode() == IR::Opcode::CompositeConstructU32x2) {
                    lo = c.InstRecursive()->Arg(0).Resolve();
                    if (!lo.IsImmediate() && lo.InstRecursive()->GetOpcode() == IR::Opcode::IAdd32) {
                        base = CbufLo(lo.InstRecursive()->Arg(0));
                        if (!base) base = CbufLo(lo.InstRecursive()->Arg(1));
                    }
                }
            }
            if (base && *base == 0x60) writes2.push_back({block, &inst, lo, extra});
            else if (base && *base == 0x70) writes1.push_back({block, &inst, lo, extra});
            else other_writes++;
        }
    if (writes2.size() != 10 || writes1.size() != 10 || other_writes != 0) return "pool writes not as expected";
    // the caps: pool 2's UMin is the one its group count comes from; its guard the first of the two, before its writes
    const std::vector<IR::Inst*> umin = UMins(program, kSimTurfPoolCap);
    const std::vector<IR::Inst*> guards = Identities(program, kSimTurfPoolCap);
    const std::vector<IR::Inst*> in_guards = Identities(program, kSimTurfBladeCap - 1);
    if (umin.size() != 2 || guards.size() != 2 || in_guards.size() != 1) return "caps not as expected";
    IR::Inst *umin2 = nullptr, *umin1 = nullptr;
    int umin2_found = 0, umin1_found = 0;
    for (IR::Inst* u : umin)
        if (ComesFrom(p2.umax->Arg(2), u, 16)) umin2 = u, umin2_found++;
        else if (ComesFrom(p1.umax->Arg(2), u, 16)) umin1 = u, umin1_found++;
    if (umin2_found != 1 || umin1_found != 1 || umin1 == umin2) return "the pools' caps were not found independently";
    size_t first2 = SIZE_MAX, last2 = 0, first1 = SIZE_MAX, last1 = 0;
    for (const GlobalWrite& w : writes2) {
        const size_t o = OrderOf(program, w.inst);
        first2 = std::min(first2, o), last2 = std::max(last2, o);
    }
    for (const GlobalWrite& w : writes1) {
        const size_t o = OrderOf(program, w.inst);
        first1 = std::min(first1, o), last1 = std::max(last1, o);
    }
    const size_t g0 = OrderOf(program, guards[0]), g1 = OrderOf(program, guards[1]);
    if (!(g0 < first2 && last2 < g1 && g1 < first1 && first1 <= last1)) return "the pools' guards are not where expected";
    const u32 used = UsedBindings(program);
    const Offset input_records = ClassifyOffset(b[input].record_loads[0]->Arg(1));
    if (!input_records.ok) return "the blade list's record offset is not as expected";
    const auto in_at = AddPrivate(info, info.storage_buffers_descriptors[input].cbuf_offset, false, false, used);
    if (!in_at) return "no room for a descriptor";
    const auto out2_at = AddPrivate(info, 0x60, true, true, used);
    const auto out1_at = out2_at ? AddPrivate(info, 0x70, true, true, used) : std::nullopt;
    if (!out2_at || !out1_at) return info.storage_buffers_descriptors = original_descs, "no room for descriptors";
    Redirect(info, b[input].record_loads, *in_at);
    const auto redirect_globals = [&](const std::vector<GlobalWrite>& writes, u32 orig, u32 binding) {
        for (const GlobalWrite& w : writes) {
            IR::Block* const block = w.block;
            IR::Inst* const inst = w.inst;
            const IR::Block::iterator it = block->Instructions().iterator_to(*inst);
            const IR::Value lo = w.extra ? IR::Value{&*block->PrependNewInst(it, IR::Opcode::IAdd32, {w.lo, IR::Value{w.extra}})} : w.lo;
            const auto word = block->PrependNewInst(it, IR::Opcode::GetCbufU32, {IR::Value{0u}, IR::Value{orig}});
            const auto base = block->PrependNewInst(it, IR::Opcode::BitwiseAnd32, {IR::Value{&*word}, IR::Value{mask}});
            const auto off = block->PrependNewInst(it, IR::Opcode::ISub32, {lo, IR::Value{&*base}});
            const bool wide = inst->GetOpcode() == IR::Opcode::WriteGlobal64;
            block->PrependNewInst(it, wide ? IR::Opcode::WriteStorage64 : IR::Opcode::WriteStorage32,
                                  {IR::Value{binding}, IR::Value{&*off}, inst->Arg(1)});
            info.used_storage_buffer_types |= wide ? IR::Type::U32x2 : IR::Type::U32;
            inst->Invalidate();
        }
    };
    redirect_globals(writes2, 0x60, *out2_at);
    redirect_globals(writes1, 0x70, *out1_at);
    umin2->SetArg(1, RuntimeCapacity(program, umin2, *out2_at, 0x60, mask, 48));
    umin1->SetArg(1, RuntimeCapacity(program, umin1, *out1_at, 0x70, mask, 48));
    guards[0]->SetArg(0, RuntimeCapacity(program, guards[0], *out2_at, 0x60, mask, 48));
    guards[1]->SetArg(0, RuntimeCapacity(program, guards[1], *out1_at, 0x70, mask, 48));
    in_guards[0]->SetArg(0, RuntimeCapacity(program, in_guards[0], *in_at, input_records.orig,
                                            input_records.mask, 8, true));
    return {};
}

/// A mesh program that draws a pool: its one guard `Identity #138240`, and one c0 binding read for a count (header +12) and
/// for records, nothing else. Its record loads from the private pool when there is one for the address
inline std::string PatchPoolReader(IR::Program& program) {
    Info& info = program.info;
    const std::vector<IR::Inst*> guards = Identities(program, kSimTurfPoolCap);
    if (guards.size() != 1) return "no";
    std::vector<Binding> b = Bindings(program, info);
    int pool = -1;
    for (size_t i = 0; i < b.size(); i++) {
        const auto& d = info.storage_buffers_descriptors[i];
        const Binding& x = b[i];
        if (d.count != 1 || d.cbuf_index != 0 || IsSimTurfStorage(d) || x.odd) continue;
        if (x.header12_loads != 1 || x.record_loads.empty() || !x.record_writes.empty() || !x.header_atomics.empty()) continue;
        if (pool != -1) return "two pools read";
        pool = static_cast<int>(i);
    }
    if (pool < 0) return "no";
    const Offset records = ClassifyOffset(b[pool].record_loads[0]->Arg(1));
    if (!records.ok) return "pool record offset not as expected";
    const auto at = AddPrivate(info, info.storage_buffers_descriptors[pool].cbuf_offset, true, false, UsedBindings(program));
    if (!at) return "no room for a descriptor";
    Redirect(info, b[pool].record_loads, *at);
    guards[0]->SetArg(0, RuntimeCapacity(program, guards[0], *at, records.orig, records.mask, 48));
    return {};
}

} // namespace sim_turf_detail

/// The pass: for d5a9 and 3de6 by key (compute), for the mesh programs by what they do. "" when nothing was for this program,
/// else a line for the log: "made larger" or why it was left as it is
inline std::string SimTurfPools(IR::Program& program, u64 key) {
    if (program.stage == Stage::Compute) {
        if (std::find(std::begin(kSimTurfBladeKeys), std::end(kSimTurfBladeKeys), key) != std::end(kSimTurfBladeKeys)) {
            const std::string why = sim_turf_detail::PatchBladeProducer(program);
            return why.empty() ? "blade list made larger" : "blade list left as it is: " + why;
        }
        if (std::find(std::begin(kSimTurfPoolKeys), std::end(kSimTurfPoolKeys), key) != std::end(kSimTurfPoolKeys)) {
            const std::string why = sim_turf_detail::PatchPoolProducer(program);
            return why.empty() ? "both pools made larger" : "pools left as they are: " + why;
        }
        return {};
    }
    if (program.stage == Stage::Mesh) {
        const std::string why = sim_turf_detail::PatchPoolReader(program);
        if (why == "no") return {};
        return why.empty() ? "pool reader made larger" : "pool reader left as it is: " + why;
    }
    return {};
}

} // namespace Shader::Optimization
