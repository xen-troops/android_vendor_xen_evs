# Exterior View System (EVS)
ENABLE_CAMERA_SERVICE=true
ENABLE_EVS_SAMPLE=true

PRODUCT_PROPERTY_OVERRIDES += \
	persist.vendor.evs.uri="ws://192.168.122.1"

PRODUCT_PACKAGES += \
    android.frameworks.automotive.display@1.0-service \
    android.hardware.automotive.evs@1.1.xt \
    android.automotive.evs.manager@1.1 \
    evs_app_xt \

PRODUCT_PACKAGES += android.hardware.automotive.evs@1.1-xt

PRODUCT_COPY_FILES += \
    vendor/xen/evs/config_override.json:/system/etc/automotive/evs/config_override.json \

include packages/services/Car/cpp/evs/sampleDriver/sepolicy/evsdriver.mk
include vendor/xen/evs/sampleDriver/sepolicy/evsdriver.mk

