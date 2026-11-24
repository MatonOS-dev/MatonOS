# Resolve the canonical AOSP checkout from paths.md for linked git worktrees.
maton_resolve_aosp_root() {
  if [[ -n ${MATON_AOSP_ROOT:-} && -f ${MATON_AOSP_ROOT}/paths.md ]]; then
    readlink -f "$MATON_AOSP_ROOT"
    return
  fi
  local common canonical checkout_root recorded
  common=$(git -C "$1" rev-parse --git-common-dir 2>/dev/null || true)
  if [[ -n $common ]]; then
    common=$(readlink -f "$1/$common" 2>/dev/null || readlink -f "$common" 2>/dev/null || true)
    canonical=$(dirname "$common")
    checkout_root=$(readlink -f "$canonical/../../.." 2>/dev/null || true)
    if [[ -f $checkout_root/paths.md ]]; then
      recorded=$(sed -n 's/^aosp: //p' "$checkout_root/paths.md" | head -n 1)
      if [[ -n $recorded && -f $recorded/paths.md ]]; then
        readlink -f "$recorded"
        return
      fi
    fi
  fi
  readlink -f "$1/../../.."
}
