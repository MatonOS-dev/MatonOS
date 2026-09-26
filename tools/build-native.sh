#!/usr/bin/env bash
# Build MatonOS daemons with the pinned NDK, outside Soong. Each daemon owns
# native/<name>/CMakeLists.txt; the shared MatonOS IPC library is built first.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
. "$(dirname -- "$(readlink -f -- "$0")")/local-env.sh"
NDK=${ANDROID_NDK:-$HOME/Documents/android-ndk-r30}
CMAKE=${MATON_CMAKE:-$AOSP/prebuilts/cmake/linux-x86/bin/cmake}
NINJA=${MATON_NINJA:-$(command -v ninja || true)}
OUT=${MATON_NATIVE_OUT:-$AOSP/out/pc-native}
API=35
JOBS=${MATON_BUILD_JOBS:-4}

while getopts 'n:a:o:j:c:h' opt; do
  case $opt in
    n) NDK=$OPTARG ;;
    a) AOSP=$OPTARG ;;
    o) OUT=$OPTARG ;;
    j) JOBS=$OPTARG ;;
    c) CMAKE=$OPTARG ;;
    *) sed -n '2,19p' "$0"; exit 1 ;;
  esac
done
[[ $JOBS =~ ^[1-4]$ ]] || die "MATON_BUILD_JOBS must be between 1 and 4"
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
[[ -x $TOOLCHAIN/bin/x86_64-linux-android$API-clang ]] || die "NDK r30/API $API missing at $NDK"
[[ -x $CMAKE ]] || die "CMake missing; expected AOSP prebuilt at $CMAKE"
[[ -n $NINJA && -x $NINJA ]] || die "Ninja is required"
AIDL=$AOSP/out/host/linux-x86/bin/aidl
[[ -x $AIDL ]] || die "AOSP aidl host tool missing at $AIDL; build it once with m aidl"
BINDER_NDK_CPP=$DEVICE_DIR/native/third_party/binder_ndk_cpp
[[ -s $BINDER_NDK_CPP/android/binder_auto_utils.h &&
   -s $BINDER_NDK_CPP/android/binder_interface_utils.h &&
   -s $BINDER_NDK_CPP/PROVENANCE.md ]] || die "pinned Binder NDK C++ helper headers are missing"
BINDER_PLATFORM_INCLUDE=$AOSP/frameworks/native/libs/binder/ndk/include_platform
BINDER_NDK_LIB=$AOSP/out/soong/.intermediates/frameworks/native/libs/binder/ndk/libbinder_ndk/android_vendor_x86_64_shared/libbinder_ndk.so
[[ -s $BINDER_PLATFORM_INCLUDE/android/binder_manager.h ]] || die "AOSP Binder NDK platform headers are missing"
[[ -s $BINDER_NDK_LIB ]] || die "AOSP vendor libbinder_ndk missing at $BINDER_NDK_LIB; request a coordinator image build"
IPC_SRC=$DEVICE_DIR/native/libmatonos-ipc
[[ -f $IPC_SRC/CMakeLists.txt ]] || die "shared native/libmatonos-ipc source is missing"
mkdir -p "$OUT" "$DEVICE_DIR/prebuilt/native" "$DEVICE_DIR/buildinfra/native-built"

# Generate the fixed stable VINTF channel interface before the shared NDK helper.
IPC_STABLE_CONFIG=$IPC_SRC/stable-aidl.list
IPC_STABLE_GEN=$OUT/libmatonos-ipc/aidl-generated/stable
mkdir -p "$IPC_STABLE_GEN/include"
while IFS=$'\t' read -r package version source_root; do
  [[ -n $package && $package != \#* ]] || continue
  snapshot=$AOSP/$source_root/aidl_api/$package/$version
  [[ -d $snapshot && -s $snapshot/.hash ]] || die "frozen stable AIDL snapshot or hash missing: $snapshot"
  hash=$(cat "$snapshot/.hash")
  while IFS= read -r aidl_file; do
    [[ -n $aidl_file ]] || continue
    "$AIDL" --lang=ndk --structured --stability=vintf --version="$version" --hash="$hash" \
      --out="$IPC_STABLE_GEN" --header_out="$IPC_STABLE_GEN/include" -I "$snapshot" "$aidl_file"
  done < <(find "$snapshot" -type f -name '*.aidl' -print | sort)
done < "$IPC_STABLE_CONFIG"
# Binder NDK channel implementation; daemon code links the staged shared object.
IPC_BUILD=$OUT/libmatonos-ipc
"$CMAKE" -S "$IPC_SRC" -B "$IPC_BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/ \
  "-DCMAKE_CXX_FLAGS=-I$BINDER_NDK_CPP -I$BINDER_PLATFORM_INCLUDE" \
  -DCMAKE_C_FLAGS=-I$BINDER_NDK_CPP \
  -DMATON_BINDER_NDK_LIB="$BINDER_NDK_LIB" \
  -DMATON_STABLE_AIDL_GENERATED_DIR="$IPC_STABLE_GEN" \
  -DMATON_STABLE_AIDL_INCLUDE_DIR="$IPC_STABLE_GEN/include" \
  -DMATON_STABLE_AIDL_COUNT=1
"$CMAKE" --build "$IPC_BUILD" --parallel "$JOBS"
ipc_built=$(find "$IPC_BUILD" -type f -name 'libmatonos-ipc.so' -print -quit)
[[ -n $ipc_built && -s $ipc_built ]] || die "libmatonos-ipc.so was not produced"
install -m 0755 "$ipc_built" "$DEVICE_DIR/prebuilt/native/libmatonos-ipc.so"
install -m 0755 "$ipc_built" "$DEVICE_DIR/buildinfra/native-built/libmatonos-ipc.so"

# Audio HAL is an out-of-Soong NDK build; keep its toolchain/output settings
# aligned with the native daemons above. It emits its executable + VINTF into
# buildinfra/native-built for the ODM bundle.
# Opt-in until the HAL's static dependency closure builds (coordinator,
# 2026-09-25): MATON_AUDIO_HAL=1 enables it; the ODM bundle entries in
# bundle/contents.list are commented out until then too.
if [[ ${MATON_AUDIO_HAL:-0} == 1 ]]; then
  ANDROID_NDK="$NDK" MATON_CMAKE="$CMAKE" MATON_NINJA="$NINJA" \
    MATON_NATIVE_OUT="$OUT/maton-audio-hal" MATON_BUILD_JOBS="$JOBS" \
    bash "$DEVICE_DIR/audio/build-hal.sh"
fi

found=0
for project in "$DEVICE_DIR"/native/*; do
  [[ -d $project && -f $project/CMakeLists.txt ]] || continue
  [[ $(basename "$project") == libmatonos-ipc ]] && continue
  found=1
  name=$(basename "$project")
  build=$OUT/$name
  gen=$build/aidl-generated
  mkdir -p "$gen/include"
  stable_aidl_config=$project/stable-aidl.list
  stable_aidl_packages=()
  stable_aidl_include_args=()
  if [[ -s $stable_aidl_config ]]; then
    while IFS=$'\t' read -r package version source_root; do
      [[ -n $package && $package != \#* ]] || continue
      [[ $package =~ ^[a-zA-Z][a-zA-Z0-9_.]*$ && $version =~ ^[1-9][0-9]*$ &&
         $source_root =~ ^[a-zA-Z0-9_./-]+$ && $source_root != *..* ]] ||
        die "bad stable AIDL row in $stable_aidl_config: $package"
      snapshot=$AOSP/$source_root/aidl_api/$package/$version
      [[ -d $snapshot && -s $snapshot/.hash ]] ||
        die "frozen stable AIDL snapshot or hash missing: $snapshot"
      for existing in "${stable_aidl_packages[@]}"; do
        [[ ${existing%%$'\t'*} != "$package" ]] ||
          die "duplicate stable AIDL package $package in $stable_aidl_config"
      done
      stable_aidl_packages+=("$package"$'\t'"$version"$'\t'"$snapshot")
      stable_aidl_include_args+=(-I "$snapshot")
    done < "$stable_aidl_config"
  fi
  aidl_args=()
  while IFS= read -r aidl_file; do
    [[ -n $aidl_file ]] || continue
    aidl_args+=("$aidl_file")
  done < <(find "$project/aidl" -type f -name '*.aidl' -print 2>/dev/null | sort || true)
  if (( ${#aidl_args[@]} )); then
    info "Generating NDK AIDL for $name"
    for file in "${aidl_args[@]}"; do
      "$AIDL" --lang=ndk --structured --stability=vintf \
        --out="$gen" --header_out="$gen/include" \
      -I "$project/aidl" "${stable_aidl_include_args[@]}" "$file"
    done
  fi
  if (( ${#stable_aidl_packages[@]} )); then
    stable_gen=$gen/stable
    mkdir -p "$stable_gen/include"
    for entry in "${stable_aidl_packages[@]}"; do
      IFS=$'\t' read -r package version snapshot <<<"$entry"
      hash=$(cat "$snapshot/.hash")
      info "Generating frozen stable NDK AIDL $package@$version for $name"
      while IFS= read -r aidl_file; do
        [[ -n $aidl_file ]] || continue
        "$AIDL" --lang=ndk --structured --stability=vintf \
          --version="$version" --hash="$hash" \
          --out="$stable_gen" --header_out="$stable_gen/include" \
          "${stable_aidl_include_args[@]}" "$aidl_file"
      done < <(find "$snapshot" -type f -name '*.aidl' -print | sort)
    done
  fi
  info "Configuring $name (x86_64, API $API)"
  "$CMAKE" -S "$project" -B "$build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/ \
    "-DCMAKE_CXX_FLAGS=-I$BINDER_NDK_CPP -I$BINDER_PLATFORM_INCLUDE" \
    -DCMAKE_C_FLAGS=-I$BINDER_NDK_CPP \
    -DMATON_BINDER_NDK_LIB="$BINDER_NDK_LIB" \
    -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib64' -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DMATON_AIDL_GENERATED_DIR="$gen" \
    -DMATON_AIDL_INCLUDE_DIR="$gen/include" \
    -DMATON_AIDL_SOURCE_DIR="$project/aidl" \
    -DMATON_AIDL_COUNT=${#aidl_args[@]} \
    -DMATON_STABLE_AIDL_GENERATED_DIR="$gen/stable" \
    -DMATON_STABLE_AIDL_INCLUDE_DIR="$gen/stable/include" \
    -DMATON_STABLE_AIDL_COUNT=${#stable_aidl_packages[@]} \
    -DMATON_IPC_INCLUDE_DIR="$IPC_SRC/include" \
    -DMATON_IPC_LIBRARY="$DEVICE_DIR/prebuilt/native/libmatonos-ipc.so"
  "$CMAKE" --build "$build" --parallel "$JOBS"
  install_dir=$OUT/install-$name
  python3 - "$install_dir" <<'PY'
import pathlib, shutil, sys
p=pathlib.Path(sys.argv[1])
if p.exists(): shutil.rmtree(p)
p.mkdir(parents=True)
PY
  DESTDIR="$install_dir" "$CMAKE" --install "$build"
  built=$(find "$install_dir" -type f -name "$name" -print -quit)
  [[ -n $built ]] || built=$(find "$build" -maxdepth 2 -type f -name "$name" -executable -print -quit)
  [[ -n $built && -x $built ]] || die "no executable output named $name from $project"
  install -m 0755 "$built" "$DEVICE_DIR/prebuilt/native/$name"
  install -m 0755 "$built" "$DEVICE_DIR/buildinfra/native-built/$name"
  if [[ -d $project/vintf ]]; then
    while IFS= read -r fragment; do
      [[ -n $fragment ]] || continue
      install -D -m 0644 "$fragment" \
        "$DEVICE_DIR/prebuilt/native/vintf/$name/$(basename "$fragment")"
      install -D -m 0644 "$fragment" \
        "$DEVICE_DIR/buildinfra/native-built/vintf/$name/$(basename "$fragment")"
    done < <(find "$project/vintf" -type f -name '*.xml' -print | sort)
  fi
done
(( found )) || die "no native daemon projects found"
info "Staged NDK daemons and libmatonos-ipc for x86_64 / API $API"
