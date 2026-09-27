#!/vendor/bin/sh
# pc-gpu-detect.sh: pick the Vulkan HAL for this PC at early boot.
#
# Android's Vulkan loader loads exactly one HAL, /vendor/lib64/hw/
# vulkan.<ro.hardware.vulkan>.so, but a generic PC may have an Intel, AMD,
# NVIDIA or virtio GPU. Decide from the PCI IDs of the primary display adapter (the
# one with boot_vga=1, else the first display-class device); this works
# before any GPU kernel module has loaded. EGL/GLES needs no selection: Mesa's
# EGL picks the gallium driver from the render node itself.

# Check whether a render node belongs to the driver's expected PCI GPU. This
# prevents a concurrently loaded vgem node (or a second adapter) from masking
# the real GPU's not-yet-created node.
has_render_node() {
    expected=$1
    for node in /sys/class/drm/renderD*; do
        [ -e "$node/device/driver" ] || continue
        driver=$(basename "$(readlink "$node/device/driver" 2>/dev/null)")
        case " $expected " in *" $driver "*) return 0 ;; esac
    done
    return 1
}

primary=""
first=""
supported_drivers=""
for dev in /sys/bus/pci/devices/*; do
    read -r class < "$dev/class" 2>/dev/null || continue
    case $class in 0x03*) ;; *) continue ;; esac
    [ -z "$first" ] && first=$dev
    if [ "$(cat "$dev/boot_vga" 2>/dev/null)" = "1" ]; then
        [ -z "$primary" ] && primary=$dev
    fi
done
[ -z "$primary" ] && primary=$first

# Identify hardware before looking for render nodes. early-init runs before
# modalias-triggered module loading is guaranteed to finish; a known PCI GPU
# with a configured kernel/Mesa driver must never be mistaken for no GPU.
# Drivers in this list are built in or staged in the device's module bundle.
for dev in /sys/bus/pci/devices/*; do
    read -r class < "$dev/class" 2>/dev/null || continue
    case $class in 0x03*) ;; *) continue ;; esac
    read -r pci_vendor < "$dev/vendor"
    read -r pci_device < "$dev/device"
    case $pci_vendor in
        0x8086) supported_drivers="$supported_drivers i915 xe" ;;
        0x1002) supported_drivers="$supported_drivers amdgpu radeon" ;;
        0x10de) supported_drivers="$supported_drivers nouveau" ;;
        0x1af4)
            [ "$pci_device" = "0x1050" ] &&
                supported_drivers="$supported_drivers virtio_gpu" ;;
        0x15ad) supported_drivers="$supported_drivers vmwgfx" ;;
    esac
done

if [ -n "$primary" ]; then
    read -r vendor < "$primary/vendor"
    read -r device < "$primary/device"
    case $vendor in
        0x8086)
            # anv is Gen9+; Gen7-8 use hasvk; older Intel has no Vulkan.
            vk=intel
            while read -r id drv; do
                [ "$id" = "$device" ] || continue
                vk=$drv
                break
            done < /vendor/etc/intel_vulkan_pci_ids.txt
            [ "$vk" = "none" ] && vk="" ;;
        0x1002) vk=radeon ;;
        0x10de) vk=nouveau ;;
        0x1af4)
            # virtio-gpu (0x1050); Mesa's venus needs host Vulkan (QEMU venus=on)
            [ "$device" = "0x1050" ] && vk=virtio ;;
    esac
    setprop vendor.pc.gpu "$vendor:$device"
fi

# Wait at most 1.5 seconds for a known GPU's expected render node. The wait
# is only a startup grace period; if node creation is slower, still keep the
# hardware path and let Android's normal driver startup continue. A recognized
# GPU driver that is staged/configured is never replaced with vgem.
attempt=0
while [ -n "$supported_drivers" ] && [ "$attempt" -lt 15 ] &&
      ! has_render_node "$supported_drivers"; do
    sleep 0.1
    attempt=$((attempt + 1))
done

if [ -z "$supported_drivers" ]; then
    # Do not override a render node from another supported adapter.
    if ! has_render_node "i915 xe amdgpu radeon nouveau virtio_gpu vmwgfx"; then
        # vgem is a shmem-backed DRM render node. Mesa's kms_swrast/llvmpipe
        # renders into its buffers; drm_hwcomposer sends frames to real KMS.
        # Mesa reads this vendor-owned option to select software rendering.
        setprop vendor.mesa.libgl.always.software true
        if [ -x /vendor/bin/modprobe ]; then
            /vendor/bin/modprobe -d /vendor/lib/modules vgem >/dev/null 2>&1 || :
        fi
    fi
fi

# No hardware Vulkan driver for this GPU (old Intel, VMware, unknown): swrast
# is Mesa's LLVM software Vulkan implementation.
setprop ro.hardware.vulkan "${vk:-swrast}"
