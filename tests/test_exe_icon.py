"""The game's exe has Longshot's icon (nx2/build.py exe_resources): builder/setup/longshot.ico, as work/longshot_brand.res,
in every build, beside the name Windows shows for it when the builder read the game's from its .nup (work/longshot_game.res,
nx2/game_title.py: version strings, no icon). A release's exe, built where there is no .nup, had no icon: none in its window
(host/display.c loads icon 1) or its taskbar. The .res is read back here: icon group 1 with every size of the .ico, each the
.ico's own PNG; the game's .res no icon of its own; a developer's longshot.res not linked. No game, no compiler.

    python -I -B tests/test_exe_icon.py
"""
import importlib.util
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import game_title  # noqa: E402
import make_icon  # noqa: E402


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def entries(res):
    """(type, name, data) of each entry of a .res"""
    out, at = [], 0
    while at < len(res):
        size, header = struct.unpack("<II", res[at:at + 8])
        kind, name = struct.unpack("<xxHxxH", res[at + 8:at + 16])
        out.append((kind, name, res[at + header:at + header + size]))
        at += header + size + (-size & 3)
    return out


def rgba(png):
    """(width, height, RGBA bytes) of an 8-bit, non-interlaced RGBA PNG, decoded here: the builders have no Pillow"""
    width, height, depth, kind, _, _, interlace = struct.unpack(">IIBBBBB", png[16:29])
    if (depth, kind, interlace) != (8, 6, 0):
        fail("a PNG that is not 8-bit RGBA without interlacing (depth %d, colour type %d, interlace %d)" % (depth, kind, interlace))
    data, at = b"", 8
    while at < len(png):
        size, tag = struct.unpack(">I4s", png[at:at + 8])
        if tag == b"IDAT":
            data += png[at + 8:at + 8 + size]
        at += 12 + size
    raw, stride = zlib.decompress(data), width * 4
    out, prev = bytearray(), bytearray(stride)
    for y in range(height):
        f, row = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - 4] if i >= 4 else 0
            b, c = prev[i], (prev[i - 4] if i >= 4 else 0)
            if f == 1:
                row[i] = (row[i] + a) & 255
            elif f == 2:
                row[i] = (row[i] + b) & 255
            elif f == 3:
                row[i] = (row[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += row
        prev = row
    return width, height, bytes(out)


def shrunk(picture, size):
    """an RGBA picture (rgba's) averaged down to size x size, colour weighted by alpha: [(r, g, b, a)]"""
    width, height, px = picture
    out = []
    for by in range(size):
        for bx in range(size):
            r = g = b = a = n = 0
            for y in range(by * height // size, (by + 1) * height // size):
                for x in range(bx * width // size, (bx + 1) * width // size):
                    i = (y * width + x) * 4
                    w = px[i + 3]
                    r, g, b, a, n = r + px[i] * w, g + px[i + 1] * w, b + px[i + 2] * w, a + w, n + 1
            out.append((r / a, g / a, b / a, a / n) if a else (0, 0, 0, 0))
    return out


def distance(one, two):
    """the mean difference of two shrunk pictures, colour counted where both are drawn (0 to 255)"""
    total = 0
    for (r1, g1, b1, a1), (r2, g2, b2, a2) in zip(one, two):
        total += abs(a1 - a2)
        if a1 > 64 and a2 > 64:
            total += (abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2)) / 3
    return total / len(one)


SAME = 12   # (the largest distance at which the icon is still the picture: make_icon's own images are 7.8 at 32 pixels and
#              1.7 at 256, the Hookshot icon's 35)


def check_picture(images):
    """the .ico's 32 and 256 pixel images are assets/longshot.png's. builder/setup/longshot.ico was renamed from
    hookshot.ico when the project became Longshot and kept the Hookshot picture in every exe since, while the logo changed"""
    picture = rgba((ROOT / "assets" / "longshot.png").read_bytes())
    for size, png in images:
        if size in (32, 256):
            got = distance(shrunk(rgba(png), 16), shrunk(picture, 16))
            if got > SAME:
                fail("builder/setup/longshot.ico's %d pixel image is not assets/longshot.png (distance %.1f, above %d): "
                     "make it again: python nx2/make_icon.py assets/longshot.png work, then copy work/longshot.ico to builder/setup/"
                     % (size, got, SAME))


def main():
    ico = (ROOT / "builder" / "setup" / "longshot.ico").read_bytes()
    images = make_icon.images_of_ico(ico)
    if [s for s, _ in images] != list(make_icon.SIZES):
        fail("builder/setup/longshot.ico's sizes are %s, not %s" % ([s for s, _ in images], list(make_icon.SIZES)))
    check_picture(images)
    if make_icon.ico_of(images) != ico:
        fail("make_icon.ico_of does not write longshot.ico back as it is: images_of_ico read it wrong")
    build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    if "exe_resources() +" not in build:
        fail("nx2/build.py does not link exe_resources() into the exe")
    spec = importlib.util.spec_from_file_location("build", ROOT / "nx2" / "build.py")
    build_mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build_mod)
    with tempfile.TemporaryDirectory(prefix="exe-icon-") as work:
        # (a developer's picture is not linked: Longshot's icon is the exe's)
        Path(work, "longshot.res").write_bytes(make_icon.res_of(images[:1]))
        got = build_mod.exe_resources(work)
        if got != [os.path.join(work, build_mod.BRAND_RES)]:
            fail("with no name from a .nup, exe_resources gives %s, not Longshot's icon alone" % got)
        res = Path(got[0]).read_bytes()
        found = entries(res)
        icons = [(n, d) for k, n, d in found if k == 3]
        groups = [(n, d) for k, n, d in found if k == 14]
        if len(groups) != 1 or groups[0][0] != 1:
            fail("the .res has icon groups %s, not group 1 (what host/display.c loads)" % [n for n, _ in groups])
        if [d for _, d in icons] != [png for _, png in images]:
            fail("the .res's icons are not longshot.ico's images")
        if struct.unpack("<HHH", groups[0][1][:6])[2] != len(images):
            fail("the icon group does not name every image of the .ico")
        if build_mod.exe_resources(work) != got or Path(got[0]).read_bytes() != res:
            fail("a second build makes the .res again differently")
        # (the game's name, read from a .nup, kept in the folder: its .res the name alone, linked beside Longshot's icon)
        folder = Path(work, "game")
        folder.mkdir()
        (folder / game_title.TITLE_FILE).write_text("A game\n", encoding="utf-8")
        named = Path(work, game_title.RES_FILE)
        if not game_title.write_res(str(folder), str(named)):
            fail("game_title.write_res wrote nothing for a game with a name")
        kinds = {k for k, _, _ in entries(named.read_bytes()) if k}
        if kinds != {16}:
            fail("the game's .res holds resource types %s, not its version strings alone (16): its icon would clash" % kinds)
        if build_mod.exe_resources(work) != [os.path.join(work, build_mod.BRAND_RES), str(named)]:
            fail("with the game's name, exe_resources gives %s, not Longshot's icon and the name" % build_mod.exe_resources(work))
        legacy = make_icon.res_of(images[:1], {"FileDescription": "Cached title"})
        named.write_bytes(legacy)
        linked = build_mod.exe_resources(work)
        version = [entry for entry in entries(legacy) if entry[0] == 16]
        actual = [entry for entry in entries(Path(linked[1]).read_bytes()) if entry[0]]
        if actual != version or named.read_bytes() != legacy:
            fail("cached game resource still links its old icon or loses its title")
        named.write_bytes(make_icon.res_of(images[:1]))
        if build_mod.exe_resources(work) != [linked[0]]:
            fail("an icon-only cached resource was linked beside the brand")
    print("PASS: the exe's icon is Longshot's (%d sizes), beside the game's name when it has one" % len(images))


if __name__ == "__main__":
    main()
