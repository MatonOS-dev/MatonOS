#!/usr/bin/env bash
# NDK build of the bionic fork; Soong uses the same six upstream sources.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MATON_AOSP:?Set MATON_AOSP to the matching AOSP checkout}"
WORK=${MATON_FLATPAK_NDK:-$MATON_AOSP/out/matonos/flatpak-ndk}
BUILD=${MATON_BWRAP_BUILD:-$HERE/build/ndk}
JOBS=${MATON_BUILD_JOBS:-4}
[[ $JOBS =~ ^[1-4]$ ]] || { echo 'Use 1 to 4 jobs' >&2; exit 1; }
[[ $(git -C "$HERE/upstream" rev-parse HEAD) == 719a4fd474d44b26906bcf2b1b0fb6eddd8d56d0 ]]
python3 "$HERE/../verify-source.py" bubblewrap "$HERE/upstream"
setup=()
[[ ! -f $BUILD/meson-private/coredata.dat ]] || setup+=(--reconfigure)
meson setup "${setup[@]}" "$BUILD" "$HERE/upstream" \
  --cross-file "$WORK/android-x86_64-api35.ini" --prefix /apex/com.matonos.flatpak \
  --wrap-mode=nofallback -Dselinux=disabled -Dman=disabled -Dtests=false \
  -Dbash_completion=disabled -Dzsh_completion=disabled -Dc_args=-march=x86-64-v2
meson compile -C "$BUILD" -j "$JOBS"
# Do not stage into the shared AOSP product output from a worktree build.
