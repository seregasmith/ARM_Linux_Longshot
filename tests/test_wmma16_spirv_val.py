"""The 16x16x16 cooperative matrix multiply DefineWmma16MatrixFunction emits (AMD RDNA 3/4: mma_layout.h mma_wmma16)
must be valid SPIR-V for Vulkan 1.3.

The single and fused functions and planner are cut out of emit_spirv_warp.cpp into a Sirit fixture whose compute shader
calls them and writes their results to a storage buffer, with the capabilities and memory model EmitSPIRV gives a
program that uses cooperative matrices. It is built for each kind of program the game's networks are: one and two
subgroups, with and without the program's own shared memory in an explicit layout (the tiles then follow it in an
Aliased block), and with mma_wmma16_fast off (two K halves) and on (one multiply). Every module must pass spirv-val.
A workgroup whose X width is not a multiple of 32, or a device without full subgroups, must get no function at all
(the software multiply). Nothing runs on a GPU.

	python -I -B tests/test_wmma16_spirv_val.py --headers <SPIRV-Headers/include> [--cxx g++] [--spirv-val spirv-val]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"]
# (spirv-val: SPIRV-Tools comes with the Linux runner's packages, not on Windows)
RUN_ALL_SKIP = {"windows": "needs spirv-val: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
	at = text.index(marker)
	end = text.index("{", at) + 1
	depth = 1
	while depth:
		depth += (text[end] == "{") - (text[end] == "}")
		end += 1
	return text[at:end]


FIXTURE = r"""
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/mma_layout.h"

namespace Shader::Backend::SPIRV {
using Sirit::Id;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
enum class Stage { Compute, Fragment };
namespace IR {
enum class Opcode { Input, MatrixMulAddF16, CompositeExtractU32x2, CompositeConstructU32x2, CompositeConstructU32x4, Identity,
	Barrier, WorkgroupMemoryBarrier, DeviceMemoryBarrier };
struct Inst;
struct Value {
	IR::Inst* inst{};
	u32 immediate{};
	bool is_immediate{};
	Value() = default;
	Value(IR::Inst* i) : inst(i) {}
	Value(u32 v) : immediate(v), is_immediate(true) {}
	Value Resolve() const { return *this; }
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
struct Block {
	std::list<Inst> instructions;
	auto& Instructions() { return instructions; }
	Inst* Add(Opcode op, std::initializer_list<Value> args = {}) {
		instructions.push_back({op, args});
		return &instructions.back();
	}
};
struct Program {   // (what Wmma16Fits and DefineWmma16MatrixFunction read)
	Stage stage = Stage::Compute;
	std::array<u32, 3> workgroup_size{};
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
}

using namespace Shader::Backend::SPIRV;

int main(int argc, char** argv) {
	if (argc < 8) return 2;
	const u32 width = std::atoi(argv[2]), own = std::atoi(argv[4]);
	const bool explicit_layout = argv[3][0] == '1';
	mma_wmma16_fast = argv[5][0] == '1';
	mma_wmma16_full_subgroups = argv[6][0] == '1';
	mma_wmma16_shared_limit = std::atoi(argv[7]);
	mma_wmma16_max_subgroups = 2;
	EmitContext ctx{0x00010600};
	ctx.AddCapability(spv::Capability::Shader);
	ctx.AddCapability(spv::Capability::Float16);
	ctx.AddExtension("SPV_KHR_cooperative_matrix");   // (as EmitSPIRV for a program with cooperative matrices)
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
	ctx.AddCapability(spv::Capability::GroupNonUniform);   // (as DefineInputs for a program with a warp lane)
	ctx.subgroup_local_invocation_id = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Input, ctx.U32[1]),
															 spv::StorageClass::Input);
	ctx.Decorate(ctx.subgroup_local_invocation_id, spv::Decoration::BuiltIn, spv::BuiltIn::SubgroupLocalInvocationId);
	ctx.interfaces.push_back(ctx.subgroup_local_invocation_id);
	const Id runtime{ctx.TypeRuntimeArray(ctx.U32[1])};
	ctx.Decorate(runtime, spv::Decoration::ArrayStride, 4U);
	const Id ssbo_block{ctx.TypeStruct(runtime)};
	ctx.Decorate(ssbo_block, spv::Decoration::Block);
	ctx.MemberDecorate(ssbo_block, 0U, spv::Decoration::Offset, 0U);
	const Id ssbo{ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::StorageBuffer, ssbo_block),
										spv::StorageClass::StorageBuffer)};
	ctx.Decorate(ssbo, spv::Decoration::DescriptorSet, 0U);
	ctx.Decorate(ssbo, spv::Decoration::Binding, 0U);
	ctx.interfaces.push_back(ssbo);
	IR::Program program;
	program.workgroup_size = {width, 1, 1};
	program.shared_memory_size = own;
	IR::Block block;
	program.blocks.push_back(&block);
	using IR::Opcode;
	const auto a = block.Add(Opcode::Input), other_a = block.Add(Opcode::Input);
	const auto a_pz = block.Add(Opcode::Input), a_quad = block.Add(Opcode::Input);
	const auto a_k0 = block.Add(Opcode::Input), a_k1 = block.Add(Opcode::Input);
	const auto b = block.Add(Opcode::Input), c = block.Add(Opcode::Input);
	const auto c_zero = block.Add(Opcode::CompositeConstructU32x2, {u32(0), u32(0)});
	const auto m0 = block.Add(Opcode::MatrixMulAddF16, {a, b, c});
	const auto ext = block.Add(Opcode::CompositeExtractU32x2, {m0, u32(0)});
	const auto identity = block.Add(Opcode::Identity, {ext});
	const auto m1 = block.Add(Opcode::MatrixMulAddF16, {a, b, c});
	const auto m2 = block.Add(Opcode::MatrixMulAddF16, {a, b, c});
	const auto m3 = block.Add(Opcode::MatrixMulAddF16, {a, b, m2});
	const auto m4 = block.Add(Opcode::MatrixMulAddF16, {other_a, b, c});
	block.Add(Opcode::Barrier);
	const auto m5 = block.Add(Opcode::MatrixMulAddF16, {other_a, b, c});
	const auto pz0 = block.Add(Opcode::MatrixMulAddF16, {a_pz, b, c_zero});
	const auto pz1 = block.Add(Opcode::MatrixMulAddF16, {a_pz, b, c_zero});
	const auto q0 = block.Add(Opcode::MatrixMulAddF16, {a_quad, b, c_zero});
	const auto q1 = block.Add(Opcode::MatrixMulAddF16, {a_quad, b, c_zero});
	const auto q2 = block.Add(Opcode::MatrixMulAddF16, {a_quad, b, c_zero});
	const auto q3 = block.Add(Opcode::MatrixMulAddF16, {a_quad, b, c_zero});
	const auto k0_0 = block.Add(Opcode::MatrixMulAddF16, {a_k0, b, c_zero});
	const auto k0_1 = block.Add(Opcode::MatrixMulAddF16, {a_k0, b, c_zero});
	const auto k1_0 = block.Add(Opcode::MatrixMulAddF16, {a_k1, b, k0_0});
	const auto k1_1 = block.Add(Opcode::MatrixMulAddF16, {a_k1, b, k0_1});
	Id own_shared{};
	if (explicit_layout) {   // (as DefineSharedMemory's make with an explicit layout)
		ctx.uses_explicit_workgroup_layout = true;
		ctx.profile.support_explicit_workgroup_layout = true;
		ctx.AddExtension("SPV_KHR_workgroup_memory_explicit_layout");
		ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayoutKHR);
		const Id array_type{ctx.TypeArray(ctx.U32[1], ctx.Const((own + 3) / 4))};
		ctx.Decorate(array_type, spv::Decoration::ArrayStride, 4U);
		const Id struct_type{ctx.TypeStruct(array_type)};
		ctx.MemberDecorate(struct_type, 0U, spv::Decoration::Offset, 0U);
		ctx.Decorate(struct_type, spv::Decoration::Block);
		own_shared = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Workgroup, struct_type),
										   spv::StorageClass::Workgroup);
		ctx.Decorate(own_shared, spv::Decoration::Aliased);
		ctx.interfaces.push_back(own_shared);
	}
	if (!DefineWmma16MatrixFunction(ctx, program)) {
		std::puts("software");
		return 0;
	}
	DefineWmma16PairFunction(ctx, program);
	const auto& fs = ctx.wmma16_fused;
	using Kind = EmitContext::Wmma16Step::Kind;
	if (!fs.active || fs.quad_ret_type.value == 0 || fs.steps.at(m0).kind != Kind::Skip ||
		fs.steps.at(m1).kind != Kind::Pair || fs.steps.at(m1).m0 != m0 ||
		fs.steps.at(m1).m1 != m1 || fs.steps.at(m1).deferred_extracts != std::vector<IR::Inst*>{ext, identity} ||
		fs.steps.at(pz1).kind != Kind::PairZeroC || fs.steps.at(q3).kind != Kind::QuadZeroC ||
		fs.steps.at(k1_1).kind != Kind::PairK2ZeroC ||
		fs.skip_insts.size() != 2 || !fs.skip_insts.contains(ext) || !fs.skip_insts.contains(identity) ||
		fs.steps.contains(m2) || fs.steps.contains(m3) || fs.steps.contains(m4) || fs.steps.contains(m5)) return 3;
	const Id main_fn{ctx.OpFunction(void_t, spv::FunctionControlMask::MaskNone, ctx.TypeFunction(void_t))};
	ctx.AddLabel();
	const Id one{ctx.Const(1)};
	const Id av{ctx.OpCompositeConstruct(ctx.U32[4], one, one, one, one)};
	const Id bc{ctx.OpCompositeConstruct(ctx.U32[2], one, one)};
	const Id zc{ctx.OpCompositeConstruct(ctx.U32[2], ctx.u32_zero_value, ctx.u32_zero_value)};
	a->SetDefinition(av); other_a->SetDefinition(av); a_pz->SetDefinition(av); a_quad->SetDefinition(av);
	a_k0->SetDefinition(av); a_k1->SetDefinition(av); b->SetDefinition(bc); c->SetDefinition(bc); c_zero->SetDefinition(zc);
	for (IR::Inst& inst : block.Instructions()) {
		if (fs.skip_insts.contains(&inst)) continue;
		if (inst.GetOpcode() == Opcode::MatrixMulAddF16) EmitMatrixMulAddF16Fused(ctx, &inst);
	}
	const Id lane{ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id)};
	const Id word_ptr{ctx.TypePointer(spv::StorageClass::StorageBuffer, ctx.U32[1])};
	for (IR::Inst* result : {m0, m1, m2, m3, m4, m5, pz0, pz1, q0, q1, q2, q3, k1_0, k1_1})
		ctx.OpStore(ctx.OpAccessChain(word_ptr, ssbo, ctx.u32_zero_value, lane),
			ctx.OpCompositeExtract(ctx.U32[1], result->definition, 0U));
	ctx.OpStore(ctx.OpAccessChain(word_ptr, ssbo, ctx.u32_zero_value, lane), identity->definition);
	if (own_shared.value != 0) {   // (the program's own words are used too)
		const Id own_ptr{ctx.TypePointer(spv::StorageClass::Workgroup, ctx.U32[1])};
		ctx.OpStore(ctx.OpAccessChain(own_ptr, own_shared, ctx.u32_zero_value, ctx.u32_zero_value), lane);
	}
	ctx.OpReturn();
	ctx.OpFunctionEnd();
	ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, main_fn, "main", ctx.interfaces);
	ctx.AddExecutionMode(main_fn, spv::ExecutionMode::LocalSize, width, 1U, 1U);
	const auto code{ctx.Assemble()};
	std::ofstream(argv[1], std::ios::binary).write(reinterpret_cast<const char*>(code.data()),
												   static_cast<std::streamsize>(code.size() * 4));
	std::puts("wmma16");
	return 0;
}
"""

# name, workgroup X, explicit layout, own shared bytes, fast, full subgroups, shared memory limit (0: none), wanted
CASES = (
	("one subgroup", 32, 0, 0, 0, 1, 0, "wmma16"),
	("one subgroup, one multiply", 32, 0, 0, 1, 1, 0, "wmma16"),
	("two subgroups", 64, 0, 0, 0, 1, 0, "wmma16"),
	("two subgroups, own shared memory, explicit layout", 64, 1, 100, 0, 1, 0, "wmma16"),
	("one subgroup, own shared memory, explicit layout, one multiply", 32, 1, 64, 1, 1, 0, "wmma16"),
	("48 lanes wide", 48, 0, 0, 0, 1, 0, "software"),
	("above subgroup pin limit", 96, 0, 0, 0, 1, 0, "software"),
	("no full subgroups", 64, 0, 0, 0, 0, 0, "software"),
	("two subgroups, tiles fill the shared memory", 64, 0, 0, 0, 1, 2 * 2048 + 4, "wmma16"),
	("two subgroups, tiles past the shared memory", 64, 0, 0, 0, 1, 2 * 2048, "software"),
	("own shared memory, explicit layout, tiles past the shared memory", 32, 1, 100, 0, 1, 112 + 2048, "software"),
)


def main():
	p = argparse.ArgumentParser(description=__doc__)
	p.add_argument("--headers", type=Path, required=True)
	p.add_argument("--cxx", default="g++")
	p.add_argument("--spirv-val", default="spirv-val")
	p.add_argument("--source", type=Path, default=ROOT)
	args = p.parse_args()
	vk = args.source / "gpu/vk"
	warp = (vk / "eden/shader_recompiler/backend/spirv/emit_spirv_warp.cpp").read_text(encoding="utf-8")
	marker = "bool DefineWmma16MatrixFunction(EmitContext& ctx, const IR::Program& program)"
	if marker not in warp:
		raise SystemExit("FAIL: emit_spirv_warp.cpp has no DefineWmma16MatrixFunction")
	if "bool Wmma16Fits(" not in warp:
		raise SystemExit("FAIL: emit_spirv_warp.cpp has no Wmma16Fits")
	fixture = FIXTURE.replace("@@FUNCTION@@", "\n".join((block(warp, "bool Wmma16Fits("), block(warp, marker),
		block(warp, "void DefineWmma16PairFunction("), block(warp, "void EmitMatrixMulAddF16Fused("))))
	sirit = vk / "sirit"
	with tempfile.TemporaryDirectory(prefix="wmma16-spirv-val-") as tmp:
		tmp = Path(tmp)
		cpp, exe = tmp / "fixture.cpp", tmp / "fixture"
		cpp.write_text(fixture, encoding="utf-8")
		r = subprocess.run([args.cxx, "-std=c++20", "-O1", "-I" + str(sirit / "include"), "-I" + str(sirit / "src"),
							"-I" + str(args.headers), "-I" + str(vk / "eden"), str(cpp),
							*map(str, sorted((sirit / "src").rglob("*.cpp"))), "-o", str(exe)],
						   capture_output=True, text=True, timeout=600)
		if r.returncode:
			print(r.stdout + r.stderr)
			raise SystemExit("FAIL: the fixture did not build")
		for n, (name, width, explicit, own, fast, full, limit, want) in enumerate(CASES):
			spv = tmp / ("case%d.spv" % n)
			r = subprocess.run([str(exe), str(spv), str(width), str(explicit), str(own), str(fast), str(full), str(limit)],
							   capture_output=True, text=True, timeout=30)
			if r.returncode or r.stdout.split() != [want]:
				print(r.stdout + r.stderr)
				raise SystemExit("FAIL: %s: the fixture stopped or did not take the %s path" % (name, want))
			if want == "software":
				print("%s: software multiply" % name)
				continue
			r = subprocess.run([args.spirv_val, "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True,
							   timeout=60)
			if r.returncode:
				print(r.stdout + r.stderr)
				raise SystemExit("FAIL: %s: spirv-val" % name)
			print("%s: spirv-val ok" % name)
	print("PASS: every WMMA16 module is valid SPIR-V for Vulkan 1.3, and partial subgroups take the software multiply")


if __name__ == "__main__":
	main()
