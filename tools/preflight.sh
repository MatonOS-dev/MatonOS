#!/usr/bin/env bash
# preflight.sh: fast static checks before a coordinator-started AOSP build.
# Usage: MATON_BUILD_COORDINATOR=1 tools/preflight.sh
set -Eeuo pipefail

if [[ ${MATON_BUILD_COORDINATOR:-} != 1 ]]; then
  echo "ERROR: preflight is reserved for the coordinating agent; set MATON_BUILD_COORDINATOR=1." >&2
  exit 3
fi

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
exec python3 "$DEVICE_DIR/preflight/checks.py"
