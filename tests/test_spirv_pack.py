"""The shader store (gpu/vk/spirv_pack.h): SPIR-V kept once, by its content, for every game folder of the player.

Built from the header as the game has it and run on a folder of its own (tests/spirv_pack_test.cpp):

- the same words are one blob, read back by their place, and a read at another place or count is refused;
- two Longshots on one store each find what the other wrote (each append reads the index the other wrote first);
- the next start finds the same blobs; a write cut short, or a pack cut short by hand, loses only what nothing names, and the
  next blob goes past every place an entry names;
- words changed under a blob's head, or zeros where its head was, are not read nor given out again: written again;
- an index entry cut short (a full disk) leaves the entries after it on their grid, for this Longshot and the others;
- a store file held by another program at the start (Windows): none this run, and the store kept rather than made again;
- a deleted store comes back with another id and none of the old blobs, which is what makes a game folder's cache start over
  (vk_progcache.cpp disk_init);
- no folder: no store, and every put says so (the record keeps its SPIR-V).

And in vk_progcache.cpp (its text: disk_init is not built here): the cache's format was raised with the store, its file head
names the store, a file of another store starts over, one naming a store that did not open this run is kept for the next, and every reader of a stage's code takes the store's references (get_code / read_code).
Synthetic data only. python -I -B tests/test_spirv_pack.py [--cxx <compiler>]
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check_cache_source():
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    m = re.search(r"constexpr uint32_t kProgramCacheFormat = (\d+);", cache)
    if not m or int(m.group(1)) < 2:
        raise SystemExit("FAIL: the program cache's format was not raised with the shader store")
    # (disk_init is not built here: its three choices by the file's store, read from its text)
    init = cache[cache.index("void disk_init("):cache.index("void put_bytes(")]
    if "if (read_head && (!head[1] || head[1] == store_id)) {" not in init or "const uint32_t h[2] = {magic, store_id};" not in init:
        raise SystemExit("FAIL: the program cache's head does not name its shader store, or a file of another store is read")
    keep = "if (read_head && head[1] && !store_on && !disk_zstd && !spirv_pack::default_dir().empty()) {"
    if keep not in init or init.index(keep) > init.index("if (read_head && (!head[1]"):
        raise SystemExit("FAIL: a cache naming a store that did not open this run is not kept for the next")
    if "if (!file_store && store_id) {" not in init:
        raise SystemExit("FAIL: a cache of no store does not name the store before its first record that does")
    # (every place a stage's code is read: the whole read, the light read, the code by its place, the prediction)
    save = cache[cache.index("void program_cache_save(const Program& p"):cache.index("bool program_cache_payload(")]
    if "store.put(" not in save or "kStoreWords" not in save:
        raise SystemExit("FAIL: program_cache_save does not put the SPIR-V in the store")
    read = cache[cache.index("void program_cache_read(FILE* in"):cache.index("// ---- (longshot) A compute program's kept variants")]
    each = cache[cache.index("void program_cache_each("):cache.index("unsigned usable_cpus()")]
    light = cache[cache.index("std::unique_ptr<Program> program_cache_read_light("):cache.index("bool complete_from(")]
    complete = cache[cache.index("bool complete_from("):cache.index("bool program_cache_complete(Program& p)")]
    graphics = cache[cache.index("bool program_cache_complete_graphics("):cache.index("size_t program_cache_drop_modules()")]
    for name, body, want in (("program_cache_read", read, "get_code("), ("program_cache_each", each, "get_code("),
                             ("program_cache_read_light", light, "kStoreWords"), ("complete_from", complete, "read_code("),
                             ("program_cache_complete_graphics", graphics, "read_code(")):
        if want not in body:
            raise SystemExit("FAIL: %s does not read a stage's code from the shader store (%s)" % (name, want))
    for name, body in (("program_cache_read", read), ("program_cache_each", each)):
        if "r.list(spirv)" in body:
            raise SystemExit("FAIL: %s reads a stage's code as a list in the record, not through get_code" % name)
    programs = (ROOT / "gpu/vk/vk_programs.cpp").read_text(encoding="utf-8")
    if "p->code_words * 4);" in programs and "program_cache_code_in_record" not in programs:
        raise SystemExit("FAIL: vk_programs.cpp counts a light-read record's bytes as if its code were in it")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default="clang++")
    args = ap.parse_args()
    check_cache_source()
    with tempfile.TemporaryDirectory(prefix="spirv-pack-") as tmp:
        folder = Path(tmp)
        exe = folder / "check.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        "-I" + str(ROOT / "gpu/vk"), str(ROOT / "tests/spirv_pack_test.cpp"), "-o", str(exe)], check=True, cwd=folder)
        ran = subprocess.run([str(exe), str(folder / "store")], capture_output=True, text=True, timeout=60)
        if ran.returncode != 0 or "ok" not in ran.stdout:
            raise SystemExit("FAIL: the shader store's checks:\n" + ran.stdout + ran.stderr)
    print("PASS: the shader store keeps each SPIR-V once for every game folder, shared, and the cache reads it from there")


if __name__ == "__main__":
    main()
