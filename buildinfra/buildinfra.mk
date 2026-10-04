# Apps remain fixed Soong prebuilts. Driver binaries and libmatonos-ipc are
# assembled from NDK outputs into the ODM partition by tools/build-bundle.sh.
PRODUCT_PACKAGES += MatonOSSettings
PRODUCT_PACKAGES += MatonOSFlathub
# compositor: v4 Wayland compositor host (not privileged)
PRODUCT_PACKAGES += MatonWaylandHost
# linux runtimes: owns the UID for Flatpak SYSTEM installations
PRODUCT_PACKAGES += MatonOSLinuxRuntimes
