#!/usr/bin/env bash
set -Eeuo pipefail

usage() { echo "Usage: $0 <adb-serial> <evidence-directory>" >&2; exit 2; }
[[ $# == 2 ]] || usage
SERIAL=$1
EVIDENCE=$2
. "$(dirname -- "$(readlink -f -- "$0")")/../../../device/maton/pc_x86_64/tools/local-env.sh"
ADB=${ADB:-$MATON_ROOT/out/host/linux-x86/bin/adb}
PACKAGE=org.matonos.shell
RECENTS_PACKAGE=org.matonos.recents
SHELF_PACKAGE=org.matonos.shelf

[[ -x $ADB ]] || { echo "adb not executable: $ADB" >&2; exit 1; }
mkdir -p "$EVIDENCE"
adb_shell() { "$ADB" -s "$SERIAL" shell "$@"; }

"$ADB" -s "$SERIAL" wait-for-device
for _ in {1..120}; do
  [[ $(adb_shell getprop sys.boot_completed 2>/dev/null | tr -d '\r') == 1 ]] && break
  sleep 1
done
[[ $(adb_shell getprop sys.boot_completed 2>/dev/null | tr -d '\r') == 1 ]] || {
  echo "Android did not finish booting on $SERIAL" >&2
  exit 1
}

adb_shell setprop persist.vendor.maton.sleep_idle_s 0
adb_shell wm dismiss-keyguard
adb_shell am start -W -a android.intent.action.MAIN \
  -c android.intent.category.HOME -f 0x10000000 > "$EVIDENCE/home-start.txt"
sleep 15
adb_shell dumpsys activity activities > "$EVIDENCE/activities.txt"
if ! grep -Fq "$PACKAGE/.HomeActivity" "$EVIDENCE/activities.txt"; then
  echo "MatonOS HomeActivity is not present in the activity dump" >&2
  exit 1
fi

"$ADB" -s "$SERIAL" exec-out screencap -p > "$EVIDENCE/home.png"
[[ -s "$EVIDENCE/home.png" ]] || { echo "Home screenshot is empty" >&2; exit 1; }
file "$EVIDENCE/home.png" | grep -q 'PNG image data' || { echo "Home screenshot is not a PNG" >&2; exit 1; }

# Exercise every Expo-hosting Activity; the 22:31 crash loop happened when an
# RN lifecycle resume reached an Activity that did not extend AppCompatActivity.
adb_shell am start -W -n "$PACKAGE/.DrawerActivity" > "$EVIDENCE/drawer-start.txt"
grep -Fq 'Status: ok' "$EVIDENCE/drawer-start.txt" || {
  echo "DrawerActivity did not start successfully" >&2; exit 1;
}
sleep 3
adb_shell am start -W -n "$RECENTS_PACKAGE/.RecentsActivity" > "$EVIDENCE/recents-start.txt"
grep -Fq 'Status: ok' "$EVIDENCE/recents-start.txt" || {
  echo "RecentsActivity did not start successfully" >&2; exit 1;
}
sleep 3
adb_shell am start -W -a android.intent.action.MAIN \
  -c android.intent.category.HOME -f 0x10000000 > "$EVIDENCE/home-return.txt"
sleep 3
adb_shell dumpsys activity activities > "$EVIDENCE/activities-after-host-resume.txt"
grep -Fq "$PACKAGE/.HomeActivity" "$EVIDENCE/activities-after-host-resume.txt" || {
  echo "HomeActivity did not return after the AppCompat host-resume checks" >&2
  exit 1
}

"$ADB" -s "$SERIAL" logcat -b crash -d -v brief > "$EVIDENCE/crash.log"
if grep -E "$PACKAGE|$RECENTS_PACKAGE|$SHELF_PACKAGE|Current Activity is of incorrect class|AppContext\.onHostResume" "$EVIDENCE/crash.log"; then
  echo "Crash buffer contains a MatonOS app crash or Expo AppCompat lifecycle failure; see $EVIDENCE/crash.log" >&2
  exit 1
fi

echo "Boot smoke passed on $SERIAL. Review $EVIDENCE/home.png; no shell crash was logged."
