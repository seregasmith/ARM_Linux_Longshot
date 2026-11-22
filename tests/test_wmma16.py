"""HMMA_nx2 through 16x16x16 cooperative matrices (AMD RDNA 3/4: mma_layout.h mma_wmma16).

Checks that the pieces are wired (Sirit's store, the probe, the emitter, the program cache's stamp, the network gate),
and runs the tile layout DefineWmma16MatrixFunction emits: the lanes' stores are read from the source, a 16x16x16
multiply is done on the tiles as the device would (row major A and C, column major B, stride 8 words), and each lane's
two words of D must be the console's fragment (PTX m16n8k16) of A*B+C. The tiles are used only where every subgroup is
full (32 lanes): the device has computeFullSubgroups and the workgroup's X width is a multiple of 32, as pin_subgroup
then creates the pipeline with REQUIRE_FULL_SUBGROUPS; otherwise the software multiply.
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu/vk"
SPIRV = VK / "eden/shader_recompiler/backend/spirv"


def need(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def wiring():
    sirit_h = (VK / "sirit/include/sirit/sirit.h").read_text(encoding="utf-8")
    arith = (VK / "sirit/src/instructions/arithmetic.cpp").read_text(encoding="utf-8")
    need("Id OpCooperativeMatrixStoreKHR(" in sirit_h and "Id Module::OpCooperativeMatrixStoreKHR(" in arith,
         "Sirit has no OpCooperativeMatrixStoreKHR")
    store = block(arith, "Id Module::OpCooperativeMatrixStoreKHR(")
    need("OpId{" not in store, "OpCooperativeMatrixStoreKHR has no result id, yet one is written")
    layout = (SPIRV / "mma_layout.h").read_text(encoding="utf-8")
    need("inline bool mma_wmma16 = false;" in layout and "mma_wmma16_tile_words = 4 * 128;" in layout,
         "mma_layout.h lacks mma_wmma16")
    coop = (VK / "vk_coopmat.cpp").read_text(encoding="utf-8")
    init = block(coop, "void coopmat_init(")
    need("shape16" in init and re.search(r"if \(!shape && shape16\) \{[^\n]*\n\s*wmma16_init\(\);", init),
         "coopmat_init does not take the 16x16x16 path when 16x8x16 is missing")
    need("bool test_multiply_wmma16()" in coop and "test_multiply_wmma16()" in block(coop, "void wmma16_init("),
         "wmma16_init does not check the multiply")
    need("extra_interfaces" in block(coop, "std::vector<uint32_t> build_shader("),
         "the probe's workgroup tiles are not entry point interfaces")
    emit = (SPIRV / "emit_spirv.cpp").read_text(encoding="utf-8")
    need("!DefineWmma16MatrixFunction(ctx, program))" in emit and "DefineWmma16PairFunction(ctx, program);" in emit,
         "EmitSPIRV does not define the WMMA16 single and paired functions")
    ctx_h = (SPIRV / "spirv_emit_context.h").read_text(encoding="utf-8")
    need("Id wmma16_function{};" in ctx_h, "EmitContext has no wmma16_function")
    warp = (SPIRV / "emit_spirv_warp.cpp").read_text(encoding="utf-8")
    mul = block(warp, "Id EmitMatrixMulAddF16(")
    need("ctx.wmma16_function" in mul and "ctx.software_matrix_function.value != 0" in mul,
         "EmitMatrixMulAddF16 does not call the function that was defined")
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    need("(mma_wmma16 ? 65536u : 0u)" in shader and "(mma_wmma16_fast ? 131072u : 0u)" in shader and
         "(mma_wmma16 && mma_wmma16_fused ? 262144u : 0u)" in shader,
         "the program cache's stamp does not tell WMMA16 programs apart")
    gate_line = [l for l in shader.split("\n") if "dev.policy.networks_left_out &&" in l and "network_left_out" not in l]
    need(len(gate_line) == 1, "the network gate was not found (%d)" % len(gate_line))
    need("Wmma16Fits(profile_pinned(uint64_t(wg[0]) * wg[1] * wg[2]), t->program)" in gate_line[0],
         "a network the WMMA16 tiles do not run is kept on AMD, and compiled as the software multiply the gate leaves out")
    need("inline bool mma_wmma16_full_subgroups = false;" in layout, "mma_layout.h lacks mma_wmma16_full_subgroups")
    wi = block(coop, "void wmma16_init(")
    need(re.search(r"if \(!dev\.vk13\.computeFullSubgroups\) \{[^}]*software_multiplies", wi) and
         wi.index("computeFullSubgroups") < wi.index("mma_wmma16_full_subgroups = true;") < wi.index("mma_wmma16 = true;"),
         "wmma16_init turns the tiles on without full compute subgroups")
    need("[[nodiscard]] bool Wmma16Fits(const Profile& profile, const IR::Program& program);" in (SPIRV / "emit_spirv.h").read_text(encoding="utf-8"),
         "emit_spirv.h does not declare Wmma16Fits")
    fn = block(warp, "bool DefineWmma16MatrixFunction(")
    call = "if (!Wmma16Fits(ctx.profile, program)) return false;"
    need(call in fn and "const u32 threads" in fn and fn.index(call) < fn.index("const u32 threads"),
         "DefineWmma16MatrixFunction does not take its tiles exactly where Wmma16Fits says")
    fits = block(warp, "bool Wmma16Fits(")
    gate = fits[:fits.index("const uint64_t threads")] if "const uint64_t threads" in fits else ""
    need("profile.support_explicit_workgroup_layout" in fits and "mma_wmma16_shared_limit" in fits,
         "Wmma16Fits does not follow the explicit layout and the shared memory limit")
    need("!mma_wmma16_full_subgroups" in gate and "program.workgroup_size[0] % 32 != 0" in gate and "return false;" in gate,
         "DefineWmma16MatrixFunction runs the tiles in a workgroup whose subgroups may lack lanes")
    need("program.stage != Stage::Compute" in gate and "32ull * mma_wmma16_max_subgroups" in fits,
         "WMMA16 must use a pinned compute workgroup within the subgroup limit")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    pin = block(draw, "void pin_subgroup(")
    need("!(workgroup_x % 32) && dev.vk13.computeFullSubgroups" in pin and "REQUIRE_FULL_SUBGROUPS_BIT" in pin,
         "pin_subgroup no longer asks for full subgroups where the tiles are used")
    need(draw.count("pin_subgroup(st, p.workgroup[0]") == 1,
         "a compute pipeline is not pinned with its workgroup width")
    return warp


def tiles(warp):
    fn = block(warp, "bool DefineWmma16MatrixFunction(")
    stores = re.findall(r"store\(word\((region_\w+), (\d+), (\d+)\), (part\((matrix_\w), (\d)\)|zero|mma_wmma16_fast \? part\(matrix_b, 1\) : zero)\);", fn)
    need(len(stores) >= 14, "the lanes' stores were not found (%d)" % len(stores))
    regions = {"region_a": 0, "region_b_lo": 1, "region_b_hi": 2, "region_c": 3}
    need("stride{imm(8)}" in fn and "region * 128 + plus_rows * 8 + plus_words" in fn, "the tile addressing changed")

    def a_val(r, k): return ((r * 7 + k * 3 + 1) % 5) - 2
    def b_val(k, n): return ((k * 5 + n * 11 + 2) % 4) - 1
    def c_val(r, n): return ((r * 3 + n * 13) % 9) - 4

    # the console's fragments (vk_coopmat.cpp console_a/b/c), as pairs of halves per word
    def frag(t):
        g, i = t // 4, t % 4
        a = [(a_val(g + ((e >> 1) & 1) * 8, 2 * i + (e & 1) + (e >> 2) * 8)) for e in range(8)]
        b = [(b_val(2 * i + (e & 1) + (e >> 1) * 8, g)) for e in range(4)]
        c = [(c_val(g + (e >> 1) * 8, 2 * i + (e & 1))) for e in range(4)]
        return {"a": [a[0:2], a[2:4], a[4:6], a[6:8]], "b": [b[0:2], b[2:4]], "c": [c[0:2], c[2:4]]}

    mem = [[None, None] for _ in range(512)]
    for t in range(32):
        g, i = t // 4, t % 4
        f = frag(t)
        for region, rows, words, value, mat, idx in stores:
            w = regions[region] * 128 + int(rows) * 8 + int(words) + g * 8 + i
            if value == "zero" or value.startswith("mma_wmma16_fast"):
                mem[w] = [0, 0]
            else:
                mem[w] = list(f[mat[-1]][int(idx)])
    need(all(x[0] is not None for x in mem[0:128]) and all(x[0] is not None for x in mem[384:512]),
         "A or C tiles have words no lane writes")

    def half(base, index):
        return mem[base + index // 2][index & 1]

    def row(base, r, c): return half(base, r * 16 + c)
    def col(base, r, c): return half(base, c * 16 + r)

    d = [[sum(row(0, r, k) * (col(128, k, n) + col(256, k, n)) for k in range(16)) + row(384, r, n)
          for n in range(16)] for r in range(16)]
    bad = 0
    for t in range(32):
        g, i = t // 4, t % 4
        for e in range(4):
            r, n = g + (e >> 1) * 8, 2 * i + (e & 1)
            got = d[r][n]   # (D stored row major over C, each lane reads words (g, i) and (g + 8, i))
            want = c_val(r, n) + sum(a_val(r, k) * b_val(k, n) for k in range(16))
            bad += got != want
    need(bad == 0, "%d of 128 elements of D are not A*B+C at the console's positions" % bad)

    # Also verify DefineWmma16PairFunction's full 16x16 N-paired layout ([B0|B1], [C0|C1] -> [D0|D1], 256 elements)
    pfn = block(warp, "void DefineWmma16PairFunction(")
    pstores = re.findall(r"store\(pword\((region_\w+), (\d+), (\d+)\), (part\((p[abc]\d?), (\d)\)|zero)\);", pfn)
    need(len(pstores) >= 16, "the paired lanes' stores were not found (%d)" % len(pstores))
    # Take the exact-K=8 branch (last 8 B stores + A + C)
    pmem = [[None, None] for _ in range(512)]
    for t in range(32):
        g, i = t // 4, t % 4
        fa = [(a_val(g + ((e >> 1) & 1) * 8, 2 * i + (e & 1) + (e >> 2) * 8)) for e in range(8)]
        fb0 = [(b_val(2 * i + (e & 1) + (e >> 1) * 8, g)) for e in range(4)]
        fb1 = [(b_val(2 * i + (e & 1) + (e >> 1) * 8, g + 8)) for e in range(4)]
        fc0 = [(c_val(g + (e >> 1) * 8, 2 * i + (e & 1))) for e in range(4)]
        fc1 = [(c_val(g + (e >> 1) * 8, 2 * i + (e & 1) + 8)) for e in range(4)]
        pm = {"pa": [fa[0:2], fa[2:4], fa[4:6], fa[6:8]], "pb0": [fb0[0:2], fb0[2:4]], "pb1": [fb1[0:2], fb1[2:4]],
              "pc0": [fc0[0:2], fc0[2:4]], "pc1": [fc1[0:2], fc1[2:4]]}
        for region, rows, words, value, mat, idx in pstores:
            w = regions[region] * 128 + int(rows) * 8 + int(words) + g * 8 + i
            pmem[w] = [0, 0] if value == "zero" else list(pm[mat][int(idx)])
    need(all(x[0] is not None for x in pmem), "paired tiles have words no lane writes")
    def phalf(base, index): return pmem[base + index // 2][index & 1]
    def prow(base, r, c): return phalf(base, r * 16 + c)
    def pcol(base, r, c): return phalf(base, c * 16 + r)
    pd = [[sum(prow(0, r, k) * (pcol(128, k, n) + pcol(256, k, n)) for k in range(16)) + prow(384, r, n)
           for n in range(16)] for r in range(16)]
    pbad = 0
    for t in range(32):
        g, i = t // 4, t % 4
        for half_idx in range(2):
            for e in range(4):
                r, n = g + (e >> 1) * 8, 2 * i + (e & 1) + half_idx * 8
                pbad += pd[r][n] != (c_val(r, n) + sum(a_val(r, k) * b_val(k, n) for k in range(16)))
    need(pbad == 0, "%d of 256 elements of paired [D0|D1] are not A*[B0|B1]+[C0|C1]" % pbad)


def main():
    global VK, SPIRV
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    a = p.parse_args()
    VK = a.source / "gpu/vk"
    SPIRV = VK / "eden/shader_recompiler/backend/spirv"
    tiles(wiring())
    print("HMMA_nx2 through 16x16x16 cooperative matrices: wired, and the tiles give A*B+C at the console's positions: ok")


if __name__ == "__main__":
    main()
