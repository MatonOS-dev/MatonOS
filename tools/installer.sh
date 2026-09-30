#!/usr/bin/env bash
# installer.sh — wipe a disk and install Android (x86_64, UEFI).
#
# Expects a payload directory produced by make-payload.sh, by default
# ./payload next to this script (override with PAYLOAD_DIR=...).
#
# Needs: bash, util-linux (lsblk, wipefs, blockdev, findmnt, mountpoint),
#        gptfdisk (sgdisk), dosfstools (mkfs.vfat), zstd, coreutils,
#        udev (udevadm) and parted (partprobe); efibootmgr optional.
#
# Optional env: PAYLOAD_DIR, LABEL (UEFI entry name), VERIFY=0 to skip
#               read-back verification, DISCARD=1 to TRIM the whole disk first.
set -Eeuo pipefail

PAYLOAD_DIR="${PAYLOAD_DIR:-$(dirname "$(readlink -f "$0")")/payload}"
LABEL="${LABEL:-Android}"
VERIFY="${VERIFY:-1}"
DISCARD="${DISCARD:-0}"

MNT=$(mktemp -d)
cleanup() {
  mountpoint -q "$MNT" 2>/dev/null && umount "$MNT"
  rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT
trap 'echo; echo "Installation FAILED at line $LINENO. The target disk is in an unknown state." >&2' ERR

die()  { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARNING: $*" >&2; }
info() { echo "==> $*"; }
field() { lsblk -dno "$1" "$2" 2>/dev/null | sed 's/^ *//; s/ *$//'; }

# Partition node for disk + number (sda -> sda1, nvme0n1 -> nvme0n1p1).
part() { if [[ $1 =~ [0-9]$ ]]; then echo "${1}p$2"; else echo "${1}$2"; fi; }

wait_for_node() {
  local i
  for i in $(seq 1 50); do [[ -b $1 ]] && return 0; sleep 0.2; done
  die "partition node $1 did not appear"
}

# ---------------------------------------------------------------- preflight
[[ $EUID -eq 0 ]]           || die "run as root"
[[ -d /sys/firmware/efi ]]  || die "this machine was not booted in UEFI mode"
for t in lsblk sgdisk wipefs blockdev findmnt mkfs.vfat zstd dd partprobe udevadm sha256sum; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done
[[ -f $PAYLOAD_DIR/payload.conf ]] || die "no payload found in $PAYLOAD_DIR"
# Parse only the documented scalar fields; never execute payload-controlled shell.
declare -A conf=()
while IFS= read -r line || [[ -n $line ]]; do
  [[ $line =~ ^([A-Z_]+)=(.*)$ ]] || continue
  key=${BASH_REMATCH[1]} value=${BASH_REMATCH[2]}
  value=${value%\"}; value=${value#\"}
  [[ $value != *$'\n'* && $value != *$'\r'* ]] || die "invalid payload config value: $key"
  case $key in SUPER_SIZE_BYTES|SUPER_SHA256|ESP_SIZE_MIB|MIN_USERDATA_MIB|KERNEL_CMDLINE|DEBUG_CMDLINE|SECURE_BOOT) conf[$key]=$value ;; esac
done < "$PAYLOAD_DIR/payload.conf"
SUPER_SIZE_BYTES=${conf[SUPER_SIZE_BYTES]:-} SUPER_SHA256=${conf[SUPER_SHA256]:-}
ESP_SIZE_MIB=${conf[ESP_SIZE_MIB]:-} MIN_USERDATA_MIB=${conf[MIN_USERDATA_MIB]:-}
KERNEL_CMDLINE=${conf[KERNEL_CMDLINE]:-} DEBUG_CMDLINE=${conf[DEBUG_CMDLINE]:-}
SECURE_BOOT=${conf[SECURE_BOOT]:-0}
: "${SUPER_SIZE_BYTES:?} ${SUPER_SHA256:?} ${ESP_SIZE_MIB:?} ${MIN_USERDATA_MIB:?} ${KERNEL_CMDLINE:?}"
[[ $SUPER_SIZE_BYTES =~ ^[0-9]+$ && $ESP_SIZE_MIB =~ ^[0-9]+$ && $MIN_USERDATA_MIB =~ ^[0-9]+$ && $SUPER_SHA256 =~ ^[a-fA-F0-9]{64}$ && $SECURE_BOOT =~ ^[01]$ ]] || die "payload config has invalid numeric or hash fields"
# secureboot: CPU microcode is a required first-initrd asset in every installer profile.
for f in microcode.cpio licenses/Fedora-shim-x64-BSD-3-Clause.txt \
         licenses/Intel-Microcode-LICENSE.txt licenses/AMD-WHENCE.txt; do
  [[ -s $PAYLOAD_DIR/$f ]] || die "payload is missing required CPU microcode asset ($f)"
done
if [[ ${SECURE_BOOT:-0} == 1 ]]; then
  for f in efi/EFI/BOOT/BOOTX64.EFI efi/EFI/BOOT/grubx64.efi efi/EFI/BOOT/mmx64.efi \
           efi/EFI/BOOT/matonos-dev.der efi/systemd-bootx64.efi efi/EFI/Linux/matonos.efi; do
    [[ -s $PAYLOAD_DIR/$f ]] || die "Secure Boot payload is incomplete ($f missing)"
  done
fi

info "Checking payload integrity"
( cd "$PAYLOAD_DIR" && sha256sum -c --quiet SHA256SUMS ) || die "payload is corrupt (checksum mismatch)"

# Work out which disk we booted from so we never offer it as a target.
boot_disk=""
src=$(findmnt -no SOURCE --target "$PAYLOAD_DIR" 2>/dev/null || true)
# Live images expose the ESP PARTUUID in the kernel command line, even when
# the payload directory is on tmpfs, overlayfs, or a device-mapper root.
boot_uuid=$(sed -n 's/.*androidboot\.boot_part_uuid=\([^ ]*\).*/\1/p' /proc/cmdline)
if [[ -n $boot_uuid ]]; then
  while read -r part_uuid node parent; do
    if [[ ${part_uuid,,} == "${boot_uuid,,}" ]]; then
      boot_disk=${parent:+/dev/$parent}
      [[ -n $boot_disk ]] || boot_disk=$node
      break
    fi
  done < <(lsblk -nrpo PARTUUID,NAME,PKNAME)
fi
if [[ -z $boot_disk && -b $src ]]; then
  pk=$(lsblk -no PKNAME "$src" 2>/dev/null | head -n1)
  boot_disk="/dev/${pk:-$(basename "$src")}"
fi

# ---------------------------------------------------------------- pick disk
mapfile -t disks < <(
  lsblk -dnpo NAME,TYPE,RO | awk '$2=="disk" && $3=="0" {print $1}' |
  grep -Ev '^/dev/(zram|loop|ram|sr|mmcblk[0-9]+boot)' |
  { if [[ -n $boot_disk ]]; then grep -vx "$boot_disk"; else cat; fi; } || true
)
(( ${#disks[@]} > 0 )) || die "no suitable target disks found"

echo
echo "Android installer — available disks:"
echo
for i in "${!disks[@]}"; do
  d=${disks[$i]}
  printf "  %d) %-16s %8s  %-5s %s\n" $((i + 1)) "$d" "$(field SIZE "$d")" \
    "$(field TRAN "$d")" "$(field MODEL "$d")"
done
[[ -n $boot_disk ]] && echo && echo "  (installer medium $boot_disk hidden)"
echo

read -rp "Select target disk [1-${#disks[@]}]: " sel
if ! [[ $sel =~ ^[0-9]+$ ]] || (( sel < 1 || sel > ${#disks[@]} )); then die "invalid selection"; fi
DISK=${disks[$((sel - 1))]}
NAME=$(basename "$DISK")

disk_bytes=$(blockdev --getsize64 "$DISK")
need_bytes=$(( (ESP_SIZE_MIB + 4 + 64 + 2 + MIN_USERDATA_MIB) * 1048576 + SUPER_SIZE_BYTES ))
(( disk_bytes >= need_bytes )) ||
  die "$DISK is too small: $((disk_bytes / 1048576)) MiB, need at least $((need_bytes / 1048576)) MiB"

# Work out androidboot.boot_devices for this disk. ueventd matches the PCI
# domain/bus plus the first device, e.g. pci0000:00/0000:00:1d.0
sysdev=$(readlink -f "/sys/block/$NAME")
rel=${sysdev#/sys}
if [[ $rel =~ ^/devices/(pci[^/]+/[^/]+) ]]; then
  BOOT_DEVICE=${BASH_REMATCH[1]}
elif [[ $rel =~ ^/devices/platform/(.+)/(ata|host|nvme|mmc_host)[^/]*/ ]]; then
  BOOT_DEVICE=${BASH_REMATCH[1]}
else
  die "can't determine boot device path for $DISK (sysfs: $rel)"
fi

echo
echo "Current contents of $DISK:"
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "$DISK" | sed 's/^/  /'
echo
echo "ALL DATA ON $DISK ($(field SIZE "$DISK") $(field MODEL "$DISK")) WILL BE DESTROYED."
read -rp "Type '$NAME' to confirm: " confirm
[[ $confirm == "$NAME" ]] || die "confirmation did not match, nothing was changed"

# ---------------------------------------------------------------- wipe
info "Releasing $DISK"
for p in $(lsblk -lnpo NAME "$DISK" | tac); do
  umount -q "$p" 2>/dev/null || true
  swapoff "$p" 2>/dev/null || true
done
if lsblk -lnpo MOUNTPOINT "$DISK" | grep -q .; then
  die "$DISK still has mounted filesystems"
fi

info "Wiping $DISK"
wipefs -af "$DISK" >/dev/null
sgdisk --zap-all "$DISK" >/dev/null
if [[ $DISCARD == 1 ]]; then
  blkdiscard "$DISK" 2>/dev/null || warn "discard not supported, continuing"
fi

# ---------------------------------------------------------------- partition
info "Creating partitions"
sgdisk -a 2048 \
  -n 1:0:+"${ESP_SIZE_MIB}"M          -t 1:EF00 -c 1:esp \
  -n 2:0:+4M                          -t 2:8300 -c 2:misc \
  -n 3:0:+64M                         -t 3:8300 -c 3:metadata \
  -n 4:0:+$((SUPER_SIZE_BYTES / 1048576))M -t 4:8300 -c 4:super \
  -n 5:0:0                            -t 5:8300 -c 5:userdata \
  "$DISK" >/dev/null
partprobe "$DISK" || true
udevadm settle

ESP=$(part "$DISK" 1); MISC=$(part "$DISK" 2); META=$(part "$DISK" 3)
SUPER=$(part "$DISK" 4); DATA=$(part "$DISK" 5)
for n in "$ESP" "$MISC" "$META" "$SUPER" "$DATA"; do wait_for_node "$n"; done

# ---------------------------------------------------------------- system
info "Writing system image to $SUPER"
zstd -dc "$PAYLOAD_DIR/super.img.zst" |
  dd of="$SUPER" bs=4M iflag=fullblock oflag=direct conv=fsync status=progress

if [[ $VERIFY == 1 ]]; then
  info "Verifying system image"
  sync; echo 3 > /proc/sys/vm/drop_caches
  got=$(head -c "$SUPER_SIZE_BYTES" "$SUPER" | sha256sum | cut -d' ' -f1)
  [[ $got == "$SUPER_SHA256" ]] || die "verification failed: data read back from $SUPER does not match"
fi

# misc must be zeroed; metadata/userdata get formatted by Android on first
# boot (the 'formattable' fstab flag), so just clear any old signatures.
info "Preparing misc, metadata and userdata"
dd if=/dev/zero of="$MISC" bs=1M count=4 conv=fsync status=none
for p in "$META" "$DATA"; do
  wipefs -af "$p" >/dev/null
  dd if=/dev/zero of="$p" bs=1M count=16 conv=fsync status=none
done

# ---------------------------------------------------------------- bootloader
info "Setting up EFI system partition"
mkfs.vfat -F 32 -n ANDROIDESP "$ESP" >/dev/null
mount "$ESP" "$MNT"
# secureboot: retain microcode source notices beside the installed boot chain.
mkdir -p "$MNT/EFI/BOOT/licenses"
install -m 0644 "$PAYLOAD_DIR/licenses/Intel-Microcode-LICENSE.txt" "$MNT/EFI/BOOT/licenses/"
install -m 0644 "$PAYLOAD_DIR/licenses/AMD-WHENCE.txt" "$MNT/EFI/BOOT/licenses/"
install -m 0644 "$PAYLOAD_DIR/licenses/Fedora-shim-x64-BSD-3-Clause.txt" "$MNT/EFI/BOOT/licenses/"

# secureboot: Secure Boot payloads install the shim chain and UKI on the same ESP.
if [[ ${SECURE_BOOT:-0} == 1 ]]; then
  install -D -m644 "$PAYLOAD_DIR/efi/EFI/BOOT/BOOTX64.EFI" "$MNT/EFI/BOOT/BOOTX64.EFI"
  install -D -m644 "$PAYLOAD_DIR/efi/EFI/BOOT/grubx64.efi" "$MNT/EFI/BOOT/grubx64.efi"
  install -D -m644 "$PAYLOAD_DIR/efi/EFI/BOOT/mmx64.efi" "$MNT/EFI/BOOT/mmx64.efi"
  install -D -m644 "$PAYLOAD_DIR/efi/EFI/BOOT/matonos-dev.der" "$MNT/EFI/BOOT/matonos-dev.der"
  install -D -m644 "$PAYLOAD_DIR/efi/systemd-bootx64.efi" "$MNT/EFI/systemd/systemd-bootx64.efi"
  install -D -m644 "$PAYLOAD_DIR/efi/EFI/Linux/matonos.efi" "$MNT/EFI/Linux/matonos.efi"
else
  install -D -m644 "$PAYLOAD_DIR/microcode.cpio" "$MNT/android/microcode.cpio"
  install -D -m644 "$PAYLOAD_DIR/efi/systemd-bootx64.efi" "$MNT/EFI/systemd/systemd-bootx64.efi"
  install -D -m644 "$PAYLOAD_DIR/efi/systemd-bootx64.efi" "$MNT/EFI/BOOT/BOOTX64.EFI"
  install -D -m644 "$PAYLOAD_DIR/bzImage"     "$MNT/android/bzImage"
  initrds="initrd  /android/microcode.cpio"
  if [[ -f $PAYLOAD_DIR/vendor_ramdisk.img ]]; then
    install -D -m644 "$PAYLOAD_DIR/vendor_ramdisk.img" "$MNT/android/vendor_ramdisk.img"
    initrds+=$'\n'"initrd  /android/vendor_ramdisk.img"
  fi
  install -D -m644 "$PAYLOAD_DIR/ramdisk.img" "$MNT/android/ramdisk.img"
  initrds+=$'\n'"initrd  /android/ramdisk.img"
fi

mkdir -p "$MNT/loader/entries"
cat > "$MNT/loader/loader.conf" <<EOF
default android.conf
timeout 3
console-mode keep
editor no
EOF

cmdline="$KERNEL_CMDLINE androidboot.boot_devices=$BOOT_DEVICE"
if [[ ${SECURE_BOOT:-0} == 1 ]]; then
  cat > "$MNT/loader/entries/android.conf" <<EOF
title   $LABEL
efi     /EFI/Linux/matonos.efi
options $cmdline
EOF
  cat > "$MNT/loader/entries/android-debug.conf" <<EOF
title   $LABEL (debug)
efi     /EFI/Linux/matonos.efi
options $cmdline ${DEBUG_CMDLINE:-}
EOF
else
  cat > "$MNT/loader/entries/android.conf" <<EOF
title   $LABEL
linux   /android/bzImage
$initrds
options $cmdline
EOF
cat > "$MNT/loader/entries/android-debug.conf" <<EOF
title   $LABEL (debug)
linux   /android/bzImage
$initrds
options $cmdline ${DEBUG_CMDLINE:-}
EOF
fi

sync
umount "$MNT"

# Register a firmware boot entry; the fallback BOOTX64.EFI covers firmware
# that ignores or loses NVRAM entries.
if command -v efibootmgr >/dev/null; then
  info "Registering UEFI boot entry '$LABEL'"
  while read -r num; do
    efibootmgr -q -b "$num" -B || true
  done < <(efibootmgr | sed -n "s/^Boot\([0-9A-Fa-f]\{4\}\)\*\{0,1\} $LABEL\(\t.*\)\{0,1\}$/\1/p")
  loader_path='\EFI\systemd\systemd-bootx64.efi'
  [[ ${SECURE_BOOT:-0} == 1 ]] && loader_path='\EFI\BOOT\BOOTX64.EFI'
  efibootmgr -q -c -d "$DISK" -p 1 -L "$LABEL" -l "$loader_path" ||
    warn "could not create UEFI entry; the fallback loader will still boot"
fi

trap - ERR
echo
info "Done. $LABEL installed to $DISK (boot device $BOOT_DEVICE)."
read -rp "Remove the installer medium and press Enter to reboot (Ctrl+C to stay): " _
reboot
