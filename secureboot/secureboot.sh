#!/usr/bin/env bash
# Shared host-side helpers for MatonOS Secure Boot image creation.
set -Eeuo pipefail

SECUREBOOT_DIR=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
DEVICE_DIR=$(dirname "$SECUREBOOT_DIR")
SB_KEY_DIR=${MATON_SECUREBOOT_KEY_DIR:-$HOME/.config/matonos/secureboot-dev}
SB_SHIM=${MATON_SHIM_EFI:-/usr/lib/shim/shimx64.efi.signed.latest}
SB_MOK_MANAGER=${MATON_MOK_MANAGER_EFI:-/usr/lib/shim/mmx64.efi}
SB_SYSTEMD_STUB=${MATON_SYSTEMD_STUB:-/usr/lib/systemd/boot/efi/linuxx64.efi.stub}

sb_die() { echo "ERROR: $*" >&2; exit 1; }

sb_require_tools() {
  local tool shim_sig
  for tool in openssl sbsign sbverify ukify objdump; do
    command -v "$tool" >/dev/null || sb_die "Secure Boot needs $tool (install sbsigntool and systemd-ukify)"
  done
  [[ -f $SB_SHIM ]] || sb_die "Microsoft-signed shim missing: $SB_SHIM (set MATON_SHIM_EFI)"
  [[ -f $SB_MOK_MANAGER ]] || sb_die "MokManager missing: $SB_MOK_MANAGER (set MATON_MOK_MANAGER_EFI)"
  [[ -f $SB_SYSTEMD_STUB ]] || sb_die "systemd UKI stub missing: $SB_SYSTEMD_STUB (set MATON_SYSTEMD_STUB)"
  shim_sig=$(sbverify --list "$SB_SHIM" 2>&1) || sb_die "shim is not a signed PE/COFF image: $SB_SHIM"
  if [[ ${MATON_ALLOW_2011_ONLY_SHIM:-0} != 1 && $shim_sig != *"Microsoft UEFI CA 2023"* ]]; then
    sb_die "shim lacks a Microsoft UEFI CA 2023 signature; use a current dual-signed shim (or set MATON_ALLOW_2011_ONLY_SHIM=1 for legacy-only testing)"
  fi
  objdump -h "$SB_SHIM" 2>/dev/null | grep -q '[.]sbat' || sb_die "shim has no SBAT section: $SB_SHIM"
}

sb_ensure_dev_key() (
  umask 077
  mkdir -p "$SB_KEY_DIR"
  chmod 0700 "$SB_KEY_DIR"
  local key=$SB_KEY_DIR/matonos-dev.key cert=$SB_KEY_DIR/matonos-dev.pem
  if [[ ! -e $key && ! -e $cert ]]; then
    echo "==> Generating DEVELOPMENT Secure Boot key at $SB_KEY_DIR (keep this directory off image media)" >&2
    openssl req -new -x509 -newkey rsa:3072 -sha256 -days 3650 -nodes \
      -subj "/CN=MatonOS Development Secure Boot Key/" \
      -addext "keyUsage=critical,digitalSignature" \
      -addext "extendedKeyUsage=codeSigning" \
      -keyout "$key" -out "$cert"
    openssl x509 -in "$cert" -outform DER -out "$SB_KEY_DIR/matonos-dev.der"
    { cat "$key"; printf '\n'; cat "$cert"; } > "$SB_KEY_DIR/kernel-signing-key.pem"
    chmod 0600 "$key" "$SB_KEY_DIR/kernel-signing-key.pem"
    chmod 0644 "$cert" "$SB_KEY_DIR/matonos-dev.der"
  elif [[ ! -s $key || ! -s $cert || ! -s $SB_KEY_DIR/matonos-dev.der || ! -s $SB_KEY_DIR/kernel-signing-key.pem ]]; then
    sb_die "incomplete Secure Boot key set under $SB_KEY_DIR; restore it or remove it and regenerate the DEV key"
  fi
  [[ $(stat -c %a "$key") == 600 ]] || chmod 0600 "$key"
)

sb_sign_pe() {
  local input=$1 output=$2
  sbsign --key "$SB_KEY_DIR/matonos-dev.key" --cert "$SB_KEY_DIR/matonos-dev.pem" \
    --output "$output" "$input" >/dev/null
  sbverify --cert "$SB_KEY_DIR/matonos-dev.pem" "$output" >/dev/null
}

sb_build_uki() {
  local output=$1 kernel=$2 cmdline=$3 sbat_file=$4 work=$5
  shift 5
  local initrd
  local -a args=(build --stub "$SB_SYSTEMD_STUB" --linux "$kernel" --os-release=/etc/os-release \
    --sbat "@$sbat_file" --output "$work/unsigned.efi")
  if [[ -n $cmdline ]]; then
    printf '%s\n' "$cmdline" > "$work/cmdline"
    args+=(--cmdline "@$work/cmdline")
  fi
  for initrd in "$@"; do args+=(--initrd "$initrd"); done
  ukify "${args[@]}"
  sb_sign_pe "$work/unsigned.efi" "$output"
  rm -f "$work/unsigned.efi" "$work/cmdline"
}

sb_install_esp_assets() {
  local esp_dir=$1 bootefi=$2
  mkdir -p "$esp_dir/EFI/BOOT" "$esp_dir/EFI/systemd" "$esp_dir/EFI/Linux"
  # shim's x86_64 second-stage filename is fixed to grubx64.efi. systemd-boot
  # already carries its upstream .sbat metadata and is signed with the MOK key.
  cp "$SB_SHIM" "$esp_dir/EFI/BOOT/BOOTX64.EFI"
  cp "$SB_MOK_MANAGER" "$esp_dir/EFI/BOOT/mmx64.efi"
  cp "$bootefi" "$esp_dir/EFI/BOOT/grubx64.efi"
  cp "$bootefi" "$esp_dir/EFI/systemd/systemd-bootx64.efi"
  cp "$SB_KEY_DIR/matonos-dev.der" "$esp_dir/EFI/BOOT/matonos-dev.der"
}
