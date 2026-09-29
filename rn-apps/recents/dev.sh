#!/usr/bin/env bash
set -Eeuo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PACKAGE=org.matonos.recents
DEVICE=${MATON_ADB_DEVICE:-127.0.0.1:5555}
. "$(dirname -- "$(readlink -f -- "$0")")/../../tools/local-env.sh"
ADB=${ADB:-$MATON_ROOT/out/host/linux-x86/bin/adb}
SDK=${MATON_ANDROID_SDK:-$HOME/Documents/matonos-android-sdk}
KEY_DIR=${MATON_KEY_DIR:-$HOME/.config/matonos-keys}
JAVA_HOME=${JAVA_HOME:-$MATON_ROOT/prebuilts/jdk/jdk21/linux-x86}
METRO_PORT=8083
METRO_PID=$ROOT/.metro.pid
METRO_LOG=$ROOT/.metro.log

fail() { echo "dev.sh: $*" >&2; exit 1; }
[[ -x $ADB ]] || fail "adb not found: $ADB"
device() { "$ADB" -s "$DEVICE" "$@"; }
is_our_metro() {
  local pid=${1:-}
  [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null \
    && ps -p "$pid" -o args= | grep -Fq "$ROOT/node_modules/.bin/expo start"
}
stop_metro() {
  if [[ -s $METRO_PID ]]; then
    local pid
    pid=$(<"$METRO_PID")
    if is_our_metro "$pid"; then kill "$pid" 2>/dev/null || true; fi
    rm -f "$METRO_PID"
  fi
}

if [[ ${1:-} == --reset ]]; then
  [[ $# == 1 ]] || fail "usage: ./dev.sh [--reset]"
  "$ADB" connect "$DEVICE" >/dev/null 2>&1 || true
  device get-state >/dev/null 2>&1 || fail "device $DEVICE is not connected"
  result=$(device shell pm uninstall-system-updates "$PACKAGE" 2>&1) || true
  [[ $result == *Success* ]] || fail "could not restore image Recents APK: $result"
  device shell am force-stop "$PACKAGE" || true
  device reverse --remove "tcp:$METRO_PORT" >/dev/null 2>&1 || true
  stop_metro
  device shell am start -a android.intent.action.MAIN -c android.intent.category.HOME -f 0x10000000 >/dev/null
  echo "Restored the image's Hermes-bytecode Recents app."
  exit 0
elif [[ $# -ne 0 ]]; then
  fail "usage: ./dev.sh [--reset]"
fi

[[ -s $KEY_DIR/matonos-recents.jks && -s $KEY_DIR/matonos-recents.pass ]] \
  || fail "Recents signing key missing under $KEY_DIR; build-apps.sh creates it"
[[ -x $SDK/build-tools/36.0.0/apksigner ]] || fail "Android build-tools 36.0.0 are missing under $SDK"
export ANDROID_HOME=$SDK ANDROID_SDK_ROOT=$SDK
if [[ -x $JAVA_HOME/bin/java ]]; then export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"; fi
export MATON_SIGNING_STORE_FILE=$KEY_DIR/matonos-recents.jks
MATON_SIGNING_STORE_PASSWORD=$(<"$KEY_DIR/matonos-recents.pass")
export MATON_SIGNING_STORE_PASSWORD MATON_SIGNING_KEY_ALIAS=matonos-dev
export MATON_SIGNING_KEY_PASSWORD=$MATON_SIGNING_STORE_PASSWORD

"$ADB" connect "$DEVICE" >/dev/null 2>&1 || true
device get-state >/dev/null 2>&1 || fail "device $DEVICE is not connected; start the windowed VM and retry"
cd "$ROOT"
# npm ci recreates node_modules: a Metro left running keeps a stale file map
# (unresolvable react-native internals -> RN surface falls back). Restart it.
stop_metro
npm ci --ignore-scripts
npm run typecheck || echo "WARNING: TypeScript errors above (dev loop continues; fix before committing)" >&2
CI=1 npx expo prebuild --platform android --clean --no-install
./android/gradlew -p android --no-daemon --max-workers 2 :app:assembleDebug
APK=$ROOT/android/app/build/outputs/apk/debug/app-debug.apk
[[ -s $APK ]] || fail "Gradle did not produce $APK"

if curl --silent --fail "http://localhost:$METRO_PORT/status" | grep -q 'packager-status:running'; then
  [[ -s $METRO_PID ]] && is_our_metro "$(<"$METRO_PID")" \
    || fail "port $METRO_PORT belongs to another Metro; stop it and retry"
else
  stop_metro
  nohup "$ROOT/node_modules/.bin/expo" start --dev-client --localhost --port "$METRO_PORT" --clear >"$METRO_LOG" 2>&1 </dev/null &
  echo $! > "$METRO_PID"
  ready=0
  for _ in {1..120}; do
    if curl --silent --fail "http://localhost:$METRO_PORT/status" | grep -q 'packager-status:running'; then ready=1; break; fi
    sleep 0.25
  done
  [[ $ready == 1 ]] || { stop_metro; tail -80 "$METRO_LOG" >&2; fail "Expo Metro did not start"; }
fi

device install -r "$APK"
device reverse "tcp:$METRO_PORT" "tcp:$METRO_PORT"
# adb reverse does not work over adb-over-TCP (QEMU VMs): point the dev
# client at Metro directly. QEMU guests reach the host at 10.0.2.2; set
# MATON_DEV_HOST=<host IP> for real devices over the network.
DEV_HOST=${MATON_DEV_HOST:-10.0.2.2}
prefs=$(mktemp)
printf '<?xml version="1.0" encoding="utf-8" standalone="yes" ?>\n<map>\n    <string name="debug_http_host">%s:%s</string>\n</map>\n' "$DEV_HOST" "$METRO_PORT" > "$prefs"
device push "$prefs" "/data/local/tmp/$PACKAGE.devhost.xml" >/dev/null
rm -f "$prefs"
device shell "run-as $PACKAGE sh -c 'mkdir -p shared_prefs && cat /data/local/tmp/$PACKAGE.devhost.xml > shared_prefs/${PACKAGE}_preferences.xml'" ||
  echo "WARNING: could not set the dev server host; the app may not reach Metro" >&2
device shell am force-stop "$PACKAGE" || true
device shell am start -n "$PACKAGE/.RecentsActivity" >/dev/null
echo "Recents dev client installed on $DEVICE; Metro is on port $METRO_PORT. Edit src/**/*.tsx for Fast Refresh."
