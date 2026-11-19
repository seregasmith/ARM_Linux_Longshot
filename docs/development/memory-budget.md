# The renderer's memory budget

The Vulkan renderer holds memory in about twenty pools: the texture cache, the render target pool, the VRAM mirrors of the
game's memory, the staging arenas, the captures, and more. Each grew a limit and a log line of its own. `gpu/vk/mem_account.h`
is where they meet: every pool reports what it holds, one log line sums them beside what the driver counts, and one arbiter
asks the pools for memory back when the card is nearly full. `tests/test_mem_account.py` checks it without a game.

## The budget line

Every 600 presents the log has the renderer's memory lines: `vk: memory:` (the process's working set and private memory, the
staging arenas), `vk: memory heaps:` and `vk: image blocks` (what the allocator holds in each heap and image pool),
`vk: render target images:`, `vk: VRAM:` (the driver's count and allowance, the textures and their budget) and then:

```
vk: budget: VRAM <sum> of <allowed> MB (<used> MB by the driver, <unaccounted> MB unaccounted): <pool> <MB>, ...; RAM <sum> MB (the process's private memory <private> MB): <pool> <MB>, ...
```

The pools are listed largest first, those under 1 MB together as `others`. VRAM is what the driver counts for the device (on
an integrated GPU with one heap, everything); RAM is system memory. The process's private memory is printed beside the RAM
sum, not as a whole it is part of: on Linux the guest memory behind `imports` is shared memory, outside that figure.
`scripts/memtour.py` writes the line's figures into `mem.csv`, a column for each pool.

| Pool | What it is | Where the figure comes from |
|---|---|---|
| `textures`, `sparse textures`, `textures in RAM` | The texture cache: images in the allocator's blocks, sparse images' own device memory, and images that went to system memory because VRAM was full | Each texture's `bytes + pad`, walked when the line is made |
| `target pool` | Dead render targets' images kept for the next target of their shape | `pool_bytes` |
| `dead images`, `dead buffers` | Images and buffers let go that wait for the GPU | The dead list, by each allocation's size |
| `other images` | Every image the renderer made that is no texture's: the upscalers' and frame generation's inputs and history, the scratch images, the menu's | A count of every image made (`vk_vma.cpp`), less the ones above |
| `blocks free` | Free space inside the allocator's blocks, in every pool of blocks: the driver counts a block whole | The allocator's block bytes less its allocations |
| `mirrors`, `shadows` | VRAM copies of the game's memory pools, and the write-back's second copy | `mirror_bytes`, `shadow_bytes`, as of the last submission |
| `staging` | The texture uploads' arenas (VRAM behind a resizable BAR, else RAM) | The arenas' sizes |
| `captures`, `capture spares`, `capture staging spares`, `capture table` | The submit-time copies of guest pages, the spare buffers and blocks kept for the next ones, and the table of pages | Their own counters |
| `other buffers` | Every buffer the renderer made that no pool above reports: the recordings' streams (`vk: the recording streams:` has their size), the guest page table (24 MB), the mesh rings and the mesh fallback's buffers, the stamp blocks, a mirror or a shadow given back until the GPU is past it, a capture being taken | A count of every buffer and sparse block made (`vk_vma.cpp`), less the ones above |
| `imports`, `retired imports`, `retired records` | Guest memory the driver has pinned for the GPU, live and retired, and the retired pools' records | Summed at each submission |

A pool's bytes are placed by where its memory is: an image or a buffer the allocator put in a system heap is in the RAM list
under the same name.

### What `unaccounted` is

`unaccounted` is the driver's count less the VRAM sum. Every image, buffer and sparse block the renderer makes through its
allocator is in a pool, and `blocks free` is the rest of the allocator's blocks, so the sum is what the allocator holds for
the device plus the sparse textures' own memory. What is left is memory the renderer did not allocate itself:

- what the driver charges the process on its own: the swapchain's images, pipelines and shader code it keeps on the device,
  descriptor pools, command buffers, its own bookkeeping;
- what the upscaler libraries allocate inside themselves (DLSS, FSR, XeSS and their frame generation): their networks,
  histories and scratch images are made by the library, not by `create_image`;
- two small buffers made beside the allocator (the checkpoint markers, a matrix-extension probe);
- any pool's counting error. A texture is counted as the budget counts it (at least its allocation), and the pools' figures
  are taken at slightly different times, so a few tens of MB either way are noise.

It is below zero when the pools count more than the driver does, and `unaccounted n/a` where the driver gives no count of
its own (no `VK_EXT_memory_budget`). To split it further, compare `vk: memory heaps:` on the same present: for the device's
heap, `used` less the heap's `blocks` MB is what was not allocated through the allocator at all.

On an RTX 3090 at 2560 x 1440 the line left 426 MB unaccounted in Kokiri Forest and 427 MB in Hyrule Field before
`other buffers` existed. How much of that the renderer's own uncounted buffers were is what the next run's `other buffers`
says; the rest is the list above.

## The arbiter

Only the texture cache answered every sign that memory was short; the target pool had a rule of its own for the same lines,
and the staging arenas and the capture spares answered Linux's memory pressure but nothing of the card's. Now one look at
the lines, at every eviction pass (10 presents), serves every pool.

**The signal** is the texture cache's own, unchanged: `pressure` is the driver's count less the free space the textures can
still take in their blocks.

| Level | Line | Where it came from |
|---|---|---|
| 1, near the allowance | `pressure` past 90% of what the driver allows | The texture budget's "90% rule", and the target pool's "halve" |
| 2, at the driver's line | the count itself past 97% of the allowance | The target pool's "flush"; past there the driver pages VRAM out |

Below level 1 nobody is asked, and a frame is what it was before the arbiter: two comparisons more in the pass.

**The order.** At level 1 or 2 the pools that keep memory only to use it again are asked, each for what the ones before left
of the excess over the line, until it is covered:

| Order | Pool | What it does when asked | When |
|---|---|---|---|
| 1 | `capture spares` | `cap_free_trim` down to 32 MB, what Linux's memory pressure leaves them | Once as the pressure begins, and only where the spares are the device's memory (an integrated GPU's one heap) |
| 2 | `staging` | Every idle arena goes at the next present, as when Linux's memory pressure begins | Once as the pressure begins, and only the arenas that are VRAM |
| 3 | `blocks free` | The compaction looks at the next present instead of after its rest, and runs if there is enough to return | Once as the pressure begins; its own lines (`vk: VRAM compaction ...`) say what came back |
| 4 | `target pool` | `pool_trim` at the level's bound, now instead of at the next present: half the pool's cap at level 1, none of it at level 2 | Every pass at level 1 or 2 where the pools before it did not cover the excess (the pool's own trim, every present, keeps the same bound either way) |

"As the pressure begins" is the level rising past the highest it reached since memory was last calm, and calm is 60 looks
(600 presents) in a row at level 0: a count that sways across a line asks the first three once, not at every crossing. A
staging arena or a spare buffer taken away is made again at the next load, 9 to 108 ms of `vkAllocateMemory` each.

**Before the textures.** What the pools let go of in the pass comes off the excess the 90% rule takes from the texture
cache, so a texture the game is using stays while a spare buffer or a pooled image can go in its place. It does not come
off the 97% rule's excess, nor off the compaction's hold: at the driver's own line the textures are squeezed as before,
whatever the pools gave.

Each pool that gave is logged, the lot at most once a second:

```
vk: budget: gave back <MB> MB from <pool> at <P>% of the allowance
```

The mirrors, the shadows, the imports and the captures in use are not asked: they have no trim that is safe to call from
here, and the mirrors keep their own budget by the card (`gpu/vk/mirror_budget.h`). Giving them a place in the order is
the next step.
