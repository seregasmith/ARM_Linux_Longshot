#!/bin/bash
# build_docker.sh: build Longshot from your own copy of the game in a Linux Docker container with Nix, on any host, for
#   aarch64  ARM64 Linux handhelds (Armada OS, Snapdragon with Turnip; docs/arm64-port-plan.md) -> a game folder to copy
#            to the device, or the setup AppImage that builds the game on the device
#   x86_64   x86-64 Linux PCs and the Steam Deck
# After the TotK port's build_docker.sh (github.com/seregasmith/ARM_Linux_totk_recomp), with the same nixpkgs pin as it
# and the Bloodborne port, so one Nix store volume serves all three (LONGSHOT_NIX_VOLUME below).
# Hosts: macOS on Apple Silicon or Intel (Docker Desktop, OrbStack or colima), Linux on arm64 or x86-64 (Docker Engine).
# A target that matches Docker's own architecture builds natively; the other one runs emulated (Rosetta or QEMU, slow).
#
# The aarch64 game does not compile yet: the x86-only code that stops it is listed in docs/arm64-port-plan.md ("What has
# to change", Phase 2). Until it is ported, tools, test and shell are what work for aarch64.
#
# Usage:  ./build_docker.sh [--arch aarch64|x86_64] [tools|test|game|setup|lock|shell|clean]
#   --arch  target architecture (or LONGSHOT_ARCH; arm64 and amd64 work too). Default: Docker's own architecture
#   tools   (no game files) from Nix: LLVM 23's clang and lld, the target sysroot (Ubuntu 24.04: glibc 2.39,
#           libstdc++ 13), FFmpeg with dav1d and SDL2 built against it, the Vulkan, SPIR-V and VMA headers
#           (scripts/nix/default.nix, at scripts/linux/deps.sh's versions); the Python environment (requirements.txt)
#   test    tools, then the checks that need no game files (python -I -B tests/run_all.py); after a game build also the
#           instruction test on the game's own instruction forms (nx2/test_insn.py)
#   game    (default) tools, then nx2/make_game.py on your game: the lift, the build, and a game folder in LONGSHOT_OUT.
#           The lifted C and the objects are kept, so a later run compiles only what changed. No shaders are prepared
#           (the container has no GPU): the game prepares them at its first start on the device
#   setup   (no game files) the setup AppImage, Longshot-<version>-<arch>.AppImage in LONGSHOT_OUT: Longshot's Linux
#           builder (scripts/linux/appimage.sh) with its build tools inside it, made for the target (LLVM's release of
#           clang and lld, the sysroot, FFmpeg, SDL2, the headers; python-build-standalone with requirements.txt).
#           On the device it builds the game from the player's own copy, downloading nothing:
#             ./Longshot-<version>-aarch64.AppImage --cli --nup game.nup --out ~/Games/Longshot
#           It holds no game code: it can be shared as Longshot's own AppImage is
#   lock    write scripts/nix/sysroot-<arm64|amd64>.lock.json: the pinned Ubuntu packages of the target sysroot (a run
#           does it when the file is missing or scripts/nix/sysroot_lock.py lists other packages; commit it)
#   shell   tools, then an interactive nix-shell with the build's environment, in the container's copy of the tree
#   clean   delete this target's image and volumes (the work folder, the Python environment and this script's own Nix
#           store; not a volume named by LONGSHOT_NIX_VOLUME). The source tree and LONGSHOT_OUT are untouched
#
# Your game (game mode; mounted read-only, never written):
#   LONGSHOT_GAME  a .nup file, or a folder with romfs/ and exefs/ in it (nx2/make_game.py --game): program
#                  0400486024DEC000, update v131072, build e0f3ccfa (README.md, "What you need")
# Example:
#   LONGSHOT_GAME=~/Switch/game.nup ./build_docker.sh
# Then copy the folder to the handheld and start ./longshot in it. The folder holds code and data made from your game:
# for your own devices only (CONTRIBUTING.md).
#
# Settings (environment variables):
#   LONGSHOT_ARCH       target, as --arch (the option wins)
#   LONGSHOT_SRC        source tree (default: the directory this script is in)
#   LONGSHOT_OUT        where the game folder, longshot-<arch>, and the setup AppImage go (default:
#                       build-linux/install-<arch> in the source tree, which git ignores)
#   LONGSHOT_LANGUAGE   the game's language (make_game.py --language, e.g. en-US)
#   LONGSHOT_CPU        the CPU level the game is built for (nx2/build.py RX_MARCH). aarch64: armv8.2-a+lse+rcpc+dotprod
#                       by default (Snapdragon 8 Gen 2, 8 Gen 3 and 8 Elite); x86_64: build.py's own choice
#   LONGSHOT_NIX_VOLUME an existing Docker volume to use as the Nix store. The Bloodborne and TotK ports pin the same
#                       nixpkgs, so theirs saves the downloads: LONGSHOT_NIX_VOLUME=bbport-nix or totk-nix-aarch64
#                       (aarch64), bbport-nix-x86_64. It must exist and hold the target architecture's Nix
#   JOBS                parallel compiler processes (default: from Docker's RAM, about 2 GB each)
#   GITHUB_TOKEN        optional GitHub token for Nix downloads, if GitHub's API rate limit hits
#
# Logs: build-linux/docker-logs-<arch>/ in the source tree: each step's output, and make_game.log. The work itself (the
# container's copy of the tree, the lifted C, the objects, the Python environment) lives in Docker volumes: macOS shares
# folders slowly and without case sensitivity. Linux host with Docker Engine: files the container writes into the tree
# and LONGSHOT_OUT are given back to you (ownership only). Written for macOS's stock bash 3.2 and any newer bash.
set -eo pipefail

NIXPKGS_REV=6510408d147d8c5e6d76043e899e9251756f6a75   # nixos-unstable, 2026-10-10 (the Bloodborne and TotK ports' pin)
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
SRC=${LONGSHOT_SRC:-$HERE}
ARCH=${LONGSHOT_ARCH:-}
MODE=game

die() { echo "ERROR: $*" >&2; exit 1; }
usage() {
    echo "Usage: $0 [--arch aarch64|x86_64] [tools|test|game|setup|lock|shell|clean]" >&2
    exit 2
}
norm_arch() {
    case $1 in
        aarch64|arm64|armv8*) echo aarch64 ;;
        x86_64|amd64|x64|x86-64|x86) echo x86_64 ;;
        *) echo "" ;;
    esac
}

while [ $# -gt 0 ]; do
    case $1 in
        --arch) [ $# -ge 2 ] || usage; ARCH=$2; shift 2 ;;
        --arch=*) ARCH=${1#--arch=}; shift ;;
        -h|--help) awk 'NR > 1 && /^set -eo/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        tools|test|game|setup|lock|shell|clean) MODE=$1; shift ;;
        *) echo "ERROR: unknown argument '$1'" >&2; usage ;;
    esac
done

command -v docker >/dev/null || die "docker not found. Install Docker Desktop, OrbStack, colima or Docker Engine."
docker info >/dev/null 2>&1 || die "Docker is installed but not running (or this user may not use it)."
DOCKER_ARCH=$(norm_arch "$(docker info --format '{{.Architecture}}' 2>/dev/null)")
[ -n "$DOCKER_ARCH" ] || die "cannot tell Docker's CPU architecture (docker info --format '{{.Architecture}}')"
if [ -z "$ARCH" ]; then
    ARCH=$DOCKER_ARCH
else
    wanted=$(norm_arch "$ARCH")
    [ -n "$wanted" ] || die "unknown target architecture '$ARCH' (aarch64 or x86_64)"
    ARCH=$wanted
fi
case $ARCH in
    aarch64) PLATFORM=linux/arm64; MARCH=${LONGSHOT_CPU:-armv8.2-a+lse+rcpc+dotprod} ;;
    x86_64)  PLATFORM=linux/amd64; MARCH=${LONGSHOT_CPU:-} ;;
esac
DOCKER_PLATFORM_ARCH=${PLATFORM#linux/}

# Per target: image, Nix store volume (a store holds one architecture's Nix) and the work volumes.
IMAGE="longshot-builder:${NIXPKGS_REV:0:8}-$ARCH"
OWN_NIX=longshot-nix-$ARCH
NIX_VOLUME=${LONGSHOT_NIX_VOLUME:-$OWN_NIX}
WORK_VOLUMES="work venv"   # longshot-<arch>-<name>: /work (the tree's copy, the game's work folder, caches), /venv

if [ "$MODE" = clean ]; then
    docker image rm "$IMAGE" 2>/dev/null || true
    for v in $WORK_VOLUMES; do docker volume rm "longshot-$ARCH-$v" 2>/dev/null || true; done
    echo "Removed the $ARCH image $IMAGE and the work volumes longshot-$ARCH-*."
    if [ "$NIX_VOLUME" = "$OWN_NIX" ]; then
        docker volume rm "$NIX_VOLUME" 2>/dev/null || true
        echo "Removed the Nix store volume $NIX_VOLUME (the next build downloads it again)."
    else
        echo "Kept the Nix store volume $NIX_VOLUME: it is not this script's own ($OWN_NIX)."
    fi
    exit 0
fi
if [ "$NIX_VOLUME" != "$OWN_NIX" ] && ! docker volume inspect "$NIX_VOLUME" >/dev/null 2>&1; then
    echo "ERROR: LONGSHOT_NIX_VOLUME=$NIX_VOLUME: no such Docker volume. Volumes here:" >&2
    docker volume ls --format '  {{.Name}}' >&2 || true
    die "name an existing one, or unset LONGSHOT_NIX_VOLUME to use this script's own ($OWN_NIX)"
fi

SRC=$(cd -- "$SRC" 2>/dev/null && pwd) || die "LONGSHOT_SRC directory not found"
[ -f "$SRC/nx2/make_game.py" ] && [ -f "$SRC/scripts/nix/shell.nix" ] && [ -f "$SRC/scripts/linux/deps.sh" ] ||
    die "$SRC is not the Longshot source tree (no nx2/make_game.py, scripts/nix/shell.nix, scripts/linux/deps.sh)." \
        "Put this script in its root or set LONGSHOT_SRC."

# The game: mounted read-only on its own, under its own name (make_game.py --game tells a .nup from a folder by it).
game_flags=()
if [ "$MODE" = game ]; then
    [ -n "${LONGSHOT_GAME:-}" ] ||
        die "LONGSHOT_GAME is not set: game mode needs your .nup file, or a folder with romfs/ and exefs/"
    [ -e "$LONGSHOT_GAME" ] || die "LONGSHOT_GAME: no such file or folder: $LONGSHOT_GAME"
    game=$(cd -- "$(dirname -- "$LONGSHOT_GAME")" && echo "$(pwd)/$(basename -- "$LONGSHOT_GAME")")
    if [ -d "$game" ] && { [ ! -d "$game/romfs" ] || [ ! -d "$game/exefs" ]; }; then
        die "LONGSHOT_GAME: $game is a folder without romfs/ and exefs/ in it"
    fi
    game_flags=(-v "$game:/game/$(basename "$game"):ro" -e "LONGSHOT_GAME_IN=/game/$(basename "$game")")
fi
if [ "$MODE" = game ] || [ "$MODE" = setup ]; then
    OUT=${LONGSHOT_OUT:-$SRC/build-linux/install-$ARCH}
    mkdir -p "$OUT"
    OUT=$(cd -- "$OUT" && pwd)
    game_flags+=(-v "$OUT:/out")
fi
# test: a game folder built before holds the game's exefs, which the instruction test reads (read-only here)
if [ "$MODE" = test ]; then
    OUT=${LONGSHOT_OUT:-$SRC/build-linux/install-$ARCH}
    if [ -d "$OUT/longshot-$ARCH/exefs" ]; then game_flags=(-v "$(cd -- "$OUT" && pwd):/out:ro"); fi
fi
LOGS=$SRC/build-linux/docker-logs-$ARCH
mkdir -p "$LOGS"

emulation_help() {
    echo "How to enable it:" >&2
    if [ "$(uname -s)" = Darwin ]; then
        echo "  Docker Desktop: Settings > General > \"Use Rosetta for x86_64/amd64 emulation on Apple Silicon\"" >&2
        echo "  OrbStack:       built in" >&2
        echo "  colima:         colima start --vm-type vz --vz-rosetta" >&2
    else
        echo "  Register QEMU for $PLATFORM (lasts until reboot):" >&2
        echo "    docker run --privileged --rm tonistiigi/binfmt --install $DOCKER_PLATFORM_ARCH" >&2
    fi
}

echo "==> Target: $ARCH ($PLATFORM); Docker runs on $DOCKER_ARCH"
if [ "$ARCH" != "$DOCKER_ARCH" ]; then
    echo "==> Emulated build (Rosetta or QEMU): slower than a native $ARCH build." \
         "Checking that $PLATFORM containers run"
    got=$(docker run --rm --platform "$PLATFORM" nixos/nix:latest uname -m 2>&1 | tail -n 1) || true
    if [ "$(norm_arch "$got")" != "$ARCH" ]; then
        echo "ERROR: this Docker can't run $PLATFORM containers (test output: ${got:-nothing})." >&2
        emulation_help
        exit 1
    fi
fi

mem=$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 0)
ncpu=$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 0)
case $mem in ''|*[!0-9]*) mem=0 ;; esac
case $ncpu in ''|*[!0-9]*) ncpu=0 ;; esac
mem_mib=$((mem / 1048576))
if [ -z "${JOBS:-}" ]; then
    JOBS=$((mem_mib / 2048))
    if [ "$ncpu" -gt 0 ] && [ "$JOBS" -gt "$ncpu" ]; then JOBS=$ncpu; fi
    if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
fi
case $JOBS in ''|*[!0-9]*|0) die "JOBS must be a positive number" ;; esac
echo "==> Source tree: $SRC; logs: $LOGS"
echo "==> Docker VM: ${ncpu} CPUs, ${mem_mib} MiB RAM; compiling with JOBS=${JOBS}${MARCH:+; CPU level $MARCH}"
if [ "$mem_mib" -lt 7500 ]; then
    echo "WARNING: little RAM for this build. Give Docker 8 GB or more" \
         "(Docker Desktop: Settings > Resources > Memory), or expect a slow build." >&2
fi
if [ "$MODE" = game ]; then
    echo "==> The game into $OUT/longshot-$ARCH; the build needs about 60 GB free in Docker's disk," \
         "the folder about 30 GB"
fi

# Linux host, rootful Docker Engine: files the container writes into the tree belong to root on the host; the
# container gives them back to this user at the end. Not with rootless Docker, Podman or Docker Desktop.
OWNER=""
if [ "$(uname -s)" = Linux ]; then
    engine="$(docker --version 2>/dev/null)"
    engine="$engine $(docker info --format '{{.OperatingSystem}} {{.SecurityOptions}}' 2>/dev/null)"
    case $engine in
        *odman*|*rootless*|*"Docker Desktop"*) ;;
        *) OWNER="${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}" ;;
    esac
fi

# Image: the official Nix image plus a pinned nixpkgs tree (NIX_PATH), as the Bloodborne and TotK ports', so
# scripts/nix/shell.nix's `import <nixpkgs>` always resolves to the same package set. git fetches the pinned tags.
image_arch=$(docker image inspect --format '{{.Architecture}}' "$IMAGE" 2>/dev/null || true)
if [ "$image_arch" != "$DOCKER_PLATFORM_ARCH" ]; then
    echo "==> Building image $IMAGE ($PLATFORM)"
    docker build --platform "$PLATFORM" -t "$IMAGE" --build-arg NIXPKGS_REV="$NIXPKGS_REV" - <<'DOCKERFILE'
FROM nixos/nix:latest
ARG NIXPKGS_REV
RUN mkdir -p /opt/nixpkgs \
 && curl -fsSL "https://github.com/NixOS/nixpkgs/archive/${NIXPKGS_REV}.tar.gz" \
    | tar -xz --strip-components=1 -C /opt/nixpkgs \
 && (command -v git >/dev/null \
     || nix-env --option sandbox false --option filter-syscalls false --option build-users-group '' \
            -f /opt/nixpkgs -iA git) \
 && git config --global --add safe.directory '*'
ENV NIX_PATH=nixpkgs=/opt/nixpkgs
WORKDIR /src
DOCKERFILE
fi

if [ "$NIX_VOLUME" != "$OWN_NIX" ]; then
    vol_system=$(docker run --rm --platform "$PLATFORM" -v "$NIX_VOLUME":/nix "$IMAGE" \
        nix-instantiate --eval --expr builtins.currentSystem 2>/dev/null | tr -d '"' | tail -n 1) || true
    if [ "$vol_system" != "$ARCH-linux" ]; then
        die "LONGSHOT_NIX_VOLUME=$NIX_VOLUME holds ${vol_system:-no working} Nix ($ARCH-linux needed)"
    fi
fi

# Runs inside the container, in /src (the mounted tree). $1 = mode. The build itself runs in /work/src, a copy of the
# tree in the work volume; only the sysroot lock is written into /src. Only the Nix image's own tools outside nix-shell.
read -r -d '' INNER <<'INNER_SCRIPT' || true
set -euo pipefail
mode=$1; shift
cd /src
log=/logs/build_docker.log
: > "$log"
# When the container exits, also after a failure: on a Linux host with Docker Engine, root-owned files in what the build
# writes into the tree go back to the host user (ownership only).
on_exit() {
    if [ -n "${LONGSHOT_OWNER:-}" ]; then
        local p
        for p in /logs /out /src/scripts/nix; do
            if [ -e "$p" ]; then find "$p" -user 0 -exec chown -h "$LONGSHOT_OWNER" {} + 2>/dev/null || true; fi
        done
    fi
}
trap on_exit EXIT
# Nix in a throwaway root container: the Bloodborne and TotK ports' settings and guards, unchanged: no sandbox, no
# build users, no seccomp filter (Rosetta, QEMU), builds one at a time with $LONGSHOT_JOBS cores, and /homeless-shelter
# removed whenever it appears between builds, since Nix refuses to start a build while it exists.
export HOME=/root XDG_CACHE_HOME=/root/.cache
mkdir -p /tmp/nixhooks /tmp/nix-builds "$XDG_CACHE_HOME"
chmod 700 /tmp/nix-builds
printf '#!/bin/sh\nrm -rf /homeless-shelter\n' > /tmp/nixhooks/post-build
chmod +x /tmp/nixhooks/post-build
clear_homeless() {
    if [ -e /homeless-shelter ]; then
        echo "==> Removing /homeless-shelter left by $1"
        rm -rf /homeless-shelter
    fi
}
clear_homeless "the image"
(
    while sleep 0.2; do
        [ -e /homeless-shelter ] || continue
        [ -z "$(ls -A /tmp/nix-builds 2>/dev/null)" ] || continue
        rm -rf /homeless-shelter
    done
) &
export NIX_CONFIG="experimental-features = nix-command flakes
sandbox = false
build-users-group =
filter-syscalls = false
max-jobs = 1
build-dir = /tmp/nix-builds
cores = ${LONGSHOT_JOBS:-2}
post-build-hook = /tmp/nixhooks/post-build"
if [ -n "${GITHUB_TOKEN:-}" ]; then
    NIX_CONFIG="$NIX_CONFIG
access-tokens = github.com=${GITHUB_TOKEN}"
fi

# What every step runs first, inside nix-shell: caches in the work volume, and nix-ld as the dynamic linker that
# prebuilt Linux programs name (Python's binary wheels).
cat > /tmp/longshot-env.sh <<'ENV'
export HOME=/root XDG_CACHE_HOME=/root/.cache
export UV_CACHE_DIR=/work/uv-cache UV_LINK_MODE=copy UV_PYTHON_DOWNLOADS=never
# glibc's fatal messages (a failed fortify check, a corrupted heap) go to stderr, which the logs keep, not the terminal
export LIBC_FATAL_STDERR_=1
case $(uname -m) in
    aarch64) mkdir -p /lib && ln -sf "$LONGSHOT_NIX_LD_STUB" /lib/ld-linux-aarch64.so.1 ;;
    x86_64) mkdir -p /lib64 && ln -sf "$LONGSHOT_NIX_LD_STUB" /lib64/ld-linux-x86-64.so.2 ;;
esac
# The Python environment, as Longshot's tools run (python -I), with the libraries its binary wheels expect beside glibc
py() { LD_LIBRARY_PATH="$LONGSHOT_WHEEL_LIBS" /venv/bin/python -I -B "$@"; }
[ -f /tmp/longshot-build.sh ] && . /tmp/longshot-build.sh
ENV

# in_shell LABEL COMMAND: COMMAND in the build environment (scripts/nix/shell.nix); runs again (up to 3 times) when Nix
# stopped on /homeless-shelter, keeping what it built.
in_shell() {
    local status attempt
    for attempt in 1 2 3; do
        status=0
        clear_homeless "an earlier step"
        echo "==> $1" | tee -a "$log"
        nix-shell scripts/nix/shell.nix --argstr arch "$LONGSHOT_ARCH" --run ". /tmp/longshot-env.sh; $2" 2>&1 |
            tee -a "$log" | tee /tmp/step.log || status=$?
        clear_homeless "$1"
        if [ "$status" -ne 0 ] && [ "$attempt" -lt 3 ] &&
           grep -q 'home directory "/homeless-shelter" exists' /tmp/step.log; then
            echo "==> Nix stopped on /homeless-shelter during $1: running it again (attempt $((attempt + 1)) of 3)"
            continue
        fi
        return "$status"
    done
}

case $LONGSHOT_ARCH in aarch64) debarch=arm64 ;; x86_64) debarch=amd64 ;; esac
triple=$LONGSHOT_ARCH-linux-gnu
echo "==> Container: $(uname -m), page size $(getconf PAGESIZE)"
# The lock is written anew when it is missing or scripts/nix/sysroot_lock.py's package list changed (its --check)
if [ "$mode" = lock ] ||
   ! in_shell "the sysroot lock is current" "python3 -I scripts/nix/sysroot_lock.py --arch $debarch --check"; then
    in_shell "the sysroot lock (scripts/nix/sysroot-$debarch.lock.json; commit it)" \
        "python3 -I scripts/nix/sysroot_lock.py --arch $debarch"
    [ "$mode" = lock ] && exit 0
fi

# ---- tools: everything that needs no game files
roots=/work/nix-roots
mkdir -p "$roots"
in_shell "the compiler, the target sysroot and libraries (Nix; the first run builds dav1d, FFmpeg and SDL2)" \
    "for a in toolchain sysroot ffmpeg sdl2 headers; do
         nix-build scripts/nix -A \$a --argstr arch $LONGSHOT_ARCH -o $roots/\$a >/dev/null || exit 1
     done && ls -l $roots"
toolchain=$(readlink -f "$roots/toolchain")
sysroot=$(readlink -f "$roots/sysroot")
ffmpeg=$(readlink -f "$roots/ffmpeg")
sdl2=$(readlink -f "$roots/sdl2")
headers=$(readlink -f "$roots/headers")
case $LONGSHOT_ARCH in
    aarch64) loader=$sysroot/lib/ld-linux-aarch64.so.1 ;;
    x86_64) loader=$sysroot/lib64/ld-linux-x86-64.so.2 ;;
esac
# Target programs run in the container through the sysroot's own loader and libraries (as on the handheld).
run_target="$loader --library-path $sysroot/lib/$triple:$sysroot/usr/lib/$triple"
in_shell "test programs for the target (C, and C++20 with the static libstdc++, against the sysroot)" \
    "printf '#include <stdio.h>\nint main(void){puts(\"C ok\");return 0;}\n' > /tmp/t.c &&
     printf '#include <cstdio>\n#include <format>\n' > /tmp/t.cpp &&
     printf 'int main(){std::puts(std::format(\"{} ok\", \"C++20\").c_str());}\n' >> /tmp/t.cpp &&
     $toolchain/bin/clang --target=$triple --sysroot=$sysroot -fuse-ld=lld -O2 /tmp/t.c -o /tmp/t &&
     $run_target /tmp/t &&
     $toolchain/bin/clang++ --target=$triple --sysroot=$sysroot -fuse-ld=lld -O2 -std=c++20 \
         -static-libstdc++ -static-libgcc /tmp/t.cpp -o /tmp/tpp &&
     $run_target /tmp/tpp"
# nx2/make_game.py and nx2/build.py find the compiler as EMSDK/upstream/bin (third_party/a64-kit/tools/toolchain.py)
emsdk=/work/emsdk
in_shell "the compiler where Longshot looks for it (EMSDK/upstream/bin)" \
    "rm -rf $emsdk && mkdir -p $emsdk/upstream/bin && ln -s $toolchain/bin/* $emsdk/upstream/bin/ &&
     $emsdk/upstream/bin/clang --version | head -n 1"
# requirements.txt's pins, binary wheels only. The button-prompt packages (nx2/prompt_requirements.txt) are optional:
# without them the game shows the Switch's icons (make_game.py says so), so a missing wheel for this CPU is a warning
in_shell "the Python environment (/venv: requirements.txt)" \
    "uv venv -q --allow-existing --python \$(command -v python3.12) /venv &&
     grep -v '^-r ' requirements.txt > /tmp/requirements-core.txt &&
     LD_LIBRARY_PATH=\$LONGSHOT_WHEEL_LIBS uv pip install -q --python /venv/bin/python --only-binary :all: \
         -r /tmp/requirements-core.txt &&
     { LD_LIBRARY_PATH=\$LONGSHOT_WHEEL_LIBS uv pip install -q --python /venv/bin/python --only-binary :all: \
           -r nx2/prompt_requirements.txt ||
       echo 'WARNING: the button-prompt packages did not install for $LONGSHOT_ARCH:' \
            'the game shows the Switch icons'; }"
# What the build reads (nx2/make_game.py, nx2/build.py), sourced by every later step
cat > /tmp/longshot-build.sh <<BUILD_ENV
export EMSDK=$emsdk LONGSHOT_SYSROOT=$sysroot LONGSHOT_FFMPEG=$ffmpeg LONGSHOT_SDL2=$sdl2/lib/libSDL2-2.0.so.0
export LONGSHOT_VULKAN_HEADERS=$headers/vulkanheaders LONGSHOT_SPIRV_HEADERS=$headers/spirv-headers
export LONGSHOT_VMA_HEADERS=$headers/vulkanmemoryallocator
export LONGSHOT_JOBS=$LONGSHOT_JOBS
${LONGSHOT_MARCH:+export RX_MARCH=$LONGSHOT_MARCH}
BUILD_ENV
# The build runs in its own copy of the tree (/work/src): the tree is not written (the translator and the unpacker are
# built beside their sources, for this architecture), and the work stays off the host's shared folder. Edit the tree on
# the host; each run copies what changed. .git comes too: the build names its version with git describe.
cat > /tmp/sync-excludes <<'EXCLUDES'
/build/
/build-linux/
/work/
/cpm/
/llvm-mingw/
__pycache__/
.idea/
.vs/
.vscode/
/third_party/a64-kit/lift/liba64emit.so*
/third_party/a64-kit/lift/a64emit.dll*
/third_party/zbic/zbic.so
/third_party/zbic/zbic.dll
EXCLUDES
in_shell "the source tree, copied to /work/src" \
    "rsync -a --delete --exclude-from=/tmp/sync-excludes /src/ /work/src/ && git -C /work/src describe --tags --always"

# ---- shell
if [ "$mode" = shell ]; then
    cd /work/src
    echo "==> A shell in the build environment, in /work/src (the container's copy: edit the tree on the host instead)"
    nix-shell /src/scripts/nix/shell.nix --argstr arch "$LONGSHOT_ARCH" --command ". /tmp/longshot-env.sh; return"
    exit $?
fi
[ "$mode" = tools ] && { echo "==> Tools ready"; exit 0; }

# ---- setup: the setup AppImage, without game files. scripts/linux/appimage.sh builds it from the container's copy of
# the tree, with LONGSHOT_APPIMAGE_TOOLS: the build tools for the target, at tools.lock's places, which it then carries
if [ "$mode" = setup ]; then
    in_shell "its parts (Nix: LLVM's release, python-build-standalone, appimagetool, its runtime, the CPM headers)" \
        "for a in setupLlvm setupPython appimagetool appimageRuntime cpm; do
             nix-build scripts/nix -A \$a --argstr arch $LONGSHOT_ARCH -o $roots/\$a >/dev/null || exit 1
         done && ls -l $roots"
    s=/work/setup
    in_shell "the build tools it carries, at tools.lock's places ($s/tools/usr)" \
        "rm -rf $s && L=$s/tools/usr/lib/longshot && mkdir -p \$L/sdl2 $s/tools/usr/share/longshot $s/deps &&
         cp -a $roots/setupLlvm/. \$L/emsdk && cp -a $sysroot/. \$L/sysroot && cp -a $ffmpeg/. \$L/ffmpeg &&
         cp -L $sdl2/lib/libSDL2-2.0.so.0 \$L/sdl2/ &&
         cp $sdl2/share/licenses/SDL2-LICENSE.txt \$L/sdl2/LICENSE.txt &&
         cp -a $roots/cpm/. $s/tools/usr/share/longshot/cpm && chmod -R u+w $s/tools &&
         \$L/emsdk/upstream/bin/clang --version | head -n 1 && du -sh $s/tools"
    # (the setup window, builder/linux/setup_ui: SDL2 and Dear ImGui, against the sysroot, in the AppImage's usr/bin. Its
    # self-test draws every screen offscreen and runs a stand-in build through the window to its end, with the target's loader)
    in_shell "the setup window (builder/linux/setup_ui: usr/bin/longshot-setup) and its self-test" \
        "CXX=$toolchain/bin/clang++ CXXFLAGS='--target=$triple --sysroot=$sysroot -fuse-ld=lld' \
             sh builder/linux/setup_ui/build.sh $s/tools/usr/bin/longshot-setup $sdl2/include/SDL2 \
                 $s/tools/usr/lib/longshot/sdl2/libSDL2-2.0.so.0 &&
         rm -rf $s/ui-selftest && mkdir -p $s/ui-selftest && printf x > $s/ui-selftest/game.nup &&
         LONGSHOT_TOOLS=/work/src SDL_VIDEODRIVER=offscreen HOME=$s/ui-selftest/home \
             LONGSHOT_SETUP_BUILD=/work/src/builder/linux/setup_ui/stand_in_build.sh \
             $run_target $s/tools/usr/bin/longshot-setup --selftest $s/ui-selftest --size 1280x800 \
                 --nup $s/ui-selftest/game.nup --out $s/ui-selftest/out > $s/ui-selftest/selftest.txt &&
         tail -n 1 $s/ui-selftest/selftest.txt &&
         grep -q '@@done' $s/ui-selftest/logs/build-*.log"
    # (python-build-standalone's own pip, binary wheels only; the button-prompt packages are optional, as for /venv. Its
    # tests, IDLE and Tk go, as scripts/linux/deps.sh leaves them out: the setup window is longshot-setup, above)
    in_shell "its Python (python-build-standalone with requirements.txt)" \
        "tar -xzf $roots/setupPython -C $s/deps && P=$s/deps/python/bin/python3 &&
         grep -v '^-r ' requirements.txt > /tmp/requirements-core.txt &&
         \$P -m pip install -q --no-cache-dir --disable-pip-version-check --only-binary :all: \
             -r /tmp/requirements-core.txt &&
         { \$P -m pip install -q --no-cache-dir --disable-pip-version-check --only-binary :all: \
               -r nx2/prompt_requirements.txt ||
           echo 'WARNING: the button-prompt packages did not install for $LONGSHOT_ARCH:' \
                'the game shows the Switch icons'; } &&
         (cd $s/deps/python/lib && rm -rf python3*/test python3*/idlelib python3*/tkinter python3*/turtledemo \
             python3*/lib-dynload/_tkinter* libtcl* libtk* tcl[0-9]* tk[0-9]*) &&
         find $s/deps/python -name __pycache__ -prune -exec rm -rf {} + &&
         cp $roots/appimagetool $s/deps/appimagetool && chmod +x $s/deps/appimagetool &&
         cp $roots/appimageRuntime $s/deps/runtime-$LONGSHOT_ARCH && \$P --version"
    # (the build's own variables left out: appimage.sh takes the tools above, the compiler from their emsdk)
    in_shell "the setup AppImage (scripts/linux/appimage.sh, with the build tools inside it)" \
        "cd /work/src && rm -f build-linux/Longshot-*-$LONGSHOT_ARCH.AppImage &&
         env -u LONGSHOT_SYSROOT -u LONGSHOT_FFMPEG -u LONGSHOT_SDL2 -u LONGSHOT_VULKAN_HEADERS \
             -u LONGSHOT_SPIRV_HEADERS -u LONGSHOT_VMA_HEADERS DEPS=$s/deps LONGSHOT_APPIMAGE_TOOLS=$s/tools \
             ${LONGSHOT_MARCH:+RX_MARCH=$LONGSHOT_MARCH} bash scripts/linux/appimage.sh &&
         cp build-linux/Longshot-*-$LONGSHOT_ARCH.AppImage /out/ && ls -l /out/Longshot-*-$LONGSHOT_ARCH.AppImage"
    exit 0
fi

# ---- test
if [ "$mode" = test ]; then
    failed=""
    in_shell "test: tests/run_all.py (the checks that need no game files)" \
        "cd /work/src && py tests/run_all.py" || failed="$failed tests/run_all.py"
    # (make_game.py runs the lift with OOT_WORK = its work folder and OOT_DUMP = the game folder, which keeps the exefs)
    if [ -d /work/game ] && [ -d "/out/longshot-$LONGSHOT_ARCH/exefs" ]; then
        in_shell "test: nx2/test_insn.py (every instruction form the game uses, against Unicorn)" \
            "cd /work/src && OOT_WORK=/work/game OOT_DUMP=/out/longshot-$LONGSHOT_ARCH py nx2/test_insn.py" ||
            failed="$failed nx2/test_insn.py"
    else
        echo "==> The tests that need the game run after a game build (its folder in LONGSHOT_OUT)" | tee -a "$log"
    fi
    if [ -n "$failed" ]; then echo "==> FAILED:$failed" | tee -a "$log"; exit 3; fi
    echo "==> Tests passed" | tee -a "$log"
    exit 0
fi

# ---- game: nx2/make_game.py on the read-only game, into /out. Its work folder is in the work volume (not a RAM file
# system, as the development guide asks), and keeps the lifted C and the objects for the next run
in_shell "the game (nx2/make_game.py: lift, build, the folder longshot-$LONGSHOT_ARCH in LONGSHOT_OUT)" \
    "cd /work/src && LONGSHOT_KEEP_BUILD_FILES=1 py nx2/make_game.py --game '$LONGSHOT_GAME_IN' \
         --out /out/longshot-$LONGSHOT_ARCH --work /work/game --jobs $LONGSHOT_JOBS --no-prepare-shaders \
         ${LONGSHOT_LANGUAGE:+--language $LONGSHOT_LANGUAGE} --log /logs/make_game.log &&
     du -sh /out/longshot-$LONGSHOT_ARCH"
INNER_SCRIPT

tty_flags=()
if [ -t 0 ] && [ -t 1 ]; then tty_flags=(-it); fi

docker run --rm ${tty_flags[@]+"${tty_flags[@]}"} \
    --platform "$PLATFORM" \
    -v "$NIX_VOLUME":/nix -v "$SRC":/src -v "$LOGS":/logs \
    -v "longshot-$ARCH-work":/work -v "longshot-$ARCH-venv":/venv \
    ${game_flags[@]+"${game_flags[@]}"} \
    -e LONGSHOT_ARCH="$ARCH" -e LONGSHOT_MARCH="$MARCH" -e LONGSHOT_LANGUAGE="${LONGSHOT_LANGUAGE:-}" \
    -e LONGSHOT_OWNER="$OWNER" -e LONGSHOT_JOBS="$JOBS" -e GITHUB_TOKEN="${GITHUB_TOKEN:-}" \
    -e SDL_VIDEO_DRIVER=dummy -e SDL_AUDIO_DRIVER=dummy \
    "$IMAGE" bash -c "$INNER" inner "$MODE"

case $MODE in
    game) echo "==> Done: the folder longshot-$ARCH in $OUT. Copy it to the device and start ./longshot there." ;;
    setup) echo "==> Done: Longshot-<version>-$ARCH.AppImage in $OUT. Copy it to the device with your game and run it" \
                "there: --cli --nup <game.nup> --out ~/Games/Longshot builds the game; --play starts it." ;;
    tools) echo "==> Done: the $ARCH build tools are ready in the Docker volumes." ;;
esac
