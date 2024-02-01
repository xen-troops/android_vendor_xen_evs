
PRODUCT_PRODUCT_PROPERTIES += \
   persist.automotive.evs.mode=1

PRODUCT_PACKAGES += cardisplayproxyd \
                    evs_app_epam \
                    evsmanagerd \
                    android.hardware.automotive.evs-epam \

# Selinux policies for the reference EVS HAL implementation
BOARD_SEPOLICY_DIRS += vendor/xen/evs/sampleDriver/sepolicy

# EVS HAL implementation for the emulators requires AIDL version of the automotive display
# service implementation.
USE_AIDL_DISPLAY_SERVICE := true

#EVS APP config
PRODUCT_COPY_FILES += \
    vendor/xen/evs/sampleDriver/config/config_override.json:vendor/etc/automotive/evs/config_override.json \

# Selinux policies for the sample EVS Epam application
PRODUCT_PRIVATE_SEPOLICY_DIRS += vendor/xen/evs/app/sepolicy/private

PRODUCT_COPY_FILES += \
    packages/services/Car/cpp/evs/manager/aidl/init.evs.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/init.evs.rc
