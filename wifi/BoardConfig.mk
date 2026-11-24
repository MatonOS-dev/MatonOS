# Wi-Fi policy is consolidated in the shared MatonOS driver domain.
BOARD_WLAN_DEVICE := emulator
# hostapd's Soong module is disabled unless this is set (wpa_build_hostapd);
# without it PRODUCT_PACKAGES += hostapd is silently dropped.
BOARD_HOSTAPD_DRIVER := NL80211
