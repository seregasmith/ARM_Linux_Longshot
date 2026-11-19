# The memory check (`RX_VK_MEM_CHECK=1`)

A diagnostic for the renderer's memory sync, off by default. It changes nothing in what the renderer does and never
stops the game: it counts breaks of the rules the sync rests on, logs the first eight of each kind, and writes one
summary line every 600 submits. Unset, each of its hooks costs one test of a flag.

Set it in `longshot_env.txt` or the environment, play the area you are looking at, and search `longshot_log.txt` for
`vk: mem check`.

## Why it exists

The renderer keeps several copies of the game's memory: the pages captured at each submit, the mirrors in VRAM, the
shadows the write-back compares against. Which copy of a page is the newest is spread over several tables, and the
rules that keep them agreeing are written as comments beside the code that relies on them
([design notes, rule 2](../design.md#4-the-lessons-from-mkw-as-rules-here) is the only one in the docs). Most of the
flashes and stale frames fixed so far were two of those tables disagreeing. The check states six of the rules as
counters, so a run says which one broke and on which page, instead of a picture that is wrong for a frame.

## What it counts

| In the log | The rule | What a count means |
|---|---|---|
| `take_open` | A submission's capture is taken only between recordings | A capture was taken while a recording was open. The take stamps CPU pages with a new generation, and the recording's end marks every pool it synced as current: those pages are then taken for synced and may never reach their mirror |
| `collect_open` | The written pages are collected only between recordings | The same, for the collection of the pages no capture took |
| `seq_mismatch` | A submission takes the capture its own submit made | Captures and submissions are paired by their place in a queue. The check numbers each capture; a take that gets another number (or none, or one where none was made) means the pairing slipped, and every submission after it reads another submit's memory. `takes not paired` beside it counts takes that came with no number to expect: a path the check does not cover, not a break |
| `sync_over_gpu` | (not a count of breaks: see below) | A mirror sync copied a capture's page that the GPU wrote after the capture's view of it, the page not late. `replaced` says the sync put words over different ones in the mirror; whether that lost anything the bytes cannot say |
| `sync_over_gpu_only` | (not a count of breaks: see below) | The same counts, for pages a shader wrote through a table pointer or a wide binding: those words are in the mirror alone and are never written back |
| `pages late at the replay` | (not a break) | Pages of a capture that only the replay found late: written back from a mirror by a submission recorded after the game's thread made the capture (`RX_VK_LATE_AT_REPLAY`, on by default). Before that fix these were `sync_over_gpu` candidates |
| `live reads` | Nothing the replay reads comes from live guest memory (design rule 2) | Dirty pages a sync copied from guest memory as it is at the replay, by why no capture had them: `never captured`, `written since their capture` (by the collection or the replay itself), `capture retired` (its buffer was let go), `not copied by their capture` (the capture had no buffer, or was larger than a capture copies) |
| `gen_back`, `gen` | The page generation counter only goes up | The counter is 64 bits (it was 32, and at the 23,000 a second measured in play came round in about two days). `gen` gives its value, how far it moved since the last summary and its speed. `gen_back` counts a step backwards |

### The two sync rules: candidates, then the bytes

Neither count is a count of losses. The records cannot say that a sync lost anything: a written binding covers pages
the GPU never touches. The bytes cannot say it either, and that is the limit of this check.

1. **A candidate**, by the records. For `sync_over_gpu`: the sync copies a capture's page, the GPU wrote the page in a
   submission that was not done when the capture was made, and the page is not late in that capture (a late page waits
   for the write-back or is fixed). For `sync_over_gpu_only`: the GPU wrote the page through a pointer.
2. **The bytes.** The renderer reads the mirror's page back as it is before the sync (a copy into a small buffer,
   recorded ahead of the sync in the same uploads), and when the GPU is done with that recording compares three copies
   of the page: what the last sync of the page copied (prev), what this one copies (cur), and the mirror before it.
   - `replaced`: at least one word where the copy holds what the last sync left (cur equals prev) and the mirror held
     something else. Logged, with the first such word. It is one of two things:
     - **a loss**: the CPU never wrote that word, the GPU did, and the page was copied whole because the CPU wrote
       another word of it;
     - **the CPU's own write of the same value as before**: a counter or a list the GPU fills and the game sets back
       to zero every frame. On the console memory is shared, and a write of zero is a write whatever was there. The
       sync is right to replace the word, and wrong not to.

     A compare of values cannot tell these apart: both read "the same as last time". Only the page's write mark says
     the CPU wrote, and it says which page, not which words. (A sync that copied only the words whose values had
     changed was tried: the game's resets never reached the mirror, the GPU's counters grew,
     and the frame rate halved. It is gone.)
   - `on a page no CPU write made dirty`, beside `replaced`: the part that is certain. The page was synced from live
     guest memory, and the stamp that made it dirty is the renderer's own, for a GPU write: no capture took the page
     and no collection found it written. That copy is not there to bring a reset, so a word it replaced is a loss
     (the one exception: a CPU write still waiting to be collected, whose bytes live memory holds already). A page
     synced from a capture is never in this count: a capture takes the pages whose write mark was set, so the CPU
     wrote it, and which words it wrote is unknown.
   - `same`: no such word. The sync put back what was there, or only words whose values changed. `unbound` beside it
     counts the ones in a sparse mirror's block with no memory: the GPU cannot have written there.
   - `not judged`, by reason: `first sync seen` (no earlier sync of the page to compare with), `read-backs pending`
     (more than 16,384 candidates waiting for the GPU at once), `past the pages followed` (more than 65,536 pages
     followed), `other` (no bytes to copy from, no read-back buffer, no mirror).

What would tell a loss from a reset: a record of which words the CPU wrote, which the page marks do not have (a mark
per word, or the page write-protected between a capture and its sync), or the GPU side's knowledge of the page (a
shader that only ever reads a word after the CPU set it cannot lose it). Short of that, read `replaced` by what the
page is: the logged line names the pool, the writer and the first word.

A page of a sparse mirror (the default on a card with mirrors) is read back like any other when its block has memory.

A `sync_over_gpu` candidate is also counted by how it came to be one, from the write-backs recorded for its page when
it is synced, and the first eight of each path are logged (`sync_over_gpu candidate (...)`) whatever their verdict:

| Path | What it is | What to do with it |
|---|---|---|
| `no write-back` | A written binding covers the page and nothing wrote the page back since the capture's view of the GPU: the GPU did not write this page, or its write-back is left out (`wb_learn.h`), and its words are in the mirror alone | `replaced` here is the same kind as `sync_over_gpu_only`'s: a loss or the CPU's reset |
| `in place` | The newest write-back of the page is one the GPU made in guest memory (no mirror at that binding) | The capture lacks those words. `RX_VK_LATE_AT_REPLAY=2` makes such a page late; it waits at the take |
| `mirror write-back not late` | A mirror wrote the page back since the capture's view, and the page is not late | Should be 0 with `RX_VK_LATE_AT_REPLAY` on: any here is a hole in the fix. With `=0` these are the cases the fix is for |

A word the CPU gave a new value is not counted, whatever the GPU wrote there: the bytes cannot say whose is newer.

A logged case says what the page is, with what the renderer knows: the pool's size and kind (a shadow, written
through pointers, a written binding), whether the game reads the page back (`invalidate`) and waits for it, the
submissions that last wrote it back and how (the diff pass, a whole copy, in place), and for `sync_over_gpu` what
wrote it on the GPU: a storage buffer a program writes, with the program's key (search the log for the key), or a
copy or fill. The shape of a line, with made-up numbers:

```
vk: mem check: sync_over_gpu #1: a sync of page 0x108c6f2000 (pool 0x108c000000) from a capture replaced 6 words with the values the page's last sync had given them (first at +0x40: the mirror had 3f1c2a00, the capture has 00000000); a loss if the CPU did not write those words, its own write of the same value as before if it did (a counter set back to zero): the bytes cannot tell; the page written by a storage buffer the program writes (5b0c9f6e21d4a377) in submission 96300, the capture made with the GPU at 96296 and the page not late; pool of 65536 KB, with a shadow, a written binding; a page the game reads back (invalidate) and waits for; last written back by submission 96300 (diff pass 96300, whole copy 0, in place 0), the GPU now at 96304
```

The summary (made-up numbers again):

```
vk: mem check: 600 submits: take_open 0, collect_open 0, seq_mismatch 0 (0 of 600 takes not paired); sync_over_gpu: 0 replaced (a loss or the CPU's reset; 0 on a page no CPU write made dirty), 2 same (0 unbound), 1 not judged (of 3 candidates; not judged: 1 first sync seen, 0 read-backs pending, 0 past the pages followed, 0 other; candidates by path: 3 no write-back, 0 in place, 0 mirror write-back not late); sync_over_gpu_only: 12 replaced (a loss or the CPU's reset; 1 on a page no CPU write made dirty), 240 same (31 unbound), 20 not judged (of 272 candidates; not judged: 20 first sync seen, 0 read-backs pending, 0 past the pages followed, 0 other); 72400 pages synced, 85 pages late at the replay; gen_back 0; live reads: 0 never captured, 0 written since their capture, 224 capture retired, 0 not copied by their capture; gen 2210344 (+13810, 23016 a second); 5120 pages tracked, 310 followed, 14 large ranges (0 table resets, 0 ranges too large to note)
```

The counts are totals since the start, not per 600 submits.

### Reading a run after the late pages' fix

With `RX_VK_LATE_AT_REPLAY` on (the default), a page a mirror's write-back wrote after the capture's view of the GPU
is late by the time the check hears of the take, so it is not a candidate: `pages late at the replay` rises where
`sync_over_gpu` candidates did before. What is left as a `sync_over_gpu` candidate is a page the GPU's binding covered
that no mirror's write-back wrote since the capture (a write-back left out, or a write in place); `replaced` there is
a loss or the CPU's reset, as above. `RX_VK_LATE_AT_REPLAY=0` gives the count before the fix, for an A/B.

## What it costs

Unset: one test of a flag at each hook. Set, a few per cent of the frame rate (about 7% in Kokiri Forest on an RTX
3090). Where it goes: a lock and a hash lookup for every dirty page a mirror sync copies (some 130 a submit); a record
written for every page of every binding the GPU writes, at every draw and dispatch that binds one; two or three
record updates for every page of every capture; and for a candidate, 8 KB copied on the CPU, a 4 KB copy on the GPU and
its compare.

## What it does not see

- A GPU write of more than 64 MB in one binding is kept as a range, not page by page (`large ranges`): every page
  of it is a candidate as if written. Past 4,096 distinct ranges the rest are dropped (`ranges too large to note`).
- A sparse mirror's block that was given memory again after a release has lost what the GPU wrote there before: the
  read-back then compares fresh memory, and a `replaced` on such a page is the release's loss, not the sync's.
- A candidate's verdict compares with the last sync of the page the check saw. The check follows a page from its first
  time as a candidate; a word the CPU changed and changed back between two syncs it compared reads as left alone.
- A page fixed on the GPU (a late page) is compared as its capture holds it on the CPU side, before the fix.
- With `RX_VK_CAPTURE_WAIT=0` the GPU's progress at a capture is not recorded, and every capture's page the GPU ever
  wrote is a candidate.
- The host's writes with no mark are not checked here. `tests/test_host_guest_writes.py` holds the host's bulk writes
  to the rule by their text; a run-time audit (a hash of clean pages between collections) is not written yet.
- It keeps a record a page for the pages a capture took or the GPU wrote, up to about two million; past that the
  records are dropped and counted (`table resets`), and the counts that need them start again.

## Where it is

`gpu/vk/mem_check.h` holds the rules and the counters, with no Vulkan in it; `tests/test_mem_check.py` drives it with
made-up events, one sequence per rule. The hooks are in `gpu/vk/vk_memory.cpp` (`memory_capture_submit`,
`memory_take_capture`, `memory_capture_now`, `memory_stamp_gpu`, `memory_note_gpu_write`, `mirror_sync` with
`mem_check_sync` and `mem_check_probes_resolve` for the read-back) and `gpu/vk/vk_replay.cpp` (`submit_q` and the job
that runs a submit, which carry a capture's number from the submit to its take; `submit_memory_work`, once a
submission). `gpu/vk/vk_draw.cpp` says which program writes a storage buffer (`memory_check_writer`).
