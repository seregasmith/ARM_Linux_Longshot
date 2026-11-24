"""Simulated turf's lists made larger for high-demand area settings (gpu/vk/eden/shader_recompiler/ir_opt/sim_turf_pool_pass.h), on
synthetic IR shaped as the game's programs (tests/sim_turf_pools_ir.cpp): the blade list's producer, the pools' producer and a mesh
reader are made larger (caps come from the descriptor's runtime length, records move to private descriptors before the tables, both pools' records at the same
place), the programs that differ a little are left as they are. And the switch both ways, through vk_shader.cpp's own
sim_turf_pools and sim_turf_pools_on (cut out of the source): unset, the programs are made larger; RX_VK_SIM_TURF_POOLS=0, none is.
Then the wiring: both kinds of program go through it, the translator's stamp keeps the two caches apart, the back end binds
the private descriptors.

What it does not model: the GPU, the game's own programs (their IR is the game's, not committed), the buffers the back end
makes (vk_draw.cpp sim_turf_private_binding); the run in the game is in the pull request.

python -I -B tests/test_sim_turf_pools.py [--cxx clang++]
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
ENV_ON_H = str(VK / "compat" / "longshot_env.h")


def cut_switch(folder):
    """vk_shader.cpp's sim_turf_pools_on and sim_turf_pools, as they are, for the fixture to call"""
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    text = block(shader, "bool sim_turf_pools_on() {") + "\n" + \
        block(shader, "void sim_turf_pools(Shader::IR::Program& program, uint64_t code_key) {") + "\n"
    (folder / "sim_turf_pools_cut.inc").write_text(text, encoding="utf-8")


def build_and_run(cxx):
    sources = sorted((VK / "eden" / "shader_recompiler" / "frontend" / "ir").glob("*.cpp"))
    flags = ["-std=c++20", "-include", ENV_ON_H, "-O1", "-D_CRT_SECURE_NO_WARNINGS", "-DMKWVK_INCLUDES=2"]
    for header in ("algorithm", "cstdio", "cstdlib", "cstring", "functional", "string", "vector"):
        flags += ["-include", header]
    flags += ["-I" + str(p) for p in (VK / "compat", ROOT / "gpu" / "compat", VK / "eden", ROOT / "gpu" / "third_party" / "suyu")]
    with tempfile.TemporaryDirectory(prefix="sim-turf-pools-") as temp:
        folder = Path(temp)
        cut_switch(folder)
        sources = [ROOT / "tests" / "sim_turf_pools_ir.cpp", *sources]
        objs = [folder / ("%03d.o" % i) for i in range(len(sources))]
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
            list(pool.map(lambda job: subprocess.run([cxx, *flags, "-I" + str(folder), "-c", str(job[0]), "-o", str(job[1])],
                                                     check=True), zip(sources, objs)))
        exe = folder / "fixture.exe"
        subprocess.run([cxx, *map(str, objs), "-o", str(exe)], check=True)
        for mode, value in (("on", None), ("off", "0")):
            env = {k: v for k, v in os.environ.items() if k != "RX_VK_SIM_TURF_POOLS"}
            env["PATH"] = str(Path(cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
            if value is not None:
                env["RX_VK_SIM_TURF_POOLS"] = value
            run = subprocess.run([str(exe), mode], capture_output=True, text=True, env=env)
            print(run.stdout, end="")
            print(run.stderr, end="")
            if run.returncode != 0 or "PASS" not in run.stdout:
                raise SystemExit("FAIL: simulated turf's lists on synthetic IR, switch %s" % mode)


def wiring():
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    switches = (VK / "switches.def").read_text(encoding="utf-8")
    fails = []
    # (the producers are compute programs found by key, the readers mesh programs: both translations run the pass)
    if "sim_turf_pools(t->program, p.key);" not in shader:
        fails.append("the compute programs do not go through sim_turf_pools")
    if "sim_turf_pools(mesh_t->program, mesh.key);" not in shader:
        fails.append("the mesh programs do not go through sim_turf_pools")
    # (a program translated with the switch off is another program: the cache keeps them apart)
    stamp = block(shader, "uint64_t translator_stamp() {")
    if "if (!sim_turf_pools_on()) {" not in stamp:
        fails.append("translator_stamp does not tell RX_VK_SIM_TURF_POOLS=0 apart")
    # (a private descriptor is bound by the back end, from the address it stands for)
    if "Shader::IsSimTurfStorage(d)" not in draw or "Shader::SimTurfStorageOrig(d)" not in shader:
        fails.append("the back end does not bind the private descriptors")
    if "SW(RX_VK_SIM_TURF_POOLS, kill, bool," not in switches:
        fails.append("RX_VK_SIM_TURF_POOLS not in switches.def as a kill switch")
    if "bool sim_turf_adaptive()" not in draw or 'env_on_or("RX_VK_SIM_TURF_ADAPTIVE", 1)' not in draw:
        fails.append("RX_VK_SIM_TURF_ADAPTIVE does not default demand sampling on")
    if "sim_turf_capacity::mirror_span" not in draw or "VK_BUFFER_USAGE_TRANSFER_SRC_BIT" not in draw:
        fails.append("private producers do not mirror a stock-compatible record window for unpatched readers")
    if "sim_turf_adaptive() && !sim_turf_monitor_ready" not in draw:
        fails.append("a failed demand monitor does not fail private bindings closed to the game's stock buffers")
    # (the floor is the game's own size since #2174: a list shrunk below it in an area without turf came back empty and
    # stalled the replay as it grew; tests/test_sim_turf_floor.py plays that)
    ctor = " ".join(draw[draw.find("SimTurfPrivate(bool pool, uint32_t maximum)"):].split("{}", 1)[0].split())
    stock = "pool ? Shader::Optimization::kSimTurfPoolCap : Shader::Optimization::kSimTurfBladeCap"
    if f"maximum, {stock})" not in ctor or "RX_VK_SIM_TURF_SUBSTOCK" in draw or "RX_VK_SIM_TURF_SUBSTOCK" in switches:
        fails.append("adaptive buffers do not keep the game's own size as their floor")
    begin = block(draw, "void draw_begin_recording(")
    if "if (sim_turf_adaptive())" not in begin:
        fails.append("RX_VK_SIM_TURF_ADAPTIVE=0 does not skip demand readback and observation")
    opcodes = (VK / "eden" / "shader_recompiler" / "frontend" / "ir" / "opcodes.inc").read_text(encoding="utf-8")
    emitter = (VK / "eden" / "shader_recompiler" / "backend" / "spirv" / "emit_spirv_memory.cpp").read_text(encoding="utf-8")
    if "OPCODE(StorageBufferLength," not in opcodes or "EmitStorageBufferLength" not in emitter:
        fails.append("private-list caps cannot read the descriptor's runtime length")
    if fails:
        raise SystemExit("FAIL: " + "; ".join(fails))
    print("wiring: compute and mesh programs, the cache stamp, the back end's bindings, the switch")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    wiring()
    build_and_run(a.cxx)
    print("PASS: simulated turf's lists made larger")


if __name__ == "__main__":
    main()
