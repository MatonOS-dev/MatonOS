#!/vendor/bin/sh
# pc-gpu-detect.sh: pick the Vulkan HAL for this PC at early boot.
#
# Android's Vulkan loader loads exactly one HAL, /vendor/lib64/hw/
# vulkan.<ro.hardware.vulkan>.so, but a generic PC may have an Intel, AMD or
# virtio GPU. Decide from the PCI IDs of the primary display adapter (the
# one with boot_vga=1, else the first display-class device); this works
# before any GPU kernel module has loaded. EGL/GLES needs no selection: Mesa's
# EGL picks the gallium driver from the render node itself.

primary=""
first=""
for dev in /sys/bus/pci/devices/*; do
    read -r class < "$dev/class" 2>/dev/null || continue
    case $class in 0x03*) ;; *) continue ;; esac
    [ -z "$first" ] && first=$dev
    if [ "$(cat "$dev/boot_vga" 2>/dev/null)" = "1" ]; then
        primary=$dev
        break
    fi
done
[ -z "$primary" ] && primary=$first
[ -z "$primary" ] && exit 0

read -r vendor < "$primary/vendor"
read -r device < "$primary/device"

case $vendor in
    0x8086) vk=intel ;;
    0x1002) vk=radeon ;;
    0x1af4)
        # virtio-gpu (0x1050); Mesa's venus needs host Vulkan (QEMU venus=on)
        [ "$device" = "0x1050" ] && vk=virtio ;;
esac

if [ -n "$vk" ]; then
    setprop ro.hardware.vulkan "$vk"
fi
setprop vendor.pc.gpu "$vendor:$device"
