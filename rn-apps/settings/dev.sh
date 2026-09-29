#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
AOSP=$(cd "$ROOT/../.." && pwd)
DEVICE_TREE=$AOSP/device/maton/pc_x86_64
ADB=${ADB:-$AOSP/out/host/linux-x86/bin/adb}
DEVICE=${MATON_ADB_DEVICE:-127.0.0.1:5564}
KEY_DIR=${MATON_KEY_DIR:-$HOME/.config/matonos-keys}
SDK=${MATON_ANDROID_SDK:-$HOME/Documents/matonos-android-sdk}
JAVA_HOME=${JAVA_HOME:-$AOSP/prebuilts/jdk/jdk21/linux-x86}
METRO_PID=$ROOT/.metro.pid
METRO_LOG=$ROOT/.metro.log
EXPECTED_CERT=8e4be1a54dfcf59bb0f8c37fc3d912ba3ded66e35cc47b264dad7b56592df01d
fail(){ echo "dev.sh: $*" >&2; exit 1; }
[[ -x $ADB ]] || fail "adb not found at $ADB"
status=$(tail -n 1 /mnt/data/aosp/out/pc-logs/agents/build-status.txt 2>/dev/null || true)
[[ $status != *RUNNING* ]] || fail "coordinated image build is RUNNING; wait before building the debug app"
adb_device(){ "$ADB" -s "$DEVICE" "$@"; }
is_our_metro(){ local pid=${1:-}; [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null && ps -p "$pid" -o args= | grep -Fq "$ROOT/node_modules/.bin/expo start"; }
stop_metro(){ if [[ -s $METRO_PID ]]; then local pid; pid=$(<"$METRO_PID"); if is_our_metro "$pid"; then kill "$pid" 2>/dev/null || true; fi; rm -f "$METRO_PID"; fi; }
[[ -f $KEY_DIR/matonos-settings.jks && -s $KEY_DIR/matonos-settings.pass ]] || fail "settings signing key is missing under $KEY_DIR"
[[ -x $SDK/build-tools/36.0.0/apksigner ]] || fail "Android build-tools 36.0.0 are missing"
"$ADB" connect "$DEVICE" >/dev/null 2>&1 || true
adb_device get-state >/dev/null 2>&1 || fail "device $DEVICE is not connected; start its headless QEMU slot first"
cd "$ROOT"
stop_metro
npm ci --ignore-scripts
npm run typecheck || echo "WARNING: TypeScript errors above (dev loop continues; fix before committing)" >&2
export ANDROID_HOME=$SDK ANDROID_SDK_ROOT=$SDK
if [[ -x $JAVA_HOME/bin/java ]]; then export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"; fi
export MATON_SIGNING_STORE_FILE=$KEY_DIR/matonos-settings.jks
MATON_SIGNING_STORE_PASSWORD=$(<"$KEY_DIR/matonos-settings.pass")
export MATON_SIGNING_STORE_PASSWORD MATON_SIGNING_KEY_ALIAS=matonos-dev MATON_SIGNING_KEY_PASSWORD=$MATON_SIGNING_STORE_PASSWORD
CI=1 npx expo prebuild --platform android --clean --no-install
./android/gradlew -p android --no-daemon --max-workers 2 :app:assembleDebug
APK=$ROOT/android/app/build/outputs/apk/debug/app-debug.apk
[[ -s $APK ]] || fail "Gradle did not produce $APK"
CERT=$("$SDK/build-tools/36.0.0/apksigner" verify --print-certs "$APK" | awk -F': ' '/Signer #1 certificate SHA-256 digest:/ {print $2; exit}')
[[ ${CERT,,} == "$EXPECTED_CERT" ]] || fail "debug APK signer mismatch: ${CERT:-no certificate}"
if curl --silent --fail http://localhost:8084/status | grep -q 'packager-status:running'; then
  [[ -s $METRO_PID ]] && is_our_metro "$(<"$METRO_PID")" || fail "port 8084 belongs to another Metro process"
  stop_metro
fi
nohup "$ROOT/node_modules/.bin/expo" start --dev-client --localhost --port 8084 --clear >"$METRO_LOG" 2>&1 </dev/null &
echo $! > "$METRO_PID"
ready=0
for _ in {1..120}; do if curl --silent --fail http://localhost:8084/status | grep -q 'packager-status:running'; then ready=1; break; fi; sleep .25; done
[[ $ready == 1 ]] || { stop_metro; tail -80 "$METRO_LOG" >&2; fail "Expo Metro did not start"; }
adb_device install -r "$APK"
DEV_HOST=${MATON_DEV_HOST:-10.0.2.2}
prefs=$(mktemp)
printf '<?xml version="1.0" encoding="utf-8" standalone="yes" ?>\n<map>\n    <string name="debug_http_host">%s:8084</string>\n</map>\n' "$DEV_HOST" > "$prefs"
adb_device push "$prefs" "/data/local/tmp/org.matonos.settings.devhost.xml" >/dev/null
rm -f "$prefs"
adb_device shell "run-as org.matonos.settings sh -c 'mkdir -p shared_prefs && cat /data/local/tmp/org.matonos.settings.devhost.xml > shared_prefs/org.matonos.settings_preferences.xml'" || echo "WARNING: could not set the dev server host" >&2
adb_device shell am start -n org.matonos.settings/.MainActivity >/dev/null
echo "Settings dev client installed on $DEVICE; Metro uses 10.0.2.2:8084. Log: $METRO_LOG"
