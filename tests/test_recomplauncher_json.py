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
        self.assertIn(out + "/user/save", manifest["saves"])
        self.assertNotIn("{home}/Games", json.dumps(manifest))

    def test_saves_are_the_save_folders_on_both_systems(self):
        # (RecompLauncher's backups and cloud saves copy what "saves" names. Started by the launcher's Play, the game keeps
        # its saves in <user>/save: <game folder>/user on Windows, $XDG_DATA_HOME/longshot ({local}/longshot) on Linux. That
        # Linux folder, and %LOCALAPPDATA%\Longshot on Windows, also hold the builder, the launcher, NVIDIA's DLSS files, the
        # NFC tags and the save backups, none of which a save copy may carry: so the save folder, never the whole data folder)
        # (save-work beside it holds a save while it is being written: the game moves it into save at Commit, host/nn_fs.c)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        out = manifest["folders"]["game"][0]["path"]
        windows, linux = out + "/user", "{local}/longshot"
        self.assertEqual(manifest["saves"], [windows + "/save", windows + "/mouse_sens.txt",
                                             linux + "/save", linux + "/mouse_sens.txt"])
        self.assertIn('"%s\\\\save\\\\%016llx%016llx"', (ROOT / "host" / "nn_fs.c").read_text(encoding="utf-8"))
        main = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
        self.assertIn('snprintf(beside_user, MAX_PATH, "%s" SEP_S "user", dir);', main)
        self.assertIn('"/longshot"', main)
        self.assertIn('"%s/mouse_sens.txt", hos_user_dir', (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8"))

    def test_old_save_copies_go_back_where_they_came_from(self):
        # (RecompLauncher restores a copy made before a move into the new folder at the same place in the list, only when
        # the list keeps its length. The copies made before this list name <user> itself, so a copy of <user>/save/<id>
        # would land in <user>/save/save/<id>. A list of another length restores each copy into the folders it names)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        before = ["{local}/longshot", "{install}/Longshot/user"]
        self.assertNotEqual(len(manifest["saves"]), len(before))

    def test_the_saves_button_opens_the_save_folder_there_is(self):
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        out = manifest["folders"]["game"][0]["path"]
        places = manifest["folders"]["saves"]
        self.assertEqual([p.get("if") for p in places[:2]], [p["path"] for p in places[:2]])
        self.assertEqual([p["path"] for p in places], [out + "/user/save", "{local}/longshot/save", out])

    def test_builds_again_from_what_the_game_folder_kept(self):
        # (RecompLauncher's prepare.keeps_copy: with the player's copy gone, it runs the step again leaving out every argument
        # with {input:game} in it. What is left must still build into the same folder: make_game then builds from the files
        # it kept)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        prepare = manifest["prepare"]
        self.assertTrue(prepare["keeps_copy"])
        with_copy = [arg for arg in prepare["args"] if "{input:game}" in arg]
        self.assertEqual(with_copy, ["--game={input:game}"])
        out = manifest["folders"]["game"][0]["path"]
        without_copy = [arg for arg in prepare["args"] if "{input:game}" not in arg]
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
        # (the file as it is: nothing in it changes per release)
        self.assertIn('cp recomplauncher.json "$APPDIR/recomplauncher.json"',
                      (ROOT / "scripts" / "linux" / "appimage.sh").read_text(encoding="utf-8"))

    def test_the_copy_is_the_input_game(self):
        # (RecompLauncher's named input game: the .nup or the extracted folder, refused when its build ID isn't the one
        # Longshot is made from)
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        self.assertNotIn("game", manifest)
        copy = manifest["inputs"][0]
        self.assertEqual((copy["id"], copy["role"]), ("game", "game"))
        self.assertEqual([f["type"] for f in copy["formats"]], ["file", "folder"])
        self.assertEqual(copy["check"], {"nup_build_id": "e0f3ccfa"})

    def test_a_release_asks_for_a_new_build_by_itself(self):
        # (RecompLauncher builds the game again whenever the release it installed changes: no prepare.version to stamp, and
        # no timeout, which it no longer reads)
        prepare = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))["prepare"]
        self.assertNotIn("version", prepare)
        self.assertNotIn("timeout", prepare)

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
                # (and this file as it is)
                self.assertEqual(z.read("recomplauncher.json"), (ROOT / "recomplauncher.json").read_bytes())
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
