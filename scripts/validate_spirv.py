"""The SPIR-V Longshot's translator kept in longshot_programs.bin, checked offline before a driver sees it again.

Each stage goes through the Vulkan SDK's spirv-val, and through the device limits spirv-val does not know: the workgroup's
invocations and its workgroup memory (the mesh fallback keeps a mesh program's outputs there, on top of the game's own
shared memory). A stage that fails is written to --out as <key>_<n>_<stage>.spv, its disassembly (.spvasm, spirv-dis) and
the reasons (.txt); the summary groups the validator's errors by message.

python -I scripts/validate_spirv.py "D:\\Games\\Longshot\\cache\\longshot_programs.bin"    (the mesh-as-compute programs)
python -I scripts/validate_spirv.py <file> --all                                        (every program)
python -I scripts/validate_spirv.py <file> --key a5da9f84b72d1471                       (the programs whose key starts so)

A stage whose SPIR-V is in the shader store (gpu/vk/spirv_pack.h) is read from it: --store names its folder, else the
player's (LONGSHOT_SHADER_STORE, then %LOCALAPPDATA%\\Longshot\\shaders or ~/.cache/longshot/shaders). The store must be
the one the cache's head names, and each stage the store does not give (another blob at its place, or none) is a failure:
a run that could not read the stages it found does not pass.

The exit status is 1 when a stage failed or was not read, 0 when none did.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import struct
import subprocess
import sys

CACHE_MAGIC = 0x31505348   # (gpu/vk/vk_shader.cpp kDiskMagic)
FALLBACK = 4               # (a record's kind: a task or mesh program run as compute)
SPV_MAGIC = b"\x03\x02\x23\x07"
TARGET_ENV = "vulkan1.3"   # (the Vulkan Longshot asks for)
MAX_SHARED = 65536         # (maxComputeSharedMemorySize: RDNA1's)
MAX_INVOCATIONS = 1024     # (maxComputeWorkGroupInvocations: RDNA1's)
STORE_WORDS = 1 << 63      # (a stage's word count with this bit: its SPIR-V is in the shader store, at the i64 after it,
                           # with the two u64 of its hash after that)
MOST_WORDS = 1 << 24       # (gpu/vk/spirv_pack.h kMostWords)
NEEDLE = b"\x00\x00\x00\x80"   # (the top four bytes of a word count with STORE_WORDS: no stage has 2^32 words)
STAGES = {0x1: "vertex", 0x2: "tesscontrol", 0x4: "tesseval", 0x8: "geometry", 0x10: "fragment", 0x20: "compute",
          0x40: "task", 0x80: "mesh"}


def records(path, want):
    """(key, kind, bytes) of each record want(key, kind) takes, in file order. The layout is disk_init's: a 16-byte head
    (magic, the shader store's id or 0, stamp), then records of a u64 key, a u64 size and that many bytes, which start with the u32 kind. A record
    cut short ends the walk, as it does there"""
    with open(path, "rb") as f:
        head = f.read(16)
        if len(head) < 16 or struct.unpack_from("<I", head)[0] != CACHE_MAGIC:
            raise SystemExit("%s: not a Longshot program cache" % path)
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            key, size = struct.unpack("<QQ", rec)
            at = f.tell()
            kind = f.read(4)
            if len(kind) < 4:
                return
            kind = struct.unpack("<I", kind)[0]
            if want(key, kind):
                f.seek(at)
                data = f.read(size)
                if len(data) < size:
                    return
                yield key, kind, data
            f.seek(at + size)


def store_dir():
    """The player's shader store folder, as gpu/vk/spirv_pack.h default_dir finds it ("" for none)"""
    s = os.environ.get("LONGSHOT_SHADER_STORE")
    if s:
        return "" if s == "0" else s
    if os.name == "nt":
        return os.path.join(os.environ["LOCALAPPDATA"], "Longshot", "shaders") if os.environ.get("LOCALAPPDATA") else ""
    x = os.environ.get("XDG_CACHE_HOME", "")
    return os.path.join(x if x.startswith("/") else os.path.expanduser("~/.cache"), "longshot", "shaders")


def cache_store(path):
    """The shader store id a program cache's head names (0: none)"""
    with open(path, "rb") as f:
        head = f.read(16)
    return struct.unpack_from("<I", head, 4)[0] if len(head) == 16 else 0


class Store:
    """The shader store's spirv.pack, read by place: a 16-byte head ('LSPK', version, the store's id), then blobs of a
    24-byte head (two u64 hashes, the u64 word count) and the words; a record names the words' offset and their hash"""

    def __init__(self, folder):
        path = os.path.join(folder, "spirv.pack") if folder else ""
        self.f = open(path, "rb") if path and os.path.isfile(path) else None
        self.id = 0
        if self.f:
            head = self.f.read(16)
            if len(head) == 16 and struct.unpack_from("<2I", head) == (0x4B50534C, 1):
                self.id = struct.unpack_from("<I", head, 8)[0]

    def read(self, at, words, hash_a, hash_b):
        """The words at at, when the blob there has this hash and count in its head; None otherwise"""
        if not self.f or at < 40:
            return None
        self.f.seek(at - 24)
        head = self.f.read(24)
        if len(head) < 24 or struct.unpack("<3Q", head) != (hash_a, hash_b, words):
            return None
        data = self.f.read(4 * words)
        return data if len(data) == 4 * words else None


def stages(data, store=None):
    """(nvn stage, vk stage, SPIR-V bytes) of each stage of a record that could be read (stages_read's)"""
    return stages_read(data, store)[0]


def stages_read(data, store=None):
    """(nvn stage, vk stage, SPIR-V bytes) of each stage of a record. program_cache_save writes a stage as int nvn_stage,
    VkShaderStageFlagBits vk_stage, then the SPIR-V as a list (a u64 word count, the words): each is found by its magic
    word and kept when the three fields before it agree, so the record's other fields (the answers, Shader::Info) need no
    layout of their own here. A stage in the shader store has, after the two, its word count with STORE_WORDS, the
    words' place in the store (an i64) and their hash (two u64): found by that count's top byte, its words read from store.
    Returns (those stages, how many stages in the store were found but not read: no store, or not those words there)"""
    out = [(i, nvn, vk, spirv) for i, nvn, vk, spirv in in_record(data)]
    spans = [(i, i + len(spirv)) for i, _, _, spirv in out]   # (a record's own SPIR-V: not read for references)
    missing = 0
    i = data.find(NEEDLE)
    while i != -1:
        at = i - 4   # (the count's first byte)
        if at >= 8 and at + 32 <= len(data) and not any(lo <= at < hi for lo, hi in spans):
            nvn, vk = struct.unpack_from("<iI", data, at - 8)
            n, place, hash_a, hash_b = struct.unpack_from("<QqQQ", data, at)
            words = n & ~STORE_WORDS
            if n & STORE_WORDS and 0 <= nvn <= 7 and vk in STAGES and 5 <= words < MOST_WORDS and place > 0:
                spirv = store.read(place, words, hash_a, hash_b) if store else None
                if spirv and spirv[:4] == SPV_MAGIC:
                    out.append((at, nvn, vk, spirv))
                else:
                    missing += 1
                i = data.find(NEEDLE, at + 32)
                continue
        i = data.find(NEEDLE, i + 1)
    return [(nvn, vk, spirv) for _, nvn, vk, spirv in sorted(out, key=lambda e: e[0])], missing


def in_record(data):
    """(place, nvn stage, vk stage, SPIR-V bytes) of each stage whose SPIR-V the record holds"""
    out = []
    i = data.find(SPV_MAGIC)
    while i != -1:
        if i >= 16:
            nvn, vk = struct.unpack_from("<iI", data, i - 16)
            n = struct.unpack_from("<Q", data, i - 8)[0]
            if 0 <= nvn <= 7 and vk in STAGES and 5 <= n and i + 4 * n <= len(data):
                out.append((i, nvn, vk, data[i:i + 4 * n]))
                i = data.find(SPV_MAGIC, i + 4 * n)
                continue
        i = data.find(SPV_MAGIC, i + 1)
    return out


def reflect(spirv):
    """(local size or None, workgroup memory bytes) of a module. The bytes are its Workgroup variables' types summed with
    no padding: a lower bound of what the driver lays out, so a module over the limit by it is over by any layout"""
    w = struct.unpack("<%dI" % (len(spirv) // 4), spirv[:len(spirv) // 4 * 4])
    size, const, pointee = {}, {}, {}
    local, local_ids, shared = None, None, 0
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0 or i + n > len(w):
            break
        a = w[i + 1:i + n]
        if op == 16 and len(a) >= 5 and a[1] == 17:           # OpExecutionMode LocalSize x y z
            local = tuple(a[2:5])
        elif op == 331 and len(a) >= 5 and a[1] == 38:        # OpExecutionModeId LocalSizeId (constants defined later)
            local_ids = a[2:5]
        elif op in (43, 50) and len(a) >= 3:                  # OpConstant, OpSpecConstant (its default): the low word
            const[a[1]] = a[2]
        elif op == 20:                                        # OpTypeBool
            size[a[0]] = 4
        elif op in (21, 22):                                  # OpTypeInt, OpTypeFloat: width in bits
            size[a[0]] = a[1] // 8
        elif op in (23, 24):                                  # OpTypeVector, OpTypeMatrix: component type, count
            size[a[0]] = size.get(a[1], 0) * a[2]
        elif op == 28:                                        # OpTypeArray: element type, length constant
            size[a[0]] = size.get(a[1], 0) * const.get(a[2], 0)
        elif op == 30:                                        # OpTypeStruct: members
            size[a[0]] = sum(size.get(m, 0) for m in a[1:])
        elif op == 32:                                        # OpTypePointer: id, storage class, pointee
            pointee[a[0]] = a[2]
        elif op == 59 and len(a) >= 3 and a[2] == 4:          # OpVariable of storage class Workgroup
            shared += size.get(pointee.get(a[0]), 0)
        i += n
    if local_ids is not None:
        local = tuple(const.get(x, 0) for x in local_ids)
    return local, shared


def find_tool(name):
    """A Vulkan SDK tool: on PATH, else in %VULKAN_SDK%\\Bin"""
    found = shutil.which(name)
    if not found and os.environ.get("VULKAN_SDK"):
        candidate = os.path.join(os.environ["VULKAN_SDK"], "Bin", name + (".exe" if os.name == "nt" else ""))
        found = candidate if os.path.isfile(candidate) else None
    return found


def check(job, val):
    """The reasons a stage fails ([] when it passes): spirv-val's errors (on stdin), then the device limits"""
    key, nvn, vk, spirv = job
    reasons = []
    r = subprocess.run([val, "--target-env", TARGET_ENV, "-"], input=spirv, capture_output=True)
    if r.returncode:
        text = (r.stdout + r.stderr).decode("utf-8", "replace").strip()
        reasons.append(("spirv-val", text or "spirv-val exit status %d" % r.returncode))
    local, shared = reflect(spirv)
    if local and local[0] * local[1] * local[2] > MAX_INVOCATIONS:
        reasons.append(("limit", "workgroup %dx%dx%d is %d invocations, over maxComputeWorkGroupInvocations %d"
                        % (*local, local[0] * local[1] * local[2], MAX_INVOCATIONS)))
    if shared > MAX_SHARED:
        reasons.append(("limit", "workgroup memory at least %d bytes, over maxComputeSharedMemorySize %d"
                        % (shared, MAX_SHARED)))
    return reasons, local, shared


def first_error(text):
    """A validator message with its numbers taken out, so the same fault in many stages counts as one"""
    line = next((x for x in text.splitlines() if x.startswith("error")), text.splitlines()[0] if text else "")
    return re.sub(r"\d+", "N", line).strip()


def dump(out, job, reasons, local, shared, dis):
    key, nvn, vk, spirv = job
    base = os.path.join(out, "%016x_%d_%s" % (key, nvn, STAGES[vk]))
    with open(base + ".spv", "wb") as f:
        f.write(spirv)
    with open(base + ".txt", "w", encoding="utf-8") as f:
        f.write("program %016x, NVN stage %d, %s stage, %d words\n" % (key, nvn, STAGES[vk], len(spirv) // 4))
        f.write("local size %s, workgroup memory at least %d bytes\n\n" % (local, shared))
        for kind, text in reasons:
            f.write("[%s]\n%s\n\n" % (kind, text))
    if dis:
        subprocess.run([dis, base + ".spv", "-o", base + ".spvasm"], capture_output=True)
    return base


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("cache", help="longshot_programs.bin")
    p.add_argument("--all", action="store_true", help="every program, not only the mesh-as-compute ones")
    p.add_argument("--key", action="append", default=[], help="a program key (hex) or its start; may be given again")
    p.add_argument("--out", help="where failing stages are written (default: spirv_failures beside the cache)")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    p.add_argument("--store", default=None, help="the shader store's folder (default: the player's)")
    a = p.parse_args(argv)
    val, dis = find_tool("spirv-val"), find_tool("spirv-dis")
    if not val:
        raise SystemExit("spirv-val not found: install the Vulkan SDK or put its Bin folder on PATH")
    keys = [k.lower().removeprefix("0x") for k in a.key]
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.cache)), "spirv_failures")

    def want(key, kind):
        if keys:
            return any(("%016x" % key).startswith(k) for k in keys)
        return a.all or bool(kind & FALLBACK)

    programs, checked_stages, missing, failed, groups = 0, 0, 0, [], collections.defaultdict(list)
    store = Store(store_dir() if a.store is None else a.store)
    named = cache_store(a.cache)
    if named and store.id != named:
        # (its stages in the store are not checked against another's blobs: each counts as not read)
        print("%s: its stages in the shader store are in store %08x, and %s" % (
            a.cache, named, "no store was found" if not store.f else "the store found is %08x" % store.id))
        store = None
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        pending = collections.deque()

        def settle(future):
            job, (reasons, local, shared) = future.job, future.result()
            if not reasons:
                return
            os.makedirs(out, exist_ok=True)
            base = dump(out, job, reasons, local, shared, dis)
            failed.append(base)
            for kind, text in reasons:
                groups[first_error(text) if kind == "spirv-val" else re.sub(r"\d+", "N", text)].append(
                    "%016x %s" % (job[0], STAGES[job[2]]))

        for key, kind, data in records(a.cache, want):
            programs += 1
            found, unread = stages_read(data, store)
            missing += unread
            checked_stages += len(found)
            for nvn, vk, spirv in found:
                job = (key, nvn, vk, spirv)
                future = pool.submit(check, job, val)
                future.job = job
                pending.append(future)
                while len(pending) > 4 * a.jobs:   # (a few batches in flight: --all reads gigabytes)
                    settle(pending.popleft())
        while pending:
            settle(pending.popleft())

    checked = "programs" if a.all or keys else "mesh-as-compute programs"
    print("%s: %d %s, %d stages checked, %d failed" % (a.cache, programs, checked, checked_stages, len(failed)))
    if missing:
        print("  %d stages in the shader store not read (no store, another store, or not those words at their place)" % missing)
    for message, where in sorted(groups.items(), key=lambda g: -len(g[1])):
        print("  %4dx  %s" % (len(where), message))
        print("         e.g. %s" % ", ".join(where[:3]))
    if failed:
        print("written to %s (.spv, .spvasm, .txt)" % out)
    if not programs:
        print("  (no records matched%s)" % (": is the key right?" if keys else ""))
    return 1 if failed or missing else 0


if __name__ == "__main__":
    sys.exit(main())
