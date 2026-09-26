#!/usr/bin/env bash
# make-live.sh: build a bootable MatonOS live image (matonos-live-x86_64.img) from an AOSP
# product out dir and the staged kernel.
#
# Usage:
#   ./make-live.sh -o <aosp product out dir> [-k <bzImage>] [-b <systemd-bootx64.efi>]
#                  [-d <output image>] [-c "<extra kernel cmdline>"]
#
# Layout (GPT, write to a USB stick with dd):
#   1 esp    FAT32, systemd-boot + bzImage + ramdisks + loader entries
#   2 super  compact dynamic-partition image, repacked with lpmake so it's
#            only as big as system+system_ext+product+vendor
# /data and /metadata live in RAM (brd, fstab.pc_x86_64.live): nothing
# persists. The boot disk is found through androidboot.boot_part_uuid (the
# ESP's partition UUID, random per image), so it works on any port.
#
# Needs no root: the ESP is filled with mtools and the disk is assembled with
# sgdisk + dd into a regular file.
set -Eeuo pipefail

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
PRODUCT_OUT=""
KERNEL=$DEVICE_DIR/prebuilt/bzImage
BOOTEFI=/usr/lib/systemd/boot/efi/systemd-bootx64.efi
IMAGE=""
EXTRA_CMDLINE=""
ESP_MIB=128
GROUP=pc_dynamic_partitions
PARTITIONS=(system system_ext product vendor odm)

while getopts "o:k:b:d:c:h" opt; do
  case $opt in
    o) PRODUCT_OUT=$OPTARG ;;
    k) KERNEL=$OPTARG ;;
    b) BOOTEFI=$OPTARG ;;
    d) IMAGE=$OPTARG ;;
    c) EXTRA_CMDLINE=$OPTARG ;;
    *) sed -n '2,19p' "$0"; exit 1 ;;
  esac
done

PRODUCT_OUT=${PRODUCT_OUT:-$AOSP/out/target/product/pc_x86_64}
IMAGE=${IMAGE:-$PRODUCT_OUT/matonos-live-x86_64.img}
HOSTBIN=$AOSP/out/host/linux-x86/bin
# AOSP host tools last: we only need lpmake/simg2img from there, and its
# cut-down sgdisk must not shadow the system one. lunch puts HOSTBIN first,
# so drop it from wherever it is before appending it.
path_no_host=$(tr ':' '\n' <<<"$PATH" | grep -vxF "$HOSTBIN" | paste -sd:)
PATH=$path_no_host:/sbin:/usr/sbin:$HOSTBIN

# ---------------------------------------------------------------- preflight
[[ -d $PRODUCT_OUT ]] || die "product out dir not found (-o)"
[[ -f $KERNEL ]]      || die "kernel not found: $KERNEL (run build-kernel.sh or pass -k)"
[[ -f $BOOTEFI ]]     || die "systemd-boot EFI binary not found (-b); install systemd-boot-efi"
for f in ramdisk.img vendor_ramdisk.img; do
  [[ -f $PRODUCT_OUT/$f ]] || die "$PRODUCT_OUT/$f missing"
done
for p in "${PARTITIONS[@]}"; do
  [[ -f $PRODUCT_OUT/$p.img ]] || die "$PRODUCT_OUT/$p.img missing"
done
for t in lpmake simg2img sgdisk mformat mmd mcopy od; do
  command -v "$t" >/dev/null || die "missing tool: $t (lpmake/simg2img come from AOSP's out/host)"
done

work=$(mktemp -d --tmpdir pc-live.XXXXXX)
trap 'rm -rf "$work"' EXIT

# ---------------------------------------------------------------- super
mib() { echo $(( ($1 + 1048575) / 1048576 * 1048576 )); }

info "Repacking compact super"
lp_args=()
total=0
for p in "${PARTITIONS[@]}"; do
  img=$PRODUCT_OUT/$p.img
  if [[ $(od -An -tx1 -N4 "$img" | tr -d ' \n') == "3aff26ed" ]]; then
    simg2img "$img" "$work/$p.img"
    img=$work/$p.img
  fi
  size=$(mib "$(stat -c %s "$img")")
  total=$((total + size))
  lp_args+=(--partition "$p:readonly:$size:$GROUP" --image "$p=$img")
done
# LP metadata (2 slots x 64 KiB, primary + backup) + alignment headroom.
super_size=$(( total + 4 * 1048576 ))
lpmake --metadata-size 65536 --super-name super --metadata-slots 2 \
  --device "super:$super_size" --group "$GROUP:$total" \
  "${lp_args[@]}" --output "$work/super.img" >/dev/null
[[ $(stat -c %s "$work/super.img") -eq $super_size ]] ||
  die "lpmake output size mismatch"
info "super: $((super_size / 1048576)) MiB"

# ---------------------------------------------------------------- ESP
esp_uuid=$(cat /proc/sys/kernel/random/uuid)
cmdline="console=ttyS0,115200 console=tty0 quiet"
cmdline+=" firmware_class.path=/vendor/firmware"
cmdline+=" brd.rd_nr=2 brd.rd_size=8388608"
cmdline+=" androidboot.hardware=pc_x86_64"
cmdline+=" androidboot.fstab_suffix=pc_x86_64.live"
cmdline+=" androidboot.boot_part_uuid=$esp_uuid"
cmdline+=" androidboot.selinux=permissive androidboot.verifiedbootstate=orange"
[[ -n $EXTRA_CMDLINE ]] && cmdline+=" $EXTRA_CMDLINE"
debug_cmdline="loglevel=7 printk.devkmsg=on androidboot.console=ttyS0"

info "Building ESP (${ESP_MIB} MiB)"
esp=$work/esp.img
truncate -s "${ESP_MIB}M" "$esp"
mformat -i "$esp" -F -v MATONOS ::
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/systemd ::/android ::/loader ::/loader/entries
mcopy -i "$esp" "$BOOTEFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$esp" "$BOOTEFI" ::/EFI/systemd/systemd-bootx64.efi
mcopy -i "$esp" "$KERNEL" ::/android/bzImage
mcopy -i "$esp" "$PRODUCT_OUT/vendor_ramdisk.img" ::/android/vendor_ramdisk.img
mcopy -i "$esp" "$PRODUCT_OUT/ramdisk.img" ::/android/ramdisk.img

# The final initrd overlays a tiny pre-init shim on top of the two stock
# ramdisks. It immediately execs the saved Android /init on normal boots.
stage="$work/ventoy-initrd"
mkdir -p "$stage"
gzip -dc "$PRODUCT_OUT/ramdisk.img" | cpio -i --quiet --to-stdout init > "$stage/init.android"
[[ -s $stage/init.android ]] || die "could not extract Android /init from ramdisk.img"
mkdir -p "$stage/ventoy/modules"
cp "$DEVICE_DIR/prebuilt/modules/nls_utf8.ko" "$DEVICE_DIR/prebuilt/modules/exfat.ko" \
  "$DEVICE_DIR/prebuilt/modules/ntfs3.ko" "$stage/ventoy/modules/"
bash "$DEVICE_DIR/ventoyboot/build.sh" "$stage/init" \
  || die "failed to build static NDK Ventoy pre-init"
chmod 0755 "$stage/init" "$stage/init.android"
(cd "$stage" && find . -print0 | cpio --null -o -H newc --quiet | gzip -9 > "$work/ventoy-initrd.img")

cat > "$work/loader.conf" <<EOF
default matonos-live.conf
timeout 3
console-mode keep
editor no
EOF
# Vendor ramdisk first, then the generic ramdisk (boot image v4 order).
for entry in live debug; do
  if [[ $entry == live ]]; then title="MatonOS Live"; opts=$cmdline
  else title="MatonOS Live (debug)"; opts="$cmdline $debug_cmdline"; fi
  cat > "$work/matonos-$entry.conf" <<EOF
title   $title
linux   /android/bzImage
initrd  /android/vendor_ramdisk.img
initrd  /android/ramdisk.img
initrd  /android/ventoy-initrd.img
options $opts
EOF
mcopy -i "$esp" "$work/matonos-$entry.conf" ::/loader/entries/
done
mcopy -i "$esp" "$work/ventoy-initrd.img" ::/android/ventoy-initrd.img
mcopy -i "$esp" "$work/loader.conf" ::/loader/loader.conf

# ---------------------------------------------------------------- disk
info "Assembling $IMAGE"
esp_bytes=$((ESP_MIB * 1048576))
start=1048576                                  # 1 MiB alignment
disk_bytes=$(( start + esp_bytes + super_size + 1048576 ))   # + backup GPT
rm -f "$IMAGE"
truncate -s "$disk_bytes" "$IMAGE"
esp_first=$((start / 512))
super_first=$(( (start + esp_bytes) / 512 ))
esp_last=$(( esp_first + esp_bytes / 512 - 1 ))
super_last=$(( super_first + super_size / 512 - 1 ))
sgdisk -a 2048 \
  -n "1:$esp_first:$esp_last" -t 1:EF00 -c 1:esp -u "1:$esp_uuid" \
  -n "2:$super_first:$super_last" -t 2:8300 -c 2:super \
  "$IMAGE" >/dev/null
dd if="$esp" of="$IMAGE" bs=1M seek=$((start / 1048576)) conv=notrunc,sparse status=none
dd if="$work/super.img" of="$IMAGE" bs=1M seek=$(( (start + esp_bytes) / 1048576 )) conv=notrunc,sparse status=none
sgdisk -v "$IMAGE" >/dev/null || die "GPT verification failed"

info "Live image ready: $IMAGE ($((disk_bytes / 1048576)) MiB, ESP PARTUUID $esp_uuid)"
echo "    Write to a USB stick with: sudo dd if=$IMAGE of=/dev/sdX bs=4M conv=fsync status=progress"
