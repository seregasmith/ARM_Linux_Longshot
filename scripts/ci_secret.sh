#!/usr/bin/env bash
# Sealed CI secrets (docs/dev/reference/ci.md, Secrets only our runners can open). A Forgejo secret holds a secret's
# text sealed (AES-256, a key from PBKDF2), and the key to open it, seal.key, is a file only on the runner kept for the
# jobs that deploy and release (label `deploy`), never in Forgejo. So a runner that takes such a job without that file,
# anyone else's, gets text it can't open; and a job that has the file but not the secret (a fork's pull request) has
# nothing to open.
#
#   scripts/ci_secret.sh seal --key <seal.key> < secret.txt    the sealed text, for the Forgejo secret (on a PC you trust)
#   scripts/ci_secret.sh unseal < sealed.txt                   the secret, in a job (the key at $SEAL_KEY)
#   scripts/ci_secret.sh newkey > seal.key                     a new seal.key
set -euo pipefail

SEAL_KEY=${SEAL_KEY:-/run/magicbean-secrets/seal.key}
cipher=(-aes-256-cbc -pbkdf2 -iter 600000 -md sha256)

case ${1:-} in
  seal)
    [[ ${2:-} == --key && -n ${3:-} ]] || { echo "usage: $0 seal --key <seal.key> < secret" >&2; exit 2; }
    openssl enc -e "${cipher[@]}" -salt -a -A -pass "file:$3"
    echo
    ;;
  unseal)
    if [[ ! -r $SEAL_KEY ]]; then
      echo "::error::no $SEAL_KEY: this job runs only on the runner kept for deploys and releases (label deploy, docs/dev/reference/ci.md)" >&2
      exit 1
    fi
    sealed=$(cat)
    [[ -n $sealed ]] || { echo "::error::the sealed secret is empty: not set, or a run that gets no secrets (a fork's pull request)" >&2; exit 1; }
    printf '%s\n' "$sealed" | openssl enc -d "${cipher[@]}" -a -A -pass "file:$SEAL_KEY"
    ;;
  newkey)
    openssl rand -hex 32
    ;;
  *)
    echo "usage: $0 seal --key <seal.key> | unseal | newkey" >&2
    exit 2
    ;;
esac
