#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SRC="$ROOT/src/main/java/org/matonos/flathub/fdroid"
TEST="$ROOT/src/testHost/java/org/matonos/flathub/fdroid"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/classes" "$TMP/payload"
printf '{"timestamp":1}' > "$TMP/payload/entry.json"
keytool -genkeypair -alias fixture -keyalg RSA -keysize 2048 -validity 2 \
  -dname 'CN=F-Droid host test' -storetype PKCS12 -keystore "$TMP/test.p12" \
  -storepass changeit -keypass changeit -noprompt >/dev/null 2>&1
jar --create --file "$TMP/entry.jar" -C "$TMP/payload" .
jarsigner -keystore "$TMP/test.p12" -storepass changeit -keypass changeit "$TMP/entry.jar" fixture >/dev/null
keytool -exportcert -alias fixture -keystore "$TMP/test.p12" -storepass changeit -file "$TMP/cert.der" >/dev/null 2>&1
javac -d "$TMP/classes" "$SRC/Json.java" "$SRC/FdroidRepository.java" "$SRC/FdroidIndexClient.java" "$SRC/FdroidApkInstaller.java" "$TEST/FdroidIndexClientTest.java"
java -cp "$TMP/classes" org.matonos.flathub.fdroid.FdroidIndexClientTest "$TMP/entry.jar" "$TMP/cert.der"
