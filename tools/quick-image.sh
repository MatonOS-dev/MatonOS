#!/usr/bin/env bash
# quick-image.sh: reuse the existing combined AOSP graph when config is unchanged.
# Usage: MATON_BUILD_COORDINATOR=1 tools/quick-image.sh [--bundle-only [--dry-run]]
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

[[ ${MATON_BUILD_COORDINATOR:-} == 1 ]] || die "quick-image is coordinator-only; set MATON_BUILD_COORDINATOR=1."

TOOLS=$(dirname "$(readlink -f "$0")")
DEVICE_DIR=$(dirname "$TOOLS")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
PRODUCT=pc_x86_64
VARIANT=userdebug
PRODUCT_OUT=$AOSP/out/target/product/$PRODUCT
# build-bundle.sh stages here; install/BoardConfig.mk copies it to odm.img via
# BOARD_PREBUILT_ODMIMAGE during a full build. The quick paths below do not run
# that copy, so they stage the bundle and mirror it to odm.img themselves.
BUNDLE_IMG=$PRODUCT_OUT/odm-bundle.img
ODM_IMG=$PRODUCT_OUT/odm.img
BUNDLE_ONLY=0
DRY_RUN=0

for arg in "$@"; do
  case "$arg" in
    --bundle-only) BUNDLE_ONLY=1 ;;
    --dry-run) DRY_RUN=1 ;;
    -h|--help)
      sed -n '2,4p' "$0"
      exit 0
      ;;
    *) die "unknown option: $arg" ;;
  esac
done
(( DRY_RUN == 0 || BUNDLE_ONLY == 1 )) || die "--dry-run is supported with --bundle-only only."

if [[ $BUNDLE_ONLY == 1 ]]; then
  LOCK=$AOSP/out/.maton-build.lock
  mkdir -p "$AOSP/out"
  exec 9>"$LOCK"
  flock -n 9 || die "another AOSP build holds $LOCK; retry after it finishes."
  pgrep -x soong_ui >/dev/null && die "an AOSP build is running (soong_ui); retry after it finishes."
  [[ -s $DEVICE_DIR/bundle/contents.list ]] || die "bundle registry missing: $DEVICE_DIR/bundle/contents.list"
  [[ -d $PRODUCT_OUT ]] || die "product output directory missing: $PRODUCT_OUT"
  for partition in system system_ext product vendor; do
    [[ -s $PRODUCT_OUT/$partition.img ]] || die "required partition image is missing: $PRODUCT_OUT/$partition.img"
  done
  if [[ $DRY_RUN == 1 ]]; then
    info "Dry run: would rebuild the ODM bundle, then repack the live image. No files changed."
    printf '  %q -o %q\n' "$TOOLS/build-bundle.sh" "$BUNDLE_IMG"
    printf '  cp -f %q %q\n' "$BUNDLE_IMG" "$ODM_IMG"
    printf '  %q -o %q\n' "$TOOLS/make-live.sh" "$PRODUCT_OUT"
    exit 0
  fi
  "$TOOLS/build-bundle.sh" -o "$BUNDLE_IMG"
  cp -f "$BUNDLE_IMG" "$ODM_IMG"
  "$TOOLS/make-live.sh" -o "$PRODUCT_OUT"
  exit 0
fi

[[ $DRY_RUN == 0 ]] || die "invalid dry-run mode."
"$TOOLS/preflight.sh"

GRAPH=$AOSP/out/soong/build.$PRODUCT.ninja
COMBINED=$AOSP/out/combined-$PRODUCT.ninja
[[ -s $GRAPH ]] || die "Soong Ninja graph missing: $GRAPH"
[[ -s $COMBINED ]] || die "combined Ninja graph missing: $COMBINED"

# Match build.sh's product environment and ccache settings, then run the
# environment comparison before handing the already generated graph to Ninja.
cd "$AOSP"
# shellcheck source=/dev/null
source build/envsetup.sh >/dev/null
lunch "$PRODUCT-aosp_current-$VARIANT" >/dev/null
export SOONG_INCREMENTAL_ANALYSIS=${SOONG_INCREMENTAL_ANALYSIS:-true}
if [[ -f $AOSP/.maton-pruned ]]; then
  export ALLOW_MISSING_DEPENDENCIES=true
fi
ccache_dir=${CCACHE_DIR:-$AOSP/ccache}
if [[ ${MATON_CCACHE:-1} != 0 ]] && command -v ccache >/dev/null && mkdir -p "$ccache_dir"; then
  [[ -f $ccache_dir/ccache.conf ]] || CCACHE_DIR=$ccache_dir ccache -M 40G >/dev/null
  export USE_CCACHE=true CCACHE_DIR=$ccache_dir
  CCACHE_EXEC=$(command -v ccache); export CCACHE_EXEC
  export CC_WRAPPER=$CCACHE_EXEC
  export CCACHE_COMPILERCHECK=content CCACHE_BASEDIR=$AOSP
  export CCACHE_SLOPPINESS=time_macros,include_file_mtime,file_macro
fi

python3 "$DEVICE_DIR/preflight/quick_image.py" "$AOSP" "$DEVICE_DIR" "$GRAPH"

LOCK=$AOSP/out/.maton-build.lock
mkdir -p "$AOSP/out"
exec 9>"$LOCK"
flock -n 9 || die "another AOSP build holds $LOCK; retry after it finishes."
if pgrep -x soong_ui >/dev/null; then
  die "soong_ui is running; quick-image will not overlap a full build."
fi

NINJA=${MATON_NINJA:-$AOSP/prebuilts/build-tools/linux-x86/bin/ninja}
[[ -x $NINJA ]] || die "AOSP Ninja executable not found: $NINJA"
JOBS=${MATON_BUILD_JOBS:-6}
[[ $JOBS =~ ^[1-6]$ ]] || die "MATON_BUILD_JOBS must be between 1 and 6 (host memory limit)."
# make-live.sh also packs both ramdisks (init, fstab): keep them current too.
TARGETS=(systemimage systemextimage vendorimage productimage
  "out/target/product/$PRODUCT/ramdisk.img" "out/target/product/$PRODUCT/vendor_ramdisk.img")
info "Building image targets on the existing combined graph (-j$JOBS): ${TARGETS[*]}"
"$NINJA" -f "$COMBINED" -j"$JOBS" "${TARGETS[@]}"

info "Rebuilding the ODM driver bundle and live image"
"$TOOLS/build-bundle.sh" -o "$BUNDLE_IMG"
cp -f "$BUNDLE_IMG" "$ODM_IMG"
"$TOOLS/make-live.sh" -o "$PRODUCT_OUT"
