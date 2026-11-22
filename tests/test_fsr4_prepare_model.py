"""AMD FSR 4's shader preparation (gpu/vk/vk_fsr4.cpp fsr4_prepare_shaders) builds against make_context as the file defines
it, and warms the contexts of the model the player picked (fsr4_model: the menu's Model row, or RX_VK_FSR4_MODEL), as the
per-frame path asks for (prepare). No game or GPU required.

The function is only compiled in a build with LONGSHOT_FSR (make_game.py --fsr, CI's FSR jobs), so a change to make_context
that leaves its call here behind breaks those builds and no other (#2122: #1989 gave make_context the model as a fourth
argument after #1886's warm call, which passed three). This check cuts the function out of the file and builds it against
stand-ins, with a make_context declared with the parameters of the file's own definition, which records each call:
- Auto and Lite picked, SDR: one context made, for the shown output, LDR, of the picked model;
- Native HDR picked (opts.hdr 2): two, LDR then HDR, both of the picked model; RX_VK_FSR4_HDR=0 leaves the HDR one out.

python -I -B tests/test_fsr4_prepare_model.py [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "gpu" / "vk" / "vk_fsr4.cpp"
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

FIXTURE = r'''#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include "gpu/vk/vk_fsr4.h"

namespace mkwvk {
struct Context {};
struct State { std::shared_ptr<Context> context; };
struct Out { uint32_t w, h; };
struct Opts { int hdr = 0; } opts;
static State state;
static Fsr4Model picked = FSR4_MODEL_AUTO;
static int hdr_switch = -1;   // (RX_VK_FSR4_HDR: -1 unset)
static std::vector<std::vector<long long>> calls;   // (make_context's arguments, in order)
static int frames;

Fsr4Model fsr4_model() { return picked; }
static bool load() { return true; }
static Out shown_output() { return {2560, 1440}; }
static int env_switch(const char*) { return hdr_switch; }
static void prepare_compiled() {}
static void logf(const char*, ...) {}
static State& st() { return state; }
static bool prepare_frame(uint32_t, uint32_t) { return ++frames, true; }
namespace tune {
struct Jobs { std::mutex mu; std::condition_variable cv; int running = 0; };
static Jobs the_jobs;
static std::atomic<int> better_count{0};
static bool on() { return true; }
static Jobs& jobs() { return the_jobs; }
static bool save_own_cache() { return true; }
}  // namespace tune

template <class... A> static void record(A... a) { calls.push_back({(long long)a...}); }
std::shared_ptr<Context> make_context(@PARAMS@) {
    record(@NAMES@);
    return std::make_shared<Context>();
}

@PREPARE@
}  // namespace mkwvk

using namespace mkwvk;
static int fails;
#define CHECK(x, ...) do { if (!(x)) { std::fprintf(stderr, "FAIL: " __VA_ARGS__); std::fprintf(stderr, "\n"); fails++; } } while (0)

static const char* name(int m) { return m == FSR4_MODEL_LITE ? "Lite" : "Auto"; }

// (one preparation: the contexts it made, each for the shown output, of the picked model, LDR then HDR)
static void run(Fsr4Model model, int hdr_option, int env, size_t contexts) {
    picked = model, opts.hdr = hdr_option, hdr_switch = env;
    calls.clear(), frames = 0;
    fsr4_prepare_shaders();
    const char* what = hdr_option == 2 ? (env == 0 ? "Native HDR, RX_VK_FSR4_HDR=0" : "Native HDR") : "SDR";
    CHECK(calls.size() == contexts, "%s, %s picked: %zu contexts made, %zu wanted", what, name(model), calls.size(), contexts);
    for (size_t i = 0; i < calls.size(); i++) {
        const auto& c = calls[i];
        CHECK(c.size() == 4, "make_context takes %zu arguments: this check knows (width, height, hdr, model)", c.size());
        if (c.size() != 4) continue;
        CHECK(c[0] == 2560 && c[1] == 1440, "%s: context %zu for %lldx%lld, not the shown output's 2560x1440", what, i, c[0], c[1]);
        CHECK(c[2] == (long long)(i == 1), "%s: context %zu is %s, wanted %s", what, i, c[2] ? "HDR" : "LDR", i == 1 ? "HDR" : "LDR");
        CHECK(c[3] == model, "%s: context %zu warmed for the %s model, but the player picked %s (fsr4_model)", what, i,
              name((int)c[3]), name(model));
    }
    CHECK(frames == (int)contexts, "%s: %d warm-up frames for %zu contexts", what, frames, contexts);
    CHECK(!state.context, "%s: the last context was kept after the preparation", what);
}

int main() {
    for (Fsr4Model model : {FSR4_MODEL_AUTO, FSR4_MODEL_LITE}) {
        run(model, 0, -1, 1);
        run(model, 2, -1, 2);
        run(model, 2, 0, 1);
    }
    if (!fails) std::printf("fsr4_prepare_shaders: the picked model warmed, SDR and Native HDR, Auto and Lite\n");
    return fails ? 1 : 0;
}
'''


def make_context_params(text):
    """make_context's parameter list as the file defines it, and its parameters' names"""
    found = re.findall(r"^std::shared_ptr<Context> make_context\(([^)]*)\) \{", text, re.M)
    if len(found) != 1:
        raise SystemExit("%s: %d definitions of make_context found, wanted one" % (SOURCE.relative_to(ROOT), len(found)))
    params = found[0].strip()
    names = [re.findall(r"\w+", p)[-1] for p in params.split(",")] if params else []
    return params, names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    text = SOURCE.read_text(encoding="utf-8")
    params, names = make_context_params(text)
    prepare = block(text, "void fsr4_prepare_shaders() {\n", where=SOURCE)
    fixture = FIXTURE.replace("@PARAMS@", params).replace("@NAMES@", ", ".join(names)).replace("@PREPARE@", prepare)
    with tempfile.TemporaryDirectory(prefix="fsr4-prepare-model-") as folder:
        source, exe = Path(folder) / "fixture.cpp", Path(folder) / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        built = subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                "-I" + str(ROOT), str(source), "-o", str(exe)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        if built.returncode:
            sys.stdout.write(built.stdout)
            raise SystemExit("FAIL: fsr4_prepare_shaders does not build against make_context(%s), the file's own definition: "
                             "a LONGSHOT_FSR build of vk_fsr4.cpp fails the same way" % params)
        raise SystemExit(subprocess.run([str(exe)], timeout=60).returncode)


if __name__ == "__main__":
    main()
