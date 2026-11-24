# Help

Find what you see below and follow its steps: each one names the exact message, file or setting to look for. Nothing
fits, or the fix didn't work? [Still stuck?](#still-stuck) says where the log is and how to ask.

- [Installing and building](#installing-and-building)
- [Starting the game](#starting-the-game)
- [In game: graphics](#in-game-graphics)
- [In game: performance and freezes](#in-game-performance-and-freezes)
- [In game: controls, saves and language](#in-game-controls-saves-and-language)
- [Downloads and antivirus](#downloads-and-antivirus)
- [Still stuck?](#still-stuck)

## Installing and building

### The launcher says "This version isn't supported"

Or: "Longshot supports build e0f3ccfa". Longshot builds from one version of the game only.

1. **Check the version.** You need program `0400486024DEC000` with update **v131072** (build `e0f3ccfa`); see
   [the requirements](requirements.md). The game without its update, or with another one, won't build.
2. **Check the files you chose.** Use a `.nup` file, or the `romfs` and `exefs` folders. The launcher looks for `.nup`
   files in your Downloads, Desktop and Documents folders; if the right one isn't listed, point it at the file.

[Search version reports](https://longshot.magicbean.dev/progress/#problem-search?q=version)

### "Your antivirus removed part of the build tools"

A false alarm on unsigned programs: Longshot isn't code-signed yet.

1. **Get the files back.** Restore them from Windows Security's **Protection history**, and add the launcher's folders
   to its exclusions: `%LOCALAPPDATA%\LongshotLauncher` (its data: each version's builder and build tools, in
   `toolchains`, and the build files) and `%LOCALAPPDATA%\Programs\LongshotLauncher` (the launcher itself). The portable
   launcher keeps its data in `LongshotLauncher-data` beside `LongshotLauncher.exe` instead.
2. **Build again.** If a file is still missing, delete that version's folder in
   `%LOCALAPPDATA%\LongshotLauncher\toolchains` (it is named after the version, such as `v3.0.0`): the next build
   downloads it again and checks it.

The [antivirus guide](https://longshot.magicbean.dev/help/antivirus/) shows each step, and how to check first that the
download is genuine.

[Search antivirus reports](https://longshot.magicbean.dev/progress/#problem-search?q=antivirus)

### The build is very slow, or runs out of space

1. **Expect about 15 minutes on a 12-core desktop, and longer on a slower PC.** Most of the time is compiling and
   preparing shaders, which use every CPU thread, so a 6-core PC can take twice as long or more (an estimate, not a
   measurement). A hard drive works, but builds are much slower than on an SSD.
2. **Free about 60 GB** before building. The build's own files (about 4 GB) are deleted once the game is ready.

[Search build reports](https://longshot.magicbean.dev/progress/#problem-search?q=build)

### Linux: the launcher or the build won't start

1. **Let the launcher run as a program.** In your file manager, open its **Properties** and turn on "Allow executing
   file as program", or run `chmod +x LongshotLauncher-*-x86_64.AppImage`.
2. **Check your system.** Linux needs glibc 2.35 or newer (Ubuntu 22.04, Debian 12 or newer). A CPU without AVX2
   builds and plays too: the game and its renderer are built for your processor.

[Search Linux reports](https://longshot.magicbean.dev/progress/#problem-search?q=linux)

## Starting the game

### The game opens and closes, shows a black screen, or the log says "device lost"

1. **Update your graphics driver.** On Linux, update Mesa or the NVIDIA driver.
2. **Check your graphics card.** Longshot needs **Vulkan 1.3**; see [the requirements](requirements.md). A
   driver update can't add it to an older card that doesn't have it.
3. **AMD cards** have known crashes ([#708](https://github.com/NotRexed/Hookshot/issues/708),
   [#614](https://github.com/NotRexed/Hookshot/issues/614)). To try the other drawing path, add the line
   `RX_VK_MESH_SHADERS=compute` to `longshot_env.txt` in the game folder (usually a little slower; delete the line to
   undo it).
4. **Read the end of the log.** `longshot_log.txt` in the game folder's `logs` folder has the last run; its last lines
   usually say what failed.

[Search crash reports](https://longshot.magicbean.dev/progress/#problem-search?q=crash) · [Search black-screen reports](https://longshot.magicbean.dev/progress/#problem-search?q=black+screen)

### Windows, Intel: the game closes in the first scene

The log names `igvk64.dll`, Intel's driver.

1. **Update the graphics driver.**
2. **Keep the Debug tab's Intel shader compiles row on Auto or One at a time.** Side by side builds faster on a CPU with
   many cores, but Intel's driver can crash the game then.
3. **Build with Prepare shaders now ticked.** If the game folder's `cache` folder has a `longshot_prepare_skipped` file,
   delete it first. A shader the driver faulted on is listed in `longshot_driver_faults.txt` there and left out on later
   starts with the same driver; delete that file to try them all again.
4. **On integrated graphics with 4 CPU threads or fewer,** choose **Set up for a weak laptop** on the Debug tab, and leave
   **ASTC as RGBA8** off: its larger textures take memory the graphics share with the rest of the PC.

If it still crashes, [report it](#still-stuck) with a log from a run with `HOS_VIDEO_LOG=1` in `longshot_env.txt`, and
the FPS on the title screen and in the first scene.

[Search Intel reports](https://longshot.magicbean.dev/progress/#problem-search?q=intel)

### Linux: the game doesn't start, or the picture is broken

1. **Check Vulkan and mesh shaders.** `vulkaninfo | grep mesh` should list `VK_EXT_mesh_shader`. The game needs a
   Vulkan 1.3 driver, ideally with mesh shaders.
2. **Update Mesa or the NVIDIA driver** if it doesn't.
3. **NVIDIA on a real-time kernel** (one with `rt` in its name, such as CachyOS's `linux-cachyos-rt-bore`): if the NVIDIA
   kernel module is not loaded, a driver compatibility or loading issue may be involved. Check the driver and kernel module
   logs, try a supported NVIDIA driver or a standard kernel, and check that a Vulkan driver is installed and loaded.
   `vulkaninfo --summary` lists the available Vulkan devices.
4. **The log warns that `/dev/udmabuf` is denied:** the game runs, but imports GPU memory more slowly, much more on AMD.
   [Give your user access](advanced.md#linux-faster-gpu-memory-devudmabuf).

[Search Linux reports](https://longshot.magicbean.dev/progress/#problem-search?q=linux)

### Linux: Play closed before the game's log appeared

Wait for the game to exit, then open `longshot_startup.log` in the game folder's `logs` folder: it has up to 64 KiB of the game's
startup errors and its exit status. Started from a terminal, the game prints there instead.

[Search startup reports](https://longshot.magicbean.dev/progress/#problem-search?q=startup)

### Linux: a low-memory warning, or the desktop freezes while loading

1. **Close other programs** and check `free -h`: the `Mem` and `Swap` rows are separate. Below 10 GiB of RAM the game
   warns even with plenty of swap.
2. **Don't keep retrying** a load that locks up the PC. After restarting, keep the failed run's log, and check
   `journalctl -k -b -1` for an out-of-memory kill or a GPU reset.

[Search memory reports](https://longshot.magicbean.dev/progress/#problem-search?q=memory)

### Windows: an out-of-memory notice, or a crash with `0xc00000fd` (stack overflow)

Windows has run out of memory: RAM plus the page file. The log has `mem: ... at the commit limit` lines before it. The
game waits up to 5 seconds for memory to come back; when it doesn't, it shows the notice and closes. A crash report that
says `0xc00000fd` with those lines is the same thing, from a version before the wait.

1. **Let Windows manage the page file.** In System > About > Advanced system settings > Performance > Advanced >
   Virtual memory, choose "Automatically manage paging file size for all drives", or give it at least 16 GB.
2. **Close other large programs**, such as browsers and other games, while you play.
3. **Choose a lower Quality preset** in the Insert menu: it uses less memory.

[Search memory reports](https://longshot.magicbean.dev/progress/#problem-search?q=memory)

## In game: graphics

### Parts of the picture are missing, or a message says your card has no mesh shader support

Without mesh shaders, Longshot draws through its compute fallback, which leaves some draws out
([#640](https://github.com/NotRexed/Hookshot/issues/640)).

1. **Update your graphics driver:** a card and driver with mesh shaders draw everything. NVIDIA RTX 20 series and
   newer, AMD RX 6000 series and newer, and Intel Arc have them.
2. **Check your card** against [the requirements](requirements.md).

[Search graphics reports](https://longshot.magicbean.dev/progress/#problem-search?q=graphics)

### One place has a wrong texture, or a floor or wall is missing

1. **Look for `longshot_damaged_files.txt`** in the game folder. The build writes it when your dump of the game is
   incomplete (about 23.3 GB instead of 23.8 GB), listing each damaged file and what was put in its place.
2. **Look in `longshot_log.txt` for `is damaged in this copy of the game`.** Game files that Longshot Builder didn't
   unpack (copied from another tool, or by hand) get no `longshot_damaged_files.txt`. The game then reads a similar
   texture in place of each damaged one as it plays, writes nothing to your game files, and logs one line per file.
   Without it, the Great Fairy Fountain's damaged floor cost about a third of the frame rate in the starting forest
   and the castle town's market.
3. **Dump the game again** and build from the new dump. The file and the lines go once the dump is whole.

[Search texture reports](https://longshot.magicbean.dev/progress/#problem-search?q=texture)

### AMD: blocky textures, solid rectangles in the sky, or dark courtyard surfaces

1. **Turn on ASTC as RGBA8:** open the menu (<kbd>F1</kbd>), go to the **Debug** tab, and under **Developer** turn on
   **ASTC as RGBA8**. Restart the game. It keeps those textures as decoded RGBA8 instead of re-encoding them.
2. **Watch the memory.** It can use much more graphics memory, and stalls when the card runs out. To undo it, turn the
   row off and restart.

If the row can't be changed, `longshot_env.txt` sets it: remove its `RX_VK_ASTC_RGBA8=1` line.

[Search AMD reports](https://longshot.magicbean.dev/progress/#problem-search?q=amd)

### Windows: HDR is on, but the game stays SDR

The **HDR** row in the **Graphics** tab of the menu (<kbd>F1</kbd>) says why under its help ("The picture stays
SDR: ..."), and `longshot_log.txt` has the same in its `vk: HDR:` line.

1. **"HDR is off for this display":** turn on **Use HDR** for the display the game is on, in Windows' **Settings >
   System > Display**.
2. **"Windows has HDR on, but the graphics driver offers the game no HDR output":** Windows' desktop is in HDR, but
   the driver doesn't offer HDR to Vulkan games. Updating the driver may not be enough: reinstall it with a clean
   install (NVIDIA: run the driver's installer from nvidia.com, choose **Custom**, tick **Perform a clean
   installation**), then restart the PC. A clean install resets the driver's settings, such as the output colour
   depth. If the line stays, [report it](#still-stuck) with your graphics card, your display and `longshot_log.txt`.
3. **Update the graphics driver** if neither fits.

[Search HDR reports](https://longshot.magicbean.dev/progress/#problem-search?q=hdr)

## In game: performance and freezes

### It stutters the first time you see something

Normal for now: the game prepares each new shader the first time it needs it, and it is smooth the next time you pass.
You can help remove it for everyone: [send your pipeline list](reporting-and-contributing.md#send-your-pipeline-list)
(**Debug → Save my pipeline list** in the menu).

If it stutters everywhere, for a long time, and the log says `the shaders' preparation was skipped at the build`: delete
`longshot_prepare_skipped` from the game folder's `cache` folder so the next start prepares them, or build again with
**Prepare shaders now** ticked.

[Search stutter reports](https://longshot.magicbean.dev/progress/#problem-search?q=stutter)

### Windows, NVIDIA: it stutters on every start, in places you have seen

The driver's shader cache is too small to keep them all (the first start builds about 2 GB; the driver's default often holds
only 1 GB, and it throws shaders out as it fills). The launcher raises a cache under 4 GB to 10 GB before a build or a start,
and says so on Home; when Windows needs an administrator for it, Home's **Raise it to 10 GB** asks. To do it yourself: open
the **NVIDIA Control Panel**, then **Manage 3D settings**, and set **Shader Cache Size** to **Unlimited** (or 10 GB). The
log's `vk: driver shader cache: NVIDIA's holds at most` line says what the game found. On Linux the game raises the
driver's limit itself.

[Search stutter reports](https://longshot.magicbean.dev/progress/#problem-search?q=stutter)

### Low FPS, or it slows down after a while

1. **Match the preset to your card.** In the menu's **Graphics** tab, pick the **Quality preset** for your graphics
   memory: Minimum (4GB), Low (6GB), Balanced (8GB) and so on. If the log says
   `vk: over the driver's video-memory allowance`, choose a lower one: its smaller render resolution needs less video
   memory.
2. **Walk through the same spot twice** once the shaders are prepared. Note whether the slowdown clears the second time,
   or lasts until you restart.
3. **Compare fairly.** Turn off **Frame generation**, so the FPS counts the game's own frames, and keep the resolution,
   upscaler and frame rate cap the same between runs.
4. **Keep the log from the slow run** (and from the earlier version, after an update), and say where the FPS dropped.
   Every 600 frames it writes a `vk: status:` line with the video memory in use against what the driver allows.
5. **To find out why,** follow [Report slow performance](performance-report.md): the game measures itself while it is
   slow.

[Search performance reports](https://longshot.magicbean.dev/progress/#problem-search?q=performance)

### A movie stutters, falls behind its sound, or starts on a black picture

On a CPU with 4 logical processors or fewer, the decoder uses 2 threads; `HOS_DAV1D_THREADS=<n>` in `longshot_env.txt`
sets another count. Run once with `HOS_VIDEO_LOG=1` in `longshot_env.txt` and attach the log: its `video:` lines say how
long each picture took to decode.

[Search movie reports](https://longshot.magicbean.dev/progress/#problem-search?q=movie)

### Steam Deck: the game freezes and closes in the starting area or first dungeon

`journalctl -k` shows `gfx_0.0.0 timeout`. Add the line `RX_VK_MESH_NOCULL=2c56dc1006942f74` to `longshot_env.txt` in
the game folder: it turns off the culling pass that runs for seconds there, and every model is still drawn
([#817](https://github.com/NotRexed/Hookshot/issues/817)).

[Search Steam Deck reports](https://longshot.magicbean.dev/progress/#problem-search?q=steam+deck)

## In game: controls, saves and language

### The menu won't open

Press <kbd>F1</kbd>: it always works. <kbd>Insert</kbd> and <kbd>Esc</kbd> do too, unless **Controls → Menu shortcut**
was changed. On a controller, press Home, or Select and Start together; on a DualShock 4 or DualSense, click the
touchpad.

[Search menu reports](https://longshot.magicbean.dev/progress/#problem-search?q=menu)

### A and B are swapped, or a button does the wrong thing

1. **Check the layout.** In the menu, open **Controls → Controls** and check **Layout preset**: the default layout has A on the
   right face button, Xbox / PC and Swap Face put it on the bottom one.
2. **Rebind a button** on that page: choose its row and the Gamepad or Keyboard column, press <kbd>Enter</kbd>, then the
   new button. <kbd>Esc</kbd> cancels, <kbd>Delete</kbd> clears it, and **Reset to defaults** puts them all back.
3. **Wrong icons?** **Button prompts** on the same page picks the icons the game shows. It doesn't change what a button
   does.

The [controls](graphics-and-controls.md#controls) cover the keyboard, the mouse and rumble.

[Search controller reports](https://longshot.magicbean.dev/progress/#problem-search?q=controller)

### A controller button opens the pause menu, or the menu opens by itself

Controller software can turn a pad's buttons into keys. Steam Input's **Desktop Layout** does this for a game started
outside Steam (as a Steam Deck in Desktop Mode does): the pad's A becomes <kbd>Enter</kbd>, B and Menu become
<kbd>Esc</kbd> and View becomes <kbd>Tab</kbd>, and the game sees them as keys beside the pad. Plus (the game's pause)
was <kbd>Tab</kbd> and <kbd>Enter</kbd> until options_version 8, so A opened the pause menu; Plus is <kbd>Tab</kbd> alone
now, so on the old default <kbd>Enter</kbd> no longer pauses. That is the one deliberate change: <kbd>Tab</kbd> and the
pad's Start always pause, and a file that chose <kbd>Enter</kbd> for Plus itself keeps it. On an older build, or if you
rebound Plus to <kbd>Enter</kbd> yourself, choose Controls → Controls → Plus and clear its <kbd>Enter</kbd> binding, or
turn Steam's non-game controller layout off: **Steam → Settings → Controller → Desktop Layout → Edit → Disable Steam
Input**.

[Search pause menu reports](https://longshot.magicbean.dev/progress/#problem-search?q=pause+menu)

### A save is missing, or a save transfer or update went wrong

1. **Close the game** and stop copying, so nothing else is overwritten.
2. **Copy the save folder somewhere safe:** see [where Longshot keeps them](saves.md#where-longshot-keeps-them).
3. **Put the backup back.** Every copy the launcher makes backs up the save it replaces first:
   [undo a copy](saves.md#undo-a-copy) shows where. An update keeps your saves; the
   [updates guide](updates-and-saves.md) says what else it keeps.

Keep save files private: don't post them.

[Search save reports](https://longshot.magicbean.dev/progress/#problem-search?q=save)

### The game is in the wrong language

Change **Gameplay → Game language** in the menu and restart, or change the code in `language.txt` in the game folder:
the file lists them all, and an update keeps the one you set.

[Search language reports](https://longshot.magicbean.dev/progress/#problem-search?q=language)

## Downloads and antivirus

### Windows Defender or the browser flags the download

Longshot isn't code-signed yet, so Defender often guesses `Trojan:Win32/Wacatac.H!ml` or `Wacatac.B!ml`: the `!ml`
means a guess, not a known virus.

1. **Check where it came from.** Only the [launcher's releases](https://git.magicbean.dev/MagicBeanDev/LongshotLauncher/releases)
   and [Longshot's](https://git.magicbean.dev/MagicBeanDev/Longshot/releases) are official;
   [the download page](https://longshot.magicbean.dev/download) sends you there.
2. **Let it through safely.** The [antivirus guide](https://longshot.magicbean.dev/help/antivirus/) shows how to check
   the file, restore it, and exclude only Longshot's folders.

[Search antivirus reports](https://longshot.magicbean.dev/progress/#problem-search?q=antivirus)

## Still stuck?

1. **After a crash, click Report.** When Longshot closes unexpectedly, at the start or while you play, a window says what
   happened and, where it can tell, why (update the graphics driver: Longshot needs Vulkan 1.3; out of memory; build
   the game again). **Report** copies a report to your clipboard (the error, your PC's hardware and the end of the log,
   with your home folder and user name taken out; no saves or game files) and opens the issue form in your browser:
   paste it into the form's `longshot_log.txt` box, then say what you were doing. The game sends nothing itself.
   **Open the log folder** shows the logs. The report is also saved in the game folder's `bug-reports/pending` folder.
2. **Make a bug report.** Open the menu (<kbd>F1</kbd>), go to **Gameplay** and choose **Bug report**. It saves a folder
   in `bug-reports` in the game folder with the log, a picture and `report.txt`, which ends with a link that opens the
   report form already filled in. Your home folder and user name are taken out.
3. **Or find the log yourself,** if the game doesn't get that far: `longshot_log.txt` in the game folder's `logs` folder is the last
   run, and `longshot_log.1.txt` to `.4.txt` the four before it. On Linux, `longshot_startup.log` has the startup
   errors. The logs are safe to send: your home folder is written `~` and your user name `<user>`.
4. **Search the reports.** [Someone may have had it already](https://longshot.magicbean.dev/progress/#problem-search),
   with a workaround.
5. **Ask on [Discord](https://discord.gg/magicbean).** The team and other players are there.
6. **Report it** with the [report form](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/new): your version,
   where you were in the game, your graphics card and driver, the log and a screenshot.
   [What to include](reporting-and-contributing.md) has the details.

Never share game files, keys or saves.
