#!/usr/bin/env bash
# fetch-apps.sh: download MatonOS's preinstalled apps from F-Droid's main repo.
#
# Usage:
#   ./fetch-apps.sh        download every app pinned in apps/apps.lock into
#                          apps/apks/<module>.apk (git-ignored) and verify it
#   ./fetch-apps.sh -u     re-pin apps/apps.lock to the newest version of each
#                          package in F-Droid's index (x86_64 or ABI-less
#                          builds), then download
#
# apps/apps.lock, one app per line:
#   <soong module> <package> <versionCode> <apk sha256> <signer cert sha256> [url]
#   With [url], the APK comes from that upstream URL instead of F-Droid
#   (upstream-signed builds, e.g. Aurora Store's preload flavor); -u leaves
#   such lines alone.
#
# The APKs are installed byte for byte (apps/Android.bp: preprocessed,
# presigned), never re-signed: F-Droid can only update an app whose signer
# matches, so every APK is checked against both the pinned file hash and the
# signer F-Droid's index lists for that version.

set -Eeuo pipefail

die() { echo "fetch-apps: $*" >&2; exit 1; }
info() { echo "==> $*"; }

HERE=$(dirname "$(readlink -f "$0")")
DEVICE_DIR=$(dirname "$HERE")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
LOCK=$DEVICE_DIR/apps/apps.lock
OUT=$DEVICE_DIR/apps/apks
REPO=https://f-droid.org/repo
ARCHIVE=https://f-droid.org/archive
# AOSP's bundled JDK + apksigner.jar (the prebuilts wrapper script can't find
# its jar, and host Java may be too old).
JAVA=$AOSP/prebuilts/jdk/jdk21/linux-x86/bin/java
APKSIGNER_JAR=$AOSP/prebuilts/sdk/tools/linux/lib/apksigner.jar

UPDATE=0
while getopts "uh" opt; do
  case $opt in
    u) UPDATE=1 ;;
    *) sed -n '2,17p' "$0"; exit 0 ;;
  esac
done

[[ -f $LOCK ]] || die "missing $LOCK"
[[ -x $JAVA && -f $APKSIGNER_JAR ]] || die "missing AOSP JDK or apksigner.jar in prebuilts/"
command -v curl >/dev/null || die "curl not found"
command -v python3 >/dev/null || die "python3 not found"

work=$(mktemp -d --tmpdir fetch-apps.XXXXXX)
trap 'rm -rf "$work"' EXIT

# ---------------------------------------------------------------- re-pin
if (( UPDATE )); then
  info "Downloading F-Droid index"
  curl -sSfL -o "$work/index-v2.json" "$REPO/index-v2.json"
  python3 - "$work/index-v2.json" "$LOCK" <<'EOF'
import json, sys
index, lock = sys.argv[1], sys.argv[2]
packages = json.load(open(index))["packages"]
out = []
for line in open(lock):
    if not line.strip() or line.startswith("#"):
        out.append(line)
        continue
    if len(line.split()) > 5:
        out.append(line)
        continue
    module, pkg = line.split()[:2]
    best = None
    for v in packages[pkg]["versions"].values():
        m = v["manifest"]
        abis = m.get("nativecode")
        if abis and "x86_64" not in abis:
            continue
        if best is None or m["versionCode"] > best["manifest"]["versionCode"]:
            best = v
    if best is None:
        sys.exit(f"{pkg}: no x86_64 build in the index")
    m = best["manifest"]
    signer = m["signer"]["sha256"][0]
    out.append(f"{module} {pkg} {m['versionCode']} {best['file']['sha256']} {signer}\n")
    print(f"  {pkg} -> {m['versionName']} ({m['versionCode']})")
open(lock, "w").writelines(out)
EOF
fi

# ---------------------------------------------------------------- download
mkdir -p "$OUT"
while read -r module pkg vc sha signer url; do
  [[ -z $module || $module == \#* ]] && continue
  dest=$OUT/$module.apk
  if [[ -f $dest ]] && echo "$sha  $dest" | sha256sum -c --status; then
    info "$pkg $vc: up to date"
    continue
  fi
  info "$pkg $vc: downloading"
  tmp=$work/$module.apk
  file=${pkg}_$vc.apk
  # Superseded versions move from repo/ to archive/.
  if [[ -n $url ]]; then
    curl -sSfL -o "$tmp" "$url" || die "$module: download failed ($url)"
  else
    curl -sSfL -o "$tmp" "$REPO/$file" || curl -sSfL -o "$tmp" "$ARCHIVE/$file" ||
      die "$file not found in F-Droid's repo or archive"
  fi
  echo "$sha  $tmp" | sha256sum -c --status || die "$file: SHA-256 mismatch"
  certs=$("$JAVA" -jar "$APKSIGNER_JAR" verify --print-certs "$tmp") || die "$file: signature invalid"
  grep -qi "certificate SHA-256 digest: $signer" <<<"$certs" ||
    die "$file: signer is not the pinned one ($signer)"
  mv "$tmp" "$dest"
done < "$LOCK"

# Soong only accepts skip_preprocessed_apk_checks on APKs that fail its
# checks (compressed JNI libs, misalignment), and fails on the others; a
# re-pin can flip that. Compare with apps/Android.bp now instead of midway
# through a build.
CHECK=$AOSP/build/soong/scripts/check_prebuilt_presigned_apk.py
AAPT2=$AOSP/out/host/linux-x86/bin/aapt2
ZIPALIGN=$AOSP/out/host/linux-x86/bin/zipalign
if [[ -x $AAPT2 && -x $ZIPALIGN ]]; then
  bp=$DEVICE_DIR/apps/Android.bp
  mismatch=0
  while read -r module _; do
    [[ -z $module || $module == \#* ]] && continue
    # Same flags Soong passes: priv-apps must also have uncompressed dex.
    block=$(awk -v m="\"$module\"," '$1 == "name:" && $2 == m {f = 1}
              f {print} f && /^}/ {exit}' "$bp")
    priv=()
    grep -q "privileged: true" <<<"$block" &&
      priv=(--privileged --uncompress-priv-app-dex)
    if python3 "$CHECK" --aapt2 "$AAPT2" --zipalign "$ZIPALIGN" "${priv[@]}" \
         --preprocessed "$OUT/$module.apk" "$work/stamp" >/dev/null 2>&1; then
      needs=0
    else
      needs=1
    fi
    has=0
    grep -q "skip_preprocessed_apk_checks: true" <<<"$block" && has=1
    if [[ $needs != "$has" ]]; then
      echo "fetch-apps: $module: set skip_preprocessed_apk_checks to" \
           "$([[ $needs == 1 ]] && echo true || echo "nothing (remove it)") in apps/Android.bp" >&2
      mismatch=1
    fi
  done < "$LOCK"
  (( mismatch == 0 )) || die "apps/Android.bp doesn't match the APKs"
else
  info "no aapt2/zipalign in out/host yet: skipping the preprocessed-APK check"
fi

# Preinstalled apps get no native-library extraction: the package manager
# loads them from the APK (stored uncompressed) or from <app dir>/lib/<abi>/.
# For APKs with compressed x86_64 libraries (e.g. Fennec), extract them to
# apks/<module>.lib/x86_64/; apps/apps.mk installs them next to the APK.
python3 - "$OUT" <<'EOF'
import os, shutil, sys, zipfile
out = sys.argv[1]
for apk in sorted(os.listdir(out)):
    if not apk.endswith(".apk"):
        continue
    libdir = os.path.join(out, apk[:-4] + ".lib")
    shutil.rmtree(libdir, ignore_errors=True)
    with zipfile.ZipFile(os.path.join(out, apk)) as z:
        libs = [i for i in z.infolist()
                if i.filename.startswith("lib/x86_64/") and i.filename.endswith(".so")]
        if not any(i.compress_type != zipfile.ZIP_STORED for i in libs):
            continue
        dest = os.path.join(libdir, "x86_64")
        os.makedirs(dest)
        for i in libs:
            name = os.path.join(dest, os.path.basename(i.filename))
            with z.open(i) as src, open(name, "wb") as f:
                shutil.copyfileobj(src, f)
        print(f"==> {apk[:-4]}: extracted {len(libs)} x86_64 native libraries")
EOF

# Remove APKs no longer pinned.
for f in "$OUT"/*.apk; do
  [[ -e $f ]] || continue
  m=$(basename "$f" .apk)
  grep -q "^$m " "$LOCK" || { info "removing unpinned $m"; rm -rf "$f" "$OUT/$m.lib"; }
done
info "Apps ready in $OUT"
