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
if ! grep -E 'org.matonos.permission.SYSTEM_BRIDGE: granted=true|org.matonos.permission.SYSTEM_BRIDGE.*granted=true' "$EVIDENCE/systembridge-package.txt" >/dev/null; then
  echo "System Bridge lacks its provider-service bind permission" >&2
  exit 1
fi
adb_shell logcat -b all -d -s MatonSystemBridge:I > "$EVIDENCE/bridge-startup.log"
if ! grep -Fq 'Enabled image-bundled Shelf as the certificate-pinned default navigation provider' "$EVIDENCE/bridge-startup.log"; then
  echo "Fresh boot did not record the certificate-pinned built-in Shelf default" >&2
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
capture settings-shelf
adb_shell dumpsys input > "$EVIDENCE/settings-input.txt"
adb_shell logcat -b all -d -s MatonSystemBridge:I > "$EVIDENCE/nav-input-before.log"
display_size=$(adb_shell wm size | awk '/Physical size:/ {size=$3} /Override size:/ {size=$3} END {print size}')
density_dpi=$(adb_shell wm density | awk '/Physical density:/ {dpi=$3} /Override density:/ {dpi=$3} END {print dpi}')
[[ $display_size =~ ^([0-9]+)x([0-9]+)$ && $density_dpi =~ ^[0-9]+$ ]] || {
  echo "Could not read display geometry for Shelf input test" >&2; exit 1;
}
IFS=x read -r display_width display_height <<< "$display_size"
dp_to_px() { echo $(( ($1 * density_dpi + 80) / 160 )); }
# On Settings the bar is compact. Tap the handle to expand it, then tap Back;
# the bridge audit proves the embedded view received the touch and invoked nav.back.
adb_shell input tap "$((display_width / 2))" "$((display_height - $(dp_to_px 14)))"
sleep 1
adb_shell input tap "$(dp_to_px 24)" "$((display_height - $(dp_to_px 28)))"
sleep 2
adb_shell logcat -b all -d -s MatonSystemBridge:I > "$EVIDENCE/nav-input.log"
touch_before=$(grep -F -c 'Authorized provider action=navigateBack target=nav.back' "$EVIDENCE/nav-input-before.log" || true)
touch_after=$(grep -F -c 'Authorized provider action=navigateBack target=nav.back' "$EVIDENCE/nav-input.log" || true)
if (( touch_after <= touch_before )); then
  echo "ADB input tap did not reach Shelf Back through the embedded host; see $EVIDENCE/settings-input.txt and nav-input.log" >&2
  exit 1
fi
if [[ -n ${MATON_QMP_SOCKET:-} ]]; then
  adb_shell am start -W -a android.intent.action.MAIN -c android.intent.category.HOME -f 0x10000000 >/dev/null
  sleep 5
  adb_shell logcat -b all -d -s MatonSystemBridge:I > "$EVIDENCE/nav-mouse-before.log"
  python3 - "$MATON_QMP_SOCKET" "$display_width" "$display_height" "$(($display_width - $(dp_to_px 24)))" "$(($display_height - $(dp_to_px 28)))" <<'PY'
import json
import socket
import sys

path, width, height, x, y = sys.argv[1], *(int(value) for value in sys.argv[2:])
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(path)
reader = sock.makefile("r", encoding="utf-8")
reader.readline()  # QMP greeting

def execute(command, arguments=None):
    request = {"execute": command}
    if arguments is not None:
        request["arguments"] = arguments
    sock.sendall((json.dumps(request) + "\r\n").encode())
    while True:
        response = json.loads(reader.readline())
        if "error" in response:
            raise RuntimeError(response["error"])
        if "return" in response:
            return response["return"]

execute("qmp_capabilities")
events = [
    {"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / max(1, width - 1))}},
    {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / max(1, height - 1))}},
    {"type": "btn", "data": {"button": "left", "down": True}},
    {"type": "btn", "data": {"button": "left", "down": False}},
]
for event in events:
    execute("input-send-event", {"events": [event]})
sock.close()
PY
  sleep 3
  adb_shell logcat -b all -d -s MatonSystemBridge:I > "$EVIDENCE/nav-mouse.log"
  mouse_before=$(grep -F -c 'Authorized provider action=navigateRecents target=nav.recents' "$EVIDENCE/nav-mouse-before.log" || true)
  mouse_after=$(grep -F -c 'Authorized provider action=navigateRecents target=nav.recents' "$EVIDENCE/nav-mouse.log" || true)
  if (( mouse_after <= mouse_before )); then
    echo "QEMU mouse click did not reach Shelf Recents through the embedded host; see nav-mouse.log" >&2
    exit 1
  fi
fi
adb_shell logcat -b crash -d -v brief > "$EVIDENCE/crash.log"
if grep -E "$SHELL_PACKAGE|$SHELF_PACKAGE|$RECENTS_PACKAGE|Current Activity is of incorrect class|AppContext\.onHostResume" "$EVIDENCE/crash.log"; then
  echo "Crash buffer contains a MatonOS app crash or Expo AppCompat lifecycle failure; see $EVIDENCE/crash.log" >&2
  exit 1
fi
compact_height_px=$(( (28 * density_dpi + 80) / 160 ))
python3 - "$EVIDENCE/settings-windows.txt" "$EVIDENCE/settings-shelf-window.txt" "$compact_height_px" <<'PY'
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
expected_height = int(sys.argv[3])
requested = re.search(r"^\s*Requested w=\d+ h=(\d+)\s*$", block, re.MULTILINE)
inset = re.search(r"type=navigationBars, source=FRAME, flags=\[\], insetsSize=Insets\{left=0, top=0, right=0, bottom=(\d+)\}", block)
frame = re.search(r"^\s*Frames:.*?frame=\[\d+,(-?\d+)\]\[\d+,(-?\d+)\]", block, re.MULTILINE)
if requested is None or inset is None or frame is None:
    raise SystemExit("Could not inspect compact Shelf host height and navigation inset")
requested_height = int(requested.group(1))
inset_height = int(inset.group(1))
frame_height = int(frame.group(2)) - int(frame.group(1))
if (requested_height, inset_height, frame_height) != (expected_height,) * 3:
    raise SystemExit(
        f"Compact Shelf host height mismatch: expected {expected_height}px, "
        f"window={requested_height}px inset={inset_height}px frame={frame_height}px"
    )
PY

adb_shell dumpsys meminfo "$SHELL_PACKAGE" > "$EVIDENCE/shell-meminfo.txt"
adb_shell dumpsys meminfo "$SHELF_PACKAGE" > "$EVIDENCE/shelf-meminfo.txt"
adb_shell dumpsys meminfo "$RECENTS_PACKAGE" > "$EVIDENCE/recents-meminfo.txt"

for screen in home drawer recents settings-shelf; do
  [[ -s "$EVIDENCE/$screen.png" ]] || { echo "Missing screenshot $screen.png" >&2; exit 1; }
  file "$EVIDENCE/$screen.png" | grep -q 'PNG image data' || { echo "$screen screenshot is not a PNG" >&2; exit 1; }
done
echo "Split app smoke passed on $SERIAL. Review screenshots, Settings window dump, and individual process PSS in $EVIDENCE."
