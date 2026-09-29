#!/usr/bin/env bash
# Build empty package-reservation APKs signed with per-package MatonOS keys.
set -Eeuo pipefail
die() { echo "placeholders: $*" >&2; exit 1; }
HERE=$(dirname "$(readlink -f "$0")")
DEVICE_DIR=$(dirname "$HERE")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
SDK=${MATON_ANDROID_SDK:-$HOME/Documents/matonos-android-sdk}
KEY_DIR=${MATON_KEY_DIR:-$HOME/.config/matonos-keys}
BT=$SDK/build-tools/36.0.0
ANDROID_JAR=$AOSP/prebuilts/sdk/36/public/android.jar
for tool in aapt zipalign apksigner; do [[ -x $BT/$tool ]] || die "$BT/$tool missing"; done
[[ -f $ANDROID_JAR ]] || die "API 36 android.jar missing"
mkdir -p "$HERE/placeholders" "$KEY_DIR"
chmod 700 "$KEY_DIR"
while read -r package root_cert; do
  [[ -z ${package:-} || $package == \#* ]] && continue
  [[ $package =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+$ ]] || die "invalid package $package"
  [[ $root_cert =~ ^[A-Fa-f0-9]{64}$ ]] || die "invalid root certificate for $package"
  key_file=$KEY_DIR/placeholder-${package//./_}.jks
  password_file=$KEY_DIR/placeholder-${package//./_}.pass
  if [[ ! -s $key_file ]]; then
    umask 077
    openssl rand -hex 24 > "$password_file"
    password=$(cat "$password_file")
    keytool -genkeypair -noprompt -keystore "$key_file" -storetype JKS \
      -storepass "$password" -keypass "$password" -alias matonos-dev \
      -keyalg RSA -keysize 3072 -validity 10000 \
      -dname "CN=MatonOS placeholder $package, O=MatonOS"
  fi
  [[ -s $password_file ]] || die "missing key password for $package"
  password=$(cat "$password_file")
  tmp=$(mktemp -d --tmpdir "placeholder.XXXXXX")
  trap 'rm -rf "$tmp"' EXIT
  cat > "$tmp/AndroidManifest.xml" <<MANIFEST
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$package" android:versionCode="1" android:versionName="1">
    <uses-sdk android:minSdkVersion="23" />
    <application android:hasCode="false" android:allowBackup="false" />
</manifest>
MANIFEST
  "$BT/aapt" package -f -M "$tmp/AndroidManifest.xml" -I "$ANDROID_JAR" -F "$tmp/unsigned.apk"
  "$BT/zipalign" -f -p 4 "$tmp/unsigned.apk" "$tmp/aligned.apk"
  "$BT/apksigner" sign --ks "$key_file" --ks-key-alias matonos-dev \
    --ks-pass "pass:$password" --key-pass "pass:$password" \
    --out "$HERE/placeholders/$package.apk" "$tmp/aligned.apk"
  "$BT/apksigner" verify --verbose "$HERE/placeholders/$package.apk" >/dev/null
  "$BT/aapt" dump badging "$HERE/placeholders/$package.apk" | grep -q "package: name='$package' versionCode='1'" || die "bad manifest for $package"
  "$BT/aapt" dump badging "$HERE/placeholders/$package.apk" | grep -q "application-label" && die "unexpected app label for $package"
  echo "==> Built $HERE/placeholders/$package.apk ($package; root $root_cert)"
  rm -rf "$tmp"
  trap - EXIT
done < "$HERE/placeholders.list"
