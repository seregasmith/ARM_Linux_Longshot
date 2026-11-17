#!/bin/sh
# A stand-in for "AppRun --cli" in the setup window's self-test (longshot-setup --selftest <dir> --nup <file> --out <folder>,
# with LONGSHOT_SETUP_BUILD=<this file>): make_game.py's builder lines for the four stages and some log lines, written to the
# --log file too, as the real build writes them. STAND_IN_FAIL=1: it stops in the compile with an @@error and exit status 1.
# No game, no compiler: build_docker.sh setup runs it to see the window's build path work end to end.
log=
while [ $# -gt 0 ]; do
    case $1 in --log) log=$2; shift ;; esac
    shift
done
say() {
    echo "$@"
    if [ -n "$log" ]; then echo "$@" >> "$log"; fi
}
say "@@stage read Reading your game files"
for i in 1 2 3; do say "checked $i of 3 files"; say "@@progress 0.$((i * 3))"; done
say "@@stage lift Translating the game to PC code"
say "lift: the stand-in's functions"
say "@@progress 1"
say "@@stage compile Compiling"
say "@@plan 40 50"
say "@@eta 30"
i=0
while [ $i -lt 40 ]; do
    i=$((i + 1))
    say "@@file code_$i.c 0.1 new"
    say "  code_$i.c   1 MB   0s"
    say "@@progress 0.$((i * 2 + 10))"
done
if [ -n "${STAND_IN_FAIL:-}" ]; then
    say "@@error The compiler stopped (the stand-in's error)"
    exit 1
fi
say "@@stage finish Putting the game folder together"
say "@@progress 1"
say "@@times 12 8 -1"
say "@@done ${OUT:-/tmp/longshot}"
exit 0
