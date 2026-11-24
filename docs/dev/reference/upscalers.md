# Upscalers and frame generation

The game draws its 3D scene smaller than the output and upscales it with its own network. Longshot can replace that one
dispatch with a temporal upscaler (DLSS, FSR 3.1, FSR 4, XeSS), with a plain stretch (Off), or leave the network to run.
Separately, frame generation makes frames between the game's at the present. This page covers what each part is given and
the rules it keeps. Player settings are in [Settings](../../user/graphics-and-controls.md). How frame generation's sets
are paced and presented is in [architecture](architecture.md#frame-generations-presents).

## The engines

The Upscaler row's values are in [`upscaler_rows.h`](../../../gpu/vk/upscaler_rows.h). Saved values never change meaning,
so new engines are added at the end.

| Value | Upscaler | Code | Needs |
|---|---|---|---|
| 0 | The game's own network | — | Nothing. The TAAU stand-in replaces it where the network would run on software multiplies (`RX_VK_TAAU_KEYS`) |
| 1, 2 | NVIDIA DLSS, DLAA | [`vk_dlss.cpp`](../../../gpu/vk/vk_dlss.cpp) | A build with NVIDIA's NGX SDK (`LONGSHOT_NGX`), an RTX GPU, NGX's runtime |
| 3 | AMD FSR 3.1 | [`vk_fsr3.cpp`](../../../gpu/vk/vk_fsr3.cpp) | Nothing extra. AMD's shaders are compiled into every build (`scripts/shaders.py fsr3`) |
| 4, 5 | AMD FSR 4, its native AA | [`vk_fsr4.cpp`](../../../gpu/vk/vk_fsr4.cpp) | A build with `--fsr` (`LONGSHOT_FSR`) and AMD's FSR 4 library on the player's PC |
| 6 | Off | `temporal_off` in [`vk_temporal.cpp`](../../../gpu/vk/vk_temporal.cpp) | Nothing. The network is skipped and its input is stretched with a linear filter |
| 7, 8 | Intel XeSS, its Native AA | [`vk_xess.cpp`](../../../gpu/vk/vk_xess.cpp) | Windows, and Intel's `libxess.dll` beside the game |

No vendor library ships with Longshot. Each engine reports why it cannot run (`dlss_why`, `fsr4_why`, `xess_why`), and
the menu offers only the engines that can.

AMD FSR 4's layers are unrolled and built into a pipeline cache of their own, `cache/longshot_fsr4_pipelines.bin`
(`own_cache` in `vk_fsr4.cpp`), apart from the translator's, whose files a new translator drops: an update keeps it.
`RX_VK_PCACHE_LOAD=0` neither reads nor saves it.

DLSS, FSR 4, XeSS and FSR 3.1 are engines of one **temporal chain** (`vk_temporal.h`). The chain finds the scene, makes
the inputs, keeps the history and writes the result into the network's output. An engine only turns the inputs into a
picture (`TemporalEngine`: `start`, `prepare`, `evaluate`, `release`, `memory`, `hdr`). With `RX_VK_FSR3_CHAIN=0`, FSR 3.1
instead runs on its own inputs (`vk_fsr3_inputs.cpp`), which carry the camera's motion only.

## Vendor files

`make_game.py` fetches a vendor's files only when asked and only with its license accepted: a fetch flag without the
matching `--accept-*-license` stops the build. `scripts/fetch.py` does the download, at one pinned commit, and checks
every file against its SHA-256 ([setup](../setup.md#upscaler-sdks-optional)). Each kept folder is reused by the next
build, which downloads only what is missing or changed. How players turn them on is in
[advanced setup](../../user/advanced.md#optional-upscalers).

| Engine | `make_game.py` | Kept in | The game looks in, in order |
|---|---|---|---|
| DLSS | `--fetch-ngx-sdk --accept-nvidia-license`, or `LONGSHOT_NGX_SDK=<sdk folder>` for an SDK you fetched. Both runtimes (`nvngx_dlss`, `nvngx_dlssg`) go into the game folder's `ngx` | `LONGSHOT_NGX_SDK`, else `work/ngx-sdk` | `RX_VK_DLSS_DIR`, the game folder's `ngx`, then `%LOCALAPPDATA%\Longshot\ngx` (Windows) or `$XDG_DATA_HOME/longshot/ngx`, else `~/.local/share/longshot/ngx` (Linux). NGX takes the newest runtime it finds across them |
| FSR 4 | `--fsr --fetch-fsr-runtime --accept-amd-license` (Windows): FSR SDK v2.4.0 (FSR 4.1.2), `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll`, with `LICENSE-AMD-FSR-SDK.md` | `LONGSHOT_FFX_SDK`, else `work/ffx-sdk` | `RX_VK_FSR_DIR`, the executable's folder (`bin`), then the game folder |
| XeSS | `--fetch-xess-runtime --accept-intel-license` (Windows): `libxess.dll` 2.0.2.68 (78 MB), with Intel's license and notices | `LONGSHOT_XESS_SDK`, else `work/xess-sdk` | `RX_VK_XESS_DIR`, the executable's folder, then the game folder |

- A game folder that already holds other versions of AMD's or Intel's files (a newer SDK put there by hand) keeps
  them, and the build says so.
- `--fsr` is kept by a rebuild with `--update`; `--no-fsr` takes it out. XeSS is in every Windows build and runs once
  `libxess.dll` is there.
- On Linux both fetch flags only say there is nothing to fetch: AMD publishes no Linux FSR 4 runtime (a Vulkan
  `amd_fidelityfx_upscaler_vk.so` put beside the game stands in), and there is no XeSS for Linux.
- `RX_VK_FSR_DIR` and `RX_VK_XESS_DIR` are paths, which `longshot_env.txt` leaves out: set them in the environment or a
  `HOS_ENV_FILE` file. `RX_VK_DLSS_DIR` is the one path `longshot_env.txt` takes ([switches](switches.md#longshot_envtxt)).
- Never put NGX's core, `_nvngx.dll`, beside the game: it comes with NVIDIA's driver. Without `nvngx_dlssg.dll` in a
  folder above, frame generation takes the driver's own copy, which may be older (one that makes 1 frame at most, or
  refuses every set).

## What an engine gets

`temporal_upscale` takes the gameplay upscale's dispatch (by program key, or by a network of the same shape whose input
is this frame's scene). It checks the scene's camera, depth, formats and size, then builds one `TemporalFrame`:

| Input | Format and meaning |
|---|---|
| `color` | RGBA16F at the render size. Tone-mapped and square-root encoded (LDR), or linear light past 1 when the engine's `hdr()` says it takes native HDR |
| `depth` | R32F, 0..1. Reversed (`1 - d`) for an engine with `fsr_depth` (FSR 3.1) when the projection puts the near plane at 0; `depth_reversed` says so. Anything else reading this depth must check that flag |
| `motion` | RG16F in render pixels, towards the previous frame, jitter removed. Camera reprojection, with skinned draws drawn again with their previous bones (actor motion) |
| `output` | RGBA16F at the output size (at the render size for DLAA and the native AA modes, scaled afterwards) |
| `jitter`, `jitter_phases` | This frame's sub-pixel offset in render pixels, and the sequence length (`dlss_jitter_phases`: 8 to 64 by scale) |
| `projection`, `near_z`, `far_z`, `fov_y` | The scene's projection without the jitter |
| `reset` | No usable history (see below) |

Inputs are allocated at the size the engine asks for (`in_w` × `in_h`), with the frame's part at the top left.
`RX_VK_REACTIVE=1` also builds a reactive mask for DLSS and FSR 3.1 from blended draws (`vk_reactive.cpp`).

### DLSS feature size

The DLSS feature is created at the quality mode's **recommended** size from `NGX_DLSS_GET_OPTIMAL_SETTINGS`. The mode's
minimum and maximum size the input images and decide whether the existing feature can be reused. Each evaluation still
receives the frame's actual render size. Creating at the maximum makes models L and M cost more even when the render size
is smaller. DLAA creates at the output size. A render size that no mode takes is refused and logged once per size.

### DLSS models

The DLSS model row (`dlss_model=`; `upscaler_rows.h` `model_preset`) asks NGX for one preset in every mode. A change
applies at once: the feature is made again.

| Row (saved value) | Preset |
|---|---|
| Transformer (K) (0, default) | K; E on a card with less than 7 GB of video memory (`cnn_for_vram`), the row then saying why |
| Transformer 2 (M) (1) | M |
| Transformer 2 (L) (2) | L |
| NVIDIA default (3) | `0`: the runtime picks per mode, so a runtime update can change it |
| CNN (E) (4), CNN (F) (5) | E, F |

`RX_VK_DLSS_PRESET=<letter>` (either case) or `0` wins over the row for one run, and the row shows
`Preset <letter> (RX_VK_DLSS_PRESET)`. The log's `vk: NVIDIA DLSS: ... preset <letter>` line is what was asked for; with
`RX_VK_DLSS_LOG=1`, NGX's `Using App hint Preset <letter>` lines are what it took.

### FSR 4 models

The FSR 4 model row (`fsr4_model=`; `RX_VK_FSR4_MODEL=auto|lite` over it for one run) picks AMD's network: Auto lets
AMD's own `IsSupported` choose (FP8 on RDNA 4, full INT8 on RDNA 3 and Strix Halo, the Navi 2 INT8 model on RX 6000, Lite
on Strix Point and Van Gogh); Lite forces the INT8 Lite provider.

The hook is for one file: FSR SDK 2.4's `amd_fidelityfx_upscaler_vk.dll` (4.1.2.4555), named by the SHA-256
`scripts/fetch.py` pins for it (`fsr4_models.cpp`, `kKnown`). `fsr4_models::load_known` hashes the file beside AMD's
loader; only when it is that file does it load it from that path (before the loader asks for the name, so the loader's
own load finds this module), pin it, check that the module's file is the one hashed, and scan it. Any other file is not
loaded here and not scanned. `fsr4_models::install` then patches only each provider's `IsSupported` entry (`vtable[2]`),
after the 4.1.2 vtable shape check, and only in this process's copy; without the known library, with a shape that is not
understood, or with a vtable that changed under the scan, nothing is written (Auto). Once the loader has listed its
upscalers, `known_module_is_loaders` checks that the module under that name is still the one that was checked, and
otherwise the hook answers as AMD does (`release`). A newer AMD file is Auto only until its layout is read and its
SHA-256 added. The log's `vk: AMD FSR 4: models:` lines say what was found. Linux has no Lite provider: the row is hidden
and the switch does nothing.

Where AMD's library lists no FSR 4 for the GPU (its own `IsSupported` says no for every model: an NVIDIA or Intel GPU, a
Radeon AMD names for none) and the Lite provider is hooked, `load` asks the list again with Lite answering and, when
FSR 4 is listed then, runs every context on it (`g_standin`): FSR 4 is offered, the model row is not shown, AMD's own
shaders run (the tuned passes are measured on AMD's pick), and the log says `INT8 Lite stands in`.
`RX_VK_FSR4_STANDIN=0` leaves FSR 4 off there, as it was.

### DLSS's log and memory

NGX starts on demand: when the menu asks whether DLSS can run (`dlss_probe`), at the first DLSS evaluation, or at
DLSS frame generation's first set. A log from the title screen may have no NGX lines yet.

`RX_VK_DLSS_LOG=1` sends NGX's own lines into `longshot_log.txt` as `ngx:` (instead of NGX's log files in the game
folder's `ngx`). Two kinds look like errors and are not:

- `failed to load NGXCore: 126`, twice: NGX looking for `_nvngx.dll` and `nvngx.dll` beside the game, which are not there
  on purpose, before it loads the driver's.
- `Feature ... failed to load from cache`: NGX finding no model among its own downloads
  (`C:\ProgramData\NVIDIA\NGX\models`) before it uses the game folder's `nvngx_dlss.dll`.

DLAA draws the 3D at full size, and DLSS and DLAA add images of their own beside the game's. On an 8 GB card, busy
areas can pass the driver's allowance and stutter; Quality or Balanced is safer there. The `vk: status:` line (every 600
frames) shows the video memory in use against the allowance, with DLSS's images, jitter and actor motion appended while
it draws, and `vk: over the driver's video-memory allowance` says when the game passed it.

### Native HDR and PsychoV35

With Native HDR and the PsychoV35 tone mapper, the network's input and output are RGBA16F instead of packed B10G11R11.
DLSS/DLAA, FSR 4 and its native AA, XeSS and its Native AA, and FSR 3.1 on the chain take that scene (`takes_psychov35`).
`RX_VK_PSYCHOV35_DLSS=0` turns this off. FSR 3.1 interpolation's admission of the RGBA16F scene uses the same policy
(`upscaler_rows::psychov35_compatibility`, read once).

- The network's colour is perceptual even when stored as RGBA16F. The prep pass still decodes it to linear light. The
  typed RGBA16F output pass writes the engine's linear result without encoding it again.
- The route waits until the engine's HDR feature or context exists (`PsychoV35 waits for the native HDR ... context`).
  `RX_VK_FSR4_HDR=0` and `RX_VK_XESS_HDR=0` keep those engines on their LDR contexts.
- FSR 3.1's legacy path (`RX_VK_FSR3_CHAIN=0`) does not take RGBA16F. The TAAU stand-in and Off accept the matching
  RGBA16F pair on their own paths, without the switch.
- The log names the route: `vk: PsychoV35 <engine>: RGBA16F linear route; scene format ..., network input 97, output 97`.

The RGBA16F output shader header is generated:
`python -I -B tests/test_psychov35_dlss.py --write --glslang glslang --cxx clang++`.

## Temporal history

The chain starts a new history (`reset`) in the same frame when any of these holds:

- it is the first frame, or more than two frames have passed since the last evaluation;
- camera reprojection fails: a non-finite input or result, or a coefficient outside float range
  (`dlss_math::reprojection`). Without a valid reprojection, the old picture has no mapping to the new camera;
- a different engine draws (the previous engine is released);
- the engine made its feature or context again (`prepare` returned 2);
- the scene's format, the PsychoV35 route, or DLAA's before/after-bloom position changed;
- the previous evaluation failed with `retry` set.

Valid consecutive camera motion keeps history. There is no camera-cut heuristic beyond these.

After 120 frames with no engine evaluation (menus, Off), `temporal_idle` releases the engine's feature or context and the
chain's images.

## Camera jitter

The jitter is applied in the game's code (`nx2/oot_jitter.py`, in every lift): the scene's projection gets a Halton (2, 3)
sub-pixel offset in its uniform buffers only, so shadows and culling see the unjittered camera.

1. After each evaluation, the chain publishes one `MkwVkJitter` (render width, height, sequence length) in a
   `TemporalJitter` ([`temporal_jitter.h`](../../../gpu/vk/temporal_jitter.h)). It has its own small mutex, never the
   upscaler's GPU/NGX lock. A sample older than 30 renderer frames reads as inactive (all zeros).
2. The host calls the renderer's `jitter_sample` callback once per present (`hos_dlss_jitter_clip` in
   [`host/rx_backend.c`](../../../host/rx_backend.c)) and caches it for every view of that frame. The chain's sample wins;
   FSR 3.1's own-inputs sample is the fallback.
3. The renderer reads the applied offset back from the camera's uniform buffer, so the host only has to pick offsets.

Use the batch callback, not three scalar option queries: a resize between them gives an incoherent sample.
`RX_VK_DLSS_JITTER=0` and `RX_VK_FSR_JITTER=0` turn the jitter off.

## Texture LOD bias

The game draws its 3D at 3/4 of the output and biases its world materials' samplers by -0.5 for that. An upscaler
drawing from another share adds to the samplers the game biases below 0 (`upscale_lod_bias` in `vk_draw.cpp`,
`upscale_lod_bias_for` in `upscaler_rows.h`):

| Upscaler | Added bias |
|---|---|
| DLSS, DLAA | `log2(share / 0.75) + offset`, offset -1: NVIDIA's guide, the output's detail and one mip step finer, which DLSS resolves over its frames (Quality -1.17, Performance -1.58, DLAA -0.58, Auto -1) |
| FSR 3.1, FSR 4, XeSS | `log2(share / 0.75)` below 75%; 0 at Auto and above |
| The game's own, Off, FSR 4's and XeSS's native AA | 0 |

The bias is part of every sampler's key: the Upscaler, Render resolution and Quality preset rows make the samplers
again. `RX_VK_UPSCALE_LOD_BIAS=0` adds none; `RX_VK_DLSS_LOD_OFFSET=<steps>` sets DLSS's offset for a run (clamped to
-3..0; `0` gives DLSS the render size's own detail).

## Resources and failures

| Rule | Where |
|---|---|
| FSR 3.1 and FSR 3.1 interpolation take their constants from a slice of the recording's upload stream (`uniform_upload`), never a ring sized by CPU frames. No room means the evaluation is rejected before it touches history | `vk_fsr3.cpp`, `vk_fsr3fi.cpp` |
| Images from an old size are retired on the GPU timeline (`vk_retirement_value()`), freed once `vk_timeline_completed()` passes it. A zero ticket (no timeline) is never freed early | `drop_image`, `free_dead` |
| Engine resources released while work may be in flight go through `temporal_later` | `vk_temporal.h` |
| A failed DLSS feature creation is not retried until a DLSS setting changes (`dlss_retry`), except one refused for want of video memory: it is tried again after 600 frames, at most three creations, then the refusal stays; any other result is final at once | `vk_dlss.cpp` `prepare`, `dlss_checks.h` `retry` |
| `prepare` returning -1, or a failed `evaluate` without `retry`, disables the engine for the run; the game's network draws from then on | `draw_engine` |
| DLSS and FSR 3.1 set `retry`: a failed evaluation drops that frame to the network and resets history. DLSS remakes its feature after 30 failures in a row | `kEngine`, `kChainEngine` |

General rules for retirement, mapped memory and failed creations are in
[architecture](architecture.md#rules-for-resources-and-presentation).

## Frame generation

The Frame generation row (`opts.framegen`: 0 Off, 1 Auto, 2–8 a fixed multiplier) and the Frame generation by row choose
who makes the frames. `framegen_record` in [`vk_framegen.cpp`](../../../gpu/vk/vk_framegen.cpp) tries, per set:

| Order | Maker | Inputs | When |
|---|---|---|---|
| 1 | NVIDIA DLSS Frame Generation (NGX) | The chain's depth, motion and camera, copied by `dlss_fg_inputs` after a successful evaluation | Fixed multiplier only, up to the runtime's most frames + 1 (`dlss_fg_most`). Needs a temporal upscaler drawing; without one the set is skipped with `FG_SKIP_INPUTS` |
| 2 | AMD FSR 3.1 interpolation | Its own scene depth and camera motion, replaced by the chain's motion when the chain makes one (`temporal_fg_inputs` makes it even with no engine drawing) | FSR 3.1 FG chosen, or DLSS FG where DLSS-G does not run (`fsr3fi::frames_wanted`). Fixed or Auto |
| 3 | Built-in interpolation | Colour only: block-matched optical flow (`RX_VK_FRAMEGEN_RADIUS`) | Whatever the others did not make |

With DLSS FG chosen and no upscaler picture (the game's menus, movies, loading screens), the set is shown as the game's
frame alone (`dlss_fg_holds`, `FgMaker::DlssPaused`), not handed to another maker.

FSR 3.1 interpolation ([`vk_fsr3fi.cpp`](../../../gpu/vk/vk_fsr3fi.cpp)) is AMD's passes compiled from patched copies
of the SDK headers (`FI_PATCHES` in `scripts/shaders.py`; `third_party/` is unchanged). The patches read each made
frame's place `t` from the constant block's padding word, so any multiplier works, not only the midpoint. Optical flow
and the dilated inputs run once per game frame, the rest once per made frame. It is called every present, even with no
frames to make, so its history is always the previous frame.

### Its history and colour

- A reset (`Inputs::reset`, a skipped sequence number, a change in PQ encoding, peak luminance or HUD-less mode) makes no
  frames that present; the caller shows the game's frame.
- The captured present can hold PQ codes in UNORM storage. Both presentation paths pass `native_pq`; the interpolation
  and optical-flow constants then get transfer function 1 and 0..the HDR peak in nits. SDR uses transfer 0 and 0..1.

### The HUD

The game draws its HUD into the same picture as the scene, after the gameplay upscale.

1. `framegen_hud_before` copies the picture right after the upscale writes it, whoever drew the upscale.
2. At the present, `hud_source` (`shaders/fg_hud.comp`) finds the HUD as what the game drew since.
3. In SDR and Expanded HDR, by default (`RX_VK_FSR_FG_HUDLESS`, on) FSR 3.1 interpolates the HUD-less copy, and the HUD
   is laid over each made frame. With `=0`, the HUD is instead marked in the source's alpha (`Inputs::hud_mask`) and kept
   in place. Native HDR uses the marked, presented PQ frame when a copy is available, regardless of this switch.
4. In SDR and Expanded HDR, the HUD layer (`RX_VK_FSR_FG_HUD_LAYER`, on): the game blends its HUD as `D = B + T × S`, so
   from this frame's and the frame before's pictures before and after the HUD, `hud_layer` (`shaders/fg_hud_layer.comp`) fits per pixel how
   see-through the HUD is (T) and its colour (B), kept from frame to frame. FSR 3.1 is given the HUD-less colour as its
   present, and `shaders/fg_hud_composite.comp` lays `B + T ×` the made frame's own scene over each made frame; where T
   isn't known yet, the presented pixel. With `=0`, the presented frame is laid over as it is, and the real frame's
   scene shows through the HUD's see-through parts. Native HDR does not use the layer, regardless of this switch.
5. With no copy this frame (no gameplay upscale, or a picture that is not the presented size), the patched passes guess
   the HUD as pixels unchanged while the scene moves. `RX_VK_FSR_FG_HUD=0` runs AMD's passes unpatched, the HUD moving
   with the scene.

### Setup failures

Built-in interpolation is optional. Each pipeline and image is published only when complete. A failed pipeline,
sampler, image or view creation releases what was made and sets `fg.failed` (atomic; Reflex reads it through
`framegen_on()`) for the session, leaving ordinary presentation. A resize drains the display jobs and waits for the device
before replacing images; size and multiplier are committed only after every image exists.

## Proving an engine ran

A selected menu row proves nothing. Check the log:

- `vk: <engine> draws OoT's upscale: WxH -> WxH`, and the status line's drawn-frame counts;
- `vk: <engine>: the game's upscale runs (<reason>)` when the chain falls back (`vk: fsr3: ...` for FSR 3.1 on its own
  inputs);
- frame generation's maker in the FPS counter and the Frame generation by row (`framegen_maker`), and DLSS-G's skip
  reason (`dlss_fg_skip`).

## Switches

All switches are in [`switches.def`](../../../gpu/vk/switches.def) and [switches](switches.md). The ones you need here:

| Switch | Effect |
|---|---|
| `RX_VK_DLSS`, `RX_VK_FSR`, `RX_VK_FSR4`, `RX_VK_XESS` | Force or forbid an engine for one run, over the Upscaler row |
| `RX_VK_FSR3_CHAIN=0` | FSR 3.1 on its own inputs instead of the chain |
| `RX_VK_DLSS_PRESET` | A DLSS preset letter (or `0`) over the DLSS model row ([models](#dlss-models)) |
| `RX_VK_FSR4_MODEL=auto\|lite` | The FSR 4 network for one run, over the FSR 4 model row ([models](#fsr-4-models)) |
| `RX_VK_DLSS_LOG=1` | NGX's own lines in the game's log ([which are harmless](#dlsss-log-and-memory)) |
| `RX_VK_DLSS_DIR`, `RX_VK_FSR_DIR`, `RX_VK_XESS_DIR` | A folder looked in first for the vendor's runtime ([vendor files](#vendor-files)) |
| `RX_VK_DLSS_DEBUG` | `motion` shows the motion vectors; `alternate` shows the game's upscale every other frame |
| `RX_VK_DLSS_JITTER=0`, `RX_VK_FSR_JITTER=0` | No camera jitter for the temporal engines, or for FSR 3.1 |
| `RX_VK_UPSCALE_LOD_BIAS=0`, `RX_VK_DLSS_LOD_OFFSET=<steps>` | No upscaler texture bias, or DLSS's offset ([texture LOD bias](#texture-lod-bias)) |
| `RX_VK_FSR4_HDR=0`, `RX_VK_XESS_HDR=0` | FSR 4 and XeSS on their LDR contexts with native HDR |
| `RX_VK_REFLEX_LOG=1` | NVIDIA Reflex's latency timings in the log every 300 frames while Reflex is on |
| `RX_VK_PSYCHOV35_DLSS=0` | Turn off the PsychoV35 RGBA16F route |
| `RX_VK_FRAMEGEN=0`, `RX_VK_FRAMEGEN_MULT` | Frame generation off, or a multiplier for one run |
| `RX_VK_DLSS_FG`, `RX_VK_FSR_FG` | Force or forbid DLSS-G or FSR 3.1 interpolation |
| `RX_VK_FSR_FG_HUDLESS=0`, `RX_VK_FSR_FG_HUD_LAYER=0`, `RX_VK_FSR_FG_HUD=0` | A/B the HUD handling |

## Tools and checks

| File | What it is |
|---|---|
| `scripts/shaders.py fsr3` | Compiles FSR 3.1's upscaler and interpolation passes into `gpu/vk/shaders` |
| `scripts/fetch.py dlss\|fsr\|xess` | Fetch NVIDIA's, AMD's and Intel's SDK files for a local build ([setup](../setup.md#upscaler-sdks-optional)) |
| `tests/test_temporal_history.py` | Reprojection, reset rules and every engine's reset assignment |
| `tests/test_temporal_jitter.py` | Jitter publication, freshness and the host's per-frame cache |
| `tests/test_dlss_feature_size.py` | DLSS creation size, input range and reuse |
| `tests/test_psychov35_*.py` | The RGBA16F route per engine, the default and its opt-out |
| `tests/test_fsr_image_retirement.py`, `tests/test_fsr_uniform_ownership.py` | FSR image retirement and constant ownership |
| `tests/test_framegen_resources.py` | Frame generation setup failures and resize |
| `tests/test_fsr3fi_hud_*.py`, `tests/test_fsr3fi_hudless_colour.py` | The HUD copy, mark and HUD-less source |

These checks run without a GPU or game. They do not prove image quality or GPU behaviour; test those in the game (see
[developing](../developing.md#run-the-game-for-a-test)).
