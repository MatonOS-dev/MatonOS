#!/usr/bin/env bash
# Assemble the pre-packaged, updateable vendor audio APEX outside Soong.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
PRODUCT_OUT=$AOSP/out/target/product/pc_x86_64
HOSTBIN=$AOSP/out/host/linux-x86/bin
HOSTLIB=$AOSP/out/host/linux-x86/lib64
NAME=com.matonos.hardware.audio
VERSION=
KEY_DIR=${MATON_RELEASE_KEY_DIR:-}
[[ $KEY_DIR == /* ]] || die "MATON_RELEASE_KEY_DIR must be an absolute path outside the repository"
KEY_DIR=$(realpath -m "$KEY_DIR")
case "$KEY_DIR/" in "$DEVICE_DIR/"*) die "APEX keys must be outside the repository" ;; esac
PAYLOAD_KEY=$KEY_DIR/$NAME.pem
PAYLOAD_PUB=$KEY_DIR/$NAME.avbpubkey
CONTAINER_KEY=$KEY_DIR/releasekey.pk8
CONTAINER_CERT=$KEY_DIR/releasekey.x509.pem
STAGE=$DEVICE_DIR/buildinfra/native-built/audio-apex
OUT_DIR=$DEVICE_DIR/audio/apex/prebuilt
TMP=$AOSP/out/audio-apex-build
INPUT=$TMP/input

for tool in apexer conv_apex_manifest signapk avbtool deapexer; do
  [[ -x $HOSTBIN/$tool ]] || die "missing AOSP host tool: $HOSTBIN/$tool"
done
[[ -x $AOSP/out/host/linux-x86/framework/signapk.jar ]] || die "missing signapk.jar"
[[ -d $STAGE/bin && -d $STAGE/lib64 ]] || die "PipeWire APEX staging missing; run tools/build-pipewire.sh"
[[ -s $PRODUCT_OUT/vendor/bin/hw/matonos-audio-hal ]] || die "Soong HAL binary missing; run the first AOSP build pass"
[[ -s $PRODUCT_OUT/vendor/apex/com.android.hardware.audio.generic.apex ]] || die "Soong generic audio APEX missing; run the bootstrap product pass"
for lib in libbundleaidl.so libdownmixaidl.so libdynamicsprocessingaidl.so \
           libhapticgeneratoraidl.so libloudnessenhanceraidl.so libpreprocessingaidl.so \
           libreverbaidl.so libvisualizeraidl.so; do
  [[ -s $PRODUCT_OUT/vendor/lib64/soundfx/$lib ]] || die "bootstrap AOSP effect library missing: $lib"
done
[[ -s $CONTAINER_KEY && -s $CONTAINER_CERT ]] || die "run tools/release-keys.sh before building the audio APEX"
mkdir -p "$KEY_DIR" "$OUT_DIR" "$TMP"
chmod 0700 "$KEY_DIR"
VERSION_FILE=$KEY_DIR/$NAME.version
now=$(date -u +%s)
previous=0
[[ -s $VERSION_FILE ]] && read -r previous < "$VERSION_FILE"
[[ $previous =~ ^[0-9]+$ ]] || die "invalid saved APEX version in $VERSION_FILE"
VERSION=$now
(( VERSION > previous )) || VERSION=$((previous + 1))
printf '%s\n' "$VERSION" > "$VERSION_FILE"
chmod 0600 "$VERSION_FILE"
if [[ ! -s $PAYLOAD_KEY ]]; then
  umask 077
  openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$PAYLOAD_KEY"
fi
export APEXER_TOOL_PATH="$HOSTBIN:$HOSTLIB:$AOSP/prebuilts/sdk/tools/linux/bin"
"$HOSTBIN/deapexer" extract "$PRODUCT_OUT/vendor/apex/com.android.hardware.audio.generic.apex" "$TMP/generic-audio"
[[ -s $TMP/generic-audio/bin/hw/android.hardware.audio.effect.service-aidl.generic ]] || die "APEX-compiled effects service missing from generic audio APEX"
[[ -d $TMP/generic-audio/lib64/soundfx ]] || die "generic audio APEX effects libraries missing"
"$HOSTBIN/avbtool" extract_public_key --key "$PAYLOAD_KEY" --output "$PAYLOAD_PUB"

python3 - "$STAGE" "$INPUT" "$PRODUCT_OUT" "$DEVICE_DIR" "$VERSION" "$TMP" <<'PY'
import json, os, pathlib, shutil, sys
stage, input_dir, product_out, device, version, temp = map(pathlib.Path, sys.argv[1:])
version = sys.argv[5]
if input_dir.exists():
    shutil.rmtree(input_dir)
shutil.copytree(stage, input_dir, symlinks=False)
for diagnostic in (input_dir / "SOURCE", input_dir / "NEEDED.txt"):
    diagnostic.unlink(missing_ok=True)
# Keep the AOSP generic software-effect implementations currently referenced
# by the MatonOS config, but bind them to this APEX namespace and update unit.
source_lib64 = pathlib.Path(sys.argv[6]) / "generic-audio/lib64"
for lib in source_lib64.glob("*.so*"):
    shutil.copy2(lib, input_dir / "lib64" / lib.name)
source_effects = source_lib64 / "soundfx"
target_effects = input_dir / "lib64/soundfx"
target_effects.mkdir(parents=True, exist_ok=True)
for lib in source_effects.glob("*.so*"):
    shutil.copy2(lib, target_effects / lib.name)
for lib in (pathlib.Path(sys.argv[3]) / "vendor/lib64/soundfx").glob("*.so*"):
    shutil.copy2(lib, target_effects / lib.name)
for rel, source in [
    ("bin/hw/matonos-audio-hal", product_out / "vendor/bin/hw/matonos-audio-hal"),
    ("bin/hw/audio-effect-service", pathlib.Path(sys.argv[6]) / "generic-audio/bin/hw/android.hardware.audio.effect.service-aidl.generic"),
    ("etc/audio_effects_config.xml", device / "audio/effects/audio_effects_config.xml"),
    ("etc/init/maton-pipewire.rc", device / "audio/maton-pipewire.rc"),
    ("etc/init/matonos-audio-hal.rc", device / "audio/native-hal/matonos-audio-hal.rc"),
    ("etc/init/audio-effect-service.rc", device / "audio/apex/audio-effect-service.rc"),
    ("etc/vintf/manifest/audio.xml", device / "audio/native-hal/vintf/manifest.xml"),
]:
    target = input_dir / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
# Linkerconfig must expose vendor/LLNDK dependencies to this vendor APEX namespace.
needed = set()
import re, subprocess
elfs = [p for p in input_dir.rglob("*") if p.is_file() and os.access(p, os.X_OK)]
elfs += [p for p in (input_dir / "lib64").rglob("*.so*") if p.is_file()]
for elf in elfs:
    if elf.read_bytes()[:4] != b"\x7fELF":
        continue
    output = subprocess.run(["readelf", "-d", str(elf)], check=True, text=True, capture_output=True).stdout
    for lib in re.findall(r"Shared library: \[(.*?)\]", output):
        if not ((input_dir / "lib64" / lib).exists() or (input_dir / "lib64/soundfx" / lib).exists()):
            needed.add(lib)
hal_needed = subprocess.run(["readelf", "-d", str(input_dir / "bin/hw/matonos-audio-hal")], check=True, text=True, capture_output=True).stdout
if "libpipewire-0.3.so" not in hal_needed or not (input_dir / "lib64/libpipewire-0.3.so").is_file():
    raise SystemExit("HAL DT_NEEDED libpipewire-0.3.so is not resolved inside the audio APEX")
if not list((input_dir / "lib64/spa-0.2").rglob("*.so")):
    raise SystemExit("PipeWire SPA plugins missing from audio APEX")
for path in (device / "audio/effects/audio_effects_config.xml",):
    import xml.etree.ElementTree as ET
    for element in ET.parse(path).iter("library"):
        needed.add(element.attrib["path"])
manifest = {"name": "com.matonos.hardware.audio", "version": int(version), "requireNativeLibs": sorted(needed)}
(temp / "apex_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
# apexer requires an explicit uid/gid/mode for every payload path.
with (temp / "canned_fs_config").open("w") as f:
    f.write("/ 0 0 0755\n")
    f.write("/apex_manifest.pb 0 0 0644\n")
    f.write("/apex_manifest.json 0 0 0644\n")
    for path in sorted(input_dir.rglob("*")):
        rel = "/" + path.relative_to(input_dir).as_posix()
        mode = 0o755 if path.is_dir() or (path.stat().st_mode & 0o111) else 0o644
        f.write(f"{rel} 0 0 {mode:04o}\n")
android_manifest = f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.matonos.hardware.audio" android:versionCode="{version}" android:versionName="{version}"><application android:hasCode="false" /></manifest>\n'''
(temp / "AndroidManifest.xml").write_text(android_manifest)
PY
"$HOSTBIN/conv_apex_manifest" proto "$TMP/apex_manifest.json" -o "$TMP/apex_manifest.pb"
"$HOSTBIN/apexer" --force --do_not_check_keyname \
  --manifest "$TMP/apex_manifest.pb" --manifest_json "$TMP/apex_manifest.json" \
  --file_contexts "$DEVICE_DIR/audio/apex/file_contexts" \
  --canned_fs_config "$TMP/canned_fs_config" \
  --key "$PAYLOAD_KEY" --pubkey "$PAYLOAD_PUB" \
  --android_manifest "$TMP/AndroidManifest.xml" \
  --android_jar_path "$AOSP/prebuilts/sdk/current/public/android.jar" \
  --apexer_tool_path "$APEXER_TOOL_PATH" \
  "$INPUT" "$TMP/$NAME.apex.unsigned"
java -Djava.library.path="$HOSTLIB" -jar "$AOSP/out/host/linux-x86/framework/signapk.jar" \
  -a 4096 --align-file-size --disable-v1 "$CONTAINER_CERT" "$CONTAINER_KEY" \
  "$TMP/$NAME.apex.unsigned" "$OUT_DIR/$NAME.apex"
echo "Built $OUT_DIR/$NAME.apex (version $VERSION, payload key $PAYLOAD_KEY)"
