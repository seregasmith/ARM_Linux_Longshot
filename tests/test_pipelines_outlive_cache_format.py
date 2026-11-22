"""The pipeline side of the shader caches outlives a change of the program cache's own format (kProgramCacheFormat): the driver's
pipeline cache (longshot_vk_pipelines.bin) and the pipeline notes (longshot_pipelines.bin) a build made are kept by the next
build of the same translator whose program cache format is higher, and still dropped for another translator. No game or GPU.

Why: every file the caches keep was stamped with program_cache_stamp(), the translator's stamp mixed with kProgramCacheFormat.
#1809 raised the format from 1 to 3 (a change to how longshot_programs.bin lays out a record; the SPIR-V it holds is the same),
and nightly-622's first start dropped every player's driver pipeline cache and truncated their notes along with the program file:
the preparation compiled every pipeline again cold, hours on AMD, where the same pipelines were all still good in the driver's
cache. Only the program file and the prepare record are about the format; what the pipeline side holds is about the SPIR-V.

How: the real pcache_drop_stale() and notes_open_locked() are cut out of gpu/vk/vk_draw.cpp, and program_cache_stamp() (with
what it calls) out of gpu/vk/vk_progcache.cpp, and compiled with stand-ins for the rest, as two builds: one with the format at 1
writes a stamped pipeline cache and three notes into a folder, the other with the format the source has now opens them. A third,
of another translator, must carry the pipeline cache and keep the notes (tests/test_pcache_stamp.cpp: what another
translator carries, and up to where). The other readers of the pipeline side (the notes' read and their references, the
precompile's record, the prediction's keys and sample, the module identifiers) are checked to judge their file's head by
pipelines_stamp_ok, never by program_cache_stamp() alone: a reader left on it reads none of a kept file (notes kept but not
referenced: the warm builds nothing and play compiles the noted pipelines with draws left out).


python -I -B tests/test_pipelines_outlive_cache_format.py [--cxx g++]
"""
import argparse
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "pcache_stamp.h"
#ifndef _WIN32   /* (the notes' open seeks past their head with _fseeki64: Windows has it, and no fseeko) */
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

static const char* g_dir = nullptr;
static uint64_t translator_stamp() { return TRANSLATOR; }
constexpr uint32_t kProgramCacheFormat = FORMAT;
static std::string cache_path(const char* name) { return std::string(g_dir) + "/" + name; }
static std::string pipe_notes_path() { return cache_path("longshot_pipelines.bin"); }
static void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    std::printf("\n");
}
static void cut_part_record(const std::string&, size_t, size_t) {}
static bool note_sane(struct PipeNote&) { return true; }
struct NvnState {
    uint8_t s[100]{};
};
struct PipeNote {
    uint64_t key = 0;
    uint8_t rest[120]{};
};
static std::unordered_set<uint64_t> noted;
static FILE* note_file = nullptr;

// ---- cut from gpu/vk/vk_progcache.cpp ----
PROGCACHE
// ---- cut from gpu/vk/vk_draw.cpp ----
DRAW

static long size_of(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    g_dir = argv[1];
    const std::string role = argv[2];
    const std::string pcache = cache_path("longshot_vk_pipelines.bin");
    if (role == "write" || role == "write-legacy") {
        // (the old build: its start stamps the folder, play fills the pipeline cache and notes three pipelines; write-legacy:
        // the notes' head as a build before the notes' own stamp wrote it, the program cache's stamp)
        pcache_drop_stale();
        FILE* f = std::fopen(pcache.c_str(), "wb");
        for (int i = 0; i < 3000; i++) std::fputc('p', f);
        std::fclose(f);
        if (role == "write") {
            notes_open_locked(nullptr);
        } else {
            note_file = std::fopen(pipe_notes_path().c_str(), "wb");
            const uint64_t h[2] = {program_cache_stamp(), sizeof(PipeNote)};
            std::fwrite(h, 8, 2, note_file);
        }
        for (uint64_t k = 1; k <= 3; k++) {
            PipeNote n;
            n.key = 0x100 + k;
            std::fwrite(&n, sizeof n, 1, note_file);
        }
        std::fclose(note_file);
        return 0;
    }
#ifdef HAS_NOTES_READ
    if (role == "read-only") {   // (a start that reads the notes and opens none for play: a preparation's, say)
        pcache_drop_stale();
        return pipe_notes_read().size() == 3 ? 0 : 1;
    }
    if (role == "noted-again") {   // (play noted a key whose old note was passed over: the later note is the one read)
        pcache_drop_stale();
        PipeNote again;
        again.key = 0x102, again.rest[0] = 7;
        FILE* f = std::fopen(pipe_notes_path().c_str(), "ab");
        std::fwrite(&again, sizeof again, 1, f);
        std::fclose(f);
        const auto notes = pipe_notes_read();
        return notes.size() == 3 && notes[1]->key == 0x102 && notes[1]->rest[0] == 7 ? 0 : 1;
    }
#endif
    // (the next build's start: the pipeline cache's files judged, then the notes opened for play's new pipelines)
    const bool dropped = pcache_drop_stale();
    notes_open_locked(nullptr);
    std::fclose(note_file);
    const long notes_bytes = size_of(pipe_notes_path());
    std::printf("pipeline cache %s (%ld bytes), %zu notes kept, notes file %ld bytes\n", dropped ? "dropped" : "kept",
                size_of(pcache), noted.size(), notes_bytes);
    const bool pcache_kept = !dropped && size_of(pcache) == 3000, pcache_gone = dropped && size_of(pcache) < 0;
    const bool notes_kept = noted.size() == 3 && notes_bytes == 16 + 3 * (long)sizeof(PipeNote);
    const bool notes_gone = noted.empty() && notes_bytes == 16;
    if (role == "keep") return pcache_kept && notes_kept ? 0 : 1;
    if (role == "drop") return pcache_gone && notes_gone ? 0 : 1;
    if (role == "keep-pcache") return pcache_kept && notes_gone ? 0 : 1;   // (the notes another translator's program cache stamped)
    if (role == "keep-notes") return pcache_gone && notes_kept ? 0 : 1;   // (the pipeline cache past its carry's bound)
    return 2;
}
"""


def cut_progcache(text):
    """program_cache_stamp() and what it calls in vk_progcache.cpp (pipelines_stamp_ok and program_cache_stamp_of where the
    source has them), the format number left to the harness"""
    parts = [block(text, marker) for marker in ("uint64_t program_cache_stamp_of(uint32_t format)",
                                                "uint64_t program_cache_stamp()",
                                                "bool pipelines_stamp_ok(uint64_t stamp)") if marker in text]
    return "\n".join(parts)


READERS = (   # (vk_draw.cpp's readers of a pipeline-side file, and what each must judge its head by; the notes by their own
    # stamp, which takes pipelines_stamp_ok's too)
    ("std::vector<std::unique_ptr<PipeNote>> pipe_notes_read()", "notes_stamp_ok(head[0])"),
    ("std::vector<NoteRef> pipe_notes_refs(bool* whole)", "notes_stamp_ok(head[0])"),
    ("bool precompile_covers_notes()", "pipelines_stamp_ok(rec[0])"),
    ("bool predicted_keys_read(std::vector<uint64_t>& keys)", "pipelines_stamp_ok(head[0])"),
    ("void predicted_check()", "pipelines_stamp_ok(head[0])"),
)


def check_readers(draw_src):
    bad = []
    for marker, need in READERS:
        body = block(draw_src, marker)
        if need not in body or "== program_cache_stamp()" in body or "!= program_cache_stamp()" in body:
            bad.append(marker)
    programs = (ROOT / "gpu" / "vk" / "vk_programs.cpp").read_text(encoding="utf-8")
    if "ip.shaderModuleIdentifierAlgorithmUUID, pipelines_stamp_ok);" not in programs:
        bad.append("vk_programs.cpp's ids.open (longshot_module_ids.bin)")
    return bad


KEY_CHECK = r"""
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>
typedef int VkPrimitiveTopology;
struct Program {};
struct NvnState {};
struct Targets {};
struct PipeNote {
    uint64_t key = 0;
    NvnState s;
    uint32_t topo = 0;
};
static uint64_t g_key;
static Targets note_targets(const PipeNote&) { return {}; }
static uint64_t pipeline_key(const Program&, const NvnState&, const Targets&, VkPrimitiveTopology) { return g_key; }
static std::atomic<size_t> notes_key_moved{0};
static std::mutex note_mu;
static std::unordered_set<uint64_t> noted, notes_key_left;
static void logf(const char*) {}
PASSED_FN
NEW_FN
CHECK_FN
int main() {
    Program p;
    PipeNote n;
    n.key = g_key = 7;
    if (!note_key_check(p, n) || notes_key_moved != 0) return 1;   // (the key this build makes: it holds)
    g_key = 8;
    if (note_key_check(p, n) || notes_key_moved != 1) return 2;    // (another key: passed over, and counted)
    // (the key it held is free for the state that makes it now, once; a key a note that holds has stays taken)
    noted = {7, 9};
    if (note_new_locked(9)) return 3;
    if (!note_new_locked(7) || note_new_locked(7)) return 4;
    if (!note_new_locked(11) || note_new_locked(11)) return 5;
    return 0;
}
"""


def check_note_key(cxx, tmp, draw_src):
    """the real note_key_check: a note whose state now makes another key is not built under the old one"""
    src, exe = Path(tmp) / "note_key.cpp", Path(tmp) / "note_key.exe"
    src.write_text(KEY_CHECK.replace("PASSED_FN", block(draw_src, "void note_key_passed(uint64_t key)"))
                   .replace("NEW_FN", block(draw_src, "bool note_new_locked(uint64_t key)"))
                   .replace("CHECK_FN", block(draw_src, "bool note_key_check(const Program& p, const PipeNote& n)")), encoding="utf-8")
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    return subprocess.run([str(exe)]).returncode == 0


def build(cxx, tmp, name, translator, fmt, progcache, draw):
    src = Path(tmp) / (name + ".cpp")
    exe = Path(tmp) / (name + ".exe")
    src.write_text(HARNESS.replace("PROGCACHE", progcache).replace("DRAW", draw), encoding="utf-8")
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"] +
                   (["-DHAS_NOTES_READ"] if "std::vector<std::unique_ptr<PipeNote>> pipe_notes_read()" in draw else []) +
                   ["-DTRANSLATOR=0x%xull" % translator, "-DFORMAT=%du" % fmt, "-I", str(ROOT / "gpu" / "vk"), str(src),
                    "-o", str(exe)], check=True)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    progcache_src = (ROOT / "gpu" / "vk" / "vk_progcache.cpp").read_text(encoding="utf-8")
    draw_src = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    now = int(re.search(r"constexpr uint32_t kProgramCacheFormat = (\d+);", progcache_src).group(1))
    if now < 2:
        raise SystemExit("kProgramCacheFormat is %d: this check needs an earlier format to start from" % now)
    progcache = cut_progcache(progcache_src)
    notes = ""
    if "constexpr uint32_t kNotesFormat" in draw_src:   # (the notes' own stamp: notes_stamp, notes_stamp_ok, notes_restamp)
        start = draw_src.index("constexpr uint32_t kNotesFormat")
        notes = draw_src[start:draw_src.index(block(draw_src, "void notes_restamp(const std::string& path, uint64_t had)"))] + \
            block(draw_src, "void notes_restamp(const std::string& path, uint64_t had)") + "\n" + \
            block(draw_src, "std::vector<std::unique_ptr<PipeNote>> pipe_notes_read()") + "\n"
    draw = notes + "\n".join(block(draw_src, marker) for marker in ("bool pcache_drop_stale()",
                                                                    "void notes_open_locked(const std::vector<std::unique_ptr<PipeNote>>* known)"))
    failed = ["judged by program_cache_stamp() alone: " + r for r in check_readers(draw_src)]
    if "if (!note_file || !note_new_locked(key)) return false;" not in block(draw_src, "bool pipeline_note(uint64_t key"):
        failed.append("pipeline_note does not let play note a key only a passed-over note holds")
    for r in failed:
        print("FAIL " + r)
    with tempfile.TemporaryDirectory(prefix="pipelines-format-") as tmp:
        if not check_note_key(args.cxx, tmp, draw_src):
            print("FAIL a note whose key moved is not passed over")
            failed.append("note_key_check")
        old = build(args.cxx, tmp, "old", 0x1111, 1, progcache, draw)
        older = build(args.cxx, tmp, "older", 0x1111, now - 1, progcache, draw)
        new = build(args.cxx, tmp, "new", 0x1111, now, progcache, draw)
        other = build(args.cxx, tmp, "other", 0x2222, now, progcache, draw)
        # (what, how the first build writes, the builds that start after it in turn with what each must find, a base file
        # written beside the first's cache: its size, beside the stamp the first wrote)
        cases = [
            ("format 1 -> %d, the same translator: kept" % now, "write", old, [(new, "keep")], None),
            ("format %d -> %d, the same translator: kept" % (now - 1, now), "write", older, [(new, "keep")], None),
            ("format %d -> %d, the same build again: kept" % (now, now), "write", new, [(new, "keep")], None),
            ("another translator: the pipeline cache carried, the notes kept", "write", new, [(other, "keep")], None),
            ("another translator and format 1: the pipeline cache carried (one translator's, no base yet), the notes kept",
             "write", old, [(other, "keep")], None),
            ("another translator, the pipeline cache past half as large again of its base: dropped, the notes kept",
             "write", new, [(other, "keep-notes")], 1000),
            ("notes stamped as before, read by their translator, then another: stamped anew, kept by both",
             "write-legacy", new, [(new, "keep"), (other, "keep")], None),
            ("notes stamped as before, only read by their translator (no notes opened for play), then another: kept by both",
             "write-legacy", new, [(new, "read-only"), (other, "keep")], None),
            ("a key noted again after its old note was passed over: the later note is read", "write", new, [(new, "noted-again")], None),
            ("notes stamped as before by another translator: dropped once (their stamp is not this one's), the pipeline cache carried",
             "write-legacy", new, [(other, "keep-pcache")], None),
        ]
        for i, (what, how, writer, readers, base) in enumerate(cases):
            folder = Path(tmp) / ("case%d" % i)
            folder.mkdir()
            subprocess.run([str(writer), str(folder), how], check=True, stdout=subprocess.DEVNULL)
            if base:
                stamp = (folder / "longshot_vk_pipelines.stamp").read_bytes()
                (folder / "longshot_vk_pipelines.base").write_bytes(struct.pack("<Q", base) + stamp + bytes(16))
            lines, ok = [], True
            for reader, want in readers:
                ran = subprocess.run([str(reader), str(folder), want], capture_output=True, text=True)
                lines.append(ran.stdout.strip().splitlines()[-1] if ran.stdout.strip() else ran.stderr.strip())
                ok = ok and ran.returncode == 0
            print("%s %s: %s" % ("ok  " if ok else "FAIL", what, "; then ".join(lines)))
            if not ok:
                failed.append(what)
    if failed:
        raise SystemExit("the pipeline side of the caches does not outlive a program cache format change: " + "; ".join(failed))


if __name__ == "__main__":
    main()
