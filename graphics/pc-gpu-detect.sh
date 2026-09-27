#!/vendor/bin/sh
# pc-gpu-detect.sh: pick the Vulkan HAL for this PC at early boot.
#
# Android's Vulkan loader loads exactly one HAL, /vendor/lib64/hw/
# vulkan.<ro.hardware.vulkan>.so, but a generic PC may have an Intel, AMD,
# NVIDIA or virtio GPU. Decide from the PCI IDs of the primary display adapter (the
# one with boot_vga=1, else the first display-class device); this works
# before any GPU kernel module has loaded. EGL/GLES needs no selection: Mesa's
# EGL picks the gallium driver from the render node itself.

# Wait briefly for PCI GPU modules to publish render nodes. This is bounded so
# a missing or slow driver can never hold boot indefinitely. The kernel module
# loader and device-node creation run asynchronously during early boot.
has_supported_render_node() {
    for node in /sys/class/drm/renderD*; do
        [ -e "$node/device/driver" ] || continue
        driver=$(basename "$(readlink "$node/device/driver" 2>/dev/null)")
        case "$driver" in
            i915|xe|amdgpu|radeon|nouveau|virtio_gpu|vmwgfx) return 0 ;;
        esac
    done
    return 1
}

attempt=0
while [ "$attempt" -lt 15 ] && ! has_supported_render_node; do
    sleep 0.1
    attempt=$((attempt + 1))
done

if ! has_supported_render_node; then
    # vgem is a shmem-backed DRM render node. Mesa's kms_swrast/llvmpipe uses
    # it for software rendering; drm_hwcomposer sends the resulting buffers
    # to the real KMS display. Ignore absence/failure: graphics must still
    # continue booting with the platform's existing no-renderer fallback.
    # Mesa's Android option lookup reads vendor.mesa.* properties. Keep this
    # software-rendering flag in the vendor-owned namespace; vendor domains
    # cannot write the core debug_prop namespace.
    setprop vendor.mesa.libgl.always.software true
    if [ -x /vendor/bin/modprobe ]; then
        /vendor/bin/modprobe -d /vendor/lib/modules vgem >/dev/null 2>&1 || :
    fi
fi

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
    0x8086)
        # anv is Gen9+; Gen7-8 use hasvk, older Intel has no Vulkan (table
        # generated from Mesa's PCI IDs by tools/build-mesa.sh).
        vk=intel
        while read -r id drv; do
            [ "$id" = "$device" ] || continue
            vk=$drv
            break
        done < /vendor/etc/intel_vulkan_pci_ids.txt
        [ "$vk" = "none" ] && vk="" ;;
    0x1002) vk=radeon ;;
    0x10de) vk=nouveau ;;  # NVK (Turing+ fully; older GPUs fail gracefully)
    0x1af4)
        # virtio-gpu (0x1050); Mesa's venus needs host Vulkan (QEMU venus=on)
        [ "$device" = "0x1050" ] && vk=virtio ;;
esac

# No hardware Vulkan driver for this GPU (old Intel, VMware svga, unknown):
# lavapipe, Mesa's LLVM software Vulkan, so Vulkan apps still run.
setprop ro.hardware.vulkan "${vk:-swrast}"
setprop vendor.pc.gpu "$vendor:$device"
