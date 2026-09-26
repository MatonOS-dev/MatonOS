#!/usr/bin/env bash
# Build PipeWire, WirePlumber, ALSA-lib and their small GLib dependency stack
# for Android x86_64, then stage vendor-relative files in prebuilt/pipewire.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
. "$(dirname -- "$(readlink -f -- "$0")")/local-env.sh"
NDK=${ANDROID_NDK:-$HOME/Documents/android-ndk-r30}
API=35
JOBS=${PIPEWIRE_JOBS:-4}
OUT=$AOSP/out/pc-pipewire
STAGE=$DEVICE_DIR/prebuilt/pipewire
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$TOOLCHAIN/bin/x86_64-linux-android$API-clang
CXX=$TOOLCHAIN/bin/x86_64-linux-android$API-clang++
AR=$TOOLCHAIN/bin/llvm-ar
STRIP=$TOOLCHAIN/bin/llvm-strip
SRC=$OUT/src
PREFIX=/vendor
DEST=$OUT/dest

while getopts "n:a:o:j:h" opt; do
  case $opt in
    n) NDK=$OPTARG; TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
       CC=$TOOLCHAIN/bin/x86_64-linux-android$API-clang
       CXX=$TOOLCHAIN/bin/x86_64-linux-android$API-clang++
       AR=$TOOLCHAIN/bin/llvm-ar; STRIP=$TOOLCHAIN/bin/llvm-strip ;;
    a) AOSP=$(readlink -f "$OPTARG") ;;
    o) OUT=$(readlink -m "$OPTARG"); SRC=$OUT/src; PREFIX=$OUT/prefix; DEST=$OUT/dest ;;
    j) JOBS=$OPTARG ;;
    *) echo "Usage: $0 [-n NDK] [-a AOSP] [-o build-dir] [-j jobs]"; exit 0 ;;
  esac
done

for tool in git meson ninja pkg-config python3 curl sha256sum tar make; do
  command -v "$tool" >/dev/null || die "missing host tool: $tool"
done
[[ -x $CC && -x $CXX ]] || die "Android NDK API $API toolchain not found at $NDK"
mkdir -p "$SRC" "$DEST"

fetch() {
  local name=$1 url=$2 commit=$3 dir=$SRC/$1
  if [[ ! -d $dir/.git ]]; then
    git clone --filter=blob:none --no-checkout "$url" "$dir"
  fi
  git -C "$dir" remote set-url origin "$url"
  git -C "$dir" fetch --depth=1 origin "$commit"
  git -C "$dir" checkout --detach FETCH_HEAD
  [[ $(git -C "$dir" rev-parse HEAD) == "$commit" ]] || die "$name source revision mismatch"
}

# Immutable upstream revisions. The tags and source provenance are also
# recorded in audio/README.md; no build-host paths are embedded in images.
fetch alsa-ucm-conf https://github.com/alsa-project/alsa-ucm-conf.git 28776e1e22a952e4ab01037033380aa111985101
fetch glib https://gitlab.gnome.org/GNOME/glib.git 41eca60845d3fc309af361f5e7f801ba339099aa
fetch libffi https://gitlab.freedesktop.org/gstreamer/meson-ports/libffi.git 83d0cfd00d7d37af4b4349511d29f1f0512621b3
fetch libudev-zero https://github.com/illiliti/libudev-zero.git 7a6eee2db11f2eb0f7fd065ae9597fcd270e6734
fetch pipewire https://gitlab.freedesktop.org/pipewire/pipewire.git 8fa27cabdc6c0c1350c69c026af5850ef0af1e26
fetch wireplumber https://gitlab.freedesktop.org/pipewire/wireplumber.git 11e501181fb87dc8e72e55156d672109fbad2434

# Bionic does not expose pthread_cancel(). PipeWire's cancel flag is an
# optimization; invoke the same loop stop callback on Android instead.
for PIPEWIRE_ANDROID_PATCH in "$DEVICE_DIR"/audio/patches/*.patch; do
  if git -C "$SRC/pipewire" apply --check "$PIPEWIRE_ANDROID_PATCH" 2>/dev/null; then
    git -C "$SRC/pipewire" apply "$PIPEWIRE_ANDROID_PATCH"
  elif ! git -C "$SRC/pipewire" apply --reverse --check "$PIPEWIRE_ANDROID_PATCH" 2>/dev/null; then
    die "PipeWire Android patch does not apply cleanly: $PIPEWIRE_ANDROID_PATCH"
  fi
done
for WIREPLUMBER_ANDROID_PATCH in "$DEVICE_DIR"/audio/patches/wireplumber/*.patch; do
  if git -C "$SRC/wireplumber" apply --check "$WIREPLUMBER_ANDROID_PATCH" 2>/dev/null; then
    git -C "$SRC/wireplumber" apply "$WIREPLUMBER_ANDROID_PATCH"
  elif ! git -C "$SRC/wireplumber" apply --reverse --check "$WIREPLUMBER_ANDROID_PATCH" 2>/dev/null; then
    die "WirePlumber Android patch does not apply cleanly: $WIREPLUMBER_ANDROID_PATCH"
  fi
done

# ALSA-lib's upstream release archive includes its generated configure script;
# the Git checkout does not, and building it must not depend on host autotools.
ALSA_TARBALL=$SRC/alsa-lib-1.2.15.tar.bz2
if [[ ! -f $ALSA_TARBALL ]]; then
  curl -fL --retry 3 -o "$ALSA_TARBALL" \
    https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.15.tar.bz2
fi
echo '83770841585e766a60c99fd23f8c574c22643ae0cb1f2d20b793c3d84eb95a8d  '"$ALSA_TARBALL" \
  | sha256sum -c -
rm -rf "$SRC/alsa-lib"
tar -xf "$ALSA_TARBALL" -C "$SRC"
PCRE2_TARBALL=$SRC/pcre2-10.46.tar.bz2
if [[ ! -f $PCRE2_TARBALL ]]; then
  curl -fL --retry 3 -o "$PCRE2_TARBALL" \
    https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.46/pcre2-10.46.tar.bz2
fi
echo '15fbc5aba6beee0b17aecb04602ae39432393aba1ebd8e39b7cabf7db883299f  '"$PCRE2_TARBALL" \
  | sha256sum -c -
rm -rf "$SRC/pcre2-10.46"
tar -xf "$PCRE2_TARBALL" -C "$SRC"
SNDFILE_TARBALL=$SRC/libsndfile-1.2.2.tar.xz
if [[ ! -f $SNDFILE_TARBALL ]]; then
  curl -fL --retry 3 -o "$SNDFILE_TARBALL" \
    https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz
fi
echo '3799ca9924d3125038880367bf1468e53a1b7e3686a934f098b7e1d286cdb80e  '"$SNDFILE_TARBALL" \
  | sha256sum -c -
rm -rf "$SRC/libsndfile-1.2.2"
tar -xf "$SNDFILE_TARBALL" -C "$SRC"

cat > "$OUT/android-x86_64.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF
export CC CXX AR STRIP
export CFLAGS="-O2 -fPIC -D_FILE_OFFSET_BITS=64 -I$DEVICE_DIR/audio/compat/include"
export CXXFLAGS="-O2 -fPIC -D_FILE_OFFSET_BITS=64"
export LDFLAGS="-Wl,-z,relro -Wl,-z,now"
export PKG_CONFIG_PATH="$DEST/vendor/lib64/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
export PKG_CONFIG_SYSROOT_DIR="$DEST"

info "Building libffi"
meson setup "$OUT/libffi" "$SRC/libffi" --cross-file "$OUT/android-x86_64.ini" \
  --prefix="$PREFIX" --libdir=lib64 --buildtype=release -Ddefault_library=shared
ninja -C "$OUT/libffi" -j"$JOBS"
DESTDIR="$DEST" ninja -C "$OUT/libffi" install

info "Building PCRE2"
mkdir -p "$OUT/pcre2"
  (cd "$OUT/pcre2" && "$SRC/pcre2-10.46/configure" --host=x86_64-linux-android \
    --prefix=/vendor --libdir=/vendor/lib64 --enable-shared --disable-static \
    --disable-pcre2-16 --disable-pcre2-32 --disable-jit)
make -C "$OUT/pcre2" -j"$JOBS"
make -C "$OUT/pcre2" DESTDIR="$DEST" install

info "Building libsndfile (raw PCM support only; codecs disabled)"
mkdir -p "$OUT/libsndfile"
(cd "$OUT/libsndfile" && "$SRC/libsndfile-1.2.2/configure" \
  --host=x86_64-linux-android --prefix=/vendor --libdir=/vendor/lib64 \
  --enable-shared --disable-static --disable-external-libs --disable-mpeg \
  --disable-experimental --disable-sqlite --disable-full-suite)
make -C "$OUT/libsndfile" -j"$JOBS"
make -C "$OUT/libsndfile" DESTDIR="$DEST" install

info "Building daemonless libudev compatibility for PipeWire hotplug"
make -C "$SRC/libudev-zero" clean
make -C "$SRC/libudev-zero" -j"$JOBS" CC="$CC" AR="$AR" \
  CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" PREFIX="$PREFIX" \
  LIBDIR="$PREFIX/lib64" INCLUDEDIR="$PREFIX/include" DESTDIR="$DEST" \
  USB_IDS_PATH=/vendor/etc/usb.ids install-shared

info "Building GLib/GIO (WirePlumber runtime dependency)"
if [[ ! -f $DEST/vendor/lib64/libgio-2.0.so ]]; then
  meson setup "$OUT/glib" "$SRC/glib" --cross-file "$OUT/android-x86_64.ini" \
    --prefix="$PREFIX" --libdir=lib64 --buildtype=release -Ddefault_library=shared \
    -Dtests=false -Dintrospection=disabled -Dman-pages=disabled -Dnls=disabled \
    -Dselinux=disabled -Dlibmount=disabled -Dlibelf=disabled -Ddtrace=disabled \
    -Dsystemtap=disabled -Dsysprof=disabled -Dglib_debug=disabled \
    -Dforce_posix_threads=true
  ninja -C "$OUT/glib" -j"$JOBS"
  DESTDIR="$DEST" ninja -C "$OUT/glib" install
fi
# Restore headers/pkg-config metadata in the build sysroot if a previous run
# pruned only the staged image copy.
DESTDIR="$DEST" ninja -C "$OUT/glib" install >/dev/null

info "Building host-native GLib generators for WirePlumber"
if [[ ! -x $OUT/glib-host/gio/glib-compile-resources ]]; then
  env -u CC -u CXX -u AR -u STRIP -u CFLAGS -u CXXFLAGS -u LDFLAGS \
      -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
    meson setup "$OUT/glib-host" "$SRC/glib" --prefix="$OUT/glib-host-prefix" \
      --buildtype=release -Ddefault_library=shared -Dtests=false \
      -Dintrospection=disabled -Dman-pages=disabled -Dnls=disabled \
      -Dselinux=disabled -Dlibmount=disabled -Dlibelf=disabled \
      -Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled \
      -Dglib_debug=disabled -Dforce_posix_threads=true
fi
env -u CC -u CXX -u AR -u STRIP -u CFLAGS -u CXXFLAGS -u LDFLAGS \
    -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
  ninja -C "$OUT/glib-host" -j"$JOBS" \
    gio/glib-compile-resources gobject/glib-mkenums gio/gdbus-2.0/codegen/gdbus-codegen
HOST_GLIB_RESOURCES=$OUT/glib-host/gio/glib-compile-resources
HOST_GLIB_MKENUMS=$OUT/glib-host/gobject/glib-mkenums
HOST_GDBUS_CODEGEN=$OUT/glib-host/gio/gdbus-2.0/codegen/gdbus-codegen

info "Building ALSA-lib"
if [[ ! -f $DEST/vendor/lib64/libasound.so.2 ]]; then
  mkdir -p "$OUT/alsa-lib"
  (cd "$OUT/alsa-lib" && "$SRC/alsa-lib-1.2.15/configure" --host=x86_64-linux-android \
    --prefix=/vendor --libdir=/vendor/lib64 --with-configdir=/vendor/etc/alsa \
    --with-plugindir=/vendor/lib64/alsa-lib --disable-python --disable-old-symbols \
    --disable-alisp --disable-aserver --disable-topology)
  make -C "$OUT/alsa-lib" -j"$JOBS"
  make -C "$OUT/alsa-lib" DESTDIR="$DEST" install
fi

info "Building PipeWire (ALSA SPA backend; systemd, D-Bus and BlueZ disabled)"
export PKG_CONFIG_PATH="$DEST/vendor/lib64/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
meson setup "$OUT/pipewire" "$SRC/pipewire" --cross-file "$OUT/android-x86_64.ini" \
  --prefix=/vendor --libdir=lib64 --buildtype=release -Ddefault_library=shared \
  -Dalsa=enabled -Dlibsystemd=disabled \
  -Dsystemd-system-service=disabled -Dsystemd-user-service=disabled \
  -Ddbus=disabled -Dbluez5=disabled -Dpipewire-alsa=disabled \
  -Dpipewire-jack=disabled -Dpipewire-v4l2=disabled -Dlibcamera=disabled \
  -Dvulkan=disabled -Dffmpeg=disabled -Dgstreamer=disabled \
  -Dgstreamer-device-provider=disabled -Dman=disabled -Ddocs=disabled \
  -Dtests=disabled -Dexamples=disabled -Dsession-managers=[] -Dudev=enabled \
  -Dflatpak=disabled -Droc=disabled \
  -Dlibpulse=disabled -Dpw-cat=enabled -Dgsettings=disabled
ninja -C "$OUT/pipewire" -j"$JOBS"
DESTDIR="$DEST" ninja -C "$OUT/pipewire" install

info "Building WirePlumber (bundled Lua; no systemd or D-Bus)"
# WirePlumber's Meson helpers execute GLib generators at build time. Their
# .pc tool paths are sysrooted, so temporarily put host-native tools at those
# paths. The final vendor bin pruning below removes these build-only tools.
for tool in "$HOST_GLIB_RESOURCES:glib-compile-resources" \
            "$HOST_GLIB_MKENUMS:glib-mkenums" \
            "$HOST_GDBUS_CODEGEN:gdbus-codegen"; do
  source_tool=${tool%%:*}
  tool_name=${tool##*:}
  cp -L "$source_tool" "$DEST/vendor/bin/$tool_name"
done
sed -i \
  -e 's|^glib_compile_resources=.*|glib_compile_resources=${bindir}/glib-compile-resources|' \
  -e 's|^gdbus_codegen=.*|gdbus_codegen=${bindir}/gdbus-codegen|' \
  "$DEST/vendor/lib64/pkgconfig/gio-2.0.pc"
sed -i \
  -e 's|^glib_mkenums=.*|glib_mkenums=${bindir}/glib-mkenums|' \
  "$DEST/vendor/lib64/pkgconfig/glib-2.0.pc"
meson setup "$OUT/wireplumber" "$SRC/wireplumber" --cross-file "$OUT/android-x86_64.ini" \
  --prefix=/vendor --libdir=lib64 --buildtype=release -Ddefault_library=shared \
  -Dsystem-lua=false -Dsystemd=disabled -Dsystemd-system-service=false \
  -Dsystemd-user-service=false -Dintrospection=disabled \
  -Ddoc=disabled -Dtests=false -Dtools=false
ninja -C "$OUT/wireplumber" -j"$JOBS"
DESTDIR="$DEST" ninja -C "$OUT/wireplumber" install

info "Staging ALSA UCM profiles and vendor files"
mkdir -p "$DEST/vendor/etc/alsa/ucm2"
cp -a "$SRC/alsa-ucm-conf/ucm2/." "$DEST/vendor/etc/alsa/ucm2/"
mkdir -p "$DEST/vendor/etc/pipewire" "$DEST/vendor/etc/wireplumber"
cp "$DEST/vendor/share/pipewire/pipewire.conf" "$DEST/vendor/etc/pipewire/pipewire.conf"
# PipeWire clients (WirePlumber and the HAL proxy) load client.conf before
# connecting. The private ODM prefix differs from the compiled /vendor path,
# so install the defaults beside pipewire.conf and set PIPEWIRE_CONFIG_DIR in
# each service's init environment.
cp "$DEST/vendor/share/pipewire/client.conf" "$DEST/vendor/etc/pipewire/client.conf"
cp "$DEST/vendor/share/wireplumber/wireplumber.conf" "$DEST/vendor/etc/wireplumber/wireplumber.conf"
mkdir -p "$DEST/vendor/etc/wireplumber/wireplumber.conf.d"
cp "$DEVICE_DIR/audio/config/wireplumber/wireplumber.conf.d/10-maton.conf" \
  "$DEST/vendor/etc/wireplumber/wireplumber.conf.d/10-maton.conf"
mkdir -p "$DEST/vendor/etc/pipewire/pipewire.conf.d"
cp "$DEVICE_DIR/audio/config/pipewire/pipewire.conf.d/10-maton-fallback.conf" \
  "$DEST/vendor/etc/pipewire/pipewire.conf.d/10-maton-fallback.conf"
# Keep only runtime assets in the image; cross-build headers, pkg-config files
# and GLib development utilities are build-time dependencies.
for bin in "$DEST/vendor/bin"/*; do
  case $(basename "$bin") in pipewire|wireplumber|pw-cat|pw-cli) ;; *) rm -f "$bin" ;; esac
done
# PRODUCT_COPY_FILES installs regular files only. Dereference the versioned
# upstream library symlinks into regular vendor files so DT_NEEDED names resolve.
while IFS= read -r -d '' link; do
  [[ -d $link ]] && continue
  tmp=$link.copy
  cp -L "$link" "$tmp"
  mv "$tmp" "$link"
done < <(find "$DEST/vendor" -type l -print0)
mkdir -p "$STAGE"
rm -rf "$STAGE/vendor"
cp -a "$DEST/vendor" "$STAGE/vendor"
# The PipeWire stack is bundled under /odm, not installed into /vendor by
# PRODUCT_COPY_FILES. Keep its pinned libffi beside GLib so the ODM runtime
# works even on products which do not install AOSP's optional vendor libffi.
rm -rf "$STAGE/vendor/include" "$STAGE/vendor/lib64/pkgconfig" \
  "$STAGE/vendor/lib64/glib-2.0/include" \
  "$STAGE/vendor/share/bash-completion" "$STAGE/vendor/share/gettext" \
  "$STAGE/vendor/share/aclocal" "$STAGE/vendor/share/glib-2.0"
find "$STAGE/vendor/lib64" -maxdepth 1 -type f \( -name '*.a' -o -name '*.la' \) -delete
rm -f "$STAGE/vendor/lib64/libgirepository-2.0.so"
echo "PipeWire vendor prebuilts staged in $STAGE/vendor"
