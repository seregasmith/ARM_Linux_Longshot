"""Build the development-only ReShade FX probe and exercise real upstream shaders."""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def main():
    probe_source = Path(__file__).resolve().parent
    root = probe_source.parents[1]
    sys.path.insert(0, str(root / "nx2"))
    from reshade_fx import write_preprocessor

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reshade-source", default=root / "third_party" / "reshade_fx", type=Path,
                        help="ReShade compiler source tree (defaults to vendored sources)")
    parser.add_argument("--shader-dir", type=Path, help="optionally test the Shaders directory of reshade-shaders")
    parser.add_argument("--spirv-headers", required=True, type=Path, help="include directory containing spirv.hpp")
    parser.add_argument("--cxx", default="clang++", help="C++ compiler (LLVM-MinGW on Windows)")
    args = parser.parse_args()

    bridge = root / "gpu" / "vk" / "reshade"
    upstream = args.reshade_source / "source"
    sources = [
        upstream / f"effect_{name}.cpp"
        for name in ("codegen_spirv", "expression", "lexer", "parser_exp", "parser_stmt", "symbol_table")
    ]

    windows = os.name == "nt"
    with tempfile.TemporaryDirectory(prefix="longshot-fx-") as temp:
        tmp = Path(temp)
        sources.append(write_preprocessor(upstream, tmp / "effect_preprocessor.cpp"))
        exe = tmp / ("fx_probe.exe" if windows else "fx_probe")
        # Upstream FX uses filesystem::u8string as a std::string (/Zc:char8_t- with MSVC).
        command = [args.cxx, "-std=c++20", "-O0", "-fno-char8_t"]
        if windows:
            command += ["-municode", "-static", "-include", "share.h"]
        for include in (bridge, upstream, args.spirv_headers, root / "third_party" / "stb_image"):
            command += ["-I", str(include)]
        subprocess.run([*command, "-o", exe, probe_source / "fx_probe.cpp", bridge / "fx_compile.cpp",
                        bridge / "fx_files.cpp", bridge / "fx_preset.cpp", bridge / "fx_texture.cpp", *sources], check=True)

        fixtures = {
            "Daltonize.fx": {"technique Daltonize 1", "uniform Type", "bindings 1 1"},
            "LUT.fx": {"technique LUT 1", "uniform fLUT_AmountChroma", "texture texLUT", "bindings 2 2"},
        }
        for name, expected in fixtures.items() if args.shader_dir else ():
            result = subprocess.run([exe, args.shader_dir / name], text=True, capture_output=True, encoding="utf-8")
            if result.returncode:
                raise SystemExit(f"{name}: probe failed ({result.returncode})\n{result.stderr}")
            missing = expected.difference(result.stdout.splitlines())
            if missing:
                raise SystemExit(f"{name}: missing metadata {sorted(missing)}\n{result.stdout}")
            print(f"{name}: techniques, bindings and SPIR-V entry points verified")

        shaders = tmp / "Shaders"
        shaders.mkdir()
        invalid = shaders / "effetto-è.fx"
        invalid.write_text('#include "missing.fxh"\n', encoding="utf-8")
        result = subprocess.run([exe, invalid], text=True, capture_output=True, encoding="utf-8")
        if result.returncode != 1 or "missing.fxh" not in result.stderr:
            raise SystemExit(f"Missing include was not reported: {result.stderr}")
        print("Missing include: diagnostic verified with a non-ASCII source path")

        (tmp / "outside.fxh").write_text('#error OUTSIDE_SHADER_ROOT\n', encoding="utf-8")
        invalid.write_text('#define OUTSIDE "../outside.fxh"\n#include OUTSIDE\n', encoding="utf-8")
        result = subprocess.run([exe, invalid], text=True, capture_output=True, encoding="utf-8")
        if result.returncode != 1 or "could not open included file" not in result.stderr or "OUTSIDE_SHADER_ROOT" in result.stderr:
            raise SystemExit(f"Include left the shader root: {result.stderr}")
        print("Include confinement: macro-expanded traversal blocked")

        preset = tmp / "Preset.ini"
        preset.write_text("\ufeffTechniques=A@one.fx,B@two.fx\nTechniqueSorting=B@two.fx,A@one.fx\n"
                          "PreprocessorDefinitions=SWITCH=1\n[one.fx]\nPreprocessorDefinitions=SWITCH=2\n"
                          "Amount=0.5\n", encoding="utf-8")
        result = subprocess.run([exe, preset], check=True, text=True, capture_output=True, encoding="utf-8")
        expected = ["B@two.fx", "A@one.fx", "SWITCH=2", "Amount=0.5"]
        if result.stdout.splitlines() != expected:
            raise SystemExit(f"Preset metadata differs: {result.stdout}")
        print("Preset INI: technique order, local definitions and uniform values verified")

        result = subprocess.run([exe, "--builtins"], text=True, capture_output=True, encoding="utf-8")
        if result.returncode:
            raise SystemExit(f"Built-in effects failed: {result.stderr}")
        print("Built-in presets compile their fullscreen shader and supply the expected uniforms")

        example = probe_source / "examples" / "TintedHighlights.fx"
        result = subprocess.run([exe, example], check=True, text=True, capture_output=True, encoding="utf-8")
        if "technique TintedHighlights 2" not in result.stdout.splitlines() or result.stdout.count("bindings 1 1") != 2:
            raise SystemExit(f"TintedHighlights: unexpected pass bindings:\n{result.stdout}")
        print("TintedHighlights: both graphics passes compile with their texture bindings")

        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

        png = tmp / "LUT.png"
        png.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
                        chunk(b"IDAT", zlib.compress(bytes([0, 17, 34, 51, 255]))) + chunk(b"IEND", b""))
        result = subprocess.run([exe, png], text=True, capture_output=True, encoding="utf-8")
        if result.returncode or result.stdout.strip() != "1x1 17 34 51 255":
            raise SystemExit(f"PNG input did not retain RGBA values: {result.stdout} {result.stderr}")
        print("PNG texture: RGBA values verified")


if __name__ == "__main__":
    main()
