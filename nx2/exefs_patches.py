"""(hookshot) The mods' ExeFS patches, applied to the game's code when it is built: IPS, IPS32 and IPSwitch (.pchtxt) files
for the game's main.

Longshot compiles the game's code ahead of time, so a patch to its instructions cannot be applied when the game runs: here
the patch files of the mods that are on are read when the game is built (nx2/make_game.py), and the lift (nx2/image.py)
writes them into main's image before it relocates and translates it, as emulators and Atmosphere apply a patch file to the
module before its dynamic linker runs. The code is then translated from the patched instructions, and image.bin holds the
patched data. A new patch file, a changed one or a mod switched on or off applies at the next build; the game says so in
the Insert menu's Mods tab (host/mods.c reads exefs_patches.txt, which this writes into the game folder).

The mods are those host/mods.c finds: <mods>/<name>/exefs/*.ips and *.pchtxt, <mods> the game folder's "mods" folder (or
HOS_MODS), a mod any folder up to four deep holding romfs, exefs or cheats, folders starting with '-' or '_' left out, the
order and switches of mods.json (the Mods tab's: list_file), the rest after the named ones by name and on. Where two files
write the same byte the later one in the list wins. A file is for the game when its build id is main's: an IPS file's name,
a pchtxt file's @nsobid line (trailing zeros may be left out). Offsets are as emulators read them: IPS offsets count the
NSO header (0x100 bytes) before main's image; a pchtxt offset adds its `@flag offset_shift` first. A pchtxt patch line
applies between @enabled and @disabled; @stop ends the file.

A file is unsupported as a whole when a write falls outside main's image or into the dynamic linker's tables or a
function pointer it binds (the lift relocates main itself, from the tables as the game ships them). Bytes into a word the
dynamic linker fills with an explicit addend change nothing, as on the console (the relocation writes over them).
A write into the padding after the code (a code cave) makes that padding code the lift translates.

  python -I nx2/exefs_patches.py --exefs <dir> --game <game folder> [--user <folder>] [--out <json>] [--list <txt>]

The parse of the three formats follows valentderah/totk-pc-recomp tools/exefs_patches.py (MIT):
Copyright (c) 2026 Valiantsin Dzerakh
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
is furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""
import argparse
import json
import os
import re
import struct
import sys

NSO_HEADER = 0x100
ENV = "OOT_EXEFS_PATCHES"   # (the lift's: the JSON this writes, nx2/image.py applies it)


def fnv32(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


# ---- the mods, as host/mods.c finds and orders them

def mods_root(game):
    return os.environ.get("HOS_MODS") or os.path.join(game, "mods")


def find_mods(root):
    """[(name, path)] of the mod folders under root: a folder holding romfs, exefs or cheats, up to four deep; '-' and '_'
    left out (host/mods.c find_mods)"""
    out = []

    def walk(d, name, depth):
        if depth > 0 and any(os.path.isdir(os.path.join(d, p)) for p in ("romfs", "exefs", "cheats")):
            out.append((name, d))
            return
        if depth >= 4:
            return
        try:
            entries = sorted(os.listdir(d))
        except OSError:
            return
        for e in entries:
            if e.startswith((".", "-", "_")) or not os.path.isdir(os.path.join(d, e)):
                continue
            walk(os.path.join(d, e), name + "/" + e if name else e, depth + 1)

    if os.path.isdir(root):
        walk(root, "", 0)
    return out


# ---- mods.json: the mods' order and switches, read as host/mods.c reads it (host/mods_list.c has the file's rules)

LIST_NAME = "mods.json"
OLD_LIST_NAME = "longshot_mods.txt"   # (the list of builds before mods.json, in the game folder: read while there is no mods.json)
LIST_NOTE = os.path.join("modcache", "mods_json.txt")   # (host/mods.c note_state_file: where the game last read its list)
LIST_MAX = 1 << 20    # (host/mods_list.h MODS_LIST_MAX: a file of this many bytes or more is not a list)
LIST_DEPTH = 64       # (host/mods_list.h MODS_LIST_DEPTH)


def _absolute(path):
    """A whole path as host/options_path.c tells one: on Windows it starts with a separator or a drive, elsewhere with /"""
    return (path[:1] in ("/", "\\") or path[1:2] == ":") if os.name == "nt" else path[:1] == "/"


def list_file(game, user=None, environ=None, log=None):
    """Where mods.json is, by the game's rule (host/mods.c state_file: beside the player's settings file, which
    host/options_path.c chooses): the folder of $HOS_OPTIONS_FILE; else the user folder the game is started with (--user: the
    launcher's, a relative one whole against the game folder); else, the build not being told one, where the game itself
    last read the list (its note in the game folder, which the game writes when that is not the game folder); else the game
    folder. A note is followed only when it holds a whole path to a file that is there: what it says, and why it is not
    followed, goes to log"""
    environ = os.environ if environ is None else environ
    own = environ.get("HOS_OPTIONS_FILE")
    if own:
        return os.path.join(os.path.dirname(os.path.abspath(own)), LIST_NAME)
    if user:
        return os.path.join(os.path.abspath(os.path.join(game, user)), LIST_NAME)
    beside, note = os.path.join(game, LIST_NAME), os.path.join(game, LIST_NOTE)
    log = log or (lambda line: None)
    try:
        with open(note, "rb") as f:
            raw = f.read(1 << 16)
    except OSError:
        return beside
    try:
        noted = raw.decode("utf-8").splitlines()[0].strip()
    except (ValueError, IndexError):
        noted = ""
    if not _absolute(noted):
        log("exefs: warning: %s does not hold a whole path (%r): not followed, the mods' list is the game folder's" % (note, noted[:200]))
        return beside
    if not os.path.exists(noted):
        log("exefs: warning: %s names %s, which is not there: not followed, the mods' list is the game folder's (start the game "
            "once, or tell the build the user folder: --user)" % (note, noted))
        return beside
    log("exefs: the mods' list is where the game last read it (%s names it; --user <folder> tells the build instead)" % note)
    return noted


class _Object(list):
    """A JSON object as its (key, value) pairs in the file's order: a key that is there twice is seen"""


def _no_constant(name):
    raise ValueError("%s is not JSON" % name)


def _strict(value, depth=1):
    """What Python's json takes and host/mods_list.c does not: a NUL or half a surrogate pair in a string (from an escape),
    arrays and objects more than LIST_DEPTH deep inside each other"""
    if isinstance(value, str):
        if any(c == "\0" or "\ud800" <= c <= "\udfff" for c in value):
            raise ValueError("a NUL or half a surrogate pair in a string")
    elif isinstance(value, list):
        if depth > LIST_DEPTH:
            raise ValueError("arrays and objects too deep inside each other")
        for item in value:
            for part in (item if isinstance(value, _Object) else (item,)):
                _strict(part, depth + 1)


def parse_list(raw):
    """[(id, on)] of mods.json's bytes, in the list's order; ValueError saying why when it is not a list (host/mods_list.c
    mods_list_parse: one JSON object in UTF-8, "mods" an array of objects with one "id", a string, and at most one "enabled",
    true or false; "cheats" an object; each of the two at most once)"""
    if len(raw) >= LIST_MAX:
        raise ValueError("it is too large")
    try:
        top = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_Object, parse_constant=_no_constant,
                         parse_int=lambda digits: 0, parse_float=lambda digits: 0.0)
    except RecursionError:
        raise ValueError("arrays and objects too deep inside each other") from None
    _strict(top)
    if not isinstance(top, _Object):
        raise ValueError("it is not a JSON object")
    keys = [k for k, _ in top]
    if keys.count("mods") > 1 or keys.count("cheats") > 1:
        raise ValueError('"mods" or "cheats" twice')
    members = dict(top)
    if "cheats" in members and not isinstance(members["cheats"], _Object):
        raise ValueError('"cheats" is not an object')
    mods = members.get("mods", [])
    if type(mods) is not list:
        raise ValueError('"mods" is not an array')
    out = []
    for entry in mods:
        if not isinstance(entry, _Object):
            raise ValueError('an entry of "mods" is not an object')
        keys = [k for k, _ in entry]
        fields = dict(entry)
        if keys.count("id") != 1 or type(fields["id"]) is not str:
            raise ValueError('an entry of "mods" without one "id" that is a string')
        on = fields.get("enabled", True)
        if keys.count("enabled") > 1 or (on is not True and on is not False):
            raise ValueError('a mod\'s "enabled" is not true or false, once')
        out.append((fields["id"], on))
    return out


def old_list(text):
    """[(id, on)] of longshot_mods.txt's lines, "on <name>" and "off <name>" (host/mods.c read_old_state: lines end at
    carriage returns and line feeds, nothing else)"""
    out = []
    for line in re.split("[\r\n]+", text):
        for word, on in (("on ", True), ("off ", False)):
            if line.startswith(word):
                out.append(("local/" + line[len(word):], on))
    return out


def name_key(name):
    """A mod's name as host/mods.c sorts the mods no list names (mod_cmp, _stricmp: by bytes, A to Z as a to z and no other
    letter changed)"""
    try:
        raw = name.encode("utf-8", "surrogateescape")
    except UnicodeEncodeError:
        raw = name.encode("utf-8", "surrogatepass")
    return bytes(c + 32 if 65 <= c <= 90 else c for c in raw)


def read_list(path):
    """[(id, on)] of the mods.json at path; OSError or ValueError when it cannot be read or is not a list"""
    with open(path, "rb") as f:
        return parse_list(f.read(LIST_MAX))   # (no more is read: a file of that size is refused)


def ordered(mods, state_path, old_path=None, log=None, beside=None):
    """[(name, path, on)] in the list's order: those mods.json (state_path) names, in its order and with its switches, then
    the rest by name, on (host/mods.c read_state). A mod's id there is "local/" and its name, byte for byte. A file that is
    not a list names nothing, as in the game. With no mods.json there: the game folder's (beside), which the game copies
    into the user folder at its next start (host/mods.c carry_state), when that is a list; else the list of a build before
    mods.json (old_path, longshot_mods.txt). Which was read goes to log. Nothing is written or renamed"""
    log = log or (lambda line: None)
    mods = sorted(mods, key=lambda m: name_key(m[0]))
    named, source = [], None
    if os.path.exists(state_path):
        source = state_path
        try:
            named = read_list(state_path)
        except (OSError, ValueError) as e:
            log("exefs: %s is not a list of mods (%s): every mod is on, in the order of the folders' names, as in the game"
                % (state_path, e))
    else:
        if beside and os.path.exists(beside):
            try:
                named, source = read_list(beside), beside
            except (OSError, ValueError) as e:
                log("exefs: %s is not a list of mods (%s): not read, as the game does not copy it to %s" % (beside, e, state_path))
        if source is None and old_path and os.path.exists(old_path):
            try:
                with open(old_path, encoding="utf-8", errors="replace") as f:
                    named, source = old_list(f.read()), old_path
            except OSError:
                pass
    log("exefs: the mods' order and switches: %s" % (source or "no list (every mod on, by name)"))
    out, placed = [], set()
    by_id = {"local/" + name: (name, path) for name, path in mods}
    for ident, on in named:
        if ident in by_id and ident not in placed:   # (an id twice: its first entry counts)
            out.append(by_id[ident] + (on,))
            placed.add(ident)
    out += [(n, p, True) for n, p in mods if "local/" + n not in placed]
    return out


def patch_files(mod_path):
    """The .ips and .pchtxt files of a mod's exefs folder, by name"""
    d = os.path.join(mod_path, "exefs")
    try:
        names = sorted(os.listdir(d))
    except OSError:
        return []
    return [n for n in names if n.lower().endswith((".ips", ".pchtxt")) and os.path.isfile(os.path.join(d, n))]


# ---- the formats: {offset in main's image: byte}

def same_build(name, build_id):
    """True when name (hex, any case, trailing zeros left out or not) is main's build id (hex)"""
    name = name.strip().lower()
    return bool(re.fullmatch(r"[0-9a-f]{1,64}", name)) and name.ljust(64, "0") == build_id.lower().ljust(64, "0")


def parse_ips(data):
    """IPS (PATCH ... EOF, 3-byte offsets) or IPS32 (IPS32 ... EEOF, 4-byte offsets); ValueError when it is neither"""
    if data[:5] == b"PATCH":
        width, end, at = 3, b"EOF", 5
    elif data[:5] == b"IPS32":
        width, end, at = 4, b"EEOF", 5
    else:
        raise ValueError("not an IPS or IPS32 file")
    writes = {}
    while True:
        if data[at:at + len(end)] == end and at + len(end) >= len(data) - 3:
            return writes
        if at + width + 2 > len(data):
            raise ValueError("the IPS file is cut short")
        off = int.from_bytes(data[at:at + width], "big")
        size = int.from_bytes(data[at + width:at + width + 2], "big")
        at += width + 2
        if size:
            body = data[at:at + size]
            at += size
        else:   # (a run: 2-byte length, 1 byte)
            run = int.from_bytes(data[at:at + 2], "big")
            body = data[at + 2:at + 3] * run
            at += 3
        if len(body) != (size or len(body)):
            raise ValueError("the IPS file is cut short")
        for i, b in enumerate(body):
            writes[off - NSO_HEADER + i] = b


def pchtxt_value(token):
    """The bytes of a pchtxt value: hex digits in byte order, or a quoted string with C escapes"""
    if token.startswith('"'):
        if not token.endswith('"') or len(token) < 2:
            raise ValueError("a string without its closing quote: %s" % token)
        return token[1:-1].encode("utf-8").decode("unicode_escape").encode("latin-1")
    if len(token) % 2 or not re.fullmatch(r"[0-9A-Fa-f]+", token):
        raise ValueError("not a hex value: %s" % token)
    return bytes.fromhex(token)


def parse_pchtxt(text):
    """(build id or None, {offset in main's image: byte}) of an IPSwitch file"""
    bid, shift, enabled, writes = None, 0, False, {}
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        if line.startswith("@stop"):
            break
        if line.startswith("@nsobid-"):
            bid = line[len("@nsobid-"):].strip()
        elif line.startswith("@enabled"):
            enabled = True
        elif line.startswith("@disabled"):
            enabled = False
        elif line.startswith("@flag"):
            f = line.split()
            if len(f) >= 3 and f[1] == "offset_shift":
                shift = int(f[2], 0)
        elif line.startswith("@") or not enabled:
            continue
        else:
            m = re.fullmatch(r"([0-9A-Fa-f]+)\s+(\".*\"|\S+)", line)
            if not m:
                continue   # (a title in brackets or a line of text)
            off = int(m.group(1), 16) + shift - NSO_HEADER
            for i, b in enumerate(pchtxt_value(m.group(2))):
                writes[off + i] = b
    return bid, writes


# ---- what a write meets in main

class Main:
    """main's image as loaded (before relocation), its code, the dynamic linker's tables and the words it binds"""

    def __init__(self, nso_main):
        n = nso_main
        self.image = n.image
        self.build_id = n.build_id
        text_off, text_size, _ = n.segments[0]
        self.code_end = n.segments[1][0]   # (the code with its padding up to .rodata)
        self.text_end = text_off + text_size
        self.size = len(n.image)
        d = lambda k: n.dynamic.get(k, [0])[0]
        tables = []
        dyn = n.mod0 + struct.unpack_from("<i", n.image, n.mod0 + 4)[0]
        end = dyn
        while struct.unpack_from("<q", n.image, end)[0] != 0:
            end += 16
        tables.append((dyn, end + 16))
        for start, size in (("RELA", "RELASZ"), ("JMPREL", "PLTRELSZ"), ("RELR", "RELRSZ")):
            if d(start) and d(size):
                tables.append((d(start), d(start) + d(size)))
        if d("SYMTAB") and d("STRTAB") > d("SYMTAB"):
            tables.append((d("SYMTAB"), d("STRTAB")))
        if d("STRTAB") and d("STRSZ"):
            tables.append((d("STRTAB"), d("STRTAB") + d("STRSZ")))
        self.tables = tables
        self.bound = {r["offset"]: r["type"] for r in n.relocs}

    def classify(self, writes):
        """(kind, note, {offset: byte}): code / data (what stays: bytes that differ from main's), empty, or unsupported"""
        keep = {}
        relocated = 0
        for off in sorted(writes):
            if not 0 <= off < self.size:
                return "unsupported", "it writes outside the game's code module at main+%X" % off, {}
            if any(a <= off < b for a, b in self.tables):
                return "unsupported", "it writes the dynamic linker's tables at main+%X" % off, {}
            word = next((w for w in range(off - 7, off + 1) if w in self.bound), None)
            if word is not None and self.bound[word] == "JUMP_SLOT":
                return "unsupported", "it writes a function pointer the dynamic linker binds at main+%X" % word, {}
            if word is not None:
                # (a word the dynamic linker relocates: left out, and the file's note says so. With an explicit addend the
                # linker writes over the word, as on the console. A DT_RELR word is different there: its addend is the word
                # itself, so the console would take the patched bytes; here nso2.py has read the addend from the file
                # before the patch, and the lift relocates from that)
                relocated += self.image[off] != writes[off]
                continue
            if self.image[off] != writes[off]:
                keep[off] = writes[off]
        note = "%d byte%s in words the dynamic linker relocates left out" % (relocated, "" if relocated == 1 else "s") if relocated else ""
        if not keep:
            return "empty", note, {}
        return ("code" if any(o < self.code_end for o in keep) else "data"), note, keep


def classify_file(main, path, raw):
    """(kind, note, writes) of one patch file: other (another version), empty, unsupported, code or data"""
    try:
        if path.lower().endswith(".ips"):
            if not same_build(os.path.splitext(os.path.basename(path))[0], main.build_id):
                return "other", "", {}
            writes = parse_ips(raw)
        else:
            bid, writes = parse_pchtxt(raw.decode("utf-8-sig", errors="replace"))
            if bid is None or not same_build(bid, main.build_id):
                return "other", "", {}
        return main.classify(writes)
    except ValueError as e:
        return "unsupported", str(e), {}


def collect(exefs, game, log=print, user=None):
    """The patch files of the mods, each classified: [{mod, file, hash, kind, note, on, writes}] in the list's order (user: the
    user folder the game is started with, when the build is told: list_file)"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import kit  # noqa: F401  (a64-kit on the path)
    import nso2  # noqa: F401  (DT_RELR)
    import nso
    root = mods_root(game)
    said = []   # (which list, and why: said when a mod has patch files)
    listed, old, beside = list_file(game, user, None, said.append), os.path.join(game, OLD_LIST_NAME), os.path.join(game, LIST_NAME)
    if os.environ.get("HOS_OPTIONS_FILE"):   # (a settings file of the run's own: the game folder's lists are the player's, not read)
        old = beside = None
    elif os.path.normcase(os.path.abspath(beside)) == os.path.normcase(os.path.abspath(listed)):
        beside = None   # (the list is the game folder's)
    mods = ordered(find_mods(root), listed, old, said.append, beside)
    files = [(name, path, on, f) for name, path, on in mods for f in patch_files(path)]
    for line in said if files else ():
        log(line)
    if not files:
        return []
    main = Main(nso.Nso(os.path.join(exefs, "main")))
    out = []
    for name, path, on, f in files:
        raw = open(os.path.join(path, "exefs", f), "rb").read()
        kind, note, writes = classify_file(main, f, raw) if on else ("off", "", {})
        out.append({"mod": name, "file": f, "hash": "%08x" % fnv32(raw), "kind": kind, "note": note, "on": on,
                    "writes": [[o, writes[o]] for o in sorted(writes)]})
        log("exefs: %-11s %s/exefs/%s%s" % (kind, name, f, (" (%s)" % note) if note else
                                            (" (%d bytes)" % len(writes)) if writes else ""))
    return out


def write_list(files, path):
    """exefs_patches.txt in the game folder: what the build applied, a line a file (host/mods.c exefs_status)"""
    lines = ["# The mods' ExeFS patch files this game was built with (nx2/exefs_patches.py): mod, file, FNV-1a of its bytes, kind, note"]
    lines += ["%s\t%s\t%s\t%s\t%s" % (f["mod"], f["file"], f["hash"], f["kind"], f["note"].replace("\t", " "))
              for f in files if f["on"]]
    with open(path, "w", encoding="utf-8", newline="\n") as o:
        o.write("\n".join(lines) + "\n")


def apply(modules, path=None, log=print):
    """nx2/image.py, before main is relocated: the writes of the code and data files in the JSON at path (or $OOT_EXEFS_PATCHES)
    into main's image, the later file winning; the padding after main's code a cave was written into becomes code"""
    path = path or os.environ.get(ENV)
    if not path:
        return 0
    files = json.load(open(path, encoding="utf-8"))
    main = next((m for m in modules if m.name == "main"), None)
    if main is None:
        return 0
    n = 0
    cave_end = 0
    text_off, text_size, text_name = main.segments[0]
    rodata = main.segments[1][0]
    for f in files:
        if f["kind"] not in ("code", "data"):
            continue
        for off, b in f["writes"]:
            if off >= len(main.image):
                raise SystemExit("exefs: %s/%s writes past main's image" % (f["mod"], f["file"]))
            main.image[off] = b
            n += 1
            if text_off + text_size <= off < rodata:
                cave_end = max(cave_end, (off + 4) & ~3)
    if cave_end:
        main.segments[0] = (text_off, cave_end - text_off, text_name)
        log("exefs: main's code reaches %#x now (a code cave)" % cave_end)
    log("exefs: %d bytes of main patched by %d files" % (n, sum(f["kind"] in ("code", "data") for f in files)))
    return n


def main_cli():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--exefs", required=True)
    p.add_argument("--game", required=True, help="the game folder (its mods folder; mods.json when the game is started by itself)")
    p.add_argument("--user", help="the user folder the game is started with (the launcher's --user): where mods.json is")
    p.add_argument("--out", help="the JSON the lift applies")
    p.add_argument("--list", help="exefs_patches.txt")
    a = p.parse_args()
    files = collect(a.exefs, a.game, user=a.user)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as o:
            json.dump(files, o)
    if a.list:
        write_list(files, a.list)


if __name__ == "__main__":
    main_cli()
