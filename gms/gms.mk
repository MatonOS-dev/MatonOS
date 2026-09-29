# Keep APK inputs present before Soong scans this directory.
MATON_GMS_MISSING := $(filter-out $(notdir $(basename $(wildcard device/maton/pc_x86_64/gms/apks/*.apk))),MatonMicroGmsCore MatonMicroGCompanion MatonMicroGGsfProxy)
ifneq ($(MATON_GMS_MISSING),)
$(error microG APKs missing ($(MATON_GMS_MISSING)): run device/maton/pc_x86_64/gms/fetch-gms.sh)
endif

PRODUCT_PACKAGES += MatonMicroGmsCore MatonMicroGCompanion MatonMicroGGsfProxy
