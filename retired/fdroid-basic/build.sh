#!/usr/bin/env bash
# Fetch, patch, build and MatonOS-sign F-Droid Basic outside Soong.
set -Eeuo pipefail

die() { echo "fdroid-basic: $*" >&2; exit 1; }
info() { echo "==> $*"; }

HERE=$(dirname "$(readlink -f "$0")")
DEVICE_DIR=$(dirname "$(dirname "$HERE")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
COMMIT=30f467b2c6b8f661191a70ae004af933dad5b5f0
ARCHIVE_SHA256=dfba974802af8faac934bd0ad275fe0edd4757eb41b4ade222b434556d0fbf76
SDK=${MATON_ANDROID_SDK:-$HOME/Documents/matonos-android-sdk}
KEY_DIR=${MATON_KEY_DIR:-$HOME/.config/matonos-keys}
KEY_ALIAS=matonos-dev
JOBS=${MATON_BUILD_JOBS:-4}
JAVA_HOME=${JAVA_HOME:-$AOSP/prebuilts/jdk/jdk21/linux-x86}
[[ $JOBS =~ ^[1-4]$ ]] || die "MATON_BUILD_JOBS must be between 1 and 4"
[[ -x $JAVA_HOME/bin/java ]] || die "JDK 21 not found at $JAVA_HOME"
export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"
for cmd in curl tar sha256sum openssl keytool; do command -v "$cmd" >/dev/null || die "$cmd is required"; done
BT=$SDK/build-tools/36.0.0
for tool in zipalign apksigner aapt; do [[ -x $BT/$tool ]] || die "$BT/$tool is missing; run tools/build-apps.sh first"; done
[[ -f $AOSP/prebuilts/sdk/36/public/android.jar ]] || die "AOSP API 36 public SDK is missing"

tmp=$(mktemp -d --tmpdir fdroid-basic.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
archive=$tmp/source.tar.gz
url="https://gitlab.com/fdroid/fdroidclient/-/archive/$COMMIT/fdroidclient-$COMMIT.tar.gz"
info "Fetching F-Droid Client commit $COMMIT"
curl -fsSL "$url" -o "$archive"
echo "$ARCHIVE_SHA256  $archive" | sha256sum -c --status || die "source archive hash mismatch"
tar -xzf "$archive" -C "$tmp"
src=$tmp/fdroidclient-$COMMIT
[[ -d $src/app ]] || die "unexpected source archive layout"
git -C "$src" init -q
git -C "$src" add app/src/main/AndroidManifest.xml app/src/main/assets/default_repos.json \
  libs/database/src/main/java/org/fdroid/database/DbAppChecker.kt
git -C "$src" -c user.name=MatonOS -c user.email=build@matonos.invalid \
  commit -q -m "Pinned upstream source" --allow-empty
git -C "$src" apply --check "$HERE/0001-privileged-install.patch" || die "permission patch no longer applies"
git -C "$src" apply "$HERE/0001-privileged-install.patch"

mkdir -p "$KEY_DIR"
chmod 700 "$KEY_DIR"
key_file=$KEY_DIR/fdroid-basic.jks
password_file=$KEY_DIR/fdroid-basic.pass
if [[ ! -s $key_file ]]; then
  umask 077
  openssl rand -hex 24 > "$password_file"
  keypass=$(cat "$password_file")
  keytool -genkeypair -noprompt -keystore "$key_file" -storetype JKS \
    -storepass "$keypass" -keypass "$keypass" -alias "$KEY_ALIAS" \
    -keyalg RSA -keysize 3072 -validity 10000 \
    -dname "CN=MatonOS fdroid-basic development key, O=MatonOS"
fi
[[ -s $password_file ]] || die "missing key password $password_file"
keypass=$(cat "$password_file")

info "Building the basic/default/release flavor"
(
  cd "$src"
  export ANDROID_HOME=$SDK ANDROID_SDK_ROOT=$SDK
  cp "$AOSP/prebuilts/sdk/36/public/android.jar" "$SDK/platforms/android-36/android.jar"
  ./gradlew --no-daemon --max-workers "$JOBS" --stacktrace :app:assembleBasicDefaultRelease
)
unsigned=$(find "$src/app/build/outputs/apk" -type f -name '*basic*default*release*.apk' -print -quit)
[[ -s $unsigned ]] || die "Basic release APK was not produced"
aligned=$tmp/aligned.apk
signed=$tmp/FdroidBasic.apk
"$BT/zipalign" -f -p 4 "$unsigned" "$aligned"
"$BT/apksigner" sign --ks "$key_file" --ks-key-alias "$KEY_ALIAS" \
  --ks-pass "pass:$keypass" --key-pass "pass:$keypass" --out "$signed" "$aligned"
"$BT/apksigner" verify --verbose "$signed" >/dev/null || die "APK signature check failed"
badging=$("$BT/aapt" dump badging "$signed")
grep -q "package: name='org.fdroid.basic'" <<<"$badging" || die "wrong package ID"
permissions=$("$BT/aapt" dump permissions "$signed")
grep -q 'android.permission.INSTALL_PACKAGES' <<<"$permissions" || die "INSTALL_PACKAGES missing"
grep -q 'android.permission.DELETE_PACKAGES' <<<"$permissions" || die "DELETE_PACKAGES missing"
install -m 0644 "$signed" "$HERE/FdroidBasic.apk"
info "Staged $HERE/FdroidBasic.apk"
