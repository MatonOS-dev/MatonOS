#!/usr/bin/env bash
set -Eeuo pipefail
if (($# != 2)); then echo "usage: tools/ota-publish.sh <images-dir> <out-dir>" >&2; exit 2; fi
exec python3 "$(dirname "$(readlink -f "$0")")/ota-publish.py" "$@"
