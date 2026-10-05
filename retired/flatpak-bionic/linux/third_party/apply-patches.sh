#!/usr/bin/env bash
# Compatibility entrypoint: touched components now carry changes in their forks.
# Refuse stale or modified sources instead of applying the retired patch series.
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
python3 "$ROOT/verify-source.py" bubblewrap "$ROOT/bubblewrap/upstream"
python3 "$ROOT/verify-source.py" flatpak "$ROOT/flatpak/upstream"
