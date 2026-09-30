#!/usr/bin/env bash
# dev-sync.sh: rapid iteration on a running MatonOS (QEMU or PC, over adb)
# without rebuilding images or rebooting.
#
# Build outputs under out/target/product/pc_x86_64/{system,system_ext,product,
# vendor} that are newer than the live image are pushed to /data/maton-devsync
# and bind-mounted over the read-only (erofs) originals. Then only what is
# needed restarts: the touched apps (SystemUI, Launcher3, Settings, ...) are
# killed and come back with the new code, or the framework is soft-restarted
# (`stop; start`, no reboot) when framework jars / native libraries changed.
#
# Usage: dev-sync.sh [-m <modules>] [-r auto|framework|apps|none] [-a] [-n]
#
#   -m  build these modules first (comma-separated, via build.sh -m), e.g.
#       -m SystemUI  -m Launcher3QuickStep  -m services  -m Settings
#   -r  restart mode (default auto: framework if anything outside an app
#       directory changed, else just the changed apps)
#   -a  push everything newer than the image again (default: only files that
#       changed since the last sync in this boot)
#   -n  dry run: list what would be pushed
#
# Mounts last until the device reboots (on the live image /data is a RAM
# disk anyway); after a reboot just run dev-sync.sh again. Files that don't
# exist in the image can't be bind-mounted; they are skipped (new files need
# an image build; a new APK can be `adb install`ed, unprivileged).
# Manifest/permission changes of an app need `-r framework` (package manager
# only re-parses packages when it starts).
# ADB_SERIAL (default 127.0.0.1:5555, the QEMU forward) selects the device.
set -Eeuo pipefail

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

TOOLS=$(dirname "$(readlink -f "$0")")
DEVICE_DIR=$(dirname "$TOOLS")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
OUT=$AOSP/out/target/product/pc_x86_64
IMAGE=${MATON_DEVSYNC_SINCE:-$OUT/matonos-live-x86_64.img}
STATE=$AOSP/out/pc-logs/devsync
DEV_DIR=/data/maton-devsync
export PATH=$AOSP/out/host/linux-x86/bin:$PATH
export ANDROID_SERIAL=${ADB_SERIAL:-127.0.0.1:5555}

MODULES="" RESTART=auto ALL=0 DRY=0
while getopts "m:r:anh" opt; do
  case $opt in
    m) MODULES=$OPTARG ;;
    r) RESTART=$OPTARG ;;
    a) ALL=1 ;;
    n) DRY=1 ;;
    *) sed -n '2,/^set -E/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
  esac
done
case $RESTART in auto|framework|apps|none) ;; *) die "bad -r $RESTART" ;; esac
[[ -f $IMAGE ]] || die "no reference image $IMAGE (set MATON_DEVSYNC_SINCE)"

if [[ -n $MODULES ]]; then
  info "building $MODULES"
  "$TOOLS/build.sh" -m "$MODULES"
fi

# --- device -----------------------------------------------------------------
if [[ $ANDROID_SERIAL == *:* ]]; then adb connect "$ANDROID_SERIAL" >/dev/null || true; fi
timeout 20 adb wait-for-device || die "no adb device at $ANDROID_SERIAL"
if [[ $(adb shell id -u) != 0 ]]; then
  adb root >/dev/null
  sleep 2
  [[ $ANDROID_SERIAL == *:* ]] && adb connect "$ANDROID_SERIAL" >/dev/null || true
  timeout 20 adb wait-for-device
  [[ $(adb shell id -u) == 0 ]] || die "adb root failed (userdebug build needed)"
fi
BOOT_ID=$(adb shell cat /proc/sys/kernel/random/boot_id | tr -d '\r')
mkdir -p "$STATE"
SYNCED=$STATE/${ANDROID_SERIAL//[^A-Za-z0-9]/_}-$BOOT_ID.list
touch "$SYNCED"
((ALL)) && : > "$SYNCED"

# --- what changed -----------------------------------------------------------
# "<device path> <mtime>" for every output newer than the image.
mapfile -t CHANGED < <(cd "$OUT" &&
  find system system_ext product vendor -type f -newer "$IMAGE" \
    -printf '/%p %T@\n' 2>/dev/null | sort)
TODO=()
for line in "${CHANGED[@]}"; do
  grep -qxF "$line" "$SYNCED" || TODO+=("${line% *}")
done
if ((${#TODO[@]} == 0)); then
  info "nothing new since the last sync (image: $(date -r "$IMAGE" '+%F %T'))"
  exit 0
fi
# /system/... in out/ is /system/... on the device; the others are top-level.
printf '  %s\n' "${TODO[@]}"
((DRY)) && exit 0

# Which exist on the device (bind-mountable) and which are new?
EXIST=() NEW=()
# shellcheck disable=SC2016 # expands on the device
mapfile -t PRESENT < <(printf '%s\n' "${TODO[@]}" |
  adb shell 'while read -r p; do [ -e "$p" ] && echo "$p"; done' | tr -d '\r')
for p in "${TODO[@]}"; do
  if printf '%s\n' "${PRESENT[@]}" | grep -qxF "$p"; then EXIST+=("$p"); else NEW+=("$p"); fi
done

# --- push + bind mount ------------------------------------------------------
if ((${#EXIST[@]})); then
  STAGE=$STATE/stage
  rm -rf "$STAGE"; mkdir -p "$STAGE"
  for p in "${EXIST[@]}"; do
    mkdir -p "$STAGE$(dirname "$p")"
    cp -p "$OUT$p" "$STAGE$p"
  done
  adb shell "rm -rf $DEV_DIR/incoming; mkdir -p $DEV_DIR/incoming"
  info "pushing ${#EXIST[@]} file(s)"
  adb push -q "$STAGE/." "$DEV_DIR/incoming/" >/dev/null
  # In adbd's mount namespace = init's default namespace, which zygote and
  # every service share; mounts propagate into app namespaces.
  printf '%s\n' "${EXIST[@]}" | adb shell "
    D=$DEV_DIR
    while read -r p; do
      # Lazy: running processes keep the old file mapped until restarted.
      while grep -Fq \" \$p \" /proc/self/mountinfo; do umount -l \"\$p\" || break; done
      mkdir -p \"\$D/files\$(dirname \"\$p\")\"
      mv -f \"\$D/incoming\$p\" \"\$D/files\$p\"
      ctx=\$(ls -Zd \"\$p\" | awk '{print \$1}')
      chcon \"\$ctx\" \"\$D/files\$p\"
      # Same mode as the original (adb push leaves 0666; ART refuses to load
      # code from a writable file).
      chmod \$(stat -c %a \"\$p\") \"\$D/files\$p\"
      if mount -o bind \"\$D/files\$p\" \"\$p\"; then echo \"  mounted \$p\"
      else echo \"  FAILED \$p\"; fi
    done
    rm -rf \$D/incoming; true"
fi

for p in "${NEW[@]}"; do
  if [[ $p == *.apk ]]; then
    echo "SKIP $p: not in the running image; \`adb install -r $OUT$p\` runs it" \
      "unprivileged, an image build installs it properly" >&2
  elif [[ $p != */oat/* ]]; then
    echo "SKIP $p: not in the running image (needs an image build)" >&2
  fi
done

# Remember what's done for this boot.
for line in "${CHANGED[@]}"; do
  grep -qxF "$line" "$SYNCED" || echo "$line" >> "$SYNCED"
done

# --- restart ----------------------------------------------------------------
APPDIRS=()
NONAPP=0
for p in "${EXIST[@]}"; do
  if [[ $p =~ ^(/[a-z_]+(/system)?)?/(priv-app|app)/[^/]+/ ]]; then
    APPDIRS+=("$(echo "$p" | grep -oE '^.*/(priv-app|app)/[^/]+')")
  else
    NONAPP=1
  fi
done
mapfile -t APPDIRS < <(printf '%s\n' "${APPDIRS[@]}" | sort -u | sed '/^$/d')

if [[ $RESTART == auto ]]; then
  if ((NONAPP)); then RESTART=framework; else RESTART=apps; fi
fi
case $RESTART in
  framework)
    info "soft restart of the framework (stop; start)"
    OLD=$(adb shell pidof system_server | tr -d '\r')
    adb shell 'stop; start'
    # Wait for a new system_server with SystemUI up, then wake the display
    # (the restart leaves it asleep) and drop the keyguard.
    for _ in $(seq 60); do
      sleep 2
      NOW=$(adb shell 'pidof system_server; pidof com.android.systemui' | tr -d '\r' | xargs)
      [[ $NOW == *\ * && $NOW != "$OLD "* ]] && break
    done
    sleep 3
    adb shell 'input keyevent WAKEUP; wm dismiss-keyguard' || true
    info "framework back up"
    ;;
  apps)
    ((${#APPDIRS[@]})) || exit 0
    PKGS=$(adb shell pm list packages -f | tr -d '\r' | sed 's/^package://' |
      while IFS= read -r l; do
        path=${l%=*}
        for d in "${APPDIRS[@]}"; do
          [[ $path == "$d/"* ]] && echo "${l##*=}"
        done
      done | sort -u)
    for pkg in $PKGS; do
      info "restarting $pkg"
      # Killing (not force-stop) lets persistent apps like SystemUI and the
      # home app come straight back.
      adb shell "pids=\$(pidof $pkg); [ -n \"\$pids\" ] && kill \$pids; true"
    done
    ;;
esac
