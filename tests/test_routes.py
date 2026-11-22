"""Routes (assets/routes/, scripts/devtools/routes.py): every committed route holds only what a route may, as the game writes
it with HOS_INPUT_ROUTE=1 (host/input_movie.c): the header naming a repository save, and pads, motion, the mouse camera's
turns, views as angles alone, marks, frames, the Debug tools' actions and rows and the loads' anchors, every field a
number or a hex word. No guest words (a view's "w" or "in"), no addresses (the menu mouse's focus and itemkey lines), no
text. The checker itself: a route the game would write passes; one with a guest word, a menu line, a stray field, a text
where a number goes, a full recording's header or a save that is not in the repository is refused.

    python -I -B tests/test_routes.py
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from devtools import routes   # noqa: E402

RUN_ALL_ARGS = []


def check(ok, message):
    if not ok:
        raise SystemExit("FAIL: " + message)


def line(**kw):
    return json.dumps(kw)


HEAD = dict(format="ls-input-recording/1", fps=60, start=120, source="longshot", **{"from": "play"}, build="v1.2 (source 1a2b3c4d)",
            fixed_time="1767225600", step="per frame", camera_before=3, link_before=1, clock=2183.0,
            map="KokiriForest_Child", age="child", profile="route", save="kokiri-start")
GOOD = [line(**HEAD),
        line(f=0, cmd="option", id=33, v=-1),
        line(f=0, cmd="pad", frames=9, bits="0x0", buttons="", lx=0, ly=0, rx=0, ry=0),
        line(f=9, cmd="pad", frames=10, bits="0x101", buttons="A+L", lx=0, ly=32767, rx=0, ry=0, fl="0x10002", fc="0x0", fh=1),
        line(f=0, cmd="cam", n=0, i=0, native=1, free=1, held=0, dyaw=0.5, dpitch=-0.25),
        line(f=0, cmd="view", n=0, i=0, yaw=5.25, pitch=0),
        line(f=0, cmd="mark", n=0, i=0, pos=[0, 2, -3]),
        line(f=5, cmd="motion", n=103, dt=5000000, v=[0.5] * 18),
        line(f=5, cmd="gyro", c=29, i=0, live=3, top=103, upto=3),
        line(f=12, cmd="action", a=1005),
        line(f=40, cmd="anchor", n=0, gap=21),
        line(f=0, cmd="frames", count=19, reads=2, read=0)]


def main():
    saves = ROOT / "assets" / "saves"
    check(not routes.check_lines(GOOD, str(saves)), "a route as the game writes it refused: %s" % routes.check_lines(GOOD, str(saves)))
    bad = {
        "a view's guest words": GOOD[:5] + [line(f=0, cmd="view", n=0, i=0, yaw=5.25, pitch=0, w="c2aa0000" * 14)],
        "the camera's inputs": GOOD[:5] + [line(f=0, cmd="view", n=0, i=0, yaw=5.25, pitch=0, **{"in": "0" * 104})],
        "a menu mouse's address": GOOD + [line(f=59, cmd="focus", screen="0xabc", button="0xdef")],
        "an item key": GOOD + [line(f=60, cmd="itemkey", screen="0xabc", key="Item_Bow")],
        "a note's text": GOOD + [line(f=1, cmd="note", text="anything")],
        "a stray field": GOOD + [line(f=1, cmd="mark", n=1, i=0, pos=[0, 0, 0], extra=1)],
        "a text where a number goes": GOOD + [line(f=1, cmd="action", a="1005")],
        "a word in buttons": GOOD + [line(f=20, cmd="pad", frames=1, bits="0x0", buttons="a secret", lx=0, ly=0, rx=0, ry=0)],
        "a full recording's header": [line(**dict(HEAD, profile="full"))] + GOOD[1:],
        "a save not in the repository": [line(**dict(HEAD, save="my-own-adventure"))] + GOOD[1:],
        "a header field more": [line(**dict(HEAD, player="someone"))] + GOOD[1:],
        "from the boot": [line(**dict(HEAD, **{"from": "boot"}))] + GOOD[1:],
        "a text in the header's map": [line(**dict(HEAD, map="my notes, here"))] + GOOD[1:],
        "a key twice": GOOD + ['{"f":1,"cmd":"view","n":1,"i":0,"yaw":"c2aa0000c2aa0000","yaw":1,"pitch":0}'],
        "a NaN": GOOD + ['{"f":1,"cmd":"view","n":1,"i":0,"yaw":NaN,"pitch":0}'],
        "an infinity": GOOD + ['{"f":1,"cmd":"mark","n":1,"i":0,"pos":[Infinity,0,0]}'],
    }
    for what, lines in bad.items():
        check(routes.check_lines(lines, str(saves)), "a route with %s passed" % what)
    print("PASS: the checker: a route as the game writes it passes; %d kinds of what a route may not hold refused" % len(bad))
    folder = ROOT / "assets" / "routes"
    files = sorted(folder.glob("*.jsonl")) if folder.is_dir() else []
    for path in files:
        problems = routes.check_lines(path.read_text(encoding="utf-8").splitlines(), str(saves))
        check(not problems, "%s: %s" % (path.relative_to(ROOT), "; ".join(problems[:6])))
    check((folder / "README.md").is_file(), "assets/routes/ has no README.md")
    print("PASS: %d committed routes hold only what a route may" % len(files))
    # (the game's own route lines are the checker's: the fields host/input_movie.h writes with HOS_INPUT_ROUTE are listed)
    movie = (ROOT / "host" / "input_movie.c").read_text(encoding="utf-8")
    check("if (S.route) v.has_w = 0, v.has_in = 0;" in movie and "if (movie_on() && !S.route) {" in movie,
          "host/input_movie.c writes guest words or menu addresses into a route")
    return 0


if __name__ == "__main__":
    sys.exit(main())
