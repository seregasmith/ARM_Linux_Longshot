"""Compile Longshot's checked-in shaders into the SPIR-V headers gpu/vk includes, or check that the headers still match.

The headers are checked in, so a clone builds without glslang; this makes them again from their sources. Player builds
include the headers and need neither glslang nor this script.

    python -I -B scripts/shaders.py crt  [--check] [--glslang <exe>]
        gpu/vk/shaders/crt_*_comp_spv.h: the hand-adapted CRT compute filters (their unchanged libretro sources and notices
        are in third_party/crt_shaders; this is not a Slang loader). Each header starts with its .comp's SHA-256 and command,
        then keeps the shader's opening notice. --check always checks that prefix; with no glslang found it stops there
    python -I -B scripts/shaders.py fsr3 [--check] [--glslang <exe>] [--spirv-val <exe>]
        gpu/vk/shaders/fsr3_*_comp_spv.h: AMD's FSR 3.1 upscaler, frame interpolation and optical flow passes, from
        third_party/fidelityfx-sdk (frame interpolation's GPU headers patched on a copy: FI_PATCHES). --spirv-val validates
        each module too
    python -I -B scripts/shaders.py host [--check] [--glslang <exe>]
        gpu/vk/host_shaders/*_comp_spv.h: the renderer's texture shaders (Eglantine's, nx2), keeping each header's opening
        comment with the shader's authors and licenses
    python -I -B scripts/shaders.py all  [--check] [--glslang <exe>] [--spirv-val <exe>]
        crt, fsr3 and host, in that order
    python -I scripts/shaders.py spirv-operands <spirv.core.grammar.json> [<out>, default gpu/vk/spirv_operands.h]
        SPIR-V's core grammar (SPIRV-Headers) as a table of each opcode's operands, for code that renames ids in SPIR-V it
        did not write (gpu/vk/fsr4_tune.h)

--check compares instead of writing, and nothing is written until every shader of a set has compiled. glslang writes its
own version into each header (`// 1116.6.0`), and another version may emit different but equivalent SPIR-V: --check says
so as a note, not a failure, since it says nothing about the sources (crt and host compare the SPIR-V words, fsr3 the
whole header but that line). The version in the checked-in headers' first line gives their bytes exactly. The tests
tests/test_crt_shaders.py, test_fsr3_shaders.py and test_host_shaders.py run --check.
"""
import argparse
import difflib
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script's folder off the path)
import helpers  # noqa: E402

ROOT = Path(helpers.ROOT)


# ---- shared: glslang and its --vn headers ----

def find_glslang(path=None):
    """glslangValidator: path when given (None when it is not an executable), else glslangValidator or glslang on PATH"""
    if path:
        return shutil.which(path)
    return shutil.which("glslangValidator") or shutil.which("glslang")


def need_glslang(path=None):
    glslang = find_glslang(path)
    if not glslang:
        raise SystemExit("glslang not found; pass --glslang with its executable path")
    return glslang


def run_glslang(cmd, what):
    """glslang with these arguments; its output is shown only when it fails"""
    helpers.run(cmd, f"{what}: glslang")


def glslang_version(text):
    """glslang's version line in a --vn header (`\t// 1116.6.0`), or None"""
    found = re.search(r"^\t// (\d+\.\d+\.\d+)$", text, re.M)
    return found.group(1) if found else None


def other_version(name, old, new):
    """Whether the checked-in header old and the one made now, new, whose SPIR-V differ, come from two glslang versions
    (said on stderr: not a source mismatch) rather than from two sources"""
    old_version, new_version = glslang_version(old), glslang_version(new)
    if old_version == new_version:   # (a header without the version line against one with it: two versions too)
        return False
    old_version, new_version = old_version or "unnamed version", new_version or "unnamed version"
    print(f"{name}: glslang {new_version} gives different SPIR-V from the checked-in {old_version}'s; not a source "
          f"mismatch (use glslang {old_version} to compare)", file=sys.stderr)
    return True


def spirv_words(text):
    """The SPIR-V words of a glslang --vn header (its array, after the first brace)"""
    return re.findall(r"0x[0-9a-fA-F]{8}", text[text.index("{"):])


# ---- crt: gpu/vk/shaders/crt_*_comp_spv.h ----

CRT_DIR = ROOT / "gpu/vk/shaders"
CRT_SHADERS = ("crt_lottes", "crt_hyllian", "crt_zfast", "crt_downsample")
CRT_TARGET_ENV = "vulkan1.2"


def crt_source_line(comp, name):
    """The header's first line: the .comp's SHA-256 (CRLF read as LF, as tests/test_fg_scale_sharpen.py's) and the command,
    under the name scripts/crt_shaders.py, which the checked-in headers carry: --check matches them byte for byte"""
    digest = hashlib.sha256(comp.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    return (f"// {comp.name} sha256 {digest} (scripts/crt_shaders.py: "
            f"glslangValidator -V --target-env {CRT_TARGET_ENV} --vn {name.upper()}_COMP_SPV)\n")


def crt_prefix(comp, name):
    """The source line, then every opening attribution/licence comment of the .comp, kept in the generated header too"""
    text = comp.read_text(encoding="utf-8")
    version, _, rest = text.partition("\n")
    if version != "#version 450" or "layout(local_size_x" not in rest:
        raise SystemExit(f"{comp.name}: expected #version 450 and the compute layout after the opening notice")
    return crt_source_line(comp, name) + rest[:rest.index("layout(local_size_x")]


def crt_words(text, name):
    """This shader's glslang --vn array as integers, not numbers from its comments"""
    array = re.search(r"\b" + name.upper() + r"_COMP_SPV\s*\[\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not array:
        raise SystemExit(f"{name}_comp_spv.h: missing {name.upper()}_COMP_SPV array")
    result = re.findall(r"0x[0-9a-fA-F]{8}", array.group(1))
    if not result or result[0].lower() != "0x07230203":
        raise SystemExit(f"{name}_comp_spv.h: missing SPIR-V magic")
    return [int(word, 16) for word in result]


def crt_checked_header(comp, name):
    """The checked-in header's glslang output, once its source line and notice are the .comp's"""
    header = comp.with_name(name + "_comp_spv.h")
    if not header.is_file():
        raise SystemExit(f"{header.name}: missing; run python -I -B scripts/shaders.py crt")
    text = header.read_text(encoding="utf-8")
    opening = crt_prefix(comp, name)
    if not text.startswith(opening):
        raise SystemExit(f"{header.name}: source hash, command or notice is stale; run python -I -B scripts/shaders.py crt")
    body = text[len(opening):]
    crt_words(body, name)
    return body


def crt_compile(glslang, comp, name, out):
    run_glslang([glslang, "-V", "--target-env", CRT_TARGET_ENV, "--vn", name.upper() + "_COMP_SPV",
                 "-o", str(out), str(comp)], comp.name)
    return out.read_text(encoding="utf-8")


def crt(glslang_path=None, check=False):
    glslang = find_glslang(glslang_path)
    if not glslang and (glslang_path or not check):
        need_glslang(glslang_path)
    old = {name: crt_checked_header(CRT_DIR / (name + ".comp"), name) for name in CRT_SHADERS} if check else {}
    if not glslang:
        print(f"CRT shaders: {len(CRT_SHADERS)} source hashes/notices match; no glslang, compilation not checked")
        return

    bad, different_version, made = [], [], {}
    with tempfile.TemporaryDirectory(prefix="crt-shaders-") as tmp:
        for name in CRT_SHADERS:
            comp = CRT_DIR / (name + ".comp")
            body = crt_compile(glslang, comp, name, Path(tmp) / (name + "_comp_spv.h"))
            new_words = crt_words(body, name)
            if check:
                if new_words != crt_words(old[name], name):
                    (different_version if other_version(name, old[name], body) else bad).append(name)
            else:
                made[name] = crt_prefix(comp, name) + body
    if bad:
        raise SystemExit("CRT headers differ from their sources compiled by the same glslang: " + ", ".join(bad) +
                         "; run python -I -B scripts/shaders.py crt")
    if check:
        print(f"CRT shaders: {len(CRT_SHADERS)} source hashes/notices match and compile; "
              f"{len(CRT_SHADERS) - len(different_version)} SPIR-V arrays match, "
              f"{len(different_version)} differ with another compiler version")
    else:
        for name in CRT_SHADERS:
            (CRT_DIR / (name + "_comp_spv.h")).write_text(made[name], encoding="utf-8", newline="\n")
        print(f"CRT shaders: {len(CRT_SHADERS)} headers written")


# ---- host: gpu/vk/host_shaders/*_comp_spv.h ----

HOST_DIR = ROOT / "gpu/vk/host_shaders"
HOST_SHADERS = ("astc_decoder", "bc7_encoder", "bc_encoder", "block_linear_unswizzle_2d")
HOST_TARGET_ENV = "vulkan1.1"   # (as Eden builds them: the bytes match only with this target)


def host_opening(text):
    """A header's own comment: everything above glslang's output, which starts at its tab-indented version line"""
    i = text.find("\t// ")
    return text[:i] if i > 0 else ""


def host(glslang_path=None, check=False):
    glslang = need_glslang(glslang_path)
    bad, made = [], {}
    with tempfile.TemporaryDirectory(prefix="host-shaders-") as tmp:
        for name in HOST_SHADERS:
            header = HOST_DIR / (name + "_comp_spv.h")
            out = Path(tmp) / header.name
            comp = HOST_DIR / (name + ".comp")
            run_glslang([glslang, "-V", "--quiet", "--target-env", HOST_TARGET_ENV, "--vn", name.upper() + "_COMP_SPV",
                         "-o", str(out), str(comp)], comp.name)
            new = out.read_text(encoding="utf-8")
            old = header.read_text(encoding="utf-8") if header.exists() else ""
            if not check:
                made[header] = host_opening(old) + new
            elif spirv_words(new) != spirv_words(old) and not other_version(header.name, old, new):
                bad.append(header.name)
    if bad:
        raise SystemExit("host shaders out of date with their .comp sources: %s (python -I -B scripts/shaders.py host)"
                         % ", ".join(bad))
    for header, text in made.items():
        header.write_text(text, encoding="utf-8", newline="\n")
    print(f"host shaders: {len(HOST_SHADERS)} {'match' if check else 'written'}")


# ---- fsr3: gpu/vk/shaders/fsr3_*_comp_spv.h ----
# The FidelityFX SDK ships each pass as GLSL that includes the SDK's own GPU headers, so the upscaler's eleven variants are
# its eight passes compiled with different options; frame interpolation's ten and optical flow's seven follow.

FFX_SDK = ROOT / "third_party/fidelityfx-sdk"
FFX_GPU = FFX_SDK / "include/FidelityFX/gpu"
FSR3_PASSES = FFX_SDK / "shaders/fsr3upscaler"
FI_PASSES = FFX_SDK / "shaders/frameinterpolation"
OF_PASSES = FFX_SDK / "shaders/opticalflow"
FSR3_OUT = ROOT / "gpu/vk/shaders"
FSR3_TARGET_ENV = "spirv1.3"

# The option set the renderer records these passes under. Each one is a promise about the inputs:
# the colour is HDR, the motion is low-resolution and not jittered, and the depth is reversed. They
# have to match what gpu/vk/vk_fsr3.cpp hands the passes, which is why they live in one place.
FSR3_OPTIONS = [
    "-DFFX_GPU=1",
    "-DFFX_GLSL=1",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",
    "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1",
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1",
    "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0",
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1",
]

# name, the pass GLSL it compiles, extra defines. The _nowave variants are for devices without the
# subgroup operations the single-pass downsampler prefers.
FSR3_VARIANTS = [
    ("prepare_inputs", "prepare_inputs", []),
    ("luma_pyramid", "luma_pyramid", []),
    ("luma_pyramid_nowave", "luma_pyramid", ["-DFFX_SPD_NO_WAVE_OPERATIONS=1"]),
    ("shading_change_pyramid", "shading_change_pyramid", []),
    ("shading_change_pyramid_nowave", "shading_change_pyramid", ["-DFFX_SPD_NO_WAVE_OPERATIONS=1"]),
    ("shading_change", "shading_change", []),
    ("prepare_reactivity", "prepare_reactivity", []),
    ("luma_instability", "luma_instability", []),
    ("accumulate", "accumulate", []),
    ("accumulate_sharpen", "accumulate", ["-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=1"]),
    ("rcas", "rcas", []),
]


# (longshot) Frame interpolation and the optical flow it runs on, the same arrangement: AMD ships
# each pass as GLSL over the SDK's own GPU headers, and the renderer records them as SPIR-V headers
# beside the upscaler's. The options are the same promises about the same inputs -- the depth is
# reversed and the motion is at the render extent -- because the inputs are the ones the upscaler is
# already given (gpu/vk/vk_fsr3fi.cpp). The names are interpolation's own, not the upscaler's: its
# low-resolution motion option is LOW_RES_, where the upscaler's is LOW_RESOLUTION_. A name the SDK
# does not read is dropped without a word, so tests/test_fsr3_shader_options.py checks each one.
FI_OPTIONS = [
    "-DFFX_GPU=1",
    "-DFFX_GLSL=1",
    "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=1",
    "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1",
    "-DFFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS=0",
]
OF_OPTIONS = ["-DFFX_GPU=1", "-DFFX_GLSL=1"]

# (longshot) Frame interpolation's GPU headers with two changes of ours, made on a copy at compile time: third_party/
# stays AMD's, byte for byte (scripts/check_third_party.py), and what changes is written here, line by line, each one
# found exactly as many times as it says or the script stops. gpu/vk/vk_fsr3fi.cpp fills what the changes read.
#
#  1. A made frame anywhere between the two game frames, not only the midpoint. AMD's passes fix t at 0.5: the vector
#     fields keep half the motion, put each vector at the pixel's place half way, and look half a motion back into the
#     frame before and half forward into this one. cbFI's padding word (_pad1) carries t instead (0: the frame before, 1:
#     this one), and the fields keep half the motion as before, scaled where it is used: 2t of it back to the frame before,
#     2(1 - t) forward to this one, the vector put at the pixel's place 2(1 - t) of it from this frame's, and the blend's
#     weights t and 1 - t. At t = 0.5 every scale is 1 and the passes are AMD's.
#  2. The HUD kept where it is (dispatchFlags bit 16, set unless RX_VK_FSR_FG_HUD=0). This game draws its HUD into the
#     same picture as the scene and has no HUD-less colour, so AMD's passes moved it with the scene behind it: smeared
#     icons and text, a copy of the HUD trailing the camera. A pixel the same in both frames while the scene there moves
#     is the HUD (or a flat colour, where the frame's own pixel is right too): it is this frame's pixel. A sample of the
#     frame before or this one that lands on such a pixel is the HUD, not the scene behind it, which neither frame shows:
#     where only one of the two samples does, the other is taken.
#  3. The HUD marked, not guessed (dispatchFlags bit 17, vk_fsr3fi.cpp kHudMask): the guess above misses the HUD's
#     see-through parts (the minimap, the button prompts' backing), its soft edges and anything of it that animates, and
#     those still moved with the scene. Where the renderer has it, the colour's alpha says which pixels are
#     the HUD (0: vk_framegen.cpp hud_source, what the game drew over its picture after the upscale), in this frame and,
#     through the previous source's copy, the frame before: those pixels are what 2. calls the same in both frames.
#  4. HUD-less mode's overlay taken whole. AMD lays the presented frame over the made one where it differs from the
#     HUD-less colour, by a factor that grows with the difference, for a HUD-less colour that is only near the presented
#     frame's. The renderer's (vk_framegen.cpp hud_source, shaders/fg_hud.comp) is the presented frame's to the bit
#     wherever nothing was drawn over the picture, so any difference is the HUD or an effect drawn after the upscale (the
#     fairy's glow): AMD's factor took its faint parts (soft edges, a glow's fall-off) at a fraction, the scene behind them
#     shown instead, and the made frames had a cut edge around them. Taken whole, they are this frame's.
FI_T = "InterpolationT()"
FI_PATCHES = {
    "ffx_frameinterpolation_inpainting.h": [
        ("            const FfxFloat32 fStaticFactor = CalculateStaticContentFactor(RawRGBToLinear(fCurrentInterpolationSource), "
         "RawRGBToLinear(fPresentColor));\n",
         "            const FfxFloat32 fStaticFactor = 1.0f;   // (longshot) the presented frame's pixel, whole (FI_PATCHES 4)\n", 1),
    ],
    "ffx_frameinterpolation_callbacks_glsl.h": [
        ("        FfxInt32        _pad1;\n",
         "        FfxFloat32      fInterpolationT;   // (longshot) the made frame's place: 0 the frame before, 1 this one\n", 1),
        ("    FfxFloat32 TanHalfFoV()\n    {\n        return cbFI.fTanHalfFOV;\n    }\n",
         "    FfxFloat32 TanHalfFoV()\n    {\n        return cbFI.fTanHalfFOV;\n    }\n"
         "\n"
         "    // (longshot) the made frame's place between the frame before (0) and this one (1): AMD's passes fix it at 0.5\n"
         "    FfxFloat32 InterpolationT()\n    {\n        return cbFI.fInterpolationT;\n    }\n"
         "    // (the vector fields keep half the motion: the frame before is 2t of it away, this one 2(1 - t))\n"
         "    FfxFloat32 PrevScale()\n    {\n        return 2.0f * cbFI.fInterpolationT;\n    }\n"
         "    FfxFloat32 CurrScale()\n    {\n        return 2.0f * (1.0f - cbFI.fInterpolationT);\n    }\n"
         "    // (longshot) the HUD kept where it is (vk_fsr3fi.cpp kKeepStatic)\n"
         "    FfxBoolean KeepStaticPixels()\n    {\n        return (cbFI.dispatchFlags & (1u << 16)) != 0u;\n    }\n"
         "    // (longshot) the colour's alpha marks the HUD, 0 where it is (vk_fsr3fi.cpp kHudMask)\n"
         "    FfxBoolean HudMaskInAlpha()\n    {\n        return (cbFI.dispatchFlags & (1u << 17)) != 0u;\n    }\n", 1),
        # (each frame's HUD mark, the alpha of the two interpolation sources: KeepStaticPixels' known HUD)
        ("        return texelFetch(r_previous_interpolation_source, iPxPos, 0).rgb;\n    }\n",
         "        return texelFetch(r_previous_interpolation_source, iPxPos, 0).rgb;\n    }\n"
         "    // (longshot) the frame before's HUD mark: 0 the HUD (HudMaskInAlpha)\n"
         "    FfxFloat32 LoadPreviousBackbufferAlpha(FFX_PARAMETER_IN FfxInt32x2 iPxPos)\n    {\n"
         "        return texelFetch(r_previous_interpolation_source, iPxPos, 0).a;\n    }\n", 1),
        ("        return texelFetch(r_current_interpolation_source, iPxPos, 0).rgb;\n    }\n",
         "        return texelFetch(r_current_interpolation_source, iPxPos, 0).rgb;\n    }\n"
         "    // (longshot) this frame's HUD mark: 0 the HUD (HudMaskInAlpha)\n"
         "    FfxFloat32 LoadCurrentBackbufferAlpha(FFX_PARAMETER_IN FfxInt32x2 iPxPos)\n    {\n"
         "        return texelFetch(r_current_interpolation_source, iPxPos, 0).a;\n    }\n", 1),
    ],
    "ffx_frameinterpolation_game_motion_vector_field.h": [
        ("    const FfxFloat32x2 fInterpolatedLocationUv = fUvInScreenSpace + fMotionVectorHalf;\n",
         "    const FfxFloat32x2 fInterpolatedLocationUv = fUvInScreenSpace + fMotionVectorHalf * CurrScale();\n", 1),
        ("        const FfxFloat32 fBreakDist = ffxMin(length(fMotionVectorHalf), length(FfxFloat32x2(0.5f, 0.5f)));\n",
         "        const FfxFloat32 fBreakDist = ffxMin(length(fMotionVectorHalf * CurrScale()), length(FfxFloat32x2(0.5f, 0.5f)));\n", 1),
    ],
    "ffx_frameinterpolation_optical_flow_vector_field.h": [
        ("GetBilinearSamplingData(fUv + fMotionVectorHalf, GetOpticalFlowSize2());\n",
         "GetBilinearSamplingData(fUv + fMotionVectorHalf * CurrScale(), GetOpticalFlowSize2());\n", 1),
    ],
    "ffx_frameinterpolation_reconstruct_previous_depth.h": [
        ("    ReconstructPrevDepth(iPxPos, 1, fDilatedDepth, fMotionVector * 0.5f, RenderSize());\n",
         "    ReconstructPrevDepth(iPxPos, 1, fDilatedDepth, fMotionVector * 0.5f * CurrScale(), RenderSize());\n", 1),
    ],
    "ffx_frameinterpolation_disocclusion_mask.h": [
        ("ComputeDepthClip(0, fDepthUv + gameMv.fMotionVector, fDilatedDepth);\n",
         "ComputeDepthClip(0, fDepthUv + gameMv.fMotionVector * PrevScale(), fDilatedDepth);\n", 1),
        ("ComputeDepthClip(1, fDepthUv - gameMv.fMotionVector, fDilatedDepth);\n",
         "ComputeDepthClip(1, fDepthUv - gameMv.fMotionVector * CurrScale(), fDilatedDepth);\n", 1),
    ],
    "ffx_frameinterpolation_common.h": [
        # (LoadInpaintedGameFieldMv's and SampleOpticalFlowMotionVectorField's: whether each end is inside the frame)
        ("    vfElement.bNegOutside = !IsUvInside(fUv - vfElement.fMotionVector);\n",
         "    vfElement.bNegOutside = !IsUvInside(fUv - vfElement.fMotionVector * CurrScale());\n", 2),
        ("    vfElement.bPosOutside = !IsUvInside(fUv + vfElement.fMotionVector);\n",
         "    vfElement.bPosOutside = !IsUvInside(fUv + vfElement.fMotionVector * PrevScale());\n", 2),
    ],
    "ffx_frameinterpolation.h": [
        ("+gameMv.fMotionVector * fUvLetterBoxScale, DisplaySize());",
         "+gameMv.fMotionVector * PrevScale() * fUvLetterBoxScale, DisplaySize());", 1),
        ("-gameMv.fMotionVector * fUvLetterBoxScale, DisplaySize());",
         "-gameMv.fMotionVector * CurrScale() * fUvLetterBoxScale, DisplaySize());", 1),
        ("+ofMv.fMotionVector * fUvLetterBoxScale, DisplaySize());",
         "+ofMv.fMotionVector * PrevScale() * fUvLetterBoxScale, DisplaySize());", 1),
        ("-ofMv.fMotionVector * fUvLetterBoxScale, DisplaySize());",
         "-ofMv.fMotionVector * CurrScale() * fUvLetterBoxScale, DisplaySize());", 1),
        ("        FfxFloat32 t = 0.5f;\n"
         "        t += 0.5f * (1 - (fDisocclusionFactor.x));\n"
         "        t -= 0.5f * (1 - (fDisocclusionFactor.y));\n",
         f"        FfxFloat32 t = {FI_T};\n"
         f"        t += (1.0f - {FI_T}) * (1 - (fDisocclusionFactor.x));\n"
         f"        t -= {FI_T} * (1 - (fDisocclusionFactor.y));\n", 1),
        ("        fInterpolatedColor = ffxLerp(fPrevColorGame.fRaw, fCurrColorGame.fRaw, ffxSaturate(t));\n",
         "        fInterpolatedColor = ffxLerp(fPrevColorGame.fRaw, fCurrColorGame.fRaw, ffxSaturate(t));\n"
         "        LongshotStaticSamples(fUvInScreenSpace, gameMv.fMotionVector * fUvLetterBoxScale, t, fPrevColorGame.fRaw,\n"
         "                              fCurrColorGame.fRaw, fInterpolatedColor);\n", 1),
        ("        FfxFloat32 ofT = 0.5f;\n", f"        FfxFloat32 ofT = {FI_T};\n", 1),
        ("            ofT = 0.5f;\n", f"            ofT = {FI_T};\n", 1),
        ("        const FfxFloat32x3 ofColor = ffxLerp(fPrevColorOF.fRaw, fCurrColorOF.fRaw, ofT);\n",
         "        FfxFloat32x3 ofColor = ffxLerp(fPrevColorOF.fRaw, fCurrColorOF.fRaw, ofT);\n"
         "        LongshotStaticSamples(fUvInScreenSpace, ofMv.fMotionVector * fUvLetterBoxScale, ofT, fPrevColorOF.fRaw,\n"
         "                              fCurrColorOF.fRaw, ofColor);\n", 1),
        ("        fInterpolatedColor = ffxLerp(ofColor, fInterpolatedColor, ffxSaturate(fGameMvBias));\n    }\n}\n",
         "        fInterpolatedColor = ffxLerp(ofColor, fInterpolatedColor, ffxSaturate(fGameMvBias));\n    }\n"
         "    LongshotStaticPixel(FfxInt32x2(iPxPos), max(gameMv.fVelocity, ofMv.fVelocity), fInterpolatedColor,\n"
         "                        fInPaintingWeight);\n}\n", 1),
        # (the two helpers, ahead of computeInterpolatedColor, which calls them)
        ("void updateInPaintingWeight(inout FfxFloat32 fInPaintingWeight, FfxFloat32 fFactor)\n",
         "// (longshot) The HUD kept where it is: a pixel the same in both frames (to within half an 8-bit step), inside the\n"
         "// frame. KeepStaticPixels says whether to look at all\n"
         "FfxBoolean LongshotSame(FfxInt32x2 iPx)\n"
         "{\n"
         "    if (!IsInRect(iPx, InterpolationRectBase(), InterpolationRectSize())) return false;\n"
         "    const FfxFloat32x3 fDiff = abs(LoadPreviousBackbuffer(iPx) - LoadCurrentBackbuffer(iPx));\n"
         "    return max(fDiff.r, max(fDiff.g, fDiff.b)) <= (0.5f / 255.0f);\n"
         "}\n"
         "\n"
         "// A pixel of the frame before (bCurrent false) or of this one that is the HUD: marked in that frame's alpha where the\n"
         "// renderer knows it (HudMaskInAlpha), else one the same in both frames (LongshotSame)\n"
         "FfxBoolean LongshotHud(FfxInt32x2 iPx, FfxBoolean bCurrent)\n"
         "{\n"
         "    if (!HudMaskInAlpha()) return LongshotSame(iPx);\n"
         "    if (!IsInRect(iPx, InterpolationRectBase(), InterpolationRectSize())) return false;\n"
         "    return (bCurrent ? LoadCurrentBackbufferAlpha(iPx) : LoadPreviousBackbufferAlpha(iPx)) < 0.5f;\n"
         "}\n"
         "\n"
         "// A blend of the two frames' samples (0 < t < 1: both ends seen) where one sample lands on a pixel the same in both\n"
         "// frames and the other does not: that one is the HUD, the scene behind it in neither frame, so the other is taken.\n"
         "// fMv: the vector the samples were taken with, in UV (half the motion, as the fields keep it)\n"
         "void LongshotStaticSamples(FfxFloat32x2 fUv, FfxFloat32x2 fMv, FfxFloat32 t, FfxFloat32x3 fPrev, FfxFloat32x3 fCurr,\n"
         "                           inout FfxFloat32x3 fColor)\n"
         "{\n"
         "    if (!KeepStaticPixels() || t <= 0.0f || t >= 1.0f || length(fMv * DisplaySize()) < 0.5f) return;\n"
         "    const FfxBoolean bPrev = LongshotHud(FfxInt32x2((fUv + fMv * PrevScale()) * DisplaySize()), false);\n"
         "    const FfxBoolean bCurr = LongshotHud(FfxInt32x2((fUv - fMv * CurrScale()) * DisplaySize()), true);\n"
         "    if (bPrev && !bCurr) fColor = fCurr;\n"
         "    else if (bCurr && !bPrev) fColor = fPrev;\n"
         "}\n"
         "\n"
         "// The made pixel itself: this frame's HUD where it is marked; else the same in both frames while the scene there moves\n"
         "// (fVelocity: the larger of the two fields' vectors, in UV) is the HUD, or a flat colour where this frame's pixel is\n"
         "// right too: this frame's pixel\n"
         "void LongshotStaticPixel(FfxInt32x2 iPx, FfxFloat32 fVelocity, inout FfxFloat32x3 fColor, inout FfxFloat32 fInPaintingWeight)\n"
         "{\n"
         "    if (!KeepStaticPixels()) return;\n"
         "    if (HudMaskInAlpha() ? !LongshotHud(iPx, true)\n"
         "                         : fVelocity * length(FfxFloat32x2(DisplaySize())) < 0.5f || !LongshotSame(iPx)) return;\n"
         "    fColor = LoadCurrentBackbuffer(iPx);\n"
         "    fInPaintingWeight = 0.0f;\n"
         "}\n"
         "\n"
         "void updateInPaintingWeight(inout FfxFloat32 fInPaintingWeight, FfxFloat32 fFactor)\n", 1),
    ],
}


def fsr3_patched_fi_headers(tmp):
    """Frame interpolation's GPU headers with FI_PATCHES applied, in tmp/frameinterpolation: the directory that goes
    first on the include path, so "frameinterpolation/..." and the headers' own includes of each other find these."""
    src = FFX_GPU / "frameinterpolation"
    dst = Path(tmp) / "patched-gpu" / "frameinterpolation"
    shutil.copytree(src, dst)
    for name, edits in FI_PATCHES.items():
        path = dst / name
        text = path.read_text(encoding="utf-8")
        for old, new, count in edits:
            found = text.count(old)
            if found != count:
                raise SystemExit(f"shaders.py fsr3: {name}: expected {count} of {old.strip()[:70]!r}, found {found} "
                                 "(the SDK changed: write the patch again)")
            text = text.replace(old, new)
        path.write_text(text, encoding="utf-8")
    return dst


# (name, the pass file, extra defines). The file is named outright rather than composed: AMD's
# optical-flow v5 passes end "_pass_v5.glsl" and the interpolation pass itself is just
# "ffx_frameinterpolation_pass.glsl", so a composed name is wrong for four of the eighteen.
FI_VARIANTS = [
    ("fi_setup", "ffx_frameinterpolation_setup_pass.glsl", []),
    ("fi_reconstruct_previous_depth", "ffx_frameinterpolation_reconstruct_previous_depth_pass.glsl", []),
    ("fi_reconstruct_and_dilate", "ffx_frameinterpolation_reconstruct_and_dilate_pass.glsl", []),
    ("fi_game_motion_vector_field", "ffx_frameinterpolation_game_motion_vector_field_pass.glsl", []),
    ("fi_optical_flow_vector_field", "ffx_frameinterpolation_optical_flow_vector_field_pass.glsl", []),
    ("fi_disocclusion_mask", "ffx_frameinterpolation_disocclusion_mask_pass.glsl", []),
    ("fi_game_vector_field_inpainting_pyramid",
     "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.glsl", []),
    ("fi_inpainting_pyramid", "ffx_frameinterpolation_compute_inpainting_pyramid_pass.glsl", []),
    ("fi_interpolate", "ffx_frameinterpolation_pass.glsl", []),
    ("fi_inpainting", "ffx_frameinterpolation_inpainting_pass.glsl", []),
]
OF_VARIANTS = [
    ("of_prepare_luma", "ffx_opticalflow_prepare_luma_pass.glsl", []),
    ("of_luminance_pyramid", "ffx_opticalflow_compute_luminance_pyramid_pass.glsl", []),
    ("of_generate_scd_histogram", "ffx_opticalflow_generate_scd_histogram_pass.glsl", []),
    ("of_compute_scd_divergence", "ffx_opticalflow_compute_scd_divergence_pass.glsl", []),
    ("of_compute_optical_flow_v5", "ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl", []),
    ("of_filter_optical_flow_v5", "ffx_opticalflow_filter_optical_flow_pass_v5.glsl", []),
    ("of_scale_optical_flow_v5", "ffx_opticalflow_scale_optical_flow_advanced_pass_v5.glsl", []),
]


def fsr3_jobs(tmp):
    """(name, pass GLSL, defines, include folders) of every variant: the upscaler's eleven, then frame interpolation's ten
    (its headers patched first: FI_PATCHES, their folder ahead of the SDK's) and optical flow's seven"""
    jobs = []
    for name, pass_name, extra in FSR3_VARIANTS:
        defines = list(FSR3_OPTIONS)
        if not any("APPLY_SHARPENING" in d for d in extra):
            defines.append("-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0")
        jobs.append((name, FSR3_PASSES / f"ffx_fsr3upscaler_{pass_name}_pass.glsl", defines + extra,
                     [FFX_GPU, FFX_GPU / "fsr3upscaler"]))
    fi_headers = fsr3_patched_fi_headers(tmp)
    jobs += [(name, FI_PASSES / f, FI_OPTIONS + extra, [fi_headers.parent, FFX_GPU, fi_headers, FFX_GPU / "opticalflow"])
             for name, f, extra in FI_VARIANTS]
    jobs += [(name, OF_PASSES / f, OF_OPTIONS + extra, [FFX_GPU, FFX_GPU / "opticalflow"])
             for name, f, extra in OF_VARIANTS]
    return jobs


def fsr3_compile(glslang, name, source, defines, includes, out, target_env, variable=True):
    """One variant. With `variable`, glslang writes a C header; without it, binary SPIR-V, which is what spirv-val reads."""
    cmd = [glslang, "-V", "--quiet", "-S", "comp", *("-I" + str(d) for d in includes), *defines]
    if variable:
        cmd += ["--variable-name", f"FSR3_{name.upper()}_COMP_SPV"]
    cmd += ["-o", str(out), str(source), "--target-env", target_env]
    run_glslang(cmd, source.name)


def fsr3_without_version(text):
    """The header's lines without glslang's own version comment (`// 1116.2.0`): two glslang builds write different ones
    over the same SPIR-V, which is no source mismatch"""
    return [line for line in text.splitlines()
            if not line.strip().startswith("// ") or not line.strip()[3:].replace(".", "").isdigit()]


def fsr3(glslang_path=None, check=False, spirv_val=None, target_env=FSR3_TARGET_ENV):
    glslang = need_glslang(glslang_path)
    bad, made = [], []
    with tempfile.TemporaryDirectory(prefix="fsr3-shaders-") as tmp:
        jobs = fsr3_jobs(tmp)
        for name, source, defines, includes in jobs:
            header = FSR3_OUT / f"fsr3_{name}_comp_spv.h"
            out = Path(tmp) / header.name
            fsr3_compile(glslang, name, source, defines, includes, out, target_env)
            if spirv_val:
                spv = Path(tmp) / f"{name}.spv"
                fsr3_compile(glslang, name, source, defines, includes, spv, target_env, variable=False)
                subprocess.run([spirv_val, str(spv)], check=True)
            if not check:
                made.append((out, header))
                continue
            have = header.read_text(encoding="utf-8") if header.exists() else ""
            want = out.read_text(encoding="utf-8")
            if fsr3_without_version(have) != fsr3_without_version(want):
                bad.append(name)
                if not have:
                    print(f"{header.name}: missing", file=sys.stderr)
                else:
                    diff = difflib.unified_diff(have.splitlines(), want.splitlines(),
                                                "checked in", "regenerated", n=0, lineterm="")
                    print("\n".join(list(diff)[:12]), file=sys.stderr)
        # (glslang's own bytes, copied as they are; and only once every variant has compiled)
        for out, header in made:
            shutil.copyfile(out, header)
            print(f"wrote {header.relative_to(ROOT)} ({header.stat().st_size} bytes)")
    if bad:
        raise SystemExit(f"{len(bad)} FSR 3.1 header(s) do not match their source: {', '.join(bad)} "
                         "(python -I -B scripts/shaders.py fsr3)")
    print(f"fsr3 shaders: {len(jobs)} {'match' if check else 'written'}")


# ---- spirv-operands: gpu/vk/spirv_operands.h ----
# Letters: T result type, R result, i an id (IdRef, IdScope, IdMemorySemantics), l a literal word, s a literal string,
# c a literal number of the instruction's type (the rest of the words), P / Q / q pairs (id id, literal id, id literal),
# M MemoryAccess, G ImageOperands, L LoopControl (masks whose parameters are read by their bits), e an enum of one word with
# no parameters, X anything else (an instruction with one is not renamed). After a letter: * repeated to the end, ? optional.

OPERAND_LETTERS = {"IdResultType": "T", "IdResult": "R", "IdRef": "i", "IdScope": "i", "IdMemorySemantics": "i",
                   "LiteralInteger": "l", "LiteralExtInstInteger": "l", "LiteralSpecConstantOpInteger": "l",
                   "LiteralFloat": "l", "LiteralString": "s", "LiteralContextDependentNumber": "c",
                   "PairIdRefIdRef": "P", "PairLiteralIntegerIdRef": "Q", "PairIdRefLiteralInteger": "q",
                   "MemoryAccess": "M", "ImageOperands": "G", "LoopControl": "L"}


def spirv_operands(grammar_path, out_path):
    grammar = json.loads(Path(grammar_path).read_text(encoding="utf-8"))
    kinds = {k["kind"]: k for k in grammar["operand_kinds"]}

    def letter(kind):
        if kind in OPERAND_LETTERS:
            return OPERAND_LETTERS[kind]
        k = kinds.get(kind)
        if k and k.get("category") in ("ValueEnum", "BitEnum"):
            if not any(e.get("parameters") for e in k.get("enumerants", [])):
                return "e"
        return "X"

    rows = {}
    for ins in grammar["instructions"]:
        s = ""
        for o in ins.get("operands", []):
            s += letter(o["kind"]) + {"*": "*", "?": "?"}.get(o.get("quantifier", ""), "")
        rows[ins["opcode"]] = (ins["opname"], s)

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        # (under the name scripts/gen_spirv_operands.py, which the checked-in header carries: made again, it is the same bytes)
        f.write("// Generated by scripts/gen_spirv_operands.py from SPIR-V's core grammar (SPIRV-Headers %s.%s, revision %s): do not edit.\n"
                % (grammar.get("major_version"), grammar.get("minor_version"), grammar.get("revision")))
        f.write("// Each opcode's operands, one letter each (the script says what they mean); nullptr: an opcode not in the grammar.\n")
        f.write("#pragma once\n\n#include <cstdint>\n\nnamespace spirv_operands {\n\n")
        f.write("inline const char* of(uint32_t op) {\n    switch (op) {\n")
        for op in sorted(rows):
            name, s = rows[op]
            f.write(f'    case {op}: return "{s}";   // {name}\n')
        f.write("    default: return nullptr;\n    }\n}\n\n} // namespace spirv_operands\n")
    print(len(rows), "opcodes")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def shader_set(name, about):
        s = sub.add_parser(name, help=about)
        s.add_argument("--glslang", help="glslangValidator; found on PATH (glslangValidator, then glslang) by default")
        s.add_argument("--check", action="store_true", help="compare with the checked-in headers instead of writing")
        return s

    shader_set("crt", "gpu/vk/shaders/crt_*_comp_spv.h")
    for name, about in (("fsr3", "gpu/vk/shaders/fsr3_*_comp_spv.h"), ("all", "crt, fsr3 and host")):
        shader_set(name, about).add_argument("--spirv-val", help="spirv-val, to validate each FSR 3.1 module")
    shader_set("host", "gpu/vk/host_shaders/*_comp_spv.h")
    s = sub.add_parser("spirv-operands", help="gpu/vk/spirv_operands.h from SPIR-V's core grammar")
    s.add_argument("grammar", help="SPIRV-Headers' spirv.core.grammar.json")
    s.add_argument("out", nargs="?", default=str(ROOT / "gpu/vk/spirv_operands.h"))
    a = p.parse_args(argv)

    if a.cmd == "spirv-operands":
        spirv_operands(a.grammar, a.out)
        return 0
    if a.cmd in ("crt", "all"):
        crt(a.glslang, a.check)
    if a.cmd in ("fsr3", "all"):
        fsr3(a.glslang, a.check, a.spirv_val)
    if a.cmd in ("host", "all"):
        host(a.glslang, a.check)
    return 0


if __name__ == "__main__":
    sys.exit(main())
