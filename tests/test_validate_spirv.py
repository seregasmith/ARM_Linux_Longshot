"""scripts/validate_spirv.py finds a program cache record's stages and tells why a stage would fail.

A cache file is made here with a mesh-as-compute record of two compute stages: a valid module whose workgroup memory
(80000 bytes) is over RDNA1's 64 KB, and the same module with its array's element type left undefined. The stages are
found by their magic words between the record's other fields, the local size and workgroup memory are read from the
module, and (when the Vulkan SDK's spirv-val is there) the run flags both stages and writes them out.

python -I -B tests/test_validate_spirv.py
"""
import importlib.util
import os
import struct
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("validate_spirv", ROOT / "scripts" / "validate_spirv.py")
vs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vs)


def op(code, *args):
    return [(len(args) + 1) << 16 | code, *args]


def module(element=3, count=20000):
    """OpCapability Shader; GLCompute main, LocalSize 64 1 1; a Workgroup uint[count]"""
    w = [0x07230203, 0x00010000, 0, 10, 0]
    w += op(17, 1) + op(14, 0, 1)
    w += op(15, 5, 8, int.from_bytes(b"main", "little"), 0)
    w += op(16, 8, 17, 64, 1, 1)
    w += op(19, 1) + op(33, 2, 1) + op(21, 3, 32, 0) + op(43, 3, 4, count)
    w += op(28, 5, element, 4) + op(32, 6, 4, 5) + op(59, 6, 7, 4)
    w += op(54, 1, 8, 0, 2) + op(248, 9) + op(253) + op(56)
    return struct.pack("<%dI" % len(w), *w)


def record(*spirvs):
    data = struct.pack("<I", vs.FALLBACK | 2) + struct.pack("<Q", 0) + struct.pack("<3I", 64, 1, 1) + bytes(24)
    data += struct.pack("<Q", len(spirvs))
    for nvn, s in enumerate(spirvs, 6):   # (the task stage, then the mesh stage)
        data += struct.pack("<iI", nvn, 0x20) + struct.pack("<Q", len(s) // 4) + s + struct.pack("<Q", 0) + bytes(40)
    return data


def main():
    good, bad = module(), module(element=99)
    data = record(good, bad)
    found = vs.stages(data)
    assert found == [(6, 0x20, good), (7, 0x20, bad)], found
    assert vs.reflect(good) == ((64, 1, 1), 80000), vs.reflect(good)

    # (a stage in the shader store, gpu/vk/spirv_pack.h: the record has its word count with STORE_WORDS, its place and hash)
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "spirv.pack"), "wb") as f:
            f.write(struct.pack("<4I", 0x4B50534C, 1, 7, 0))
            f.write(struct.pack("<3Q", 1, 2, len(bad) // 4) + bad)
        place = 16 + 24
        stored = struct.pack("<I", vs.FALLBACK | 2) + struct.pack("<Q", 0) + struct.pack("<3I", 64, 1, 1) + bytes(24)
        stored += struct.pack("<Q", 2)
        stored += struct.pack("<iI", 6, 0x20) + struct.pack("<Q", len(good) // 4) + good + struct.pack("<Q", 0) + bytes(40)
        ref = struct.pack("<QqQQ", len(bad) // 4 | vs.STORE_WORDS, place, 1, 2)
        stored += struct.pack("<iI", 7, 0x20) + ref + struct.pack("<Q", 0) + bytes(40)
        store = vs.Store(tmp)
        assert store.id == 7, store.id
        found = vs.stages(stored, store)
        assert found == [(6, 0x20, good), (7, 0x20, bad)], found
        assert vs.stages_read(stored, store)[1] == 0
        # (no store, another place, or another hash there: its own stage alone, and the store's stage counted as not read)
        assert vs.stages_read(stored) == ([(6, 0x20, good)], 1)
        moved = stored.replace(ref, struct.pack("<QqQQ", len(bad) // 4 | vs.STORE_WORDS, place + 4, 1, 2))
        assert vs.stages_read(moved, store) == ([(6, 0x20, good)], 1)
        other = stored.replace(ref, struct.pack("<QqQQ", len(bad) // 4 | vs.STORE_WORDS, place, 1, 3))
        assert vs.stages_read(other, store) == ([(6, 0x20, good)], 1)
        store.f.close()

    # (a cache whose head names a store, its one stage there: it passes with that store, and fails, having read none of its
    # stages, with no store or another)
    with tempfile.TemporaryDirectory() as tmp:
        fine = module(count=100)
        with open(os.path.join(tmp, "spirv.pack"), "wb") as f:
            f.write(struct.pack("<4I", 0x4B50534C, 1, 7, 0))
            f.write(struct.pack("<3Q", 1, 2, len(fine) // 4) + fine)
        rec = struct.pack("<I", vs.FALLBACK | 2) + struct.pack("<Q", 0) + struct.pack("<3I", 64, 1, 1) + bytes(24)
        rec += struct.pack("<Q", 1) + struct.pack("<iI", 7, 0x20) + struct.pack("<QqQQ", len(fine) // 4 | vs.STORE_WORDS, 40, 1, 2)
        rec += struct.pack("<Q", 0) + bytes(40)
        cache = os.path.join(tmp, "longshot_programs.bin")
        with open(cache, "wb") as f:
            f.write(struct.pack("<IIQ", vs.CACHE_MAGIC, 7, 1))
            f.write(struct.pack("<QQ", 0xA5DA9F84B72D1471, len(rec)) + rec)
        assert vs.cache_store(cache) == 7
        if vs.find_tool("spirv-val"):
            out = os.path.join(tmp, "out")
            assert vs.main([cache, "--store", tmp, "--out", out]) == 0
            empty = os.path.join(tmp, "none")
            os.makedirs(empty)
            assert vs.main([cache, "--store", empty, "--out", out]) == 1
            with open(os.path.join(tmp, "spirv.pack"), "r+b") as f:
                f.write(struct.pack("<4I", 0x4B50534C, 1, 8, 0))   # (another store's id)
            assert vs.main([cache, "--store", tmp, "--out", out]) == 1

    with tempfile.TemporaryDirectory() as tmp:
        cache = os.path.join(tmp, "longshot_programs.bin")
        with open(cache, "wb") as f:
            f.write(struct.pack("<IIQ", vs.CACHE_MAGIC, 0, 1))
            f.write(struct.pack("<QQ", 0xA5DA9F84B72D1471, len(data)) + data)
            other = struct.pack("<I", 0) + bytes(8)   # (an ordinary program: left out unless --all)
            f.write(struct.pack("<QQ", 0x1234, len(other)) + other)
        keys = [k for k, _, _ in vs.records(cache, lambda key, kind: bool(kind & vs.FALLBACK))]
        assert keys == [0xA5DA9F84B72D1471], keys

        if vs.find_tool("spirv-val"):
            out = os.path.join(tmp, "out")
            status = vs.main([cache, "--out", out, "--jobs", "2"])
            assert status == 1, status
            names = sorted(os.listdir(out))
            assert {"a5da9f84b72d1471_6_compute.spv", "a5da9f84b72d1471_7_compute.spv"} <= set(names), names
            text = Path(out, "a5da9f84b72d1471_6_compute.txt").read_text(encoding="utf-8")
            assert "maxComputeSharedMemorySize" in text and "[spirv-val]" not in text, text
            text = Path(out, "a5da9f84b72d1471_7_compute.txt").read_text(encoding="utf-8")
            assert "[spirv-val]" in text, text
            assert vs.main([cache, "--out", out, "--key", "0000000000001234"]) == 0
            print("validate_spirv: stages, reflection and spirv-val ok")
        else:
            print("validate_spirv: stages and reflection ok (spirv-val not found: its run skipped)")


if __name__ == "__main__":
    main()
