"""Development builds and game runs on a PC that several agents and people share. Python's standard library only; Linux and
Windows. docs/dev/developing.md, *Test a change against a base* and *Share a PC: the job queue*.

This checkout on the PC's shared bases, every step through the queue (scripts/devtools/build.py):
    dev.py base [REV]                       the shared base where this checkout left main (or of REV), built once
    dev.py pack                             this checkout as a pack over it
    dev.py rt [--empty]                     build-linux/rt-<checkout>/ to play: the base with the pack (--empty: without)
    dev.py game [--quick | --timing] [--fsr]   a whole game folder, build-linux/game/
    dev.py vk                               the renderer alone, its exports checked
    dev.py env                              the build environment, as shell lines
    dev.py bases                            the shared bases and their packs
Measure in the game, through the queue (scripts/devtools/ab.py):
    dev.py ab [--env NAME=VALUE | --a ... --b ...] [--scene NAME] [--runs 3] [--seconds 60] [--dry-run]
                                            a switch (set against unset), or this checkout's change against its base:
                                            the scene loaded, warmed up, interleaved timing runs, the Measurements written
    dev.py ab --route ROUTE ...             the same over a route (assets/routes/): the input recorded once, given back
Routes, input recordings for tests and dev.py ab (scripts/devtools/routes.py):
    dev.py route record --save NAME --name ROUTE    play it once from a repository save: to assets/routes/
    dev.py route check | list               a route holds only what a route may; the routes
The PC's job queue (scripts/devtools/jobqueue.py):
    dev.py queue run NAME [--mode MODE] -- CMD...   CMD when the queue lets it
    dev.py queue status | history | cancel JOB
A base folder you name, without the queue: what base, pack and rt run, and what the Test Farm runs
(scripts/devtools/packs.py):
    dev.py packs base DIR --romfs R --exefs E [--tree T] [--seed GAME]
    dev.py packs pack DIR NAME [--tree T]
    dev.py packs rt DIR RUNTIME [PACK...]
    dev.py packs warm DIR --run "CMD with {runtime}" [--run ...]
A game folder you play, kept on the newest commit of main (scripts/devtools/update.py):
    dev.py update [--game DIR] [--check | --rollback | --gui] [--force]
                                            built from main's newest commit as soon as it lands, backed up first
This PC's development folder (scripts/devtools/home.py):
    dev.py paths [NAME]                     where it is and what it holds (~/.local/share/longshot-dev,
                                            %LOCALAPPDATA%\\longshot-dev, or LONGSHOT_DEV_HOME)
    dev.py migrate [--dry-run]              move what older tools kept in ~/.cache and ~/.config/longshot into it

`dev.py <command> --help` says more.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script's folder off the path)
from devtools import ab, build, home, jobqueue, packs, routes, update  # noqa: E402


def parser():
    p = argparse.ArgumentParser(prog="dev.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="group", required=True, metavar="COMMAND")
    for m in (build, ab, routes, jobqueue, packs, home, update):
        m.add_commands(sub)
    return p


def main(argv):
    command = []
    if "--" in argv:   # (`queue run`'s command is everything after --, its own options too)
        i = argv.index("--")
        argv, command = argv[:i], argv[i + 1:]
    p = parser()
    a = p.parse_args(argv)
    if command and not getattr(a, "takes_command", False):
        p.error("only `queue run` takes a command after --")
    a.command = command
    return a.func(a) or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
