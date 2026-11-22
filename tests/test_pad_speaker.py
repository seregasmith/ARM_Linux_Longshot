"""The controller speaker's output (host/pad_speaker.c): the DualSense's audio endpoint, its ring and its output report.

Compiles host/pad_speaker.c as it is around a made-up pad (tests/pad_speaker_test.c): no SDL, no controller, no sound
device and no game file. Checked: it stays shut without a wired DualSense or when switched off; the report that sends
the pad's sound to its speaker and the one that gives the jack back; the mixer's mono samples on the endpoint's first
two channels and silence on the voice coils' two; the 40 ms start, the under-run and the trim; the test tone; a pad
that changes, dies or leaves; the game closing; the menu's Controller speaker page (its rows' values, defaults and
keys against the host's classes and switches).

python -I -B tests/test_pad_speaker.py [--cc gcc]
"""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = []
ROOT = Path(__file__).resolve().parents[1]
SWITCHES = [name for name in os.environ if name.startswith("HOS_PAD_SPEAKER")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pad-speaker-") as tmp:
        exe = Path(tmp) / "fixture.exe"
        cmd = [args.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
               "-I", str(ROOT / "host"), str(ROOT / "tests/pad_speaker_test.c"), "-o", str(exe), "-lm"]
        if os.name != "nt":
            cmd += ["-I", str(ROOT / "host/posix"), "-pthread"]
        subprocess.run(cmd, check=True, timeout=120)
        base = {k: v for k, v in os.environ.items() if k not in SWITCHES}
        for mode, env in (("off", {"HOS_PAD_SPEAKER": "0"}),
                          ("modes", {"HOS_PAD_SPEAKER_UI": "0", "HOS_PAD_SPEAKER_OCARINA": "1", "HOS_PAD_SPEAKER_AGONY": "7"}),
                          ("menu", {}), ("ride", {}), ("ride_other", {}), ("wrap", {}), ("reject", {}), ("play", {"HOS_PAD_SPEAKER_VOLUME": "50"}),
                          ("legacy", {"HOS_PAD_SPEAKER_HPF": "0", "HOS_PAD_SPEAKER_LIMITER": "0", "HOS_PAD_SPEAKER_LOW_LATENCY": "0",
                                      "HOS_PAD_SPEAKER_VOLUME": "50"})):
            subprocess.run([str(exe), mode], check=True, timeout=60, env={**base, **env})
    # the mixer hands it every frame, and both ways out of the game give the jack back
    audio = (ROOT / "host/nn_audio.c").read_text(encoding="utf-8")
    loop = audio[audio.index("static DWORD WINAPI renderer_main"):]
    assert "if (speaker_on) {" in loop and "hos_pad_speaker_write(speaker_frame, FRAME);" in loop
    for rel, before in (("host/hos_relaunch.c", 'hos_log("stop: close work done'), ("host/hos_main.c", "hos_crash_clean_end();\n    return 0;")):
        text = (ROOT / rel).read_text(encoding="utf-8")
        assert text.index("hos_pad_speaker_shutdown();") < text.index(before), rel
    assert "hos_pad_speaker_pad(dualsense," in (ROOT / "host/pad_sdl.c").read_text(encoding="utf-8")
    # the menu's page (gpu/vk/menu/controller_speaker.cpp) and the host agree: a row a class in the classes' order, the same
    # defaults, each row's key its switch's name
    speaker = (ROOT / "host/pad_speaker.c").read_text(encoding="utf-8")
    module = (ROOT / "gpu/vk/menu/controller_speaker.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    switches = re.findall(r'"HOS_(PAD_SPEAKER_[A-Z_]+)"', speaker[speaker.index("class_env["):speaker.index("};", speaker.index("class_env["))])
    assert len(switches) == 12
    keys = re.findall(r'\.key = "(pad_speaker_[a-z_]+)"', module)
    assert keys == ["pad_speaker_volume", "pad_speaker_sync", "pad_speaker_headphones", "pad_speaker_headphone_volume"] + \
        [name.lower() for name in switches], keys
    kinds = re.findall(r'\.value = &opts\.pad_speaker\[(\d+)\],', module)
    assert [int(n) for n in kinds] == list(range(12)), kinds   # (each kind's row on its class, in the classes' order)
    assert re.search(r'\.key = "pad_speaker_sync",.*?\.lo = -100, \.hi = 150, \.step = 10,', module, re.S)
    assert '.opens = "controller_speaker"' in module and 'menu::page("controller_speaker"' in module
    names = {"ONLY": "1", "BOTH": "2", "OFF": "0"}
    table = speaker[speaker.index("class_default["):]
    host_defaults = [names[n] for n in re.findall(r"HOS_PAD_SPEAKER_(ONLY|BOTH|OFF)", table[:table.index("};")])]
    menu_defaults = re.search(r"int pad_speaker\[12\] = \{([^}]*)\};", header).group(1).replace(" ", "").split(",")
    assert host_defaults == menu_defaults, (host_defaults, menu_defaults)
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    assert "return opts.pad_speaker[id - HOS_OPT_PAD_SPEAKER_CLASS];" in replay and \
        "return opts.pad_speaker[9 + id - HOS_OPT_PAD_SPEAKER_CLASS2];" in replay and "return opts.pad_speaker_volume;" in replay
    for row in ("SYNC) return opts.pad_speaker_sync;", "HEADPHONES) return opts.pad_speaker_headphones;",
                "HEADPHONE_VOLUME) return opts.pad_speaker_headphone_volume;"):
        assert "if (id == HOS_OPT_PAD_SPEAKER_" + row in replay, row
    # the vibration's stream carries the speaker's channels (host/rumble_renhdrum.c), both ways out of its pull
    vib = (ROOT / "host/rumble_renhdrum.c").read_text(encoding="utf-8")
    pull = vib[vib.index("static void usb_pull("):vib.index("int rh_usb_find(void *pad)")]
    assert pull.count("hos_pad_speaker_fill(f, frames);") == 2, "usb_pull does not hand the speaker its channels"
    # one speaker block for HD Haptics and the game's sounds: opened and given back in usb_speaker_sync alone
    assert vib.count("rh.hd_haptics_speaker_block(") == 1, "the cable's speaker is opened in more than one place"
    sync = vib[vib.index("static int usb_speaker_sync("):vib.index("static void usb_hd_set(int on) {")]
    assert "rh.hd_haptics_speaker_block(" in sync
    print("PASS: the controller speaker's endpoint, ring, report and shutdown")


if __name__ == "__main__":
    main()
