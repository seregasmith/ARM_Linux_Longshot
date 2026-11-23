"""Exercise Setup's shader dump with game-free cache files."""

from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

RUN_ALL_SKIP = {"windows": "the Linux Setup AppImage exports under POSIX cache locks"}

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "nx2"))
import shader_dump


class ShaderDumpTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.game = self.root / "game"
        self.cache = self.game / "cache"
        self.store = self.root / "store"
        self.store.mkdir()
        mesa = self.cache / "driver" / shader_dump.LAYOUT / "0123" / "Adreno (TM) 830"
        mesa.mkdir(parents=True)
        for name in ("foz_cache.foz", "foz_cache_idx.foz", "index"):
            (mesa / name).write_bytes(name.encode())
        (self.game / "longshot_shaders.bin").write_bytes(b"seed")
        (self.cache / "longshot_programs.bin").write_bytes(struct.pack("<4sIQ", b"HSP1", 42, 23))
        (self.cache / "longshot_programs.idx").write_bytes(struct.pack("<4sIQ", b"HPIX", 1, 23))
        (self.cache / "longshot_prepared.bin").write_bytes(struct.pack("<Q", 23))
        (self.cache / "longshot_pipelines.bin").write_bytes(b"fixture")
        (self.cache / "longshot_programs.bin.lock").touch()
        (self.store / "spirv.pack").write_bytes(struct.pack("<4sIII", b"LSPK", 1, 42, 0))
        (self.store / "spirv.idx").write_bytes(struct.pack("<4sIII", b"LSPI", 1, 42, 0))
        report = f"1\t2\t18\t3\t{'a' * 32}\t{23:016x}\tmost\tHSP1\t{self.store}\tMesa 27.0.0-devel (fixture)"
        shader_dump.record_preparation(self.game, report, "fixture")
        self.now = datetime(2026, 11, 23, 13, 15, 44)
        self.stem = "mesa-27.0.0-devel-231126_131544"

    def export(self):
        return shader_dump.export(self.game, self.root, self.now)

    def test_export_manifest_and_collision(self):
        first = self.root / f"{self.stem}.zip"
        first.write_bytes(b"existing dump")
        result = self.export()
        self.assertEqual(result.name, f"{self.stem}_1.zip")
        self.assertEqual(first.read_bytes(), b"existing dump")
        with zipfile.ZipFile(result) as archive:
            manifest = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest["driver"]["pipeline_cache_uuid"], "a" * 32)
            self.assertEqual(manifest["program_cache"]["store_id"], 42)
            self.assertNotIn("spirv_store_path", manifest)
            entries = {item["path"]: item for item in manifest["files"]}
            self.assertIn("spirv/spirv.pack", entries)
            self.assertIn("driver/mesa_shader_cache_sf/0123/Adreno (TM) 830/foz_cache.foz", entries)
            self.assertEqual(set(archive.namelist()), set(entries) | {"manifest.json"})
            for name, item in entries.items():
                data = archive.read(name)
                self.assertEqual(item["sha256"], hashlib.sha256(data).hexdigest())
                self.assertEqual(item["size"], len(data))
                self.assertFalse(name.endswith(".lock"))

    def test_suffix_limit(self):
        (self.root / f"{self.stem}.zip").touch()
        for n in range(1, 255):
            (self.root / f"{self.stem}_{n}.zip").touch()
        self.assertEqual(self.export().name, f"{self.stem}_255.zip")
        with self.assertRaisesRegex(shader_dump.DumpError, "All 256 names"):
            self.export()
        self.assertFalse(list(self.root.glob("*.part")))

    def test_refuses_running_game_or_wrong_store(self):
        with shader_dump.cache_lock(self.cache / ".shader-dump.lock", exclusive=False):
            with self.assertRaisesRegex(shader_dump.DumpError, "in use"):
                self.export()
        with shader_dump.cache_lock(self.store / "spirv.lock", exclusive=True):
            with self.assertRaisesRegex(shader_dump.DumpError, "in use"):
                self.export()
        (self.store / "spirv.idx").write_bytes(struct.pack("<4sIII", b"LSPI", 1, 41, 0))
        with self.assertRaisesRegex(shader_dump.DumpError, "different SPIR-V store"):
            self.export()
        self.assertFalse(list(self.root.glob("*.zip")))

    def test_refuses_changed_seed_or_missing_metadata(self):
        (self.game / "longshot_shaders.bin").write_bytes(b"changed")
        with self.assertRaisesRegex(shader_dump.DumpError, "seed changed"):
            self.export()
        (self.cache / shader_dump.SOURCE).unlink()
        with self.assertRaisesRegex(shader_dump.DumpError, "no shader export metadata or usable shader preparation stamp"):
            self.export()

    def legacy_install(self):
        (self.cache / shader_dump.SOURCE).unlink()
        (self.game / "longshot_shaders.bin").write_bytes(bytes(range(97)))
        (self.cache / "longshot_prepared.bin").write_bytes(
            struct.pack("<6Q", 23, 14930608981950081792, 97, 0, 0, 0x2fa4022748909335))
        layout = self.cache / "driver" / shader_dump.LAYOUT
        (layout / "0123").rename(layout / ("09" * 16))
        shutil.copytree(layout / ("09" * 16), layout / ("11" * 16))
        logs = self.game / "logs"
        logs.mkdir()
        (logs / "longshot_log.txt").write_text(
            "[hos host] longshot fixture\n"
            "[hos host] vk: policy: facts 1:2, driver id 18, version 0x3 (Mesa 27.0.0-devel (git-fixture)), type 1, not Windows\n"
            f"[hos host] vk: driver shader cache: Mesa's cache in {self.cache}/driver (MESA_SHADER_CACHE_DIR), "
            "layout mesa_shader_cache_sf; RADV info\n"
            f"[hos host] vk: program cache {self.cache}/longshot_programs.bin: 10 program keys kept, 20 KB\n"
            f"[hos host] vk: shader store {self.store}/: 10 SPIR-V blobs, for every game folder\n"
            "[hos host] vk: Compile shaders: Most (compile_shaders in longshot_options.ini)\n"
            "[hos host] vk: start-up: the shaders took 3.4 s\n",
            encoding="utf-8",
        )

    def test_exports_older_install_with_verified_driver_and_seed(self):
        self.legacy_install()
        with zipfile.ZipFile(self.export()) as archive:
            manifest = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest["driver"]["pipeline_cache_uuid"], "09" * 16)
            self.assertEqual(manifest["shader_seed_sha256"], hashlib.sha256(bytes(range(97))).hexdigest())
        self.assertFalse((self.cache / shader_dump.SOURCE).exists())
        (self.game / "longshot_shaders.bin").write_bytes(bytes(reversed(range(97))))
        with self.assertRaisesRegex(shader_dump.DumpError, "seed changed"):
            self.export()

    def test_legacy_install_requires_a_completed_launch_from_the_prepared_driver(self):
        self.legacy_install()
        log = self.game / "logs" / "longshot_log.txt"
        log.write_text(log.read_text().replace("version 0x3", "version 0x4"), encoding="utf-8")
        with self.assertRaisesRegex(shader_dump.DumpError, "could not match"):
            self.export()

    def test_apprun_exports_beside_image_without_build_tools(self):
        source = Path(__file__).resolve().parents[1]
        appdir = self.root / "unpacked"
        tools = appdir / "usr/share/longshot/nx2"
        tools.mkdir(parents=True)
        shutil.copyfile(source / "nx2/shader_dump.py", tools / "shader_dump.py")
        python = appdir / "usr/python/bin/python3"
        python.parent.mkdir(parents=True)
        python.symlink_to(sys.executable)
        apprun = appdir / "AppRun"
        shutil.copyfile(source / "builder/linux/AppRun", apprun)
        result = subprocess.run(["sh", str(apprun), "--dump-shaders", str(self.game)], capture_output=True, text=True,
                                env=dict(os.environ, APPIMAGE=str(self.root / "Setup.AppImage")))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([p.parent for p in self.root.glob("mesa-27.0.0-devel-*.zip")], [self.root])
        self.assertIn("@@done", result.stdout)


if __name__ == "__main__":
    unittest.main()
