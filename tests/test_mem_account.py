"""The renderer's memory accountant (gpu/vk/mem_account.h): what every pool holds, summed in one log line every 600 presents,
`vk: budget: VRAM <sum> of <allowed> MB (<used> MB by the driver, <unaccounted> MB unaccounted): <pool> <MB>, ...; RAM <sum> MB
(the process's private memory <private> MB): <pool> <MB>, ...`. Each pool had a figure and a line of its own and nothing added
them up.

Compiled alone (the header has no Vulkan in it) with made-up pools, tests/test_mem_account.cpp: the registry takes a pool
once; a collect asks every pool once and sums each ledger, the bytes held for reuse and the bytes on their way out; the
line puts the largest pools first, sums the ones under 1 MB as "others", leaves out the ones that hold nothing, and gives
what no pool reports as the driver's count less the sum, below zero when the pools count more, and "n/a" when the driver
gives no count.

And the arbiter, which asks the pools for memory back near what the driver allows: its two lines are the ones the texture
cache's 90% rule and the target pool read before (pressure past 90% of the allowance, the count past 97%), the pools are
asked in their order whatever order they registered in, each for what the ones before left, a pool that costs a stall to
refill gives only as the pressure begins (and a count that sways across the line does not begin it again), and nobody is
asked below the line.

Then the source: texture_frame logs the line beside `vk: VRAM:` from the driver's count and allowance it already has, the
pools are registered in vk_texture.cpp and vk_memory.cpp, and what was counted is only printed: nothing but that line reads
a pool's figures, so no eviction, limit or allocation can follow them. texture_frame takes one look at the lines each pass,
asks the pools only past them, and takes what they gave off the 90% rule's excess alone; the four pools that give stand in
the order the header's comment states. And scripts/memtour.py reads the line the header writes into its mem.csv, with a
column for every pool the renderer registers.

python -I -B tests/test_mem_account.py [--cxx g++]
"""
import argparse
import importlib.util
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu/vk"
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

LINE = ("vk: budget: VRAM 4121 of 7802 MB (4500 MB by the driver, 378 MB unaccounted): textures 3480, target pool 512, "
        "staging 128, others 1; RAM 3188 MB (the process's private memory 18200 MB): imports 2100, captures 512, capture spares "
        "512, staging 64, others 0")


def load_memtour():
    spec = importlib.util.spec_from_file_location("memtour", ROOT / "scripts" / "memtour.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def registered(source):
    """The pools a source file registers: the names of its add({"name", ...}) calls"""
    return re.findall(r'\badd\(\{"([^"]+)",', source)


def check_source(mt):
    fail = []
    header = (VK / "mem_account.h").read_text(encoding="utf-8")
    for banned in ("vulkan", "vk_internal.h", "vk_mem_alloc", "<mutex>"):
        if banned in header:
            fail.append("mem_account.h is no longer plain logic (it names %s): this check compiles it alone" % banned)
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    mem = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    frame = block(tex, "void texture_frame(uint64_t completed)")
    logged = "mem_account::format(line, sizeof line, mem_account::collect(mem_account::registry()), allowed, used, dev.memory_budget,"
    if logged not in frame:
        fail.append("texture_frame no longer logs the budget line from the driver's allowance and count")
    elif not frame.index('logf("vk: VRAM: %llu MB used of %llu MB allowed') < frame.index(logged) < frame.index('logf("vk: textures: decoded ASTC'):
        fail.append("the budget line is no longer logged beside the vk: VRAM: line, every 600 presents")
    # (counting only: the registry is read where the line is made and nowhere else)
    for name, text in (("vk_texture.cpp", tex), ("vk_memory.cpp", mem)):
        if '#include "mem_account.h"' not in text:
            fail.append(name + " no longer includes mem_account.h")
    readers = [p.name for p in sorted(VK.glob("*.cpp")) + sorted(VK.glob("*.h"))
               if p.name != "mem_account.h" and re.search(r"mem_account::(collect|registry|format|ask)\b", p.read_text(encoding="utf-8"))]
    if readers != ["vk_texture.cpp"] or tex.count("mem_account::collect(") != 1:
        fail.append("the accountant's totals are read outside texture_frame's log line (%s): it only counts" % ", ".join(readers))
    # (nor unqualified, in a file that names the namespace: vk_memory.cpp registers its pools with `using namespace mem_account`)
    # (a free call: another object's member of the same name, vk_memory.cpp's mem_checker().collect(...), is not the accountant's)
    for p in sorted(VK.glob("*.cpp")) + sorted(VK.glob("*.h")):
        text = p.read_text(encoding="utf-8")
        if p.name != "mem_account.h" and "namespace mem_account" in text and re.search(r"(?<![\w:.>])(collect|registry|ask)\(", text):
            fail.append("%s reads the accountant's registry or totals unqualified: it only counts" % p.name)
        # (the images' counter is read by the sample alone: nothing sizes or frees anything by it)
        for counter in ("images_made_bytes(", "buffers_made_bytes("):
            n = text.count(counter)
            if n != {"vk_vma.cpp": 1, "vk_internal.h": 1, "vk_texture.cpp": 1}.get(p.name, 0):
                fail.append("%s: %s appears %d times (its definition, its declaration, and account_sample's one read)"
                            % (p.name, counter, n))
    for counter in ("images_made_bytes(", "buffers_made_bytes(", "memory_account_sample("):
        if counter not in block(tex, "void account_sample(uint64_t block_free)"):
            fail.append("vk_texture.cpp no longer reads %s in account_sample" % counter)
    if tex.count("memory_account_sample(") != 1:
        fail.append("vk_texture.cpp reads vk_memory.cpp's figures outside account_sample")
    figures = lambda text: len(re.findall(r"\baccount\.\w", text))   # (the sample's fields; not "mem_account.h")
    if figures(frame):   # (texture_frame calls account_sample, and reads nothing of what it took)
        fail.append("texture_frame reads the accountant's sample (account.): it only counts")
    for name, text in (("vk_texture.cpp", tex), ("vk_memory.cpp", mem)):   # (what was counted is read by the report functions alone)
        inside = block(text, "const bool account_registered = [] {")
        own = block(text, "void account_sample(uint64_t block_free)") if name == "vk_texture.cpp" else block(
            text, "void account_publish(uint64_t live_bytes, uint64_t retired_bytes)") + block(
            text, "void memory_account_sample(uint64_t known[2])")
        if figures(text) != figures(inside) + figures(own):
            fail.append(name + ": the accountant's figures (account.) are read outside its sample and its report functions")
    # (the arbiter: one look a pass at the lines the 90% rule and the target pool read before; the pools asked only past them,
    # before the textures' rules; what they gave taken off the 90% rule's excess, and off nothing else)
    look = "const mem_account::Ask asked = arbiter.look(pressure, used, known ? allowed : 0);"
    asking = "const uint64_t given_back = asked.level ? account_ask(asked, used, allowed) : 0;"
    for needle in (look, asking, "pool_pressure = (uint8_t)asked.level;", "if (known && pressure > allowed / 10 * 9) {",
                   "if (const uint64_t left = over - std::min(over, given_back)) {"):
        if needle not in frame:
            fail.append("texture_frame no longer has: " + needle)
    if not fail and not frame.index(look) < frame.index(asking) < frame.index("uint64_t rule_budget = budget;"):
        fail.append("texture_frame asks the pools after the textures' own rules have taken their excess")
    if frame.count("given_back") != 2 or frame.count("account_ask(") != 1 or tex.count("account_ask(") != 2:
        fail.append("what the pools gave back is used beyond the 90% rule's excess, or they are asked in a second place")
    if "inline constexpr uint64_t kNearTenths = 9, kLinePct = 97;" not in header:
        fail.append("mem_account.h: the arbiter's lines are no longer 90% of the allowance and 97% of it")
    order = {}
    for name, text in (("vk_texture.cpp", tex), ("vk_memory.cpp", mem)):
        for m in re.finditer(r"\}, (\d+),\n\s+\[\]\(const Ask& a\) -> uint64_t \{", text):
            order[registered(text[:m.start()])[-1]] = int(m[1])
    if order != {"capture spares": 1, "staging": 2, "blocks free": 3, "target pool": 4}:
        fail.append("the pools that give memory back, and their order, are %s" % order)
    for give, trim in (("target pool", "pool_trim();"), ("capture spares", "cap_free_trim(32ull << 20);"),
                       ("staging", "linux_entered_pressure = true;"), ("blocks free", "compact.next_check = std::min(")):
        text = tex if give != "capture spares" else mem
        inside = block(text, "const bool account_registered = [] {")
        at = inside.index('add({"%s",' % give)
        if trim not in block(inside[at:], "[](const Ask& a) -> uint64_t {", unique=False):
            fail.append("the %s no longer gives back by the trim it had (%s)" % (give, trim))
    # (every pool the renderer registers has a column in memtour's mem.csv)
    names = registered(tex) + registered(mem)
    if len(names) < 10 or len(set(names)) != len(names):
        fail.append("the pools registered in vk_texture.cpp and vk_memory.cpp were not found, or one twice: %s" % names)
    for name in names:
        if name not in mt.BUDGET_VRAM + mt.BUDGET_RAM:
            fail.append("scripts/memtour.py has no column for the pool \"%s\"" % name)
    for name in set(mt.BUDGET_VRAM + mt.BUDGET_RAM) - set(names):
        fail.append("scripts/memtour.py has a column for \"%s\", which the renderer does not register" % name)
    if fail:
        raise SystemExit("\n".join(fail))


def check_memtour(mt, line):
    """The header's line, as the fixture printed it, through memtour's parser and its sampler's log reader"""
    need = lambda ok, what: ok or sys.exit("FAIL: memtour: " + what)
    need(line == LINE, "the fixture's line is\n%s\nnot\n%s" % (line, LINE))
    row = mt.parse_budget("[hos host] " + line + "\r\n")
    need(row is not None and sorted(row) == sorted(mt.BUDGET_COLUMNS), "parse_budget does not give every budget column")
    want = dict.fromkeys(mt.BUDGET_COLUMNS, 0)
    want.update(vram_sum=4121, vram_allowed=7802, vram_driver=4500, vram_unaccounted=378, vram_textures=3480,
                vram_target_pool=512, vram_staging=128, vram_others=1, ram_sum=3188, ram_private=18200, ram_imports=2100,
                ram_captures=512, ram_capture_spares=512, ram_staging=64, ram_others=0)
    need(mt.parse_budget(line.replace("378 MB unaccounted", "unaccounted n/a")) == dict(want, vram_unaccounted=""),
         "a line with no count from the driver does not leave vram_unaccounted empty and read the rest")
    need(row == want, "the line's columns are %s" % {k: v for k, v in row.items() if v != want[k]})
    # (below zero when the pools count more than the driver; a pool with no column goes to its ledger's others; none)
    row = mt.parse_budget("vk: budget: VRAM 4121 of 7802 MB (4000 MB by the driver, -121 MB unaccounted): textures 4000, a new "
                          "pool 120, others 1; RAM 0 MB (the process's private memory 5 MB): none")
    need(row["vram_unaccounted"] == -121 and row["vram_others"] == 121 and row["vram_textures"] == 4000, "a new pool, below zero")
    need(row["ram_sum"] == 0 and row["ram_private"] == 5 and row["ram_others"] == 0 and row["ram_imports"] == 0, "no RAM pool")
    need(mt.parse_budget("vk: VRAM: 4000 MB used of 5249 MB allowed") is None, "another line is taken for the budget's")
    need(mt.COLUMNS[:14] == ["t", "wall", "pid", "rss", "anon", "shmem", "file", "vmswap", "swap_all", "shmem_swapped",
                             "game_total", "memavail", "swapused", "sys_shmem"] and mt.COLUMNS[14:] == mt.BUDGET_COLUMNS,
         "mem.csv's columns from before are not first, in their order")
    with tempfile.TemporaryDirectory(prefix="mem-account-log-") as tmp:
        log = Path(tmp) / "log.txt"
        reader = mt.BudgetLog(log)
        need(reader.newest() == {}, "a log that is not there yet gives columns")
        older = line.replace("VRAM 4121", "VRAM 4000")
        with open(log, "ab") as f:
            f.write(b"vk: memory: working set 1 MB\n" + older.encode() + b"\n" + line.encode()[:40])   # (the newest: half written)
        need(reader.newest().get("vram_sum") == 4000, "the newest whole budget line is not the one read")
        with open(log, "ab") as f:
            # (and the arbiter's lines after it start with "vk: budget:" too: they are not the pools' line)
            f.write(line.encode()[40:] + b"\nvk: textures: decoded ASTC 1 MB\n"
                    b"vk: budget: gave back 300 MB from target pool at 92% of the allowance\n")
        need(mt.parse_budget("vk: budget: gave back 300 MB from target pool at 92% of the allowance") is None,
             "the arbiter's line is taken for the pools'")
        need(reader.newest().get("vram_sum") == 4121, "a line finished since the last look is not read")
        need(reader.newest().get("vram_sum") == 4121, "with nothing new in the log the last line's columns are lost")
        need(mt.BudgetLog(None).newest() == {}, "no log gives columns")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    mt = load_memtour()
    check_source(mt)
    with tempfile.TemporaryDirectory(prefix="mem-account-") as tmp:
        exe = Path(tmp) / "mem-account.exe"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_mem_account.cpp"), "-o",
                        str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        line = subprocess.run([str(exe), "line"], check=True, capture_output=True, text=True).stdout.rstrip("\n")
    check_memtour(mt, line)
    print("PASS: the budget line, in the renderer and in memtour")


if __name__ == "__main__":
    main()
