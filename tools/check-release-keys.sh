#!/usr/bin/env bash
# Fail if any APK or APEX installed in a product out dir is signed with one of AOSP's
# public test certificates (build/make/target/product/security/*.x509.pem).
# Usage: check-release-keys.sh <aosp-root> <product-out-dir>
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
(( $# == 2 )) || die "usage: $0 <aosp-root> <product-out-dir>"
AOSP=$1 OUT=$2
SIGNER="$AOSP/prebuilts/sdk/tools/linux/lib/apksigner.jar"
[[ -f $SIGNER ]] || die "apksigner.jar not found at $SIGNER"
[[ -d $OUT ]] || die "product out dir not found: $OUT"
command -v java >/dev/null || die "java is required"

# SHA-256 of each public test certificate, as apksigner prints it.
declare -A test_certs=()
for pem in "$AOSP"/build/make/target/product/security/*.x509.pem; do
  digest=$(openssl x509 -in "$pem" -outform DER | sha256sum | cut -d' ' -f1) ||
    die "cannot read $pem"
  test_certs[$digest]=$(basename "$pem" .x509.pem)
done
(( ${#test_certs[@]} > 0 )) || die "no test certificates found under $AOSP/build/make/target/product/security"

bad=0 checked=0
while IFS= read -r -d '' file; do
  checked=$((checked + 1))
  if ! certs=$(java -jar "$SIGNER" verify --print-certs "$file" 2>&1); then
    echo "UNVERIFIED ${file#"$OUT"/}: $(tail -1 <<<"$certs")" >&2
    bad=$((bad + 1))
    continue
  fi
  while read -r digest; do
    if [[ -n ${test_certs[$digest]:-} ]]; then
      echo "TEST-KEY ${file#"$OUT"/}: ${test_certs[$digest]}" >&2
      bad=$((bad + 1))
    fi
  done < <(sed -n 's/^Signer #[0-9]* certificate SHA-256 digest: //p' <<<"$certs")
done < <(find "$OUT"/{system,system_ext,product,vendor,odm} \
           \( -name '*.apk' -o -name '*.apex' -o -name '*.capex' \) -type f -print0 2>/dev/null)

(( checked > 0 )) || die "no APK/APEX files found under $OUT"
echo "checked $checked APK/APEX files, $bad problem(s)"
(( bad == 0 ))
