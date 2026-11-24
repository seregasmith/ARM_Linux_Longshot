# Shader cache dump and restore

## Export and restore support

The first implementation exports from a completed Turnip preparation that records compatibility
metadata and uses the install-local Mesa single-file cache (`MESA_DISK_CACHE_SINGLE_FILE=1`). Open Setup
with the game closed and choose **Dump Shaders**. The CLI equivalent is
`Longshot.AppImage --dump-shaders <game folder> [destination]`. Export uses the SPIR-V store recorded
at preparation time. For older installs, Setup can recover the driver identity from a completed game
log when the driver, Mesa cache UUID, and shader seed match the existing preparation stamp. Export
does this offline, without building or compiling shaders. Setup logs export diagnostics in
`Longshot-logs/setup-window.log` next to the AppImage by default.

Select the ZIP in Setup before building, or choose **Restore shader dump** for a game executable
that supports restore. A ZIP can come from a different Setup AppImage. The producer version in the
manifest is informational. The destination game needs restore support and a compatible shader seed,
renderer stamp, GPU, and driver. The CLI equivalents are `Longshot.AppImage --cli --shader-dump <dump.zip>`
with the usual build arguments, and `Longshot.AppImage --restore-shaders <game folder> <dump.zip>`.
Restore verifies the archive and shader seed. When the ZIP and the game executable both contain a
shader translator stamp, Setup checks that they match before it activates the cache. It preserves
the original cache and any previously restored cache. At launch, Longshot checks the GPU, exact
driver, cache UUID, and the full shader cache stamp. If the renderer changed after you made the ZIP,
use a ZIP from a compatible build or prepare shaders once for the new build and make a new ZIP.
Longshot skips bulk startup preparation with a compatible restore. Compilation on a missing shader
or pipeline during play is still possible. Strict cache-only launch remains a separate step.

The updated game executable reads **Robust buffers** from dumps that carry it and applies it at
launch while the restored cache is active. Longshot leaves the saved player setting in place.
Older executables and ZIPs use the installation's setting. Match the source installation if the
shader stamp check fails after restoring an older ZIP.

## Setup workflow

1. Open the Linux Setup AppImage, select the `.nup`, and build. Setup prepares shaders normally and
   records which Vulkan device and driver did the work.
2. Open Setup and select an existing Longshot installation with a completed shader cache. With the
   game closed, choose **Dump Shaders**. This action only reads the existing cache. It needs no `.nup`
   selection, game build, or shader compilation. Setup saves one file next to the actual AppImage,
   for example `mesa-27.0.0-devel-231126_131544.zip`. The Mesa version comes from the process that
   prepared the cache.
3. Play normally. An export is a snapshot, so gameplay can extend the live cache independently. To
   export again, close the game first.
4. To reinstall, open Setup and select both the `.nup` and the file under **Shader dump (.zip, optional)**.
   Setup builds the game, installs the cache, and skips shader preparation. For an existing compatible
   installation, choose **Restore shader dump** to restore without building.
5. Select **Add to Steam** to make a shortcut. Setup checks the installed Turnip drivers for a match
   with the restored cache or the last completed shader preparation. If it finds a match, it sets the
   driver for this shortcut. If Setup finds no match, select the correct driver in Armada Control.
6. Start Longshot. With a restored cache, Longshot checks the driver before it prepares shaders. If the
   driver differs, Longshot shows both drivers and exits. After you restore a cache for a different
   driver, select **Add to Steam** again to update the shortcut.

## File format

Name the file `mesa-<version>-<timestamp>.zip`. Use the operating system's local timezone and the
24-hour format `DDMMYY_HHMMSS`. For example, 23 November 2026 at 13:15:44 becomes `231126_131544`.
If that name already exists, append the first available suffix from `_1` through `_255` before
`.zip`, as in `mesa-27.0.0-devel-231126_131544_1.zip` and `mesa-27.0.0-devel-231126_131544_2.zip`.
If all suffixes are taken, report the collision and let the user retry. Preserve every existing dump.

Use a **ZIP64-capable `.zip`** with a root `manifest.json`. The Odin 3 snapshot already holds several
GiB of cache before adding the SPIR-V store. Python's standard `zipfile` handles large archives, and an
ordinary ZIP reader can open the manifest. Use `ZIP_STORED` and stream files in v1 to avoid compression
time during export. If the AppImage directory is read-only, lacks space, or is on a filesystem with a
smaller per-file limit, show the reason and let the user choose another directory on the same PC.
[Python ZIP documentation](https://docs.python.org/3/library/zipfile.html)

| Archive entry | Contents |
|---|---|
| `manifest.json` | Format ID and schema version, creation time, Longshot version, shader seed hash, renderer shader cache stamp, program cache format and store ID, Vulkan vendor and device IDs, driver ID and exact version, driver display name, `pipelineCacheUUID`, relevant shader settings, Mesa cache layout, and an inventory of member paths, byte sizes, and SHA-256 hashes. |
| `driver/mesa_shader_cache_sf/` | The install's Mesa single-file cache tree as written, including each hash directory and its `foz_cache.foz`, `foz_cache_idx.foz`, and `index`. Preserve the directory names. |
| `longshot/` | The program cache and its index, pipeline notes and preparation stamps, learned compute variants and state lists, and optional compatible Vulkan pipeline cache files and their sidecars when present. Take the allowlist from the persisted shader caches in [the pack tooling](../scripts/devtools/packs.py). |
| `spirv/spirv.pack` and `spirv/spirv.idx` | The matching SPIR-V store pair, copied together when the program cache references it. Preserve its ID and offsets. |

Start with Turnip and Mesa's `mesa_shader_cache_sf` layout on the Linux Setup AppImage. Keep the
complete install-local Mesa tree in v1. Directory size, modification time, and the Mesa version in the
archive name are insufficient to select a driver namespace. A fresh installation builds
`longshot_shaders.bin` from the `.nup` and checks its content against the manifest. The archive carries
neither that seed nor saves, settings, logs, executables, or lock files. Shader dumps contain
game-derived data and stay on the original PC. [Project data
rules](../CONTRIBUTING.md#the-rule-with-no-exceptions)

## Implementation

1. **Record the preparation result.** Have the renderer write the manifest's compatibility fields after
   successful shader preparation. Record the device the renderer actually selected, along with the
   effective cache locations and layout. Use an install-local SPIR-V store for new Setup builds so a
   dump can include the complete matching store without copying other installs' data. For an older
   install using the shared store, require the matching `spirv.pack` and `spirv.idx` and take a
   consistent copy under the store's lock. An install with missing files or incomplete preparation
   cannot export a restorable dump. [Shader cache architecture](dev/reference/architecture.md)
2. **Add export and selection to Setup.** In the [Setup UI](../builder/linux/setup_ui/setup_ui.cpp),
   offer **Dump Shaders** for the selected install independently of `.nup` selection. If cache files
   or preparation metadata are missing, explain what's missing. On the main Setup screen, put the
   **Shader dump (.zip, optional)** field directly below **The game folder** and above the Build and
   Play buttons. **Choose ZIP** opens the file picker in the AppImage's directory and lets the user
   browse elsewhere. Show the selected filename and expected Mesa driver, with a **Clear** action.
   Show **Restore shader dump** when a ZIP and an existing compatible installation are selected.
   Restore extracts cache files under the selected game folder's `cache/` and keeps the ZIP at its
   original location. Put archive handling in a Python helper used by the UI and Builder. Hold an
   install lock while exporting
   or restoring, and require the game and shader preparers to be stopped. Export to a temporary file
   beside the target, then publish the ZIP only after every file has been written and the archive has
   closed successfully. Publish without replacing an existing file and try the next suffix if another
   export claimed the same name.
3. **Stage and activate the restore.** In the [Builder](../nx2/make_game.py), validate the schema and
   file inventory early, then check the target's shader seed and renderer stamp before activation.
   Accept only declared shader cache files and stream them into a new directory under the game's
   `cache/`. Reject duplicate or unsafe paths, links, corrupt data, and sizes that exceed the inventory
   or available space. Atomically update one install-local record that selects the restored cache.
   Keep the previous cache usable until that record is committed. On failure
   or cancellation, report the issue and leave the previous selection usable. Report **Shader dump
   restored** in place of the preparation step only after the restore succeeds. A failed restore never
   falls back to compilation without the user explicitly choosing a normal build.
4. **Enforce cache-only mode in Longshot.** Read the installed selection before Vulkan initializes so
   Setup, Steam, and direct launches use the same restored Mesa and SPIR-V stores. Compare the real
   selected device, driver identity, `pipelineCacheUUID`, and shader settings before preparing or
   creating pipelines. Setup and Play can load different drivers, so the game process makes the final
   driver check. In cache-only mode, block shader translation and use
   `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` on pipeline creation paths that could
   compile, including initialization. If the driver cannot enforce this behavior, report it before
   compilation. Report a required cache miss separately with the pipeline and a choice to prepare
   shaders explicitly or restore a dump with better coverage. Matching drivers can still miss pipelines
   for game areas or settings the dump has never covered. [Vulkan pipeline creation
   flags](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreateFlagBits.html)

## Acceptance checks

- Reopen Setup with the game closed, select an existing prepared installation, and export with the
  `.nup` field empty. Verify that no build or shader preparation process starts.
- Open the ZIP picker from the main screen, select a dump next to the AppImage or in another folder,
  and verify the selected filename and expected driver. Clear the selection and verify that Setup
  returns to the normal build flow.
- Export a prepared install and restore it from the single ZIP into a fresh build using the same `.nup`.
  Verify matching program, SPIR-V, and Mesa cache files, the skipped preparation step, and a cache-only
  launch to the title. Repeat from Setup and Steam, then load a known save and verify zero Longshot
  shader translations and pipeline compilations for that run.
- Launch with a different Turnip build, including one reporting the same Mesa version but a different
  cache UUID. Verify the expected and detected drivers are shown and the compiler is never called.
  Switch back and relaunch without restoring again.
- Exercise a missing pipeline on the correct driver. Verify the cache-miss error and zero compilation.
  Refuse a dump with a mismatched shader seed, renderer stamp, or SPIR-V store ID.
- Use game-free fixtures for malformed archives, missing files, truncation, unsafe paths, and
  interrupted restores. Verify an existing install can restore without changing saves or settings and
  retains its previous cache selection when restore fails.
- Test filename generation with the OS set to a non-UTC timezone. Check the unsuffixed name, ascending
  collision suffixes through `_255`, and the error when all names are taken.
- On the target handheld, measure export size and time, restore time, and first-launch time against a
  normal cold install and a warm launch. Include a ZIP larger than 4 GiB and verify the output-path
  error on a destination that cannot hold it.
