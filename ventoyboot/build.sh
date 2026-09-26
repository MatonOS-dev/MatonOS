#!/usr/bin/env bash
set -Eeuo pipefail

[[ $# == 1 ]] || { echo "usage: $0 <output binary>" >&2; exit 2; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
NDK=${ANDROID_NDK:-$HOME/Documents/android-ndk-r30}
API=${MATON_NDK_API:-35}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CLANG=$TOOLCHAIN/bin/x86_64-linux-android$API-clang
STRIP=$TOOLCHAIN/bin/llvm-strip
[[ -x $CLANG ]] || { echo "NDK r30/API $API missing at $NDK" >&2; exit 1; }
"$CLANG" -O2 -static -Wall -Wextra -Werror "$DEVICE_DIR/ventoyboot/ventoyboot.c" -o "$1"
"$STRIP" "$1"
