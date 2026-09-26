#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
AOSP=$(cd "$HERE/../../../../" && pwd)
. "$(dirname -- "$(readlink -f -- "$0")")/../tools/local-env.sh"
NDK=${ANDROID_NDK:-$HOME/Documents/android-ndk-r30}
CMAKE=${MATON_CMAKE:-$AOSP/prebuilts/cmake/linux-x86/bin/cmake}
NINJA=${MATON_NINJA:-$(command -v ninja)}
OUT=$HERE/out
"$CMAKE" -S "$HERE" -B "$OUT/build" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-35 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/ \
  -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib64' -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
"$CMAKE" --build "$OUT/build" --parallel 4
DESTDIR="$OUT" "$CMAKE" --install "$OUT/build"
