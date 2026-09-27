#!/usr/bin/env bash
set -Eeuo pipefail

AREA_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
DEVICE_DIR=$(dirname "$AREA_DIR")
source "$DEVICE_DIR/tools/local-env.sh"

CMAKE=${MATON_CMAKE:-$MATON_ROOT/prebuilts/cmake/linux-x86/bin/cmake}
TOOLCHAIN=${ANDROID_NDK:?Set ANDROID_NDK in matonos.local.env}/build/cmake/android.toolchain.cmake
[[ -x $CMAKE ]] || { echo "missing CMake: $CMAKE" >&2; exit 1; }
[[ -f $TOOLCHAIN ]] || { echo "missing NDK CMake toolchain: $TOOLCHAIN" >&2; exit 1; }

build_dir=$(mktemp -d --tmpdir matonos-installer-core.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
"$CMAKE" -S "$AREA_DIR/service" -B "$build_dir" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-35 \
  -DCMAKE_BUILD_TYPE=Release
"$CMAKE" --build "$build_dir" --parallel 2
