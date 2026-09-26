# local-env.sh: sourced by MatonOS host scripts. Sets MATON_ROOT (the AOSP
# checkout, derived from this file's location) and loads per-machine settings
# from $MATON_ROOT/matonos.local.env (git-ignored) when present, e.g.
#   ANDROID_NDK=/opt/android-ndk-r30
#   MATON_ANDROID_SDK=/opt/matonos-android-sdk
MATON_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
if [[ -f $MATON_ROOT/matonos.local.env ]]; then
  set -a; . "$MATON_ROOT/matonos.local.env"; set +a
fi
