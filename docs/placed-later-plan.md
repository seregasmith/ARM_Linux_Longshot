# The big pools in ordinary memory until the GPU binds them (arm64 port, AYN Odin 3)

Status: built 2026-11-20 (not yet run on the device), from the `RX_VK_LOG_BIG_POOLS=1` run of the same day (Kokiri
Forest, Turnip 26.2.6, 5 minutes).

## Why

Where the driver takes neither host pointers nor dma-bufs of the guest window (Turnip on Armada OS: no udmabuf), every
pool the game makes is placed in the device's own memory when it is made (`placed_back`, `docs/arm64-port-plan.md`). The
measurement run showed what that costs:

- A placement takes its whole size in RAM when its memory is allocated, before anything is copied in: the 2432 MB pool
  took MemAvailable from 6976 to 4557 MB with 8 KB in it, and every other big pool did the same. Those pages are the
  driver's: never swapped, never compressed.
- Of the 4276 MB in the ten pools of 64 MB or more, the GPU's bindings (draws, dispatches, copies, texel buffers) reached
  three in 5 minutes: 512 MB (4 MB of it), 149 MB and 88 MB (12 MB of it). The other seven, 3525 MB, were never bound:

  | Pool | Size | Data after 5 min | What the GPU did with it |
  |---|---|---|---|
  | 0x109c1b0000 | 2432 MB | 1726 MB, all in its first 1735 MB | two large textures unswizzled on the GPU from it (41 MB) |
  | 0x1147c16000 | 288 MB | none | nothing |
  | 0x7feb7c6000 | 244 MB | all of it | nothing |
  | 0x7fc3075000 | 232 MB | all of it | nothing |
  | 0x11685e0000 | 201 MB | 79 MB, growing | nothing |
  | 0x113755c000, 0x113cba9000 | 64 MB each | none | nothing |

  The 2432 MB pool holds the game's textures (the slow uploads name textures inside it), which are uploaded into Vulkan
  images from the CPU: the pool itself is only ever a source for the CPU.
- The run sat at 150-300 MB of MemAvailable, with about 4 GB more in swap during play.

## What changes

1. **A pool of `RX_VK_PLACED_LATER_MB` (64) or more that runs into no placement is left in ordinary memory when the game
   makes it** (`placed_later`, `placed::Later`: by start, each pool's own end, so that letting go of one takes off that
   one alone). Ordinary memory is the guest window's own: pages are taken as they are first written, and the kernel can
   swap and compress them. A pool made inside a placement is counted there, and one that runs into placements is placed
   with them, as before.
2. **None of them ever runs into a placement** (`placed::made` decides what a pool made becomes). A pool the game makes
   that runs into some of them and into no placement (a smaller one inside one of them, say) is left in ordinary memory
   with them, whatever its size, and placed with them at a binding of any of them. One that runs into them and into a
   placement too places them first (its writes held back as below), then is placed with them. So placing them later takes
   in no placement: none the GPU may be writing (a placement's writes still under way would land in memory no longer under
   the pool), and no import to retire in the middle of a lookup. A pool whose placement is refused is remembered
   (`placed_refused`), so that letting it go takes nothing off a placement made over it since.
3. **Its first GPU binding places it** (`placed_find`, from `pool_lookup`): the pools left in ordinary memory over the bound
   range, with every other such pool they run into, become one placement. The binding then gets its import from that
   placement as any other. Refused (the device's memory), they stay as they were and are tried again 30 s on.
4. **A large texture in such a pool is unswizzled on the CPU** (`upload_unswizzle_gpu` asks `memory_placed_later` first):
   the GPU path would read the pool through a binding and place it, 2.4 GB for a couple of textures. The CPU path is the one
   most textures take already.
5. **A pool the game lets go while still in ordinary memory** is only forgotten: nothing to copy back.
6. **Not with `RX_VK_PLACED=1`** where host pointers work (the test of this path on a PC): a binding refused there would be
   imported from ordinary memory, and a placement made later would leave that import on the old pages.

## The hard part: placing a pool the game is writing

When a pool is placed at its creation nothing has written it yet. At its first binding the game may be writing it from
any thread, and a write that lands in the old pages after they were copied would be lost when the device's memory is
mapped over them. So the placement holds writes back while it copies (`placed::Barrier`, `placed::place_live`):

1. The range is published as being placed, then made read-only (mprotect).
2. Its contents are copied into the new memory (`copy_in`: the pages in memory or in swap).
3. The new memory is mapped over the range (MAP_FIXED, read-write), and the range is marked done.

A thread that writes into the range between 1 and 3 faults. The renderer's vectored exception handler (first in the
host's chain, `host/posix/win32_posix.c`) finds the address in a range being placed, waits until it is done
(async-signal-safe: atomics, the gettid and clock_gettime system calls, nanosleep), and lets the write run again, into the
new memory. A range stays known for 2 s after it is done, for a thread that faulted before the end but is handled after
it; a range being placed is looked for first, so that a write never spins on one placed before over the same pages. A
fault of the placing thread itself is not held: it writes no guest memory there, and would wait for itself. Reads are
not held back. A placement made when a pool is made, that takes in placements the game may be writing, holds writes back
the same way (before, it copied them without).

A write by the kernel on the game's behalf does not fault: it fails with EFAULT. The host's one such write into guest
memory is a file read (`nn_fs.c` through the posix `ReadFile`'s pread). `ReadFile` then tries again every millisecond, for
up to 30 s from its last progress, which waits out the copy.

Nothing that waits can be what the placement waits for: the placing thread holds `pools_mu` and `placed_mu`, and between
1 and 3 it takes no lock, calls neither the driver nor the logger and allocates nothing: it copies and maps.

What it does not hold back: a read that pinned its pages before they were made read-only (O_DIRECT, a FUSE file system's
direct reads) writes the old pages after the copy. The game's files are read through the page cache, which copies at the
read.

## What it should save, and what it costs

- In the measured run: 3525 MB never taken from RAM by the driver at all. Of it, about 1.2 GB never written (the 288 and
  two 64 MB pools, the unwritten 700 MB of the textures' pool, most of the 201 MB one); the rest, about 2.3 GB, written by
  the game but swappable and compressible, where the pool's pages go cold once their textures are uploaded. The driver's
  heap count (`VRAM 7.7 of 8.0 GB` in the run) drops by the same 3.5 GB, which the textures' budget sees as room.
- The 512, 149 and 88 MB pools are placed at their first binding, near the title screen: a one-time pause each while their
  data is copied (tens of MB), and their writers held back for that long.
- A pool left in ordinary memory that is bound later in the game (another area) is placed then, with the same pause for
  its size: for the textures' pool, up to 2.4 GB allocated and 1.7 GB copied, a second or more. The log says each one.

## Switches and logs

- `RX_VK_PLACED_LATER_MB` (tune, 64): the size from which a pool waits for its first binding; `0` places every pool when
  the game makes it, as before.
- `vk: pool ... left in ordinary memory until the GPU binds it` as each such pool is made; `vk: pool ... placed at its first
  GPU binding in N ms (... copied in; the game's writes held back N times so far)` when one is placed;
  `RX_VK_LOG_BIG_POOLS=1` follows the placements as before.

## The test (game-free)

- `placed::Barrier` and `placed::place_live` on a memfd standing in for the dma-buf: four writer threads fill 16 MB, each
  slot once, while it is placed; afterwards every slot holds its value (with the read-only step left out, about 15,000 to
  24,000 of the 2 million writes are lost). Whether a write meets the placement there depends on the scheduler, so a
  second check makes it certain (`placed::hold`, then the rest by hand): a write into the held range faults, waits while
  it is held, and lands in the placement once it is done.
- The bookkeeping of the pools left in ordinary memory: which of them, and which placements, a binding's placement takes in.
- The posix `ReadFile` into a buffer that is read-only for 50 ms: it waits and reads the whole file (it failed before).
- The source: `memory_pool_created` defers, `placed_find` places at the first binding, `upload_unswizzle_gpu` asks
  `memory_placed_later` before the GPU path, the handler is registered before `placed_on`.

## How it is checked on the device

The same walk as the measurement run (boot, Kokiri Forest, 5 minutes) with `RX_VK_LOG_BIG_POOLS=1`, against that run:
MemAvailable and swap, the driver's heap count, the frame rate and the slowest frames, and the new lines: which pools
were left in ordinary memory, which were placed at a binding, how long that took and how many writes it held back.

## Review

A second agent reviewed the change cold. What it found is fixed:

- A placement at a binding over 1 GB that took in an imported placement left both imports in the table (the "left
  overlapping" rule), the binding handed the old one: no pool left in ordinary memory runs into a placement now (2
  above), and `pool_lookup` retires the imports of placements gone at its own lookup, as `pool_of` does before it.
- A placement taking in another the GPU was writing lost those writes: the same.
- Letting go of a pool counted against whatever pool left in ordinary memory held its range, a refused placement's pool
  included: each pool's own end is kept, and only that pool is let go.
- The test with writer threads only sometimes met the placement (27 of 30 failures on one CPU, all of them that check): it
  says how many writes it held back, and a second check makes a write meet it for sure. A `held()` that held every fault
  passed before: the checks of what it does not hold catch it now.
- The placing thread's own fault waited for itself; an older range over the same pages made writers spin; a refused
  read-only step left part of the range read-only; a refused placement was tried again every second, logging each time.

A second look at the fixes found two more, also fixed:

- A pool made while the pools it ran into could not be placed (refused, or waiting 30 s for the retry) was neither placed,
  left in ordinary memory nor counted: letting it go later took it off a placement made over it since, which could give
  back a pool still in use. Such a pool is now left in ordinary memory with them when it runs into no placement (which
  also spares a hitch: before, a small pool made inside the textures' pool placed all 2.4 GB of it as it was made), and
  remembered as refused when it runs into a placement too. `tests/placed_memory_test.cpp` runs `placed::made` and
  `placed::Later` through 40 times 3,000 random pools made, let go and bound, a quarter of the placements refused, and
  checks after each step that none left in ordinary memory runs into a placement and that every live pool is counted once.
- A placement that takes in others holds the game's writes back since this change, but the handler was registered only
  with `RX_VK_PLACED_LATER_MB` set: with `0`, such a write would have crashed. It is registered always.

Left as they are: a pool made across a placement and a pool left in ordinary memory places the latter, then copies it again
into one placement with the first, so for a moment twice its memory; and the host's handler table is changed without a lock that the signal handler takes (`AddVectoredExceptionHandler`,
once, as the renderer starts): a fault at that moment can run a handler twice or skip one.

## Left for later

- The 512 MB pool is placed whole for 20 MB of use: placing pools in parts would need a placement per part.
