"""Live output previews preserve unrelated settings and route linked options through restart."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}", "--cc", "{cc}"]


def block(text, marker):
    start = text.index(marker)
    depth = 0
    for at in range(text.index("{", start), len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise ValueError(marker)


FIXTURE = r'''
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include "host/hos_atomic_file.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Out { unsigned w, h; };
std::mutex options_save_mu;
std::string path;
std::string ini_path() { return path; }
bool linked_path;
bool preview_path_is_link(const char* name) { return linked_path || hos_af_is_link(name); }
#define hos_af_is_link preview_path_is_link
struct { int handheld; int output_res; } opts{};
Out output_preview_target{};
bool output_preview_from_field;
bool env_holds(int*) { return false; }
Out shown_output() { return {1280, 720}; }
int requests;
bool output_preview_request(unsigned, unsigned) { ++requests; return true; }
@@REQUEST@@
@@SAVE@@
#undef hos_af_is_link
std::string read_file() {
    FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f);
    std::string result;
    char b[128];
    while (size_t n = std::fread(b, 1, sizeof b, f)) result.append(b, n);
    CHECK(!std::ferror(f));
    std::fclose(f);
    return result;
}
int main(int argc, char** argv) {
    CHECK(argc == 3);
    path = argv[1];
    linked_path = true;
    CHECK(!request_native_preview({1024, 576}, false));
    CHECK(!save_preview_size({1024, 576}));
    CHECK(requests == 0 && output_preview_target.w == 0);
    linked_path = false;
    path.clear();
    CHECK(!request_native_preview({1024, 576}, false));
    path = argv[1];
    CHECK(request_native_preview({1024, 576}, true));
    CHECK(requests == 1 && output_preview_target.w == 1024 && output_preview_target.h == 576);
    CHECK(output_preview_from_field);
    CHECK(save_preview_size({1024, 576}));
    CHECK(read_file() == "output_res=1024x576\n");
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f);
    CHECK(std::fputs("other=1\noutput_res=1280x720\n", f) >= 0);
    CHECK(std::fclose(f) == 0);
    CHECK(save_preview_size({1024, 576}));
    CHECK(read_file() == "other=1\noutput_res=1024x576\n");
    path = argv[2];   // A directory cannot be read as an options file.
    CHECK(!save_preview_size({1024, 576}));
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="preview-save-") as tmp:
        work = Path(tmp)
        source = work / "preview.cpp"
        request = block(menu, "static bool request_native_preview(Out target, bool from_field) {")
        save = block(menu, "static bool save_preview_size(")
        source.write_text(FIXTURE.replace("@@REQUEST@@", request).replace("@@SAVE@@", save), encoding="utf-8")
        exe = work / "preview"
        flags = ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT)]
        if os.name != "nt":
            shim = work / "win32_posix.o"
            subprocess.run([args.cc, "-std=gnu11", "-w", "-I", str(ROOT / "host/posix"), "-c",
                            str(ROOT / "host/posix/win32_posix.c"), "-o", str(shim)], check=True)
            flags += ["-I", str(ROOT / "host/posix"), str(shim), "-lpthread", "-ldl"]
        subprocess.run([args.cxx, str(source), *flags, "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(work / "options.ini"), str(work)], check=True)
        assert not (work / "options.ini.hosnew").exists()


if __name__ == "__main__":
    main()
