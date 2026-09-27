#!/usr/bin/env bash
# run-qemu-live.sh: boot the live image in QEMU (UEFI via OVMF).
#
# Usage: ./run-qemu-live.sh [-i <live image>] [-g virgl|venus|std|none] [-m <MiB>]
#                           [-r <WxH>] [-s <serial log>] [-a <adb port>]
#                           [-x "<extra QEMU args>"] [-n]
#
#   -i  image       (default: <aosp>/out/target/product/pc_x86_64/matonos-live-x86_64.img)
#   -g  graphics    virgl: virtio-gpu with host GL (default)
#                   venus: virgl + Vulkan passthrough (Mesa venus in the guest;
#                          needs QEMU >= 9.2, venus-enabled virglrenderer and
#                          host Vulkan)
#                   std:   plain VGA framebuffer, no GPU acceleration
#                   none:  headless, serial console only
#   -m  RAM in MiB  (default: 8192; /data lives in RAM on the live image)
#   -r  resolution  (default: 1600x900; at least ~1000x600 keeps Android's
#                   large-screen desktop layout)
#   -s  serial log  (default: <image dir>/serial-live.log)
#   -a  host port forwarded to the guest's adb (default: 5555); give each
#       concurrently running VM its own port and -s log
#   -x  extra QEMU arguments, word-split (e.g. -x "-device intel-hda
#       -device hda-duplex,audiodev=snd0 -audiodev pa,id=snd0")
#   -n  no KVM
#
# The image is attached as a USB stick (qemu-xhci + usb-storage): that's the
# real use case, and androidboot.boot_part_uuid matches SCSI/NVMe/MMC disks
# but not virtio-blk. adb: `adb connect localhost:5555` (userdebug builds).
# The image is opened copy-on-write (snapshot=on): nothing is written to it.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
IMAGE=$AOSP/out/target/product/pc_x86_64/matonos-live-x86_64.img
GFX=virgl
MEM=8192
RES=1600x900
LOG=""
KVM=1
ADB_PORT=5555
EXTRA=""

while getopts "i:g:m:r:s:a:x:nh" opt; do
  case $opt in
    i) IMAGE=$OPTARG ;;
    g) GFX=$OPTARG ;;
    m) MEM=$OPTARG ;;
    r) RES=$OPTARG ;;
    s) LOG=$OPTARG ;;
    a) ADB_PORT=$OPTARG ;;
    x) EXTRA=$OPTARG ;;
    n) KVM=0 ;;
    *) sed -n '2,/^set -E/p' "$0" | sed '$d'; exit 1 ;;
  esac
done

[[ -f $IMAGE ]] || die "no image at $IMAGE (-i)"
[[ $RES =~ ^([0-9]+)x([0-9]+)$ ]] || die "resolution must be WIDTHxHEIGHT (-r)"
XRES=${BASH_REMATCH[1]} YRES=${BASH_REMATCH[2]}
LOG=${LOG:-$(dirname "$IMAGE")/serial-live.log}
command -v qemu-system-x86_64 >/dev/null || die "install qemu-system-x86"

OVMF_CODE=""
for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd; do
  [[ -f $f ]] && { OVMF_CODE=$f; break; }
done
[[ -n $OVMF_CODE ]] || die "OVMF not found (install ovmf)"
OVMF_VARS_SRC=${OVMF_CODE/CODE/VARS}
[[ -f $OVMF_VARS_SRC ]] || die "missing $OVMF_VARS_SRC"
vars=$(mktemp --tmpdir ovmf-vars.XXXXXX.fd)
trap 'rm -f "$vars"' EXIT
cp "$OVMF_VARS_SRC" "$vars"

machine=q35,vmport=on  # vmport: VMware absolute mouse (see input below)
if [[ $GFX == venus ]]; then
  # venus needs blob resources, which need guest RAM in a shareable memfd.
  machine+=",memory-backend=mem0"
fi

args=(
  -machine "$machine" -m "$MEM" -smp 4
  # No S3: virtio-gpu loses its scanout over suspend-to-RAM and the screen
  # stays black after wake. Android then suspends with s2idle instead.
  -global ICH9-LPC.disable_s3=1
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
  -drive "if=pflash,format=raw,file=$vars"
  -device "qemu-xhci,id=xhci"
  -drive "id=live,if=none,format=raw,snapshot=on,file=$IMAGE"
  -device "usb-storage,bus=xhci.0,drive=live,bootindex=0"
  # Input: q35's built-in PS/2 keyboard and the VMware absolute mouse
  # (vmmouse, PS/2 AUX), not usb-kbd/usb-tablet. QEMU's xHCI can't wake the
  # guest (no PME/ACPI wake), but the i8042 PS/2 controller can, so keys and
  # the mouse wake MatonOS from s2idle (power/pc-wakeup.sh enables them).
  # vmmouse is absolute: no pointer grab (patches/frameworks/native).
  -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$ADB_PORT-:5555"
  -device "virtio-net-pci,netdev=net0"
  # Serial console: logged to $LOG and also reachable as a unix socket
  # ($LOG.sock) for the root shell (debug entry sets androidboot.console).
  -chardev "socket,id=ser0,path=$LOG.sock,server=on,wait=off,logfile=$LOG"
  -serial chardev:ser0
)
if [[ $KVM == 1 ]]; then
  [[ -w /dev/kvm ]] || die "/dev/kvm not writable (add yourself to the kvm group, or use -n)"
  args+=(-enable-kvm -cpu host)
else
  args+=(-cpu max)
fi
# Agent VMs (anything started under a `codex` process) never open a window on
# the user's desktop and never take the user's adb port 5555 (user rule).
# Start the interactive display full-screen before firmware/Android boot. This
# avoids opening a 640x480 GTK window during POST and changing the guest mode
# before SurfaceFlinger picks its boot-time display size. zoom-to-fit keeps the
# EDID-selected guest mode independent from later host window resizes.
gtk_gl="gtk,gl=on,zoom-to-fit=on,full-screen=on" gtk_plain="gtk,full-screen=on"
# MATON_QEMU_HEADLESS=1: no window (unattended runs; a GTK GL window stalls
# the guest while the host display is off or locked).
[[ ${MATON_QEMU_HEADLESS:-0} == 1 ]] && gtk_gl=egl-headless gtk_plain=none
pid=$$; while [[ $pid -gt 1 ]]; do
  if [[ $(ps -o comm= -p "$pid") == codex* ]]; then
    gtk_gl=egl-headless gtk_plain=none
    [[ $ADB_PORT == 5555 ]] && die "port 5555 is the user's VM; agents use -a 5556+"
    break
  fi
  pid=$(ps -o ppid= -p "$pid" | tr -d ' ')
done
case $GFX in
  # xres/yres (-r): virtio-gpu's own default (640x480) is below Android's
  # large-screen size (600 dp), which changes the taskbar/desktop layout.
  # EDID advertises the requested boot mode. GTK zoom-to-fit scales the window
  # without sending host-window resizes back as guest display-mode changes.
  virgl) args+=(-device "virtio-vga-gl,edid=on,xres=$XRES,yres=$YRES" -display "$gtk_gl") ;;
  venus) args+=(-object "memory-backend-memfd,id=mem0,size=${MEM}M,share=on"
                -device "virtio-vga-gl,edid=on,hostmem=4G,blob=true,venus=true,xres=$XRES,yres=$YRES"
                -display "$gtk_gl") ;;
  std)   args+=(-vga std -display "$gtk_plain") ;;
  none)  args+=(-vga none -display none) ;;
  *)     die "unknown graphics mode: $GFX" ;;
esac

# Sound card (Intel HDA, like most PCs) unless -x brings its own: windowed VMs
# play through the host's PipeWire, headless ones into a null backend.
if [[ $EXTRA != *hda* ]]; then
  # pa (via pipewire-pulse on PipeWire hosts) is QEMU's best-tested backend;
  # MATON_QEMU_AUDIO=pipewire|alsa|none overrides.
  [[ $GFX == none || $gtk_gl == egl-headless ]] && snd=none || snd=${MATON_QEMU_AUDIO:-pa}
  # MATON_QEMU_SOUND=hda (default: Intel HDA like real PCs, same driver/HAL
  # path) or virtio (paravirtual virtio-sound, guest module virtio_snd).
  case ${MATON_QEMU_SOUND:-hda} in
    virtio) args+=(-audiodev "$snd,id=snd0" -device "virtio-sound-pci,audiodev=snd0") ;;
    *)      args+=(-audiodev "$snd,id=snd0" -device ich9-intel-hda -device "hda-duplex,audiodev=snd0") ;;
  esac
fi

if [[ -n $EXTRA ]]; then
  read -r -a extra_args <<< "$EXTRA"
  args+=("${extra_args[@]}")
fi

# Host RAM guard: at most MATON_VM_SLOTS (default 2) VMs at once across all
# agents; wait for a free slot. The lock fd stays open in QEMU (exec), so the
# slot is held exactly as long as the VM runs.
slots=${MATON_VM_SLOTS:-2}
lockdir=$AOSP/out/pc-logs  # fixed, whatever image -i names
mkdir -p "$lockdir"
got=""
while [[ -z $got ]]; do
  for i in $(seq 1 "$slots"); do
    exec 8>"$lockdir/.vm-slot-$i.lock"
    if flock -n 8; then got=$i; break; fi
    exec 8>&-
  done
  [[ -n $got ]] || { echo "waiting for a free VM slot ($slots in use)"; sleep 20; }
done
echo "VM slot $got"

rm -f "$LOG.sock"
echo "Serial console -> $LOG (interactive: $LOG.sock)"
exec qemu-system-x86_64 "${args[@]}"
