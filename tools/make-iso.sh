#!/usr/bin/env bash
# make-iso.sh: wrap a MatonOS live image (from make-live.sh) in a UEFI-bootable ISO.
#
# Usage: ./make-iso.sh [-i <live image>] [-d <output iso>]
#
# Layout:
#   El Torito EFI image  FAT copy of the live ESP minus the installer-only UKIs
#                        (matonos-installed-*.efi). Firmware boots systemd-boot
#                        from it with the same signed live UKIs.
#   /matonos-live-x86_64.img  the unchanged live disk image.
# Optical drives expose no partitions, so the boot PARTUUID never appears; the
# ventoyboot pre-init then mounts sr* as iso9660, finds the image by its
# embedded ESP PARTUUID and loop-attaches it (same path as Ventoy).
# -boot-load-size 1: the El Torito count is 16-bit and the EFI image is >32 MiB;
# UEFI (EDK2) reads a count below 2 as "to the end of the volume".
# For USB sticks, dd the .img itself, not this ISO.
set -Eeuo pipefail

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
IMAGE=$AOSP/out/target/product/pc_x86_64/matonos-live-x86_64.img
ISO=""
while getopts "i:d:" opt; do
  case $opt in
    i) IMAGE=$OPTARG ;;
    d) ISO=$OPTARG ;;
    *) die "usage: $0 [-i <live image>] [-d <output iso>]" ;;
  esac
done
[[ -f $IMAGE ]] || die "live image not found: $IMAGE (run make-live.sh first)"
ISO=${ISO:-${IMAGE%.img}.iso}
for t in sgdisk mcopy mformat genisoimage; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done

work=$(mktemp -d "${TMPDIR:-/tmp}/matonos-iso.XXXXXX")
trap 'rm -rf "$work"' EXIT

esp_start=$(sgdisk -i 1 "$IMAGE" | awk '/^First sector/{print $3}')
[[ $esp_start =~ ^[0-9]+$ ]] || die "could not read ESP start from $IMAGE"
esp="$IMAGE@@$((esp_start * 512))"

info "extracting live ESP"
mkdir -p "$work/esp"
mcopy -s -n -i "$esp" '::/*' "$work/esp/"
[[ -d $work/esp/EFI/BOOT ]] || die "ESP of $IMAGE has no EFI/BOOT"
find "$work/esp" -iname 'matonos-installed-*.efi' -delete

# FAT32 needs at least ~33 MiB; add 10% + 16 MiB headroom for FAT metadata.
kib=$(du -sk "$work/esp" | cut -f1)
size_mib=$(( (kib * 11 / 10) / 1024 + 16 ))
(( size_mib < 64 )) && size_mib=64
info "building ${size_mib} MiB El Torito EFI image"
mkdir -p "$work/iso"
efi="$work/iso/efiboot.img"
truncate -s "${size_mib}M" "$efi"
mformat -i "$efi" -F -v MATONOS ::
mcopy -s -i "$efi" "$work/esp/"* ::/

info "writing $ISO"
genisoimage -quiet -o "$ISO" -V MATONOS -J -R -iso-level 3 \
  -e efiboot.img -no-emul-boot -boot-load-size 1 -hide efiboot.img -hide-joliet efiboot.img \
  -graft-points "/=$work/iso" "/$(basename "$IMAGE")=$IMAGE"
info "done: $ISO ($(du -h "$ISO" | cut -f1))"
