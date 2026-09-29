#!/usr/bin/env bash
set -Eeuo pipefail

ROOT=$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)

apply_series() {
    local tree=$1 patch_file
    shift
    for patch_file in "$@"; do
        local reverse_output
        if reverse_output=$(cd "$tree" && patch --batch --dry-run --reverse -p1 < "$patch_file" 2>&1); then
            # GNU patch exits 0 after ignoring -R when the forward patch is
            # still unapplied; do not mistake that message for an applied patch.
            if [[ $reverse_output != *"Unreversed patch detected"* ]]; then
                continue
            fi
        fi
        if ! (cd "$tree" && patch --batch --dry-run --silent -p1 < "$patch_file"); then
            echo "Patch does not apply cleanly: $patch_file" >&2
            return 1
        fi
        (cd "$tree" && patch --batch --forward -p1 < "$patch_file")
    done
}

apply_series "$ROOT/bubblewrap/upstream" \
    "$ROOT/bubblewrap/patches/0001-c23-bool-guard.patch" \
    "$ROOT/bubblewrap/patches/0002-bionic-getcwd.patch" \
    "$ROOT/bubblewrap/patches/0003-bionic-root-bind-realpath.patch" \
    "$ROOT/bubblewrap/patches/0004-bionic-newroot-subpath-realpath.patch"
