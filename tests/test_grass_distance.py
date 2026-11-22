"""The Grass distance row (gpu/vk/menu/detail.cpp) and the reach it gives (gpu/vk/vk_draw.cpp grass_reach) agree: as many
choices on the row as labels in tx_grass_distance and reaches in grass_reach's table, Game's the first at 1x, the default
(vk_menu.h) one of them. A game-free C++ check exercises the actual buffer patch.

    python -I -B tests/test_grass_distance.py
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile
import unittest

VK = Path(__file__).resolve().parents[1] / "gpu" / "vk"
MENU = (VK / "menu" / "detail.cpp").read_text(encoding="utf-8")
DRAW = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
HEADER = (VK / "vk_menu.h").read_text(encoding="utf-8")
RUN_ALL_ARGS = ["--cxx", "{cxx}"]
CXX = os.environ.get("CXX", "g++")


class GrassDistance(unittest.TestCase):
    def test_row_labels_and_reaches_agree(self):
        row = re.search(r'\.row\(\{\.key = "grass_distance",\s*\.label = "Grass distance",'
                        r'.*?\.value = &opts\.grass_distance,\s*\.lo = (\d+),\s*\.hi = (\d+),'
                        r'.*?\.text = \[\]\(int v\) \{ return std::string\(tx_grass_distance\(v\)\); \}', MENU, re.S)
        self.assertTrue(row, "no Grass distance row")
        lo, hi = int(row.group(1)), int(row.group(2))
        labels = re.search(r'const char\* tx_grass_distance\(int v\) \{\s*static const char\* t\[\] = \{([^}]*)\}', MENU)
        reaches = re.search(r'float grass_reach\(\) \{.*?static const float times\[\] = \{([^}]*)\}', DRAW, re.S)
        self.assertTrue(labels and reaches)
        n_labels = len(re.findall(r'"[^"]*"', labels.group(1)))
        times = [float(x.strip().rstrip("f")) for x in reaches.group(1).split(",")]
        self.assertEqual(lo, 0)
        self.assertEqual(hi + 1, n_labels)
        self.assertEqual(hi + 1, len(times))
        self.assertEqual(times[0], 1.0, "Game's is the game's own reach")
        self.assertEqual(times, sorted(times))
        default = int(re.search(r'int grass_distance = (\d+);', HEADER).group(1))
        self.assertTrue(lo <= default <= hi)

    def test_buffer_patch(self):
        self.assertIn('grass_patch(ub_prog_key, index, cur->mapped + at, n, reach,', DRAW)
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "grass_test"
            subprocess.run([CXX, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(VK.parent.parent / "tests" / "grass_distance_test.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=CXX)
    args, rest = parser.parse_known_args()
    CXX = args.cxx
    unittest.main(argv=[__file__, *rest])
