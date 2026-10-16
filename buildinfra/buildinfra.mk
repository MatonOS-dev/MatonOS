# Apps remain fixed Soong prebuilts. Driver binaries and libmatonos-ipc are
# assembled from NDK outputs into the ODM partition by tools/build-bundle.sh.
PRODUCT_PACKAGES += MatonOSSettings
PRODUCT_PACKAGES += MatonOSFlathub
# compositor: v4 Wayland compositor host (not privileged)
PRODUCT_PACKAGES += MatonLinuxSubsystem

# v3.5: system APKs do not receive PackageManager native-library extraction.
# These files are extracted from the exact pinned APK, in its app-local path.
SUBSYSTEM_NATIVE := device/maton/pc_x86_64/prebuilt/linux-subsystem
PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(SUBSYSTEM_NATIVE)/lib*.so),$(f):$(TARGET_COPY_OUT_SYSTEM_EXT)/priv-app/MatonLinuxSubsystem/lib/x86_64/$(notdir $(f)))
