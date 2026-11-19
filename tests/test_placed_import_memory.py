"""Imports of the guest's pools placed in the GPU's own memory (gpu/vk/vk_memory.cpp placed_*, the arm64 port) are counted once
by the driver: Turnip, Mesa's Adreno driver, counts every memory object at its buffer object's whole size against its heap
(75% of RAM: 11216 MB on the AYN Odin 3's 15 GB), an import of a buffer object it already holds too. Made as the
placement's memory imported again, the imports counted their placements once more: in Kokiri Forest 3.5 GB of imports over
5.1 GB of placements took the count to 11 GB, and images, placements and imports were refused (VkResult -2) with about 7.4 GB
really held: textures not made, stalls of 4 to 21 s (longshot_log.txt and .1, Turnip 26.2.6).

Cuts placed_back, placed_unback, placed_import and what they call out of vk_memory.cpp, with reap_mirrors (where a retired
import's memory is freed) and imports_give_back's first loop (an idle one's), and runs tests/placed_import_memory_test.cpp:
a stand-in driver that counts as Turnip does, memfds
for its buffer objects, a window made as host/hos_main.c makes it. It checks the count (a placement imported twice counted
once; a pool still placed under a heap its import would have filled) and the memory's life (a placement given back or taken
into a larger one while imports are bound to it keeps its memory until the last of them is freed, an idle import given back
leaves its placement's memory, and nothing is left after).
Then that no other place in vk_memory.cpp frees an import's memory with vkFreeMemory itself, as a placed import's memory is
its placement's. No game, no GPU; Linux only (memfd, mmap).

    python -I tests/test_placed_import_memory.py --headers <Vulkan-Headers include> [--cxx g++] [--sanitize]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import between, block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
RUN_ALL_SKIP = {"windows": "memfd and mmap over the guest window: Linux only, as the placements are"}

ROOT = Path(__file__).resolve().parents[1]
# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/longshot_env.h)
ENV_ON_H = str(ROOT / "gpu" / "vk" / "compat" / "longshot_env.h")
# (the non-Linux placed_find closes the placements' block: what follows it, up to import_pool, is cut too)
AFTER_PLACED = "bool placed_find(uint64_t, uint64_t, GuestPool&) { return false; }\n#endif\n"


def actual(src):
    """The placements' state and functions, in the order they are defined, then reap_mirrors and imports_give_back's first
    loop (the idle imports given back: what follows it is the pressure rules, not run here)"""
    after = between(src, AFTER_PLACED, "bool import_pool(")[len(AFTER_PLACED):]
    give_back = block(src, "void imports_give_back()")
    give_back = give_back[:give_back.index("    // (longshot) Default by OS and vendor")] + "}\n"
    return "\n".join([
        between(src, "struct Placement {", "/// New memory of the device's"),
        block(src, "void destroy_import_buffer("),
        block(src, "VkDeviceMemory placed_allocate("),
        block(src, "int placed_export("),
        block(src, "VkDeviceMemory placed_reimport("),
        block(src, "void placed_back("),
        block(src, "void placed_unback("),
        block(src, "bool placed_import("),
        after,
        block(src, "void reap_mirrors() {"),
        give_back,
    ])


def direct_frees(src):
    """The lines that free an import's or a placement's memory member with vkFreeMemory itself. import_pool's own rollback
    is left out: it frees the host-pointer memory it allocated a few lines before, never a placement's"""
    own = block(src, "bool import_pool(")
    rest = src.replace(own, "\n" * own.count("\n"))
    return [(n, line.strip()) for n, line in enumerate(rest.splitlines(), 1)
            if re.search(r"vkFreeMemory\(\s*dev\.device\s*,\s*[^,;]*(?:\.|->)memory\s*,", line)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()
    src = (a.source / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    fixture = (ROOT / "tests/placed_import_memory_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* DEAD IMPORT */", block(src, "struct DeadImport {") + ";")
    fixture = fixture.replace("/* ACTUAL */", actual(src))
    with tempfile.TemporaryDirectory(prefix="placed-import-memory-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", "-pthread", "-I" + str(a.headers), "-I" + str(ROOT / "gpu/vk"),
                        *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if a.sanitize else []),
                        str(cpp), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        sys.stdout.write(run.stdout)
        if run.returncode != 0:
            sys.stderr.write(run.stderr)
            raise SystemExit("placed imports: the driver's count or the placements' memory went wrong (above)")
    # (the other places an import's memory is freed: prefetched imports dropped, idle ones given back, a trim's rollback.
    # A placed import's memory is its placement's, which the placement and every import bound to it hold: freed there with
    # vkFreeMemory, it would go under the placement and the other imports while the GPU reads them)
    frees = direct_frees(src)
    if frees:
        raise SystemExit("vk_memory.cpp frees an import's or a placement's memory with vkFreeMemory itself, where a placed "
                         "import's memory is its placement's, shared:\n" +
                         "\n".join(f"  line {n}: {line}" for n, line in frees))
    print("placed imports: no import's memory freed past its holders")


if __name__ == "__main__":
    main()
