// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/backend/spirv/mma_software.h"

namespace Shader::Backend::SPIRV {
namespace {
Id SubgroupScope(EmitContext& ctx) {
    return ctx.Const(static_cast<u32>(spv::Scope::Subgroup));
}

bool StageSupportsSubgroups(EmitContext& ctx) {
    return ctx.profile.SupportsSubgroupStage(ctx.stage);
}

Id GetThreadId(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    return ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
}

Id WarpExtract(EmitContext& ctx, Id value) {
    const Id thread_id{GetThreadId(ctx)};
    const Id local_index{ctx.OpShiftRightArithmetic(ctx.U32[1], thread_id, ctx.Const(5U))};
    if (ctx.profile.has_broken_spirv_subgroup_mask_vector_extract_dynamic) {
        const Id c0_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(0U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 0U), ctx.Const(0U))};
        const Id c1_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(1U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 1U), ctx.Const(0U))};
        const Id c2_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(2U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 2U), ctx.Const(0U))};
        const Id c3_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(3U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 3U), ctx.Const(0U))};
        const Id c0_or_c1{ctx.OpBitwiseOr(ctx.U32[1], c0_sel, c1_sel)};
        const Id c2_or_c3{ctx.OpBitwiseOr(ctx.U32[1], c2_sel, c3_sel)};
        const Id c0_or_c1_or_c2_or_c3{ctx.OpBitwiseOr(ctx.U32[1], c0_or_c1, c2_or_c3)};
        return c0_or_c1_or_c2_or_c3;
    } else {
        return ctx.OpVectorExtractDynamic(ctx.U32[1], value, local_index);
    }
}

Id LoadMask(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpLoad(ctx.U32[4], mask)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpCompositeExtract(ctx.U32[1], value, 0U);
    }
    return WarpExtract(ctx, value);
}

void SetInBoundsFlag(IR::Inst* inst, Id result) {
    IR::Inst* const in_bounds{inst->GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)};
    if (!in_bounds) {
        return;
    }
    in_bounds->SetDefinition(result);
    in_bounds->Invalidate();
}

Id ComputeMinThreadId(EmitContext& ctx, Id thread_id, Id segmentation_mask) {
    return ctx.OpBitwiseAnd(ctx.U32[1], thread_id, segmentation_mask);
}

Id ComputeMaxThreadId(EmitContext& ctx, Id min_thread_id, Id clamp, Id not_seg_mask) {
    return ctx.OpBitwiseOr(ctx.U32[1], min_thread_id,
                           ctx.OpBitwiseAnd(ctx.U32[1], clamp, not_seg_mask));
}

Id GetMaxThreadId(EmitContext& ctx, Id thread_id, Id clamp, Id segmentation_mask) {
    const Id not_seg_mask{ctx.OpNot(ctx.U32[1], segmentation_mask)};
    const Id min_thread_id{ComputeMinThreadId(ctx, thread_id, segmentation_mask)};
    return ComputeMaxThreadId(ctx, min_thread_id, clamp, not_seg_mask);
}

Id HostThreadId(EmitContext& ctx, Id thread_id) {
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return thread_id;
    }
    const Id partition_idx{ctx.OpShiftRightLogical(ctx.U32[1], GetThreadId(ctx), ctx.Const(5u))};
    const Id partition_base{ctx.OpShiftLeftLogical(ctx.U32[1], partition_idx, ctx.Const(5u))};
    return ctx.OpIAdd(ctx.U32[1], thread_id, partition_base);
}

Id GuestLane(EmitContext& ctx, Id index) {
    return ctx.OpBitwiseAnd(ctx.U32[1], index, ctx.Const(31U));
}

bool UseShuffleFallback(EmitContext& ctx) {
    return ctx.profile.has_broken_spirv_subgroup_shuffle ||
           (ctx.profile.has_broken_spirv_fragment_shuffle && ctx.stage == Stage::Fragment);
}

Id ShuffleAbsolute(EmitContext& ctx, Id value, Id src_thread_id, bool broadcast) {
    if (!broadcast) {
        return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, src_thread_id);
    }
    Id result{ctx.u32_zero_value};
    for (u32 lane = 0; lane < ctx.profile.max_subgroup_size; ++lane) {
        const Id read{
            ctx.OpGroupNonUniformBroadcast(ctx.U32[1], SubgroupScope(ctx), value, ctx.Const(lane))};
        const Id matches{ctx.OpIEqual(ctx.U1, src_thread_id, ctx.Const(lane))};
        result = ctx.OpSelect(ctx.U32[1], matches, read, result);
    }
    return result;
}

Id ShuffleRelative(EmitContext& ctx, Id value, Id delta, Id src_thread_id, spv::Op op) {
    const bool broadcast{UseShuffleFallback(ctx)};
    if (!ctx.profile.support_shuffle_relative || broadcast) {
        return ShuffleAbsolute(ctx, value, HostThreadId(ctx, src_thread_id), broadcast);
    }
    const Id result{ctx.OpGroupNonUniformShuffleXor(ctx.U32[1], SubgroupScope(ctx), value, delta)};
    ctx.opcode_rewrites.emplace_back(result.value, op);
    return result;
}

Id BroadcastLane(EmitContext& ctx, Id value, u32 lane) {
    Id result{
        ctx.OpGroupNonUniformBroadcast(ctx.U32[1], SubgroupScope(ctx), value, ctx.Const(lane))};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return result;
    }
    const Id partition_idx{ctx.OpShiftRightLogical(ctx.U32[1], GetThreadId(ctx), ctx.Const(5u))};
    for (u32 base = 32; base < ctx.profile.max_subgroup_size; base += 32) {
        const Id read{ctx.OpGroupNonUniformBroadcast(ctx.U32[1], SubgroupScope(ctx), value,
                                                     ctx.Const(base + lane))};
        const Id matches{ctx.OpIEqual(ctx.U1, partition_idx, ctx.Const(base >> 5))};
        result = ctx.OpSelect(ctx.U32[1], matches, read, result);
    }
    return result;
}

// (longshot, mma_layout.h mesh_fallback_shared_warp) The shared warp: a fallback stage's 32 lanes are its workgroup's invocations,
// on however many hardware threads the driver runs them, and what a warp operation needs of the other lanes goes through words of
// workgroup memory. The lanes that run a block are its region's: the whole warp at the top, and inside an if the lanes that took
// it, which every lane of the region around it says (WarpOpenRegion) before any goes in. A site (a ballot, a vote, a shuffle) is
// reached by all of its region's lanes, since the planner (EmitContext::DefineSharedWarp) takes no stage that leaves an if early
// or has a site in a loop: each lane writes its part and its arrival, then waits for the arrivals it needs. A lane only waits
// for lanes behind it in the program, which wait for none ahead of them, so the waits end.
Id WarpWord(EmitContext& ctx, Id index) {
    return ctx.OpAccessChain(ctx.warp_word_pointer, ctx.warp_words, index);
}

Id WarpWord(EmitContext& ctx, u32 index) {
    return WarpWord(ctx, ctx.Const(index));
}

u32 WarpTake(EmitContext& ctx, u32 words) {
    if (ctx.warp_word_next + words > ctx.warp_word_count) {
        throw LogicError("Shared warp: {} words past the {} planned", ctx.warp_word_next + words, ctx.warp_word_count);
    }
    const u32 first{ctx.warp_word_next};
    ctx.warp_word_next += words;
    return first;
}

Id WarpScope(EmitContext& ctx) {
    return ctx.Const(static_cast<u32>(spv::Scope::Workgroup));
}

void WarpOr(EmitContext& ctx, u32 word, Id bits) {
    ctx.OpAtomicOr(ctx.U32[1], WarpWord(ctx, word), WarpScope(ctx), ctx.u32_zero_value, bits);
}

Id WarpRead(EmitContext& ctx, Id pointer) {
    return ctx.OpAtomicLoad(ctx.U32[1], pointer, WarpScope(ctx), ctx.u32_zero_value);
}

void WarpWait(EmitContext& ctx, u32 word, Id lanes) {
    ctx.OpFunctionCall(ctx.void_id, ctx.warp_wait, ctx.Const(word), lanes);
}

/// (RX_VK_MESH_SHARED_WARP_CHECK) one more answer compared, and one more difference where there is one: the counters past
/// the fallback's records (emit_spirv.h WARP_COUNTER_*)
void WarpCheck(EmitContext& ctx, Id differs) {
    const Id device{ctx.Const(static_cast<u32>(spv::Scope::Device))};
    ctx.OpAtomicIAdd(ctx.U32[1], ctx.MeshFallbackWarpCounter(WARP_COUNTER_COMPARED), device, ctx.u32_zero_value, ctx.Const(1U));
    ctx.OpAtomicIAdd(ctx.U32[1], ctx.MeshFallbackWarpCounter(WARP_COUNTER_DIFFERENT), device, ctx.u32_zero_value,
                     ctx.OpSelect(ctx.U32[1], differs, ctx.Const(1U), ctx.u32_zero_value));
}

/// The lanes of the block's region for which `pred` holds
Id SharedBallot(EmitContext& ctx, Id pred) {
    const u32 word{WarpTake(ctx, WARP_BALLOT_WORDS)};
    if (pred.value == ctx.true_value.value) {
        return ctx.warp_region;
    }
    WarpOr(ctx, word, ctx.OpSelect(ctx.U32[1], pred, ctx.warp_bit, ctx.u32_zero_value));
    WarpOr(ctx, word + 1, ctx.warp_bit);
    WarpWait(ctx, word + 1, ctx.warp_region);
    return WarpRead(ctx, WarpWord(ctx, word));
}

/// `value` of lane `source` (0 where that lane is not in the block's region: the subgroup's shuffle leaves it undefined)
Id SharedShuffle(EmitContext& ctx, Id value, Id source) {
    const u32 word{WarpTake(ctx, WARP_SHUFFLE_WORDS)};
    const Id lane{ctx.OpBitwiseAnd(ctx.U32[1], source, ctx.Const(31U))};
    ctx.OpAtomicExchange(ctx.U32[1], WarpWord(ctx, ctx.OpIAdd(ctx.U32[1], ctx.warp_lane, ctx.Const(word))), WarpScope(ctx),
                         ctx.u32_zero_value, value);
    WarpOr(ctx, word + 32, ctx.warp_bit);
    WarpWait(ctx, word + 32,
             ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpShiftLeftLogical(ctx.U32[1], ctx.Const(1U), lane), ctx.warp_region));
    return WarpRead(ctx, WarpWord(ctx, ctx.OpIAdd(ctx.U32[1], lane, ctx.Const(word))));
}

/// `value` of the constant lane `lane`: one word, which only that lane writes
Id SharedBroadcast(EmitContext& ctx, Id value, u32 lane) {
    const u32 word{WarpTake(ctx, WARP_BROADCAST_WORDS)};
    WarpOr(ctx, word,
           ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, ctx.warp_lane, ctx.Const(lane)), value, ctx.u32_zero_value));
    WarpOr(ctx, word + 1, ctx.warp_bit);
    WarpWait(ctx, word + 1, ctx.OpBitwiseAnd(ctx.U32[1], ctx.Const(1U << lane), ctx.warp_region));
    return WarpRead(ctx, WarpWord(ctx, word));
}

/// A lane mask of the shared warp, from the lane's bit: lower lanes, lower or equal, higher, higher or equal
Id SharedLtMask(EmitContext& ctx) {
    return ctx.OpISub(ctx.U32[1], ctx.warp_bit, ctx.Const(1U));
}

Id SharedLeMask(EmitContext& ctx) {
    return ctx.OpBitwiseOr(ctx.U32[1], SharedLtMask(ctx), ctx.warp_bit);
}

/// (the check: where a shuffle's two answers must agree: its source in range and a lane of the region; the subgroup's answer
/// for any other lane is undefined)
Id ShuffleCounts(EmitContext& ctx, Id in_range, Id source) {
    if (!ctx.warp_shared_check) {
        return Id{};
    }
    const Id bit{ctx.OpShiftLeftLogical(ctx.U32[1], ctx.Const(1U), ctx.OpBitwiseAnd(ctx.U32[1], source, ctx.Const(31U)))};
    return ctx.OpLogicalAnd(ctx.U1, in_range,
                            ctx.OpINotEqual(ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], bit, ctx.warp_region), ctx.u32_zero_value));
}

/// What the stage uses: the shared answer, or with the check the subgroup's, the two compared (`counts`: where they must agree)
template <typename Native>
Id SharedOr(EmitContext& ctx, Id shared, Native&& native, Id counts = Id{}, bool boolean = false) {
    if (!ctx.warp_shared_check) {
        return shared;
    }
    const Id subgroup{native()};
    Id differs{boolean ? ctx.OpLogicalNotEqual(ctx.U1, shared, subgroup) : ctx.OpINotEqual(ctx.U1, shared, subgroup)};
    if (Sirit::ValidId(counts)) {
        differs = ctx.OpLogicalAnd(ctx.U1, differs, counts);
    }
    WarpCheck(ctx, differs);
    return subgroup;
}
} // Anonymous namespace

void WarpOpenRegion(EmitContext& ctx, const IR::Block* body, Id cond) {
    if (!ctx.warp_shared) {
        return;
    }
    const bool region{ctx.warp_region_ifs.contains(body)};
    ctx.warp_region_opened.push_back(region);
    if (!region) {
        return;
    }
    // (every lane of the region around says whether it goes in, then that it has said so; the lanes that go in are known
    // once all of them have)
    const u32 word{WarpTake(ctx, WARP_REGION_WORDS)};
    WarpOr(ctx, word, ctx.OpSelect(ctx.U32[1], cond, ctx.warp_bit, ctx.u32_zero_value));
    WarpOr(ctx, word + 1, ctx.warp_bit);
    WarpWait(ctx, word + 1, ctx.warp_region);
    ctx.warp_regions.push_back(ctx.warp_region);
    ctx.warp_region = WarpRead(ctx, WarpWord(ctx, word));
}

void WarpCloseRegion(EmitContext& ctx) {
    if (!ctx.warp_shared || ctx.warp_region_opened.empty()) {
        return;
    }
    const bool region{ctx.warp_region_opened.back()};
    ctx.warp_region_opened.pop_back();
    if (region) {
        ctx.warp_region = ctx.warp_regions.back();
        ctx.warp_regions.pop_back();
    }
}

/// The shared warp's start, in the stage's entry block: the invocation's lane and bit, the whole warp as the region, and
/// the words at zero (the caller's barrier follows: every lane has zeroed its words before any writes one)
void WarpPrologue(EmitContext& ctx) {
    if (!ctx.warp_shared) {
        return;
    }
    ctx.warp_lane = ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id), 0U);
    ctx.warp_bit = ctx.OpShiftLeftLogical(ctx.U32[1], ctx.Const(1U), ctx.warp_lane);
    ctx.warp_region = ctx.Const(0xffffffffU);
    for (u32 first = 0; first < ctx.warp_word_count; first += 32) {
        const Id at{ctx.OpUMin(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], ctx.warp_lane, ctx.Const(first)),
                               ctx.Const(ctx.warp_word_count - 1))};
        ctx.OpStore(WarpWord(ctx, at), ctx.u32_zero_value);
    }
}

Id EmitLaneId(EmitContext& ctx) {
    const auto subgroup = [&] {
        const Id id{GetThreadId(ctx)};
        if (!ctx.profile.warp_size_potentially_larger_than_guest) {
            return id;
        }
        return ctx.OpBitwiseAnd(ctx.U32[1], id, ctx.Const(31U));
    };
    if (ctx.warp_shared) {
        return SharedOr(ctx, ctx.warp_lane, subgroup);
    }
    return subgroup();
}

Id EmitVoteAll(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return pred;
    }
    const auto subgroup = [&] {
        if (!ctx.profile.warp_size_potentially_larger_than_guest) {
            return ctx.OpGroupNonUniformAll(ctx.U1, SubgroupScope(ctx), pred);
        }
        const Id mask_ballot{
            ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
        const Id active_mask{WarpExtract(ctx, mask_ballot)};
        const Id ballot{
            WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
        const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], ballot, active_mask)};
        return ctx.OpIEqual(ctx.U1, lhs, active_mask);
    };
    if (ctx.warp_shared) {
        return SharedOr(ctx, ctx.OpIEqual(ctx.U1, SharedBallot(ctx, pred), ctx.warp_region), subgroup, Id{}, true);
    }
    return subgroup();
}

Id EmitVoteAny(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return pred;
    }
    const auto subgroup = [&] {
        if (!ctx.profile.warp_size_potentially_larger_than_guest) {
            return ctx.OpGroupNonUniformAny(ctx.U1, SubgroupScope(ctx), pred);
        }
        const Id mask_ballot{
            ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
        const Id active_mask{WarpExtract(ctx, mask_ballot)};
        const Id ballot{
            WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
        const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], ballot, active_mask)};
        return ctx.OpINotEqual(ctx.U1, lhs, ctx.u32_zero_value);
    };
    if (ctx.warp_shared) {
        return SharedOr(ctx, ctx.OpINotEqual(ctx.U1, SharedBallot(ctx, pred), ctx.u32_zero_value), subgroup, Id{}, true);
    }
    return subgroup();
}

Id EmitVoteEqual(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.true_value;
    }
    const auto subgroup = [&] {
        if (!ctx.profile.warp_size_potentially_larger_than_guest) {
            return ctx.OpGroupNonUniformAllEqual(ctx.U1, SubgroupScope(ctx), pred);
        }
        const Id mask_ballot{
            ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
        const Id active_mask{WarpExtract(ctx, mask_ballot)};
        const Id ballot{
            WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
        const Id lhs{ctx.OpBitwiseXor(ctx.U32[1], ballot, active_mask)};
        return ctx.OpLogicalOr(ctx.U1, ctx.OpIEqual(ctx.U1, lhs, ctx.u32_zero_value),
                               ctx.OpIEqual(ctx.U1, lhs, active_mask));
    };
    if (ctx.warp_shared) {
        const Id ballot{SharedBallot(ctx, pred)};
        return SharedOr(ctx,
                        ctx.OpLogicalOr(ctx.U1, ctx.OpIEqual(ctx.U1, ballot, ctx.u32_zero_value),
                                        ctx.OpIEqual(ctx.U1, ballot, ctx.warp_region)),
                        subgroup, Id{}, true);
    }
    return subgroup();
}

Id EmitSubgroupBallot(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.OpSelect(ctx.U32[1], pred, ctx.Const(1u), ctx.u32_zero_value);
    }
    const auto subgroup = [&] {
        const Id ballot{ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred)};
        if (!ctx.profile.warp_size_potentially_larger_than_guest) {
            return ctx.OpCompositeExtract(ctx.U32[1], ballot, 0U);
        }
        return WarpExtract(ctx, ballot);
    };
    if (ctx.warp_shared) {
        return SharedOr(ctx, SharedBallot(ctx, pred), subgroup);
    }
    return subgroup();
}

Id EmitSubgroupEqMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    if (ctx.warp_shared) {
        return SharedOr(ctx, ctx.warp_bit, [&] { return LoadMask(ctx, ctx.subgroup_mask_eq); });
    }
    return LoadMask(ctx, ctx.subgroup_mask_eq);
}

Id EmitSubgroupLtMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    if (ctx.warp_shared) {
        return SharedOr(ctx, SharedLtMask(ctx), [&] { return LoadMask(ctx, ctx.subgroup_mask_lt); });
    }
    return LoadMask(ctx, ctx.subgroup_mask_lt);
}

Id EmitSubgroupLeMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    if (ctx.warp_shared) {
        return SharedOr(ctx, SharedLeMask(ctx), [&] { return LoadMask(ctx, ctx.subgroup_mask_le); });
    }
    return LoadMask(ctx, ctx.subgroup_mask_le);
}

Id EmitSubgroupGtMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    if (ctx.warp_shared) {
        return SharedOr(ctx, ctx.OpNot(ctx.U32[1], SharedLeMask(ctx)), [&] { return LoadMask(ctx, ctx.subgroup_mask_gt); });
    }
    return LoadMask(ctx, ctx.subgroup_mask_gt);
}

Id EmitSubgroupGeMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    if (ctx.warp_shared) {
        return SharedOr(ctx, ctx.OpNot(ctx.U32[1], SharedLtMask(ctx)), [&] { return LoadMask(ctx, ctx.subgroup_mask_ge); });
    }
    return LoadMask(ctx, ctx.subgroup_mask_ge);
}

Id EmitShuffleIndex(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                    Id segmentation_mask) {
    const Id not_seg_mask{ctx.OpNot(ctx.U32[1], segmentation_mask)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id min_thread_id{ComputeMinThreadId(ctx, thread_id, segmentation_mask)};
    const Id max_thread_id{ComputeMaxThreadId(ctx, min_thread_id, clamp, not_seg_mask)};

    const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], GuestLane(ctx, index), not_seg_mask)};
    const Id src_thread_id{ctx.OpBitwiseOr(ctx.U32[1], lhs, min_thread_id)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    SetInBoundsFlag(inst, in_range);
    if (!StageSupportsSubgroups(ctx)) {
        return value;
    }
    const IR::Value lane{inst->Arg(1).Resolve()};
    const IR::Value segment{inst->Arg(3).Resolve()};
    if (lane.IsImmediate() && segment.IsImmediate() && segment.U32() == 0) {
        const auto subgroup = [&] { return BroadcastLane(ctx, value, lane.U32() & 31); };
        if (ctx.warp_shared) {
            const Id source{ctx.Const(lane.U32() & 31)};
            const Id shared{SharedBroadcast(ctx, value, lane.U32() & 31)};
            return ctx.OpSelect(ctx.U32[1], in_range,
                                SharedOr(ctx, shared, subgroup, ShuffleCounts(ctx, in_range, source)),
                                value);
        }
        return ctx.OpSelect(ctx.U32[1], in_range, subgroup(), value);
    }
    const auto subgroup = [&] {
        return ShuffleAbsolute(ctx, value, HostThreadId(ctx, src_thread_id), UseShuffleFallback(ctx));
    };
    if (ctx.warp_shared) {
        const Id shared{SharedShuffle(ctx, value, src_thread_id)};
        return ctx.OpSelect(ctx.U32[1], in_range,
                            SharedOr(ctx, shared, subgroup, ShuffleCounts(ctx, in_range, src_thread_id)),
                            value);
    }
    return ctx.OpSelect(ctx.U32[1], in_range, subgroup(), value);
}

Id EmitShuffleUp(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                 Id segmentation_mask) {
    if (!StageSupportsSubgroups(ctx)) {
        SetInBoundsFlag(inst, ctx.false_value);
        return value;
    }
    const Id delta{GuestLane(ctx, index)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    const Id src_thread_id{ctx.OpISub(ctx.U32[1], thread_id, delta)};
    const Id in_range{ctx.OpSGreaterThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    SetInBoundsFlag(inst, in_range);
    const auto subgroup = [&] {
        return ShuffleRelative(ctx, value, delta, src_thread_id, spv::Op::OpGroupNonUniformShuffleUp);
    };
    if (ctx.warp_shared) {
        const Id shared{SharedShuffle(ctx, value, src_thread_id)};
        return ctx.OpSelect(ctx.U32[1], in_range,
                            SharedOr(ctx, shared, subgroup, ShuffleCounts(ctx, in_range, src_thread_id)),
                            value);
    }
    return ctx.OpSelect(ctx.U32[1], in_range, subgroup(), value);
}

Id EmitShuffleDown(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                   Id segmentation_mask) {
    if (!StageSupportsSubgroups(ctx)) {
        SetInBoundsFlag(inst, ctx.false_value);
        return value;
    }
    const Id delta{GuestLane(ctx, index)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    const Id src_thread_id{ctx.OpIAdd(ctx.U32[1], thread_id, delta)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    SetInBoundsFlag(inst, in_range);
    const auto subgroup = [&] {
        return ShuffleRelative(ctx, value, delta, src_thread_id, spv::Op::OpGroupNonUniformShuffleDown);
    };
    if (ctx.warp_shared) {
        const Id shared{SharedShuffle(ctx, value, src_thread_id)};
        return ctx.OpSelect(ctx.U32[1], in_range,
                            SharedOr(ctx, shared, subgroup, ShuffleCounts(ctx, in_range, src_thread_id)),
                            value);
    }
    return ctx.OpSelect(ctx.U32[1], in_range, subgroup(), value);
}

Id EmitShuffleButterfly(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                        Id segmentation_mask) {
    if (!StageSupportsSubgroups(ctx)) {
        SetInBoundsFlag(inst, ctx.false_value);
        return value;
    }
    const Id mask{GuestLane(ctx, index)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    const Id src_thread_id{ctx.OpBitwiseXor(ctx.U32[1], thread_id, mask)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    SetInBoundsFlag(inst, in_range);
    // (longshot) through the broadcasts where shuffles are broken (UseShuffleFallback: AMD's fragment stages, Lichii #273),
    // as the absolute and relative shuffles are; #273 left SHFL.BFLY native, and some of Kokiri Forest's plants use it
    const auto subgroup = [&] {
        return UseShuffleFallback(ctx)
                   ? ShuffleAbsolute(ctx, value, HostThreadId(ctx, src_thread_id), true)
                   : ctx.OpGroupNonUniformShuffleXor(ctx.U32[1], SubgroupScope(ctx), value, mask);
    };
    if (ctx.warp_shared) {
        const Id shared{SharedShuffle(ctx, value, src_thread_id)};
        return ctx.OpSelect(ctx.U32[1], in_range,
                            SharedOr(ctx, shared, subgroup, ShuffleCounts(ctx, in_range, src_thread_id)),
                            value);
    }
    return ctx.OpSelect(ctx.U32[1], in_range, subgroup(), value);
}

Id EmitQuadBroadcast(EmitContext& ctx, Id value, Id lane) {
    if (ctx.profile.support_quad_shuffles) {
        return ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], SubgroupScope(ctx), value, lane);
    }
    const Id base{ctx.OpBitwiseAnd(ctx.U32[1], GetThreadId(ctx), ctx.Const(~3u))};
    const Id local_lane{ctx.OpBitwiseAnd(ctx.U32[1], lane, ctx.Const(3u))};
    const Id src_thread_id{ctx.OpBitwiseOr(ctx.U32[1], base, local_lane)};
    return ShuffleAbsolute(ctx, value, src_thread_id,
                           ctx.profile.has_broken_spirv_subgroup_shuffle);
}

Id EmitQuadSwap(EmitContext& ctx, Id value, Id direction) {
    if (ctx.profile.support_quad_shuffles) {
        const Id result{
            ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], SubgroupScope(ctx), value, direction)};
        ctx.opcode_rewrites.emplace_back(result.value, spv::Op::OpGroupNonUniformQuadSwap);
        return result;
    }
    const Id xor_mask{ctx.OpIAdd(ctx.U32[1], direction, ctx.Const(1u))};
    // (longshot) not through UseShuffleFallback's broadcasts: a program with quad shuffles alone (uses_quad_shuffles) has
    // neither the lane id input nor the ballot capability they need, and the SPIR-V made with them crashed the start
    return ctx.OpGroupNonUniformShuffleXor(ctx.U32[1], SubgroupScope(ctx), value, xor_mask);
}

Id EmitFSwizzleAdd(EmitContext& ctx, Id op_a, Id op_b, Id swizzle) {
    const Id three{ctx.Const(3U)};
    Id mask{GetThreadId(ctx)};
    mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, three);
    mask = ctx.OpShiftLeftLogical(ctx.U32[1], mask, ctx.Const(1U));
    mask = ctx.OpShiftRightLogical(ctx.U32[1], swizzle, mask);
    mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, three);

    const Id modifier_a{ctx.OpVectorExtractDynamic(ctx.F32[1], ctx.fswzadd_lut_a, mask)};
    const Id modifier_b{ctx.OpVectorExtractDynamic(ctx.F32[1], ctx.fswzadd_lut_b, mask)};

    const Id result_a{ctx.OpFMul(ctx.F32[1], op_a, modifier_a)};
    const Id result_b{ctx.OpFMul(ctx.F32[1], op_b, modifier_b)};
    return ctx.OpFAdd(ctx.F32[1], result_a, result_b);
}

Id EmitDPdxFine(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdxFine(ctx.F32[1], op_a);
}

Id EmitDPdyFine(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdyFine(ctx.F32[1], op_a);
}

Id EmitDPdxCoarse(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdxCoarse(ctx.F32[1], op_a);
}

Id EmitDPdyCoarse(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdyCoarse(ctx.F32[1], op_a);
}

// (longshot) HMMA in software (mma_software.h): the 32 lanes' fragments through subgroup operations in one helper function,
// accumulated with FMA in f32 and rounded to f16 after every 8 columns of K, as HMMA.F16 does. It needs no cooperative matrices
// (RX_VK_MMA_SOFTWARE=1, vk_coopmat.cpp); the form is the one vk_coopmat.cpp's self-test chose for this device (mma_software_form).
void DefineSoftwareMatrixFunction(EmitContext& ctx) {
    ctx.AddCapability(spv::Capability::GroupNonUniform);
    ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
    if (!ctx.profile.SupportsSubgroupStage(ctx.stage) || ctx.profile.has_broken_spirv_subgroup_shuffle)
        throw NotImplementedException("Software HMMA requires subgroup shuffle support");
    const auto form = static_cast<SoftwareMmaForm>(mma_software_form);
    const bool quad = form != SoftwareMmaForm::Loop && mma_software_quad;
    if (quad) ctx.AddCapability(spv::Capability::GroupNonUniformQuad);
    const SoftwareMmaTypes types{ctx.U32[1], ctx.U32[2], ctx.U32[4], ctx.F32[1], ctx.F32[2], ctx.F16[1], ctx.U1};
    ctx.software_matrix_function = DefineSoftwareMma(ctx, types, ctx.subgroup_local_invocation_id, form, quad);
}


bool Wmma16Fits(const Profile& profile, const IR::Program& program) {
    // (only where every subgroup is full: the cooperative matrix operations and the subgroup barriers then run with all
    // 32 lanes, as the startup test checked them. Otherwise the software multiply)
    if (program.stage != Stage::Compute || !mma_wmma16_full_subgroups || program.workgroup_size[0] == 0 || program.workgroup_size[0] % 32 != 0) return false;
    const uint64_t threads = uint64_t(program.workgroup_size[0]) * program.workgroup_size[1] * program.workgroup_size[2];
    if (!threads || threads > 32ull * mma_wmma16_max_subgroups || profile.warp_size_potentially_larger_than_guest || profile.max_subgroup_size != 32) return false;
    const uint64_t subgroups = (threads + 31) / 32;
    // (EmitContext::DefineSharedMemory uses_explicit_workgroup_layout)
    const bool explicit_layout = profile.support_explicit_workgroup_layout &&
                                 (!program.info.uses_int8 || profile.support_workgroup_layout_8bit_access) &&
                                 (!program.info.uses_int16 || profile.support_workgroup_layout_16bit_access) &&
                                 program.shared_memory_size != 0;
    const uint64_t own_bytes = explicit_layout ? (uint64_t(program.shared_memory_size) + 15) & ~15ull : (uint64_t(program.shared_memory_size) + 3) & ~3ull;
    return mma_wmma16_shared_limit == 0 || own_bytes + subgroups * mma_wmma16_tile_words * 4 + 4 <= mma_wmma16_shared_limit;
}

// (longshot, AMD RDNA 3/4) HMMA_nx2 on 16x16x16 cooperative matrices (mma_layout.h mma_wmma16). Each subgroup has 512
// words of workgroup memory past the program's own: A (row major), B's K halves 0-7 and 8-15 (column major, the
// other half's rows zero) and C/D (row major), every 16x16 f16. The lanes write their fragments at the console's
// positions (PTX m16n8k16) and zeros where B and C have no columns 8-15, the device loads the tiles in its own
// layout, D = A*B_hi + (A*B_lo + C) is rounded to f16 between the two as HMMA.F16 does, and each lane reads its two
// words of D back. False when the tiles do not fit the device's shared memory: the program then takes the software
// multiply.
bool DefineWmma16MatrixFunction(EmitContext& ctx, const IR::Program& program) {
    if (!Wmma16Fits(ctx.profile, program)) return false;
    const u32 threads = std::max(1U, program.workgroup_size[0] * program.workgroup_size[1] * program.workgroup_size[2]);
    const u32 subgroups = (threads + 31) / 32;
    const bool explicit_layout = ctx.uses_explicit_workgroup_layout && program.shared_memory_size != 0;
    const u32 offset_bytes = explicit_layout ? (program.shared_memory_size + 15U) & ~15U : 0U;
    const u32 words = subgroups * mma_wmma16_tile_words;

    const Id u32ty{ctx.U32[1]};
    const Id word_pointer{ctx.TypePointer(spv::StorageClass::Workgroup, u32ty)};
    const Id array_type{ctx.TypeArray(u32ty, ctx.Const(words))};
    Id scratch{};
    if (explicit_layout) {
        // (all the Workgroup variables are blocks over the same memory: these words follow the program's. The second
        // member keeps the structure distinct from the program's own single-member ones, decorated at offset 0)
        ctx.Decorate(array_type, spv::Decoration::ArrayStride, 4U);
        const Id block{ctx.TypeStruct(array_type, u32ty)};
        ctx.MemberDecorate(block, 0U, spv::Decoration::Offset, offset_bytes);
        ctx.MemberDecorate(block, 1U, spv::Decoration::Offset, offset_bytes + words * 4);
        ctx.Decorate(block, spv::Decoration::Block);
        scratch = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Workgroup, block),
                                        spv::StorageClass::Workgroup);
        ctx.Decorate(scratch, spv::Decoration::Aliased);
    } else {
        scratch = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Workgroup, array_type),
                                        spv::StorageClass::Workgroup);
    }
    ctx.interfaces.push_back(scratch);
    ctx.AddCapability(spv::Capability::GroupNonUniform);
    const Id subgroup_id{ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Input, u32ty),
                                               spv::StorageClass::Input)};
    ctx.Decorate(subgroup_id, spv::Decoration::BuiltIn, spv::BuiltIn::SubgroupId);
    ctx.interfaces.push_back(subgroup_id);

    const auto imm = [&](u32 v) { return ctx.Const(v); };
    const auto add = [&](Id a, Id b) { return ctx.OpIAdd(u32ty, a, b); };
    const auto at = [&](Id index) {
        return explicit_layout ? ctx.OpAccessChain(word_pointer, scratch, ctx.u32_zero_value, index)
                               : ctx.OpAccessChain(word_pointer, scratch, index);
    };
    const auto store = [&](Id index, Id value) { ctx.OpStore(at(index), value, spv::MemoryAccessMask::NonPrivatePointer); };
    const auto load = [&](Id index) { return ctx.OpLoad(u32ty, at(index), spv::MemoryAccessMask::NonPrivatePointer); };
    const Id subgroup_scope{imm(static_cast<u32>(spv::Scope::Subgroup))};
    const auto barrier = [&] {
        ctx.OpControlBarrier(subgroup_scope, subgroup_scope,
                             imm(static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease |
                                                  spv::MemorySemanticsMask::WorkgroupMemory |
                                                  spv::MemorySemanticsMask::MakeAvailable |
                                                  spv::MemorySemanticsMask::MakeVisible)));
    };
    const Id f16{ctx.F16[1]};
    const auto type = [&](spv::CooperativeMatrixUse use) {
        return ctx.TypeCooperativeMatrixKHR(f16, subgroup_scope, imm(16), imm(16), imm(static_cast<u32>(use)));
    };
    const Id a_type{type(spv::CooperativeMatrixUse::MatrixAKHR)};
    const Id b_type{type(spv::CooperativeMatrixUse::MatrixBKHR)};
    const Id c_type{type(spv::CooperativeMatrixUse::MatrixAccumulatorKHR)};

    const Id fn_type{ctx.TypeFunction(ctx.U32[2], ctx.U32[4], ctx.U32[2], ctx.U32[2])};
    const Id fn{ctx.OpFunction(ctx.U32[2], spv::FunctionControlMask::DontInline, fn_type)};
    ctx.wmma16_function = fn;
    const Id matrix_a{ctx.OpFunctionParameter(ctx.U32[4])};
    const Id matrix_b{ctx.OpFunctionParameter(ctx.U32[2])};
    const Id matrix_c{ctx.OpFunctionParameter(ctx.U32[2])};
    ctx.AddLabel();
    const Id lane{ctx.OpBitwiseAnd(u32ty, ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id), imm(31))};
    const Id base{ctx.OpIMul(u32ty, ctx.OpLoad(u32ty, subgroup_id), imm(mma_wmma16_tile_words))};
    const Id g{ctx.OpShiftRightLogical(u32ty, lane, imm(2))};
    const Id i{ctx.OpBitwiseAnd(u32ty, lane, imm(3))};
    // the word of row (or column) g (+8) at word i (+4) of a region
    const auto word = [&](u32 region, u32 plus_rows, u32 plus_words) {
        return add(add(base, imm(region * 128 + plus_rows * 8 + plus_words)),
                   add(ctx.OpShiftLeftLogical(u32ty, g, imm(3)), i));
    };
    const Id zero{ctx.u32_zero_value};
    const auto part = [&](Id words, u32 index) { return ctx.OpCompositeExtract(u32ty, words, index); };
    constexpr u32 region_a = 0, region_b_lo = 1, region_b_hi = 2, region_c = 3;
    // trailing reads of the last multiply in this subgroup are done before these writes (the barrier at the end)
    store(word(region_a, 0, 0), part(matrix_a, 0));
    store(word(region_a, 8, 0), part(matrix_a, 1));
    store(word(region_a, 0, 4), part(matrix_a, 2));
    store(word(region_a, 8, 4), part(matrix_a, 3));
    store(word(region_b_lo, 0, 0), part(matrix_b, 0));
    store(word(region_b_lo, 0, 4), mma_wmma16_fast ? part(matrix_b, 1) : zero);
    store(word(region_b_lo, 8, 0), zero);
    store(word(region_b_lo, 8, 4), zero);
    if (!mma_wmma16_fast) {
        store(word(region_b_hi, 0, 0), zero);
        store(word(region_b_hi, 0, 4), part(matrix_b, 1));
        store(word(region_b_hi, 8, 0), zero);
        store(word(region_b_hi, 8, 4), zero);
    }
    store(word(region_c, 0, 0), part(matrix_c, 0));
    store(word(region_c, 8, 0), part(matrix_c, 1));
    store(word(region_c, 0, 4), zero);
    store(word(region_c, 8, 4), zero);
    barrier();
    const Id row_major{imm(0)}, column_major{imm(1)}, stride{imm(8)};
    const auto tile = [&](Id matrix_type, u32 region, Id layout) {
        return ctx.OpCooperativeMatrixLoadKHR(matrix_type, at(add(base, imm(region * 128))), layout, stride,
                                              spv::MemoryAccessMask::NonPrivatePointer);
    };
    const Id a{tile(a_type, region_a, row_major)};
    Id d{ctx.OpCooperativeMatrixMulAddKHR(c_type, a, tile(b_type, region_b_lo, column_major),
                                           tile(c_type, region_c, row_major))};
    if (!mma_wmma16_fast)
        d = ctx.OpCooperativeMatrixMulAddKHR(c_type, a, tile(b_type, region_b_hi, column_major), d);
    barrier();
    ctx.OpCooperativeMatrixStoreKHR(at(add(base, imm(region_c * 128))), d, row_major, stride,
                                    spv::MemoryAccessMask::NonPrivatePointer);
    barrier();
    const Id d0{load(word(region_c, 0, 0))};
    const Id d1{load(word(region_c, 8, 0))};
    barrier();
    ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], d0, d1));
    ctx.OpFunctionEnd();
    return true;
}

// (longshot, AMD RDNA 3/3.5/4) Fused WMMA16 N-pair DontInline helper + block-local N-pair planner.
// Pairs two HMMA_nx2 ops sharing canonical A into one 16x16 [B0|B1] / [C0|C1] -> [D0|D1] call, doubling N-utilization
// and halving barriers, A loads, CoopStores, and WMMA instructions. Kept separate from DefineWmma16MatrixFunction so
// tests/test_wmma16_spirv_val.py's standalone fixture compiles cleanly without IR::Block/IR::Inst.
void DefineWmma16PairFunction(EmitContext& ctx, const IR::Program& program) {
    if (ctx.interfaces.size() < 2) return;
    const Id scratch{ctx.interfaces[ctx.interfaces.size() - 2]};
    const Id subgroup_id{ctx.interfaces.back()};
    const bool explicit_layout = ctx.uses_explicit_workgroup_layout && program.shared_memory_size != 0;

    const Id u32ty{ctx.U32[1]};
    const Id word_pointer{ctx.TypePointer(spv::StorageClass::Workgroup, u32ty)};
    const auto imm = [&](u32 v) { return ctx.Const(v); };
    const auto add = [&](Id a, Id b) { return ctx.OpIAdd(u32ty, a, b); };
    const auto at = [&](Id index) {
        return explicit_layout ? ctx.OpAccessChain(word_pointer, scratch, ctx.u32_zero_value, index)
                               : ctx.OpAccessChain(word_pointer, scratch, index);
    };
    const auto store = [&](Id index, Id value) { ctx.OpStore(at(index), value, spv::MemoryAccessMask::NonPrivatePointer); };
    const auto load = [&](Id index) { return ctx.OpLoad(u32ty, at(index), spv::MemoryAccessMask::NonPrivatePointer); };
    const Id subgroup_scope{imm(static_cast<u32>(spv::Scope::Subgroup))};
    const auto barrier = [&] {
        ctx.OpControlBarrier(subgroup_scope, subgroup_scope,
                             imm(static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease |
                                                  spv::MemorySemanticsMask::WorkgroupMemory |
                                                  spv::MemorySemanticsMask::MakeAvailable |
                                                  spv::MemorySemanticsMask::MakeVisible)));
    };
    const Id f16{ctx.F16[1]};
    const auto type = [&](spv::CooperativeMatrixUse use) {
        return ctx.TypeCooperativeMatrixKHR(f16, subgroup_scope, imm(16), imm(16), imm(static_cast<u32>(use)));
    };
    const Id a_type{type(spv::CooperativeMatrixUse::MatrixAKHR)};
    const Id b_type{type(spv::CooperativeMatrixUse::MatrixBKHR)};
    const Id c_type{type(spv::CooperativeMatrixUse::MatrixAccumulatorKHR)};
    const Id zero{ctx.u32_zero_value};
    const Id row_major{imm(0)}, column_major{imm(1)}, stride{imm(8)};
    const auto part = [&](Id words, u32 index) { return ctx.OpCompositeExtract(u32ty, words, index); };
    constexpr u32 region_a = 0, region_b_lo = 1, region_b_hi = 2, region_c = 3;

    auto& fs = ctx.wmma16_fused;
    fs.active = true;

    const Id pair_fn_type{ctx.TypeFunction(ctx.U32[4], ctx.U32[4], ctx.U32[2], ctx.U32[2], ctx.U32[2], ctx.U32[2])};
    const Id pfn{ctx.OpFunction(ctx.U32[4], spv::FunctionControlMask::DontInline, pair_fn_type)};
    fs.pair_fn = pfn;
    const Id pa{ctx.OpFunctionParameter(ctx.U32[4])};
    const Id pb0{ctx.OpFunctionParameter(ctx.U32[2])};
    const Id pb1{ctx.OpFunctionParameter(ctx.U32[2])};
    const Id pc0{ctx.OpFunctionParameter(ctx.U32[2])};
    const Id pc1{ctx.OpFunctionParameter(ctx.U32[2])};
    ctx.AddLabel();
    const Id plane{ctx.OpBitwiseAnd(u32ty, ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id), imm(31))};
    const Id pbase{ctx.OpIMul(u32ty, ctx.OpLoad(u32ty, subgroup_id), imm(mma_wmma16_tile_words))};
    const Id pg{ctx.OpShiftRightLogical(u32ty, plane, imm(2))};
    const Id pi{ctx.OpBitwiseAnd(u32ty, plane, imm(3))};
    const Id pgi{add(ctx.OpShiftLeftLogical(u32ty, pg, imm(3)), pi)};
    const auto pword = [&](u32 region, u32 plus_rows, u32 plus_words) {
        return add(add(pbase, imm(region * 128 + plus_rows * 8 + plus_words)), pgi);
    };
    store(pword(region_a, 0, 0), part(pa, 0));
    store(pword(region_a, 8, 0), part(pa, 1));
    store(pword(region_a, 0, 4), part(pa, 2));
    store(pword(region_a, 8, 4), part(pa, 3));
    if (mma_wmma16_fast) {
        store(pword(region_b_lo, 0, 0), part(pb0, 0));
        store(pword(region_b_lo, 0, 4), part(pb0, 1));
        store(pword(region_b_lo, 8, 0), part(pb1, 0));
        store(pword(region_b_lo, 8, 4), part(pb1, 1));
    } else {
        store(pword(region_b_lo, 0, 0), part(pb0, 0));
        store(pword(region_b_lo, 0, 4), zero);
        store(pword(region_b_lo, 8, 0), part(pb1, 0));
        store(pword(region_b_lo, 8, 4), zero);
        store(pword(region_b_hi, 0, 0), zero);
        store(pword(region_b_hi, 0, 4), part(pb0, 1));
        store(pword(region_b_hi, 8, 0), zero);
        store(pword(region_b_hi, 8, 4), part(pb1, 1));
    }
    store(pword(region_c, 0, 0), part(pc0, 0));
    store(pword(region_c, 8, 0), part(pc0, 1));
    store(pword(region_c, 0, 4), part(pc1, 0));
    store(pword(region_c, 8, 4), part(pc1, 1));
    barrier();
    const auto ptile = [&](Id matrix_type, u32 region, Id layout) {
        return ctx.OpCooperativeMatrixLoadKHR(matrix_type, at(add(pbase, imm(region * 128))), layout, stride,
                                              spv::MemoryAccessMask::NonPrivatePointer);
    };
    const Id pa_mat{ptile(a_type, region_a, row_major)};
    Id pd{ctx.OpCooperativeMatrixMulAddKHR(c_type, pa_mat, ptile(b_type, region_b_lo, column_major),
                                           ptile(c_type, region_c, row_major))};
    if (!mma_wmma16_fast)
        pd = ctx.OpCooperativeMatrixMulAddKHR(c_type, pa_mat, ptile(b_type, region_b_hi, column_major), pd);
    barrier();
    ctx.OpCooperativeMatrixStoreKHR(at(add(pbase, imm(region_c * 128))), pd, row_major, stride,
                                    spv::MemoryAccessMask::NonPrivatePointer);
    barrier();
    const Id pd0_0{load(pword(region_c, 0, 0))};
    const Id pd0_1{load(pword(region_c, 8, 0))};
    const Id pd1_0{load(pword(region_c, 0, 4))};
    const Id pd1_1{load(pword(region_c, 8, 4))};
    barrier();
    ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[4], pd0_0, pd0_1, pd1_0, pd1_1));
    ctx.OpFunctionEnd();

    struct CanonKey4 {
        std::array<uintptr_t, 4> e{};
        bool operator==(const CanonKey4&) const = default;
    };
    struct CanonKey4Hash {
        size_t operator()(const CanonKey4& k) const noexcept {
            size_t h = 0;
            for (uintptr_t v : k.e) h ^= std::hash<uintptr_t>{}(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    const auto encode_val = [](IR::Value v) -> uintptr_t {
        v = v.Resolve();
        if (v.IsImmediate()) return (static_cast<uintptr_t>(v.U32()) << 2) | 1u;
        return reinterpret_cast<uintptr_t>(v.Inst());
    };
    const auto canon_a = [&](IR::Value a) -> CanonKey4 {
        a = a.Resolve();
        if (!a.IsImmediate() && a.Inst()->GetOpcode() == IR::Opcode::CompositeConstructU32x4) {
            const IR::Inst* cc = a.Inst();
            return CanonKey4{{encode_val(cc->Arg(0)), encode_val(cc->Arg(1)), encode_val(cc->Arg(2)), encode_val(cc->Arg(3))}};
        }
        return CanonKey4{{encode_val(a), 0, 0, 0}};
    };
    const auto is_seg_barrier = [](IR::Opcode op) -> bool {
        return op == IR::Opcode::Barrier || op == IR::Opcode::WorkgroupMemoryBarrier ||
               op == IR::Opcode::DeviceMemoryBarrier;
    };

    u32 next_a_id = 1;
    for (IR::Block* const block : program.blocks) {
        std::unordered_map<const IR::Inst*, u32> inst_pos;
        std::unordered_map<const IR::Inst*, u32> inst_seg;
        std::unordered_map<const IR::Inst*, std::vector<IR::Inst*>> uses;
        std::vector<IR::Inst*> mmas;
        u32 pos = 0, seg = 0;
        for (IR::Inst& inst : block->Instructions()) {
            inst_pos[&inst] = pos++;
            inst_seg[&inst] = seg;
            if (is_seg_barrier(inst.GetOpcode())) ++seg;
            const size_t nargs = inst.NumArgs();
            for (size_t k = 0; k < nargs; ++k) {
                const IR::Value v = inst.Arg(k);
                if (!v.IsImmediate() && !v.IsEmpty()) {
                    uses[v.Inst()].push_back(&inst);
                }
            }
            if (inst.GetOpcode() == IR::Opcode::MatrixMulAddF16) mmas.push_back(&inst);
        }
        if (mmas.size() < 2) continue;

        std::unordered_map<CanonKey4, u32, CanonKey4Hash> a_ids;
        std::unordered_map<u64, std::vector<IR::Inst*>> groups;
        std::vector<u64> group_order;
        for (IR::Inst* m : mmas) {
            const CanonKey4 ck = canon_a(m->Arg(0));
            auto [it, ins] = a_ids.emplace(ck, next_a_id);
            if (ins) ++next_a_id;
            const u64 gk = (static_cast<u64>(inst_seg[m]) << 32) | it->second;
            auto& vec = groups[gk];
            if (vec.empty()) group_order.push_back(gk);
            vec.push_back(m);
        }

        for (u64 gk : group_order) {
            const auto& grp = groups[gk];
            size_t idx = 0;
            while (idx + 1 < grp.size()) {
                IR::Inst* m0 = grp[idx];
                IR::Inst* m1 = grp[idx + 1];
                const u32 pos1 = inst_pos[m1];
                bool can_pair = true;
                std::vector<IR::Inst*> to_defer;
                std::unordered_set<const IR::Inst*> seen_defer;
                auto check_uses = [&](auto& self, IR::Inst* node) -> void {
                    auto it = uses.find(node);
                    if (it == uses.end()) return;
                    for (IR::Inst* u : it->second) {
                        const bool pure = (u->GetOpcode() == IR::Opcode::CompositeExtractU32x2 && node == m0) ||
                                          (u->GetOpcode() == IR::Opcode::Identity && node != m0);
                        if (pure) {
                            if (inst_pos[u] < pos1 && seen_defer.insert(u).second) to_defer.push_back(u);
                            self(self, u);
                        } else if (inst_pos[u] <= pos1) {
                            can_pair = false;
                        }
                    }
                };
                check_uses(check_uses, m0);
                if (!can_pair) {
                    ++idx;
                    continue;
                }
                std::sort(to_defer.begin(), to_defer.end(),
                          [&](const IR::Inst* a, const IR::Inst* b) { return inst_pos[a] < inst_pos[b]; });
                for (IR::Inst* d : to_defer) fs.skip_insts.insert(d);

                EmitContext::Wmma16Step sk{};
                sk.kind = EmitContext::Wmma16Step::Kind::Skip;
                sk.m0 = m0;
                sk.m1 = m1;
                fs.steps[m0] = std::move(sk);

                EmitContext::Wmma16Step st{};
                st.kind = EmitContext::Wmma16Step::Kind::Pair;
                st.m0 = m0;
                st.m1 = m1;
                st.deferred_extracts = std::move(to_defer);
                fs.steps[m1] = std::move(st);
                idx += 2;
            }
        }
    }
}

void EmitMatrixMulAddF16Fused(EmitContext& ctx, IR::Inst* inst) {
    auto& fs = ctx.wmma16_fused;
    const auto it = fs.steps.find(inst);
    if (it == fs.steps.end() || it->second.kind == EmitContext::Wmma16Step::Kind::Single) {
        const Id res = EmitMatrixMulAddF16(ctx, ctx.Def(inst->Arg(0)), ctx.Def(inst->Arg(1)), ctx.Def(inst->Arg(2)));
        inst->SetDefinition(res);
        return;
    }
    const auto& step = it->second;
    if (step.kind == EmitContext::Wmma16Step::Kind::Skip) return;

    // Paired N-fusion: call DontInline wmma16_pair_fn(a, b0, b1, c0, c1) -> U32x4
    const Id u32ty{ctx.U32[1]};
    const Id packed{ctx.OpFunctionCall(ctx.U32[4], fs.pair_fn,
                                       ctx.Def(step.m0->Arg(0)),
                                       ctx.Def(step.m0->Arg(1)), ctx.Def(step.m1->Arg(1)),
                                       ctx.Def(step.m0->Arg(2)), ctx.Def(step.m1->Arg(2)))};
    const Id d0_0{ctx.OpCompositeExtract(u32ty, packed, 0U)};
    const Id d0_1{ctx.OpCompositeExtract(u32ty, packed, 1U)};
    const Id d1_0{ctx.OpCompositeExtract(u32ty, packed, 2U)};
    const Id d1_1{ctx.OpCompositeExtract(u32ty, packed, 3U)};
    step.m0->SetDefinition(ctx.OpCompositeConstruct(ctx.U32[2], d0_0, d0_1));
    step.m1->SetDefinition(ctx.OpCompositeConstruct(ctx.U32[2], d1_0, d1_1));

    for (IR::Inst* ext : step.deferred_extracts) {
        if (ext->GetOpcode() == IR::Opcode::CompositeExtractU32x2) {
            ext->SetDefinition(ctx.OpCompositeExtract(u32ty, ctx.Def(ext->Arg(0)), ext->Arg(1).U32()));
        } else if (ext->GetOpcode() == IR::Opcode::Identity) {
            ext->SetDefinition(ctx.Def(ext->Arg(0)));
        }
    }
}


// (longshot) HMMA_nx2: D(16x8) = A(16x16) * B(16x8) + C on the subgroup's cooperative matrices. Each
// lane's fragment registers hold its elements in the console's order (two f16 a word); they go to and come
// from the device's elements in its own order (mma_layout.h, measured by vk_coopmat.cpp).
Id EmitMatrixMulAddF16(EmitContext& ctx, Id a, Id b, Id c) {
    if (ctx.wmma16_function.value != 0) return ctx.OpFunctionCall(ctx.U32[2], ctx.wmma16_function, a, b, c);
    if (ctx.software_matrix_function.value != 0)
        return ctx.OpFunctionCall(ctx.U32[2], ctx.software_matrix_function, a, b, c);
    const Id f16{ctx.F16[1]};
    const Id scope{ctx.Const(static_cast<u32>(spv::Scope::Subgroup))};
    const auto type = [&](u32 rows, u32 columns, spv::CooperativeMatrixUse use) {
        return ctx.TypeCooperativeMatrixKHR(f16, scope, ctx.Const(rows), ctx.Const(columns),
                                            ctx.Const(static_cast<u32>(use)));
    };
    const Id a_type{type(16, 16, spv::CooperativeMatrixUse::MatrixAKHR)};
    const Id b_type{type(16, 8, spv::CooperativeMatrixUse::MatrixBKHR)};
    const Id c_type{type(16, 8, spv::CooperativeMatrixUse::MatrixAccumulatorKHR)};
    const Id zero{ctx.Constant(f16, 0U)};
    const auto load = [&](Id matrix_type, Id words, u32 count, const auto& order) {
        Id m{ctx.OpCompositeConstruct(matrix_type, zero)};
        for (u32 w = 0; w < count; ++w) {
            const Id word{count == 1 ? words : ctx.OpCompositeExtract(ctx.U32[1], words, w)};
            const Id pair{ctx.OpBitcast(ctx.F16[2], word)};
            for (u32 h = 0; h < 2; ++h) {
                m = ctx.OpCompositeInsert(matrix_type, ctx.OpCompositeExtract(f16, pair, h), m, order[w * 2 + h]);
            }
        }
        return m;
    };
    const Id d{ctx.OpCooperativeMatrixMulAddKHR(c_type, load(a_type, a, 4, mma_order_a), load(b_type, b, 2, mma_order_b),
                                                    load(c_type, c, 2, mma_order_c))};
    Id out[2];
    for (u32 w = 0; w < 2; ++w) {
        const Id pair{ctx.OpCompositeConstruct(ctx.F16[2], ctx.OpCompositeExtract(f16, d, mma_order_c[w * 2]),
                                               ctx.OpCompositeExtract(f16, d, mma_order_c[w * 2 + 1]))};
        out[w] = ctx.OpBitcast(ctx.U32[1], pair);
    }
    return ctx.OpCompositeConstruct(ctx.U32[2], out[0], out[1]);
}

} // namespace Shader::Backend::SPIRV
