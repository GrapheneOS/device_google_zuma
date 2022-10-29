PRODUCT_COPY_FILES += \
       device/google/zuma/gnss/config/gps.cer:$(TARGET_COPY_OUT_VENDOR)/etc/gnss/gps.cer

ifneq (,$(filter userdebug eng, $(TARGET_BUILD_VARIANT)))
	PRODUCT_COPY_FILES += \
		device/google/zuma/gnss/config/lhd.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gnss/lhd.conf \
		device/google/zuma/gnss/config/scd.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gnss/scd.conf
else
	PRODUCT_COPY_FILES += \
		device/google/zuma/gnss/config/lhd_user.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gnss/lhd.conf \
		device/google/zuma/gnss/config/scd_user.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gnss/scd.conf
endif

