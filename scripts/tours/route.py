"""The pipeline tour's route: the Debug tab's action numbers as the source has them (host/hos_main.c,
gpu/vk/oot_cheat_items.h, gpu/vk/vk_menu.cpp), the stops each part visits and the presses made there."""
import os
import re

from tours import ROOT

def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def define(text, name):
    m = re.search(r"#define\s+%s\s+(\d+)" % name, text)
    if not m:
        raise SystemExit("pipeline_tour: %s not found" % name)
    return int(m.group(1))


def bancs(main):
    """Each destination's maps, child and adult (None: none), as host/hos_main.c's oot_destinations has them"""
    table = main.split("oot_destinations[] = {", 1)[1].split("\n};", 1)[0]
    q = r'(NULL|"[^"]*")'
    out = []
    for m in re.finditer(r"\{%s,\s*%s,\s*%s,\s*%s," % (q, q, q, q), table):
        out.append(tuple(None if v == "NULL" else v.strip('"') for v in (m.group(1), m.group(4))))
    return out


def actions(root_read=read):
    """The action numbers and the lists they index: {age, item, bancs, teleports: [(name, adult_only)], dungeons,
    items: {pouch name: (index, kind)}}"""
    main, items_h, menu = root_read("host/hos_main.c"), root_read("gpu/vk/oot_cheat_items.h"), root_read("gpu/vk/vk_menu.cpp")
    names = re.search(r"teleport_names\[\]\s*=\s*\{(.*?)\};", menu, re.S)
    notes = re.search(r"teleport_notes\[\]\s*=\s*\{(.*?)\};", menu, re.S)
    if not names or not notes:
        raise SystemExit("pipeline_tour: the teleport list not found in gpu/vk/vk_menu.cpp")
    labels = re.findall(r'"((?:[^"\\]|\\.)*)"', names.group(1))
    note_list = [n.strip() for n in notes.group(1).split(",")]
    destinations = len(re.findall(r"\},\s*/\*", main.split("oot_destinations[]", 1)[1].split("};", 1)[0]))
    if destinations != len(labels) or len(note_list) != len(labels):
        raise SystemExit("pipeline_tour: %d teleport names, %d notes, %d destinations" % (len(labels), len(note_list), destinations))
    items = re.findall(r'OOT_ITEM\("[^"]*",\s*"([^"]+)",\s*(\d)\)', items_h)
    dungeons = re.search(r'\{"Dungeons", (\d+)\}', menu)
    return {
        "teleport": define(main, "OOT_TELEPORT_ACTION"),
        "age": define(main, "OOT_AGE_ACTION"),
        "bancs": bancs(main),
        "item": define(items_h, "OOT_CHEAT_ITEM_ACTION"),
        "teleports": [(label, "adult only" in note) for label, note in zip(labels, note_list)],
        "dungeons": int(dungeons.group(1)) if dungeons else len(labels),
        "items": {name: (i, int(kind)) for i, (name, kind) in enumerate(items)},
    }


QUEST_ITEMS = ("Item_Medal_", "Item_Stone_", "Item_Melody_")
# Stops left out: a scene starts there that waits for something the tour does not do, and the map can then not be left
LEFT_OUT = {("adult", "Sacred Forest Meadow"): "Sheik's lesson starts on arrival and waits for the ocarina to be played",
            # (what only these stops met, against everything else)
            ("child", "Lost Woods"): "20 pipelines no other stop met", ("adult", "Lost Woods"): "7",
            ("child", "Sacred Forest Meadow"): "4",
            # (pipeline_tour.py value over full and short runs: what each stop met that neither the players' lists in
            # assets/pipeline_states/contrib nor any other kept stop has; under 10 is not worth the stop's 15-30 s)
            ("adult", "Kokiri Forest"): "4 pipelines no other stop or player's list has", ("child", "Graveyard"): "4",
            ("child", "Death Mountain Crater"): "4 (the gameover part still goes there)", ("adult", "Shadow Temple"): "2",
            ("adult", "Dodongo's Cavern"): "6", ("adult", "Gerudo Training Ground"): "7", ("adult", "Haunted Wasteland"): "7",
            ("child", "Ice Cavern"): "6", ("child", "Ganon's Castle"): "9", ("child", "Spirit Temple"): "4"}
# Game Over: the "gameover" part (its own run: hearts are not protected there) takes Link, a child, to the crater, whose heat
# empties his hearts; A now and then chooses Continue on the Game Over screen
GAME_OVER_AT = "Death Mountain Crater"
GAME_OVER_FRAMES = 60 * 75     # (the heat takes a child's three hearts in ~45 s; Try Again, back in the crater by ~60 s)
GAME_OVER_PRESS_FROM = 60 * 40
# Stops the game does not teleport out of (the request is sent, the map does not change), visited last in their part
LAST = {"Haunted Wasteland"}
# Overworld stops seen at night too (the Debug tab's clock: !cheat 100 + hour), then back to noon. Measured on a short run: Hyrule
# Field +4 pipelines at night, Kakariko +10, for ~10 s each: none by default (add names here for a run that wants them)
NIGHT_AT = set()
TIME_ACTION = 100
# Where Link uses the sword, the shield and an item picked from the pause menu (a different one at each such stop of the whole
# route: item_stops). A pipeline is a program and its fixed draw state (formats, blending, depth); a scene's light and fog are
# inputs to the same programs, so the sword, the shield and an item draw with the same pipelines wherever they are used. Once an
# age (the child's and the adult's models and items differ), at the first stop of the age's overworld:
ITEMS_ONCE = {"child": "Kokiri Forest", "adult": "Hyrule Field"}
# and at the stops with passes of their own, which an item's draws (its effect, a projectile, Link holding it) could combine
# with into pipelines met nowhere else. The rest only look around
ITEMS_PASSES = {
    "Zora's Domain": "water: its surface and what is drawn through it, at both ages (the adult's Lake Hylia dives instead)",
    "Water Temple": "water around and above Link, at both ages",
    "Shadow Temple": "the Lens of Truth's pass (the child's visit: the adult's is left out)",
    "Death Mountain Crater": "heat haze (the adult's visit: the child's is left out)",
    "Fire Temple": "heat haze, at both ages",
}


def uses_items(age, name):
    return ITEMS_ONCE.get(age) == name or name in ITEMS_PASSES


def item_stops(a):
    """The stops where Link uses the sword, the shield and an item, in the whole route's order: [(age, index)]. A stop's place
    here is its pause menu pick (pause_pick), the same whichever game of the tour visits it"""
    return [(age, i) for age, i in plan_stops(a, "all") if uses_items(age, a["teleports"][i][0])]


def items_unlock(a):
    """Every item Link can have (kind 0: unlocked once), the quantities filled (kind 1: +60): the Debug tab's actions"""
    out = []
    for name, (i, kind) in a["items"].items():
        if name.startswith("Armor_Child_Mask_") and name != "Armor_Child_Mask_Truth":
            continue   # (one mask slot: the last one added is the one kept)
        if name.startswith(QUEST_ITEMS):
            continue   # (the story's flags: medallions, stones and songs start scenes that wait for the player)
        out.append(a["item"] + i * 256 + (200 if kind == 0 else 64 + 60))
    return out


# ---- the route ----
# Presses: <seconds>:<button>[+<button>][/<ms>] (docs/dev/developing.md, "Scripted input"), in game time: the bridge's
# sequence with "units": "hos" (docs/dev/reference/control-bridge.md).
# The buttons as the remake has them: B the sword (held: the spin attack), R the shield, ZL a target, A a roll, ZR the item on the
# item button; PLUS the pause menu, whose pages ZL and ZR turn (Collection, Equipment, Items, System) and B closes; on its Items
# page the stick moves the cursor and A puts the item on the item button.

def presses(items):
    return ",".join("%g:%s" % (t, b) for t, b in items)


LOOK = presses([(0, "RLEFT/1500"), (1.5, "RUP/400"), (1.9, "RRIGHT/700"), (2.6, "RDOWN/700"), (3.3, "RUP/300"),
                (0, "LUP/600"), (0.6, "LDOWN/600")])
LOOK_SPAN = 3.7
ACT = presses([(0, "B"), (0.35, "B"), (0.7, "B/900"), (1.7, "R/500"), (2.3, "ZL/300"), (3.2, "ZR/600"),
               (4, "ZR"), (4.6, "ZR/600")])   # (no A: next to someone it starts a conversation)
ACT_SPAN = 5.4
# Presses that end what holds a map change back (a conversation, a question, an item's prompt), one at a time in turn
UNSTICK = ["0:A/100", "0:B/100", "0:LDOWN/150,0.3:A/100", "0:B/100", "0:LUP/150,0.3:A/100", "0:B/100,0.4:B/100"]


def pause_pick(k):
    """The pause menu's pages, then on the Items page the k-th item put on the item button (a cursor walk that differs from stop to
    stop), and closed. ZL three times first: from whatever page it opened on to the first, then ZR twice to Items"""
    seq, t = [(0, "PLUS")], 0.8
    for b in ("ZL", "ZL", "ZL", "ZR", "ZR"):
        seq.append((t, b))
        t += 0.45
    walk = ["LDOWN"] * (k % 5) + ["LRIGHT"] * ((k // 5) % 4)
    for b in walk:
        seq.append((t, b + "/120"))
        t += 0.25
    seq += [(t, "A"), (t + 0.4, "B"), (t + 0.9, "B")]
    return presses(seq), t + 1.3


PAUSE_TOUR = presses([(0, "PLUS"), (0.8, "ZR"), (1.5, "ZR"), (2.2, "ZR"), (2.9, "ZR"), (3.6, "ZL"), (4.3, "ZL"), (5, "ZL"),
                      (5.7, "ZL"), (6.4, "B"), (6.9, "B")])
PAUSE_SPAN = 7.4
PAUSE_PAGES = False   # (measured: 0 pipelines the item stops' own pause menu visits and the players' lists lack, for 8 s)

# Water: at these stops Link arrives swimming; A held dives (the scales are given). As adult the pause menu's Equipment page then
# puts on the iron boots and the Zora tunic (its cursor starts on the Echo Gem in a new game: up is the iron boots, right then down
# the tunics' row), the stop is reached again, and Link walks the lake's bottom; the boots come off after
DIVE_AT = {"Lake Hylia"}
DIVE = presses([(0, "A/4000")])
DIVE_SPAN = 4.5
UNDERWATER_GEAR = presses([(0, "PLUS"), (0.8, "ZL"), (1.2, "ZL"), (1.6, "ZL"), (2.0, "ZR"), (2.6, "UP"), (3.0, "A"), (3.5, "RIGHT"),
                           (3.9, "DOWN"), (4.3, "RIGHT"), (4.7, "RIGHT"), (5.1, "A"), (5.6, "B"), (6.1, "B")])
GEAR_SPAN = 6.6
BOOTS_OFF = presses([(0, "PLUS"), (0.8, "ZL"), (1.2, "ZL"), (1.6, "ZL"), (2.0, "ZR"), (2.6, "LEFT"), (3.0, "LEFT"), (3.4, "LEFT"),
                     (3.8, "A"), (4.3, "B"), (4.8, "B")])   # (from the Zora tunic: left three times is the iron boots again)
BOOTS_SPAN = 5.3


# A stop is left once no new pipeline came for SETTLE_QUIET frames (a frame a 60th of a second of game time, HOS_FPS_STEP=0:
# the faster the game, the less wall time), SETTLE_MAX frames at most. The look around and the items come before it, so its
# quiet frames are what a stop waits for nothing
SETTLE_QUIET = 60
SETTLE_MAX = 600

PARTS = ("all", "child", "adult", "opening", "child-overworld", "child-dungeons", "adult-overworld", "adult-dungeons", "dungeons",
         "gameover")


def dungeons_from(a):
    """The first dungeon's index in the teleport list (vk_menu.cpp teleport_sections: "Dungeons")"""
    return a.get("dungeons", len(a["teleports"]))


def plan_stops(a, part="all", stops=None, first=None):
    """The destinations a part visits, in order: [(age, index)]. opening: none (the new game's opening alone, watched whole);
    child-overworld, dungeons (both ages) and adult-overworld split the route in three"""
    adult_only = {i for i, (_, only) in enumerate(a["teleports"]) if only}
    first_dungeon = dungeons_from(a)
    child = [("child", i) for i in range(len(a["teleports"])) if i not in adult_only]
    adult = [("adult", i) for i in range(len(a["teleports"]))]
    over = lambda plan: [(age, i) for age, i in plan if i < first_dungeon]
    under = lambda plan: [(age, i) for age, i in plan if i >= first_dungeon]
    plan = {"all": child + adult, "child": child, "adult": adult, "opening": [], "child-overworld": over(child),
            "dungeons": under(child) + under(adult), "adult-overworld": over(adult), "child-dungeons": under(child),
            "adult-dungeons": under(adult), "gameover": []}[part]
    plan = [(age, i) for age, i in plan if (age, a["teleports"][i][0]) not in LEFT_OUT]
    # (a stop the game will not teleport out of goes last: the next map change after it is the next run's)
    ages = [age for age, _ in plan]
    plan = sorted(plan, key=lambda x: (ages.index(x[0]), a["teleports"][x[1]][0] in LAST))   # (last within its age)
    if first and part not in ("gameover", "opening"):   # (a run that starts again at a stop: "<name>" or "<name> (<age>)")
        labels = ["%s (%s)" % (a["teleports"][i][0], age) for age, i in plan]
        at = [k for k, (lab, (age, i)) in enumerate(zip(labels, plan)) if first in (lab, a["teleports"][i][0])]
        if not at:
            raise SystemExit("pipeline_tour: no stop %r in this part" % first)
        plan = plan[at[0]:]
    return plan[:stops] if stops is not None else plan


def route(a, part="all", stops=None, first=None):
    """The steps of a run: ("newgame",), ("items",), ("press", text, span, label), ("age",), ("visit", age, index, k),
    ("gameover", index). The pause menu's pages once, in the opening part or a whole run"""
    steps = [("newgame",), ("items",)]
    if PAUSE_PAGES and part in ("all", "child", "opening"):
        steps.append(("press", PAUSE_TOUR, PAUSE_SPAN, "pause menu pages"))
    age = "child"
    for k, (want, i) in enumerate(plan_stops(a, part, stops, first)):
        if want != age:
            steps += [("age",), ("items",)]   # (the adult's equipment is only given to the adult: a child's add is lost)
            age = want
        steps.append(("visit", want, i, k))
    if part == "gameover":   # (hearts not protected: the crater's heat takes them, the Game Over screen, Continue)
        steps.append(("gameover", [n for n, _ in a["teleports"]].index(GAME_OVER_AT)))
    return steps


def stop_labels(a, steps):
    """The stops of steps, as --first takes them: "<name> (<age>)" """
    return ["%s (%s)" % (a["teleports"][step[2]][0], step[1]) for step in steps if step[0] == "visit"]
