# Installer stays in the fixed matonos_driver domain and uses the existing
# fixed sepolicy file set. Do not add an area-specific sepolicy directory.
# install: one populated A slot plus COW room; B is created by an OTA.
# The base BoardConfig declares the group members and AB_OTA_UPDATER.
BOARD_SUPER_PARTITION_SIZE := 6442450944
# Our ODM driver bundle (Mesa selectors, daemons, /odm/etc) is built outside
# Soong by tools/build-bundle.sh into THIS path before the AOSP build; AOSP
# copies it as odm.img. Without this, PRODUCT_BUILD_ODM_IMAGE made AOSP
# generate its own empty odm.img over the bundle, unsetting ro.hardware.egl
# and aborting SurfaceFlinger (boot loop). SHARED-CHANGES.md 2026-09-30.
BOARD_PREBUILT_ODMIMAGE := $(PRODUCT_OUT)/odm-bundle.img
TARGET_COPY_OUT_ODM := odm

# First-stage init loads generic ramdisk modules before mapping snapshots.
# The current prebuilt kernel has CONFIG_BLK_DEV_UBLK=m; modinfo reports no
# dependencies, so ship this matching module in its early modules.load.
BOARD_GENERIC_RAMDISK_KERNEL_MODULES += $(DEVICE_PATH)/prebuilt/modules/ublk_drv.ko
BOARD_GENERIC_RAMDISK_KERNEL_MODULES_LOAD += $(DEVICE_PATH)/prebuilt/modules/ublk_drv.ko
