#!/usr/bin/env bash
# prune-tree.sh: hide AOSP projects MatonOS never builds from Soong/Kati.
#
# Usage: prune-tree.sh apply|revert|status
#
# Drops a `.find-ignore` marker into each directory of tools/prune-tree.list
# (Soong's file finder skips any directory containing one; see
# build/soong/ui/build/finder.go). Nothing is deleted and the repo checkout is
# untouched, so `repo sync` and apply-patches.sh are unaffected; `revert`
# removes the markers. Soong analysis then parses ~30% fewer Android.bp files.
#
# Tests elsewhere (e.g. frameworks/base) reference libraries in the pruned
# test suites, so builds of a pruned tree need ALLOW_MISSING_DEPENDENCIES=true
# (build.sh sets it while $AOSP/.maton-pruned exists): missing dependencies
# then only fail the modules that are actually built.
# Apply/revert only while no build runs: it changes the analysis inputs
# (one full re-analysis follows).
set -Eeuo pipefail

TOOLS=$(dirname "$(readlink -f "$0")")
AOSP=$(readlink -f "$TOOLS/../../../..")
LIST=$TOOLS/prune-tree.list
STAMP=$AOSP/.maton-pruned

mapfile -t DIRS < <(sed 's/#.*//; s/[[:space:]]*$//; /^$/d' "$LIST")
case ${1:-} in
  apply)
    for d in "${DIRS[@]}"; do
      if [[ -d $AOSP/$d ]]; then : > "$AOSP/$d/.find-ignore"; else echo "skip (missing): $d"; fi
    done
    printf '%s\n' "${DIRS[@]}" > "$STAMP"
    echo "pruned ${#DIRS[@]} projects"
    ;;
  revert)
    for d in "${DIRS[@]}"; do rm -f "$AOSP/$d/.find-ignore"; done
    # Also directories pruned by an older list.
    if [[ -f $STAMP ]]; then
      while read -r d; do rm -f "$AOSP/$d/.find-ignore"; done < "$STAMP"
      rm -f "$STAMP"
    fi
    echo "unpruned"
    ;;
  status)
    if [[ -f $STAMP ]]; then echo "pruned:"; sed 's/^/  /' "$STAMP"; else echo "not pruned"; fi
    ;;
  *) sed -n '2,/^set -E/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 1 ;;
esac
