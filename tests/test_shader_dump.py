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
        mesa = self.cache / "driver" / shader_dump.LAYOUT / ("01" * 16) / "Adreno (TM) 830"
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
            self.assertIn(f"driver/mesa_shader_cache_sf/{'01' * 16}/Adreno (TM) 830/foz_cache.foz", entries)
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
        (layout / ("01" * 16)).rename(layout / ("09" * 16))
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

    def run_apprun(self, *args):
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
        result = subprocess.run(["sh", str(apprun), *(str(arg) for arg in args)], capture_output=True, text=True,
                                env=dict(os.environ, APPIMAGE=str(self.root / "Setup.AppImage")))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_apprun_exports_beside_image_without_build_tools(self):
        output = self.run_apprun("--dump-shaders", self.game)
        self.assertEqual([p.parent for p in self.root.glob("mesa-27.0.0-devel-*.zip")], [self.root])
        self.assertIn("@@done", output)

    def restore_target(self):
        target = self.root / "restore-target"
        (target / "bin").mkdir(parents=True)
        (target / "bin/longshot").write_bytes(shader_dump.RUNTIME_MAGIC.encode("ascii"))
        (target / "longshot_shaders.bin").write_bytes(b"seed")
        (target / "longshot_options.ini").write_bytes(b"player settings")
        (target / "cache").mkdir()
        (target / "cache/longshot_pipelines.bin").write_bytes(b"previous cache")
        return target

    def rewrite_dump(self, dump, edit):
        with zipfile.ZipFile(dump) as original:
            entries = {name: original.read(name) for name in original.namelist()}
        manifest = json.loads(entries.pop("manifest.json"))
        edit(manifest, entries)
        with zipfile.ZipFile(dump, "w") as archive:
            archive.writestr("manifest.json", json.dumps(manifest))
            for name, data in entries.items():
                archive.writestr(name, data)

    def test_restore_round_trip_preserves_settings_and_previous_cache(self):
        dump = self.export()
        target = self.restore_target()
        (target / "longshot_version.txt").write_text("version=different-setup-build\n", encoding="utf-8")
        manifest = shader_dump.inspect_dump(dump)
        self.assertEqual(manifest["driver"]["mesa_version"], "27.0.0-devel")
        self.assertEqual(manifest["longshot_version"], "fixture")
        self.assertEqual(shader_dump.restore(target, dump), "27.0.0-devel")
        restored = shader_dump.active_cache(target / "cache")
        self.assertEqual((target / "longshot_options.ini").read_bytes(), b"player settings")
        self.assertEqual((target / "cache/longshot_pipelines.bin").read_bytes(), b"previous cache")
        with zipfile.ZipFile(dump) as archive:
            for name in archive.namelist():
                if name != "manifest.json":
                    self.assertEqual((restored / shader_dump.member_target(name)).read_bytes(), archive.read(name))
        output = shader_dump.export(target, self.root, self.now)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.read("longshot/longshot_pipelines.bin"), b"fixture")

    def test_apprun_restores_without_build_tools(self):
        dump = self.export()
        target = self.restore_target()
        output = self.run_apprun("--restore-shaders", target, dump)
        self.assertIn("@@done 27.0.0-devel", output)
        self.assertTrue((target / "cache" / shader_dump.ACTIVE).is_file())

    def test_bad_payload_and_cancellation_preserve_active_cache(self):
        dump = self.export()
        target = self.restore_target()
        shader_dump.restore(target, dump)
        previous = shader_dump.active_cache(target / "cache")

        def cancel(_):
            raise KeyboardInterrupt

        with self.assertRaises(KeyboardInterrupt):
            shader_dump.restore(target, dump, on_progress=cancel)
        with self.assertRaisesRegex(shader_dump.DumpError, "in use"):
            with shader_dump.cache_lock(target / "cache/.shader-dump.lock", exclusive=False):
                shader_dump.restore(target, dump)
        self.rewrite_dump(dump, lambda manifest, entries: entries.update({"longshot/longshot_pipelines.bin": b"damaged"}))
        with self.assertRaisesRegex(shader_dump.DumpError, "failed validation"):
            shader_dump.restore(target, dump)
        self.assertEqual(shader_dump.active_cache(target / "cache"), previous)
        self.assertEqual(list((target / "cache").glob("shader-dump-*")), [previous])

    def test_refuses_path_traversal_in_inventory(self):
        dump = self.export()
        target = self.restore_target()

        def add_escape(manifest, entries):
            path = "longshot/../longshot_options.ini"
            entries[path] = b"bad"
            manifest["files"].append({"path": path, "size": 3, "sha256": hashlib.sha256(b"bad").hexdigest()})

        self.rewrite_dump(dump, add_escape)
        with self.assertRaisesRegex(shader_dump.DumpError, "Unexpected file"):
            shader_dump.restore(target, dump)
        self.assertEqual((target / "longshot_options.ini").read_bytes(), b"player settings")

    def test_refuses_incompatible_seed_or_executable(self):
        dump = self.export()
        target = self.restore_target()
        (target / "longshot_shaders.bin").write_bytes(b"other seed")
        with self.assertRaisesRegex(shader_dump.DumpError, "different game shader seed"):
            shader_dump.restore(target, dump)
        (target / "bin/longshot").write_bytes(b"old executable")
        with self.assertRaisesRegex(shader_dump.DumpError, "executable in the selected game folder"):
            shader_dump.restore(target, dump)
        self.assertFalse((target / "cache" / shader_dump.ACTIVE).exists())

    def test_builder_restore_does_not_scan_the_executable_it_just_built(self):
        dump = self.export()
        target = self.restore_target()
        (target / "bin/longshot").write_bytes(b"freshly built executable without incidental parser strings")
        self.assertEqual(shader_dump.restore(target, dump, built_by_setup=True), "27.0.0-devel")
        self.assertTrue((target / "cache" / shader_dump.ACTIVE).is_file())


if __name__ == "__main__":
    unittest.main()
