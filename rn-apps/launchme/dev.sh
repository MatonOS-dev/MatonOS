#!/usr/bin/env bash
set -Eeuo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
AOSP=$(cd "$ROOT/../.." && pwd)
ADB=${ADB:-$AOSP/out/host/linux-x86/bin/adb}
DEVICE=${MATON_ADB_DEVICE:-127.0.0.1:5555}
PACKAGE=org.matonos.shell
KEY_DIR=${MATON_KEY_DIR:-$HOME/.config/matonos-keys}
SDK=${MATON_ANDROID_SDK:-$HOME/Documents/matonos-android-sdk}
JAVA_HOME=${JAVA_HOME:-$AOSP/prebuilts/jdk/jdk21/linux-x86}
METRO_PID=$ROOT/.metro.pid
METRO_LOG=$ROOT/.metro.log
EXPECTED_CERT=07522fac806c769b477641508b7799c35d8cccb8687bddca34b7dea4f6b5827d

fail() { echo "dev.sh: $*" >&2; exit 1; }
[[ -x $ADB ]] || fail "adb not found at $ADB (set ADB to override)"
adb_device() { "$ADB" -s "$DEVICE" "$@"; }

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
  adb_device get-state >/dev/null 2>&1 || fail "device $DEVICE is not connected"
  result=$(adb_device shell pm uninstall-system-updates "$PACKAGE" 2>&1) || true
  [[ $result == *Success* ]] || fail "could not restore the image shell: $result"
  echo "$result"
  adb_device shell am force-stop "$PACKAGE" || true
  adb_device reverse --remove tcp:8081 >/dev/null 2>&1 || true
  stop_metro
  adb_device shell cmd package set-home-activity --user 0 "$PACKAGE/.HomeActivity" >/dev/null
  adb_device shell am start -a android.intent.action.MAIN -c android.intent.category.HOME -f 0x10000000 >/dev/null
  echo "Restored the image's preinstalled Hermes-bytecode shell."
  exit 0
elif [[ $# -ne 0 ]]; then
  fail "usage: ./dev.sh [--reset]"
fi

[[ -f $KEY_DIR/matonos-shell.jks && -s $KEY_DIR/matonos-shell.pass ]] || fail "shell signing key missing under $KEY_DIR"
[[ -x $SDK/build-tools/36.0.0/apksigner ]] || fail "Android build-tools 36.0.0 are missing under $SDK"
export ANDROID_HOME=$SDK ANDROID_SDK_ROOT=$SDK
if [[ -x $JAVA_HOME/bin/java ]]; then export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"; fi
export MATON_SIGNING_STORE_FILE=$KEY_DIR/matonos-shell.jks
MATON_SIGNING_STORE_PASSWORD=$(<"$KEY_DIR/matonos-shell.pass")
export MATON_SIGNING_STORE_PASSWORD MATON_SIGNING_KEY_ALIAS=matonos-dev
export MATON_SIGNING_KEY_PASSWORD=$MATON_SIGNING_STORE_PASSWORD

"$ADB" connect "$DEVICE" >/dev/null 2>&1 || true
adb_device get-state >/dev/null 2>&1 || fail "device $DEVICE is not connected; start the windowed VM and retry"

cd "$ROOT"
npm ci --ignore-scripts
npm run typecheck
CI=1 npx expo prebuild --platform android --clean --no-install
./android/gradlew -p android --no-daemon --max-workers 2 :app:assembleDebug
APK=$ROOT/android/app/build/outputs/apk/debug/app-debug.apk
[[ -s $APK ]] || fail "Gradle did not produce $APK"
CERT=$("$SDK"/build-tools/36.0.0/apksigner verify --print-certs "$APK" | awk -F': ' '/Signer #1 certificate SHA-256 digest:/ {print $2; exit}')
[[ ${CERT,,} == "$EXPECTED_CERT" ]] || fail "debug APK signer mismatch: ${CERT:-no certificate}"

  if curl --silent --fail http://localhost:8081/status | grep -q 'packager-status:running'; then
  [[ -s $METRO_PID ]] && is_our_metro "$(<"$METRO_PID")" || fail "port 8081 belongs to another Metro; stop it and retry"
else
  stop_metro
  nohup "$ROOT/node_modules/.bin/expo" start --dev-client --localhost --port 8081 >"$METRO_LOG" 2>&1 </dev/null &
  echo $! > "$METRO_PID"
  ready=0
  for _ in {1..120}; do
    if curl --silent --fail http://localhost:8081/status | grep -q 'packager-status:running'; then ready=1; break; fi
    sleep 0.25
  done
  [[ $ready == 1 ]] || { stop_metro; tail -80 "$METRO_LOG" >&2; fail "Expo Metro did not start"; }
fi

adb_device install -r "$APK"
adb_device reverse tcp:8081 tcp:8081
adb_device shell cmd package set-home-activity --user 0 "$PACKAGE/.HomeActivity" >/dev/null
adb_device shell am force-stop "$PACKAGE" || true
adb_device shell am start -a android.intent.action.MAIN -c android.intent.category.HOME -f 0x10000000 >/dev/null
echo "Expo dev client installed on $DEVICE; edit src/**/*.tsx for Fast Refresh."
echo "Dev menu: adb -s $DEVICE shell input keyevent 82"
echo "Metro log: $METRO_LOG"
