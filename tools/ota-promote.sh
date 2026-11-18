#!/usr/bin/env bash
set -Eeuo pipefail
if (($# != 3)); then echo "usage: tools/ota-promote.sh <out-dir> <channel> <update_id>" >&2; exit 2; fi
exec python3 "$(dirname "$(readlink -f "$0")")/ota-promote.py" "$@"
