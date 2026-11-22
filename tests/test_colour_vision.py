"""Check the menu palette's correction and the provenance of both display shaders."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"

PROGRAM = r"""
#include "gpu/vk/colour_vision.h"
#include <cassert>

using mkwvk::colour_vision::correct_srgb;
using Pixel = std::array<float, 3>;

float luminance(Pixel rgb) {
    for (auto& c : rgb) c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

int main() {
    const Pixel input{0.76f, 0.39f, 0.57f};
    // Independent reference: apply the full simulation error row to linear Rec.709 and redistribute
    // the lost signal along the respective luminance-neutral opponent axis, at strength 0.75.
    const Pixel expected[] = {{0.745760f, 0.355311f, 0.780403f},
                              {0.748959f, 0.363379f, 0.740211f},
                              {0.809244f, 0.352167f, 0.570000f}};
    for (int mode : {-1, 0, 4, 999}) assert(correct_srgb(input, mode) == input);
    for (int mode = 1; mode <= 3; ++mode) {
        const auto output = correct_srgb(input, mode);
        for (int channel = 0; channel < 3; ++channel)
            assert(std::abs(output[channel] - expected[mode - 1][channel]) < 2e-6f);
        for (float gray : {0.0f, 0.04f, 0.5f, 1.0f})
            for (float c : correct_srgb({gray, gray, gray}, mode)) assert(std::abs(c - gray) < 2e-6f);
        for (int r = 0; r <= 4; ++r) {
            for (int g = 0; g <= 4; ++g) {
                for (int b = 0; b <= 4; ++b) {
                    const Pixel sample{r / 4.0f, g / 4.0f, b / 4.0f};
                    const auto corrected = correct_srgb(sample, mode);
                    for (float c : corrected) assert(c >= 0.0f && c <= 1.0f);
                    assert(std::abs(luminance(sample) - luminance(corrected)) < 2e-6f);
                }
            }
        }
    }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--glslang")
    args = parser.parse_args()
    include = SHADERS / "colour_vision.glsl"
    with tempfile.TemporaryDirectory() as folder:
        temp = Path(folder)
        for source in (SHADERS / "colour_vision.frag", SHADERS / "hdr_encode.comp"):
            header_name = f"{source.stem}_{source.suffix[1:]}_spv"
            header = (SHADERS / f"{header_name}.h").read_text(encoding="utf-8").splitlines(keepends=True)
            for file, line in zip((source, include), header[:2]):
                assert f"{file.name} sha256 {hashlib.sha256(file.read_bytes()).hexdigest()}" in line, file.name
            assert '#include "colour_vision.glsl"' in source.read_text(encoding="utf-8"), source.name
            if args.glslang:
                rebuilt = temp / "shader.h"
                subprocess.run([args.glslang, "-V", "--target-env", "vulkan1.2", "--vn", header_name.upper(),
                                "-o", str(rebuilt), str(source)], check=True)
                assert rebuilt.read_text(encoding="utf-8") == "".join(header[2:]), source.name
        source = temp / "fixture.cpp"
        source.write_text(PROGRAM, encoding="utf-8")
        binary = temp / "fixture.exe"
        subprocess.run([args.cxx, "-std=c++17", "-I", str(ROOT), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: colour vision identities, reference colours, luminance, gamut and shader provenance")


if __name__ == "__main__":
    main()
