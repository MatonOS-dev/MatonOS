# Input mappings stay in vendor because InputReader searches /vendor/usr;
# matonos-inputd and its init service are shipped in the ODM driver bundle.
LOCAL_PATH := device/maton/pc_x86_64
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/input/keylayout/Vendor_4d54_Product_0001.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/Vendor_4d54_Product_0001.kl \
    $(LOCAL_PATH)/input/keychars/Vendor_4d54_Product_0001.kcm:$(TARGET_COPY_OUT_VENDOR)/usr/keychars/Vendor_4d54_Product_0001.kcm \
    $(LOCAL_PATH)/input/keylayout/Vendor_4d54_Product_0002.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Vendor_4d54_Product_0002.idc
