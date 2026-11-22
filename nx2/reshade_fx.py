"""Adapt the pinned ReShade preprocessor to resolve imports through Longshot's asset reader."""

from pathlib import Path


def build_jobs(cxx, root, generated, objects, own_flags, vendor_flags, backend_headers, *, windows, third_party=b""):
    """Keep the FX compiler's flags and dependencies away from the rest of the renderer.

    The upstream files' jobs end their dependencies with third_party (build.py's tp_deps), so they are in the release's
    prebuilt set (nx2/third_party_objects.py SOURCES) and a player's build links them instead of compiling them. The adapted
    preprocessor is made in the work folder, so it is not in the set: it is compiled as the renderer's own files are."""
    root, generated, objects = Path(root), Path(generated), Path(objects)
    upstream = root / "third_party" / "reshade_fx" / "source"
    bridge = root / "gpu" / "vk" / "reshade"
    images = root / "third_party" / "stb_image"
    include = [f"-I{upstream}", f"-I{bridge}"]
    headers = b"".join(file.read_bytes() for file in sorted(upstream.glob("*.hpp")))
    headers += (upstream / "effect_symbol_table_intrinsics.inl").read_bytes()
    adapted = write_preprocessor(upstream, generated / "reshade_preprocessor.cpp")
    flags = vendor_flags + include + ["-fno-char8_t"] + (["-include", "share.h"] if windows else [])
    jobs = []
    for name in ("codegen_spirv", "expression", "lexer", "parser_exp", "parser_stmt", "preprocessor", "symbol_table"):
        source = adapted if name == "preprocessor" else upstream / f"effect_{name}.cpp"
        # Optimizing the generated intrinsic table dominates the build. FX compilation runs on a worker.
        options = flags + (["-O0"] if name == "symbol_table" else [])
        jobs.append((cxx, str(source), str(objects / f"vk_reshade_vendor_{name}.o"), options,
                     headers + (b"" if name == "preprocessor" else third_party)))
    for name in ("compile", "files", "pipeline_cache", "preset", "runtime", "texture"):
        options, dependencies = own_flags + include, backend_headers + headers
        if name == "texture":
            options += [f"-I{images}"]
            dependencies += (images / "stb_image.h").read_bytes()
        jobs.append((cxx, str(bridge / f"fx_{name}.cpp"), str(objects / f"vk_reshade_fx_{name}.o"), options, dependencies))
    return jobs


def write_preprocessor(source, output):
    source = Path(source) / "effect_preprocessor.cpp"
    text = source.read_text(encoding="utf-8")

    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError(f"Update the ReShade include adapter for {source}: {old[:80]}")
        text = text.replace(old, new)

    replace('#include "effect_preprocessor.hpp"', '#include "effect_preprocessor.hpp"\n#include "fx_files.h"')
    start = text.index("\t// Read file contents into memory\n", text.index("static bool read_file("))
    end = text.index("\t// Append a new line feed", start)
    replace(text[start:end], "\tif (!ls::reshade::read_fx_file(path, file_data))\n\t\treturn false;\n\tfile_data.push_back('\\n');\n\n")

    start = text.index("\tstd::filesystem::path file_name =", text.index("void reshadefx::preprocessor::parse_include()"))
    end = text.index("\tconst std::string file_path_string", start)
    replace(text[start:end], """\tconst auto file_name = std::filesystem::u8path(_token.literal_as_string);
\tconst auto file_path = ls::reshade::resolve_fx_include(
\t\tstd::filesystem::u8path(_output_location.source), file_name, _include_paths);
\tif (file_path.empty())
\t\treturn error(keyword_location, "could not open included file '" + _token.literal_as_string + '\\'');

""")
    output = Path(output)
    if not output.exists() or output.read_text(encoding="utf-8") != text:
        output.write_text(text, encoding="utf-8")
    return output
