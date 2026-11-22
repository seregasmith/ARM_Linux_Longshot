# Playing on the Steam Deck

Longshot runs natively on the Steam Deck LCD and OLED (both use the same RDNA2 graphics; the OLED's memory is faster).
This page is the Deck's own install guide: where to build, how to put the game in your Steam library and play it from
Game Mode, what the game already sets up for the Deck by itself, and what to expect. The general install guide for
any PC is [getting started](getting-started.md).

**We do not host, store, or distribute game files, ROMs, game dumps, or decryption keys.**

## What you need

- The launcher's Linux AppImage: [Download for Linux](https://longshot.magicbean.dev/download/launcher/linux). The
  launcher needs no installing; its own Deck notes are in its [README](https://git.magicbean.dev/MagicBeanDev/LongshotLauncher/src/branch/main/README.md).
- Your own copy of the game, at the supported version ([requirements](requirements.md)).
- Room: about 60 GB free while building, then roughly 15 GB for the game and its shader cache. An SD card works for the
  finished game, but stutter when you arrive in an area is worse there: keep the game on the internal drive if you can.
- Somewhere to build: the Deck itself, or any other x86-64 Linux PC
  ([the same requirements](requirements.md)). A Windows PC builds the game for Windows, not for the Deck, so it can't
  take this route.

## Build on the Deck: no other PC needed

1. Switch to Desktop mode: in Game Mode press the **Steam** button, then **Power > Switch to Desktop**. To come back,
   the desktop's **Return to Gaming Mode** icon.
2. Download the launcher's `.AppImage`. Before opening it, enable **Allow executing file as program** in its
   Properties, or run `chmod +x LongshotLauncher-*-x86_64.AppImage` in a terminal.
3. Open the launcher. Its welcome page names the version it will build, with a **Change** link beside that name:
   press **Build** for the version shown, or **Change** first if you want a different channel. Either way the
   launcher then walks you through turning your own copy of the game into a PC game, a few pages at a time. It
   waits for its build tools to download and asks for your game file: press **Next** once both are ready, leave the
   settings page as it stands (its defaults suit a Deck), and press the last button, **Build** with the version on
   it. The compile starts.
4. Leave it running, plugged in, with the display timeout off: the Deck's CPU takes well over the ~15 minutes a
   12-core desktop needs, so budget an evening. The launcher finishes by itself; you don't sit and watch it, and it
   says what it did, or what went wrong, when it's done.

## Or build on a Linux PC and copy the game over

Build on any x86-64 Linux desktop or laptop exactly as the [install guide](getting-started.md) says. Then, from the
Deck's Desktop mode, copy the **whole game folder**, everything in it, to the Deck's internal storage: with the file
manager, or over the network. Copy it whole rather than picking files out of it; the game, its caches and its shader
list belong together.

The game's code is compiled to fit the CPU of the PC that built it, and a newer PC may use instructions the Deck's
CPU doesn't have: a game copied from that PC can close as soon as it starts. Whether your PC is safe is hard to
check, so the safe route is to build it for a level the Deck's CPU is certain to run: from a terminal, the builder's
own command line ([what that is](advanced.md#the-builder-from-the-command-line), where the builder comes from), with
`RX_MARCH=x86-64-v3` in front of it:

```sh
RX_MARCH=x86-64-v3 ./Longshot-<version>-x86_64.AppImage --cli --nup ~/your-game.nup --out ~/Games/Longshot
```

`x86-64-v3` is the level the Deck's CPU runs. A plain launcher build on a PC no newer than the Deck's CPU usually
copies over fine too; the line above takes the guesswork out.

Two things the copy doesn't carry, so don't expect them to:

- **Saves.** By default the game keeps them in `~/.local/share/longshot`, not in the game folder
  ([where the files go](updates-and-saves.md#where-the-files-go)), so the Deck makes its own. To bring a save across,
  copy it with [the saves guide](saves.md).
- **Finished shader work.** The shader caches in the folder are made for the GPU they were built on, and the rest of
  that work lives in a folder outside the game, so a copy to a Deck re-prepares for the Deck's GPU on its first
  start. It takes a few minutes, and the progress is on screen.

## Put the game in Steam and play from Game Mode

The launcher's **Play** starts the game in Desktop mode, where Steam's Desktop layout can send key presses as well as
controller input, causing one action to trigger two things at once. To play with the Deck's controls, add the game to
Steam in Desktop mode and start it from Game Mode:

1. In Steam's window, open the **Games** menu (the menu bar at the top) and choose **Add a Non-Steam Game to My
   Library...**, near the bottom of that menu. In the window that opens, press **Browse**, find the launcher's
   `.AppImage`, and add it.
2. Open the new entry's **Properties**. On the **Shortcut** tab, in the **Launch options** box, add:

   ```text
   --play "<the game folder>"
   ```

   Keep any existing text on the line; the box may start blank. The game folder is where the build put the game:
   under the launcher's builds
   folder (its **Settings** page shows where), or, for the copy route, the folder you copied to the Deck. With no
   folder named, `--play` starts the launcher's default build. Note that the launcher names each build's folder after
   its version, so after an update that renames the folder, point this line at the new path.

3. Return to Game Mode. The entry sits in your library with your other games; start it there.

Starting through Steam is how the Deck expects a game to run: Steam gives the game its controller settings, and the
game leaves the Deck's own per-game performance controls (the Quick Access menu's performance page) to Steam - it
knows a clock the player set is the player's, not its own to override.

## The game already knows it's on a Deck

On a Deck the game sets itself up: the **Auto (this PC)** quality preset picks the Minimum picture and the game's own
upscaler; Output resolution starts at the screen's own size, 1280x800 on a Deck; the frame-rate cap stays at 60; and
two settings measured on Decks ship on there: the game holds the Deck's GPU at its peak clock while it runs and gives
the clock back when it closes (measured +8 to 20% frame rate on the LCD, more on the OLED, no settings, nothing to
install), and small GPU results are read
a frame late where the game's own console behaviour means it can't be relying on them being finished yet (about +20%
more in busy outdoor views). Leave these alone:

- **The quality preset and the Upscaler row.** If you built with FSR 4 (bringing AMD's own library in yourself),
  switch back to the game's own: the Deck's GPU has no AI accelerators, so FSR 4 runs as ordinary shaders - 15.7 fps
  against 20.6 fps with FSR 3.1 on the same save.
- **The cap at 60:** the game's own scripts expect it.
- **Threaded rendering on Auto:** on the Deck it measured the same or worse; it helps at high frame rates on desktop
  GPUs.

You don't need any of the `longshot_env.txt` switches to play well. If a page or a report ever asks you for one line,
it names the line exactly ([common problems](common-issues.md)).

One thing a player can still raise, per game, is Steam's own GPU clock: Steam's per-game performance settings can take
the clock past what the game's own hold keeps, and measured about 10% more in steady play at Steam's 1600 MHz ceiling.
It keeps the fan up and costs battery, so don't leave it on in a bag or for a child, and put it back when you're done.
Longshot reads the clock setting when it starts and leaves a clock you've chosen to you.

## What to expect

At the worst outdoor save measured (the starting village, standing and spinning the camera): about 21 fps at the
Deck's normal clocks, and about 28 to 31 with the clock raised as above. Indoor areas and dungeons run in the mid 30s.
Camera spins in the castle town cost about 30%, loading into a busy outdoor save stutters for a minute or two while
the world streams in, and the opening movie drops a few pictures at the normal clocks. The frame rates on this page
are from a Deck LCD; the OLED has the same graphics with faster memory, so its numbers should be the same or better.

## If something is wrong

If the game freezes and closes in the starting area or the first dungeon, that problem has a one-line fix named here:
[Steam Deck: the game freezes](common-issues.md#steam-deck-the-game-freezes-and-closes-in-the-starting-area-or-first-dungeon).
For anything else, [common problems](common-issues.md) says what to collect, and
[report slow performance](performance-report.md) says how a stutter report gets answered faster.
