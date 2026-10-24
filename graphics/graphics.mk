#
# Graphics for pc_x86_64: Mesa (prebuilt, tools/build-mesa.sh) + minigbm
# gralloc (native virtgpu on virtio_gpu; Mesa GBM on other DRM drivers) +
# drm_hwcomposer (HWC3).
#

PC_GRAPHICS_PATH := device/maton/pc_x86_64/graphics
PC_MESA_PREBUILT := device/maton/pc_x86_64/prebuilt/mesa

ifeq ($(wildcard $(PC_MESA_PREBUILT)/lib64),)
$(error 64-bit Mesa prebuilts missing at $(PC_MESA_PREBUILT); run tools/build-mesa.sh)
endif
ifeq ($(wildcard $(PC_MESA_PREBUILT)/lib),)
$(error 32-bit Mesa prebuilts missing at $(PC_MESA_PREBUILT); run tools/build-mesa.sh)
endif

# ---------------------------------------------------------------- HALs
# libmaton_mesa_sphal_deps (graphics/Android.bp) pulls vendor libhardware for
# both ABIs: the copied Mesa has no Soong deps and nothing 32-bit links it.
PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator-service.minigbm \
    mapper.minigbm \
    gralloc.minigbm_gbm_mesa \
    libmaton_mesa_sphal_deps \
    android.hardware.composer.hwc3-service.drm \
    pc-gpu-detect.sh \
    init.pc_x86_64.graphics.rc

# ---------------------------------------------------------------- Mesa
# Staged by tools/build-mesa.sh with vendor-relative paths:
#   lib64/egl/lib{EGL,GLESv1_CM,GLESv2}_mesa.so, lib64/libgallium-*.so,
#   lib64/libgbm_mesa.so + lib64/libgbm_mesa_wrapper.so (minigbm's GBM Mesa
#   backend dlopens the wrapper; virtio_gpu uses minigbm's native virtgpu
#   backend),
#   lib64/hw/vulkan.{intel,intel_hasvk,radeon,nouveau,virtio,swrast}.so,
#   etc/intel_vulkan_pci_ids.txt (Intel anv/hasvk split for pc-gpu-detect.sh)
# The same driver set is staged under lib/ for the 32-bit x86 secondary ABI.
PRODUCT_COPY_FILES += \
    $(foreach f,$(shell test -d $(PC_MESA_PREBUILT) && cd $(PC_MESA_PREBUILT) && find . -type f -printf '%P\n'),\
        $(PC_MESA_PREBUILT)/$(f):$(TARGET_COPY_OUT_VENDOR)/$(f))

# Vendor-owned Mesa hardware selectors are in /odm/etc/build.prop, staged by
# tools/build-bundle.sh. The system-owned OpenGL version and debug renderer
# defaults remain product properties. ro.hardware.vulkan is selected at boot.
PRODUCT_VENDOR_PROPERTIES += \
    ro.opengles.version=196610 \
    debug.renderengine.backend=skiaglthreaded
