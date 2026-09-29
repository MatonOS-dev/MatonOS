#!/usr/bin/env bash
# Fetch exact, pinned microG builds from the official microG F-Droid repo.
set -Eeuo pipefail
HERE=$(dirname "$(readlink -f "$0")")
DEVICE_DIR=$(dirname "$HERE")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
JAVA=$AOSP/prebuilts/jdk/jdk21/linux-x86/bin/java
APKSIGNER=$AOSP/prebuilts/sdk/tools/linux/lib/apksigner.jar
LOCK=$HERE/gms.lock
OUT=$HERE/apks
REPO=https://repo.microg.org/fdroid/repo
[[ -x $JAVA && -f $APKSIGNER ]] || { echo "fetch-gms: AOSP JDK/apksigner missing" >&2; exit 1; }
mkdir -p "$OUT"
work=$(mktemp -d --tmpdir fetch-gms.XXXXXX)
trap 'rm -rf "$work"' EXIT
while read -r module pkg version sha signer; do
  [[ -n ${module:-} && $module != \#* ]] || continue
  dest=$OUT/$module.apk
  if [[ -f $dest ]] && echo "$sha  $dest" | sha256sum -c --status; then continue; fi
  tmp=$work/$module.apk
  curl -fsSL "$REPO/${pkg}-${version}.apk" -o "$tmp"
  echo "$sha  $tmp" | sha256sum -c --status || { echo "fetch-gms: $pkg APK hash mismatch" >&2; exit 1; }
  certs=$("$JAVA" -jar "$APKSIGNER" verify --print-certs "$tmp")
  grep -qi "certificate SHA-256 digest: $signer" <<<"$certs" || { echo "fetch-gms: $pkg signer mismatch" >&2; exit 1; }
  mv "$tmp" "$dest"
done < "$LOCK"
echo "microG APKs ready in $OUT"
