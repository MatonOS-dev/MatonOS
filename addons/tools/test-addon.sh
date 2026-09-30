#!/usr/bin/env bash
set -Eeuo pipefail
die() { echo "ERROR: $*" >&2; exit 1; }
[[ $# -ge 1 ]] || die "usage: $0 <adb-port> [kernel-source]"
PORT=$1
[[ $PORT =~ ^[0-9]+$ && $PORT -ge 5556 ]] || die "use an agent VM port >= 5556; port 5555 belongs to the user"
HERE=$(dirname "$(dirname "$(readlink -f "$0")")")
DEVICE_DIR=$(dirname "$HERE")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
LINUX=${2:-$HOME/Documents/linux}
KOUT=$AOSP/out/pc-kernel
ADB=${ADB:-$AOSP/out/host/linux-x86/bin/adb}
REPO_KEY=${MATON_ADDON_TEST_KEY:-$HOME/.config/matonos/addons/repo.pem}
SERIAL=127.0.0.1:$PORT
[[ -x $ADB ]] || die "adb is missing at $ADB"
[[ -f $LINUX/Makefile && -s $KOUT/Module.symvers ]] || die "prepared kernel build tree is missing"
[[ -s $REPO_KEY ]] || die "MATON_ADDON_TEST_KEY (matching the image repo.pub) is required"
command -v modinfo >/dev/null || die "host modinfo is required"
TMP=$(mktemp -d "$AOSP/out/pc-logs/addons-test.XXXXXX")
if [[ ${KEEP_ADDON_TEST_TMP:-0} == 1 ]]; then
  echo "Preserving add-on test files in $TMP"
else
  trap 'rm -rf "$TMP"' EXIT
fi

"$ADB" -s "$SERIAL" wait-for-device
"$ADB" -s "$SERIAL" root >/dev/null
"$ADB" -s "$SERIAL" wait-for-device
DEVICE_RELEASE=$($ADB -s "$SERIAL" shell uname -r | tr -d '\r')
RELEASE=$(cat "$KOUT/include/config/kernel.release")
[[ $DEVICE_RELEASE == "$RELEASE" ]] || die "prepared kernel $RELEASE differs from booted image $DEVICE_RELEASE"
MATONOS_VERSION=${MATONOS_VERSION:-$($ADB -s "$SERIAL" shell getprop ro.matonos.version | tr -d '\r')}
MATONOS_VERSION=${MATONOS_VERSION:-dev-$RELEASE}
[[ $MATONOS_VERSION =~ ^[A-Za-z0-9._+-]{1,64}$ ]] || die "unsafe MatonOS release value"
RELEASE_REPO=${MATON_ADDON_TEST_REPO_ROOT:-$AOSP/out/pc-logs/addons-repo}/$MATONOS_VERSION/addons
mkdir -p "$RELEASE_REPO"
echo "Mirroring test payload under $RELEASE_REPO (server path: /matonos/updates/$MATONOS_VERSION/addons/)"
PACKAGE_PUB="$TMP/package.zip.pub"

cp "$HERE/tests/matonos_addon_test.c" "$HERE/tests/Makefile" "$TMP/"
CLANG_DIR=$(find "$AOSP/prebuilts/clang/host/linux-x86" -maxdepth 1 -name 'clang-r*' -type d | sort -V | tail -1)
[[ -x $CLANG_DIR/bin/clang ]] || die "AOSP clang toolchain is missing"
PATH=$CLANG_DIR/bin:$PATH make -s -C "$LINUX" O="$KOUT" M="$TMP" ARCH=x86_64 LLVM=1 modules -j4
KO=$TMP/matonos_addon_test.ko
[[ -s $KO ]] || die "test module did not build"
printf 'MatonOS add-on test firmware\n' > "$TMP/test-addon.bin"
REL=$(modinfo -F vermagic "$KO" | awk '{print $1}')
[[ $REL == "$RELEASE" ]] || die "test module vermagic does not match the booted kernel"

bash "$HERE/build/make-package.sh" addon-test 1.0 "$RELEASE" "$TMP/package.zip" "$REPO_KEY" \
  --source test-fixture --license GPL-2.0-only \
  --module matonos_addon_test="$KO" --firmware matonos/test-addon.bin="$TMP/test-addon.bin"
cp "$TMP/package.zip" "$RELEASE_REPO/addon-test.zip"
cp "$TMP/package.zip.img" "$RELEASE_REPO/addon-test.zip.img"
cp "$TMP/package.zip.pub" "$RELEASE_REPO/addon-test.zip.pub"

# Make a structurally valid, repository-signed package with the running
# release in its top-level manifest and a different ELF vermagic.
BADVERMAGIC="X${RELEASE:1}"
cp "$KO" "$TMP/mismatch.ko"
python3 - "$TMP/mismatch.ko" "$RELEASE" "$BADVERMAGIC" <<'PY'
import pathlib, sys
p=pathlib.Path(sys.argv[1]); old=b"vermagic="+sys.argv[2].encode(); new=b"vermagic="+sys.argv[3].encode()
b=p.read_bytes(); i=b.find(old)
if i < 0: raise SystemExit("could not locate vermagic in module ELF")
if len(old)!=len(new): raise SystemExit("test vermagic replacement must preserve ELF file offsets")
p.write_bytes(b[:i]+new+b[i+len(old):])
PY
MATON_ADDON_TESTING=1 bash "$HERE/build/make-package.sh" addon-mismatch 1.0 "$RELEASE" "$TMP/mismatch.zip" "$REPO_KEY" \
  --source test-fixture --license GPL-2.0-only --test-allow-mismatched-vermagic \
  --module matonos_addon_test="$TMP/mismatch.ko"

# Test-only bridge hook is root-only and enabled only on a live userdebug image.
"$ADB" -s "$SERIAL" shell setprop persist.vendor.maton.addons_test 1
cat > "$TMP/channel.py" <<'PY'
import base64, json, pathlib, subprocess, sys
adb, serial, action, *args = sys.argv[1:]
uri="content://org.matonos.systembridge.shell"
def call(command, obj):
    payload=base64.b64encode(json.dumps(obj,separators=(",",":")).encode()).decode()
    proc=subprocess.run([adb,"-s",serial,"shell","content","call","--uri",uri,
        "--method","addons_test_call","--arg",command,"--extra",f"payload_b64:s:{payload}"],
        text=True,capture_output=True)
    if proc.returncode:
        message=(proc.stderr+proc.stdout).strip()
        raise RuntimeError(message[:1000] or f"adb/content call exited {proc.returncode}")
    out=proc.stdout
    start=out.find("result=")
    if start<0: raise RuntimeError(out)
    value=out[start+7:].rsplit("}]",1)[0]
    return json.loads(value)
if action=="call":
    result=call(args[0],json.loads(args[1])); print(json.dumps(result)); sys.exit(0 if result.get("ok") else 1)
elif action in ("upload","image"):
    path=pathlib.Path(args[0]); op=args[1]; size=path.stat().st_size
    begin="begin_package" if action=="upload" else "begin_image"
    commit="commit_package" if action=="upload" else "commit_image"
    result=call(begin,{"operationId":op,"size":size})
    if not result.get("ok"): print(json.dumps(result)); sys.exit(1)
    sent=0
    with path.open("rb") as f:
        while chunk:=f.read(32768):
            result=call("package_chunk",{"data":base64.b64encode(chunk).decode()})
            if not result.get("ok"): print(json.dumps(result)); sys.exit(1)
            sent+=len(chunk)
            if action=="image" and sent%(512*1024)<len(chunk): print(f"image upload {sent}/{size} bytes",file=sys.stderr)
    result=call(commit,{}); print(json.dumps(result)); sys.exit(0 if result.get("ok") else 1)
else: raise SystemExit("bad action")
PY
chmod 0644 "$TMP/channel.py"
python3 "$TMP/channel.py" "$ADB" "$SERIAL" call available '{}'
DEVICE_PUB_HASH=$($ADB -s "$SERIAL" shell sha256sum /odm/etc/addons/repo.pub | awk '{print $1}' | tr -d '\r')
LOCAL_PUB_HASH=$(sha256sum "$PACKAGE_PUB" | awk '{print $1}')
[[ -n $DEVICE_PUB_HASH && $DEVICE_PUB_HASH == "$LOCAL_PUB_HASH" ]] || die "test repository key does not match the image's /odm/etc/addons/repo.pub"
if BAD_RESULT=$(python3 "$TMP/channel.py" "$ADB" "$SERIAL" upload "$TMP/mismatch.zip" AddonTestBad); then
  die "service accepted a mismatched-kernel package"
fi
[[ $BAD_RESULT == *"module ELF metadata"* ]] || die "mismatched package failed for an unexpected reason: $BAD_RESULT"
python3 "$TMP/channel.py" "$ADB" "$SERIAL" upload "$TMP/package.zip" AddonTestValid
gzip -n -1 -c "$TMP/package.zip.img" > "$TMP/slot.img.gz"
python3 "$TMP/channel.py" "$ADB" "$SERIAL" image "$TMP/slot.img.gz" AddonTestImage
"$ADB" -s "$SERIAL" reboot
"$ADB" -s "$SERIAL" wait-for-device
"$ADB" -s "$SERIAL" root >/dev/null
"$ADB" -s "$SERIAL" wait-for-device

"$ADB" -s "$SERIAL" shell lsmod | grep -q '^matonos_addon_test ' || die "valid module did not load"
"$ADB" -s "$SERIAL" shell dmesg | grep 'matonos_addon_test: add-on slot test module loaded' | tail -1
"$ADB" -s "$SERIAL" shell dmesg | grep 'matonos_addon_test: firmware served' | tail -1
LABELS=$($ADB -s "$SERIAL" shell ls -Z /mnt/vendor/addons/addon-test/versions/1.0/modules/matonos_addon_test.ko | tr -d '\r')
[[ $LABELS == *vendor_addon_module_file* ]] || die "module file lacks vendor_addon_module_file label: $LABELS"
"$ADB" -s "$SERIAL" reboot
"$ADB" -s "$SERIAL" wait-for-device
"$ADB" -s "$SERIAL" root >/dev/null
"$ADB" -s "$SERIAL" wait-for-device
"$ADB" -s "$SERIAL" shell lsmod | grep '^matonos_addon_test ' || die "module did not reload after reboot"
"$ADB" -s "$SERIAL" shell dmesg | grep 'matonos_addon_test: firmware served' | tail -1
echo "PASS: signed module and firmware installed; bad kernel release rejected; labels and reboot reload verified"
