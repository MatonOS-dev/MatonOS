#!/usr/bin/env bash
# build-mesa.sh: build Mesa for pc_x86_64 with the Android NDK and stage it
# as vendor prebuilts for the AOSP build.
#
# Usage:
#   ./build-mesa.sh [-m <mesa src>] [-n <ndk>] [-a <aosp dir>] [-o <build dir>]
#                   [-j <jobs>] [-S]
#
#   -m  Mesa release source    (default: $MESA_DIR or ~/Documents/mesa-26.2.3)
#   -n  Android NDK            (default: $ANDROID_NDK or ~/Documents/android-ndk-r30)
#   -a  AOSP checkout          (default: derived from this script's location)
#   -o  build directory        (default: <aosp>/out/pc-mesa)
#   -j  parallel jobs          (default: nproc)
#   -S  stage only: reuse the existing build, just redo steps 4-6
#
# Two stages:
#   1. host: mesa_clc + vtn_bindgen2 (needed by the Intel drivers), built
#      natively against LLVM $LLVM_VERSION (+ libclc, SPIRV-LLVM-Translator)
#   2. libelf (needed by radeonsi) from AOSP's external/elfutils, as a shared
#      library (LGPL) with vanilla zlib linked in, built with the NDK
#   3. android: EGL/GLES (iris, radeonsi, virgl, zink, nouveau, softpipe),
#      Vulkan (anv, radv, venus) and GBM (libgbm_mesa + dri_gbm), cross-
#      compiled with the NDK. radeonsi uses ACO (no LLVM on the target);
#      android-stub replaces libcutils & co at link time; libdrm/zlib/expat
#      are linked statically.
#   4. libgbm_mesa_wrapper.so from external/minigbm/gbm_mesa_driver (BlissOS
#      minigbm), which minigbm's gbm_mesa backend dlopens
#
# Stages into <device dir>/prebuilt/mesa/ with vendor-relative paths
# (see graphics/graphics.mk).
set -Eeuo pipefail

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
MESA=${MESA_DIR:-$HOME/Documents/mesa-26.2.3}
NDK=${ANDROID_NDK:-$HOME/Documents/android-ndk-r30}
OUT=""
JOBS=$(nproc)
API=35
LLVM_VERSION=21
STAGE_ONLY=0

while getopts "m:n:a:o:j:Sh" opt; do
  case $opt in
    m) MESA=$OPTARG ;;
    n) NDK=$OPTARG ;;
    a) AOSP=$OPTARG ;;
    o) OUT=$OPTARG ;;
    j) JOBS=$OPTARG ;;
    S) STAGE_ONLY=1 ;;
    *) sed -n '2,33p' "$0"; exit 1 ;;
  esac
done

OUT=${OUT:-$AOSP/out/pc-mesa}
STAGE=$DEVICE_DIR/prebuilt/mesa
LLVM_CONFIG=/usr/lib/llvm-$LLVM_VERSION/bin/llvm-config
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64

# ---------------------------------------------------------------- preflight
[[ -f $MESA/meson.build ]] || die "no Mesa source at $MESA (-m); get a release from https://archive.mesa3d.org/"
[[ -x $TOOLCHAIN/bin/x86_64-linux-android$API-clang ]] || die "no NDK (API $API) at $NDK (-n)"
[[ -x $LLVM_CONFIG ]] || die "missing $LLVM_CONFIG (install llvm-$LLVM_VERSION-dev)"
for t in meson ninja glslangValidator python3 readelf; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done
python3 -c 'import mako, yaml' 2>/dev/null || die "missing python3-mako / python3-yaml"

mkdir -p "$OUT"

CC=$TOOLCHAIN/bin/x86_64-linux-android$API-clang
LIBELF_PREFIX=$OUT/libelf/prefix
AND_BUILD=$OUT/android
AND_INSTALL=$OUT/android-install

if [[ $STAGE_ONLY == 0 ]]; then
# ---------------------------------------------------------------- 1. host tools
HOST_BUILD=$OUT/host
HOST_INSTALL=$OUT/host-install
if [[ ! -x $HOST_INSTALL/bin/mesa_clc || ! -x $HOST_INSTALL/bin/vtn_bindgen2 ]]; then
  info "Building host tools (mesa_clc, vtn_bindgen2) with LLVM $LLVM_VERSION"
  cat > "$OUT/host.ini" <<EOF
[binaries]
llvm-config = '$LLVM_CONFIG'
EOF
  rm -rf "$HOST_BUILD"
  meson setup "$HOST_BUILD" "$MESA" \
    --native-file "$OUT/host.ini" \
    --prefix "$HOST_INSTALL" \
    -Dbuildtype=release \
    -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers= \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
    -Dgles1=disabled -Dgles2=disabled \
    -Dllvm=enabled -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dxmlconfig=disabled \
    -Dzstd=disabled -Dbuild-tests=false
  ninja -C "$HOST_BUILD" -j"$JOBS"
  ninja -C "$HOST_BUILD" install >/dev/null
fi
[[ -x $HOST_INSTALL/bin/mesa_clc && -x $HOST_INSTALL/bin/vtn_bindgen2 ]] ||
  die "host tools not installed in $HOST_INSTALL/bin"
export PATH=$HOST_INSTALL/bin:$PATH

PKGCONFIG_DIR=$OUT/pkgconfig
mkdir -p "$PKGCONFIG_DIR"

# ---------------------------------------------------------------- 2. libelf
ELFUTILS=$AOSP/external/elfutils
LIBELF_OUT=$OUT/libelf
[[ -f $ELFUTILS/libelf/libelf.h ]] || die "missing $ELFUTILS (AOSP external/elfutils)"
( cd "$MESA" && meson subprojects download zlib >/dev/null )
zlib_src=$(find "$MESA/subprojects" -maxdepth 1 -type d -name 'zlib-*' | sort | tail -n1)
[[ -n $zlib_src && -f $zlib_src/zlib.h ]] || die "zlib source not found under $MESA/subprojects"

info "Building libelf (AOSP elfutils) + $(basename "$zlib_src") with the NDK"
rm -rf "$LIBELF_OUT"
mkdir -p "$LIBELF_OUT/obj" "$LIBELF_OUT/cfg" "$LIBELF_PREFIX/lib" "$LIBELF_PREFIX/include"
# AOSP's config.h enables zstd-compressed sections; not needed for shaders.
grep -v 'USE_ZSTD' "$ELFUTILS/config.h" > "$LIBELF_OUT/cfg/config.h"
elf_cflags=(-O2 -fPIC -DHAVE_CONFIG_H -D_GNU_SOURCE -DNMNES=1000 -std=gnu99
            -D_FILE_OFFSET_BITS=64 -Wno-pointer-arith -Wno-typedef-redefinition
            -include AndroidFixup.h
            -I"$LIBELF_OUT/cfg" -I"$ELFUTILS" -I"$ELFUTILS/include" -I"$ELFUTILS/lib"
            -I"$ELFUTILS/bionic-fixup" -I"$ELFUTILS/libelf" -I"$zlib_src")
objs=()
for src in "$ELFUTILS"/libelf/*.c "$ELFUTILS"/lib/*.c; do
  case $(basename "$src") in
    color.c|printversion.c|crc32.c|dynamicsizehash*.c) continue ;;  # as in lib/Android.bp
  esac
  obj=$LIBELF_OUT/obj/$(basename "$(dirname "$src")")_$(basename "${src%.c}").o
  "$CC" "${elf_cflags[@]}" -c "$src" -o "$obj"
  objs+=("$obj")
done
for f in adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil; do
  obj=$LIBELF_OUT/obj/zlib_$f.o
  "$CC" -O2 -fPIC -fvisibility=hidden -DHAVE_UNISTD_H -c "$zlib_src/$f.c" -o "$obj"
  objs+=("$obj")
done
"$CC" -shared -Wl,-soname,libelf.so -o "$LIBELF_PREFIX/lib/libelf.so" "${objs[@]}"
cp "$ELFUTILS"/libelf/{libelf.h,gelf.h,nlist.h,elf.h} "$LIBELF_PREFIX/include/"
elf_version=$(sed -n 's/^AC_INIT(\[[^]]*\],\[\([0-9.]*\)\].*/\1/p' "$ELFUTILS/configure.ac")
{
  echo "prefix=$LIBELF_PREFIX"
  echo "Name: libelf"
  echo "Description: elfutils libelf (AOSP external/elfutils, NDK build)"
  echo "Version: ${elf_version:-0.0}"
  echo "Libs: -L$LIBELF_PREFIX/lib -lelf"
  echo "Cflags: -I$LIBELF_PREFIX/include"
} > "$PKGCONFIG_DIR/libelf.pc"

# ---------------------------------------------------------------- 3. android

# libdrm is linked statically into Mesa, so libdrm_amdgpu's internal
# handle_table_* helpers (normally hidden inside libdrm_amdgpu.so) collide
# with Mesa's own handle_table_remove. Rename them in the downloaded libdrm.
( cd "$MESA" && meson subprojects download libdrm >/dev/null )
drm_ht=$(find "$MESA/subprojects" -maxdepth 3 -path '*libdrm-*/amdgpu/handle_table.h' | head -n1)
[[ -n $drm_ht ]] || die "libdrm subproject source not found"
if ! grep -q 'pc_x86_64: rename' "$drm_ht"; then
  tmp=$(mktemp)
  {
    echo '/* pc_x86_64: rename internal helpers that clash with Mesa when static */'
    for f in insert remove lookup fini; do
      echo "#define handle_table_$f drm_amdgpu_handle_table_$f"
    done
    cat "$drm_ht"
  } > "$tmp"
  mv "$tmp" "$drm_ht"
fi
cat > "$OUT/android-x86_64.ini" <<EOF
[binaries]
c = '$TOOLCHAIN/bin/x86_64-linux-android$API-clang'
cpp = '$TOOLCHAIN/bin/x86_64-linux-android$API-clang++'
ar = '$TOOLCHAIN/bin/llvm-ar'
strip = '$TOOLCHAIN/bin/llvm-strip'
pkg-config = 'pkg-config'

[properties]
# Only our own .pc files (libelf); everything else is stubbed or a static
# fallback, never the host's.
pkg_config_libdir = ['$PKGCONFIG_DIR']

[built-in options]
cpp_link_args = ['-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

info "Configuring Mesa for Android (API $API)"
rm -rf "$AND_BUILD" "$AND_INSTALL"
meson setup "$AND_BUILD" "$MESA" \
  --cross-file "$OUT/android-x86_64.ini" \
  --prefix /vendor \
  --libdir lib64 \
  -Dbuildtype=release -Db_ndebug=true \
  -Ddefault_library=shared \
  --force-fallback-for=libdrm,zlib,expat -Dallow-fallback-for=libdrm \
  -Dlibdrm:default_library=static -Dzlib:default_library=static -Dexpat:default_library=static \
  -Dplatforms=android -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
  -Dplatform-sdk-version=$API \
  -Dgallium-drivers=iris,radeonsi,virgl,zink,nouveau,softpipe \
  -Dvulkan-drivers=intel,amd,virtio \
  -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true \
  -Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa \
  -Dglx=disabled -Dgbm=enabled \
  -Dllvm=disabled -Damd-use-llvm=false \
  -Dmesa-clc=system -Dprecomp-compiler=system \
  -Dintel-rt=disabled \
  -Dgallium-va=disabled -Dvideo-codecs= -Dgallium-rusticl=false \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dzstd=disabled -Dxmlconfig=disabled -Dbuild-tests=false

info "Building Mesa (-j$JOBS)"
ninja -C "$AND_BUILD" -j"$JOBS"
DESTDIR=$AND_INSTALL ninja -C "$AND_BUILD" install >/dev/null
fi  # STAGE_ONLY

src=$AND_INSTALL/vendor/lib64
[[ -d $src ]] || die "nothing installed under $src"

# ---------------------------------------------------------------- 4. gbm_mesa wrapper
WRAPPER_SRC=$AOSP/external/minigbm/gbm_mesa_driver
[[ -f $WRAPPER_SRC/gbm_mesa_wrapper.c ]] ||
  die "no $WRAPPER_SRC/gbm_mesa_wrapper.c (external/minigbm must be BlissOS's, see manifest/maton.xml)"
gbm_lib=$(find "$src" -maxdepth 1 -name 'libgbm_mesa.so*' -type f | head -n1)
[[ -n $gbm_lib ]] || die "Mesa did not install libgbm_mesa"
gbm_soname=$(readelf -d "$gbm_lib" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
info "Building libgbm_mesa_wrapper.so (links $gbm_soname)"
"$CC" -O2 -fPIC -shared -Wl,-soname,libgbm_mesa_wrapper.so \
  -I"$AND_INSTALL/vendor/include" -I"$MESA/include/drm-uapi" \
  -I"$AOSP/system/logging/liblog/include" \
  "$WRAPPER_SRC/gbm_mesa_wrapper.c" -o "$OUT/libgbm_mesa_wrapper.so" \
  "$gbm_lib" -llog

# ---------------------------------------------------------------- 5. stage
info "Staging into $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib64/egl" "$STAGE/lib64/hw" "$STAGE/lib64/gbm"
cp "$gbm_lib" "$STAGE/lib64/$gbm_soname"
cp "$src/gbm/dri_gbm.so" "$STAGE/lib64/gbm/"
cp "$OUT/libgbm_mesa_wrapper.so" "$STAGE/lib64/"
cp "$LIBELF_PREFIX/lib/libelf.so" "$STAGE/lib64/"

for l in EGL GLESv1_CM GLESv2; do
  [[ -f $src/lib${l}_mesa.so ]] || die "missing lib${l}_mesa.so"
  cp "$src/lib${l}_mesa.so" "$STAGE/lib64/egl/"
done
shopt -s nullglob
gallium=("$src"/libgallium*.so)
(( ${#gallium[@]} > 0 )) || die "missing libgallium*.so"
cp "${gallium[@]}" "$STAGE/lib64/"
# Vulkan HALs: the loader opens /vendor/lib64/hw/vulkan.<ro.hardware.vulkan>.so
# (ro.hardware.vulkan is set per GPU by pc-gpu-detect.sh).
for pair in intel:intel radeon:radeon virtio:virtio; do
  lib=$src/libvulkan_${pair%%:*}.so
  [[ -f $lib ]] || die "missing $(basename "$lib")"
  cp "$lib" "$STAGE/lib64/hw/vulkan.${pair##*:}.so"
done
shopt -u nullglob
find "$STAGE" -name '*.so' -exec "$TOOLCHAIN/bin/llvm-strip" --strip-unneeded {} +

# ---------------------------------------------------------------- 6. check deps
# Everything must resolve in the vendor/sphal namespaces at runtime.
allowed='^(libc|libm|libdl|liblog|libnativewindow|libsync|libcutils|libhardware|libelf|libgbm_mesa|libgallium[_a-z0-9.-]*)\.so([.][0-9]+)*$'
bad=0
while IFS= read -r -d '' so; do
  while read -r need; do
    if ! [[ $need =~ $allowed ]]; then
      echo "    ${so#"$STAGE"/} needs $need"
      bad=1
    fi
  done < <(readelf -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
done < <(find "$STAGE" -name '*.so' -print0)
(( bad == 0 )) || die "unexpected runtime dependencies (see above)"

info "Done: $(find "$STAGE" -name '*.so' | wc -l) libraries, $(du -sh "$STAGE" | cut -f1)"
find "$STAGE" -name '*.so' -printf '    %P\n' | sort
