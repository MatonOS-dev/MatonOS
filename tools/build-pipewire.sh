#!/usr/bin/env bash
# Build the MatonOS vendor PipeWire stack with the Android NDK and stage it in
# buildinfra/native-built for inclusion in the ODM bundle.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
. "$(dirname -- "$(readlink -f -- "$0")")/local-env.sh"
NDK=${ANDROID_NDK:-}
API=35
JOBS=${MATON_BUILD_JOBS:-$(nproc)}
# Own work dir: meson trees configured for the old /vendor prefix must not be
# reused for the APEX prefix (stale -I paths fail with -Wmissing-include-dirs).
OUT=${MATON_PIPEWIRE_BUILD:-$MATON_ROOT/out/pipewire-stack-apex}
STAGE=$DEVICE_DIR/buildinfra/native-built/audio-apex
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$TOOLCHAIN/bin/x86_64-linux-android$API-clang
CXX=$TOOLCHAIN/bin/x86_64-linux-android$API-clang++
AR=$TOOLCHAIN/bin/llvm-ar
STRIP=$TOOLCHAIN/bin/llvm-strip
SRC=$OUT/src
APEX_NAME=com.matonos.hardware.media
PREFIX=/apex/$APEX_NAME
DEST=$OUT/dest

while getopts "n:o:j:h" opt; do
  case $opt in
    n) NDK=$OPTARG; TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
       CC=$TOOLCHAIN/bin/x86_64-linux-android$API-clang
       CXX=$TOOLCHAIN/bin/x86_64-linux-android$API-clang++
       AR=$TOOLCHAIN/bin/llvm-ar; STRIP=$TOOLCHAIN/bin/llvm-strip ;;
    o) OUT=$(readlink -m "$OPTARG"); SRC=$OUT/src; DEST=$OUT/dest ;;
    j) JOBS=$OPTARG ;;
    *) echo "Usage: $0 [-n NDK] [-o build-dir] [-j jobs]"; exit 0 ;;
  esac
done

[[ $JOBS =~ ^[1-9][0-9]*$ ]] || die "MATON_BUILD_JOBS must be a positive integer"
[[ -n $NDK ]] || die "set ANDROID_NDK in matonos.local.env or pass -n NDK"

for tool in git meson ninja pkg-config python3 curl sha256sum tar make; do
  command -v "$tool" >/dev/null || die "missing host tool: $tool"
done
[[ -x $CC && -x $CXX ]] || die "Android NDK API $API toolchain not found at $NDK"
mkdir -p "$SRC" "$DEST"
rm -rf "$DEST"
mkdir -p "$DEST"

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

# Meson caches dependency include paths (with $PREFIX baked in) across
# --reconfigure; a prefix change (APEX rename) needs fresh build dirs.
if [[ ! -f $OUT/.prefix || $(<"$OUT/.prefix") != "$PREFIX" ]]; then
  echo "prefix changed to $PREFIX; wiping stale build dirs" >&2
  rm -rf "$OUT/libffi" "$OUT/glib" "$OUT/alsa-lib" "$OUT/pipewire" "$OUT/wireplumber"
fi
mkdir -p "$OUT"
printf '%s\n' "$PREFIX" > "$OUT/.prefix"

meson_configure() {
  local build_dir=$1 source_dir=$2
  shift 2
  if [[ -f $build_dir/build.ninja ]]; then
    meson setup --reconfigure "$build_dir" "$source_dir" "$@"
  else
    meson setup "$build_dir" "$source_dir" "$@"
  fi
}

# Immutable upstream revisions. The tags and source provenance are also
# recorded in audio/pipewire-PROVENANCE.md; generated SOURCE records the
# exact revisions and input hashes used for this build.
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
for UDEV_ANDROID_PATCH in "$DEVICE_DIR"/audio/patches/libudev-zero/*.patch; do
  if git -C "$SRC/libudev-zero" apply --check "$UDEV_ANDROID_PATCH" 2>/dev/null; then
    git -C "$SRC/libudev-zero" apply "$UDEV_ANDROID_PATCH"
  elif ! git -C "$SRC/libudev-zero" apply --reverse --check "$UDEV_ANDROID_PATCH" 2>/dev/null; then
    die "libudev-zero patch does not apply cleanly: $UDEV_ANDROID_PATCH"
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

[built-in options]
c_args = ['-march=x86-64-v2']
cpp_args = ['-march=x86-64-v2']
EOF
export CC CXX AR STRIP
export CFLAGS="-O2 -march=x86-64-v2 -fPIC -D_FILE_OFFSET_BITS=64 -I$DEVICE_DIR/audio/compat/include"
export CXXFLAGS="-O2 -march=x86-64-v2 -fPIC -D_FILE_OFFSET_BITS=64"
export LDFLAGS="-Wl,-z,relro -Wl,-z,now"
export PKG_CONFIG_PATH="$DEST$PREFIX/lib64/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
export PKG_CONFIG_SYSROOT_DIR="$DEST"

info "Building libffi"
meson_configure "$OUT/libffi" "$SRC/libffi" --cross-file "$OUT/android-x86_64.ini" \
  --prefix="$PREFIX" --libdir=lib64 --buildtype=release -Ddefault_library=shared
ninja -C "$OUT/libffi" -j"$JOBS"
DESTDIR="$DEST" ninja -C "$OUT/libffi" install

info "Building daemonless libudev compatibility for PipeWire hotplug"
make -C "$SRC/libudev-zero" clean
make -C "$SRC/libudev-zero" -j"$JOBS" CC="$CC" AR="$AR" \
  CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" PREFIX="$PREFIX" \
  LIBDIR="$PREFIX/lib64" INCLUDEDIR="$PREFIX/include" DESTDIR="$DEST" \
  USB_IDS_PATH=$PREFIX/etc/usb.ids install-shared

info "Building GLib/GIO (WirePlumber runtime dependency)"
if [[ ! -f $DEST$PREFIX/lib64/libgio-2.0.so ]]; then
  meson_configure "$OUT/glib" "$SRC/glib" --cross-file "$OUT/android-x86_64.ini" \
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
  ( unset CC CXX AR STRIP CFLAGS CXXFLAGS LDFLAGS \
      PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
    meson_configure "$OUT/glib-host" "$SRC/glib" --prefix="$OUT/glib-host-prefix" \
      --buildtype=release -Ddefault_library=shared -Dtests=false \
      -Dintrospection=disabled -Dman-pages=disabled -Dnls=disabled \
      -Dselinux=disabled -Dlibmount=disabled -Dlibelf=disabled \
      -Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled \
      -Dglib_debug=disabled -Dforce_posix_threads=true )
fi
env -u CC -u CXX -u AR -u STRIP -u CFLAGS -u CXXFLAGS -u LDFLAGS \
    -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
  ninja -C "$OUT/glib-host" -j"$JOBS" \
    gio/glib-compile-resources gobject/glib-mkenums gio/gdbus-2.0/codegen/gdbus-codegen
HOST_GLIB_RESOURCES=$OUT/glib-host/gio/glib-compile-resources
HOST_GLIB_MKENUMS=$OUT/glib-host/gobject/glib-mkenums
HOST_GDBUS_CODEGEN=$OUT/glib-host/gio/gdbus-2.0/codegen/gdbus-codegen

info "Building ALSA-lib"
if [[ ! -f $DEST$PREFIX/lib64/libasound.so.2 ]]; then
  mkdir -p "$OUT/alsa-lib"
  (cd "$OUT/alsa-lib" && "$SRC/alsa-lib-1.2.15/configure" --host=x86_64-linux-android \
    --prefix=$PREFIX --libdir=$PREFIX/lib64 --with-configdir=$PREFIX/etc/alsa \
    --with-plugindir=$PREFIX/lib64/alsa-lib --disable-python --disable-old-symbols \
    --disable-alisp --disable-aserver --disable-topology)
  make -C "$OUT/alsa-lib" -j"$JOBS"
  make -C "$OUT/alsa-lib" DESTDIR="$DEST" install
fi

info "Building PipeWire (ALSA SPA backend; systemd, D-Bus and BlueZ disabled)"
export PKG_CONFIG_PATH="$DEST$PREFIX/lib64/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
meson_configure "$OUT/pipewire" "$SRC/pipewire" --cross-file "$OUT/android-x86_64.ini" \
  -Dc_args="-march=x86-64-v2 -I$DEVICE_DIR/audio/compat/include" \
  --prefix=$PREFIX --libdir=lib64 --buildtype=release -Ddefault_library=shared \
  -Dalsa=enabled -Dspa-plugins=enabled -Dsupport=enabled \
  -Djack=disabled -Dv4l2=disabled \
  -Daudioconvert=enabled -Daudiomixer=enabled -Dlibsystemd=disabled \
  -Dsystemd-system-service=disabled -Dsystemd-user-service=disabled \
  -Ddbus=disabled -Dbluez5=disabled -Dpipewire-alsa=disabled \
  -Dpipewire-jack=disabled -Dpipewire-v4l2=disabled -Dlibcamera=disabled \
  -Dvulkan=disabled -Dffmpeg=disabled -Dgstreamer=disabled \
  -Dgstreamer-device-provider=disabled -Dman=disabled -Ddocs=disabled \
  -Dtests=disabled -Dexamples=disabled -Dsession-managers=[] -Dudev=enabled \
  -Dflatpak=disabled -Droc=disabled \
  -Dlibpulse=disabled -Dpw-cat=disabled -Dgsettings=disabled
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
  cp -L "$source_tool" "$DEST$PREFIX/bin/$tool_name"
done
sed -i \
  -e 's|^glib_compile_resources=.*|glib_compile_resources=${bindir}/glib-compile-resources|' \
  -e 's|^gdbus_codegen=.*|gdbus_codegen=${bindir}/gdbus-codegen|' \
  "$DEST$PREFIX/lib64/pkgconfig/gio-2.0.pc"
sed -i \
  -e 's|^glib_mkenums=.*|glib_mkenums=${bindir}/glib-mkenums|' \
  "$DEST$PREFIX/lib64/pkgconfig/glib-2.0.pc"
meson_configure "$OUT/wireplumber" "$SRC/wireplumber" --cross-file "$OUT/android-x86_64.ini" \
  -Dc_args="-march=x86-64-v2 -I$DEVICE_DIR/audio/compat/include" \
  --prefix=$PREFIX --libdir=lib64 --buildtype=release -Ddefault_library=shared \
  -Dsystem-lua=false -Dsystemd=disabled -Dsystemd-system-service=false \
  -Dsystemd-user-service=false -Dintrospection=disabled \
  -Ddoc=disabled -Dtests=false -Dtools=true
LD_LIBRARY_PATH="$OUT/glib-host/gio:$OUT/glib-host/glib:$OUT/glib-host/gobject:$OUT/glib-host/gmodule${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ninja -C "$OUT/wireplumber" -j"$JOBS"
DESTDIR="$DEST" ninja -C "$OUT/wireplumber" install

info "Staging ALSA UCM profiles and vendor files"
mkdir -p "$DEST$PREFIX/etc/alsa/ucm2"
cp -a "$SRC/alsa-ucm-conf/ucm2/." "$DEST$PREFIX/etc/alsa/ucm2/"
mkdir -p "$DEST$PREFIX/etc/pipewire" "$DEST$PREFIX/etc/wireplumber"
cp "$DEST$PREFIX/share/pipewire/pipewire.conf" "$DEST$PREFIX/etc/pipewire/pipewire.conf"
# PipeWire clients (WirePlumber and the HAL) load client.conf before connecting.
# Keep defaults at the compiled APEX prefix beside the main config.
cp "$DEST$PREFIX/share/pipewire/client.conf" "$DEST$PREFIX/etc/pipewire/client.conf"
cp "$DEST$PREFIX/share/wireplumber/wireplumber.conf" "$DEST$PREFIX/etc/wireplumber/wireplumber.conf"
mkdir -p "$DEST$PREFIX/etc/wireplumber/wireplumber.conf.d"
cp "$DEVICE_DIR/audio/config/wireplumber/wireplumber.conf.d/10-maton.conf" \
  "$DEST$PREFIX/etc/wireplumber/wireplumber.conf.d/10-maton.conf"
mkdir -p "$DEST$PREFIX/etc/pipewire/pipewire.conf.d"
cp "$DEVICE_DIR/audio/config/pipewire/pipewire.conf.d/10-maton-fallback.conf" \
  "$DEST$PREFIX/etc/pipewire/pipewire.conf.d/10-maton-fallback.conf"
# Keep only runtime assets in the image; cross-build headers, pkg-config files
# and GLib development utilities are build-time dependencies.
for bin in "$DEST$PREFIX/bin"/*; do
  case $(basename "$bin") in pipewire|wireplumber|pw-cli|wpctl) ;; *) rm -f "$bin" ;; esac
done
# PipeWire 1.6.9 always builds its Pulse protocol module. No Meson switch
# exists for that module, so omit it (and the daemon alias) from the runtime.
rm -f "$DEST$PREFIX/lib64/pipewire-0.3/libpipewire-module-protocol-pulse.so"
# These optional protocol/configuration assets are not part of this runtime.
rm -f "$DEST$PREFIX/etc/pipewire/jack.conf" \
  "$DEST$PREFIX/etc/pipewire/pipewire-pulse.conf" \
  "$DEST$PREFIX/share/pipewire/jack.conf" \
  "$DEST$PREFIX/share/pipewire/pipewire-pulse.conf"
rm -rf "$DEST$PREFIX/share/pipewire/pipewire-pulse.conf.avail"
rm -rf "$DEST$PREFIX/share/doc"
rm -f "$DEST$PREFIX/lib64/pipewire-0.3/libpipewire-module-netjack2-driver.so" \
  "$DEST$PREFIX/lib64/pipewire-0.3/libpipewire-module-netjack2-manager.so"
# WirePlumber has no Meson switch for its optional D-Bus integrations. Keep
# the ALSA/Lua default policy modules and omit the unrequested integrations.
for module in dbus-connection portal-permissionstore reserve-device \
              modem-manager mpris notifications-api; do
  rm -f "$DEST$PREFIX/lib64/wireplumber-0.5/libwireplumber-module-$module.so"
done
rm -rf "$DEST$PREFIX/share/wireplumber/scripts/monitors/bluez" \
  "$DEST$PREFIX/share/wireplumber/scripts/monitors/v4l2" \
  "$DEST$PREFIX/share/wireplumber/scripts/monitors/libcamera"
rm -f "$DEST$PREFIX/share/wireplumber/scripts/monitors/bluez-midi.lua"
# PRODUCT_COPY_FILES installs regular files only. Dereference the versioned
# upstream library symlinks into regular vendor files so DT_NEEDED names resolve.
while IFS= read -r -d '' link; do
  [[ -d $link ]] && continue
  tmp=$link.copy
  cp -L "$link" "$tmp"
  mv "$tmp" "$link"
done < <(find "$DEST$PREFIX" -type l -print0)
rm -rf "$STAGE"
mkdir -p "$STAGE"
for component in bin etc lib64 share; do
  [[ ! -d $DEST$PREFIX/$component ]] || cp -a "$DEST$PREFIX/$component" "$STAGE/$component"
done
# Soong HAL inputs (audio/pipewire-hal/Android.bp): Soong only accepts paths
# inside the module directory, and headers do not belong in the ODM image.
HAL_PREBUILT=$DEVICE_DIR/audio/pipewire-hal/prebuilt
mkdir -p "$HAL_PREBUILT/include"
for include_root in "$DEST$PREFIX/include/pipewire-0.3" "$DEST$PREFIX/include/spa-0.2"; do
  [[ -d $include_root ]] || die "PipeWire header dir missing: $include_root"
  cp -a "$include_root" "$HAL_PREBUILT/include/"
done
rm -rf "$STAGE/lib64/pkgconfig" "$STAGE/lib64/glib-2.0/include" \
  "$STAGE/share/bash-completion" "$STAGE/share/gettext" \
  "$STAGE/share/aclocal" "$STAGE/share/glib-2.0"
find "$STAGE/lib64" -type f \( -name '*.a' -o -name '*.la' \) -delete
rm -f "$STAGE/lib64/libgirepository-2.0.so"
# Include license texts for the runtime and statically linked GLib fallbacks.
LICENSES=$STAGE/share/licenses/pipewire-stack
mkdir -p "$LICENSES"
install -m 0644 "$SRC/pipewire/COPYING" "$LICENSES/pipewire-MIT.txt"
install -m 0644 "$SRC/wireplumber/LICENSE" "$LICENSES/wireplumber-MIT.txt"
install -m 0644 "$SRC/alsa-lib-1.2.15/COPYING" "$LICENSES/alsa-lib-LGPL-2.1.txt"
install -m 0644 "$SRC/alsa-ucm-conf/LICENSE" "$LICENSES/alsa-ucm-conf-BSD-3-Clause.txt"
install -m 0644 "$SRC/glib/COPYING" "$LICENSES/glib-LGPL-2.1.txt"
install -m 0644 "$SRC/libffi/LICENSE" "$LICENSES/libffi-MIT.txt"
install -m 0644 "$SRC/libudev-zero/LICENSE" "$LICENSES/libudev-zero-ISC.txt"
install -m 0644 "$SRC/glib/subprojects/pcre2-10.44/LICENCE" "$LICENSES/pcre2-LICENCE.txt"
install -m 0644 "$SRC/glib/subprojects/proxy-libintl/COPYING" "$LICENSES/proxy-libintl-LGPL-2.1.txt"
install -m 0644 "$SRC/wireplumber/subprojects/lua-5.5.0/doc/readme.html" "$LICENSES/lua-MIT.html"
# Staged shared libraries and executables are compact runtime files.
while IFS= read -r -d '' file; do
  case "$file" in *.so|*.so.*) "$STRIP" --strip-unneeded "$file" ;; esac
done < <(find "$STAGE/lib64" "$STAGE/bin" -type f -print0)
for bin in "$STAGE/bin"/*; do
  case $(basename "$bin") in *.so) continue ;; *) "$STRIP" --strip-unneeded "$bin" 2>/dev/null || true ;; esac
done
{
  printf 'PipeWire 1.6.9\ncommit %s\n' 8fa27cabdc6c0c1350c69c026af5850ef0af1e26
  printf 'WirePlumber commit %s\n' 11e501181fb87dc8e72e55156d672109fbad2434
  printf 'alsa-ucm-conf commit %s\n' 28776e1e22a952e4ab01037033380aa111985101
  printf 'glib commit %s\n' 41eca60845d3fc309af361f5e7f801ba339099aa
  printf 'libffi commit %s\n' 83d0cfd00d7d37af4b4349511d29f1f0512621b3
  printf 'libudev-zero commit %s\n' 7a6eee2db11f2eb0f7fd065ae9597fcd270e6734
  printf 'ALSA-lib archive sha256 %s\n' "$(sha256sum "$ALSA_TARBALL" | cut -d' ' -f1)"
  printf 'Lua 5.5.0 source sha256 %s\n' \
    "$(awk -F ' = ' '$1 == "source_hash" {print $2}' "$SRC/wireplumber/subprojects/lua.wrap")"
  printf 'Lua 5.5.0 patch sha256 %s\n' \
    "$(awk -F ' = ' '$1 == "patch_hash" {print $2}' "$SRC/wireplumber/subprojects/lua.wrap")"
} > "$STAGE/SOURCE"

for required in bin/pipewire bin/wireplumber \
                lib64/libpipewire-0.3.so lib64/libwireplumber-0.5.so \
                lib64/libasound.so \
                lib64/spa-0.2/support/libspa-support.so \
                lib64/spa-0.2/audioconvert/libspa-audioconvert.so \
                lib64/spa-0.2/audiomixer/libspa-audiomixer.so \
                lib64/spa-0.2/alsa/libspa-alsa.so \
                etc/pipewire/pipewire.conf etc/wireplumber/wireplumber.conf \
                etc/alsa/ucm2; do
  [[ -e $STAGE/$required ]] || die "required staged PipeWire file missing: $required"
done
for unwanted in '*bluez*' '*v4l2*' '*jack*' '*pulse*' '*systemd*' '*dbus-connection*'; do
  if find "$STAGE/bin" "$STAGE/lib64" "$STAGE/share/pipewire" \
      "$STAGE/share/wireplumber" -iname "$unwanted" -print -quit | grep -q .; then
    die "disabled feature artifact remains in staged PipeWire tree: $unwanted"
  fi
done

READELF=$TOOLCHAIN/bin/llvm-readelf
SYSTEM_LIBS=$TOOLCHAIN/sysroot/usr/lib/x86_64-linux-android/$API
[[ -x $READELF && -d $SYSTEM_LIBS ]] || die "NDK readelf/system stubs missing"
NEEDED_REPORT=$STAGE/NEEDED.txt
: > "$NEEDED_REPORT"
while IFS= read -r -d '' elf; do
  [[ $(head -c4 "$elf") == $'\x7fELF' ]] || continue
  printf '%s\n' "${elf#"$STAGE"/}" >> "$NEEDED_REPORT"
  while IFS= read -r needed; do
    [[ -n $needed ]] || continue
    if [[ ! -f $STAGE/lib64/$needed && ! -f $SYSTEM_LIBS/$needed ]]; then
      die "unresolved DT_NEEDED '$needed' from ${elf#"$STAGE"/}"
    fi
    printf '  %s\n' "$needed" >> "$NEEDED_REPORT"
  done < <("$READELF" -d "$elf" | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')
done < <(find "$STAGE/bin" "$STAGE/lib64" -type f -print0)
install -m 0644 "$STAGE/lib64/libpipewire-0.3.so" "$HAL_PREBUILT/libpipewire-0.3.so"
for required in include/pipewire-0.3/pipewire/pipewire.h \
                include/spa-0.2/spa/param/audio/format-utils.h libpipewire-0.3.so; do
  [[ -e $HAL_PREBUILT/$required ]] || die "missing HAL prebuilt: $required"
done
du -sh "$STAGE" >&2
echo "PipeWire APEX contents staged in $STAGE (DT_NEEDED: $NEEDED_REPORT)"
