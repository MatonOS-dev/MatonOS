#!/usr/bin/env bash
set -Eeuo pipefail

usage() { echo "Usage: $0 <adb-serial> <evidence-directory>" >&2; exit 2; }
[[ $# == 2 ]] || usage
SERIAL=$1
EVIDENCE=$2
. "$(dirname -- "$(readlink -f -- "$0")")/../../../device/maton/pc_x86_64/tools/local-env.sh"
ADB=${ADB:-$MATON_ROOT/out/host/linux-x86/bin/adb}
SHELL_PACKAGE=org.matonos.shell
SHELF_PACKAGE=org.matonos.shelf
RECENTS_PACKAGE=org.matonos.recents

[[ -x $ADB ]] || { echo "adb not executable: $ADB" >&2; exit 1; }
mkdir -p "$EVIDENCE"
adb_shell() { "$ADB" -s "$SERIAL" shell "$@"; }
capture() {
  adb_shell svc power stayon true
  adb_shell input keyevent KEYCODE_WAKEUP
  adb_shell wm dismiss-keyguard
  sleep 1
  "$ADB" -s "$SERIAL" exec-out screencap -p > "$EVIDENCE/$1.png"
}

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
adb_shell svc power stayon true
adb_shell wm dismiss-keyguard
adb_shell input keyevent KEYCODE_WAKEUP
sleep 2

adb_shell dumpsys package org.matonos.systembridge > "$EVIDENCE/systembridge-package.txt"
if ! grep -E 'android.permission.QUERY_ALL_PACKAGES: granted=true|android.permission.QUERY_ALL_PACKAGES.*granted=true' "$EVIDENCE/systembridge-package.txt" >/dev/null; then
  echo "System Bridge does not hold QUERY_ALL_PACKAGES; provider certificate lookups may fail" >&2
  exit 1
fi
adb_shell dumpsys activity services org.matonos.shelf > "$EVIDENCE/shelf-service.txt"

adb_shell am start -W -a android.intent.action.MAIN -c android.intent.category.HOME -f 0x10000000 > "$EVIDENCE/home-start.txt"
sleep 12
adb_shell dumpsys activity activities > "$EVIDENCE/home-activities.txt"
grep -Fq "$SHELL_PACKAGE/.HomeActivity" "$EVIDENCE/home-activities.txt" || {
  echo "MatonOS HomeActivity is not present" >&2; exit 1;
}
capture home

adb_shell am start -W -n "$SHELL_PACKAGE/.DrawerActivity" > "$EVIDENCE/drawer-start.txt"
grep -Fq 'Status: ok' "$EVIDENCE/drawer-start.txt" || {
  echo "DrawerActivity did not start successfully" >&2; exit 1;
}
sleep 4
capture drawer
adb_shell am start -W -n "$RECENTS_PACKAGE/.RecentsActivity" > "$EVIDENCE/recents-start.txt"
grep -Fq 'Status: ok' "$EVIDENCE/recents-start.txt" || {
  echo "RecentsActivity did not start successfully" >&2; exit 1;
}
sleep 4
capture recents

adb_shell am start -W -a android.settings.SETTINGS > "$EVIDENCE/settings-start.txt"
sleep 8
adb_shell dumpsys window windows > "$EVIDENCE/settings-windows.txt"
python3 - "$EVIDENCE/settings-windows.txt" "$EVIDENCE/settings-shelf-window.txt" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(errors="replace")
match = re.search(
    r"^\s*Window #\d+ Window\{[^\n]*MatonOS navigation provider host[^\n]*\}:(.*?)(?=^\s*Window #\d+ Window\{|\Z)",
    source,
    re.MULTILINE | re.DOTALL,
)
if match is None:
    raise SystemExit("Shelf overlay window is absent over Settings")
block = match.group(0)
pathlib.Path(sys.argv[2]).write_text(block)
if not re.search(r"^\s*isVisible=true\s*$", block, re.MULTILINE):
    raise SystemExit("Shelf window exists over Settings but isVisible is not true")
if re.search(r"mIsForceHiddenNonSystemOverlayWindow=true", block):
    raise SystemExit("Settings is force-hiding the Shelf as a non-system overlay")
PY
capture settings-shelf

adb_shell dumpsys meminfo "$SHELL_PACKAGE" > "$EVIDENCE/shell-meminfo.txt"
adb_shell dumpsys meminfo "$SHELF_PACKAGE" > "$EVIDENCE/shelf-meminfo.txt"
adb_shell dumpsys meminfo "$RECENTS_PACKAGE" > "$EVIDENCE/recents-meminfo.txt"
adb_shell logcat -b crash -d -v brief > "$EVIDENCE/crash.log"
if grep -E "$SHELL_PACKAGE|$SHELF_PACKAGE|$RECENTS_PACKAGE|Current Activity is of incorrect class|AppContext\.onHostResume" "$EVIDENCE/crash.log"; then
  echo "Crash buffer contains a MatonOS app crash or Expo AppCompat lifecycle failure; see $EVIDENCE/crash.log" >&2
  exit 1
fi

for screen in home drawer recents settings-shelf; do
  [[ -s "$EVIDENCE/$screen.png" ]] || { echo "Missing screenshot $screen.png" >&2; exit 1; }
  file "$EVIDENCE/$screen.png" | grep -q 'PNG image data' || { echo "$screen screenshot is not a PNG" >&2; exit 1; }
done
echo "Split app smoke passed on $SERIAL. Review screenshots, Settings window dump, and individual process PSS in $EVIDENCE."
