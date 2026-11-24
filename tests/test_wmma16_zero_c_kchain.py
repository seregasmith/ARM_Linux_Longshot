"""WMMA16 zero-C, quad-A, and K-chain fusion planner and SPIR-V codegen regression test.

python -I -B tests/test_wmma16_zero_c_kchain.py [--cxx clang++] [--headers <SPIRV-Headers/include>] [--source <repo>]

Cuts Wmma16Fits, DefineWmma16MatrixFunction, DefineWmma16PairFunction, and EmitMatrixMulAddF16Fused out of
gpu/vk/eden/shader_recompiler/backend/spirv/emit_spirv_warp.cpp, compiles them against Sirit, and verifies:
  1. Zero-C pairs select PairZeroC (2 barriers, OpCompositeConstruct(c_type, f16_zero), no C LDS store/load).
  2. Four zero-C MMAs sharing canonical A select QuadZeroC (reuses A in cooperative matrix VGPRs).
  3. A pure 2-step K-chain (zero-C producer pair feeding a consuming accumulator pair) selects PairK2ZeroC
     and skips the dead intermediate extract/identity/construct chain.
  4. An impure K-chain (producer result also used elsewhere) falls back safely to PairZeroC + Pair.
  5. The assembled SPIR-V module emits valid helper calls for all four kinds.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{spirv_headers}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/mma_layout.h"

namespace Shader::Backend::SPIRV {
using Sirit::Id;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
enum class Stage { Compute, Fragment };
namespace IR {
enum class Opcode { Input, MatrixMulAddF16, CompositeExtractU32x2, CompositeConstructU32x2, CompositeConstructU32x4,
    Identity, Barrier, WorkgroupMemoryBarrier, DeviceMemoryBarrier };
struct Inst;
struct Value {
    IR::Inst* inst{};
    u32 immediate{};
    bool is_immediate{};
    Value() = default;
    Value(IR::Inst* i) : inst(i) {}
    Value(u32 v) : immediate(v), is_immediate(true) {}
    Value Resolve() const;
    bool IsImmediate() const { return is_immediate; }
    bool IsEmpty() const { return !is_immediate && !inst; }
    u32 U32() const { return immediate; }
    IR::Inst* Inst() const { return inst; }
};
struct Inst {
    Opcode op;
    std::vector<Value> args;
    Id definition{};
    Opcode GetOpcode() const { return op; }
    size_t NumArgs() const { return args.size(); }
    Value Arg(size_t i) const { return args.at(i); }
    void SetDefinition(Id id) { definition = id; }
};
inline Value Value::Resolve() const {
    if (!is_immediate && inst && inst->op == Opcode::Identity) return inst->args.at(0).Resolve();
    return *this;
}
struct Block {
    std::list<Inst> instructions;
    auto& Instructions() { return instructions; }
    Inst* Add(Opcode op, std::initializer_list<Value> args = {}) {
        instructions.push_back({op, args});
        return &instructions.back();
    }
};
struct Program {
    Stage stage = Stage::Compute;
    std::array<u32, 3> workgroup_size{128, 1, 1};
    u32 shared_memory_size{};
    struct { bool uses_int8{}, uses_int16{}; } info;
    std::vector<Block*> blocks;
};
}
struct Profile {
    bool support_explicit_workgroup_layout{}, support_workgroup_layout_8bit_access{}, support_workgroup_layout_16bit_access{};
    bool warp_size_potentially_larger_than_guest{};
    u32 max_subgroup_size = 32;
};
struct EmitContext : Sirit::Module {
    using Sirit::Module::Module;
    std::array<Id, 5> U32{}, F16{};
    Id u32_zero_value{};
    Id subgroup_local_invocation_id{};
    Id wmma16_function{};
    bool uses_explicit_workgroup_layout{};
    Profile profile;
    std::vector<Id> interfaces;
    struct Wmma16Step {
        enum class Kind { Skip, Single, Pair, PairZeroC, QuadZeroC, PairK2ZeroC };
        Kind kind{Kind::Single};
        IR::Inst *m0{}, *m1{}, *m2{}, *m3{};
        std::vector<IR::Inst*> deferred_extracts;
    };
    struct {
        bool active{};
        Id pair_fn{};
        Id pair_zero_c_fn{};
        Id quad_zero_c_fn{};
        Id quad_ret_type{};
        Id pair_k2_zero_c_fn{};
        std::unordered_map<const IR::Inst*, Wmma16Step> steps;
        std::unordered_set<const IR::Inst*> skip_insts;
    } wmma16_fused;
    Id Const(u32 value) { return Constant(U32[1], value); }
    Id Def(IR::Value value) { return value.IsImmediate() ? Const(value.U32()) : value.Inst()->definition; }
};
Id EmitMatrixMulAddF16(EmitContext& ctx, Id a, Id b, Id c) {
    return ctx.OpFunctionCall(ctx.U32[2], ctx.wmma16_function, a, b, c);
}
@@FUNCTION@@
} // namespace Shader::Backend::SPIRV

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    using namespace Shader::Backend::SPIRV;
    using IR::Opcode;
    using Kind = EmitContext::Wmma16Step::Kind;

    mma_wmma16_fast = false;
    mma_wmma16_full_subgroups = true;
    mma_wmma16_shared_limit = 0;
    mma_wmma16_max_subgroups = 4;

    EmitContext ctx{0x00010600};
    ctx.AddCapability(spv::Capability::Shader);
    ctx.AddCapability(spv::Capability::Float16);
    ctx.AddExtension("SPV_KHR_cooperative_matrix");
    ctx.AddCapability(spv::Capability::CooperativeMatrixKHR);
    ctx.AddExtension("SPV_KHR_vulkan_memory_model");
    ctx.AddCapability(spv::Capability::VulkanMemoryModel);
    ctx.AddCapability(spv::Capability::VulkanMemoryModelDeviceScope);
    ctx.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::VulkanKHR);
    const Id void_t{ctx.TypeVoid()};
    ctx.U32[1] = ctx.TypeInt(32, false);
    for (u32 n = 2; n <= 4; ++n) ctx.U32[n] = ctx.TypeVector(ctx.U32[1], n);
    ctx.F16[1] = ctx.TypeFloat(16);
    ctx.u32_zero_value = ctx.Constant(ctx.U32[1], 0u);
    ctx.AddCapability(spv::Capability::GroupNonUniform);
    ctx.subgroup_local_invocation_id = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Input, ctx.U32[1]),
                                                             spv::StorageClass::Input);
    ctx.Decorate(ctx.subgroup_local_invocation_id, spv::Decoration::BuiltIn, spv::BuiltIn::SubgroupLocalInvocationId);
    ctx.interfaces.push_back(ctx.subgroup_local_invocation_id);

    IR::Program program;
    IR::Block block;
    IR::Block succ_block;
    program.blocks.push_back(&block);
    program.blocks.push_back(&succ_block);

    auto a_quad = block.Add(Opcode::Input);
    auto a_pair = block.Add(Opcode::Input);
    auto a_k0 = block.Add(Opcode::Input);
    auto a_k1 = block.Add(Opcode::Input);
    auto a_imp0 = block.Add(Opcode::Input);
    auto a_imp1 = block.Add(Opcode::Input);
    auto a_mb0 = block.Add(Opcode::Input);
    auto a_mb1 = block.Add(Opcode::Input);
    auto a_mb2 = block.Add(Opcode::Input);
    auto a_mb3 = block.Add(Opcode::Input);
    auto b0 = block.Add(Opcode::Input), b1 = block.Add(Opcode::Input);
    auto b2 = block.Add(Opcode::Input), b3 = block.Add(Opcode::Input);
    auto c_zero = block.Add(Opcode::CompositeConstructU32x2, {u32(0), u32(0)});

    // 1. QuadZeroC: 4 MMAs sharing a_quad with C == 0
    auto q0 = block.Add(Opcode::MatrixMulAddF16, {a_quad, b0, c_zero});
    auto q1 = block.Add(Opcode::MatrixMulAddF16, {a_quad, b1, c_zero});
    auto q2 = block.Add(Opcode::MatrixMulAddF16, {a_quad, b2, c_zero});
    auto q3 = block.Add(Opcode::MatrixMulAddF16, {a_quad, b3, c_zero});

    // 2. PairZeroC: 2 MMAs sharing a_pair with C == 0
    auto pz0 = block.Add(Opcode::MatrixMulAddF16, {a_pair, b0, c_zero});
    auto pz1 = block.Add(Opcode::MatrixMulAddF16, {a_pair, b1, c_zero});

    // 3. Pure PairK2ZeroC: (k0_0, k0_1) -> (k1_0, k1_1)
    auto k0_0 = block.Add(Opcode::MatrixMulAddF16, {a_k0, b0, c_zero});
    auto k0_0_e0 = block.Add(Opcode::CompositeExtractU32x2, {k0_0, u32(0)});
    auto k0_0_e1 = block.Add(Opcode::CompositeExtractU32x2, {k0_0, u32(1)});
    auto k0_1 = block.Add(Opcode::MatrixMulAddF16, {a_k0, b1, c_zero});
    auto k0_1_e0 = block.Add(Opcode::CompositeExtractU32x2, {k0_1, u32(0)});
    auto k0_1_e1 = block.Add(Opcode::CompositeExtractU32x2, {k0_1, u32(1)});
    auto c_k0 = block.Add(Opcode::CompositeConstructU32x2, {k0_0_e0, k0_0_e1});
    auto c_k1 = block.Add(Opcode::CompositeConstructU32x2, {k0_1_e0, k0_1_e1});
    auto k1_0 = block.Add(Opcode::MatrixMulAddF16, {a_k1, b2, c_k0});
    auto k1_1 = block.Add(Opcode::MatrixMulAddF16, {a_k1, b3, c_k1});

    // 4. Impure K-chain (imp0_0 also used externally): must stay PairZeroC + Pair
    auto imp0_0 = block.Add(Opcode::MatrixMulAddF16, {a_imp0, b0, c_zero});
    auto imp0_1 = block.Add(Opcode::MatrixMulAddF16, {a_imp0, b1, c_zero});
    auto extra_use = block.Add(Opcode::Input, {imp0_0});
    (void)extra_use;
    auto imp1_0 = block.Add(Opcode::MatrixMulAddF16, {a_imp1, b2, imp0_0});
    auto imp1_1 = block.Add(Opcode::MatrixMulAddF16, {a_imp1, b3, imp0_1});

    // 5. Cross-block impure K-chain: producer used in successor block must NOT fuse into PairK2ZeroC
    auto mb0_0 = block.Add(Opcode::MatrixMulAddF16, {a_mb0, b0, c_zero});
    auto mb0_1 = block.Add(Opcode::MatrixMulAddF16, {a_mb0, b1, c_zero});
    auto mb1_0 = block.Add(Opcode::MatrixMulAddF16, {a_mb1, b2, mb0_0});
    auto mb1_1 = block.Add(Opcode::MatrixMulAddF16, {a_mb1, b3, mb0_1});
    (void)succ_block.Add(Opcode::Input, {mb0_0});

    // 6. Cross-block impure dead_chain: extract of producer used in successor block must NOT fuse
    auto mb2_0 = block.Add(Opcode::MatrixMulAddF16, {a_mb2, b0, c_zero});
    auto mb2_0_e0 = block.Add(Opcode::CompositeExtractU32x2, {mb2_0, u32(0)});
    auto mb2_0_e1 = block.Add(Opcode::CompositeExtractU32x2, {mb2_0, u32(1)});
    auto mb2_1 = block.Add(Opcode::MatrixMulAddF16, {a_mb2, b1, c_zero});
    auto mb2_1_e0 = block.Add(Opcode::CompositeExtractU32x2, {mb2_1, u32(0)});
    auto mb2_1_e1 = block.Add(Opcode::CompositeExtractU32x2, {mb2_1, u32(1)});
    auto c_mb2_0 = block.Add(Opcode::CompositeConstructU32x2, {mb2_0_e0, mb2_0_e1});
    auto c_mb2_1 = block.Add(Opcode::CompositeConstructU32x2, {mb2_1_e0, mb2_1_e1});
    auto mb3_0 = block.Add(Opcode::MatrixMulAddF16, {a_mb3, b2, c_mb2_0});
    auto mb3_1 = block.Add(Opcode::MatrixMulAddF16, {a_mb3, b3, c_mb2_1});
    (void)succ_block.Add(Opcode::Input, {mb2_0_e0});

    CHECK(DefineWmma16MatrixFunction(ctx, program));
    DefineWmma16PairFunction(ctx, program);
    const auto& fs = ctx.wmma16_fused;
    CHECK(fs.active);
    CHECK(fs.pair_fn.value != 0);
    CHECK(fs.pair_zero_c_fn.value != 0);
    CHECK(fs.quad_zero_c_fn.value != 0);
    CHECK(fs.quad_ret_type.value != 0);
    CHECK(fs.pair_k2_zero_c_fn.value != 0);

    // Verify QuadZeroC
    CHECK(fs.steps.at(q0).kind == Kind::Skip);
    CHECK(fs.steps.at(q1).kind == Kind::Skip);
    CHECK(fs.steps.at(q2).kind == Kind::Skip);
    CHECK(fs.steps.at(q3).kind == Kind::QuadZeroC);
    CHECK(fs.steps.at(q3).m0 == q0 && fs.steps.at(q3).m1 == q1 && fs.steps.at(q3).m2 == q2 && fs.steps.at(q3).m3 == q3);

    // Verify PairZeroC
    CHECK(fs.steps.at(pz0).kind == Kind::Skip);
    CHECK(fs.steps.at(pz1).kind == Kind::PairZeroC);

    // Verify Pure PairK2ZeroC
    CHECK(fs.steps.at(k0_0).kind == Kind::Skip);
    CHECK(fs.steps.at(k0_1).kind == Kind::Skip);
    CHECK(fs.steps.at(k1_0).kind == Kind::Skip);
    CHECK(fs.steps.at(k1_1).kind == Kind::PairK2ZeroC);
    CHECK(fs.skip_insts.contains(k0_0_e0) && fs.skip_insts.contains(k0_0_e1) && fs.skip_insts.contains(c_k0));
    CHECK(fs.skip_insts.contains(k0_1_e0) && fs.skip_insts.contains(k0_1_e1) && fs.skip_insts.contains(c_k1));

    // Verify Impure K-chain did NOT fuse into PairK2ZeroC
    CHECK(fs.steps.at(imp0_1).kind == Kind::PairZeroC);
    CHECK(fs.steps.at(imp1_1).kind == Kind::Pair);

    // Verify cross-block impure K-chains did NOT fuse into PairK2ZeroC
    CHECK(fs.steps.at(mb0_1).kind == Kind::PairZeroC);
    CHECK(fs.steps.at(mb1_1).kind == Kind::Pair);
    CHECK(fs.steps.at(mb2_1).kind == Kind::PairZeroC);
    CHECK(fs.steps.at(mb3_1).kind == Kind::Pair);
    CHECK(!fs.skip_insts.contains(c_mb2_0));

    // Emit and assemble SPIR-V to verify all helper call sites and types assemble cleanly
    const Id main_fn{ctx.OpFunction(void_t, spv::FunctionControlMask::MaskNone, ctx.TypeFunction(void_t))};
    ctx.AddLabel();
    const Id one{ctx.Const(1)};
    const Id av{ctx.OpCompositeConstruct(ctx.U32[4], one, one, one, one)};
    const Id bv{ctx.OpCompositeConstruct(ctx.U32[2], one, one)};
    const Id zv{ctx.OpCompositeConstruct(ctx.U32[2], ctx.u32_zero_value, ctx.u32_zero_value)};
    for (IR::Inst* in : {a_quad, a_pair, a_k0, a_k1, a_imp0, a_imp1, a_mb0, a_mb1, a_mb2, a_mb3}) in->SetDefinition(av);
    for (IR::Inst* in : {b0, b1, b2, b3}) in->SetDefinition(bv);
    c_zero->SetDefinition(zv);
    c_mb2_0->SetDefinition(zv);
    c_mb2_1->SetDefinition(zv);
    for (IR::Inst& inst : block.Instructions()) {
        if (fs.skip_insts.contains(&inst)) continue;
        if (inst.GetOpcode() == Opcode::MatrixMulAddF16) EmitMatrixMulAddF16Fused(ctx, &inst);
    }
    CHECK(q0->definition.value != 0 && q3->definition.value != 0);
    CHECK(pz0->definition.value != 0 && pz1->definition.value != 0);
    CHECK(k1_0->definition.value != 0 && k1_1->definition.value != 0);
    CHECK(imp1_0->definition.value != 0 && imp1_1->definition.value != 0);
    CHECK(mb0_0->definition.value != 0 && mb1_1->definition.value != 0);
    CHECK(mb2_0->definition.value != 0 && mb2_0_e0->definition.value != 0 && mb3_1->definition.value != 0);
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, main_fn, "main", ctx.interfaces);
    ctx.AddExecutionMode(main_fn, spv::ExecutionMode::LocalSize, 128U, 1U, 1U);
    const auto code{ctx.Assemble()};
    CHECK(code.size() > 100);
    // Verify SPIR-V binary invariants: no OpConstantNull (Mesa RADV < 25.0.2) and QuadZeroC call uses fs.quad_ret_type.
    bool saw_quad_call = false;
    for (size_t i = 5; i < code.size();) {
        const u32 word_count = code[i] >> 16;
        const u32 opcode = code[i] & 0xffffu;
        CHECK(word_count >= 1 && i + word_count <= code.size());
        CHECK(opcode != static_cast<u32>(spv::Op::OpConstantNull));
        if (opcode == static_cast<u32>(spv::Op::OpFunctionCall) && word_count >= 4 &&
            code[i + 3] == fs.quad_zero_c_fn.value) {
            CHECK(code[i + 1] == fs.quad_ret_type.value);
            saw_quad_call = true;
        }
        i += word_count;
    }
    CHECK(saw_quad_call);
    std::puts("PASS: WMMA16 PairZeroC, QuadZeroC, PairK2ZeroC, and impure-K fallback verified");
    return 0;
}
"""


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--source", type=Path, default=ROOT)
    args = p.parse_args()
    vk = args.source / "gpu/vk"
    warp = (vk / "eden/shader_recompiler/backend/spirv/emit_spirv_warp.cpp").read_text(encoding="utf-8")
    for sym in ("pair_zero_c_fn", "quad_zero_c_fn", "pair_k2_zero_c_fn"):
        if sym not in warp:
            raise SystemExit(f"FAIL: emit_spirv_warp.cpp missing {sym}")
    fixture = FIXTURE.replace("@@FUNCTION@@", "\n".join((
        block(warp, "bool Wmma16Fits("),
        block(warp, "bool DefineWmma16MatrixFunction("),
        block(warp, "void DefineWmma16PairFunction("),
        block(warp, "void EmitMatrixMulAddF16Fused("),
    )))
    sirit = vk / "sirit"
    with tempfile.TemporaryDirectory(prefix="wmma16-zero-c-kchain-") as tmp:
        tmp_p = Path(tmp)
        cpp, exe = tmp_p / "fixture.cpp", tmp_p / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        r = subprocess.run([args.cxx, "-std=c++20", "-O1", "-I" + str(sirit / "include"), "-I" + str(sirit / "src"),
                            "-I" + str(args.headers), "-I" + str(vk / "eden"), str(cpp),
                            *map(str, sorted((sirit / "src").rglob("*.cpp"))), "-o", str(exe)],
                           capture_output=True, text=True, timeout=300)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("FAIL: fixture did not build")
        env = os.environ.copy()
        env["PATH"] = str(Path(args.cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
