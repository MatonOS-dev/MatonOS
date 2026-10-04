#!/usr/bin/env bash
# Disposable live-QEMU exercise for the Flatpak bridge target.
set -Eeuo pipefail
ADB=${MATON_ADB:-/mnt/data/aosp/out/host/linux-x86/bin/adb}
SERIAL=${MATON_ADB_SERIAL:-127.0.0.1:5562}
MODE=${1:-}
REF=${2:-}
[[ -x $ADB ]] || { echo "adb not found: $ADB" >&2; exit 2; }
[[ $MODE == --list || ($MODE == --install && -n $REF) || ($MODE == --uninstall && -n $REF) ]] || {
  echo "Usage: $0 --list | --install <app|runtime/ID/ARCH/BRANCH> | --uninstall <ref>" >&2; exit 2;
}
adb_cmd() { "$ADB" -s "$SERIAL" "$@" </dev/null; }
bridge_call() {
  local command=$1 json=$2 encoded output
  encoded=$(printf '%s' "$json" | base64 | tr -d '\n')
  output=$(adb_cmd shell content call --uri content://org.matonos.systembridge.shell \
    --method flatpak_test_call --arg "$command" --extra "payload_b64:s:$encoded")
  python3 -c 'import sys; s=sys.stdin.read(); k="result="; i=s.find(k); assert i>=0, s; value=s[i+len(k):].rsplit("}]",1)[0]; print(value)' <<<"$output"
}
list_installed() { bridge_call list_installed '{}'; }
wait_for_ref() {
  local wanted=$1 present=$2 result
  for ((attempt=0; attempt<300; attempt++)); do
    result=$(list_installed)
    if python3 -c 'import json,sys; ref,wanted=sys.argv[1],sys.argv[2]=="yes"; text=json.load(sys.stdin).get("output",""); raise SystemExit(0 if ((ref in text)==wanted) else 1)' \
        "$wanted" "$present" <<<"$result"
    then return 0; fi
    sleep 2
  done
  echo "Timed out waiting for ref state: $present $wanted" >&2
  return 1
}
"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
adb_cmd wait-for-device
adb_cmd root >/dev/null
adb_cmd wait-for-device
adb_cmd shell setprop persist.vendor.maton.flatpak_test 1
# Verify the expected spike executable before network or repository changes.
# The real path runs the launcher as linuxd's system UID (1000). Running it as
# root would leave root-owned files under /data/matonos/linux (machine-id) and
# poison later real launches, so switch to that UID first.
adb_cmd shell su 1000 /apex/com.matonos.flatpak/bin/flatpak --version
adb_cmd shell su 1000 /apex/com.matonos.flatpak/bin/flatpak --help >/dev/null
if [[ $MODE == --list ]]; then
  bridge_call list_installed '{}'
  bridge_call list_remotes '{}'
  exit 0
fi

payload=$(python3 - "$REF" <<'PY'
import json, re, sys
ref=sys.argv[1]
parts=ref.split("/")
ok=(len(parts)==4 and parts[0] in ("app","runtime") and
    len(parts[1].split(".")) >= 2 and
    all(re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*",p) for p in parts[1].split(".")[:-1]) and
    re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*",parts[1].split(".")[-1]) and
    not parts[1].split(".")[-1].endswith("-") and
    re.fullmatch(r"[A-Za-z0-9_.-]+",parts[2]) and
    re.fullmatch(r"[A-Za-z0-9_.-]+",parts[3]))
if not ok: raise SystemExit("invalid Flatpak ref")
print(json.dumps({"ref":ref},separators=(",",":")))
PY
)

if [[ $MODE == --install ]]; then
  echo "Installed before:"
  list_installed
  bridge_call add_flathub '{}'
  bridge_call install "$payload"
  wait_for_ref "$REF" yes
  echo "Installed after:"
  list_installed
  exit 0
fi

app_id=${REF#*/}
app_id=${app_id%%/*}
bridge_call uninstall "$payload"
wait_for_ref "$REF" no
data_dir="/data/matonos/linux/flatpak-data/.var/app/$app_id"
if adb_cmd shell test -e "$data_dir"; then
  echo "Flatpak app data still exists: $data_dir" >&2
  exit 1
fi
echo "Installed after uninstall:"
list_installed
echo "App data removed: $data_dir"
