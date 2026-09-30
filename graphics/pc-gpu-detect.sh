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
# Does a DRM render node belong to one of the expected GPUs? Match by the
# node's PCI vendor/device. The old code compared the node's *bus* driver
# (e.g. virtio-pci) against the *DRM* driver name (virtio_gpu); they differ
# for virtio-gpu and the faux/vgem node, so a real virtio-gpu render node was
# never recognised and the detector forced the software (llvmpipe) fallback
# on QEMU virgl. A node with no PCI parent is the faux/vgem software node.
has_render_node() {
    expected=$1
    for node in /sys/class/drm/renderD*; do
        [ -e "$node" ] || continue
        v=""; d=""
        [ -e "$node/device/vendor" ] && read -r v < "$node/device/vendor" 2>/dev/null
        [ -e "$node/device/device" ] && read -r d < "$node/device/device" 2>/dev/null
        if [ -z "$v" ]; then
            case " $expected " in *" vgem "*) return 0 ;; esac
            continue
        fi
        for want in $expected; do
            case "$want" in
                i915|xe)       [ "$v" = "0x8086" ] && return 0 ;;
                amdgpu|radeon) [ "$v" = "0x1002" ] && return 0 ;;
                nouveau)       [ "$v" = "0x10de" ] && return 0 ;;
                virtio_gpu)    [ "$v" = "0x1af4" ] && [ "$d" = "0x1050" ] && return 0 ;;
                vmwgfx)        [ "$v" = "0x15ad" ] && return 0 ;;
            esac
        done
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
    setprop vendor.maton.graphics.gpu "$vendor:$device"
fi

# Wait at most 1.5 seconds for the expected render node. A missing node after
# this grace period means the configured driver did not start; use vgem so EGL
# still has a render node and SurfaceFlinger can boot.
attempt=0
while [ -n "$supported_drivers" ] && [ "$attempt" -lt 15 ] &&
      ! has_render_node "$supported_drivers"; do
    sleep 0.1
    attempt=$((attempt + 1))
done

# A GPU-specific Vulkan HAL cannot create a device without its kernel render
# node. Match the software EGL fallback in that case.
if [ -n "$supported_drivers" ] && ! has_render_node "$supported_drivers"; then
    vk=swrast
fi

if { [ -z "$supported_drivers" ] || ! has_render_node "$supported_drivers"; } &&
   ! has_render_node "vgem"; then
    # vgem is a shmem-backed DRM render node. Mesa's kms_swrast/llvmpipe
    # renders into its buffers; drm_hwcomposer sends frames to real KMS.
    #
    # Mesa reads this via os_misc.c os_get_android_option(): the debug option
    # LIBGL_ALWAYS_SOFTWARE maps to the key mesa.libgl.always.software and is
    # looked up as debug.mesa.*, then vendor.mesa.*, then bare. So the property
    # Mesa actually honours is vendor.mesa.libgl.always.software. The
    # vendor.maton.graphics.software alias is what the Settings hardware report
    # reads; set both.
    setprop vendor.mesa.libgl.always.software true
    setprop vendor.maton.graphics.software true
    if [ -x /vendor/bin/modprobe ]; then
        if [ -x /vendor/bin/timeout ]; then
            /vendor/bin/timeout 3 /vendor/bin/modprobe -d /vendor/lib/modules vgem >/dev/null 2>&1 || :
        else
            /vendor/bin/modprobe -d /vendor/lib/modules vgem >/dev/null 2>&1 &
            modprobe_pid=$!
            attempt=0
            while kill -0 "$modprobe_pid" 2>/dev/null && [ "$attempt" -lt 30 ]; do
                sleep 0.1
                attempt=$((attempt + 1))
            done
            kill "$modprobe_pid" 2>/dev/null || :
        fi
    fi
fi

# No hardware Vulkan driver for this GPU (old Intel, VMware, unknown): swrast
# is Mesa's LLVM software Vulkan implementation.
setprop vendor.maton.graphics.vulkan "${vk:-swrast}"
