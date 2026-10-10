#
# MatonOS preinstalled apps (apps/Android.bp). APKs come from
# tools/fetch-apps.sh; tools/build.sh runs it before building.
#

MATON_APPS := $(shell sed -n 's/^\(Maton[A-Za-z]*\) .*/\1/p' device/maton/pc_x86_64/apps/apps.lock)
MATON_APPS_MISSING := $(filter-out $(notdir $(basename $(wildcard device/maton/pc_x86_64/apps/apks/*.apk))),$(MATON_APPS))
ifneq ($(MATON_APPS_MISSING),)
$(error Preinstalled app APKs missing ($(MATON_APPS_MISSING)): run device/maton/pc_x86_64/tools/fetch-apps.sh)
endif

PRODUCT_PACKAGES += $(MATON_APPS)

# Stock camera/gallery/music remain bundled. Calendar, Clock, Contacts,
# Messaging and Dialer are excluded by MatonSystemBridge's module overrides,
# including copies inherited from the generic AOSP product.
# This checkout has no stock Calculator or Notes module.
PRODUCT_PACKAGES += Camera2 Gallery2 Music

# Empty, MatonOS-signed placeholders reserve these package names for store updates.
PRODUCT_PACKAGES += MatonAuroraPlaceholder MatonYoutubePlaceholder

# Native libraries that an APK stores compressed (e.g. Fennec): preinstalled
# apps get no extraction at install time, so tools/fetch-apps.sh extracts them
# to apks/<module>.lib/ and they go next to the APK, where the package
# manager looks (<app dir>/lib/x86_64). F-Droid updates install to /data and
# extract as usual.
MATON_APPS_DIR := device/maton/pc_x86_64/apps/apks
PRODUCT_COPY_FILES += \
    $(foreach m,$(MATON_APPS),\
        $(foreach f,$(notdir $(wildcard $(MATON_APPS_DIR)/$(m).lib/x86_64/*.so)),\
            $(MATON_APPS_DIR)/$(m).lib/x86_64/$(f):$(TARGET_COPY_OUT_PRODUCT)/app/$(m)/lib/x86_64/$(f)))
