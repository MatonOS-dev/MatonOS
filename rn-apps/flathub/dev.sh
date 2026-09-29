#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
AOSP=$(cd "$ROOT/../.." && pwd)
DEVICE_TREE=$AOSP/device/maton/pc_x86_64
ADB=${ADB:-$AOSP/out/host/linux-x86/bin/adb}
DEVICE=${MATON_ADB_DEVICE:-127.0.0.1:5562}
APK=$DEVICE_TREE/prebuilt/apps-built/Flathub.apk
[[ -x $ADB ]] || { echo "dev.sh: adb not found at $ADB" >&2; exit 1; }
status=$(tail -n 1 /mnt/data/aosp/out/pc-logs/agents/build-status.txt 2>/dev/null || true)
[[ $status != *RUNNING* ]] || { echo "dev.sh: coordinated image build is RUNNING" >&2; exit 1; }
MATON_APPS_ONLY=matonos-flathub MATON_APPS_FORCE=1 "$DEVICE_TREE/tools/build-apps.sh"
[[ -s $APK ]] || { echo "dev.sh: app build did not stage $APK" >&2; exit 1; }
"$ADB" connect "$DEVICE" >/dev/null 2>&1 || true
"$ADB" -s "$DEVICE" get-state >/dev/null
"$ADB" -s "$DEVICE" install -r "$APK"
"$ADB" -s "$DEVICE" shell am start -n org.matonos.flathub/.MainActivity
echo "Software Centre installed on $DEVICE."
