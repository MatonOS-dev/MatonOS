#!/usr/bin/env bash
# Build the X11/Xwayland stack for bionic x86_64 with the pinned NDK, outside
# Soong: xorgproto, xtrans, util-macros, libpthread-stubs, xcb-proto, libXau,
# libXdmcp, libxcb, xcb-util-wm (icccm/ewmh), xcb-util-errors, libX11,
# libxkbfile, libXfont2 and Xwayland. wlroots' xwayland module needs the xcb
# libraries and the Xwayland server binary; X clients in Flatpaks need libX11.
# Sources are pinned with SHA-256 in UPSTREAMS; nothing is patched.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
AOSP=$(cd "$ROOT/../../../../.." && pwd)
NDK=${MATON_NDK:-$HOME/Documents/android-ndk-r30}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
API=35
JOBS=${MATON_BUILD_JOBS:-4}
[[ $JOBS =~ ^[1-4]$ ]] || die 'MATON_BUILD_JOBS must be 1..4'
[[ -x $TOOLCHAIN/bin/x86_64-linux-android${API}-clang ]] || die "missing NDK at $NDK"

BUILD=${MATON_XWAYLAND_BUILD:-$AOSP/out/matonos/compositor}
PREFIX=$BUILD/xstage/x86_64
SOURCES=${MATON_COMPOSITOR_SOURCES:-$BUILD/sources}
SRC=$SOURCES/src
DOWN=$SOURCES/downloads
COMPOSITOR_PREFIX=$BUILD/stage/x86_64
COMPOSITOR_HOST=$BUILD/host
FLATPAK_PREFIX=$AOSP/out/matonos/flatpak-ndk/prefix
mkdir -p "$BUILD" "$PREFIX" "$SRC"
CC=$TOOLCHAIN/bin/x86_64-linux-android${API}-clang
CXX=$TOOLCHAIN/bin/x86_64-linux-android${API}-clang++
STRIP=$TOOLCHAIN/bin/llvm-strip

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$COMPOSITOR_PREFIX/lib/pkgconfig:$COMPOSITOR_PREFIX/lib64/pkgconfig:$COMPOSITOR_PREFIX/share/pkgconfig:$COMPOSITOR_HOST/lib/pkgconfig:$COMPOSITOR_HOST/lib/x86_64-linux-gnu/pkgconfig:$COMPOSITOR_HOST/share/pkgconfig:$FLATPAK_PREFIX/lib/pkgconfig"
export PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1
# Meson sees the cross file below; autotools projects get the toolchain via
# environment variables. No sysroot flag: the NDK wrappers carry it.
export CC=$CC CXX=$CXX CPP="$CC -E" AR=$TOOLCHAIN/bin/llvm-ar \
  STRIP=$STRIP RANLIB=$TOOLCHAIN/bin/llvm-ranlib LD=$TOOLCHAIN/bin/ld.lld
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$COMPOSITOR_PREFIX/lib/pkgconfig:$COMPOSITOR_PREFIX/lib64/pkgconfig:$COMPOSITOR_PREFIX/share/pkgconfig:$COMPOSITOR_HOST/lib/pkgconfig:$COMPOSITOR_HOST/lib/x86_64-linux-gnu/pkgconfig:$COMPOSITOR_HOST/share/pkgconfig:$FLATPAK_PREFIX/lib/pkgconfig"
export PATH="$PREFIX/bin:$COMPOSITOR_HOST/bin:$PATH"

# Android provides pthreads in libc, not libpthread. Upstream projects
# unconditionally link -lpthread; an empty archive satisfies the probe, like
# the empty librt archive the compositor build provides.
mkdir -p "$PREFIX/lib"
printf 'void maton_empty_pthread_archive(void) {}\n' > "$BUILD/empty-pthread.c"
"$CC" -fPIC -c "$BUILD/empty-pthread.c" -o "$BUILD/empty-pthread.o"
"$AR" rcs "$PREFIX/lib/libpthread.a" "$BUILD/empty-pthread.o"
# <values.h> is a BSD/glibc header bionic lacks; upstream's futex helper only
# uses it for limits. A shim in our prefix satisfies it.
mkdir -p "$PREFIX/include"
cat > "$PREFIX/include/values.h" <<'EOF'
#include <limits.h>
#ifndef MAXINT
#define MAXINT INT_MAX
#endif
EOF

# Pinned archives; sizes and hashes are recorded in UPSTREAMS.
for entry in \
  'xorgproto-2025.1:56898c716c0578df8a2d828c9c3e5c528277705c0484381a81960fe1a67668e8' \
  'xtrans-1.6.0:faafea166bf2451a173d9d593352940ec6404145c5d1da5c213423ce4d359e92' \
  'util-macros-1.20.2:9ac269eba24f672d7d7b3574e4be5f333d13f04a7712303b1821b2a51ac82e8e' \
  'libpthread-stubs-0.5:59da566decceba7c2a7970a4a03b48d9905f1262ff94410a649224e33d2442bc' \
  'libXau-1.0.12:74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb' \
  'libXdmcp-1.1.5:d8a5222828c3adab70adf69a5583f1d32eb5ece04304f7f8392b6a353aa2228c' \
  'xcb-proto-1.17.0:2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c' \
  'libxcb-1.17.0:599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55' \
  'xcb-util-wm-0.4.2:62c34e21d06264687faea7edbf63632c9f04d55e72114aa4a57bb95e4f888a0b' \
  'xcb-util-errors-1.0.1:5628c87b984259ad927bacd8a42958319c36bdf4b065887803c9d820fb80f357' \
  'libX11-1.8.13:69606f485c2c07c14ef64f75b7bb326d48587af33795d9ab3e607c0b5f94f11c' \
  'libxkbfile-1.2.0:7f71884e5faf56fb0e823f3848599cf9b5a9afce51c90982baeb64f635233ebf' \
  'libXfont2-2.0.9:f042a370666815e7b941e9b7019024755bd1c6c2954afbfa515af378251799e2' \
  'libfontenc-1.1.0:8597e761b7e3624fbf8de4b4f19b9927822f24eeaf15524dc5dbbaf6ae7d8dd1' \
  'libxcvt-0.1.3:a929998a8767de7dfa36d6da4751cdbeef34ed630714f2f4a767b351f2442e01' \
  'libxshmfence-1.3.3:d4a4df096aba96fea02c029ee3a44e11a47eb7f7213c1a729be83e85ec3fde10' \
  'freetype-2.14.1:32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc' \
  'xwayland-24.1.13:173aea3d6f79609164c04528e1c8e4c9b60fcd59391c3c9dad4667297d727fb6'; do
  name=${entry%%:*}; hash=${entry#*:}
  archive=$DOWN/$name.tar.xz
  [[ -s $archive ]] || archive=$DOWN/$name.tar.gz
  [[ -s $archive ]] || die "missing pinned source archive $archive"
  echo "$hash  $archive" | sha256sum -c - >/dev/null || die "checksum mismatch: $name"
done

# Autotools cross build helper. Bionic needs _GNU_SOURCE-free POSIX builds;
# the NDK wrapper defines its own defaults.
configure_build() { # name configure-args...
  local name=$1; shift
  local src=$SRC/$name
  [[ -d $src ]] || { tar -xf "$DOWN/$name.tar.xz" -C "$SRC" 2>/dev/null ||
    tar -xzf "$DOWN/$name.tar.gz" -C "$SRC"; }
  if [[ ! -f $src/config.status ]]; then
    (cd "$src" && ./configure --host=x86_64-linux-android --prefix="$PREFIX" "$@") || die "configure failed: $name"
  fi
  make -C "$src" -j"$JOBS" || die "make failed: $name"
  make -C "$src" install || die "install failed: $name"
  echo "== $name installed"
}

# Meson cross build helper for projects with meson support.
configure_meson() { # name src args...
  local name=$1 src=$2; shift 2
  local dir=$BUILD/xwayland-stack/$name
  [[ -d $src ]] || tar -xf "$DOWN/$(basename "$src").tar.xz" -C "$SRC"
  meson setup "$dir" "$src" --cross-file "$BUILD/android-x86_64-x.ini" \
    --prefix="$PREFIX" --libdir=lib --buildtype=release "$@"
  meson compile -C "$dir" -j"$JOBS"
  meson install -C "$dir" >/dev/null
  echo "== $name installed"
}

cat > "$BUILD/android-x86_64-x.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
c_link_args = ['-L$PREFIX/lib']
EOF

# xorgproto: all X protocol headers + the *proto pc files (incl. xwaylandproto).
configure_build xorgproto-2025.1 --without-xmlto --without-fop
# xtrans: transport headers only.
configure_build xtrans-1.6.0
# util-macros: X.Org autotools macros (installed into the shared prefix).
configure_build util-macros-1.20.2
# libpthread-stubs: pc file only; bionic has pthreads.
configure_build libpthread-stubs-0.5
# xcb-proto: protocol XML + the xcbgen python module used by libxcb's build.
PYTHON=python3 configure_build xcb-proto-1.17.0
# libXau/libXdmcp: X connection auth helpers libxcb requires.
configure_build libXau-1.0.12
configure_build libXdmcp-1.1.5
# libxcb: the X protocol C bindings incl. composite/render/res/xfixes/xkb
# extensions wlroots' xwayland module uses. Threads disabled: bionic provides
# pthreads in libc and libxcb's pthread-stubs probe would fail otherwise.
PYTHON=python3 configure_build libxcb-1.17.0 --without-xcb-xprint \
  --disable-sendfds --disable-xcms
# xcb-util-wm (icccm + ewmh) and xcb-util-errors: wlroots xwm requirements.
configure_build xcb-util-wm-0.4.2
configure_build xcb-util-errors-1.0.1
# libX11: the client-side Xlib for Flatpak apps (links our libxcb).
configure_build libX11-1.8.13 --disable-udb --disable-xlocale
# libxkbfile is meson-only (no configure script); libXfont2 still ships
# autotools. Both are Xwayland's XKB and font backend requirements.
configure_meson libxkbfile "$SRC/libxkbfile-1.2.0"
# freetype: required by libXfont2. Core glyph rendering only; PNG, Bzip2,
# Brotli and HarfBuzz stay disabled.
configure_meson freetype "$SRC/freetype-2.14.1" \
  -Dpng=disabled -Dbzip2=disabled -Dbrotli=disabled -Dharfbuzz=disabled \
  -Dzlib=disabled -Dtests=disabled
# libfontenc: compressed font support libXfont2 requires.
configure_build libfontenc-1.1.0
configure_build libXfont2-2.0.9
# libxcvt: CVT modeline math split out of the X server, required by Xwayland.
configure_meson libxcvt "$SRC/libxcvt-0.1.3"
# libxshmfence: MIT-SHM sync fences; miext/sync includes its header.
configure_build libxshmfence-1.3.3
# Meson's cross pkg-config search misses the external flatpak prefix that
# provides bionic libgcrypt; mirror its pc files (absolute paths inside).
cp "$FLATPAK_PREFIX/lib/pkgconfig/libgcrypt.pc" \
   "$FLATPAK_PREFIX/lib64/pkgconfig/gpg-error.pc" "$PREFIX/lib/pkgconfig/"

# Xwayland: the standalone X server for Wayland (meson). GLX/DRI3/glamor are
# disabled: no GL stack exists in this environment, X11 apps render in
# software. SHA1 comes from the bionic libgcrypt the Flatpak stack already
# ships in system_ext/lib64.
[[ -d $SRC/xwayland-24.1.13 ]] || tar -xf "$DOWN/xwayland-24.1.13.tar.xz" -C "$SRC"
configure_meson xwayland "$SRC/xwayland-24.1.13" \
  -Dglx=false -Dglamor=false -Dxv=false -Ddri3=false -Dmitshm=auto \
  -Dsecure-rpc=false \
  -Dxwayland_ei=false -Dxdmcp=false -Dsystemd_notify=false \
  -Dlibdecor=false -Dxvfb=false -Dsha1=libgcrypt \
  -Dxwayland-path="$PREFIX/bin"

# wlroots reads the xwayland.pc dependency and its have_* feature variables.
# Upstream's generated pc file is replicated here verbatim.
cat > "$PREFIX/lib/pkgconfig/xwayland.pc" <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Xwayland
Description: X Server for Wayland
Version: 24.1.13
URL: https://gitlab.freedesktop.org/xorg/xserver/
# The device runtime path: wlroots bakes this into XWAYLAND_PATH and checks
# access(X_OK) on it at server creation. The build prefix would fail there.
xwayland=/system_ext/bin/Xwayland
have_glamor=false
have_glamor_api=false
have_eglstream=false
have_initfd=true
have_listenfd=true
have_verbose=true
have_terminate_delay=true
have_no_touch_pointer_emulation=true
have_force_xrandr_emulation=true
have_geometry=true
have_fullscreen=true
have_host_grab=true
have_decorate=false
have_enable_ei_portal=false
have_byteswappedclients=true
have_hidpi=true
EOF

echo "X11/Xwayland stack installed under $PREFIX"
ls -la "$PREFIX/bin/Xwayland" 2>/dev/null || die "Xwayland binary missing after build"