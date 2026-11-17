#!/usr/bin/env bash
# make-payload.sh — package an AOSP PC build into an installer payload.
#
# Usage:
#   ./make-payload.sh -o <aosp product out dir> -k <bzImage> [-b <systemd-bootx64.efi>]
#                     [-c "<kernel cmdline>"] [-d <payload dir>] [-S] [-R]
#
# -R (or MATON_RELEASE=1): release packaging, refuses permissive development mode.
# MATON_SELINUX_PERMISSIVE=1: explicit development boot profile (default enforcing).
#
# Example:
#   ./make-payload.sh -o ~/aosp/out/target/product/pc_x86_64 \
#                     -k ~/linux/arch/x86/boot/bzImage
#
# Produces <payload dir>/ containing:
#   bzImage, ramdisk.img, [vendor_ramdisk.img], system_a.erofs.zst,
#   system_ext_a.erofs.zst, product_a.erofs.zst, vendor_a.erofs.zst, odm_a.erofs.zst,
#   efi/systemd-bootx64.efi, payload.conf, SHA256SUMS
set -Eeuo pipefail

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
# secureboot: pinned AMD/Intel microcode is an always-on early-initrd input.
source "$DEVICE_DIR/secureboot/microcode.sh"
source "$DEVICE_DIR/tools/selinux-boot.sh"

OUT=""
KERNEL=""
BOOTEFI="/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
CMDLINE="console=tty0 audit_backlog_limit=8192 quiet loglevel=3 vt.global_cursor_default=0 fbcon=vc:2-6 firmware_class.path=/vendor/firmware androidboot.hardware=pc_x86_64 androidboot.verifiedbootstate=orange"
PAYLOAD="$PWD/payload"
SECURE_BOOT=0

while getopts "o:k:b:c:d:SRh" opt; do
  case $opt in
    o) OUT=$OPTARG ;;
    k) KERNEL=$OPTARG ;;
    b) BOOTEFI=$OPTARG ;;
    c) CMDLINE=$OPTARG ;;
    d) PAYLOAD=$OPTARG ;;
    S) SECURE_BOOT=1 ;;
    R) export MATON_RELEASE=1 ;;
    *) sed -n '2,15p' "$0"; exit 1 ;;
  esac
done

maton_selinux_init || die "invalid SELinux boot profile"

CMDLINE=$(maton_selinux_cmdline "$CMDLINE") || die "invalid payload boot command line"

[[ -d $OUT ]]     || die "product out dir not found (-o)"
[[ -f $KERNEL ]]  || die "kernel bzImage not found (-k)"
[[ -f $BOOTEFI ]] || die "systemd-boot EFI binary not found (-b); on Debian/Ubuntu install systemd-boot-efi"
for image in system system_ext product vendor odm; do
  [[ -f $OUT/$image.img ]] || die "$OUT/$image.img missing"
done
[[ -f $OUT/ramdisk.img ]] || die "$OUT/ramdisk.img missing"
for t in zstd sha256sum od git cpio; do command -v "$t" >/dev/null || die "missing tool: $t"; done
if (( SECURE_BOOT )); then
  # shellcheck source=../secureboot/secureboot.sh
  source "$DEVICE_DIR/secureboot/secureboot.sh"
  sb_require_tools
  sb_ensure_dev_key
fi

[[ $PAYLOAD == /* ]] || die "payload destination must be an absolute path: $PAYLOAD"
PAYLOAD=$(realpath -m -- "$PAYLOAD")
[[ $PAYLOAD != / ]] || die "payload destination must not be /"
if [[ -e $PAYLOAD && ( -b $PAYLOAD || -c $PAYLOAD || -p $PAYLOAD || -S $PAYLOAD ) ]]; then
  die "payload destination must not be a device or special file: $PAYLOAD"
fi
rm -rf -- "$PAYLOAD"
mkdir -p "$PAYLOAD/efi"
microcode_work=$(mktemp -d --tmpdir pc-microcode.XXXXXX)
trap 'rm -rf "$microcode_work"' EXIT
sb_build_microcode_cpio "$PAYLOAD/microcode.cpio" "$microcode_work"
mkdir -p "$PAYLOAD/licenses"
cp "$DEVICE_DIR/secureboot/licenses/shim-x64-BSD-3-Clause.txt" "$PAYLOAD/licenses/"
cp "$DEVICE_DIR/secureboot/licenses/Intel-Microcode-LICENSE.txt" "$PAYLOAD/licenses/"
cp "$microcode_work/amd-WHENCE.txt" "$PAYLOAD/licenses/AMD-WHENCE.txt"

info "Copying kernel, ramdisks and bootloader"
# secureboot: emit signed chain/UKI and only the public MOK certificate in the payload.
if (( SECURE_BOOT )); then
  sb_sign_pe "$BOOTEFI" "$PAYLOAD/efi/systemd-bootx64.efi"
  sb_install_esp_assets "$PAYLOAD/efi" "$PAYLOAD/efi/systemd-bootx64.efi"
  mkdir -p "$PAYLOAD/efi/EFI/Linux"
  cat > "$PAYLOAD/matonos.sbat" <<'EOF'
sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md
matonos,1,MatonOS,matonos,1,https://matonos.org/
EOF
  initrds=()
  [[ -f $OUT/vendor_ramdisk.img ]] && initrds+=("$OUT/vendor_ramdisk.img")
  initrds=("$PAYLOAD/microcode.cpio" "${initrds[@]}" "$OUT/ramdisk.img")
  sb_build_uki "$PAYLOAD/efi/EFI/Linux/matonos.efi" "$KERNEL" "" "$PAYLOAD/matonos.sbat" "$PAYLOAD" "${initrds[@]}"
  cp "$SB_KEY_DIR/matonos-dev.der" "$PAYLOAD/efi/EFI/BOOT/matonos-dev.der"
  rm -f "$PAYLOAD/matonos.sbat"
else
  cp "$KERNEL" "$PAYLOAD/bzImage"
  cp "$OUT/ramdisk.img" "$PAYLOAD/ramdisk.img"
  [[ -f $OUT/vendor_ramdisk.img ]] && cp "$OUT/vendor_ramdisk.img" "$PAYLOAD/vendor_ramdisk.img"
  cp "$BOOTEFI" "$PAYLOAD/efi/systemd-bootx64.efi"
fi

# Package the five built logical-partition images individually. AOSP may emit
# sparse Android images; libfiemap on the target must receive raw EROFS bytes.
image_work=$(mktemp -d --tmpdir pc-images.XXXXXX)
trap 'rm -rf "$microcode_work" "$image_work"' EXIT
for partition in system system_ext product vendor odm; do
  input="$OUT/$partition.img"
  raw="$image_work/$partition.raw"
  magic=$(od -An -tx1 -N4 "$input" | tr -d ' \n')
  if [[ $magic == "3aff26ed" ]]; then
    command -v simg2img >/dev/null || die "$input is sparse; put AOSP's out/host/linux-x86/bin on PATH for simg2img"
    simg2img "$input" "$raw"
  else
    cp "$input" "$raw"
  fi
  info "Compressing $partition EROFS image"
  zstd -19 -T0 -q "$raw" -o "$PAYLOAD/${partition}_a.erofs.zst"
done

cat > "$PAYLOAD/payload.conf" <<EOF
# Generated by make-payload.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)
ESP_SIZE_MIB=2048
MIN_USERDATA_MIB=8192
KERNEL_CMDLINE="$CMDLINE"
DEBUG_CMDLINE="loglevel=7 printk.devkmsg=on vt.global_cursor_default=1 fbcon=vc:1-6"
SECURE_BOOT=$SECURE_BOOT
EOF

info "Writing SHA256SUMS"
sums=$( cd "$PAYLOAD" && find . -type f -printf '%P\n' | sort | xargs sha256sum )
printf '%s\n' "$sums" > "$PAYLOAD/SHA256SUMS"

info "Payload ready in $PAYLOAD:"
ls -lh "$PAYLOAD" "$PAYLOAD/efi"
