#!/usr/bin/env bash
# make-live.sh: build a bootable MatonOS live image (matonos-live-x86_64.img) from an AOSP
# product out dir and the staged kernel.
#
# Usage:
#   ./make-live.sh -o <aosp product out dir> [-k <bzImage>] [-b <systemd-bootx64.efi>]
#                  [-d <output image>] [-c "<extra kernel cmdline>"] [-S] [-R]
#
# -R (or MATON_RELEASE=1): release packaging, refuses permissive development mode.
# MATON_SELINUX_PERMISSIVE=1: explicit development boot profile (default enforcing).
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
# secureboot: early CPU microcode is included for every boot profile.
source "$DEVICE_DIR/secureboot/microcode.sh"
source "$DEVICE_DIR/tools/selinux-boot.sh"
PRODUCT_OUT=""
KERNEL=$DEVICE_DIR/prebuilt/bzImage
BOOTEFI=/usr/lib/systemd/boot/efi/systemd-bootx64.efi
IMAGE=""
EXTRA_CMDLINE=""
# User boot-chain decision: live and installed kernels ship only as signed UKIs.
SECURE_BOOT=1
# Four signed UKIs (live/debug/A/B) share the live ESP; initrds make these
# substantially larger than the old raw-kernel boot files.
ESP_MIB=1024
GROUP=pc_dynamic_partitions
PARTITIONS=(system system_ext product vendor odm)

while getopts "o:k:b:d:c:SRh" opt; do
  case $opt in
    o) PRODUCT_OUT=$OPTARG ;;
    k) KERNEL=$OPTARG ;;
    b) BOOTEFI=$OPTARG ;;
    d) IMAGE=$OPTARG ;;
    c) EXTRA_CMDLINE=$OPTARG ;;
    S) SECURE_BOOT=1 ;;
    R) export MATON_RELEASE=1 ;;
    *) sed -n '2,19p' "$0"; exit 1 ;;
  esac
done

maton_selinux_init || die "invalid SELinux boot profile"

maton_selinux_validate "$EXTRA_CMDLINE" || die "invalid extra boot command line"
[[ ! " $EXTRA_CMDLINE " =~ [[:space:]]androidboot\.selinux= ]] || die "use MATON_SELINUX_PERMISSIVE, not -c, to select SELinux mode"

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
if (( SECURE_BOOT )); then
  # shellcheck source=../secureboot/secureboot.sh
  source "$DEVICE_DIR/secureboot/secureboot.sh"
  # This development host only has the dual-signed 2011 shim. Keep the
  # development image buildable; release builds can require CA 2023 explicitly.
  if [[ ${MATON_REQUIRE_2023_SHIM:-0} != 1 ]]; then
    export MATON_ALLOW_2011_ONLY_SHIM=1
  fi
  sb_require_tools
  sb_ensure_dev_key
fi
for f in ramdisk.img vendor_ramdisk.img; do
  [[ -f $PRODUCT_OUT/$f ]] || die "$PRODUCT_OUT/$f missing"
done
for p in "${PARTITIONS[@]}"; do
  [[ -f $PRODUCT_OUT/$p.img ]] || die "$PRODUCT_OUT/$p.img missing"
done
for t in lpmake simg2img sgdisk mformat mmd mcopy od; do
  command -v "$t" >/dev/null || die "missing tool: $t (lpmake/simg2img come from AOSP's out/host)"
done
for t in git cpio; do command -v "$t" >/dev/null || die "missing tool: $t (CPU microcode is required in every boot image)"; done

mkdir -p "$AOSP/out/pc-logs"
work=$(mktemp -d --tmpdir="$AOSP/out/pc-logs" pc-live.XXXXXX)
trap 'rm -rf "$work"' EXIT
sb_build_microcode_cpio "$work/microcode.cpio" "$work"

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
  lp_args+=(--partition "${p}_a:readonly:$size:${GROUP}_a" --image "${p}_a=$img")
done
# A/B LP metadata (3 metadata slots) plus alignment headroom. The live image
# stores only the *_a members; the installer creates and clones slot B.
super_size=$(( total + 4 * 1048576 ))
lpmake --metadata-size 65536 --super-name super --metadata-slots 3 \
  --device "super:$super_size" --group "${GROUP}_a:$total" \
  "${lp_args[@]}" --output "$work/super.img" >/dev/null
[[ $(stat -c %s "$work/super.img") -eq $super_size ]] ||
  die "lpmake output size mismatch"
info "super: $((super_size / 1048576)) MiB"

# ---------------------------------------------------------------- ESP
esp_uuid=$(cat /proc/sys/kernel/random/uuid)
cmdline="console=ttyS0,115200 console=tty0 quiet"
# Keep the screen black until Android's boot animation: no blinking cursor, only
# real errors, and the kernel console on a VT nobody looks at (serial keeps all).
cmdline+=" loglevel=3 vt.global_cursor_default=0 fbcon=vc:2-6"
cmdline+=" firmware_class.path=/vendor/firmware"
cmdline+=" brd.rd_nr=2 brd.rd_size=8388608"
cmdline+=" androidboot.hardware=pc_x86_64"
cmdline+=" androidboot.fstab_suffix=pc_x86_64.live"
cmdline+=" androidboot.slot_suffix=_a"
# Live-image marker (ro.boot.matonos.live): shows the "Install MatonOS" entry
# and starts the install service only here, never on installed systems.
cmdline+=" androidboot.matonos.live=1"
cmdline+=" androidboot.boot_part_uuid=$esp_uuid"
cmdline+=" androidboot.selinux=$MATON_SELINUX_MODE androidboot.verifiedbootstate=orange"
[[ -n $EXTRA_CMDLINE ]] && cmdline+=" $EXTRA_CMDLINE"
debug_cmdline="loglevel=7 printk.devkmsg=on androidboot.console=ttyS0 vt.global_cursor_default=1 fbcon=vc:1-6"

info "Building ESP (${ESP_MIB} MiB)"
esp=$work/esp.img
truncate -s "${ESP_MIB}M" "$esp"
mformat -i "$esp" -F -v MATONOS ::
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/systemd ::/EFI/Linux ::/android ::/loader ::/loader/entries
# secureboot: keep source notices on the ESP; Secure Boot UKIs also embed this archive.
mmd -i "$esp" ::/EFI/BOOT/licenses
mcopy -i "$esp" "$DEVICE_DIR/secureboot/licenses/Fedora-shim-x64-BSD-3-Clause.txt" ::/EFI/BOOT/licenses/
mcopy -i "$esp" "$DEVICE_DIR/secureboot/licenses/Intel-Microcode-LICENSE.txt" ::/EFI/BOOT/licenses/
mcopy -i "$esp" "$work/amd-WHENCE.txt" ::/EFI/BOOT/licenses/
# secureboot: opt-in shim/systemd-boot assets and UKIs; default ESP stays compatible.
if (( SECURE_BOOT )); then
  sb_sign_pe "$BOOTEFI" "$work/systemd-bootx64.efi"
  sb_install_esp_assets "$work/esp-files" "$work/systemd-bootx64.efi"
  for f in EFI/BOOT/BOOTX64.EFI EFI/BOOT/mmx64.efi EFI/BOOT/grubx64.efi \
           EFI/BOOT/matonos-dev.der EFI/systemd/systemd-bootx64.efi; do
    mcopy -i "$esp" "$work/esp-files/$f" "::/$f"
  done
else
  mcopy -i "$esp" "$BOOTEFI" ::/EFI/BOOT/BOOTX64.EFI
  mcopy -i "$esp" "$BOOTEFI" ::/EFI/systemd/systemd-bootx64.efi
fi
if (( ! SECURE_BOOT )); then
  mcopy -i "$esp" "$work/microcode.cpio" ::/android/microcode.cpio
  mcopy -i "$esp" "$KERNEL" ::/android/bzImage
  mcopy -i "$esp" "$PRODUCT_OUT/vendor_ramdisk.img" ::/android/vendor_ramdisk.img
  mcopy -i "$esp" "$PRODUCT_OUT/ramdisk.img" ::/android/ramdisk.img
fi

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
selinux_title=""
[[ $MATON_SELINUX_MODE == permissive ]] && selinux_title=" (DEVELOPMENT: SELinux permissive)"
# Debug adds logging and the serial root console; it does not change SELinux.
for entry in live debug; do
  if [[ $entry == live ]]; then title="MatonOS Live$selinux_title"; opts=$cmdline
  else title="MatonOS Live (debug)$selinux_title"; opts="$cmdline $debug_cmdline"; fi
  if (( SECURE_BOOT )); then
    uki=matonos-live.efi
    [[ $entry == debug ]] && uki=matonos-live-debug.efi
    cat > "$work/matonos-$entry.conf" <<EOF
title   $title
efi     /EFI/Linux/$uki
EOF
  else
    cat > "$work/matonos-$entry.conf" <<EOF
title   $title
linux   /android/bzImage
initrd  /android/microcode.cpio
initrd  /android/vendor_ramdisk.img
initrd  /android/ramdisk.img
initrd  /android/ventoy-initrd.img
options $opts
EOF
  fi
mcopy -i "$esp" "$work/matonos-$entry.conf" ::/loader/entries/
done
if (( SECURE_BOOT )); then
  cat > "$work/matonos.sbat" <<'EOF'
sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md
matonos,1,MatonOS,matonos,1,https://matonos.org/
EOF
  sb_build_uki "$work/matonos-live.efi" "$KERNEL" "$cmdline" "$work/matonos.sbat" "$work" \
    "$work/microcode.cpio" "$PRODUCT_OUT/vendor_ramdisk.img" "$PRODUCT_OUT/ramdisk.img" "$work/ventoy-initrd.img"
  sb_build_uki "$work/matonos-live-debug.efi" "$KERNEL" "$cmdline $debug_cmdline" "$work/matonos.sbat" "$work" \
    "$work/microcode.cpio" "$PRODUCT_OUT/vendor_ramdisk.img" "$PRODUCT_OUT/ramdisk.img" "$work/ventoy-initrd.img"
  # Installed UKIs always enforce, including on permissive development media.
  installed_a="console=ttyS0,115200 console=tty0 quiet loglevel=3 vt.global_cursor_default=0 fbcon=vc:2-6 firmware_class.path=/vendor/firmware brd.rd_nr=2 brd.rd_size=8388608 androidboot.hardware=pc_x86_64 androidboot.fstab_suffix=pc_x86_64 androidboot.slot_suffix=_a androidboot.matonos.live=0 androidboot.selinux=enforcing androidboot.verifiedbootstate=orange"
  installed_b=${installed_a/_a /_b }
  sb_build_uki "$work/matonos-installed-a.efi" "$KERNEL" "$installed_a" "$work/matonos.sbat" "$work" \
    "$work/microcode.cpio" "$PRODUCT_OUT/vendor_ramdisk.img" "$PRODUCT_OUT/ramdisk.img"
  sb_build_uki "$work/matonos-installed-b.efi" "$KERNEL" "$installed_b" "$work/matonos.sbat" "$work" \
    "$work/microcode.cpio" "$PRODUCT_OUT/vendor_ramdisk.img" "$PRODUCT_OUT/ramdisk.img"
  mcopy -i "$esp" "$work/matonos-live.efi" ::/EFI/Linux/matonos-live.efi
  mcopy -i "$esp" "$work/matonos-live-debug.efi" ::/EFI/Linux/matonos-live-debug.efi
  mcopy -i "$esp" "$work/matonos-installed-a.efi" ::/EFI/Linux/matonos-installed-a.efi
  mcopy -i "$esp" "$work/matonos-installed-b.efi" ::/EFI/Linux/matonos-installed-b.efi
else
  mcopy -i "$esp" "$work/ventoy-initrd.img" ::/android/ventoy-initrd.img
fi
mcopy -i "$esp" "$work/loader.conf" ::/loader/loader.conf

# ---------------------------------------------------------------- disk
info "Assembling $IMAGE"
esp_bytes=$((ESP_MIB * 1048576))
start=1048576                                  # 1 MiB alignment
disk_bytes=$(( start + esp_bytes + super_size + 1048576 ))   # + backup GPT
if [[ -e $IMAGE && ! -f $IMAGE ]]; then die "output image exists and is not a regular file: $IMAGE"; fi
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
