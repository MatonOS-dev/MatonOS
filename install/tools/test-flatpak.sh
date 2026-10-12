#!/usr/bin/env bash
# Disposable live-QEMU exercise for the Flatpak bridge target.
set -Eeuo pipefail
ADB=${MATON_ADB:-/mnt/data/aosp/out/host/linux-x86/bin/adb}
SERIAL=${MATON_ADB_SERIAL:-127.0.0.1:5562}
MODE=${1:-}
REF=${2:-}
ANDROID_USER=${MATON_ANDROID_USER:-0}
[[ $ANDROID_USER =~ ^[0-9]+$ ]] || { echo "Invalid Android user" >&2; exit 2; }
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
list_installed() { bridge_call list_installed "{\"userId\":$ANDROID_USER}"; }
wait_for_ref() {
  local wanted=$1 present=$2 result
  for ((attempt=0; attempt<300; attempt++)); do
    result=$(list_installed)
    if python3 -c 'import json,sys; ref,wanted=sys.argv[1],sys.argv[2]=="yes"; ref=ref.split("/",1)[1] if ref.startswith(("app/","runtime/")) else ref; text=json.load(sys.stdin).get("output",""); raise SystemExit(0 if ((ref in text)==wanted) else 1)' \
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
FLATPAK=/apex/com.matonos.flatpak/bin/matonos-flatpak
adb_cmd shell su 1000 "$FLATPAK" flatpak --version
adb_cmd shell su 1000 "$FLATPAK" flatpak --help >/dev/null
if [[ $MODE == --list ]]; then
  bridge_call list_installed "{\"userId\":$ANDROID_USER}"
  bridge_call list_remotes "{\"userId\":$ANDROID_USER}"
  exit 0
fi

python3 - "$REF" <<'PY'
import re, sys
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
PY

if [[ $MODE == --install ]]; then
  echo "Installed before:"
  list_installed
  bridge_call add_flathub "{\"userId\":$ANDROID_USER}"
  operation_id=$(python3 -c 'import uuid; print(uuid.uuid4())')
  payload=$(python3 -c 'import json,sys; print(json.dumps({"ref":sys.argv[1],"operationId":sys.argv[2],"userId":int(sys.argv[3])}))' "$REF" "$operation_id" "$ANDROID_USER")
  result=$(bridge_call install "$payload")
  stub_package=$(python3 -c 'import json,re,sys; r=json.load(sys.stdin); assert r.get("ok") and r.get("accepted"), r; p=r["stubPackage"]; assert re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+",p), p; print(p)' <<<"$result")
  # Stub creation is asynchronous. Resolve its INSTALL activity in this user.
  component=
  for ((attempt=0; attempt<300; attempt++)); do
    resolved=$(adb_cmd shell cmd package resolve-activity --brief --user "$ANDROID_USER" \
      -a org.matonos.linuxhost.INSTALL -p "$stub_package" | tr -d '\r')
    component=$(printf '%s\n' "$resolved" | awk -v package="$stub_package" 'index($0, package "/") == 1 {print; exit}')
    [[ -n $component ]] && break
    sleep 2
  done
  [[ -n $component ]] || { echo "Timed out waiting for stub INSTALL activity: $stub_package" >&2; exit 1; }
  adb_cmd shell am start --user "$ANDROID_USER" -n "$component" \
    -a org.matonos.linuxhost.INSTALL --es org.matonos.linuxhost.OPERATION_ID "$operation_id"
  wait_for_ref "$REF" yes
  installed_uid=$(adb_cmd shell pm list packages -U --user "$ANDROID_USER" "$stub_package" | \
    tr -d '\r' | awk -v package="package:$stub_package" '$1 == package && $2 ~ /^uid:[0-9]+$/ {sub(/^uid:/,"",$2); print $2}')
  [[ $installed_uid =~ ^[0-9]+$ ]] || { echo "Installed stub UID unavailable: $stub_package" >&2; exit 1; }
  echo "Installed UID: $installed_uid (Android user $ANDROID_USER)"
  echo "Installed after:"
  list_installed
  exit 0
fi

app_id=${REF#*/}
app_id=${app_id%%/*}
payload=$(python3 -c 'import json,sys; print(json.dumps({"ref":sys.argv[1],"userId":int(sys.argv[2]),"deleteData":True}))' "$REF" "$ANDROID_USER")
bridge_call uninstall "$payload"
wait_for_ref "$REF" no
# App data lives in the stub's per-app home, not a global Flatpak tree.
data_glob="/data/misc_ce/$ANDROID_USER/matonos-linux/home/*/.var/app/$app_id"
if adb_cmd shell sh -c "ls -d $data_glob >/dev/null 2>&1"; then
  echo "Flatpak app data still exists: $data_glob" >&2
  exit 1
fi
echo "Installed after uninstall:"
list_installed
echo "App data removed: $data_glob"
