LOCAL_PATH := device/maton/pc_x86_64

ifeq ($(MATON_RADIO_APEX_READY),true)
PRODUCT_PACKAGES += com.matonos.hardware.radio
endif
