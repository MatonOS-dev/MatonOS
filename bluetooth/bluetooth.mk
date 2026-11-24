LOCAL_PATH := device/maton/pc_x86_64

# Bootstrap only: final image imports the HAL from the radio APEX.
ifneq ($(MATON_RADIO_APEX_READY),true)
PRODUCT_PACKAGES += android.hardware.bluetooth-service.matonos
endif
