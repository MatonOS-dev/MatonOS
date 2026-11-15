#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SRC="$ROOT/src/main/java/org/matonos/store/fdroid"
TEST="$ROOT/src/testHost/java/org/matonos/store/fdroid"
TMP=$(mktemp -d "${MATON_TEST_TMPDIR:-${TMPDIR:-/tmp}}/fdroid-host.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/classes" "$TMP/payload"
printf '{"timestamp":1}' > "$TMP/payload/entry.json"
keytool -genkeypair -alias fixture -keyalg RSA -keysize 2048 -validity 2 \
  -dname 'CN=F-Droid host test' -storetype PKCS12 -keystore "$TMP/test.p12" \
  -storepass changeit -keypass changeit -noprompt >/dev/null 2>&1
jar --create --file "$TMP/entry.jar" -C "$TMP/payload" .
jarsigner -keystore "$TMP/test.p12" -storepass changeit -keypass changeit "$TMP/entry.jar" fixture >/dev/null
keytool -exportcert -alias fixture -keystore "$TMP/test.p12" -storepass changeit -file "$TMP/cert.der" >/dev/null 2>&1
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
AAPT2="$SDK/build-tools/36.0.0/aapt2"
for kind in base split; do
  split_attr=; [[ $kind != split ]] || split_attr='split="config.en"'
  printf '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.example.demo" %s android:versionCode="1"><uses-sdk android:minSdkVersion="37"/><application/></manifest>' "$split_attr" > "$TMP/AndroidManifest.xml"
  "$AAPT2" link --manifest "$TMP/AndroidManifest.xml" -I "$SDK/platforms/android-37.0/android.jar" -o "$TMP/$kind.apk"
done
javac -d "$TMP/classes" "$SRC/Json.java" "$SRC/FdroidRepository.java" "$SRC/FdroidIndexClient.java" "$SRC/FdroidApkInstaller.java" "$SRC/FdroidSafety.java" "$SRC/ApkManifest.java" "$TEST/FdroidIndexClientTest.java"
java -cp "$TMP/classes" org.matonos.store.fdroid.FdroidIndexClientTest "$TMP/entry.jar" "$TMP/cert.der"
