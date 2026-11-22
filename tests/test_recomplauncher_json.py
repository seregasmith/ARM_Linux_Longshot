"""Game-free checks of recomplauncher.json: it parses, and every settings key is a row of the in-game menu.

Run with: python -I tests/test_recomplauncher_json.py
"""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RecompLauncherJsonTests(unittest.TestCase):
    def test_settings_are_menu_rows(self):
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["format"], 1)
        menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
        rows = {re.sub(" ", "_", name.lower()) for name in re.findall(r'\{TAB_[A-Z]+, "([^"]+)"', menu)}
        # (vk_menu.cpp ini_key: the rows whose key is not their name, renamed rows keeping the key an options file has:
        # output_res, resolution, upscaling, frame_rate_cap, dlss)
        ini_key = menu.split("const char* ini_key(const Row& r) {", 1)[1].split("\n}", 1)[0]
        rows |= set(re.findall(r'return "([a-z_0-9]+)";', ini_key))
        # (#1633: the rows the menu's modules describe, gpu/vk/menu/, by their keys)
        for module in (ROOT / "gpu" / "vk" / "menu").glob("*.cpp"):
            rows |= set(re.findall(r'\.key = "([a-z_0-9]+)"', module.read_text(encoding="utf-8")))
        keys = {key for setting in manifest["settings"] for choice in setting["choices"] for key in choice["set"]}
        self.assertTrue(keys)
        # (keys the game reads that are no menu row: Compile shaders, which the preparation reads, vk_draw.cpp compile_all)
        draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
        read_elsewhere = {"compile_shaders"} if '"compile_shaders="' in draw else set()
        self.assertEqual(keys - rows - read_elsewhere, set())

    def test_builds_into_the_launchers_game_folder(self):
        # (RecompLauncher issue 215: without --out the build went to ~/Games/Longshot, not the drive the player
        # installed to. The folder it builds into is the one it plays, and the settings and saves live in it.)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        prepare = manifest["prepare"]["args"]
        out = prepare[prepare.index("--out") + 1]
        self.assertTrue(out.startswith("{install}/"))
        self.assertEqual(manifest["start"]["args"], ["--play", out])
        self.assertEqual(manifest["folders"]["game"][0]["path"], out)
        self.assertEqual(manifest["config"][0]["file"][0]["path"], out + "/longshot_options.ini")
        self.assertIn(out + "/user", manifest["saves"])
        self.assertNotIn("{home}/Games", json.dumps(manifest))

    def test_builds_again_from_what_the_game_folder_kept(self):
        # (RecompLauncher's prepare.keeps_copy: with the player's copy gone, it runs the step again leaving out every argument
        # with {game} in it. What is left must still build into the same folder: make_game then builds from the files it kept)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        prepare = manifest["prepare"]
        self.assertTrue(prepare["keeps_copy"])
        with_copy = [arg for arg in prepare["args"] if "{game}" in arg]
        self.assertEqual(with_copy, ["--game={game}"])
        out = manifest["folders"]["game"][0]["path"]
        without_copy = [arg for arg in prepare["args"] if "{game}" not in arg]
        self.assertEqual(without_copy[:4], ["--cli", "--prepare", "--out", out])

    def test_the_work_folder_is_in_the_game_folder(self):
        # (the work folder is what makes a build after an update quick: the translated code and the compiled objects. In the
        # install folder it moves and uninstalls with the game, and nothing is left in the player's ~/.cache or LOCALAPPDATA.
        # recompdata/cache is RecompLauncher's {data}/cache, which its backups and cloud saves leave out. Written out, not
        # {data}, so a launcher from before {data} builds into the same place)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        args = manifest["prepare"]["args"]
        work = args[args.index("--work") + 1]
        self.assertEqual(work, "{install}/recompdata/cache/work")
        out = manifest["folders"]["game"][0]["path"]
        self.assertFalse(work.startswith(out + "/"), "the work folder stays out of the game folder the build writes")
        self.assertFalse(any(work.startswith(save + "/") or work == save for save in manifest["saves"]))

    def test_a_finished_build_is_told_by_its_image(self):
        # (prepare.done: the file every finished build has on Windows and Linux, the one Play's start script looks for)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        out = manifest["folders"]["game"][0]["path"]
        self.assertEqual(manifest["prepare"]["done"], [out + "/image.bin"])
        self.assertIn('[ -f "$folder/image.bin" ]', (ROOT / "builder" / "linux" / "longshot-launch").read_text(encoding="utf-8"))

    def test_appimage_carries_it(self):
        # (the release's copy, with its own prepare.version: release_manifest)
        self.assertIn('"$PY" -I scripts/builder_package.py manifest "$VERSION" --out "$APPDIR/recomplauncher.json"',
                      (ROOT / "scripts" / "linux" / "appimage.sh").read_text(encoding="utf-8"))

    def test_a_release_asks_for_a_new_build(self):
        # (RecompLauncher keeps a game set up through an update and builds it again only when prepare.version changes: each
        # release's copy carries its own version, and nothing else of the file changes)
        bp = self.builder_package()
        shipped = json.loads(bp.release_manifest("v3.2.1").decode("utf-8"))
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        self.assertEqual(shipped["prepare"]["version"], "v3.2.1")
        manifest["prepare"]["version"] = "v3.2.1"
        self.assertEqual(shipped, manifest)

    def test_a_release_rebuilds_without_the_copy(self):
        # (the new build an update asks for runs from what the game folder kept, so the player isn't asked for the copy)
        shipped = json.loads(self.builder_package().release_manifest("v3.2.1").decode("utf-8"))
        self.assertTrue(shipped["prepare"]["keeps_copy"])

    @staticmethod
    def builder_package():
        import importlib.util
        spec = importlib.util.spec_from_file_location("builder_package", ROOT / "scripts" / "builder_package.py")
        bp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bp)
        return bp

    def test_windows_zip_carries_it(self):
        # (scripts/builder_package.py zip: the setup as LongshotBuilder.exe, the builder, tools.lock and VERSION, and this
        # file at the top, where RecompLauncher reads it; the setup installs the tools tools.lock names, with no release lookup)
        import importlib.util, tempfile, zipfile
        spec = importlib.util.spec_from_file_location("builder_package", ROOT / "scripts" / "builder_package.py")
        bp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bp)
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            (package / "tools").mkdir(parents=True)
            (package / "LongshotBuilder.exe").write_bytes(b"MZ builder")
            setup = Path(tmp) / "setup.exe"
            setup.write_bytes(b"MZ setup")
            out = Path(tmp) / "LongshotBuilder-v3.0.0-windows-x64.zip"
            import contextlib, io
            with contextlib.redirect_stdout(io.StringIO()):
                bp.make_zip(str(package), "v3.0.0", str(setup), str(out))
            with zipfile.ZipFile(out) as z:
                self.assertEqual(sorted(z.namelist()), ["LongshotBuilder.exe", "LongshotBuilder/LongshotBuilder.exe",
                                                        "LongshotBuilder/tools/version.txt", "VERSION",
                                                        "recomplauncher.json", "tools.lock"])
                self.assertEqual(z.read("LongshotBuilder.exe"), b"MZ setup")
                self.assertEqual(z.read("VERSION"), b"v3.0.0\n")
                # (and where the builder and the game read it: without it a release's game logged "longshot unknown")
                self.assertEqual(z.read("LongshotBuilder/tools/version.txt"), b"v3.0.0\n")
                # (and in the setup step's version, so RecompLauncher builds the game again after the update)
                self.assertEqual(json.loads(z.read("recomplauncher.json"))["prepare"]["version"], "v3.0.0")
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        self.assertIn("LongshotBuilder.exe", manifest["start"]["program"])
        self.assertIn("LongshotBuilder.exe", manifest["prepare"]["program"])
        workflow = (ROOT / ".forgejo" / "workflows" / "build-windows.yml").read_text(encoding="utf-8")
        self.assertIn('--out "dist/LongshotBuilder-$TAG-windows-x64.zip"', workflow)

    def test_no_repository_it_cannot_read(self):
        # (RecompLauncher's "repo" is a GitHub owner/name, and Longshot's releases are on git.magicbean.dev: none is named)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        self.assertNotIn("repo", manifest)


if __name__ == "__main__":
    unittest.main()
