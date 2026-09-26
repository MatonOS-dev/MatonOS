#!/usr/bin/env bash
# Generate the frozen audio AIDL NDK backend and build our vendored AIDL HAL.
set -Eeuo pipefail
die() { echo "ERROR: $*" >&2; exit 1; }
DEVICE_DIR=$(cd "$(dirname "$0")/.." && pwd)
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
. "$(dirname -- "$(readlink -f -- "$0")")/../tools/local-env.sh"
NDK=${ANDROID_NDK:-$HOME/Documents/android-ndk-r30}
CMAKE=${MATON_CMAKE:-$AOSP/prebuilts/cmake/linux-x86/bin/cmake}
NINJA=${MATON_NINJA:-$(command -v ninja || true)}
AIDL=$AOSP/out/host/linux-x86/bin/aidl
OUT=${MATON_AUDIO_OUT:-$AOSP/out/pc-native/maton-audio-hal}
API=35
JOBS=${MATON_BUILD_JOBS:-4}
GEN=$OUT/aidl
STATIC_LIBS=$DEVICE_DIR/audio/native-hal/deps/lib64
DEPS_INCLUDE=$DEVICE_DIR/audio/native-hal/deps/include

[[ $JOBS =~ ^[1-4]$ ]] || die "MATON_BUILD_JOBS must be between 1 and 4"
[[ -x $AIDL && -x $CMAKE && -n $NINJA ]] || die "AOSP aidl, CMake or Ninja missing"
(cd "$DEVICE_DIR/audio/native-hal" && sha256sum -c SHA256SUMS >/dev/null) ||
  die "pinned audio HAL snapshot checksum mismatch; review changes and refresh SHA256SUMS"
[[ -x $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android$API-clang ]] ||
  die "NDK r30 / API $API is required"
mkdir -p "$GEN/include" "$STATIC_LIBS"
rm -f "$STATIC_LIBS/libmedia_helper.a" \
      "$STATIC_LIBS/libaudio_aidl_conversion_common_ndk.a"
TP=$DEVICE_DIR/audio/native-hal/third_party
include_roots=(
  "$TP/hardware_interfaces/hardware_interfaces/audio/aidl/common/include"
  "$TP/frameworks_av/frameworks/av/media/audioaidlconversion/include"
  "$TP/frameworks_av/frameworks/av/media/libmediahelper/include"
  "$TP/frameworks_av/frameworks/av/media/liberror/include"
  "$TP/frameworks_native/frameworks/native/libs/binder/ndk/include_cpp"
  "$TP/frameworks_native/frameworks/native/libs/binder/ndk/include_ndk"
  "$TP/frameworks_native/frameworks/native/libs/binder/ndk/include_platform"
  "$TP/system_core/system/core/include"
  "$TP/system_core/system/core/libutils/include"
  "$TP/system_core/system/core/libutils/binder/include"
  "$TP/system_core/system/core/libcutils/include"
  "$TP/system_core/system/core/libcutils/include_outside_system"
  "$TP/system_libbase/system/libbase/include"
  "$TP/system_fmq/system/libfmq/include"
  "$TP/system_fmq/system/libfmq/base"
  "$TP/system_media/system/media/audio/include"
  "$TP/system_logging/system/logging/liblog/include"
  "$TP/hardware_libhardware/hardware/libhardware/include_all"
  "$TP/fmtlib/external/fmtlib/include"
)
for include_root in "${include_roots[@]}"; do
  [[ -d $include_root ]] || die "pinned header snapshot missing: $include_root"
done
header_hash=$(find -L "${include_roots[@]}" -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
header_hash_file=$OUT/deps-include.sha256
if [[ ! -s $header_hash_file || $(cat "$header_hash_file") != "$header_hash" ]]; then
  rm -rf "$DEPS_INCLUDE"
  mkdir -p "$DEPS_INCLUDE"
  for include_root in "${include_roots[@]}"; do cp -RL "$include_root/." "$DEPS_INCLUDE/"; done
  printf '%s\n' "$header_hash" > "$header_hash_file"
fi

snapshot_root=$DEVICE_DIR/audio/native-hal/aidl-snapshots
roots=()
entries=()
while IFS=$'\t' read -r package version relative; do
  [[ -n $package && $package != \#* ]] || continue
  snapshot=$snapshot_root/$relative
  [[ -s $snapshot/.hash ]] || die "pinned AIDL snapshot/hash missing: $snapshot"
  roots+=(-I "$snapshot")
  entries+=("$package"$'\t'"$version"$'\t'"$snapshot")
done < "$DEVICE_DIR/audio/native-hal/stable-aidl.list"
aidl_inputs_hash=$(find "$snapshot_root" -type f \( -name '*.aidl' -o -name '.hash' \) -print0 |
  sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
aidl_stamp=$({
  sha256sum "$AIDL" "$DEVICE_DIR/audio/native-hal/stable-aidl.list"
  printf '%s\n' "$aidl_inputs_hash" 'lang=ndk min_sdk=31 structured=1 stability=vintf'
} | sha256sum | cut -d' ' -f1)
if [[ ! -s $GEN/.maton-aidl-stamp || $(cat "$GEN/.maton-aidl-stamp") != "$aidl_stamp" ]]; then
  rm -rf "$GEN"
  mkdir -p "$GEN/include"
  for entry in "${entries[@]}"; do
    IFS=$'\t' read -r package version snapshot <<< "$entry"
    hash=$(cat "$snapshot/.hash")
    while IFS= read -r source; do
      "$AIDL" --lang=ndk --min_sdk_version=31 --structured --stability=vintf --version="$version" \
        --hash="$hash" --out="$GEN" --header_out="$GEN/include" \
        "${roots[@]}" "$source"
    done < <(find "$snapshot" -type f -name '*.aidl' -print | sort)
  done
  printf '%s\n' "$aidl_stamp" > "$GEN/.maton-aidl-stamp"
fi

"$CMAKE" -S "$DEVICE_DIR/audio/native-hal" -B "$OUT/build" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-$API \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/ -DCMAKE_INSTALL_RPATH='$ORIGIN/../../lib64' \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
  -DMATON_AIDL_GENERATED_DIR="$GEN" \
  -DMATON_AUDIO_STATIC_LIB_DIR="$STATIC_LIBS" \
  -DMATON_AUDIO_DEPS_INCLUDE_DIR="$DEPS_INCLUDE"
"$CMAKE" --build "$OUT/build" --parallel "$JOBS"
for archive in libfmt.a libbase.a libutils_binder.a libutils.a libcutils.a \
               libfmq.a libaudioaidlcommon.a; do
  [[ -s $STATIC_LIBS/$archive ]] || die "static dependency was not produced: $archive"
done
READELF=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
[[ -x $READELF ]] || die "NDK llvm-readelf is missing"
[[ -x $STRIP ]] || die "NDK llvm-strip is missing"
mapfile -t needed < <("$READELF" -d "$OUT/build/matonos-audio-hal" |
  sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' | sort -u)
for soname in "${needed[@]}"; do
  case "$soname" in
    libc.so|libm.so|libdl.so|liblog.so|libbinder_ndk.so|libc++_shared.so) ;;
    *) die "unexpected dynamic dependency in HAL: $soname" ;;
  esac
done
mapfile -t undefined < <("$READELF" --dyn-syms --wide "$OUT/build/matonos-audio-hal" |
  awk '$7 == "UND" && $8 != "" {sub(/@.*/, "", $8); print $8}' | sort -u)
SYSROOT=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot
IMPORT_DIR=$SYSROOT/usr/lib/x86_64-linux-android/$API
IMPORTS=$OUT/allowed-imports.txt
: > "$IMPORTS"
for library in libc.so libm.so libdl.so liblog.so libbinder_ndk.so; do
  [[ -s $IMPORT_DIR/$library ]] || die "NDK API-$API import stub missing: $library"
  "$READELF" --dyn-syms --wide "$IMPORT_DIR/$library" |
    awk '$7 != "UND" && $8 != "" {sub(/@.*/, "", $8); print $8}' >> "$IMPORTS"
done
sort -u -o "$IMPORTS" "$IMPORTS"
for symbol in "${undefined[@]}"; do
  if ! grep -Fxq "$symbol" "$IMPORTS"; then
    case "$symbol" in
      # Platform HAL Binder service/scheduling exports are in libbinder_ndk.so,
      # but omitted from NDK import stubs because app callers cannot use them.
      AIBinder_Class_setHandleShellCommand|\
      ABinderProcess_setThreadPoolMaxThreadCount|ABinderProcess_startThreadPool|\
      ABinderProcess_joinThreadPool|AServiceManager_addService|\
      AIBinder_setMinSchedulerPolicy|AIBinder_setInheritRt) ;;
      *) die "unresolved symbol is outside the NDK imports and HAL Binder allowlist: $symbol" ;;
    esac
  fi
done
install -D -m 0755 "$OUT/build/matonos-audio-hal" \
  "$DEVICE_DIR/buildinfra/native-built/matonos-audio-hal"
"$STRIP" --strip-debug "$DEVICE_DIR/buildinfra/native-built/matonos-audio-hal"
install -D -m 0644 "$DEVICE_DIR/audio/native-hal/vintf/manifest.xml" \
  "$DEVICE_DIR/buildinfra/native-built/vintf/matonos-audio-hal/manifest.xml"
