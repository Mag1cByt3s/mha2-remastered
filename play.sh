#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -x ./mha2_linux ]; then
  nix-shell --run "make -j\$(nproc)"
fi
cmd="./mha2_linux"
for a in "$@"; do
  cmd+=" $(printf '%q' "$a")"
done
exec nix-shell --run "$cmd"
