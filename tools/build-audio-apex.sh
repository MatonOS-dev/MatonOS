#!/usr/bin/env bash
# Assemble the pre-packaged, updateable vendor audio APEX outside Soong.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
. "$DEVICE_DIR/tools/local-env.sh"
AOSP=${MATON_AOSP_ROOT:-$(readlink -f "$DEVICE_DIR/../../..")}
PRODUCT_OUT=$AOSP/out/target/product/pc_x86_64
HOSTBIN=$AOSP/out/host/linux-x86/bin
HOSTLIB=$AOSP/out/host/linux-x86/lib64
NAME=com.matonos.hardware.media
KEY_DIR=${MATON_RELEASE_KEY_DIR:-}
[[ $KEY_DIR == /* ]] || die "MATON_RELEASE_KEY_DIR must be an absolute path outside the repository"
KEY_DIR=$(realpath -m "$KEY_DIR")
case "$KEY_DIR/" in "$DEVICE_DIR/"*) die "APEX keys must be outside the repository" ;; esac
PAYLOAD_KEY=$KEY_DIR/$NAME.pem
STAGE=$DEVICE_DIR/buildinfra/native-built/audio-apex
OUT_DIR=$DEVICE_DIR/audio/apex/prebuilt
TMP=$AOSP/out/audio-apex-build
INPUT=$TMP/input
for tool in apexer conv_apex_manifest signapk avbtool deapexer; do
  [[ -x $HOSTBIN/$tool ]] || die "missing AOSP host tool: $HOSTBIN/$tool"
done
[[ -s $AOSP/out/host/linux-x86/framework/signapk.jar ]] || die "missing signapk.jar"
[[ -d $STAGE/bin && -d $STAGE/lib64 ]] || die "PipeWire APEX staging missing; run tools/build-pipewire.sh"
[[ -s $PRODUCT_OUT/vendor/bin/hw/matonos-audio-hal ]] || die "Soong HAL binary missing; run the first AOSP build pass"
[[ -s $PRODUCT_OUT/vendor/apex/com.android.hardware.audio.generic.apex ]] || die "Soong generic audio APEX missing; run the bootstrap product pass"
for lib in libbundleaidl.so libdownmixaidl.so libdynamicsprocessingaidl.so libhapticgeneratoraidl.so libloudnessenhanceraidl.so libpreprocessingaidl.so libreverbaidl.so libvisualizeraidl.so; do
  [[ -s $PRODUCT_OUT/vendor/lib64/soundfx/$lib ]] || die "bootstrap AOSP effect library missing: $lib"
done
[[ -s $KEY_DIR/releasekey.pk8 && -s $KEY_DIR/releasekey.x509.pem ]] || die "run tools/release-keys.sh before building the audio APEX"
mkdir -p "$KEY_DIR" "$OUT_DIR" "$TMP"
chmod 0700 "$KEY_DIR"
VERSION_FILE=$KEY_DIR/$NAME.version
now=$(date -u +%s); previous=0
[[ -s $VERSION_FILE ]] && read -r previous < "$VERSION_FILE"
[[ $previous =~ ^[0-9]+$ ]] || die "invalid saved APEX version in $VERSION_FILE"
VERSION=$now; (( VERSION > previous )) || VERSION=$((previous + 1))
printf '%s\n' "$VERSION" > "$VERSION_FILE"; chmod 0600 "$VERSION_FILE"
if [[ ! -s $PAYLOAD_KEY ]]; then umask 077; openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:4096 -out "$PAYLOAD_KEY"; fi
export APEXER_TOOL_PATH="$HOSTBIN:$HOSTLIB:$AOSP/prebuilts/sdk/tools/linux/bin"
"$HOSTBIN/deapexer" extract "$PRODUCT_OUT/vendor/apex/com.android.hardware.audio.generic.apex" "$TMP/generic-audio"
[[ -s $TMP/generic-audio/bin/hw/android.hardware.audio.effect.service-aidl.generic ]] || die "APEX-compiled effects service missing from generic audio APEX"
[[ -d $TMP/generic-audio/lib64/soundfx ]] || die "generic audio APEX effects libraries missing"

python3 - "$STAGE" "$INPUT" "$PRODUCT_OUT" "$DEVICE_DIR" "$TMP" <<'PY'
import pathlib, shutil, sys
stage, payload, product, device, temp = map(pathlib.Path, sys.argv[1:])
if payload.exists(): shutil.rmtree(payload)
shutil.copytree(stage, payload, symlinks=False)
for name in ('SOURCE', 'NEEDED.txt'): (payload / name).unlink(missing_ok=True)
source_lib64 = temp / 'generic-audio/lib64'
for lib in source_lib64.glob('*.so*'): shutil.copy2(lib, payload / 'lib64' / lib.name)
# A bootstrap effects APEX may carry the stock provider. The final HAL owns
# the software factory in-process; never retain a second provider implementation.
(payload / 'lib64/android.hardware.bluetooth.audio-impl.so').unlink(missing_ok=True)
source_effects, target_effects = source_lib64 / 'soundfx', payload / 'lib64/soundfx'
target_effects.mkdir(parents=True, exist_ok=True)
for lib in source_effects.glob('*.so*'): shutil.copy2(lib, target_effects / lib.name)
for lib in (product / 'vendor/lib64/soundfx').glob('*.so*'): shutil.copy2(lib, target_effects / lib.name)
for rel, source in [
 ('share/licenses/matonos-bluetooth-audio/NOTICE', device / 'audio/bt-audio/NOTICE'),
 ('share/licenses/matonos-bluetooth-audio/PROVENANCE.md', device / 'audio/bt-audio/PROVENANCE.md'),
 ('bin/hw/matonos-audio-hal', product / 'vendor/bin/hw/matonos-audio-hal'),
 ('bin/hw/audio-effect-service', temp / 'generic-audio/bin/hw/android.hardware.audio.effect.service-aidl.generic'),
 ('etc/audio_effects_config.xml', device / 'audio/effects/audio_effects_config.xml'),
 ('etc/maton-pipewire.rc', device / 'audio/maton-pipewire.rc'),
 ('etc/matonos-audio-hal.rc', device / 'audio/native-hal/matonos-audio-hal.rc'),
 ('etc/audio-effect-service.rc', device / 'audio/apex/audio-effect-service.rc'),
 ('etc/vintf/audio.xml', device / 'audio/native-hal/vintf/manifest.xml')]:
 target = payload / rel; target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, target)
PY
READELF=${READELF:-$HOSTBIN/llvm-readelf}; [[ -x $READELF ]] || READELF=$(command -v readelf || true)
[[ -n $READELF && -x $READELF ]] || die "readelf is required"
VENDOR_APEX_PREPARE_ARGS=(--lib-root "$PRODUCT_OUT/vendor/lib64" --xml "$DEVICE_DIR/audio/effects/audio_effects_config.xml" --require-needed libpipewire-0.3.so)
# Audio-specific payload checks remain local to this builder.
python3 - "$INPUT" "$READELF" <<'PY'
import pathlib, subprocess, sys
payload, readelf = pathlib.Path(sys.argv[1]), sys.argv[2]
hal = payload / 'bin/hw/matonos-audio-hal'
needed = subprocess.run([readelf, '-d', str(hal)], check=True, text=True, capture_output=True).stdout
if 'libpipewire-0.3.so' not in needed or not (payload / 'lib64/libpipewire-0.3.so').is_file():
 raise SystemExit('HAL DT_NEEDED libpipewire-0.3.so is not resolved inside the audio APEX')
# The whole-linked software factory must resolve ModuleBluetooth's C entrypoint.
# A shared stock provider here would silently restore offload implementations.
if 'android.hardware.bluetooth.audio-impl.so' in needed:
 raise SystemExit('HAL still imports the stock Bluetooth audio provider; software factory was not linked')
if not list((payload / 'lib64/spa-0.2').rglob('*.so')): raise SystemExit('PipeWire SPA plugins missing from audio APEX')
PY
source "$DEVICE_DIR/tools/lib/vendor-apex.sh"
vendor_apex_finish "$NAME" "$INPUT" "$DEVICE_DIR/audio/apex/file_contexts" "$INPUT/etc/vintf/audio.xml" "$VERSION" "$OUT_DIR" "$KEY_DIR" "$TMP" "$DEVICE_DIR" "$AOSP" "$PRODUCT_OUT" "$HOSTBIN" "$HOSTLIB" "$READELF"
