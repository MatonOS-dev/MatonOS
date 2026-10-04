#!/usr/bin/env bash
# Disposable-QEMU installer exercise. Requests pass through the system bridge
# debug provider to the same install channel and OperationRequestV1 executor
# used by MatonOS Settings. Never point this script at a real disk.
set -Eeuo pipefail

DEVICE_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ADB=${MATON_ADB:-/mnt/data/aosp/out/host/linux-x86/bin/adb}
SERIAL=${MATON_ADB_SERIAL:-127.0.0.1:5556}
MODE=${1:-}
TARGET_ID=${2:-}
FIRST_OPERATION=1
[[ -x $ADB ]] || { echo "adb not found: $ADB" >&2; exit 2; }
if [[ $MODE == --install-from && $# -eq 3 ]]; then
  FIRST_OPERATION=$3
  [[ $FIRST_OPERATION =~ ^[1-9][0-9]*$ ]] || { echo "Invalid first operation number." >&2; exit 2; }
elif [[ $MODE == --install && $# -eq 2 ]]; then
  :
elif [[ $MODE == --list && $# -eq 1 ]]; then
  :
else
  echo "Usage: $0 --list | --install <target-major:minor> | --install-from <target-major:minor> <first-operation-number>" >&2; exit 2;
fi
adb_cmd() { "$ADB" -s "$SERIAL" "$@" </dev/null; }
bridge_call() {
  local command=$1 json=$2 encoded output
  encoded=$(printf '%s' "$json" | base64 | tr -d '\n')
  output=$(adb_cmd shell content call --uri content://org.matonos.systembridge.shell \
    --method installer_test_call --arg "$command" --extra "payload_b64:s:$encoded")
  python3 -c 'import sys; s=sys.stdin.read(); k="result="; i=s.find(k); assert i>=0, s; value=s[i+len(k):]; value=value.rsplit("}]",1)[0]; print(value)' <<<"$output"
}

"$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
adb_cmd wait-for-device
adb_cmd root >/dev/null
adb_cmd wait-for-device
adb_cmd shell setprop persist.vendor.maton.installer_test 1

service_status=
for ((attempt=0; attempt<30; attempt++)); do
  if service_status=$(bridge_call get_status '{}' 2>/dev/null); then break; fi
  sleep 1
done
[[ -n $service_status ]] || { echo "installer bridge channel did not become ready" >&2; exit 1; }

if [[ $MODE == --list ]]; then
  echo "$service_status"
  bridge_call list_drives '{}'
  exit 0
fi


work=$(mktemp -d "${TMPDIR:-/tmp}/maton-install.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$TARGET_ID" "$work/operations.jsonl" <<'PY'
import json, math, secrets, sys
target, output = sys.argv[1:]
MiB, GiB = 1024**2, 1024**3
guid = lambda: str(__import__('uuid').uuid4())
cursor = MiB
parts=[]
def add(name, typ, size):
    global cursor
    p={"name":name,"typeGuid":typ,"partGuid":guid(),"startBytes":cursor,"sizeBytes":size}
    parts.append(p); cursor += size
    return p
linux="0fc63daf-8483-4772-8e79-3d69d8477de4"
esp=add("esp","c12a7328-f81f-11d2-ba4b-00a0c93ec93b",512*MiB)
boot=add("boot","bc13c2ff-59e6-4262-a352-b275fd6f7172",GiB)
add("misc",linux,4*MiB)
metadata=add("metadata",linux,64*MiB)
superp=add("super",linux,6*GiB)
addons_a=add("addons_a",linux,512*MiB)
addons_b=add("addons_b",linux,512*MiB)
user_start=math.ceil(cursor/MiB)*MiB
disk_bytes=64*GiB
userdata=add("userdata",linux,(disk_bytes-MiB)//MiB*MiB-user_start)
ops=[{"kind":"write_gpt","declaredDiskBytes":disk_bytes,"diskGuid":guid(),"partitions":parts}]
for part,fs,label in [(esp,"vfat","MATON_ESP"),(boot,"vfat","MATON_BOOT"),
                      (metadata,"ext4","metadata"),
                      (addons_a,"ext4","addons_a"),(addons_b,"ext4","addons_b"),
                      (userdata,"ext4","userdata")]:
    ops.append({"kind":"format","target":{"partGuid":part["partGuid"]},"filesystem":fs,"label":label})
slots=[]
for suffix in "ab":
    members=[{"name":f"{name}_{suffix}","sizeBytes":size*MiB if suffix=="a" else 0} for name,size in
             [("system",2048),("system_ext",512),("product",1536),("vendor",960),("odm",64)]]
    slots.append({"name":f"pc_dynamic_partitions_{suffix}","maximumSizeBytes":5*GiB,"partitions":members})
ops.append({"kind":"create_lp_metadata","superPartGuid":superp["partGuid"],"metadataSizeBytes":64*1024,"metadataSlots":3,"groups":slots})
ops.append({"kind":"write_files","target":{"partGuid":esp["partGuid"]},"relativeDirectory":"EFI","files":[
    {"relativePath":"BOOT/BOOTX64.EFI","livePayloadPath":"live/esp/EFI/BOOT/BOOTX64.EFI"},
    {"relativePath":"BOOT/grubx64.efi","livePayloadPath":"live/esp/EFI/BOOT/grubx64.efi"},
    {"relativePath":"systemd/systemd-bootx64.efi","livePayloadPath":"live/esp/EFI/systemd/systemd-bootx64.efi"},
    {"relativePath":"Linux/matonos-a.efi","livePayloadPath":"live/esp/EFI/Linux/matonos-installed-a.efi"},
    {"relativePath":"Linux/matonos-b.efi","livePayloadPath":"live/esp/EFI/Linux/matonos-installed-b.efi"}]})
ops.append({"kind":"write_files","target":{"partGuid":esp["partGuid"]},"relativeDirectory":"loader","files":[
    {"relativePath":"loader.conf","inlineContents":"default A+3-0.conf\ntimeout 5\neditor no\n"}]})
for name in ("system","system_ext","product","vendor","odm"):
    ops.append({"kind":"copy_partition","source":{"kind":"live_image","partitionName":name+"_a"},"target":{"logicalName":name+"_a"}})
ops.append({"kind":"write_files","target":{"partGuid":esp["partGuid"]},"relativeDirectory":"loader/entries","files":[
    {"relativePath":"A+3-0.conf","inlineContents":"title MatonOS (slot A)\nefi /EFI/Linux/matonos-a.efi\noptions androidboot.hardware=pc_x86_64 androidboot.fstab_suffix=pc_x86_64 androidboot.slot_suffix=_a androidboot.boot_part_uuid=@BOOT_PART_UUID@ androidboot.boot_devices=@BOOT_DEVICE@ androidboot.matonos.live=0 androidboot.selinux=enforcing androidboot.verifiedbootstate=orange firmware_class.path=/vendor/firmware console=ttyS0,115200 console=tty0 quiet loglevel=3 vt.global_cursor_default=0 fbcon=vc:2-6\n"},
    {"relativePath":"B.DIS","inlineContents":"title MatonOS (slot B)\nefi /EFI/Linux/matonos-b.efi\noptions androidboot.hardware=pc_x86_64 androidboot.fstab_suffix=pc_x86_64 androidboot.slot_suffix=_b androidboot.boot_part_uuid=@BOOT_PART_UUID@ androidboot.boot_devices=@BOOT_DEVICE@ androidboot.matonos.live=0 androidboot.selinux=enforcing androidboot.verifiedbootstate=orange firmware_class.path=/vendor/firmware console=ttyS0,115200 console=tty0 quiet loglevel=3 vt.global_cursor_default=0 fbcon=vc:2-6\n"}]})
with open(output,"w") as f:
    for op in ops:
        f.write(json.dumps({"apiVersion":1,"targetDiskId":target,"operation":op},separators=(",",":"))+"\n")
PY

status=$(bridge_call get_status '{}')
echo "service: $status"
drives=$(bridge_call list_drives '{}')
echo "$drives" | python3 -m json.tool
python3 - "$drives" "$TARGET_ID" <<'PY'
import json,sys
data=json.loads(sys.argv[1]); target=next((d for d in data.get("drives",[]) if d.get("id")==sys.argv[2]),None)
if target is None: raise SystemExit("selected target is not present in the fresh bridge enumeration")
if not target.get("safe"): raise SystemExit("selected target is unsafe: "+str(target.get("reason")))
if "MATONTGT" not in target.get("serial",""): raise SystemExit("refusing to erase a disk not identified as serial MATONTGT")
if not target.get("identity"): raise SystemExit("target has no stable physical identity")
print("selected disposable target:",target["model"],target["sizeBytes"],"bytes",target["id"])
PY
target_identity=$(python3 - "$drives" "$TARGET_ID" <<'PY'
import json,sys
print(next(d["identity"] for d in json.loads(sys.argv[1])["drives"] if d["id"]==sys.argv[2]))
PY
)
python3 - "$work/operations.jsonl" "$TARGET_ID" "$target_identity" "$drives" "$FIRST_OPERATION" <<'PY'
import json,sys
path,target,identity,drives_json,first_operation=sys.argv[1:]
sys_path=identity.split(";",1)[0][4:]
if not sys_path.startswith("/sys/devices/"): raise SystemExit("target lacks a sysfs boot path")
rel=sys_path[len("/sys"):]
import re
m=re.match(r"^/devices/(pci[^/]+/[^/]+)",rel)
if m: boot_device=m.group(1)
else:
    m=re.match(r"^/devices/platform/(.+)/(?:ata|host|nvme|mmc_host)[^/]*/",rel)
    if not m: raise SystemExit("target firmware device path is unsupported")
    boot_device=m.group(1)
with open(path) as f: requests=[json.loads(line) for line in f]
if int(first_operation)>1:
    drives=json.loads(drives_json)
    disk=next(d for d in drives.get("drives",[]) if d.get("id")==target)
    esp_guid=next(p["partGuid"] for p in disk.get("partitions",[]) if p.get("name")=="esp")
else:
    esp_guid=next(part["partGuid"] for request in requests
                  for part in request["operation"].get("partitions",[])
                  if part["name"]=="esp")
with open(path,"w") as f:
    for request in requests:
        request["targetDiskId"]=target
        request["targetDiskIdentity"]=identity
        operation=request["operation"]
        if operation.get("kind")=="write_files":
            for file in operation.get("files",[]):
                file["inlineContents"]=(file.get("inlineContents","")
                    .replace("@BOOT_DEVICE@",boot_device)
                    .replace("@BOOT_PART_UUID@",esp_guid))
        f.write(json.dumps(request,separators=(",",":"))+"\n")
PY
target_path=$(python3 - "$drives" "$TARGET_ID" <<'PY'
import json,sys
print(next(d["path"] for d in json.loads(sys.argv[1])["drives"] if d["id"]==sys.argv[2]))
PY
)

index=0
while IFS= read -r request; do
  index=$((index+1))
  (( index >= FIRST_OPERATION )) || continue
  kind=$(python3 -c 'import json,sys;print(json.loads(sys.argv[1])["operation"]["kind"])' "$request")
  echo "operation $index ($kind)"
  response=$(bridge_call execute_operation "$request")
  echo "$response"
  if ! python3 -c 'import json,sys; raise SystemExit(0 if json.loads(sys.argv[1]).get("ok") else 1)' "$response"; then
    echo "Stopped at operation $index ($kind); the service rejected or failed this primitive." >&2
    exit 1
  fi
  if [[ $kind == write_gpt ]]; then
    echo "Verifying target GPT with the shipped sgdisk: $target_path"
    table=$(adb_cmd shell /system/bin/sgdisk --print "$target_path")
    echo "$table"
      for name in esp boot misc metadata super addons_a addons_b userdata; do
      grep -Eq "[[:space:]]${name}[[:space:]]*$" <<<"$table" || {
        echo "GPT verification failed: missing partition $name" >&2; exit 1;
      }
    done
  fi
done < "$work/operations.jsonl"
echo "All OperationRequestV1 calls succeeded. Shut down the live VM and boot the target disk alone for verification."
