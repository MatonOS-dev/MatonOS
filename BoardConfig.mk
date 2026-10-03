#
# BoardConfig for pc_x86_64.
#
# Partition layout is defined by tools/installer.sh (GPT: esp, misc, metadata,
# super, userdata). Keep this file, fstab.pc_x86_64 and tools/make-payload.sh
# in sync with it.
#

DEVICE_PATH := device/maton/pc_x86_64

# ---------------------------------------------------------------- arch
TARGET_ARCH := x86_64
# x86_64-v2 baseline (user, 2026-09-30): SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT
# (2009+). Soong maps the x86_64 arch variant "sandybridge" to -march=corei7,
# which is exactly x86-64-v2. Out-of-tree builds add -march=x86-64-v2
# directly (see tools/build-mesa.sh, build-native.sh, build-pipewire.sh).
TARGET_ARCH_VARIANT := sandybridge
TARGET_CPU_ABI := x86_64
TARGET_CPU_VARIANT := generic
# 64-bit only userspace (see pc_x86_64.mk: core_64_bit_only.mk)

TARGET_BOARD_PLATFORM := pc_x86_64
TARGET_BOOTLOADER_BOARD_NAME := pc_x86_64

# ---------------------------------------------------------------- boot
# systemd-boot loads the mainline bzImage plus ramdisk.img and
# vendor_ramdisk.img directly from the ESP; there is no boot/vendor_boot/
# recovery partition. The kernel is built by tools/build-kernel.sh.
TARGET_NO_BOOTLOADER := true
TARGET_NO_KERNEL := true
TARGET_NO_RECOVERY := true

# Header v4 is only here so the build produces vendor_ramdisk.img (it comes
# with vendor_boot.img, which we don't ship). Because vendor_boot.img is
# unused, BOARD_KERNEL_CMDLINE / BOARD_BOOTCONFIG have no effect: the kernel
# command line lives in make-payload.sh (payload.conf) and installer.sh.
BOARD_BOOT_HEADER_VERSION := 4
BOARD_MKBOOTIMG_ARGS += --header_version $(BOARD_BOOT_HEADER_VERSION)
BOARD_VENDOR_BOOTIMAGE_PARTITION_SIZE := 67108864

# ---------------------------------------------------------------- kernel modules
# Staged by tools/build-kernel.sh. Installed to /vendor/lib/modules; the build
# runs depmod (modules.alias), and ueventd loads them by modalias.
PC_KERNEL_PREBUILT := $(DEVICE_PATH)/prebuilt
BOARD_VENDOR_KERNEL_MODULES := $(wildcard $(PC_KERNEL_PREBUILT)/modules/*.ko)
# Already stripped by modules_install INSTALL_MOD_STRIP=1
BOARD_DO_NOT_STRIP_VENDOR_MODULES := true
# Some linux-firmware blobs (e.g. NVIDIA GSP) are ELF files; they are copied
# to /vendor/firmware via PRODUCT_COPY_FILES in device.mk.
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

# ---------------------------------------------------------------- AVB
# Disabled: no vbmeta partition, systemd-boot does no verification.
BOARD_AVB_ENABLE := false

# ---------------------------------------------------------------- partitions
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_RO_FILE_SYSTEM_TYPE := erofs

BOARD_USES_METADATA_PARTITION := true

TARGET_COPY_OUT_VENDOR := vendor
TARGET_COPY_OUT_PRODUCT := product
TARGET_COPY_OUT_SYSTEM_EXT := system_ext

BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := $(TARGET_RO_FILE_SYSTEM_TYPE)
BOARD_USES_VENDORIMAGE := true
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := $(TARGET_RO_FILE_SYSTEM_TYPE)
BOARD_USES_PRODUCTIMAGE := true
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := $(TARGET_RO_FILE_SYSTEM_TYPE)
BOARD_USES_SYSTEM_EXTIMAGE := true
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := $(TARGET_RO_FILE_SYSTEM_TYPE)

# userdata is formatted on first boot ('formattable'); this image is not
# shipped, the size only keeps the build happy.
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USERDATAIMAGE_PARTITION_SIZE := 576716800

# install: Installed systems use A/B dynamic partitions; make-live.sh packs
# only slot A for the live USB image. Each 5 GiB group fits in half of super.
AB_OTA_UPDATER := true
BOARD_SUPER_PARTITION_SIZE := 11274289152
BOARD_SUPER_PARTITION_GROUPS := pc_dynamic_partitions
BOARD_PC_DYNAMIC_PARTITIONS_SIZE := 5368709120
BOARD_PC_DYNAMIC_PARTITIONS_PARTITION_LIST := system system_ext product vendor odm
BOARD_BUILD_SUPER_IMAGE_BY_DEFAULT := true

BOARD_FLASH_BLOCK_SIZE := 4096

# ---------------------------------------------------------------- vintf
DEVICE_MANIFEST_FILE += $(DEVICE_PATH)/manifest.xml

# ---------------------------------------------------------------- sepolicy
# Enforcement is controlled by androidboot.selinux on the kernel cmdline
# (permissive for bring-up; only honoured on userdebug/eng).
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/matonos
# AOSP components we configure (graphics HALs, block devices, Mesa/gralloc
# same-process libs, pc-gpu-detect/pc-wakeup). Dropped by the 2026-09-25
# consolidation by mistake: without it init refuses the composer/allocator
# and the partitions lose their labels. Keep both dirs.
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/vendor
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/system_ext/private

# Areas (audio, wifi, bluetooth; v2: sleep, install, updater) each own their directory: board
# settings in <area>/BoardConfig.mk, packages in <area>/<area>.mk (from
# device.mk), SELinux in <area>/sepolicy/ (added to the dirs by the area).
-include $(DEVICE_PATH)/audio/BoardConfig.mk
-include $(DEVICE_PATH)/wifi/BoardConfig.mk
-include $(DEVICE_PATH)/bluetooth/BoardConfig.mk
-include $(DEVICE_PATH)/sleep/BoardConfig.mk
-include $(DEVICE_PATH)/install/BoardConfig.mk
-include $(DEVICE_PATH)/updater/BoardConfig.mk
-include $(DEVICE_PATH)/settings/BoardConfig.mk
-include $(DEVICE_PATH)/buildinfra/BoardConfig.mk
-include $(DEVICE_PATH)/systembridge/BoardConfig.mk
-include $(DEVICE_PATH)/camera/BoardConfig.mk
-include $(DEVICE_PATH)/input/BoardConfig.mk

# ---------------------------------------------------------------- graphics
# minigbm is BlissOS/android-generic's (manifest/maton.xml). Its gbm_mesa backend
# allocates through Mesa's GBM for every Mesa GPU. The matonos/v1.2 fork
# branch pinned in manifest/maton.xml selects gbm_mesa for the AIDL allocator
# and stable-C mapper (vendorforks/README.md).
$(call soong_config_set,minigbm,backend,gbm_mesa)
# HWUI renders with GL for v1 (sets ro.hwui.use_vulkan=false): Vulkan
# availability varies per PC.
TARGET_USES_VULKAN := false

# OEM AIDs generate vendor/etc/passwd and vendor/etc/group.
TARGET_FS_CONFIG_GEN += $(DEVICE_PATH)/config.fs

# ---------------------------------------------------------------- misc
BOARD_PROPERTY_OVERRIDES_SPLIT_ENABLED := true
BOARD_MALLOC_ALIGNMENT := 16
TARGET_USES_HWC2 := true
