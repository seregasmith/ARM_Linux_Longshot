# RenHDrum

RenHDrum is the HD Rumble and DualSense haptics core the game's fine haptics come from: it turns the console's own
vibration values (a low and a high band, an amplitude and a frequency each, per side) into the four-channel 48 kHz audio
a DualSense's voice coils play on a USB cable. Copyright (c) 2026 Three-Dola, MIT OR Apache-2.0 (`LICENSE-MIT`,
`LICENSE-APACHE`, `NOTICE`).

These are the files of <https://github.com/trzz1214/RenHDrum> at the **v0.5.0** tag (commit
`e778da6e169640e225a7f898b77cbb40af97c60f`) that the library is built from, unmodified: `Cargo.toml`, `Cargo.lock` (no
dependency the library links; one build dependency, the `cc` crate, with the two it needs), `build.rs`, `src/`,
`include/renhdrum.h` (the C ABI `host/rumble_renhdrum.c` binds at run time; `RENHDRUM_VERSION 0x000500`, and any 0.5.x
keeps it), the licences and the notice. Its tests, examples, documentation and integrations are left out, and so are the
repository's GPL-3.0-or-later parts (`integrations/brontide`, `integrations/recomp-c/reference`).

`vendor/opus/` (libopus 1.5.2, BSD 3-clause, which `build.rs` compiles into the library for the DualSense's Bluetooth
speaker) is left out too: it is the same release as `third_party/opus`, file for file but for its Windows line endings
and three headers no file it builds includes (`celt/fixed_c5x.h`, `celt/fixed_c6x.h`, `src/tansig_table.h`), and
`scripts/renhdrum.py` puts `third_party/opus` where `build.rs` reads it (without `celt/opus_custom_demo.c`, a program of
its own that upstream's copy does not have). Its licence is `third_party/opus/COPYING`.

upstream: https://github.com/trzz1214/RenHDrum/archive/refs/tags/v0.5.0.tar.gz
upstream-sha256: 4e922ffb6ac15cba85dd6bf29277def7daaa28459c6e2690e04df58c49594cf7
upstream-root: RenHDrum-0.5.0

**How it reaches the game.** `python -I scripts/renhdrum.py build` compiles it into `renhdrum.dll` (ignored by git) for
x86_64-pc-windows-gnullvm, with a pinned Rust and the llvm-mingw the game is built with (its clang compiles libopus too),
and the `cc` crate downloaded by the SHA-256 `Cargo.lock` gives it; the same source gives the same bytes. The Builder's release does that in CI (`.forgejo/workflows/build-windows.yml`) and ships the DLL in its package as
`tools/third_party/renhdrum/renhdrum.dll`, and the AppImage's (`.forgejo/workflows/build-linux.yml`) builds `renhdrum.so`
and carries it in `third_party/renhdrum/`, so a player's build puts it beside the game (`nx2/build.py`,
`nx2/make_game.py`) and needs no Rust. `host/rumble_renhdrum.c` loads it (`renhdrum.dll`, `renhdrum.so` on Linux) from the game's own folder by full path; without it
every controller keeps its motors or its own HD rumble, and Test vibration says why.

**A newer RenHDrum.** Replace these files with the new tag's, change the commit and the three `upstream` lines, run
`python -I scripts/check_third_party.py --update renhdrum` and `--upstream renhdrum`, and check `host/rumble_renhdrum.c`'s
`RH_ABI` against the new `include/renhdrum.h`. If its `vendor/opus` is a new libopus, `third_party/opus` moves to it
first (the game builds it too).

tree-sha256: 7dcd5dc79a79c6a05bc59fb7d637f5359f407e956ad01686220b117b30f44870
