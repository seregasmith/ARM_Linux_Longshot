# Reporting and contributing

## Not sure it's a bug?

[Ask on Discord](https://discord.gg/magicbean) first: the team and other players are there, and someone may know the
fix already. Check [common problems](common-issues.md) and [search the reports](https://git.magicbean.dev/MagicBeanDev/Longshot/issues)
too.

## Report a bug

Open a [new report](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/new) and include:

- **The version:** the first line of `longshot_log.txt`, in the game folder's `logs` folder.
- **Your PC:** the operating system, the graphics card and its driver version.
- **What happened,** and where in the game, with the steps to make it happen again if you can.
- **`longshot_log.txt`** from the run, and a screenshot or a short video.

**Gameplay → Bug report** in the in-game menu gathers all of it in one press, into a new folder
`bug-reports/<date>_<time>` in the game folder:

- `report.txt`, with the report form's answers (the version, where you are, your graphics card and driver, CPU, RAM and
  system, the menu options and switches you changed) and a link that opens the form with them filled in;
- `longshot_log.txt` (this run's log so far) and `longshot_log.1.txt` (the run before's, which has the crash if the game
  crashed last time);
- `picture.png`, the game's picture without the menu.

Your home folder and user name are taken out, and nothing else goes in: no saves, no game files, not your settings
files. Nothing is sent: open the form, write what happened, and drag the folder's files into it. Never attach game
files, keys or saves.

If the problem is that the game runs slowly or stutters, [report slow performance](performance-report.md) shows how to
have the game measure itself first.

## Send your pipeline list

The first time the game meets a pipeline it builds it on the spot, which stutters. Releases prepare a list of known
pipelines, and every player's play adds to it. To send yours:

1. In the game, open the menu, go to the **Debug** tab and choose **Save my pipeline list** (under Developer).
2. It saves `longshot-pipelines-<date>.zip` in your Downloads folder, copies a filled-in report to the clipboard and
   opens the [Submit your pipelines](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/14) issue.
3. Attach the zip to a comment there, paste the report and say where you played.

The zip holds only shader hashes, formats and draw state, and the report (the version, your system, GPU and driver, and
your graphics settings): no game code, game data or personal information.

## Help with testing

Fixes often need checking on hardware the developer doesn't have: another graphics card, Linux, a Steam Deck. Lend your
PC to the Test Farm, Beanstalk: a small app builds approved pull requests next to `main` from your own copy of the game,
plays the same scenes on both, and posts the numbers on the pull request, while you do something else.
[How it works, and the download](https://beanstalk.magicbean.dev/how).

Playing further into the game than anyone has, and saying what works on your graphics card, helps too.

## Improve the documentation or the code

Read the [contribution guide](../../CONTRIBUTING.md) and the [developer docs](../dev/README.md), then send a pull request
on [Forgejo](https://git.magicbean.dev/MagicBeanDev/Longshot). For this site, see the
[website instructions](../../website/README.md). Questions are welcome on Discord.
