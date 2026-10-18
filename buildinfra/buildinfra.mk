# Apps remain fixed Soong prebuilts. Driver binaries and libmatonos-ipc are
# assembled from NDK outputs into the ODM partition by tools/build-bundle.sh.
PRODUCT_PACKAGES += MatonOSSettings
PRODUCT_PACKAGES += MatonOSFlathub
# Linux subsystem: the image carries only its bootstrap (versionCode 1), which
# installs the embedded real APK over itself on first boot; later versions come
# from our F-Droid repo. No native libraries are installed from the image.
PRODUCT_PACKAGES += MatonLinuxSubsystem
