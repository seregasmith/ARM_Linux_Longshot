# The renderer's threads

What each of the renderer's threads does, how work passes between them, the orders that hold across them, where they run,
and how to see it in the log. The code is `gpu/vk/vk_replay.cpp` (the render thread and the presenter),
`gpu/vk/vk_recorder.h` (the recorder thread), `gpu/vk/threaded_rendering.h` (whether the render thread and the recorder
thread run), and the Threaded rendering row (`gpu/vk/vk_menu.cpp`
`threaded_choice`). The frame's waits, in time, are in [the frame pipeline](architecture.md#the-frame-pipeline); the switches' rows are
in [switches.md](switches.md).

## The layout

```
 the game's threads ──jobs──▶ render thread: memory work, the replay of every command buffer, every submit
   (submits, presents,              │ each submission's parts handed over as they end (maybe_split)
    captures at submit)             ├─a part's vkCmd* packets, its end and submit (a ring)──▶ recorder thread ──▶ the driver
                                    └─present (copy done by a fence)──▶ presenter thread ──▶ vkQueuePresentKHR
```

| Thread | Named | Its job |
|---|---|---|
| The game's threads | the game's own (main thread, Presentation Thread, job threads) | Record NVN command streams into guest memory. At each submit the host takes the capture of the pages written since the last one and hands the render thread a job (`submit_q`); a present is a job too, handed over after the main thread's CPU signal for its frame |
| Render thread | `mkw_vk replay` (`replay_thread_main`) | Runs the jobs in hand-over order: the memory work (mirror syncs, write-backs), the replay of each command buffer into recordings, every queue submit (with the recorder thread, each part's submit decided here and made there), the frame's copy into the window. Off (Threaded rendering Off, or Auto under 4 cores): the replay runs on the game's submitting thread instead |
| Recorder thread | `mkw_vk recorder` (`vk_recorder.h` `Recorder::loop`) | Makes the driver's `vkCmd*` calls the replay thread packed into its ring, in order, then each part's two `vkEndCommandBuffer` and its `vkQueueSubmit2`, while the replay goes on into the next part ([the hand-off by part](#the-hand-off-by-part)). Threaded rendering On, Auto from 4 cores where its mode is safe, or Auto alone with serial replay at 6 or more cores when only buffer-pool mirrors are absent (`RX_VK_RECORDER`) |
| Presenter | `mkw_vk presenter` (`presenter_loop`) | Waits for the copy's fence, then makes the present. On every PC |

Not covered here: frame interpolation's display thread (`gpu/vk/vk_framegen.cpp` `display_main`, which makes its own
presents), the background workers that translate shaders and build pipelines (`vk_jobs.cpp`), the capture helpers and
the texture threads.

## Who records

The render thread replays every command buffer; from 4 cores (`threaded::kAutoCores`), where Auto's mode is safe (the
buffer pools mirrored, as a discrete GPU's are by default; the capture on: `vk_menu.cpp` `threaded_choice`), the recorder thread makes the driver's
calls for it. When only buffer-pool mirrors are absent (integrated GPUs where `!f.discrete()`, or `RX_VK_MIRROR=0`), the
render thread stays off and Auto runs the recorder thread alone with serial replay at 6 or more physical cores
(`threaded::kAutoRecorderOnlyCores`), leaving 4- and 5-core systems off.

- **The recorder thread** (`RX_VK_RECORDER=thread`) takes the replay's Vulkan calls as packets through a ring and makes
  them in order. It was removed on 2026-11-19 and brought back the same day: without it the render thread makes every one
  of the driver's calls, and on AMD's drivers (about twice NVIDIA's CPU time a call) the market's densest view, ~2,500
  draws a frame, fell 16 to 25% behind main on an RX 7900 XTX, RX 9070 XT and RX 9060 XT (the Test Farm on #1763). Its
  known costs were that it woke 20 to 25 times a frame, and on an i7-9700K those short sleeps let the CPU's clock control
  slow every core (Kokiri Forest 15 fps against 48 with it kept spinning); kept spinning, it cost a Ryzen 9 7900X 3 to
  11%. Both came from the render thread waiting for the ring to empty at every part's end and submit: since
  2026-11-20 the recorder takes whole parts and submits them itself ([the hand-off by part](#the-hand-off-by-part);
  not yet measured). `RX_VK_RECORDER=off` (or any value but `sync`, `defer` and `thread`) turns it off over the row,
  for an A/B.
- **The segment workers** (`RX_VK_PARALLEL`, removed on 2026-11-19) recorded a big command buffer's segments in
  parallel, after a dry pass over it for each segment's start state, and the render thread submitted them in order.
  With every core's worth of them they lost on every PC tried: an RTX 3090 / Ryzen 9 7900X 10 to 25%, an RTX 3070 on
  Windows 10 to 35% with an access violation in both runs, an RX 7900 XTX on Linux 3 to 34% (the Test Farm on #1763). A
  Tracy capture on the 7900X showed why: the frame is the game's own draw building on its job threads, the main thread
  waiting for it; two workers made that 28% slower (they shared its cores) while the render thread, busy 22% of the time,
  gained nothing.

Every hand-off is a job or a present, but for the recorder's packets:

- **Game threads to the render thread:** a job per submit, present and CPU event signal, with the submit's capture taken
  first, on the game's thread. `RX_VK_PRESENTS_AHEAD` (1) bounds how far ahead the presents may run.
- **Within a submission:** its parts end where the GPU needs the next one (`maybe_split`: with no render pass open when
  the GPU has a part or less left, at 512 draws and dispatches at the latest), each submitted as it ends, so the GPU
  starts on the first while the rest are recorded.
- **Render thread to the recorder:** each `vkCmd*` call as a packet (its arguments and every array they point to copied),
  handed over a part at a time with the part's end and submit ([the hand-off by part](#the-hand-off-by-part)). A thread
  without the replay's lock makes its calls directly, after the recorder has run what was published.
- **Render thread to the presenter:** the image index and the copy's fence (`present_hand_off`); the render thread goes
  on at once.

## The orders that hold

1. **Cross-queue event waits are honoured on the CPU.** A submission is replayed as far as its event waits allow, with
   per-queue state, and an event's signal is applied on the CPU's view in order (`submit_job`; architecture.md, Rules for resources and presentation,
   rule 1).
2. **Captures are taken at submit, in hand-over order**, on the game's thread, before the job is queued. Nothing the
   replay reads comes from live guest memory (architecture.md, Rules for resources and presentation).
3. **Fence values are handed out at submit, and a value maps to its last part.** A submission split into parts signals
   its timeline value from its last part only; the parts' recording slots go free with it.
4. **A readback waits for the part that writes its range back**, the camera block aside. The culling camera block
   (0x3980 bytes) is read without its wait on every PC (`RX_VK_READBACK_LATE`, `gpu/vk/readback_late.h`): the game reads
   it right after submitting the work that fills it, so on the console it cannot count on that work either, and the
   reads of it were 40 to 45% of the readback wait on a Steam Deck. On a discrete RADV card (RX 9070 XT) a rule naming two further sizes besides the camera block
   (`RX_VK_READBACK_LATE=3980,40,2000`) lowered the median frame time 3.6-10.7% on five of six legs of a heavy route
   (`docs/tested/`); the exposure meter's own sizes are left waiting.
5. **One queue, one submission order, under `dev.present_mu`.** Every submit to the replay's queue, and a present on it,
   takes that lock. The presenter thread holds the queue's lock only for its `vkQueuePresentKHR` call, after the copy's
   fence, so the call does not wait for the GPU with the replay's submits held behind it. The recorder thread takes it
   for each part's submit; any other thread about to take it waits first until the parts handed over are submitted
   (`QueueMutex`, `vk_internal.h`), so that its submit, present or idle wait comes after them.
6. **Every wait is bounded.** The presenter waits for a copy at most 4 s and a drain at most 5 s; a readback waits at
   most `RX_VK_READBACK_WAIT_MS` (250 ms) for its value; a device lost ends the waits.

The barrier bookkeeping is the recording thread's own (`vk_work.h`: `work_epoch` and `other_work_since_barrier` are
`thread_local`), so a background thread's work never makes its barrier look needed or not.

## The recorder

### The hand-off by part

`RX_VK_RECORDER_PARTS` (on wherever the recorder runs; `=0` for the old behaviour, an A/B in one build). The code:
`vk_recorder.h` (`post_part`, `boundary_parts`, `queue_lock_event`, `Recorder::wait_until`) and `vk_replay.cpp`
(`submit_recording`, `post_part_submit`, `part_submit`, `recording_wait`, `wait_rec_seq`).

- **Who publishes.** The render thread (the holder of `replay_mu`) packs every `vkCmd*` call of a part into the ring but
  publishes them only at the part's end, or in 256 KB chunks of a long one (`g_chunk_bytes`): one wake-up of the
  recorder a part, not one a call. The recorder sleeps on the ring's counter between batches (WaitOnAddress, a futex
  on Linux; a condition variable without them) after a 10 µs spin.
- **Who submits.** At a part's end `submit_recording` decides everything about the submit on the render thread (the
  timeline value it signals, its number on the recordings' timeline, the Reflex frame) and puts the recording where
  the waiters find it (`value_rec`, `part_rec`); the part's two `vkEndCommandBuffer` and its `vkQueueSubmit2` (or its
  hold with `RX_VK_BATCH`) go into the ring as one more packet (`post_part`), and the render thread goes on into the
  next part, in another recording slot, at once. The recorder runs that packet in order, after the part's own, under
  `dev.present_mu`; then it counts the part done (`parts_done`, after `R`) and wakes whoever waits for one. A failed
  submit or end goes to `device_lost` there, as on the render thread: the report, then the process ends.
- **Who waits for what.** Nothing waits at a part's end. What needs a part *submitted* waits for that, not for the
  ring to empty:
  - a slot's reuse waits for its own last part (`recording_wait`: `rec::wait_part`), then for its fence; its pools'
    resets and begins are then made at once (`OwnSlot`: nothing in the ring names them);
  - a wait on the recordings' timeline (a readback's part, a fence of one queue) waits for that recording's submit
    within its own time (`wait_rec_seq`: `wait_seq_submitted`; not submitted in time, it is a wait that timed out), so
    that a held batch's flush has it;
  - a flush before a wait (`queue_flush_for`) waits for every part handed over first, within the wait's time (a poll:
    not at all), and only then looks at what is held: a part still in the ring would be held after the flush, and with
    `RX_VK_BATCH` the wait would never come back. The driver's wait then gets what is left of the time, not all of it
    again. A wait for the parts that ends unmet (not a poll's: the game polls many times a frame, and each would send
    the batch) sends what is held itself if the queue's lock is free, and owes a flush to the part it waited for, which
    sends what is held once it is submitted (`flush_owed_upto`, `take_flush_owed`; the parts before it do not);
  - every other wait for a fence or a timeline value, and a fence's reset, waits for every part handed over so far, at
    most 250 ms (`parts_settle`, `kSettleNs`); the query pools' host reads and resets, and a mapped allocation's
    invalidation, wait for nothing (`parts_unordered`: they name no command buffer, and what they read the caller's own
    wait has made sure of);
  - the queue: a thread about to take `dev.present_mu` waits for every part handed over first (`QueueMutex`; not its
    `try_lock`, whose one caller, the DLSS Frame Gen exit, waits for them itself within its 2 s); the presenter thread,
    a deferred present (`present_commit`) and Reflex's latency sleep do not. The recorder says when it is blocked on that
    lock, and a holder of it blocked on the presents' queue locks says so too: a thread the recorder waits for, either
    way, never waits for the recorder in turn (`queue_escaped`, counted and logged once; at the device's destruction it
    ends the process, as nothing can go on). A slot's reuse never takes such an escape: it is never under a queue lock,
    and its part unmet ends the process. Two places may make their queue work past a part the recorder holds at the
    lock: a deferred present (`present_commit`, Settled: its copy was submitted after the parts before it, and a present
    needs none), and `device_wait_idle_locked` under `retire_swapchain`, whose idle wait does not cover a part handed over
    after its caller took the lock (it is submitted once the lock goes, as the render thread's own submit waited for the
    lock before). `chain_waiters` is one count for every queue lock, not per chain: a holder of `present_queue_mu` may
    give up a wait while the `present_mu` holder waits for `presenter_queue_mu` instead (logged once, counted).
- **What still drains.** On the render thread, every other boundary call still waits for the recorder to run all it
  was handed: a command buffer's or pool's begin, reset or destruction outside a slot's own (the present's copy,
  Dear ImGui, NGX, FSR, XeSS: `B_DIRECT`), any `vkDestroy*` and `vmaDestroy*`, a sparse bind, a present, an idle
  wait, a call not packed (a fallback), a packet too big for the ring, and the recorder's stop at the device's
  destruction. `replay_mu`'s release publishes and does not wait: the next holder produces after it in the one ring,
  so a thread without the lock waits, at its boundary calls and recorded calls, for what was published before them
  (`foreign_wait`).

## Where the threads run

- **The render thread** is pinned to the core the host keeps from the game's threads (`host/hos_thread.c`, on machines
  with 8 or more logical processors; on a hybrid CPU the last core of the fastest kind; `HOS_RESERVE_CORE=0`: none).
- **The presenter** is not pinned: it spends its time waiting on a fence.

## The presenter thread

On every PC since 2026-11-19 (`RX_VK_PRESENT_THREAD` is gone). With NVIDIA's driver on Linux `vkQueuePresentKHR` waits
on the CPU until the frame's copy into the window is done on the GPU, on the frame's critical path: ~2.4 ms a frame on an
RTX 3090 in Kokiri Forest on Wayland (architecture.md, The frame pipeline), 4.5 ms of the copy's fence in [the frame
pipeline](architecture.md#the-frame-pipeline)'s measurement. The presenter waits for the copy's fence itself, then presents on the presents' own queue:
the replay's queue under `dev.present_mu`, or the Present queue row's queue under its own lock (`RX_VK_PRESENT_QUEUE`).
Elsewhere the call returns at once and the thread costs a hand-off.

- No second queue. The presenter once had a queue of its own (a second queue of the replay's family, else a compute
  family's); on the GPU it took turns with the replay, and presents came 2 to 4 ms later ([architecture.md, Frame generation's presents](architecture.md#frame-generations-presents)).
- One present out at most: the next frame's acquire waits until it is made, so the swapchain is one thread's at a time;
  under V-Sync On (FIFO) with `VK_KHR_present_wait`, until it is shown. `VK_KHR_present_id` and `VK_KHR_present_wait`
  are enabled wherever the driver has them.
- With NVIDIA Reflex its present id goes with the present, and the presenter marks the present's end.
- Frame interpolation's presents are made by its display thread, as before.
- **Acquire ahead** (on by default, `RX_VK_ACQUIRE_AHEAD=0` turns it off): after a game frame's present that came back `VK_SUCCESS`, the presenter asks for the next present's image at once (a timeout of 0), while the swapchain is still its own. With AMD's Windows driver `vkAcquireNextImageKHR` returns at once and the wait is in the acquire's fence, which the system signals at its next composition: in a window the render thread waited 2.3 to 2.7 ms a frame for it at its present (RX 9070 XT, a 1280x720 window on a 144 Hz desktop). Asked for a present ahead, the image is nearly always free when the next present comes. Drivers that wait in the acquire itself (NVIDIA's, Mesa's) answer the zero-timeout acquire at once, with an image or with none, so the presenter never waits there. The present still waits up to 250 ms for an image that is not, then holds it (#1376); a swapchain made again lets it go. An image held for 1 s is taken as never coming back and the present makes the swapchain again itself (then after 2 s, then every 4 s, while none comes back; `vk: the window's image was not given back in ... s: the swapchain made again`): the system lets an image go when another is shown, and while one is held nothing else is presented, so with frame generation on AMD's Windows driver one held after Alt+Tab, or after a hitch in full screen, froze the picture until F11 (#1482). Not under frame generation or FIFO, and not for the start-up screen. The log's first present says which way it went (`vk: acquire ahead: ...`).

## A C++ exception out of a thread

Nothing above the renderer catches a C++ exception: the host is C, and a thread's body is the top of its stack. One that
got out ended the game with exception `0x20474343` (llvm-mingw's code for a C++ exception nothing caught) and "An
unexpected error", with no word of what was thrown (#1280, #947). Now (`gpu/vk/cxx_escape.h`):

- **Every function the renderer gives the host** (`MkwVkRenderer`, wrapped by `gpu/vk/renderer_guard.h` when
  `mkwvk_init` returns the table) and `mkwvk_init` itself catch a `std::exception`.
- **The render thread, the presenter and the order watch** (`sync_watch`) are started with `spawn_needed`: their bodies
  catch one, and a thread that cannot be started stops the game as out of memory.
- **The texture decode's helper threads** (the gather of a big texture's pages, the check for holes, the unswizzle) go
  through `fork_join`: a helper that cannot be started (at the commit limit a thread's stack has no commit) leaves its part
  to the thread that asked, which makes it after its own.

What is caught is said in the log, `vk: a C++ exception came out of <where>: <type>: <what()>`, then the game stops:
`std::bad_alloc`, or a `std::system_error` of no memory or a thread that could not be started, through the host's
out-of-memory line and notice (as the renderer's `new_handler` does at the commit limit, #1291); anything else with
the renderer's `vk: FATAL:` line (exit code 3). Only `std::exception` is caught, never `catch (...)`: an access
violation or a driver's own exception (`0xe06d7363`) stays the crash record's, with its backtrace. The recorder thread
and the background workers catch their own (`vk_recorder.h` `Recorder::main`, `vk_jobs.cpp` `run_guarded`). Game-free
test: `tests/test_cxx_escape.py`.

## Measuring

The log lines, in the order they come:

- `vk: Threaded rendering: ...` at the start: the row, Auto's tier and why, and the cores and logical processors it
  counted, but for a run where the mode is not safe (`Auto: on, the render thread and the recorder thread (4 cores of 8
  logical processors)`, `Auto: the recorder thread only (8 cores of 16 logical processors; the render thread off: the
  buffer pools have no mirrors)`, `Auto: off (2 cores of 4 logical processors; ...)`, `Auto: off (not safe on this run: ...)`).
- `vk: presenter thread: each present made on a thread of its own once its copy is done`, and `vk: the presenter thread
  waits for its presents to be shown: yes (VK_KHR_present_wait)` or `no`. Every 600 presents: `vk: presenter thread: the
  copy's fence X ms, the present Y ms`, with how long after it the frame was shown under FIFO.
- `vk: a submission's parts ended: X where the game had ended its render pass, Y at 512 draws` every 20,000 parts: a
  part ends at the first place the game has ended its render pass once it is `RX_VK_PART_MIN` (120) draws and dispatches
  long (a submission's first part `RX_VK_PART_FIRST`, 32, so the GPU starts sooner), and inside a pass only at
  `RX_VK_PART_MAX` (512).
- `vk: a frame: X full barriers recorded, Y left out (nothing recorded since the one before), Z pass starts ordered in
  their fragment stages only (draw passes after draw passes)` every 120 presents: a draw pass that came after only draw
  passes since the last full barrier on its command buffer waits for them in its fragment stages and its geometry starts
  at once; after a dispatch, copy, fill, blit, image clear or upscaler library's work, a full barrier.
- `vk: readbacks read without their wait: the culling camera block (0x3980) by default (...)` at the first readback.
- `vk: recorder: N packets run in 10.0 s (...); B batches over F frames (X a frame, Y packets a batch), P parts ended
  and submitted by it; the render thread (the replay lock's holder) waited for it W times, T ms (... a frame); every
  thread ...; <the six it waited longest at>` every 10 s while the recorder runs (`RX_VK_RECORDER_STATS=0`: none). With
  `RX_VK_RECORDER_PARTS=0` the line says so, and the render thread's waits include every part's end and submit.
  With the hand-off by part a second line splits the recorder's busy time: in parts' ends and submits, of that blocked on
  the queue's lock (a present holding it; how often, the longest), the rest in the ends, submits and holds, the time
  running the other packets, and the packets a frame. Waits for the recorder sleep after a short spin in this mode.

To compare against the old behaviour in one build, each change has its switch: `RX_VK_PART_FIRST=0 RX_VK_PART_MIN=240
RX_VK_PART_MAX=240` (a part every 240 draws, wherever it falls, as before), `RX_VK_RECORDER_PARTS=0` (the render thread
waits for the recorder at each part's end and submit), `RX_VK_PASS_BARRIER=full` (every draw pass
starts with a full barrier), `RX_VK_READBACK_LATE=0` (every readback waits), `RX_VK_INDEX_REBASE=0` (an indexed draw's
index buffer bound at the draw's own offset, a bind for nearly every draw). `RX_VK_CALL_COUNT=1` logs the replay's
Vulkan calls by kind, a frame and a draw. `RX_VK_GPU_TIMELINE=1` writes each recording's submit and GPU start and end,
the jobs, the game's fence waits and the presents to `longshot_gpu_timeline.csv`. In a Tracy capture
([profiling.md](profiling.md#tracy)) the render thread is `mkw_vk replay` and the presenter is `mkw_vk presenter`. Runs
and their write-up follow the measure skill (`.claude/skills/measure/SKILL.md`).
