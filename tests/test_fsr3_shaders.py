"""The checked-in FSR 3.1 pass shaders still match the FidelityFX SDK sources they came from.

gpu/vk/shaders/fsr3_*_comp_spv.h are generated from third_party/fidelityfx-sdk by
scripts/shaders.py fsr3 and checked in like every other shader here, so a clone builds without
glslang. That only stays true if the headers and their sources cannot drift apart, which is what
this check is for: it recompiles every variant and compares. It also validates each module, so a
pass that compiles but is not valid SPIR-V is caught here rather than on a device.

python -I -B tests/test_fsr3_shaders.py [--glslang glslangValidator] [--spirv-val spirv-val]
"""
import argparse
import subprocess
import sys
from pathlib import Path

RUN_ALL_ARGS = ["--glslang", "{glslang}", "--spirv-val", "{spirv_val}"]
# (glslang: a Linux runner package. The headers it checks are SPIR-V, the same on every platform)
RUN_ALL_SKIP = {"windows": "needs glslang: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]


def check_other_version():
    """shaders.py other_version: SPIR-V that differs between headers of two glslang versions is no source mismatch, and a
    header without the version line against one with it counts as two versions; the same version (or none on both) does not"""
    import contextlib
    import importlib.util
    import io
    spec = importlib.util.spec_from_file_location("shaders_tool", ROOT / "scripts/shaders.py")
    sys.path.insert(0, str(ROOT / "scripts"))
    shaders = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(shaders)
    with_version = "\t// 1116.6.0\nconst uint32_t x[] = {0x07230203};\n"
    other = "\t// 1115.0.0\nconst uint32_t x[] = {0x07230203};\n"
    without = "const uint32_t x[] = {0x07230203};\n"
    with contextlib.redirect_stderr(io.StringIO()):
        cases = {"two versions": (with_version, other, True), "none against one": (without, with_version, True),
                 "one against none": (with_version, without, True), "the same version": (with_version, with_version, False),
                 "none on both": (without, without, False)}
        for what, (old, new, want) in cases.items():
            if shaders.other_version("x", old, new) != want:
                sys.exit("shaders.py other_version is wrong for %s: want %s" % (what, want))


def main():
    check_other_version()
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--glslang")
    p.add_argument("--spirv-val")
    a = p.parse_args()
    cmd = [sys.executable, "-I", "-B", str(ROOT / "scripts/shaders.py"), "fsr3", "--check"]
    if a.glslang:
        cmd += ["--glslang", a.glslang]
    if a.spirv_val:
        cmd += ["--spirv-val", a.spirv_val]
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
