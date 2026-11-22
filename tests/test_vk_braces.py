"""The renderer's sources keep their braces balanced (gpu/vk): no `}` closes more than was opened, and each file ends with
every brace closed.

A brace too many between two functions closes the namespace early: what follows falls outside `namespace mkwvk`, where
vk_internal.h doesn't declare it, and the file's own closing `} // namespace mkwvk` is one too many. The build then stops
at that file (#1105 did that to vk_replay.cpp after present_thread_drain()). The checks that cut single functions out of a
source never see a brace between functions, so this reads each file whole: comments, string and character literals (raw
strings too) and preprocessor lines left out, and only the first branch of each #if / #ifdef counted, as the build of
one platform sees one branch. Game-free, no compiler.

    python -I -B tests/test_vk_braces.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IDENT = re.compile(r"[A-Za-z0-9_]")


def code_lines(text):
    """The file's lines with preprocessor lines, and the lines of #else / #elif branches, blanked"""
    out, depth_skip, stack, cont = [], 0, [], False
    for line in text.split("\n"):
        s = line.strip()
        if cont:   # (a preprocessor line continued with a backslash)
            cont = s.endswith("\\")
            out.append("")
            continue
        if s.startswith("#"):
            cont = s.endswith("\\")
            word = re.match(r"#\s*(\w*)", s).group(1)
            if word in ("if", "ifdef", "ifndef"):
                stack.append(False)   # (in its first branch)
            elif word in ("elif", "else") and stack:
                stack[-1] = True
            elif word == "endif" and stack:
                stack.pop()
            out.append("")
            continue
        out.append("" if any(stack) else line)
    return out


def separator(src, i):
    """Whether the ' at i is a digit separator (1'000, 0x10'0000): inside a number, not a character literal (L'\\\\')"""
    j = i
    while j > 0 and (IDENT.match(src[j - 1]) or src[j - 1] == "'"):
        j -= 1
    return j < i and src[j].isdigit()


def braces(text):
    """(line, +1 or -1) for each brace in the code, comments and literals left out"""
    lines = code_lines(text)
    src = "\n".join(lines)
    i, n, line, found = 0, len(src), 1, []
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
        elif src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
        elif src.startswith("/*", i):
            end = src.find("*/", i + 2)
            end = n if end < 0 else end + 2
            line += src.count("\n", i, end)
            i = end
        elif c == '"' and i > 0 and src[i - 1] == "R" and (i < 2 or not IDENT.match(src[i - 2]) or src[i - 2] in "uUL8"):
            m = re.compile(r'"([^()\\\s]{0,16})\(').match(src, i)
            close = ")" + (m.group(1) if m else "") + '"'
            end = src.find(close, i + 1)
            end = n if end < 0 else end + len(close)
            line += src.count("\n", i, end)
            i = end
        elif c == '"' or (c == "'" and not separator(src, i)):
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            i = j + 1
        elif c in "{}":
            found.append((line, 1 if c == "{" else -1))
            i += 1
        else:
            i += 1
    return found


def check(path):
    """None when the braces balance, else what is wrong"""
    depth = 0
    for line, step in braces(path.read_text(encoding="utf-8", errors="replace")):
        depth += step
        if depth < 0:
            return "line %d: a '}' with nothing open (one before it closed too much)" % line
    if depth:
        return "%d brace(s) left open at the end" % depth
    return None


def main():
    files = sorted(p for p in (ROOT / "gpu" / "vk").rglob("*") if p.suffix in (".cpp", ".h", ".hpp"))
    if not files:
        print("FAIL: no sources under gpu/vk")
        return 1
    bad = [(p, why) for p in files if (why := check(p))]
    for p, why in bad:
        print("FAIL: %s: %s" % (p.relative_to(ROOT).as_posix(), why))
    if bad:
        return 1
    print("OK: the braces of %d renderer sources balance" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
