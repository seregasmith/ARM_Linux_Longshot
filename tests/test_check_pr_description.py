"""scripts/check_pr_description.py on sample descriptions, game-free. The template as it comes fails on every section;
good descriptions pass: a fix (tests/pr_description/good.md), a perf change with a variant column, a docs change whose
Measurements is one line, docs/dev/standards.md's example measurements, and a description written to the earlier
template. Each rule refuses what it should (a section missing, empty or left as the template; Testing's Tested and
Still to test, each item saying who runs it; Measurements' verdict, columns named by commit, a Baseline: line, a perf
change's table; the Ready for review boxes whole and ticked, the When it applies boxes kept ticked), the warnings
come when they should, a work-in-progress PR passes with a notice, and the names Tooling's forge.py and ci.yml use are
there.

python -I -B tests/test_check_pr_description.py
"""
import contextlib
import importlib.util
import io
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("check_pr_description", ROOT / "scripts" / "check_pr_description.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)
TEMPLATE = (ROOT / ".forgejo" / "pull_request_template.md").read_text(encoding="utf-8")
GOOD = (ROOT / "tests" / "pr_description" / "good.md").read_text(encoding="utf-8")
FIX = "fix(menu): the pause menu's text fits at 4K"

DIFF = """diff --git a/gpu/vk/vk_draw.cpp b/gpu/vk/vk_draw.cpp
--- a/gpu/vk/vk_draw.cpp
+++ b/gpu/vk/vk_draw.cpp
@@ -1,3 +1,4 @@
-    if (env_on("RX_VK_OLD")) moved();
+    if (env_on("RX_VK_OLD")) moved(1);
+    const char* e = getenv("RX_VK_NEW_THING");
diff --git a/docs/dev/reference/switches.md b/docs/dev/reference/switches.md
--- a/docs/dev/reference/switches.md
+++ b/docs/dev/reference/switches.md
@@ -1 +1,2 @@
+`RX_VK_DOCUMENTED` (set) is told of here only.
"""
HEADER = "| | Before: main @ 7afe6ae1 | After: this PR @ 992a2420 | Δ |"
NO_COMMIT = "names no commit: 'Before: main @ <hash>', 'After: this PR @ <hash>'"
BASELINE = ("## Measurements compares something other than main against this PR: add a 'Baseline:' line saying what "
            "and why")

def section(body, name, text, level="##"):
    """body with that heading's text replaced (up to the next heading of that level or higher)"""
    head = "%s %s\n" % (level, name)
    start = body.index(head) + len(head)
    m = re.compile(r"^#{1,%d} " % len(level), re.M).search(body, start)
    return body[:start] + "\n" + text + "\n\n" + body[m.start() if m else len(body):]


def drop(body, name, level="##"):
    """body without that heading and its text"""
    head = "%s %s\n" % (level, name)
    start = body.index(head)
    m = re.compile(r"^#{1,%d} " % len(level), re.M).search(body, start + len(head))
    return body[:start] + body[m.start() if m else len(body):]


def filled():
    """the template filled in the plainest way that passes: a docs change, one line of Measurements"""
    body = TEMPLATE
    for name, text in (("Summary", "The Windows steps for the tests are written down in `docs/dev/setup.md`."),
                       ("Testing", "**Tested:** followed the steps on Linux.\n\n**Still to test:** None"),
                       ("Measurements", "Nothing to measure: docs only."),
                       ("Risks", "None: docs only.")):
        body = section(body, name, text)
    head, tail = body.split("### " + check.APPLIES)
    return check.keep_applicable(head.replace("- [ ]", "- [x]") + "### " + check.APPLIES + tail, [])


def perf():
    """GOOD as a perf change with a variant column"""
    table = ("| | Before: main @ 7afe6ae1 | After: this PR @ 992a2420 | After, RX_VK_MENU_FONT_SCALE=1: this PR @ "
             "992a2420 |\n|---|---|---|---|\n| menu frame time | 0.42 ms | 0.38 ms | 0.41 ms |\n")
    start = GOOD.index(HEADER)
    return GOOD[:start] + table + GOOD[GOOD.index("\n\n", start) + 2:]


def example_measurements():
    """docs/dev/standards.md's filled-in Measurements section"""
    text = (ROOT / "docs" / "dev" / "standards.md").read_text(encoding="utf-8")
    return re.search(r"```\n(\*\*Verdict:\*\*.*?)```", text[text.index("### Measurements"):], re.S).group(1)


class Samples(unittest.TestCase):
    def test_the_bare_template_fails_on_every_section(self):
        found = check.problems(TEMPLATE)
        for name in check.REQUIRED:
            self.assertTrue(any(f.startswith("## %s still holds the template's <" % name) for f in found), name)
        self.assertEqual(sum(f.startswith("not ticked: ") for f in found), 5)
        self.assertEqual(sum(f.startswith(check.APPLIES + ", not ticked: ") for f in found), 9)

    def test_the_fix_sample_passes(self):
        self.assertEqual(check.review(GOOD, diff=DIFF.replace("RX_VK_NEW_THING", "RX_VK_MENU_FONT_SCALE"), title=FIX),
                         ([], []))
        self.assertEqual(check.problems(GOOD.replace("\n", "\r\n"), title=FIX), [])
        self.assertNotIn("AI assistance", GOOD, "When it applies keeps only the boxes that apply")
        self.assertIn("- [ ] Approved in review", GOOD, "Ready to merge is not asked to leave WIP")

    def test_a_perf_change_with_a_variant_passes(self):
        self.assertEqual(check.review(perf(), title="perf(menu): x"), ([], []))
        body = perf().replace("| | Before: main @ 7afe6ae1 |", "| | Before, RX_VK_X=0: this PR @ 992a2420 |")
        self.assertEqual(check.problems(body), [BASELINE])
        self.assertEqual(check.problems(body.replace("Setup:", "Baseline: the switch off: main can't set it.\nSetup:")),
                         [])

    def test_a_docs_change_with_one_line_passes(self):
        self.assertEqual(check.review(filled(), title="docs: x"), ([], []))

    def test_standards_example_passes(self):
        example = example_measurements()
        self.assertIn("Baseline:", example)
        self.assertEqual(check.problems(section(filled(), "Measurements", example)), [])
        self.assertEqual(check.problems(section(filled(), "Measurements", re.sub(r"^Baseline:.*\n", "", example,
                                                                                 flags=re.M))), [BASELINE])


class Template(unittest.TestCase):
    def test_the_sections(self):
        top = check.sections(TEMPLATE)
        self.assertEqual(list(top), list(check.REQUIRED) + ["Checklist"])
        for name in check.REQUIRED:   # (a comment under each says what to write)
            self.assertTrue(top[name].strip().startswith("<!--"), name)
        self.assertEqual(list(check.checklist(TEMPLATE)), [check.EVERYONE, check.APPLIES, check.MERGE])
        self.assertEqual(set(check.testing_parts(check.without_comments(top["Testing"]))), set(check.TESTING_PARTS))
        self.assertIn("| | Before: main @ <commit> | After: this PR @ <commit> |", top["Measurements"],
                      "forge.py pr create fills in these two commits")

    def test_the_boxes(self):
        lists = check.checklist(TEMPLATE)
        self.assertNotIn("Assisted-by", lists[check.EVERYONE], "some accounts add no attribution: it can't be required")
        self.assertIn("Assisted-by", lists[check.APPLIES])
        self.assertIn("ships at the default the measurements chose", lists[check.APPLIES])
        self.assertIn("Tested on my own PC, in the game", lists[check.EVERYONE])
        self.assertEqual(check.problems(filled(), title="docs: x"), [])
        self.assertIn("- [ ] Approved in review", check.checklist(filled())[check.MERGE])


class Sections(unittest.TestCase):
    def test_missing_empty_or_left_as_the_template(self):
        for name in check.REQUIRED:
            with self.subTest(name=name):
                self.assertEqual(check.problems(drop(GOOD, name), title=FIX), ["no ## %s section" % name])
                self.assertEqual(check.problems(section(GOOD, name, "<!-- a note -->"), title=FIX),
                                 ["## %s is empty" % name])
        stand_in = check.STAND_IN.findall(check.without_comments(check.sections(TEMPLATE)["Summary"]))[0]
        self.assertEqual(check.problems(section(GOOD, "Summary", "It is " + stand_in + "."), title=FIX),
                         ["## Summary still holds the template's " + stand_in])
        self.assertIn("no ## Summary section", check.problems(None))


class Testing(unittest.TestCase):
    def still(self, text):
        return GOOD[:GOOD.index("**Still to test:**")] + "**Still to test:**\n" + text + "\n\n" + \
            GOOD[GOOD.index("## Measurements"):]

    def test_the_parts(self):
        self.assertEqual(set(check.testing_parts(check.sections(GOOD)["Testing"])), set(check.TESTING_PARTS))
        self.assertEqual(check.problems(GOOD.replace("**Tested:**", "Ran:"), title=FIX), ["## Testing has no 'Tested:'"])
        self.assertEqual(check.problems(self.still(""), title=FIX),
                         ["## Testing has no 'Still to test:' (write None when Tested covers it)"])
        self.assertEqual(check.review(self.still("None: Linux only."), title=FIX), ([], []))

    def test_each_item_says_who_runs_it(self):
        self.assertEqual(check.problems(self.still("- radv: the walls, expecting no gaps (by hand)"), title=FIX), [])
        self.assertEqual(check.problems(self.still("- Arc A770: the walls, expecting no gaps"), title=FIX), [
            "## Testing: 'Arc A770: the walls, expecting no gaps' says not who runs it: '@Beanstalk <what to test>' or "
            "'by hand'"])
        steps = self.still("- Windows, any GPU:\n  1. open the menu\n  2. press Start,\n  expecting the menu (by hand)")
        self.assertEqual(check.problems(steps, title=FIX), [], "steps under an item are the item's")

    def test_farm_items_and_status(self):
        self.assertEqual([farm for _, farm in check.farm_items(GOOD)], [False, True])
        self.assertTrue(check.farm_items(GOOD)[1][0].startswith("Steam Deck (steamdeck): open the pause menu"))
        self.assertEqual(check.farm_items(TEMPLATE), [], "the template's own stand-in is no item")
        line = check.status(GOOD)
        self.assertEqual(list(line), list(check.STATUS))
        self.assertEqual(line[check.STILL], "2 (1 for the Test Farm)")
        self.assertTrue(line[check.TESTED].startswith("CachyOS, RX 6700 XT"))
        self.assertEqual(check.status(TEMPLATE), {check.TESTED: "missing", check.STILL: "none"})
        self.assertIsNone(check.status("no sections"))


class Measurements(unittest.TestCase):
    def problems(self, old, new, body=GOOD, title=FIX):
        self.assertIn(old, body)
        return check.problems(body.replace(old, new), title=title)

    def test_the_columns_name_their_commits(self):
        self.assertEqual(self.problems(HEADER, "| | Before | After | Δ |"), [
            "## Measurements' column 'Before' " + NO_COMMIT, "## Measurements' column 'After' " + NO_COMMIT])
        self.assertEqual(self.problems(HEADER, "| | Before: main @ latest | After: this PR @ 992a2420 | Δ |"),
                         ["## Measurements' column 'Before: main @ latest' " + NO_COMMIT])
        self.assertEqual(self.problems(HEADER, "| | After: this PR @ 992a2420 | After, X=1: this PR @ 992a2420 | Δ |"),
                         ["## Measurements' table needs a Before and an After column"])
        self.assertEqual(self.problems(HEADER, "| | **Before:** main @ 7afe6ae1 | After: this PR @ 992a2420, "
                                               "RX_VK_X=1 | Δ |"), [], "bold, and a note after the hash")
        self.assertEqual(self.problems(HEADER, "| | Before: `main` @ 7afe6ae1 | After: this PR @ `992a2420` | Δ |"), [],
                         "a build or a hash in backticks")
        self.assertEqual(check.column("Before: main @ `7afe6ae1`"), {"side": "before", "build": "main", "hash": "7afe6ae1"})
        self.assertEqual(check.column("Before: `main` @ 7afe6ae1"), {"side": "before", "build": "main", "hash": "7afe6ae1"})

    def test_a_baseline_other_than_main(self):
        nightly = "| | Before: nightly @ abcd1234 | After: this PR @ 992a2420 | Δ |"
        self.assertEqual(self.problems(HEADER, nightly), [BASELINE])
        said = GOOD.replace("Setup:", "Baseline: nightly-123: main had not built.\nSetup:")
        self.assertEqual(self.problems(HEADER, nightly, said), [])

    def test_the_verdict_and_the_values(self):
        verdict = GOOD[GOOD.index("**Verdict:**"):GOOD.index("Setup:")]
        self.assertEqual(self.problems(verdict, ""), ["## Measurements has no 'Verdict:': did it help, by how much"])
        self.assertEqual(self.problems(verdict, "**Verdict:**\n\n"),
                         ["## Measurements has no 'Verdict:': did it help, by how much"])
        rows = GOOD[GOOD.index("| widest row"):GOOD.index("\n```\n[menu]")]
        self.assertEqual(self.problems(rows, "| widest row | | | |"),
                         ["## Measurements' table has no row with a value in each column"])

    def test_one_line_or_a_table(self):
        self.assertEqual(check.problems(section(GOOD, "Measurements", "Nothing to measure: the log's wording only."),
                                        title=FIX), [])
        self.assertEqual(check.problems(section(GOOD, "Measurements", "Docs only.\nNothing ran."), title=FIX), [
            "## Measurements has no table: | | Before: main @ <hash> | After: this PR @ <hash> |, or one line saying "
            "why there is nothing to measure"])
        self.assertEqual(check.problems(section(GOOD, "Measurements", "Nothing to measure."), title="perf(vk): x"), [
            "## Measurements: a perf change shows its gain: a Verdict and a table of main against this PR"])

    def test_the_warnings(self):
        self.assertEqual(check.review(GOOD, merge_base="7afe6ae1" + "0" * 32, title=FIX)[1], [])
        self.assertEqual(check.review(GOOD, merge_base="0123456789abcdef", title=FIX)[1], [
            "measured on main 7afe6ae1, but the PR is now on main 01234567: measure again, or say why the numbers "
            "still hold"])
        self.assertEqual(check.diff_changes(DIFF)[0], ["RX_VK_NEW_THING"], "one only moved, or in the docs, is not new")
        self.assertEqual(check.review(GOOD, diff=DIFF, title=FIX), ([], [
            "the diff adds RX_VK_NEW_THING: say in Measurements the default it ships at, and why"]))


class Checklist(unittest.TestCase):
    FIRST = "One topic, on current `main`; the title is a Conventional Commit."

    def test_ready_for_review_is_whole_and_ticked(self):
        self.assertEqual(check.problems(GOOD.replace("- [x] " + self.FIRST, "- [ ] " + self.FIRST), title=FIX),
                         ["not ticked: " + self.FIRST])
        self.assertEqual(check.problems(GOOD.replace("- [x] " + self.FIRST + "\n", ""), title=FIX),
                         ["missing from Ready for review: " + self.FIRST])
        self.assertEqual(check.problems(GOOD.replace("### Ready for review", "### Mine"), title=FIX),
                         ["no ### Ready for review under ## Checklist"])
        self.assertEqual(check.problems(GOOD[:GOOD.index("## Checklist")], title=FIX), ["no ## Checklist"])

    def test_when_it_applies_keeps_only_what_applies(self):
        self.assertEqual(check.review(GOOD.replace("- [x] Docs updated.\n", ""), title=FIX), ([], []))
        self.assertEqual(check.problems(GOOD.replace("- [x] Docs updated.", "- [ ] Docs updated."), title=FIX), [
            "When it applies, not ticked: Docs updated. (tick it, or delete it if it doesn't apply)"])

    def test_the_diff_hints_at_boxes(self):
        def hinted(diff):
            return [item.split(" ")[0] + " ... (" + why + ")" for item, why in check.suggested_boxes(diff)]

        def diff(*paths, added=""):
            return "".join("--- a/%s\n+++ b/%s\n@@ -1 +1,2 @@\n+%s\n" % (p, p, added) for p in paths)
        self.assertEqual(hinted(diff("gpu/vk/vk_api.h")), [
            "A ... (tests read gpu/vk/vk_api.h)",
            "`MKWVK_API_VERSION` ... (it changes gpu/vk/vk_api.h)",
            "Host ... (it changes host code (gpu/vk/vk_api.h))"])
        self.assertIn("The ... (it changes the shader gpu/vk/shaders/fsr.comp)", hinted(diff("gpu/vk/shaders/fsr.comp")))
        self.assertIn("A ... (it adds RX_VK_NEW)", hinted(diff("scripts/x.py", added='os.environ.get("RX_VK_NEW")')))
        self.assertEqual(hinted(diff("docs/dev/setup.md")), ["Docs ... (it changes docs/dev/setup.md)"])
        self.assertEqual(hinted(None), [])
        bare = GOOD.replace("- [x] Docs updated.\n", "")
        self.assertEqual(check.review(bare, diff=diff("docs/dev/setup.md"), title=FIX), ([], [
            "When it applies: the diff suggests 'Docs updated.' (it changes docs/dev/setup.md): add it if it applies"]))

    def test_keep_applicable(self):
        keep = [item for item, _ in check.suggested_boxes("+++ b/docs/dev/setup.md\n+x\n")]
        lists = check.checklist(check.keep_applicable(TEMPLATE, keep))
        self.assertEqual(check.boxes(lists[check.APPLIES]), [(False, "Docs updated.")])
        self.assertEqual(lists[check.EVERYONE], check.checklist(TEMPLATE)[check.EVERYONE])
        self.assertEqual(lists[check.MERGE], check.checklist(TEMPLATE)[check.MERGE])
        none = check.checklist(check.keep_applicable(TEMPLATE, []))[check.APPLIES]
        self.assertEqual(check.boxes(none), [])
        self.assertIn("Add the boxes that apply", none)
        self.assertEqual(check.without_reproduce(TEMPLATE), TEMPLATE)


class Running(unittest.TestCase):
    def test_wip_titles(self):
        for title in ("WIP: fix(menu): text cut off", "[WIP] fix", "wip: fix", "  WIP:fix"):
            self.assertTrue(check.is_draft(title), title)
        self.assertTrue(check.is_draft("fix", api_draft=True))
        for title in ("fix(menu): WIP: no", "WIPE: no", "", None):
            self.assertFalse(check.is_draft(title), title)
        self.assertEqual(check.strip_wip("[WIP] WIP:  fix: x "), "fix: x")
        self.assertEqual(check.strip_wip("fix: WIP: x"), "fix: WIP: x")

    def run_main(self, body, *argv):
        with tempfile.TemporaryDirectory() as folder:
            tmp = Path(folder) / "body.md"
            tmp.write_text(body, encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = check.main(["--file", str(tmp)] + list(argv))
        return code, out.getvalue()

    def test_main(self):
        code, out = self.run_main(GOOD, "--title", FIX)
        self.assertEqual((code, out), (0, "The description is complete.\n"))
        code, out = self.run_main(drop(GOOD, "Risks"), "--title", FIX)
        self.assertEqual(code, 1)
        self.assertIn("::error::no ## Risks section", out)
        for argv in (["--title", "WIP: " + FIX], ["--title", "[WIP] " + FIX], ["--wip"]):
            with self.subTest(argv=argv):
                code, out = self.run_main(drop(GOOD, "Risks"), *argv)
                self.assertEqual(code, 0)
                self.assertIn("::notice::Work in progress", out)
                self.assertIn("no ## Risks section", out)
                self.assertNotIn("::error::", out)

    def test_the_workflow(self):
        wf = (ROOT / ".forgejo" / "workflows" / "pr-description.yml").read_text(encoding="utf-8")
        on = wf.split("\non:")[1].split("\npermissions:")[0]
        self.assertIn("pull_request_target:", on)
        self.assertIn("types: [opened, edited, synchronize, reopened]", on)   # (edited: the title's WIP: taken off)
        self.assertRegex(wf, r"uses: https://github.com/actions/checkout@[0-9a-f]{40} ")
        self.assertIn("python3 -I scripts/check_pr_description.py --pr", wf)
        self.assertNotIn("${{ github.event.pull_request.title", wf)   # (nothing the PR's author writes reaches the shell)


class ForTooling(unittest.TestCase):
    """The names Tooling's forge.py and .forgejo/workflows/ci.yml read from this file"""

    def test_the_names(self):
        for name in ("APPLIES", "boxes", "checklist", "EVERYONE", "farm_items", "is_draft", "keep_applicable",
                     "NEEDS_MEASUREMENTS", "problems", "review", "sections", "STAND_IN", "STATUS", "status",
                     "strip_wip", "suggested_boxes", "TEMPLATE", "TESTING_PARTS", "testing_parts", "without_comments",
                     "without_reproduce", "GAME_PATHS", "TITLE", "TYPES", "WIP_PREFIXES"):
            self.assertTrue(hasattr(check, name), name)
        self.assertTrue(check.TEMPLATE.is_file())

    def test_title(self):
        for good in ("fix(vk): walls", "feat(builder)!: x", "docs: y", "ci(forge.py): z"):
            self.assertTrue(check.TITLE.match(good), good)
        for bad in ("Fix walls", "fix walls", "fix:walls", "wip: x", "WIP: fix: x"):
            self.assertFalse(check.TITLE.match(bad), bad)
        ci = (ROOT / ".forgejo" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn('spec_from_file_location("check", "scripts/check_pr_description.py")', ci)   # (the title job)

    def test_game_paths(self):
        for path in ("gpu/vk/vk_draw.cpp", "host/hos.c", "nx2/lift.py"):
            self.assertTrue(path.startswith(check.GAME_PATHS), path)
        for path in ("docs/dev/reference/switches.md", "scripts/release.py", "tests/run_all.py"):
            self.assertFalse(path.startswith(check.GAME_PATHS), path)


if __name__ == "__main__":
    unittest.main()
