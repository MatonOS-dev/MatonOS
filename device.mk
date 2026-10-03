#
# Vendor side of pc_x86_64.
#

LOCAL_PATH := device/maton/pc_x86_64

PRODUCT_USE_DYNAMIC_PARTITIONS := true
PRODUCT_SHIPPING_API_LEVEL := 37
PRODUCT_CHARACTERISTICS := tablet

# No OTA packages, and the kernel is built outside AOSP. Giving the build our
# kernel config would make VINTF check it against the ACK-only requirements
# (ashmem, dm-default-key, ...) that mainline can't meet.
PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false

$(call inherit-product, $(SRC_TARGET_DIR)/product/handheld_vendor.mk)
$(call inherit-product, frameworks/native/build/tablet-10in-xhdpi-2048-dalvik-heap.mk)

# ---------------------------------------------------------------- boot / init
# fstab goes into the vendor ramdisk (first-stage mount of system, vendor,
# metadata, ...) and into /vendor/etc for second-stage mount_all.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/fstab.pc_x86_64:$(TARGET_COPY_OUT_VENDOR_RAMDISK)/fstab.pc_x86_64 \
    $(LOCAL_PATH)/fstab.pc_x86_64:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.pc_x86_64 \
    $(LOCAL_PATH)/fstab.pc_x86_64.live:$(TARGET_COPY_OUT_VENDOR_RAMDISK)/fstab.pc_x86_64.live \
    $(LOCAL_PATH)/fstab.pc_x86_64.live:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.pc_x86_64.live \
    $(LOCAL_PATH)/init.pc_x86_64.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.pc_x86_64.rc \
    $(LOCAL_PATH)/ueventd.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc \
    $(LOCAL_PATH)/power/pc-wakeup.sh:$(TARGET_COPY_OUT_VENDOR)/bin/pc-wakeup.sh

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/apps/matonos-controller-permissions.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/permissions/matonos-controller-permissions.xml

# ---------------------------------------------------------------- firmware
# Only the files referenced by our kernel modules, staged by
# tools/build-kernel.sh from linux-firmware. The kernel finds them via
# firmware_class.path=/vendor/firmware (make-payload.sh cmdline).
PC_FIRMWARE_DIR := $(LOCAL_PATH)/prebuilt/firmware
PRODUCT_COPY_FILES += \
    $(foreach f,$(shell test -d $(PC_FIRMWARE_DIR) && cd $(PC_FIRMWARE_DIR) && find . -type f -printf '%P\n'),\
        $(PC_FIRMWARE_DIR)/$(f):$(TARGET_COPY_OUT_VENDOR)/firmware/$(f))

# ---------------------------------------------------------------- features
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/pc_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/pc_core_hardware.xml \
    frameworks/native/data/etc/aosp_excluded_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/aosp_excluded_hardware.xml \
    frameworks/native/data/etc/android.hardware.ethernet.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.ethernet.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.software.freeform_window_management.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.freeform_window_management.xml

# ------------------------------------------------- Xwayland (compositor)
# The Xwayland server and its X11 stack runtime libraries (built by
# linux/compositor/build-xwayland.sh, staged by tools/stage-xwayland.sh).
XWAYLAND_SYSTEM_EXT := device/maton/pc_x86_64/linux/compositor/prebuilt/system_ext
XWAYLAND_FILES := \
    $(wildcard $(XWAYLAND_SYSTEM_EXT)/bin/*) \
    $(wildcard $(XWAYLAND_SYSTEM_EXT)/lib64/*.so) \
    $(wildcard $(XWAYLAND_SYSTEM_EXT)/lib64/*.so.*) \
    $(wildcard $(XWAYLAND_SYSTEM_EXT)/lib64/xwayland/*.so) \
    $(shell find $(XWAYLAND_SYSTEM_EXT)/share -type f 2>/dev/null)
PRODUCT_COPY_FILES += $(foreach f,$(XWAYLAND_FILES),$(f):$(TARGET_COPY_OUT_SYSTEM_EXT)/$(patsubst $(XWAYLAND_SYSTEM_EXT)/%,%,$(f)))

# ---------------------------------------------------------------- HALs
# Software-only implementations from hardware/interfaces; fine for bring-up,
# none of them need a TEE.
PRODUCT_PACKAGES += \
    com.android.hardware.keymint.rust_nonsecure \
    com.android.hardware.gatekeeper.nonsecure \
    android.hardware.health-service.example \
    com.android.hardware.power \
    com.android.hardware.thermal \
    com.android.hardware.usb \
    com.android.hardware.dumpstate \
    com.android.hardware.audio.generic

# Audio: AOSP's example AIDL HAL (software only; real ALSA output is a v1.x
# item, see NOTES.md). What it needs to come up:
# - audio_effects_config.xml, or the effects HAL exits and takes audioserver
#   with it (system_server then blocks in AudioService until the watchdog
#   kills it).
# - audio_policy_configuration.xml: the core HAL creates one IModule per
#   <module>, and audioserver waits for every IModule the HAL's VINTF fragment
#   declares. AOSP's generic file (primary + r_submix) matches the fragment
#   once patches/hardware/interfaces drops Bluetooth audio (not every PC has
#   Bluetooth; detection is a v1.1 item).
# - The plain-named policy modules live in a separate soong namespace and are
#   silently skipped, hence the aosp_* packages plus straight copies.
$(call inherit-product, hardware/interfaces/audio/aidl/default/audio_effects.mk)
$(call inherit-product, frameworks/av/services/audiopolicy/audio_policy_config_vendor_1.mk)
PRODUCT_COPY_FILES += \
    frameworks/av/services/audiopolicy/config/audio_policy_configuration_generic.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml

# Framework config overrides (vendor RRO), see overlays/.
PRODUCT_PACKAGES += \
    matonos_overlay_frameworks_base_core \
    matonos_overlay_settings_provider \
    matonos_overlay_systemui
# Build-time aconfig values (desktop taskbar in fullscreen, multi-desks,
# desktop status bar): release/ extends AOSP's cp2a release config.
PRODUCT_RELEASE_CONFIG_MAPS += device/maton/pc_x86_64/release/release_config_map.textproto
# Home/taskbar/recents = stock Launcher3 (Launcher3QuickStep from
# handheld_system_ext), always desktop-first (user, 2026-09-27). MatonOS
# Shell/Shelf/Recents are parked (sources stay in apps/, not in the image).
# Debug property from WM Shell's DesktopDisplayModeController: forces the
# default display desktop-first regardless of keyboard/touchpad. If a later
# release drops it, the display falls back to the classic touch-first shell.
PRODUCT_PRODUCT_PROPERTIES += \
    persist.wm.debug.force_desktop_first_on_default_display_for_testing=true
# No modem on PCs: drop SIM/carrier-only apps (user, 2026-09-25). Keep
# TeleService and CarrierConfig: the framework and Settings expect them even
# on Wi-Fi-only devices.
PRODUCT_PACKAGES := $(filter-out Stk SimAppDialog CarrierDefaultApp ImsServiceEntitlement,$(PRODUCT_PACKAGES))

# Preinstalled apps from F-Droid (Fennec, Fossify, ...)
$(call inherit-product, $(LOCAL_PATH)/apps/apps.mk)

# (ro.matonos.always_taskbar / shelf_nav / maximize_fullscreen removed with
# the retired Launcher3 and frameworks/base patches; the custom Shell/Shelf
# were retired too.)

# Defaults Android has no config for (F-Droid may install apps), applied
# after boot by a system_ext script (sepolicy/system_ext).
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/setup/matonos-setup.sh:$(TARGET_COPY_OUT_SYSTEM_EXT)/bin/matonos-setup.sh \
    $(LOCAL_PATH)/setup/matonos-setup.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/matonos-setup.rc \
    $(LOCAL_PATH)/media/test-audio/ovmuz.mp3:$(TARGET_COPY_OUT_PRODUCT)/media/matonos/test-audio/ovmuz.mp3 \
    $(LOCAL_PATH)/media/test-audio/final.mp3:$(TARGET_COPY_OUT_PRODUCT)/media/matonos/test-audio/final.mp3

# Graphics: Mesa + minigbm + drm_hwcomposer
$(call inherit-product, $(LOCAL_PATH)/graphics/graphics.mk)

# Areas (hardware + v2 apps), each self-contained in its directory (see BoardConfig.mk).
$(call inherit-product-if-exists, $(LOCAL_PATH)/audio/audio.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/wifi/wifi.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/bluetooth/bluetooth.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/sleep/sleep.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/gms/gms.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/install/install.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/updater/updater.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/fonts/fonts.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/settings/settings.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/buildinfra/buildinfra.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/systembridge/systembridge.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/camera/camera.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/input/input.mk)
$(call inherit-product-if-exists, $(LOCAL_PATH)/bootanim/bootanim.mk)

# flatpak-spike: install the tested bionic CLI/dependency bundle in system_ext
# until its Android.bp Soong port is ready.
$(call inherit-product-if-exists, $(LOCAL_PATH)/linux/flatpak/flatpak.mk)

# addons: product properties/hooks for the signed per-slot driver package
# service. Its executable and init rc are assembled by bundle/contents.list.
$(call inherit-product-if-exists, $(LOCAL_PATH)/addons/addons.mk)

# ---------------------------------------------------------------- props
PRODUCT_VENDOR_PROPERTIES += \
    ro.sf.lcd_density=160

# Bigger log buffers: permissive-mode SELinux audit messages otherwise push
# everything useful out of logcat during bring-up.
PRODUCT_VENDOR_PROPERTIES += ro.logd.size=16M

# Mainline kernels have no /dev/ashmem. Without this, libcutils only uses memfd
# when the kernel has the SELinux memfd_class capability *and* the app targets
# SDK >= 37 (system/core/libcutils/ashmem-dev.cpp), and falls back to ashmem
# otherwise. (system_ext, because generic_system.mk forbids device additions
# to the system partition.)
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    sys.use_memfd=true
