"""The pipeline notes' layout has a format number of its own (gpu/vk/vk_draw.cpp: kNotesFormat, notes_stamp), raised with any change
to what a note holds as it lies in memory. No game or GPU.

Why: the notes (longshot_pipelines.bin) are kept across a translator change now, stamped by their own layout rather than by the
program cache's stamp, so nothing but kNotesFormat stops a build from reading notes written with another layout. A note is
written whole as it lies: PipeNote (gpu/vk/vk_draw.h) and the NvnState it holds (gpu/vk/vk_state.h), with the NVN structs NvnState
holds (gpu/render/rx_nvn.h, gpu/render/nvn_api.h). Only their size was ever checked, and a change of the same size (two members
swapped, a bit field moved) would hand the driver another state.

How: their definitions are read from the source, comments and spacing taken out, and hashed; the hash recorded for the source's
kNotesFormat must match. A change to any of them fails here until kNotesFormat is raised and the new hash recorded below (the
failure prints it). notes_stamp must mix kNotesFormat and both sizes.

And what a note means: its key mixes in what the translator made (vk_draw.cpp key_inputs), so a note kept across a translator
change may be under a key this build makes otherwise. Every place that builds a pipeline from a note and keeps it under the
note's key must first check the key holds (note_key_holds): otherwise a draw of that key is handed a pipeline made for another
state.

python -I -B tests/test_notes_format.py
"""
import hashlib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# (kNotesFormat -> the fingerprint of the layouts it was raised for)
RECORDED = {
    1: "1fa8151376aa98bd",
}

# (the typedef'd NVN structs a note holds, in NvnState and in the structs it holds)
NVN_TYPES = (("gpu/render/rx_nvn.h", ("nvn_view", "nvn_stage_data", "nvn_prog", "nvn_p_bind_buf", "nvn_blend_st", "nvn_color_st",
                                       "nvn_chmask_st", "nvn_ms_st", "nvn_poly_st", "nvn_ds_st", "nvn_vs_st", "nvn_va_st", "nvn_p_set_rt")),
             ("gpu/render/nvn_api.h", ("NVNbufferRange",)))
# (the plain typedefs those structs hold)
NVN_PLAIN = (("gpu/render/nvn_api.h", "NVNbufferAddress"),)

# (the places that keep a pipeline built from a note under the note's key: each checks the key holds first)
KEY_SITES = (("gpu/vk/vk_draw.cpp", "void precompile_run(void (*progress)(size_t done, size_t total))", "if (p && note_key_holds(*p, n)) {"),
             ("gpu/vk/vk_draw.cpp", "void precompile_at_create(void (*progress)(size_t done, size_t total))", "!note_key_holds(p, *note)) continue;"),
             ("gpu/vk/vk_programs.cpp", None, "if (!note_key_holds(*p, *note)) continue;"))


def normal(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return " ".join(text.split())


def typedef(text, name, where):
    end = text.find("} %s;" % name)
    if end < 0:
        raise SystemExit("FAIL: %s: no typedef'd struct %s (what a note holds moved: follow it here)" % (where, name))
    start = text.rfind("typedef struct", 0, end)
    return text[start:end + len("} %s;" % name)]


def fingerprint():
    parts = []
    head = (ROOT / "gpu/vk/vk_draw.h").read_text(encoding="utf-8")
    parts.append(block(head, "struct PipeNote {"))
    state = (ROOT / "gpu/vk/vk_state.h").read_text(encoding="utf-8")
    stages = re.search(r"constexpr int kNvnStages = \d+;", state)
    if not stages:
        raise SystemExit("FAIL: vk_state.h has no kNvnStages (NvnState's arrays are sized by it)")
    parts.append(stages.group(0))
    parts.append(block(state, "struct NvnState {"))
    for path, names in NVN_TYPES:
        text = (ROOT / path).read_text(encoding="utf-8")
        parts += [typedef(text, name, path) for name in names]
    for path, name in NVN_PLAIN:
        m = re.search(r"typedef [^;{}]* %s;" % name, (ROOT / path).read_text(encoding="utf-8"))
        if not m:
            raise SystemExit("FAIL: %s: no typedef of %s (what a note holds moved: follow it here)" % (path, name))
        parts.append(m.group(0))
    return hashlib.sha256("\n".join(normal(p) for p in parts).encode()).hexdigest()[:16]


def main():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    m = re.search(r"constexpr uint32_t kNotesFormat = (\d+);", draw)
    if not m:
        raise SystemExit("FAIL: vk_draw.cpp has no kNotesFormat: the notes are stamped by the program cache, dropped with it at "
                         "every translator change")
    stamp = block(draw, "uint64_t notes_stamp() {")
    if "kNotesFormat" not in stamp or "sizeof(PipeNote)" not in stamp or "sizeof(NvnState)" not in stamp:
        raise SystemExit("FAIL: notes_stamp does not mix kNotesFormat, sizeof(PipeNote) and sizeof(NvnState)")
    for path, marker, need in KEY_SITES:
        text = (ROOT / path).read_text(encoding="utf-8")
        where = block(text, marker) if marker else text
        if need not in where:
            raise SystemExit("FAIL: %s%s keeps a pipeline built from a note under the note's key without checking the key holds "
                             "(note_key_holds)" % (path, ": " + marker.split("(")[0] if marker else ""))
    fmt = int(m.group(1))
    now = fingerprint()
    want = RECORDED.get(fmt)
    if want != now:
        raise SystemExit("FAIL: what a pipeline note holds changed (PipeNote, NvnState or an NVN struct in it: fingerprint %s), and "
                         "kNotesFormat is %d, recorded for %s. Raise kNotesFormat in gpu/vk/vk_draw.cpp and record %d: \"%s\" in "
                         "tests/test_notes_format.py's RECORDED" % (now, fmt, want, fmt + (want is not None), now))
    print("PASS: the notes' layout is kNotesFormat %d's (%s)" % (fmt, now))


if __name__ == "__main__":
    main()
