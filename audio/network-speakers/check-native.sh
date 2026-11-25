#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# NDK checks only; no Soong, Gradle, APEX signing, VM, or system-file writes.
set -Eeuo pipefail
[[ $# == 6 ]] || { echo "usage: $0 AOSP NDK PW_SOURCE PW_BUILD PW_PREFIX OUTPUT" >&2; exit 2; }
AOSP=$(realpath "$1"); NDK=$(realpath "$2"); SOURCE=$(realpath "$3")
BUILD=$(realpath "$4"); PREFIX=$(realpath "$5"); OUTPUT=$(realpath -m "$6")
HERE=$(dirname "$(readlink -f "$0")")
CMAKE=${MATON_CMAKE:-$AOSP/prebuilts/cmake/linux-x86/bin/cmake}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang
mkdir -p "$OUTPUT"
"$CMAKE" -S "$AOSP/external/boringssl/src" -B "$OUTPUT/boringssl" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-35 -DANDROID_STL=c++_static \
  -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_BUILD_TYPE=Release
"$CMAKE" --build "$OUTPUT/boringssl" --target crypto --parallel "$(nproc)"
LIBDIR=$PREFIX/lib64; [[ -d $LIBDIR ]] || LIBDIR=$PREFIX/lib
includes=(-I"$PREFIX/include/pipewire-0.3" -I"$PREFIX/include/spa-0.2")
flags=(-std=gnu11 -fPIC -Wall -Wextra -Werror)
"$CC" "${flags[@]}" "${includes[@]}" -c "$HERE/NetworkSpeakers.c" -o "$OUTPUT/NetworkSpeakers.o"
for pair in NetworkSpeakerModules:maton-network-speakers module-maton-snapcast:maton-snapcast; do
  "$CC" "${flags[@]}" "${includes[@]}" -shared -Wl,--no-undefined \
    "$HERE/${pair%%:*}.c" -L"$LIBDIR" -lpipewire-0.3 -llog \
    -o "$OUTPUT/libpipewire-module-${pair##*:}.so"
done
"$CC" -std=gnu11 -fPIC -shared -Wall -Werror -Wno-unused-function -Wno-unused-parameter \
  -Wl,--no-undefined -include "${BIONIC_FILL_H:?set BIONIC_FILL_H to bionic-fill.h}" \
  -I"$AOSP/external/boringssl/src/include" -I"$BUILD" -I"$SOURCE/src" -I"$SOURCE/spa/include" \
  -I"$PREFIX/include/pipewire-0.3" \
  "$SOURCE/src/modules/module-raop-sink.c" "$SOURCE/src/modules/module-raop/rtsp-client.c" \
  "$BUILD/src/modules/libpipewire-module-rtp-common-lib.a" "$OUTPUT/boringssl/libcrypto.a" \
  -L"$LIBDIR" -lpipewire-0.3 -lm -ldl -lc++_static -lc++abi \
  -o "$OUTPUT/libpipewire-module-raop-sink.so"
echo 'PASS: HAL transport compiled; network controller, Snapcast and RAOP linked with --no-undefined'
