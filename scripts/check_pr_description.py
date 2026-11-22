"""Check a pull request's description against .forgejo/pull_request_template.md (docs/dev/standards.md, "Pull
requests").

It fails when:

  - a section (Summary, Testing, Measurements, Risks, Checklist) is missing, empty, or still holds a <...> stand-in;
  - Testing has no Tested, or no Still to test ("None", or items that each say who runs them: "@Beanstalk ..." for
    the Test Farm, or "by hand");
  - Measurements is neither one line saying why there is nothing to measure (not allowed for a perf change) nor a
    Verdict line and a table whose columns name their commits ("Before: main @ <hash>", "After: this PR @ <hash>"; a
    "Baseline:" line when the before side isn't main), with a row of values;
  - a Ready for review box is missing or not ticked, or a When it applies box kept is not ticked.

It warns (without failing) about a switch the diff adds that the description doesn't name, measurements taken on an
older main, and When it applies boxes the diff suggests. A work-in-progress PR (WIP: or [WIP] in the title, or a
draft) passes with a notice listing what is left. Tooling's forge.py imports this file: pr create fills in the
template, pr ready and pr merge run review().

    python -I scripts/check_pr_description.py --file body.md [--title T] [--diff pr.diff] [--wip]
    python -I scripts/check_pr_description.py --pr 12 [--repo MagicBeanDev/Longshot] [--server https://git.magicbean.dev]

--pr reads the PR, its title and its diff from the Forgejo API (token: FORGEJO_TOKEN or GITHUB_TOKEN). Exit status 0
when it passes, 1 when it doesn't. Standard library only; .forgejo/workflows/pr-description.yml runs it on every PR.
"""
import argparse
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / ".forgejo" / "pull_request_template.md"
# Forgejo's default WORK_IN_PROGRESS_PREFIXES, matched at the start of the title whatever the case; forge.py adds the
# first
WIP_PREFIXES = ("WIP:", "[WIP]")
NEEDS_MEASUREMENTS = "needs measurements"   # (the label of a PR sent back to WIP for its measurements)
# A PR is squash-merged, so its title is the commit on main: a Conventional Commit (ci.yml's title job checks it)
TYPES = ("feat", "fix", "perf", "refactor", "docs", "test", "ci", "build", "chore", "revert")
TITLE = re.compile(r"^(%s)(\([\w./-]+\))?!?: \S" % "|".join(TYPES))
# The files the game runs, by path prefix: forge.py pr create wants --tested-in-game for a branch that changes one
GAME_PATHS = ("gpu/", "host/", "nx2/", "assets/", "third_party/")

REQUIRED = ("Summary", "Testing", "Measurements", "Risks")
TESTED, STILL = "Tested", "Still to test"
TESTING_PARTS = (TESTED, STILL)
STATUS = TESTING_PARTS   # (status(): what pr status shows on its first line)
# The checklist: every PR ticks EVERYONE to leave WIP, APPLIES keeps the boxes that apply, MERGE is the merge gate
EVERYONE, APPLIES, MERGE = "Ready for review", "When it applies", "Ready to merge"
# The earlier template's names, which open pull requests still use: read as the new ones
STAND_IN = re.compile(r"<[^<>\n!]+>")   # (the template's <...> stand-ins)
FARM = re.compile(r"@Beanstalk\b|\btest farm\b", re.I)
WHO = re.compile(FARM.pattern + r"|\bby hand\b", re.I)
NONE = re.compile(r"^\W*none\b", re.I)
COMMIT = re.compile(r"^[0-9a-f]{7,40}$")
# A measurement column: "Before: main @ 7afe6ae1", "After, RX_VK_X=0: this PR @ 992a2420"
COLUMN = re.compile(r"^(?P<side>before|after)\b[^:@]*:\s*(?P<build>[^@]+?)\s*@\s*(?P<hash>[^\s,;()]+)", re.I)
DELTA = re.compile(r"^(Δ|delta|diff|change)\b", re.I)
# A switch the game, the build or a script reads, quoted in the code (each has its row in gpu/vk/switches.def)
SWITCH = re.compile(r"[\"'`]((?:RX|HOS|LONGSHOT)_[A-Z0-9_]+)[\"'`]")


# --- reading Markdown -------------------------------------------------------------------------------------------

def without_comments(text):
    return re.sub(r"<!--.*?-->", "", text, flags=re.S)


def without_code(text):
    return re.sub(r"^```.*?^```", "", text, flags=re.S | re.M)


def plain(text):
    """text without Markdown's bold and italic marks and code spans' backticks"""
    return re.sub(r"\*{1,2}|__|`", "", text)


def sections(body, level="##"):
    """{heading: text} for the headings of that level (the text up to the next heading of that level or higher)"""
    out, name = {}, None
    stop = re.compile(r"^#{1,%d} " % len(level))
    for line in body.splitlines():
        m = re.match(r"^%s (.+?)\s*#*\s*$" % level, line)
        if m:
            name = m.group(1).strip()
            out[name] = ""
        elif name is not None and stop.match(line):
            name = None
        elif name is not None:
            out[name] += line + "\n"
    return out


def aliased(found, name):
    """found[name], or None (kept for Tooling's forge.py, which calls it)"""
    return found.get(name)


def boxes(text):
    """[(ticked, the item's text)] for the task list items in text"""
    return [(m.group(1).lower() == "x", m.group(2).strip()) for m in re.finditer(r"^\s*[-*] \[([ xX])\] (.+)$", text, re.M)]


def key(item):
    """an item's first words, lower case and without marks: the same item when ticked, reflowed or added to"""
    return " ".join(re.sub(r"[^a-z0-9 ]+", " ", item.lower()).split()[:4])


def checklist(body):
    """{### heading: text} under ## Checklist"""
    return sections(sections(body).get("Checklist", ""), "###")


def parts(text, names):
    """{part: its text} for the "**Name:** ..." parts of a section"""
    pattern = re.compile(r"^[*_]*(%s)[*_]*\s*:[*_]*\s*(.*)" % "|".join(map(re.escape, names)), re.I)
    out, name = {}, None
    for line in text.splitlines():
        m = pattern.match(line.strip())
        if m:
            name = next(n for n in names if n.lower() == m.group(1).lower())
            out[name] = m.group(2) + "\n"
        elif name:
            out[name] += line + "\n"
    return out


def testing_parts(text):
    return parts(text, TESTING_PARTS)


def items(text):
    """A list's items: each top-level bullet with the lines under it, or each line when there are no bullets"""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not any(re.match(r"^ ?[-*] ", ln) for ln in lines):
        return [ln.strip() for ln in lines]
    out = []
    for ln in lines:
        if re.match(r"^ ?[-*] ", ln) or not out:
            out.append(re.sub(r"^\s*[-*] ", "", ln).strip())
        else:
            out[-1] += " " + ln.strip()
    return out


def tables(text):
    """[(header cells, [row cells])] for the Markdown tables in text, outside code blocks"""
    def cells(line):
        return [c.strip() for c in line.strip().strip("|").split("|")]
    lines = [ln.strip() for ln in without_code(text).splitlines()]
    out = []
    for i, ln in enumerate(lines[:-1]):
        if ln.startswith("|") and re.match(r"^\|[\s:|-]+$", lines[i + 1]) and "-" in lines[i + 1]:
            rows = []
            for row in lines[i + 2:]:
                if not row.startswith("|"):
                    break
                rows.append(cells(row))
            out.append((cells(ln), rows))
    return out


def line_value(text, name):
    """the text after "Name:" on its own line (bold or not), None when there is no such line"""
    m = re.search(r"^[^\w\n]*%s[ \t]*[*_]*:[*_]*[ \t]*(.*)$" % re.escape(name), text, re.M | re.I)
    return m.group(1).strip() if m else None


def template_text(template=None):
    return template if template is not None else TEMPLATE.read_text(encoding="utf-8")


# --- titles and work in progress --------------------------------------------------------------------------------

def wip_prefix(title):
    """The WIP prefix title starts with, as written there, or None"""
    t = (title or "").lstrip()
    return next((t[:len(p)] for p in WIP_PREFIXES if t[:len(p)].upper() == p), None)


def strip_wip(title):
    """title without its WIP prefixes (the prefix, then the spaces after it, as Forgejo takes it off)"""
    title = (title or "").strip()
    while wip_prefix(title):
        title = title[len(wip_prefix(title)):].strip()
    return title


def is_draft(title, api_draft=False):
    """Work in progress: a WIP prefix on the title, or the API says it is a draft"""
    return bool(api_draft) or wip_prefix(title) is not None


def title_type(title):
    m = re.match(r"^([a-z]+)(\([^)]*\))?!?:", strip_wip(title))
    return m.group(1) if m else None


# --- the sections -----------------------------------------------------------------------------------------------

def testing_problems(text):
    found, out = testing_parts(text), []
    for name in TESTING_PARTS:
        if not found.get(name, "").strip():
            out.append("## Testing has no '%s:'%s" % (name, " (write None when Tested covers it)"
                                                       if name == STILL else ""))
    still = found.get(STILL, "").strip()
    if still and not NONE.match(still):
        out += ["## Testing: '%s' says not who runs it: '@Beanstalk <what to test>' or 'by hand'" % item
                for item in items(still) if not WHO.search(item)]
    return out


def farm_items(body):
    """The Still to test items, as (item, for the Test Farm)"""
    body = without_comments((body or "").replace("\r\n", "\n"))
    still = testing_parts(sections(body).get("Testing", "")).get(STILL, "").strip()
    if not still or NONE.match(still):
        return []
    return [(item, bool(FARM.search(item))) for item in items(still) if not STAND_IN.search(item)]


def status(body):
    """{Tested, Still to test: a short summary} from ## Testing, None when there is no Testing section"""
    top = sections(without_comments(body or ""))
    if "Testing" not in top:
        return None
    found = testing_parts(top["Testing"])
    tested = " ".join(STAND_IN.sub("", found.get(TESTED, "")).split())
    todo = farm_items(body)
    return {TESTED: (tested[:57] + "...") if len(tested) > 60 else (tested or "missing"),
            STILL: "none" if not todo else "%d (%d for the Test Farm)" % (len(todo), sum(f for _, f in todo))}


def column(cell):
    """{side, build, hash} of a measurement column, or None when it names no commit"""
    m = COLUMN.match(plain(cell).strip())
    return m and {"side": m.group("side").lower(), "build": m.group("build").strip().lower(), "hash": m.group("hash")}


def measurement_problems(text, kind):
    found = tables(text)
    if not found:
        if kind == "perf":
            return ["## Measurements: a perf change shows its gain: a Verdict and a table of main against this PR"]
        if len([ln for ln in without_code(text).splitlines() if ln.strip()]) == 1:
            return []
        return ["## Measurements has no table: | | Before: main @ <hash> | After: this PR @ <hash> |, or one line "
                "saying why there is nothing to measure"]
    out = [] if line_value(text, "Verdict") else ["## Measurements has no 'Verdict:': did it help, by how much"]
    header, rows = found[0]
    values = [i for i, c in enumerate(header) if i and c and not DELTA.match(plain(c))]
    cols = []
    for i in values:
        c = column(header[i])
        if c is None or not COMMIT.match(c["hash"]):
            out.append("## Measurements' column '%s' names no commit: 'Before: main @ <hash>', 'After: this PR @ "
                       "<hash>'" % header[i])
        else:
            cols.append(c)
    if not out and {c["side"] for c in cols} != {"before", "after"}:
        out.append("## Measurements' table needs a Before and an After column")
    if any(c["build"] != {"before": "main", "after": "this pr"}[c["side"]] for c in cols) \
            and not line_value(text, "Baseline"):
        out.append("## Measurements compares something other than main against this PR: add a 'Baseline:' line "
                   "saying what and why")
    if not any(r and r[0] and all(i < len(r) and r[i] for i in values) for r in rows):
        out.append("## Measurements' table has no row with a value in each column")
    return out


def checklist_problems(body, template):
    lists = checklist(body)
    ready = aliased(lists, EVERYONE)
    if "Checklist" not in sections(body):
        return ["no ## Checklist"]
    if ready is None:
        return ["no ### %s under ## Checklist" % EVERYONE]
    out = ["not ticked: " + item for ticked, item in boxes(ready) if not ticked]
    if EVERYONE in lists:
        have = {key(item) for _, item in boxes(ready)}
        out += ["missing from %s: %s" % (EVERYONE, item)
                for _, item in boxes(checklist(without_comments(template)).get(EVERYONE, "")) if key(item) not in have]
    out += ["%s, not ticked: %s (tick it, or delete it if it doesn't apply)" % (APPLIES, item)
            for ticked, item in boxes(lists.get(APPLIES, "")) if not ticked]
    return out


# --- the boxes a diff suggests ----------------------------------------------------------------------------------

HOST_CODE = re.compile(r"^(?!tests/|third_party/).*\.(c|cc|cpp|h|hpp)$")
SHADER = re.compile(r"\.(comp|glsl|vert|frag|mesh|task|geom|tesc|tese)$")
DOCS = re.compile(r"^(docs/.*|README\.md|CONTRIBUTING\.md|AGENTS\.md)$")
# (the When it applies box, by its first words as key() reads them; why; what in the diff suggests it)
HINTS = (
    ("a switch added or", "it adds %s", lambda files, switches, read: ", ".join(switches)),
    ("a check under tests", "tests read %s", lambda files, switches, read: ", ".join(sorted(read))),
    ("mkwvk api version bumped", "it changes gpu/vk/vk_api.h", lambda files, switches, read: "gpu/vk/vk_api.h" in files),
    ("the spir v headers", "it changes the shader %s",
     lambda files, switches, read: ", ".join(sorted(f for f in files if SHADER.search(f)))),
    ("host code builds on", "it changes host code (%s)",
     lambda files, switches, read: ", ".join(sorted(f for f in files if HOST_CODE.match(f))[:3])),
    ("docs updated", "it changes %s", lambda files, switches, read: ", ".join(sorted(f for f in files if DOCS.match(f))[:3])),
)


def diff_changes(diff):
    """(the switch names the diff adds to code, the files it changes); a name only moved is not added"""
    added, removed, files, path = set(), set(), set(), None
    for line in (diff or "").splitlines():
        if line.startswith("+++ "):
            path = line[4:].strip()
            path = path[2:] if path.startswith("b/") else path
            files.add(path)
        elif line.startswith("--- "):
            continue
        elif path and not path.startswith(("docs/", "tests/")) and not path.endswith(".md"):
            if line.startswith("+"):
                added.update(SWITCH.findall(line))
            elif line.startswith("-"):
                removed.update(SWITCH.findall(line))
    return sorted(added - removed), files


def read_by_tests(files):
    """The changed source files a check under tests/ names (the tests cut functions out of the source)"""
    try:
        texts = [p.read_text(encoding="utf-8", errors="replace") for p in (ROOT / "tests").glob("test_*.py")]
    except OSError:
        return set()
    def named(f, t):
        name = f.rsplit("/", 1)[-1]
        return f in t or '"%s"' % name in t or "'%s'" % name in t
    return {f for f in files if not f.startswith(("tests/", "docs/")) and not f.endswith(".md")
            and any(named(f, t) for t in texts)}


def suggested_boxes(diff, template=None):
    """[(the When it applies item, why)] for the boxes the diff suggests"""
    switches, files = diff_changes(diff)
    if not files:
        return []
    listed = [item for _, item in boxes(checklist(without_comments(template_text(template))).get(APPLIES, ""))]
    read = read_by_tests(files)
    out = []
    for start, why, test in HINTS:
        found = test(files, switches, read)
        item = next((i for i in listed if key(i).startswith(start)), None)
        if found and item:
            out.append((item, why % found if "%s" in why else why))
    return out


def keep_applicable(body, keep):
    """body with its When it applies list down to the items in keep (none: a comment in its place)"""
    m = re.search(r"^### %s\n(.*?)(?=^#{1,3} |\Z)" % APPLIES, body, re.S | re.M)
    if not m:
        return body
    wanted = {key(i) for i in keep}
    lines = [ln for ln in m.group(1).splitlines() if not re.match(r"^\s*[-*] \[[ xX]\] ", ln)
             or key(re.sub(r"^\s*[-*] \[[ xX]\] ", "", ln)) in wanted]
    text = "\n".join(lines).strip("\n")
    if not wanted:
        text = "<!-- The diff suggested none. Add the boxes that apply from .forgejo/pull_request_template.md. -->"
    return body[:m.start(1)] + "\n" + text + "\n\n" + body[m.end(1):].lstrip("\n")


def without_reproduce(body):
    """body as pr create starts it for a type other than fix: the template has no fix-only section, so unchanged"""
    return body


# --- the whole description --------------------------------------------------------------------------------------

def review(body, diff=None, merge_base=None, template=None, title=None):
    """(problems, warnings): it passes when there are no problems. diff (the PR's unified diff) adds the switch and box
    hints, merge_base (the commit the PR is on) the rebase warning, title (the PR's) the perf rule."""
    template = template_text(template)
    stand_ins = set(STAND_IN.findall(without_comments(template)))
    body = without_comments((body or "").replace("\r\n", "\n"))
    top = sections(body)
    switches, _ = diff_changes(diff)
    out, warn = [], []
    for name in REQUIRED:
        text = aliased(top, name)
        left = sorted(s for s in stand_ins if s in (text or ""))
        if text is None:
            out.append("no ## %s section" % name)
        elif left:
            out.append("## %s still holds the template's %s" % (name, ", ".join(left[:3])))
        elif not text.strip():
            out.append("## %s is empty" % name)
        elif name == "Testing":
            out += testing_problems(text)
        elif name == "Measurements":   # (its text before any ### part, which the earlier template had)
            out += measurement_problems(re.split(r"^### ", text, maxsplit=1, flags=re.M)[0], title_type(title))
    out += checklist_problems(body, template)

    warn += ["the diff adds %s: say in Measurements the default it ships at, and why" % s
             for s in switches if s not in body]
    have = {key(item) for _, item in boxes(checklist(body).get(APPLIES, ""))}
    warn += ["%s: the diff suggests '%s' (%s): add it if it applies" % (APPLIES, item, why)
             for item, why in suggested_boxes(diff, template) if key(item) not in have]
    found = tables(top.get("Measurements", ""))
    bases = [c["hash"] for c in map(column, found[0][0][1:] if found else [])
             if c and c["side"] == "before" and c["build"] == "main"]
    if bases and merge_base and not any(merge_base.lower().startswith(h.lower()) for h in bases):
        warn.append("measured on main %s, but the PR is now on main %s: measure again, or say why the numbers still "
                    "hold" % (bases[0], merge_base[:8]))
    return out, warn


def problems(body, template=None, diff=None, title=None):
    """What keeps body from passing: an empty list when it passes"""
    return review(body, diff=diff, template=template, title=title)[0]


def fetch(server, repo, number, suffix=""):
    req = urllib.request.Request("%s/api/v1/repos/%s/pulls/%d%s" % (server.rstrip("/"), repo, number, suffix),
                                 headers={"Accept": "application/json", "User-Agent": "longshot-pr-check/1"})
    token = os.environ.get("FORGEJO_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", "token " + token)
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
    return data.decode("utf-8", "replace") if suffix else json.loads(data)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--file", help="the description, from a file")
    src.add_argument("--pr", type=int, help="the pull request's number: its description, title and diff, from the API")
    p.add_argument("--title", help="with --file: the PR's title")
    p.add_argument("--diff", help="with --file: the PR's unified diff")
    p.add_argument("--wip", action="store_true", help="with --file: treat it as work in progress (a notice only)")
    p.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY") or "MagicBeanDev/Longshot")
    p.add_argument("--server", default=os.environ.get("GITHUB_SERVER_URL") or "https://git.magicbean.dev")
    a = p.parse_args(argv)
    merge_base, diff, title = None, None, a.title
    if a.file:
        body, wip = Path(a.file).read_text(encoding="utf-8"), is_draft(title, a.wip)
        diff = Path(a.diff).read_text(encoding="utf-8", errors="replace") if a.diff else None
    else:
        pr = fetch(a.server, a.repo, a.pr)
        body, title, merge_base = pr.get("body") or "", pr.get("title"), pr.get("merge_base")
        wip = is_draft(title, pr.get("draft"))
        try:
            diff = fetch(a.server, a.repo, a.pr, ".diff")
        except OSError as e:
            print("::warning::could not read the diff (%s): no switch or box hints" % e)
    found, warnings = review(body, diff=diff, merge_base=merge_base, title=title)
    if wip:
        print("::notice::Work in progress: the description is checked once it is marked ready%s"
              % (" (%d to fix by then)" % len(found) if found else ""))
        for f in found + warnings:
            print("  " + f)
        return 0
    for w in warnings:
        print("::warning::" + w)
    if not found:
        print("The description is complete.")
        return 0
    print("The description is not complete (.forgejo/pull_request_template.md):")
    for f in found:
        print("::error::" + f)
    return 1


if __name__ == "__main__":
    sys.exit(main())
