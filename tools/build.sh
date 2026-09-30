#!/usr/bin/env bash
# build.sh: build pc_x86_64 end to end.
#
#   kernel (build-kernel.sh) -> Mesa (build-mesa.sh) -> AOSP patches
#   (apply-patches.sh) -> preinstalled apps (fetch-apps.sh) -> AOSP build -> live image (make-live.sh) and/or
#   installer payload (make-payload.sh)
#
# Usage: ./build.sh [-K] [-M] [-A] [-L] [-p] [-m <modules>] [-j <jobs>] [-v <variant>]
#
#   -K  skip the kernel build (use prebuilt/ as is)
#   -M  skip the Mesa build (use prebuilt/mesa as is)
#   -A  skip the AOSP build
#   -L  don't make the live image
#   -p  also make the installer payload (make-payload.sh)
#   -m  build only these AOSP modules (comma-separated, e.g. SystemUI);
#       implies -K -M -L, no images. Uses the same full-product Soong graph
#       as image builds, so switching between -m and full builds (or between
#       agents' modules) costs no re-analysis. MATON_PARTIAL_ANALYSIS=1
#       limits analysis to the modules instead (SOONG_PARTIAL_ANALYSIS): less
#       RAM for that one build, but every change of the module list or
#       switch back to a full build re-runs the whole analysis (~18 GB).
#
# Soong analysis is incremental (SOONG_INCREMENTAL_ANALYSIS=true): after an
# Android.bp change only the affected parts are re-analysed.
#   -j  AOSP build jobs (default: nproc; lower it if the build runs out of RAM)
#   -v  build variant (default: userdebug)
set -Eeuo pipefail

# Only the coordinating agent starts builds (user rule, 2026-09-24): Codex
# sub-agents request one instead. MATON_BUILD_COORDINATOR=1 marks the
# coordinator; the scope re-exec below keeps the environment.
if [[ -z ${MATON_BUILD_COORDINATOR:-} ]]; then
  _agents=$(readlink -f "$(dirname "$(readlink -f "$0")")/../../../..")/out/pc-logs/agents
  cat >&2 <<EOM
ERROR: builds are started only by the coordinating agent (Claude).
Request one: append a line "<area>: <modules or full image> — <why>" to
  $_agents/build-requests.txt
then poll $_agents/build-status.txt for the result (image path, errors).
EOM
  exit 3
fi

# Run the whole build in its own memory-limited scope: above MemoryHigh the
# kernel reclaims the build's memory to swap instead of letting it take all
# RAM (Soong analysis alone peaks around 18 GB on a 23 GB host, and the
# desktop, QEMU or a low-memory watchdog then kill things).
# MATON_BUILD_MEM_HIGH overrides the limit; MATON_BUILD_NO_SCOPE=1 disables it.
if [[ -z ${MATON_BUILD_IN_SCOPE:-} && -z ${MATON_BUILD_NO_SCOPE:-} ]] &&
   command -v systemd-run >/dev/null &&
   systemd-run --user --scope --quiet true 2>/dev/null; then
  export MATON_BUILD_IN_SCOPE=1
  exec systemd-run --user --scope --quiet \
    -p "MemoryHigh=${MATON_BUILD_MEM_HIGH:-20G}" -p MemorySwapMax=infinity \
    "$(readlink -f "$0")" "$@"
fi

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo; echo "######## $*"; }

TOOLS=$(dirname "$(readlink -f "$0")")
DEVICE_DIR=$(dirname "$TOOLS")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
PRODUCT=pc_x86_64
RELEASE=aosp_current
VARIANT=userdebug
JOBS=$(nproc)
DO_KERNEL=1 DO_MESA=1 DO_AOSP=1 DO_LIVE=1 DO_PAYLOAD=0
MODULES=""

while getopts "KMALpm:j:v:h" opt; do
  case $opt in
    K) DO_KERNEL=0 ;;
    M) DO_MESA=0 ;;
    A) DO_AOSP=0 ;;
    L) DO_LIVE=0 ;;
    p) DO_PAYLOAD=1 ;;
    m) MODULES=$OPTARG; DO_KERNEL=0 DO_MESA=0 DO_LIVE=0 DO_PAYLOAD=0 ;;
    j) JOBS=$OPTARG ;;
    v) VARIANT=$OPTARG ;;
    *) sed -n '2,17p' "$0"; exit 1 ;;
  esac
done

PRODUCT_OUT=$AOSP/out/target/product/$PRODUCT
start=$SECONDS

# Host requirement (user, 2026-09-25): zram swap. Soong analysis needs far
# more memory than typical hosts have; zram compresses its heap ~6x and keeps
# swapping at RAM speed (analysis ~11 min instead of 35–60 on SSD swap).
# Required: an active zram swap >= 16 GiB with the highest swap priority.
# MATON_SKIP_ZRAM_CHECK=1 bypasses (hosts that can't use zram).
if [[ -z ${MATON_SKIP_ZRAM_CHECK:-} ]]; then
  zram_ok=$(awk 'NR>1 && $1 ~ /zram/ {print $3, $5}' /proc/swaps | sort -k2 -n | tail -1)
  top_prio=$(awk 'NR>1 {print $5}' /proc/swaps | sort -n | tail -1)
  zram_kb=${zram_ok%% *}; zram_prio=${zram_ok##* }
  if [[ -z $zram_ok || ${zram_kb:-0} -lt $((16 * 1024 * 1024)) || ${zram_prio} != "${top_prio}" ]]; then
    cat >&2 <<'EOM'
ERROR: zram swap is required to build MatonOS (>= 16 GiB, highest priority).
Set it up once (systemd-zram-generator), then reboot or restart the service:
  sudo apt install -y systemd-zram-generator
  printf '[zram0]\nzram-size = 32768\ncompression-algorithm = zstd\nswap-priority = 200\n' \
    | sudo tee /etc/systemd/zram-generator.conf
  sudo systemctl daemon-reload && sudo systemctl restart systemd-zram-setup@zram0.service
Check with: swapon --show   (bypass: MATON_SKIP_ZRAM_CHECK=1)
EOM
    exit 1
  fi
fi

# Static checks for the failure types that otherwise surface only after a
# ~40 min Soong analysis (tools/preflight.sh, preflight/README.md).
# MATON_SKIP_PREFLIGHT=1 bypasses (coordinator only, with reason).
if [[ -z ${MATON_SKIP_PREFLIGHT:-} ]]; then
  info "Preflight"
  "$TOOLS/preflight.sh" || die "preflight failed (see above)"
fi

# One build at a time: several agents share out/ and the host's 23 GB of RAM.
# Wait for other build.sh runs (lock) and for builds started without build.sh
# (a bare `m` of the launcher agent).
# MATON_BUILD_LOCK_HELD=1: the caller already holds the lock (e.g. a wrapper
# that prunes the tree and builds under one lock).
LOCK=$AOSP/out/.maton-build.lock
if [[ -z ${MATON_BUILD_LOCK_HELD:-} ]]; then
  mkdir -p "$AOSP/out"
  exec 9>"$LOCK"
  if ! flock -n 9; then
    info "Waiting for another build.sh to finish ($LOCK)"
    flock 9
  fi
fi
SOONG_WAIT_TIMEOUT=${MATON_SOONG_WAIT_TIMEOUT:-7200}
[[ $SOONG_WAIT_TIMEOUT =~ ^[1-9][0-9]*$ ]] || die "MATON_SOONG_WAIT_TIMEOUT must be a positive number of seconds"
soong_wait_started=$SECONDS
while pgrep -x soong_ui >/dev/null; do
  waited=$((SECONDS - soong_wait_started))
  (( waited < SOONG_WAIT_TIMEOUT )) || die "timed out after ${waited}s waiting for a running AOSP build (soong_ui); retry when it exits or set MATON_SOONG_WAIT_TIMEOUT"
  info "Waiting for a running AOSP build (soong_ui; ${waited}/${SOONG_WAIT_TIMEOUT}s)"
  sleep 30
done

if [[ $DO_KERNEL == 1 ]]; then
  info "Kernel"
  "$TOOLS/build-kernel.sh"
fi
[[ -f $DEVICE_DIR/prebuilt/bzImage ]] || die "no staged kernel; run without -K"

if [[ $DO_MESA == 1 ]]; then
  info "Mesa"
  "$TOOLS/build-mesa.sh"
fi
[[ -d $DEVICE_DIR/prebuilt/mesa ]] || die "no staged Mesa; run without -M"

if [[ $DO_AOSP == 1 ]]; then
  info "MatonOS Gradle apps"
  "$TOOLS/build-apps.sh"

  info "MatonOS native daemons"
  "$TOOLS/build-native.sh"

  info "MatonOS ODM driver bundle"
  # Distinct path: AOSP copies this as odm.img via BOARD_PREBUILT_ODMIMAGE
  # (install/BoardConfig.mk). Writing straight to $PRODUCT_OUT/odm.img let the
  # AOSP build overwrite the bundle (boot loop). See SHARED-CHANGES.md.
  "$TOOLS/build-bundle.sh" -o "$PRODUCT_OUT/odm-bundle.img"

  info "AOSP patches"
  "$TOOLS/apply-patches.sh"

  info "Preinstalled apps"
  "$TOOLS/fetch-apps.sh"
  "$TOOLS/../gms/fetch-gms.sh"

  info "AOSP build ($PRODUCT-$RELEASE-$VARIANT, -j$JOBS${MODULES:+, modules: $MODULES})"
  # envsetup/lunch aren't set -u clean; run them in a subshell without it.
  (
    set +u
    cd "$AOSP"
    # shellcheck source=/dev/null
    source build/envsetup.sh >/dev/null
    lunch "$PRODUCT-$RELEASE-$VARIANT"
    # Overridable: incremental analysis can panic after heavy graph churn
    # ("growslice: len out of range" in writeIncrementalModules); one run with
    # SOONG_INCREMENTAL_ANALYSIS=false rebuilds its state.
    export SOONG_INCREMENTAL_ANALYSIS=${SOONG_INCREMENTAL_ANALYSIS:-true}
    # Pruned tree (tools/prune-tree.sh): tests elsewhere reference the hidden
    # test suites; only fail modules that are actually built.
    [[ -f $AOSP/.maton-pruned ]] && export ALLOW_MISSING_DEPENDENCIES=true
    # ccache for C/C++ (Soong: CC_WRAPPER; Make: USE_CCACHE + CCACHE_EXEC)
    # once ccache is installed (see CLAUDE.md).
    # Switching it on or off changes every compile command, so the first
    # build afterwards recompiles all C/C++ once. MATON_CCACHE=0 disables.
    # The cache must live under out/: the build sandbox makes everything
    # else read-only ("ccache: error: Read-only file system").
    ccache_dir=${CCACHE_DIR:-$AOSP/out/ccache}
    if [[ ${MATON_CCACHE:-1} != 0 ]] && command -v ccache >/dev/null &&
       mkdir -p "$ccache_dir"; then
      [[ -f $ccache_dir/ccache.conf ]] || CCACHE_DIR=$ccache_dir ccache -M 40G >/dev/null
      export USE_CCACHE=true CCACHE_DIR=$ccache_dir
      CCACHE_EXEC=$(command -v ccache); export CCACHE_EXEC
      export CC_WRAPPER=$CCACHE_EXEC
      export CCACHE_COMPILERCHECK=content CCACHE_BASEDIR=$AOSP
      export CCACHE_SLOPPINESS=time_macros,include_file_mtime,file_macro
    fi
    if [[ -n $MODULES ]]; then
      [[ -n ${MATON_PARTIAL_ANALYSIS:-} ]] && export SOONG_PARTIAL_ANALYSIS=$MODULES
      # shellcheck disable=SC2046
      m -j"$JOBS" $(tr ',' ' ' <<<"$MODULES")
    else
      m -j"$JOBS"
    fi
  ) 9>&-  # the lock stays with this script, not with lingering build daemons

  [[ -n $MODULES ]] && { info "Modules built"; exit 0; }

  info "SELinux label check"
  # init refuses to start vendor programs without an exec label, even in
  # permissive mode; catch that here instead of at boot.
  "$TOOLS/check-selinux-labels.sh" -o "$PRODUCT_OUT"
fi

if [[ $DO_LIVE == 1 ]]; then
  info "Live image"
  "$TOOLS/make-live.sh" -o "$PRODUCT_OUT"
fi

if [[ $DO_PAYLOAD == 1 ]]; then
  info "Installer payload"
  PATH=$AOSP/out/host/linux-x86/bin:$PATH \
    "$TOOLS/make-payload.sh" -o "$PRODUCT_OUT" -k "$DEVICE_DIR/prebuilt/bzImage" \
    -d "$PRODUCT_OUT/payload" -S
fi

info "Done in $(( (SECONDS - start) / 60 )) min"
