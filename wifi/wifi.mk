# Generic Linux nl80211 Wi-Fi stack: Settings / WifiService + wificond,
# wpa_supplicant's AIDL service, and Android's network stack DHCP client.
LOCAL_PATH := device/maton/pc_x86_64

PRODUCT_PACKAGES += \
    wpa_supplicant \
    wificond

# Build the supplicant with upstream's generic nl80211 backend. AOSP's default
# CONFIG_DRIVER_NL80211_QCA backend is for Qualcomm-specific nl80211 commands.
$(call soong_config_set,wpa_supplicant,nl80211_driver,CONFIG_DRIVER_NL80211)
$(call soong_config_set,wpa_supplicant,platform_version,$(PLATFORM_VERSION))
$(call soong_config_set_bool,wpa_supplicant_8,wifi_hidl_unified_supplicant_service_rc_entry,true)

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    $(LOCAL_PATH)/wifi/wpa_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant.conf \
    $(LOCAL_PATH)/wifi/firmware/regulatory.db:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db \
    $(LOCAL_PATH)/wifi/firmware/regulatory.db.p7s:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db.p7s
