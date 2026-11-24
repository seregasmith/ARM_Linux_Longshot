# Architecture

Longshot runs the game's Switch 2 code natively on x86-64 Windows and Linux. Its AArch64 code is translated to C ahead
of time from your own dump and compiled with clang. Horizon OS and NVN2 are implemented natively in the host, and the
game's GPU work is replayed on Vulkan. Nothing is emulated at run time.

This page covers how the parts fit together, how a frame moves through them, how the renderer accounts for memory and
decides per PC, and the rules the code keeps. To build and run it, see [setup](../setup.md) and [the build](build.md). For switches, see
[switches](switches.md). For timing tools, see [profiling](profiling.md).

## The parts

| Part | Where | What it is |
|---|---|---|
| The lifter and build | [`nx2/`](../../../nx2) | Reads the dump, lays out the image, lifts each guest function to C, and compiles and links the game |
| The host | [`host/`](../../../host) | Horizon OS: threads, SVCs, memory, file system, HID, audio, display, and NVN2 (`nvn_native.c`) |
| The renderer | [`gpu/vk/`](../../../gpu/vk) | The Vulkan back end: replays NVN2 command streams, translates shaders, presents, runs upscalers and frame generation |
| Vendored code | [`third_party/`](../../../third_party), `gpu/vk/eden`, `gpu/vk/sirit` | a64-kit (NSO loader and lifter), Eden's SM86 shader recompiler, SDL, ImGui, the upscaler SDKs. Each part of `third_party/` is checked against its `PROVENANCE.md`; `gpu/vk/eden` and `gpu/vk/sirit` are adapted copies kept in the tree |

The lineage: mkw-recomp (Mario Kart World) supplied the lifter, runtime, NVN2 layer and Vulkan back end, and Longshot
continues [Hookshot](https://git.magicbean.dev/MagicBeanDev/Hookshot). `nx2/`, `host/` and `gpu/` stay close to mkw-recomp so
its fixes still apply as a patch, which is why game-specific code goes in new files where possible.

### From dump to game folder

[`nx2/make_game.py`](../../../nx2/make_game.py) runs the whole build (see [setup](../setup.md)). The stages:

```
.nup dump ──nup.py, unpack.py──▶ exefs + romfs   (ZBIC-compressed NSOs rewritten as plain ones; third_party/zbic)
   │  a64-kit nso.py + nso2.py (DT_RELR)
   ▼
modules ──image.py──▶ image in Eden's layout from 0x80000000 (rtld, main, subsdk*, sdk), imports bound; standalone.py hooks the HLE set
   │  lift.py --standalone (a64-kit + insn.py, lift_fixes.py, poll.py, chunks.py, rtgen.py)
   ▼
one C function per guest function, in chunks (<work>/sa/gen)
   │  build.py --standalone: clang -O1 (hot functions at -O2, split_hot.py), x86-64-v3 where the CPU has it, lld
   ▼
longshot(.exe) = lifted game + host/      longshot_vk.dll / longshot_vk.so = gpu/vk, loaded by host/rx_backend.c
```

The game folder holds `bin/longshot.exe` beside its launcher. Its own files stay in the folder above `bin`. What the game
writes at run time goes in two subfolders, which the host creates (`host/game_layout.c`, `gpu/vk/vk_paths.h`):

- `cache/`: the program cache (`longshot_programs.bin` and `.idx`), pipeline caches, this PC's pipeline state list
  (`longshot_pipeline_states.bin`) and `longshot_driver_faults.txt`. The caches are made for this build and GPU, so they
  can be deleted. Deleting the driver faults file sends the programs that crashed the driver's compiler to it again, and
  deleting the state list loses the pipelines this PC's play recorded.
- `logs/`: logs and traces.

The programs' SPIR-V is not in `longshot_programs.bin` but in the player's **shader store** (`gpu/vk/spirv_pack.h`):
`%LOCALAPPDATA%\Longshot\shaders` on Windows, `$XDG_CACHE_HOME/longshot/shaders` (else `~/.cache/longshot/shaders`) on
Linux. Each program's SPIR-V is kept there once, by its content, for every game folder, and a record names its place
and hash: a read gives those words or nothing, whatever is at that place now. Builds with different translators share
nearly all of it (99.6% of programs between a stable and a nightly), so a further build adds about 0.15 GB of records
instead of about 8 GB. The cache file's head names the store, and a cache of a store that is gone starts over. Deleting
the store while no game runs is safe: each game folder prepares again. A store made again is new files put in the old
ones' place, never the old ones cut short, so a game still reading them keeps them (Linux), and while one has them open
they are not replaced (Windows). The builders keep `LONGSHOT_SHADER_STORE` for the game they start (the shaders' preparation
at the build, the AppImage's Play), so the shaders are prepared in the store the game plays with.

### The host

- **The SDK is replaced at the `nn::` boundary.** `HLE(<mangled name>)` functions in `host/` stand in for the SDK's
  OS-facing functions. The rest of the SDK (libc, `nn::util`, `nn::mem`, NVN2's front end) runs as lifted code. Any
  OS-facing import with no implementation gets a generated stub that logs its first call.
- **Memory** is one reservation of 512 GB (`host/hos.h`). The image sits at `0x80000000`, the heap grows up from 64 GB,
  and address regions grow down from 512 GB. Memory is committed when it is handed out or first touched.
- **Guest threads** run on virtual cores (`HOS_VCORES`, on by default).
- **Game-specific hooks** are gated on the game's build ID, never matched by guest address alone. For example, MKW's
  hooks check `hos_game_is_mkw`.

### The renderer

- `host/nvn_native.c` implements NVN2 over guest memory in place: objects live wherever the game puts them. Command
  buffers record an op stream (`gpu/render/rx_nvn.h`), and queue submits hand that stream to the back end.
- `gpu/vk` replays it on Vulkan 1.3. Guest pools are imported as host memory and mirrored in VRAM. SM86 programs go
  through Eden's recompiler to SPIR-V, and mesh and task programs use `VK_EXT_mesh_shader` or a compute fallback
  (`RX_VK_MESH_SHADERS`).
- Its threads: the game's own, one render thread that replays every command buffer, from 4 cores (where Auto's mode is
  safe; or alone with serial replay at 6 or more cores when only buffer-pool mirrors are absent on integrated GPUs or
  `RX_VK_MIRROR=0`) a recorder thread that makes the driver's calls for it and ends and submits each part the replay hands
  it, and a presenter thread ([threads.md](threads.md)).
- `gpu/vk` is the only renderer. Without a usable Vulkan nothing is drawn, and the log says "no renderer".
- The interface between the host and the renderer is `MkwVkHost` / `MkwVkRenderer` in
  [`gpu/vk/vk_api.h`](../../../gpu/vk/vk_api.h). Any change to it bumps `MKWVK_API_VERSION`, and both binaries are rebuilt
  together.
- The host publishes small samples that the renderer reads, such as the pointer (`MkwVkPointer`,
  `host/display_pointer.h`) and the frame's jitter (`MkwVkJitter`). Each sits behind a short lock of its own. The
  renderer never calls the window system for them, and no window call runs while one of those locks is held.

Why Vulkan: the game draws its detailed geometry with NVN2 task and mesh programs, which `VK_EXT_mesh_shader` runs as
they are. `VK_EXT_external_memory_host` lets the GPU use the game's pools in place instead of through copies, SPIR-V
comes straight from Eden's IR, and descriptors are bindless like NVN's own pools. The renderer is a library of its own
(`longshot_vk.dll`, `longshot_vk.so`), loaded by `host/rx_backend.c`. Where its code comes from is in
[`gpu/PROVENANCE.md`](../../../gpu/PROVENANCE.md).

## The frame pipeline

With the render thread on (Threaded rendering, `RX_VK_ASYNC`), a frame passes through four stages. In order: the game's
threads, the render thread (`replay_thread_main` in `vk_replay.cpp`), the presenter thread if it is on, and the GPU.

Notation: `P(k)` is present k, `S(k)` is the main thread's CPU signal for frame k, and `G(k)` is when the GPU finishes
frame k.

1. The main thread and its Presentation Thread hand over frame k's submits (`submit_q`: a capture of the pages
   written, then a job). The game's queues stop at an `nvnCommandBufferWaitEvent` on an event the CPU signals, so
   nothing can be recorded yet.
2. The Presentation Thread calls the present. It is held until `S(k)`.
3. The main thread runs **the game's frame wait**: `nvnSyncWait`, with no timeout, on the previous present's fence. The
   wait ends at `G(k-1)`.
4. The main thread signals `S(k)` (`nvnEventSignal`), with a capture of what was written since the submits.
5. `P(k)` is handed over (`RX_VK_PRESENT_AFTER_SIGNAL`, on). The render thread replays jobs as they come: the submits'
   memory work, then the signal's job, which records the frame's draws in parts and submits each part when it ends: a
   part ends at the first place the game has ended its render pass from `RX_VK_PART_MIN` (120) draws on (a submission's
   first from `RX_VK_PART_FIRST`, 32), and at `RX_VK_PART_MAX` (512) at most ([threads.md](threads.md)). With **the
   frame gate** (`RX_VK_FRAME_GATE=1`, a test) it starts frame k's jobs only once `P(k)` is handed over: whole frames.
6. The present's job records the copy into the window and calls `vkQueuePresentKHR`. On Linux, NVIDIA's driver holds
   that call until the copy is done.
7. Meanwhile the game's threads build the next frame. Each readback (`nvnBufferInvalidateMappedRange`) waits for the
   part that writes its range back.

So a frame is a ring: `G(k-1)` releases the frame wait, the signal follows, the render thread records frame k, and the
GPU runs it until `G(k)`. Nothing of frame k can be recorded before `S(k)`, and `S(k)` follows the frame wait.

### The waits

| Wait | Where | Why |
|---|---|---|
| The game's frame wait | the game | It keeps the game at most one frame ahead of the GPU. It also puts each read of GPU results (the exposure meter, the culling tables) one frame after its frame, and guards the per-frame memory the game fills in turn |
| The present after the signal (`RX_VK_PRESENT_AFTER_SIGNAL`, on with the render thread) | `vk_replay.cpp` | Present k is handed over after `S(k)`, once the game has written the frame's data; at most `RX_VK_FRAME_GATE_MS` (1000) |
| The frame gate (`gate_ready`, `RX_VK_FRAME_GATE=1`, off: a test) | `vk_replay.cpp` | Whole frames: a frame's jobs start at its present. Jobs a game thread waits on go through at once (`gate_upto`), and so does any job held past `RX_VK_FRAME_GATE_MS` (1000) with no present behind it. Off, jobs replay as they come |
| One present ahead (`RX_VK_PRESENTS_AHEAD`, 1) | `vk_replay.cpp` | Present k+1 is not accepted until present k is replayed |
| The present | the driver | NVIDIA on Linux returns when the frame's GPU work is done; other drivers return at once. `RX_VK_PRESENT_THREAD=1` (off) moves the present to a thread and queue of its own |

### What does not shorten the frame

The GPU idles at the start of each frame while the render thread records, and this is a property of the design, not
something to tune away. These have all been measured and do not help:

- **Recording faster at the end of the frame.** The GPU is behind by then, so the time saved goes to the present's wait.
- **Feeding the GPU sooner at the frame's start.** The idle time moves to mid-frame instead.
- **More presents ahead, or the presenter thread alone.** Frame k+1's draws still wait for `S(k+1)`.
- **Recording ahead of the signal.** The game writes per-frame data just before every signal, so a
  frame's first part is never final early.
- **Answering the frame wait early.** The frame's last recording both reads and writes the game's memory, so the only
  earlier answer is two frames ahead, and that made shadows flicker.

The way to a shorter frame is recording the geometry passes faster, because that is where the GPU outruns the render
thread.

### What the log counts

Every 600 presents, `vk: frame overlap (600 presents)` gives the GPU's idle time and upload time per frame, measured with GPU
timestamps. It splits them into the time before the frame's first recording with a draw and the time after. A second
line, `vk: frame overlap, by place`, gives a pair for each of the 2nd to 12th recordings, so you can see where in the
frame the GPU waited (`gpu/vk/frame_overlap.h`). The `vk: frame ahead` lines count what arrived with the signal.

## Memory budget

[`gpu/vk/mem_account.h`](../../../gpu/vk/mem_account.h) is the one place where the renderer's roughly twenty memory pools
meet. Each pool reports what it holds. One log line sums them beside the driver's count, and one arbiter asks the pools
for memory back when the card is nearly full. `tests/test_mem_account.py` checks it.

### The budget line

The line appears every 600 presents, after `vk: memory:`, `vk: memory heaps:`, `vk: image blocks`,
`vk: render target images:` and `vk: VRAM:`:

```
vk: budget: VRAM <sum> of <allowed> MB (<used> MB by the driver, <unaccounted> MB unaccounted): <pool> <MB>, ...; RAM <sum> MB (the process's private memory <private> MB): <pool> <MB>, ...
```

Pools are listed largest first. Pools under 1 MB are grouped as `others`. Each pool is listed under the heap its memory
is in. On Linux, the guest memory behind `imports` is shared memory, so it is not part of the private figure.

| Pool | What it holds |
|---|---|
| `textures`, `sparse textures`, `textures in RAM` | The texture cache |
| `target pool` | Images from dead render targets, kept for the next target of the same shape |
| `dead images`, `dead buffers` | Allocations that are released but still wait for the GPU |
| `other images`, `other buffers` | Every image or buffer made through `vk_vma.cpp` that no other pool reports: upscaler and frame generation images, recording streams, the 24 MB guest page table, mesh rings |
| `blocks free` | Free space inside the allocator's blocks (the driver counts a whole block as used) |
| `mirrors`, `shadows` | VRAM copies of the game's pools, and the write-back's second copy |
| `staging` | The texture upload arenas |
| `captures`, `capture spares`, `capture staging spares`, `capture table` | Submit-time copies of guest pages, plus spares kept for reuse |
| `imports`, `retired imports`, `retired records` | Guest memory the driver has pinned for the GPU |

`unaccounted` is the driver's count less the VRAM sum. It is the memory the renderer did not allocate itself: the
swapchain, the driver's pipelines and its own objects, memory the upscaler libraries allocate internally, two small
buffers made outside the allocator, plus a few tens of MB of timing noise. It reads `n/a` without
`VK_EXT_memory_budget`.

### The arbiter

At every eviction pass (every 10 presents), the arbiter compares `pressure` (the driver's count less the free space the
textures could still take) against two thresholds:

| Level | Trigger |
|---|---|
| 1 | `pressure` past 90% of the driver's allowance |
| 2 | the driver's count itself past 97% of the allowance |

At level 1 or 2, the pools that keep memory only for reuse are asked in order, until the excess is covered:

1. `capture spares`: trimmed to 32 MB (`cap_free_trim`). This happens only where the spares are device memory, as on an
   integrated GPU.
2. `staging`: idle VRAM arenas are released at the next present.
3. `blocks free`: compaction runs at the next present.
4. `target pool`: `pool_trim`, keeping half its cap at level 1 and nothing at level 2.

Steps 1 to 3 run once each time the level rises, and memory counts as calm again after 60 looks in a row at level 0.
Without that rule, a pool that is costly to remake would be freed and remade every pass. What the pools return comes
off the 90% rule's excess, not the 97% rule's. Mirrors, shadows, imports and captures in use are never
asked. Mirrors keep their own budget instead (`gpu/vk/mirror_budget.h`). Each return is logged as
`vk: budget: gave back`.

What the pools leave comes off the texture cache's budget. Within the allowance that budget never falls below a floor
(`gpu/vk/vram_compact.h`: 1800 MB on a card the driver allows under 8 GB, a quarter of the allowance above, and on the
smaller cards a floor that rises with an idle card up to half the allowance), so a full card does not evict the textures
in use one pass after another. Past the allowance there is no floor (`floor_within`): the driver pages VRAM out to system
memory there, and only the textures used in the last 60 presents stay.

### Each heap's budget

The allowance above is the sum over the device-local heaps, and one heap can be full while the sum has room. A card
without resizable BAR, and an AMD APU on Windows, has a second device-local heap of 256 MB that the CPU can write: the
window of VRAM (or of the UMA carve-out) behind the BAR. VMA puts a host-visible buffer the GPU reads there, the staging
arenas among them. On a Legion Go (8 GB UMA) a save load's staging took that heap to 252-277 MB of the 243 MB AMD's
driver allows while VRAM as a whole stood at 1.9 of 7.8 GB, and the driver refused every submit after
(`vk: submit ... failed (-2)`, VK_ERROR_OUT_OF_DEVICE_MEMORY): a black screen with the sound going on (#1879).

- `create_buffer` makes a host-visible buffer within its heap's budget first, so VMA takes the next memory type the
  buffer can use (system memory) when that heap is full, and makes it as before where no heap has room
  (`RX_VK_BUFFER_BUDGET=0`: as before). The `vk: memory heaps:` line shows each heap's use and budget.
- A submit the driver refuses for want of memory is made again, up to three tries, after the GPU's earlier work has
  finished, and the next present frees the idle staging arenas and the image reaper's queue (`gpu/vk/submit_retry.h`,
  `vk_replay.cpp`'s `submit_or_retry`; `RX_VK_SUBMIT_RETRY=0`: dropped, as before). Nothing is destroyed under the
  queue's lock. `RX_VK_TEST_SUBMIT_REFUSE=<n>` refuses the first try of every n-th submit, so any card plays the path.

## The memory check

`RX_VK_MEM_CHECK=1` counts breaks of the memory-sync rules. It never changes what the renderer does, and with it unset
each hook costs one flag test. It logs the first eight cases of each kind
and a summary every 600 submits (`vk: mem check`). Counts are totals since the start.

The renderer keeps several copies of each guest page: the pages captured at each submit, the VRAM mirrors, and the
write-back shadows. Most flashes and stale frames have come from two of those tables disagreeing, and the check tells
you which rule broke and on which page.

| Counter | Rule |
|---|---|
| `take_open`, `collect_open` | A capture is taken, and written pages are collected, only between recordings |
| `seq_mismatch` | Each submission takes the capture its own submit made (`takes not paired` counts paths the check does not cover) |
| `live reads` | Nothing the replay reads comes from live guest memory ([rule 2](#rules-from-mkw)), broken down by why no capture had the page |
| `gen_back` | The 64-bit page generation counter only goes up |
| `sync_over_gpu`, `sync_over_gpu_only` | Candidates only: a mirror sync copied over a page the GPU wrote. The check reads the page back. `replaced` means a word went back to its previous value, which is either a loss or the CPU resetting a counter, and the bytes cannot tell those apart. `on a page no CPU write made dirty` is the certain part: a real loss |
| `pages late at the replay` | Not a break. These are pages the replay found late (`RX_VK_LATE_AT_REPLAY`, on). Use `=0` to A/B against the behaviour without it |

The rules and counters are in [`gpu/vk/mem_check.h`](../../../gpu/vk/mem_check.h), which has no Vulkan in it and is
driven by `tests/test_mem_check.py`. The hooks are in `vk_memory.cpp` (`memory_capture_submit`, `memory_take_capture`,
`mirror_sync`) and `vk_replay.cpp` (`submit_q`, `submit_memory_work`).

The check has blind spots:

- A GPU write of more than 64 MB in one binding is tracked as a range, so every page in it counts as written.
- With `RX_VK_CAPTURE_WAIT=0`, every page the GPU ever wrote is a candidate.
- Host writes that carry no write mark are not covered. `tests/test_host_guest_writes.py` holds those to the rule by
  their source text instead.

## Device policy

[`gpu/vk/device_policy.h`](../../../gpu/vk/device_policy.h) is where the renderer decides by the PC: the GPU's vendor and
driver, VRAM, RAM, CPU count and OS. `tests/test_device_policy.py` runs every rule on made-up PCs.

- **`DeviceFacts`** is what `create_device` (`vk_main.cpp`) learns once, before the device is made. Rules ask the
  facts (including `arch()` / `arch_name()` from [`gpu/vk/gpu_arch.h`](../../../gpu/vk/gpu_arch.h), plus `rdna1()`..`rdna4()`
  and `amd_win()`), never Vulkan.
- **`device_policy(facts)`** gives each rule's default for this PC.
- **`device_policy_env(policy, facts, get)`** applies the switches and records which fields a switch set. The result is
  `dev.policy`.

Each field's comment says **read** (its site uses `dev.policy`) or **site** (its site still decides with its own
switch, and the header only mirrors the rule for the log and the test). Sizes that depend on the driver's allowance are
computed in `policy_rule`, and each site calls the rule again at its first use.

The program cache's stamp covers `vk_shader.cpp`, `vk_bank.cpp`, `vk_state.h`, `eden/`, `sirit/` and `compat/`
(`nx2/translator_files.py`). `device_policy.h` is not in the stamp, so every field `vk_shader.cpp` reads must be folded
into `translator_stamp`. The test fails if one is not.

### How switches are read

`device_policy_env` reads every switch it handles by one rule:

- **Yes/no:** unset or empty gives the default. `0`, `false`, `off` or `no` turns it off (any case, surrounding spaces
  ignored). Any other value turns it on.
- **Number:** decimal digits only, within the field's range (`RX_VK_EQ_BIAS` allows one decimal point). Anything else is
  ignored, the default stays, and the log says so.

The shader translation's switches keep their own parsing: `RX_VK_FETCH_BOUNDS`, `RX_VK_NAN_BITS` and
`RX_VK_GATHER_OFFSET` in `device_policy_fixes_env`, and `RX_VK_WARP64` and `RX_VK_FRAG_SHUFFLE_FALLBACK` in
`make_profile`.

### The log

After `vk: driver:`, six `vk: policy:` lines appear: the facts, the driver fixes, what the card can do and what was
measured, sizes by the allowance, auto settings and threads, and finally the switches. A `*` after a value means a
switch set it, and the last line names those switches along with any ignored values. A field still decided at its site
shows only its default.

### Adding or changing a rule

1. Write the rule in `device_policy()` as a test of the facts. Test the **driver** (`driver_id`) when the cause is a
   driver's code, and the vendor only when it is the hardware. Add a version bound when a report names the driver that
   fixed it. The field's comment says which PCs it applies to, why, the issue, the driver version and the switch.
2. If you need a fact the struct lacks, add it to `DeviceFacts` and fill it in `create_device`.
3. If the rule has a switch, read it in `device_policy_env` and give it an `Sw` value and a row in
   [`switches.def`](../../../gpu/vk/switches.def). Prefer a default that is right for every PC over a new switch.
4. Make the site read `dev.policy`, not Vulkan, the vendor ID or the environment.
5. Add the field to `tests/test_device_policy.py` (`BOOLS` or `NUMS`), with its value on every made-up PC and a case for
   its switch.
6. If the rule changes what a GPU gets, say which GPUs and how in the pull request.

Some rules are keyed on a fact that may be the wrong one, and settling each needs a run on that hardware:

- **The native float add list** (`native_fadd_listed`).
- **Intel vendor rules** (the equal-depth margin, the part split before a draw, the uniform snapshot). These also catch
  Mesa's Intel driver.
- **"NVIDIA" rules.** These also catch NVK.
- **`mesh_no_optimizer`** on RDNA 3 and 4.
- **Small-card sizes**, such as the mirrors' third of VRAM on a 4 GB card.
- **The worker counts' thresholds.**

## Rules from MKW

These rules come from mkw-recomp. The code that keeps each one carries a `// doc:` comment pointing here, so
read the rule before you simplify that code.

1. **Cross-queue `nvnCommandBufferWaitEvent` is honoured.** The game's queues hand stages back and forth with events,
   and replaying them in arrival order on one Vulkan queue causes flashes. `submit_job` (`vk_replay.cpp`) replays each
   submission as far as its waits allow, keeping state per queue. A stopped queue's submits still do their memory work
   in submit order and run later, when what they wait on is signalled.
2. **Nothing the replay reads comes from live guest memory.** Games write per-frame data ahead of the GPU. Every read
   comes from submit-time captures, and pools up to `RX_VK_CAPTURE_POOL_MB` (64) are captured from their creation, not
   from their first GPU use (`vk_memory.cpp`). Check: `live reads` in the [memory check](#the-memory-check) stays near 0.
3. **Texture and sampler registrations are queued in order with the submits** (`register_texture_queued`), never
   applied out of band. Applied at once, a frame being replayed would sample the next frames' render targets, which
   have another size.
4. **GPU timestamps drive dynamic resolution.** Counter reports (`NOP_REPORT_COUNTER`, `vk_replay.cpp`) give the time
   since the frame's present, scaled by `RX_VK_TS_SCALE` (0.1), in the 19.2 MHz counter ticks the game reads. The game's
   resolution controller has to settle where the PC can actually keep up. If the controller read the replay's real
   clock, it would see the replay's lag instead of GPU time and drop to its floor. Check: the render size it picks over
   time.
5. **Clears after draws, and draws that sample a target their pass already wrote (or write a target an earlier draw of
   the pass sampled), get passes of their own** (`clear_after_draws`, `draw_hazards` in `vk_draw.cpp`). Within one pass
   the GPU orders attachment writes only with each other, not with texture reads. A draw that samples a target it
   writes itself stays in the pass.
6. **2D views of 3D textures need `VK_EXT_image_2d_view_of_3d`.** Without the extension, such a sample gets no view.
7. **The CPU saturates first.** The game's threads, the replay, captures and pipeline compiles share the cores, and
   extra helper threads have lowered fps before. Measure thread counts (`HOS_THREAD_TIMES`, `HOS_SAMPLE`); never raise
   them by default (`RX_VK_CAPTURE_THREADS`).

## Frame generation's presents

These rules apply to a fixed frame generation multiplier. Breaking any of them looks as if frame generation were off:
the FPS counter and the display thread's counts say the frames were made on time, but the screen shows the game's
frame rate. `tests/test_framegen_fixed_pace.py` runs the timing functions through a model of the display loop.

1. **Frame pacing is measured on the screen, not in the log.** The display thread's `shown on time` and `shown apart`
   counts measure its present calls, and a present can still wait for the GPU after the call. Use PresentMon's
   `DisplayedTime` (the share under 2 ms, p10 to p90). To find where a frame waited, compare `DisplayLatency` against
   `CPUBusy` and `GPULatency` + `GPUTime`. The method is in the
   [measure skill](../../../.claude/skills/measure/SKILL.md).
2. **A set is shown on the game's timeline, in every present mode** (`fixed_on_timeline`, `fixed_dues`, `pace_lead`
   in `vk_framegen.cpp`), whatever the Frame pacing setting says.
   - Under immediate (V-Sync Off), a set presented as it arrives shows almost only the game's frame.
   - Spacing a set's frames evenly makes the motion stutter as much as the game's frame times vary.
   - Under FIFO, a frame goes at its time but no sooner than a refresh after the frame before (`fifo_slot`).
   - `RX_VK_FG_TIMELINE=0` turns this off for an A/B.
3. **The GPU's delay is fed back only from sets taken as they came** (`pace_delay`, and `as_came` in `display_main`).
   A set that was late for another reason (no free swapchain image, the display thread busy) would raise the delay by
   its own lateness. That pushes the next set's copies later still, and at 4x the delay grows to a whole game frame.
4. **A set presented ahead needs a frame slot for every frame of both sets in flight, plus two sets' spare swapchain
   images** (`present_slots_for` in `vk_replay.cpp`; `create_swapchain` in `vk_main.cpp`; `present_ahead_wanted`).
   - Each present waits for the copy last made in its slot. With fewer slots, a set's copies wait behind each other, or
     behind the shown set's copies and all the game work queued before them.
   - Under FIFO, a set too big for the chain to hold, or a display whose refresh is unknown, gets one set's spare images
     and frame slots, not two: the frames then go out as they come, and two sets would only deepen FIFO's queue.
   - The slot count is `dev.swap_slots`, at most `kPresentSlotsMax` (8). `RX_VK_FG_TIMELINE=0` keeps 3 slots.
   - An app may hold only imageCount - minImageCount acquired images, so the next set's copies go out as images come
     free (`copy_up_to`), never all-or-nothing.
   - Whatever a copy reads must live through `kPresentSlotsMax` presents after it: the overlays' staging buffers and
     old textures (`kFramesLater` in `vk_imgui.cpp`), and Dear ImGui's vertex buffers (`ImageCount`).
5. **A present waits on the queue it is made on.** On the replay's graphics queue it waits behind the game's next
   frame. A compute family's queue runs beside it and gives far more even frames (`RX_VK_PRESENT_QUEUE`,
   `create_device`). It is **off by default on every PC**, because with frame generation it lost the device on
   an RTX 5080 and gave an RTX 4090 a black screen. The cause has not been found.
6. **Under FIFO the made frames do not queue** (`fifo_catch_up` and `fifo_pick` in `vk_framegen.cpp`;
   `fifo_min_interval_us` in `vk_reflex.cpp`). FIFO shows one frame per refresh. Once the game's rate times the
   multiplier passes the refresh, the sets would queue, hold the game back and show each game frame later and later.
   - FIFO's catch-up keeps the most frames of a set that still let its game frame (its last) appear within a refresh
     of its time. The frames left out are spread evenly over the set (6x with 3 shown: the 2nd, 4th and 6th), and their
     copies are never made.
   - At most all made frames but one are left out, so 2x keeps its made frame.
   - The GPU's delay is still fed back against the set's first frame.
   - `RX_VK_FG_FIFO_CAP=1` (off) uses Reflex to cap the game at the multiplier's share of refresh − refresh²/3600.
     This suits G-SYNC and FreeSync displays only: on a fixed-refresh display it leaves refreshes without a new frame.

## Rules for resources and presentation

Each line below is enforced by a game-free check under `tests/` ([run the checks](../developing.md#run-the-checks)).

- **CPU reads of GPU-written mapped memory** take three steps: a copy-to-host-read barrier
  (`capture_host_read_barrier`), a wait on the owning fence or timeline value, and `invalidate_mapped` before the read.
  This covers screenshots, frame dumps and frame-log slots.
- **CPU writes to mapped memory are flushed before GPU use:** texture staging slices (aligned to
  `nonCoherentAtomSize`), UI texture uploads, and each recording's streams. Mapped memory need not be coherent.
- **Per-frame constants live in the recording's upload stream** (FSR's included), never in a ring sized by CPU frames.
- **Release on the GPU timeline, not by present count.** Anything retired waits for `vk_retirement_value()`. This
  covers FSR images from an old size and retired samplers.
- **A failed creation is never cached as a handle.** Samplers, image views and texel buffer views retry, at most once
  per present (or texture frame) per key. An object is published only when it is complete: frame generation's
  pipelines and images, recording streams and descriptor pools, and imports (`vkBindBufferMemory` is checked).
- **Samplers stay under `maxSamplerAllocationCount`.** One sampler is kept per distinct create info, and from 3/4 of
  the room the least recently used one is retired (`sampler_room` in `vk_draw.cpp`). `bind_descriptors` refuses a
  draw with a failed sampler and never substitutes another filter.
- **Texel buffer views** are keyed by the imported `VkBuffer`, not by guest address, and are released before their
  buffer (`destroy_import_buffer`).
- **`VK_ERROR_DEVICE_LOST` is terminal everywhere,** presentation included. `device_lost()` writes its report, then
  `replay_fatal()` flushes the log and exits.
- **A failed swapchain replacement still retires the old chain.** It is dropped, and the next present recreates the
  chain before acquiring. Reflex checks the swapchain generation before it marks a frame.
- **Files are written to a temporary and replaced atomically:** PNGs and the pipeline caches. The pipeline caches are
  read and written through file mappings, never a cache-sized buffer.
- **Presentation geometry** lives in `vk_present_rect.h`. A crop is clipped by `present_source_rect`, and an empty
  crop gets a black clear. The host's aspect describes the picture after the crop.
- **Temporal history is reset in the same frame when camera reprojection fails.** This applies to every upscaler path
  and to frame interpolation.
- **Float colour outputs are kept finite** (`GuardFragmentOutputs`, `RX_VK_NAN_GUARD`). A single NaN spreads through
  the bloom to the whole picture.
- **Fog and exposure come from the game's own values.** No Longshot factor scales them.
- **A program the driver's compiler faulted on is never sent to that driver again**, in this run or later ones
  (`gpu/vk/driver_faults.h`, `RX_VK_DRIVER_FAULTS`).
- **The renderer's own compute pipelines are asked again without the optimizer** when the driver refuses the optimized
  build (`extern_guarded_compute`; `texture_compute_pipeline` for the texture pipelines and the upscale helpers, through
  the pipeline cache). Intel's Windows driver on the Arc A-series answered `VK_ERROR_UNKNOWN` for optimized builds it made
  without the optimizer, and the GPU ASTC decoder was lost to it: every ASTC texture decoded on the CPU (#1874).
- **Render conditions read reports from earlier in the same stream**, so they cannot be resolved at record time.
- **Mesh compute fallback pipelines** use a static vertex stride. Binding one invalidates dynamic strides, so the draw
  path drops both vertex-bind skip caches at that bind.

## The Insert menu

Every row of the Insert (F1) menu belongs to a feature module: one feature's rows in a file of `gpu/vk/menu/`, with the
code that is theirs (state, texts, hints, actions), placed by the module itself. `gpu/vk/menu_model.h` is the model (no
ImGui or Vulkan in it), `gpu/vk/menu_module.h` what a module includes, and `vk_menu.cpp` draws what the model builds. The
build compiles every `gpu/vk/menu/*.cpp`, so a new feature is a new file. A section and its rows (`gpu/vk/menu/hdr.cpp`):

```cpp
MENU_MODULE(hdr) {
    menu::tab("graphics")
        .heading("hdr", "HDR", menu::after("screen"))
        .row({.key = "hdr", .label = "HDR", .help = "...", .value = &opts.hdr, .lo = 0, .hi = 2, .preset_exempt = true,
              .text = [](int v) { return std::string(tx_hdr(v)); }, .hint = hdr_sdr_hint});
}
```

- **Its key is its line** in `longshot_options.ini` and its name over the control bridge. Every saved key is in FROZEN in
  `tests/test_menu_ini_keys.py`: a new row's key goes there, and a row renamed or moved keeps its key, or players lose
  that setting.
- **Its texts are the translations' keys**: a row moved keeps its label, help and choices to the character.
- **Where it goes**: `after(key)` / `before(key)` beside a row, `first()`, `last()`, `order(n)`, `at(n)` within its
  section; a section (`heading(id, label, place)`) is placed among its tab's the same way. The tabs are `graphics`,
  `audio`, `gameplay`, `controls`, `nfc`, `mods`, `debug` and the pages `motion`, `fps_details`, `recordings`, `controls_page`. The
  order is worked out once, whatever order the modules register in.
- **Kinds**: Choice, Toggle, Slider, headings, Action (a button: `on_activate`, `verb`, `confirm`) and Page (`opens`).
- **Hooks** instead of checks by label elsewhere: `shown`, `locked`, `text`, `hint`, `step_fn`, `on_change`,
  `on_activate`, `restart_value`, `stops_at_ends`, `restart`, `preset_exempt`.
- **What needs the renderer is fenced**: the checks compile every module with `MENU_LAYOUT_ONLY`, without Vulkan. Such
  code goes under `#ifndef MENU_LAYOUT_ONLY` and its hooks are `MENU_HOOK(...)`. What a module asks of the renderer is
  declared in `menu_module.h`'s "what modules ask the renderer" section.
- **Room for 256** module rows and headings (`kModuleRowsMax`, `tests/test_menu_module_slots.py`).

`vk_menu.cpp`'s own table keeps only the core's three section headings (`resolution`, `screen`, `developer`) and the
custom rows it still draws itself (Custom output size's text field, the Controls row's bindings grid, the Debug tools'
panel: `Custom` items in the model, `bespoke_row`); the Mods tab's lines are drawn by `draw_mods`. Checks:
`tests/test_menu_modules.py` builds every module (no game) and fails on a missing anchor, a cycle or a doubled key; a
check that reads rows uses `tests/menu_model_fixture.py` (`menu_text()`, `built_menu()`), and
`tests/test_settings_table.py` compares the menu as built with the settings table in
[graphics and controls](../../user/graphics-and-controls.md).

## Tools

| Tool | Use |
|---|---|
| `scripts/memtour.py` | Drives a game through areas and writes the `vk: budget:` figures to `mem.csv`, one column per pool |
| `scripts/perf.py chain` | Walks each frame back from its present in a `HOS_CHAIN` recording |
| PresentMon | Frame generation's pacing on screen ([rule 1](#frame-generations-presents)) |
