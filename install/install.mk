# The system_ext native service is built by Soong and starts after boot.
PRODUCT_PACKAGES += matonos-linuxd

# install: VABC snapshots are served by snapuserd over mainline UBLK.
PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD := none
# Do NOT let AOSP generate odm.img: our driver bundle (built by
# tools/build-bundle.sh) is that partition. install/BoardConfig.mk points
# BOARD_PREBUILT_ODMIMAGE at the bundle staged outside Soong (see
# SHARED-CHANGES.md, 2026-09-30 ODM staging collision).
$(call inherit-product, $(SRC_TARGET_DIR)/product/virtual_ab_ota/vabc_features.mk)
# The release flag guarding the feature fragment's default is not fixed for
# this product. Select the kernel UBLK backend explicitly, never dm-user.
PRODUCT_VENDOR_PROPERTIES += ro.virtual_ab.ublk.enabled=true
# First-stage init needs this static binary before mounting a snapshotted
# /system. The ordinary snapuserd package above is the second-stage copy.
PRODUCT_PACKAGES += snapuserd_ramdisk
