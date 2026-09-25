#!/usr/bin/env bash
# apply-patches.sh: apply pc_x86_64's patches to AOSP projects.
#
# Patches live in <device dir>/patches/<project path>/*.patch, e.g.
# patches/<project path>/NNNN-fix.patch applies to <aosp>/<project path>.
# They're applied as uncommitted changes with `git apply`, in name order.
# Already-applied patches are skipped, so this is safe to run before every
# build. `repo sync` refuses to update projects with local changes; revert
# with `git -C <project> checkout .` first, then re-run this script.
#
# Usage: ./apply-patches.sh [-a <aosp dir>] [-c]   (-c: check only)
set -Eeuo pipefail

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
CHECK=0
while getopts "a:ch" opt; do
  case $opt in
    a) AOSP=$OPTARG ;;
    c) CHECK=1 ;;
    *) sed -n '2,13p' "$0"; exit 1 ;;
  esac
done

PATCHES=$DEVICE_DIR/patches
[[ -d $PATCHES ]] || { info "No patches"; exit 0; }

failed=0
while IFS= read -r -d '' patch; do
  rel=${patch#"$PATCHES"/}
  project=$(dirname "$rel")
  dir=$AOSP/$project
  [[ -d $dir/.git || -f $dir/.git ]] || die "$project is not a git project in $AOSP"
  if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
    info "already applied: $rel"
  elif git -C "$dir" apply --check "$patch" 2>/dev/null; then
    if [[ $CHECK == 1 ]]; then
      info "would apply: $rel"
    else
      git -C "$dir" apply "$patch"
      info "applied: $rel"
    fi
  else
    echo "ERROR: $rel does not apply to $project (upstream changed?)" >&2
    failed=1
  fi
done < <(find "$PATCHES" -name '*.patch' -print0 | sort -z)

(( failed == 0 )) || die "some patches failed"
