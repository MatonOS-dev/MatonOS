# local-env.sh: sourced by MatonOS host scripts. Paths in paths.md are the
# machine-specific source of truth; git common-dir handles linked worktrees.
_MATON_TOOLS_DIR=$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")
. "$_MATON_TOOLS_DIR/lib/aosp-root.sh"
MATON_AOSP_ROOT=$(maton_resolve_aosp_root "$(dirname "$_MATON_TOOLS_DIR")")
MATON_ROOT=$MATON_AOSP_ROOT
if [[ -f $MATON_ROOT/matonos.local.env ]]; then
  set -a; . "$MATON_ROOT/matonos.local.env"; set +a
  MATON_AOSP_ROOT=${MATON_AOSP_ROOT:-$MATON_ROOT}
fi
export MATON_AOSP_ROOT MATON_ROOT
