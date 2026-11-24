#
# pc_x86_64: AOSP for generic x86_64 UEFI PCs, mainline kernel, Mesa graphics.
#

#
# All components inherited here go to system image (same as GSI system)
#
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/generic_system.mk)

PRODUCT_ENFORCE_ARTIFACT_PATH_REQUIREMENTS := relaxed

# Ship APEXes uncompressed: otherwise apexd unpacks ~30 .capex files to /data on
# first boot, which took ~13 min on a spinning disk. The system partition has room.
PRODUCT_COMPRESSED_APEX := false

#
# All components inherited here go to system_ext image
#
$(call inherit-product, $(SRC_TARGET_DIR)/product/handheld_system_ext.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/telephony_system_ext.mk)

#
# All components inherited here go to product image
#
$(call inherit-product, $(SRC_TARGET_DIR)/product/aosp_product.mk)

#
# All components inherited here go to vendor image / vendor ramdisk
#
$(call inherit-product, device/maton/pc_x86_64/device.mk)

PRODUCT_NAME := pc_x86_64
PRODUCT_DEVICE := pc_x86_64
PRODUCT_BRAND := MatonOS
PRODUCT_MANUFACTURER := MatonOS
PRODUCT_MODEL := MatonOS on x86_64 PC

# MatonSystemUI keeps the stock SystemUI graph and provides the WM Shell variant seam.
PRODUCT_PACKAGES += MatonSystemUI

# Private, machine-provisioned signing keys. tools/build.sh creates missing
# pairs before Make/Soong starts; the key directory is never part of this repo.
ifeq ($(strip $(MATON_RELEASE_KEY_DIR)),)
$(error MATON_RELEASE_KEY_DIR must be set by MatonOS machine configuration)
endif
PRODUCT_DEFAULT_DEV_CERTIFICATE := maton-release-keys/releasekey
PRODUCT_MAINLINE_SEPOLICY_DEV_CERTIFICATES := maton-release-keys
PRODUCT_MAINLINE_BLUETOOTH_SEPOLICY_DEV_CERTIFICATES := maton-release-keys
PRODUCT_MAINLINE_NFC_SEPOLICY_DEV_CERTIFICATES := maton-release-keys
# Bluetooth pins its own certificate module (AOSP dev key); sign it with our
# bluetooth key so seinfo=bluetooth matches (else zygote aborts on BT enable).
PRODUCT_CERTIFICATE_OVERRIDES += Bluetooth:com.matonos.bluetooth.certificate

# With a non-test default certificate AOSP tags the build dev-keys
# (build/make/core/config.mk BUILD_KEYS); no manual ro.build.tags override.
