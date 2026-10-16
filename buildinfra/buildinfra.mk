# Apps remain fixed Soong prebuilts. Driver binaries and libmatonos-ipc are
# assembled from NDK outputs into the ODM partition by tools/build-bundle.sh.
PRODUCT_PACKAGES += MatonOSSettings
PRODUCT_PACKAGES += MatonOSFlathub
# compositor: v4 Wayland compositor host (not privileged)
PRODUCT_PACKAGES += MatonLinuxSubsystem

# v3-stub: system APKs do not get PM extraction. Match the host's bundled
# nativeLibraryDir; updates instead use the executable extracted from the APK.
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/buildinfra/apps-built/libmatonos-v3-loader.so:$(TARGET_COPY_OUT_SYSTEM_EXT)/priv-app/MatonLinuxSubsystem/lib/x86_64/libmatonos-v3-loader.so \
    device/maton/pc_x86_64/buildinfra/apps-built/libmatonos-v3-loader-i386.so:$(TARGET_COPY_OUT_SYSTEM_EXT)/priv-app/MatonLinuxSubsystem/lib/x86_64/libmatonos-v3-loader-i386.so

# remotes-origin: the read-only Flatpak planning helper has the same APK/image layout.
PRODUCT_COPY_FILES += \
    device/maton/pc_x86_64/buildinfra/apps-built/libmatonos-flatpak-plan.so:$(TARGET_COPY_OUT_SYSTEM_EXT)/priv-app/MatonLinuxSubsystem/lib/x86_64/libmatonos-flatpak-plan.so
