#!/usr/bin/env bash
# Assemble the updatable vendor radio APEX outside Soong.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
. "$DEVICE_DIR/tools/local-env.sh"
AOSP=${MATON_AOSP_ROOT:-$(readlink -f "$DEVICE_DIR/../../..")}
PRODUCT_OUT=$AOSP/out/target/product/pc_x86_64
HOSTBIN=$AOSP/out/host/linux-x86/bin
HOSTLIB=$AOSP/out/host/linux-x86/lib64
NAME=com.matonos.hardware.radio
KEY_DIR=${MATON_RELEASE_KEY_DIR:-}
[[ $KEY_DIR == /* ]] || die "MATON_RELEASE_KEY_DIR must be an absolute path outside the repository"
KEY_DIR=$(realpath -m "$KEY_DIR")
case "$KEY_DIR/" in "$DEVICE_DIR/"*) die "APEX keys must be outside the repository" ;; esac
PAYLOAD_KEY=$KEY_DIR/$NAME.pem
OUT_DIR=$DEVICE_DIR/radio/apex/prebuilt
TMP=$AOSP/out/radio-apex-build
INPUT=$TMP/input
NATIVE=$DEVICE_DIR/buildinfra/native-built
for tool in apexer conv_apex_manifest signapk avbtool; do [[ -x $HOSTBIN/$tool ]] || die "missing AOSP host tool: $HOSTBIN/$tool"; done
[[ -s $AOSP/out/host/linux-x86/framework/signapk.jar ]] || die "missing signapk.jar"
[[ -x $PRODUCT_OUT/vendor/bin/hw/android.hardware.bluetooth-service.matonos ]] || die "bootstrap Bluetooth HAL missing"
[[ -x $PRODUCT_OUT/vendor/bin/hw/wpa_supplicant ]] || die "bootstrap wpa_supplicant missing"
[[ -x $PRODUCT_OUT/vendor/bin/hw/hostapd ]] || die "bootstrap hostapd missing"
[[ -x $NATIVE/matonos-wifid ]] || die "NDK matonos-wifid missing; run tools/build-native.sh"
[[ -s $NATIVE/libmatonos-ipc.so ]] || die "NDK libmatonos-ipc missing; run tools/build-native.sh"
[[ -s $KEY_DIR/releasekey.pk8 && -s $KEY_DIR/releasekey.x509.pem ]] || die "run tools/release-keys.sh before building the radio APEX"
mkdir -p "$KEY_DIR" "$OUT_DIR" "$TMP"; chmod 0700 "$KEY_DIR"
VERSION_FILE=$KEY_DIR/$NAME.version
now=$(date -u +%s); previous=0
[[ -s $VERSION_FILE ]] && read -r previous < "$VERSION_FILE"
[[ $previous =~ ^[0-9]+$ ]] || die "invalid saved APEX version in $VERSION_FILE"
VERSION=$now; (( VERSION > previous )) || VERSION=$((previous + 1))
printf '%s\n' "$VERSION" > "$VERSION_FILE"; chmod 0600 "$VERSION_FILE"
if [[ ! -s $PAYLOAD_KEY ]]; then umask 077; openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:4096 -out "$PAYLOAD_KEY"; fi
python3 - "$DEVICE_DIR" "$PRODUCT_OUT" "$NATIVE" "$INPUT" "$DEVICE_DIR/radio/apex/payload.list" <<'PY'
import pathlib, shutil, sys
root, product, native, payload, listing = map(pathlib.Path, sys.argv[1:])
if payload.exists(): shutil.rmtree(payload)
payload.mkdir(parents=True)
roots = {'product': product, 'native': native, 'radio': root / 'radio'}
for row in listing.read_text().splitlines():
    row = row.strip()
    if not row or row.startswith('#'): continue
    source, target = row.split()
    prefix, rel = source.split(':', 1)
    src = roots[prefix] / rel
    if not src.is_file(): raise SystemExit(f'missing radio APEX payload input: {src}')
    dst = payload / target
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
PY
READELF=${READELF:-$HOSTBIN/llvm-readelf}; [[ -x $READELF ]] || READELF=$(command -v readelf || true)
[[ -n $READELF && -x $READELF ]] || die "readelf is required"
mkdir -p "$TMP/odm-lib64"
install -m 0644 "$NATIVE/libmatonos-ipc.so" "$TMP/odm-lib64/libmatonos-ipc.so"
VENDOR_APEX_PREPARE_ARGS=(--lib-root "$PRODUCT_OUT/vendor/lib64" --lib-root "$TMP/odm-lib64")
source "$DEVICE_DIR/tools/lib/vendor-apex.sh"
vendor_apex_finish "$NAME" "$INPUT" "$DEVICE_DIR/radio/apex/file_contexts" "$INPUT/etc/vintf/manifest.xml" "$VERSION" "$OUT_DIR" "$KEY_DIR" "$TMP" "$DEVICE_DIR" "$AOSP" "$PRODUCT_OUT" "$HOSTBIN" "$HOSTLIB" "$READELF"
