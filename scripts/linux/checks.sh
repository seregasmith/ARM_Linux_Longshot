#!/bin/bash
# tests/run_all.py in a container with CI's tools (scripts/linux/checks.Dockerfile: Ubuntu 24.04, as .forgejo/workflows/ci.yml),
# on any Linux distribution. `tests/run_all.py --container` (`mise run test --container`) runs this with its other arguments:
#
#   scripts/linux/checks.sh --strict          every check
#   scripts/linux/checks.sh --strict ub_      any other argument of run_all.py's
#
# The first run builds the image (a few minutes, ~3 GB; it needs the network). The checks run as your user, with no network:
# the checkout is mounted at its own path, and, for a git worktree, the repository it belongs to, read-only. Podman, else
# Docker; DOCKER=<program> names one. CC and CXX are passed through when set.
set -euo pipefail
CALLER_DIR=$(pwd -P)
cd "$(dirname "$0")/../.."
ROOT=$(pwd -P)
# Resolve report paths before changing the working directory seen by the runner. Only the checkout is writable in the
# container; reject other destinations before building the image or starting the checks.
ARGS=("$@")
for ((i=0; i<${#ARGS[@]}; i++)); do
    case ${ARGS[i]} in
        --) break ;;
        --ju|--jun|--juni|--junit)
            ((i+=1))
            [ "$i" -lt "${#ARGS[@]}" ] || break
            [[ ${ARGS[i]} != -* ]] || continue
            prefix= ;;
        --ju=*|--jun=*|--juni=*|--junit=*)
            prefix=${ARGS[i]%%=*}=
            ARGS[i]=${ARGS[i]#*=} ;;
        *) continue ;;
    esac
    if [ -z "${ARGS[i]}" ]; then ARGS[i]=$prefix; continue; fi
    path=$(realpath -m -- "$CALLER_DIR/${ARGS[i]}")
    if [[ ${ARGS[i]} = /* ]]; then path=$(realpath -m -- "${ARGS[i]}"); fi
    if [[ $path != "$ROOT"/* ]]; then
        echo "checks.sh: --junit destination must be inside the checkout when using --container" >&2
        exit 2
    fi
    ARGS[i]=$prefix$path
done
IMAGE=${LONGSHOT_CHECKS_IMAGE:-longshot-checks}
DOCKER=${DOCKER:-}
for engine in podman docker; do
    if [ -z "$DOCKER" ] && command -v "$engine" >/dev/null; then DOCKER=$engine; fi
done
[ -n "$DOCKER" ] || { echo "checks.sh: neither podman nor docker is installed (DOCKER=<program> names another)" >&2; exit 2; }

"$DOCKER" build -t "$IMAGE" -f scripts/linux/checks.Dockerfile scripts >&2

RUN=(run --rm --init --network none -v "$ROOT:$ROOT" -w "$ROOT")
# (a git worktree's .git is a file that names the repository's, outside it: the checks that ask git need it)
if git=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null) && [[ $git/ != "$ROOT"/* ]]; then
    RUN+=(-v "$git:$git:ro")
fi
# (the user: as docker.sh, beside this, says)
if "$DOCKER" --version 2>/dev/null | grep -qi podman; then
    RUN+=(--userns=keep-id)
elif "$DOCKER" info --format '{{.SecurityOptions}}' 2>/dev/null | grep -q rootless; then
    :
else
    RUN+=(--user "$(id -u):$(id -g)")
fi
if [ "$(cat /sys/fs/selinux/enforce 2>/dev/null)" = 1 ]; then
    RUN+=(--security-opt label=disable)
fi
for v in CC CXX; do
    if [ -n "${!v:-}" ]; then RUN+=(-e "$v"); fi
done

exec "$DOCKER" "${RUN[@]}" "$IMAGE" python3 -I -B tests/run_all.py "${ARGS[@]}"
