#!/usr/bin/env bash
# Provision the private Android app/OTA signing keys for this build machine.
set -Eeuo pipefail

TOOLS=$(dirname "$(readlink -f "$0")")
. "$TOOLS/local-env.sh"

die() { echo "ERROR: $*" >&2; exit 1; }

[[ ${MATON_RELEASE_KEY_DIR:-} == /* ]] || die "set MATON_RELEASE_KEY_DIR to an absolute path in matonos.local.env or the build-server environment"
KEY_DIR=$(realpath -m -- "$MATON_RELEASE_KEY_DIR")
REPO=$(git -C "$TOOLS/.." rev-parse --show-toplevel)
case "$KEY_DIR/" in
  "$REPO/"*) die "release keys must be outside the MatonOS repository" ;;
esac
mkdir -p "$KEY_DIR"
chmod 0700 "$KEY_DIR"

command -v openssl >/dev/null || die "openssl is required to provision release keys"
umask 077

provision_key() {
  local name=$1 subject=$2 private="$KEY_DIR/$1.pk8" cert="$KEY_DIR/$1.x509.pem"
  local temp
  if [[ -e $private || -e $cert ]]; then
    [[ -s $private && -s $cert ]] || die "incomplete key pair for $name; preserve and repair it manually"
    chmod 0600 "$private"
    chmod 0644 "$cert"
    return
  fi

  temp=$(mktemp -d "$KEY_DIR/.${name}.XXXXXX")
  trap 'rm -rf -- "${temp:-}"' EXIT INT TERM
  chmod 0700 "$temp"
  if ! openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
      -out "$temp/private.pem" >/dev/null 2>&1 ||
     ! openssl pkcs8 -topk8 -nocrypt -in "$temp/private.pem" -outform DER \
      -out "$temp/key.pk8" >/dev/null 2>&1 ||
     ! openssl req -new -x509 -sha256 -key "$temp/private.pem" -out "$temp/cert.pem" \
      -days 10000 -subj "$subject" >/dev/null 2>&1; then
    rm -rf -- "$temp"
    die "could not generate signing key $name"
  fi
  chmod 0600 "$temp/key.pk8"
  chmod 0644 "$temp/cert.pem"
  mv -- "$temp/key.pk8" "$private"
  mv -- "$temp/cert.pem" "$cert"
  rm -rf -- "$temp"
  trap - EXIT INT TERM
  echo "Generated missing signing key: $name"
}

# AOSP 17 generic app certs plus releasekey, the PRODUCT_DEFAULT_DEV_CERTIFICATE
# used for default APK/APEX container signing and the OTA package certificate.
# APEX payload keys are module-specific and are listed in target-files/apexkeys.txt.
provision_key releasekey '/CN=MatonOS Release/O=MatonOS'
provision_key platform '/CN=MatonOS Platform/O=MatonOS'
provision_key shared '/CN=MatonOS Shared/O=MatonOS'
provision_key media '/CN=MatonOS Media/O=MatonOS'
provision_key networkstack '/CN=MatonOS NetworkStack/O=MatonOS'
provision_key sdk_sandbox '/CN=MatonOS SDK Sandbox/O=MatonOS'
provision_key bluetooth '/CN=MatonOS Bluetooth/O=MatonOS'
provision_key nfc '/CN=MatonOS NFC/O=MatonOS'
