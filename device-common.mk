#
# Copyright (C) 2020 The Android Open-Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

-include vendor/google_devices/zuma/proprietary/telephony/device-vendor.mk
include device/google/zuma/device.mk

# Telephony
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.carrierlock.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.carrierlock.xml

ifneq ($(BOARD_WITHOUT_RADIO),true)
# product permissions XML from stock
PRODUCT_COPY_FILES += \
    device/google/zuma/product-permissions-stock.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/permissions/product-permissions-stock.xml
endif

PRODUCT_COPY_FILES += \
    device/google/zuma/system_ext-permissions-stock.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/permissions/system_ext-permissions-stock.xml

# Android Verified Boot
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.verified_boot.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.verified_boot.xml

# Set system properties identifying the chipset
PRODUCT_VENDOR_PROPERTIES += ro.soc.manufacturer=Google
TARGET_VENDOR_PROP += device/google/zuma/vendor.prop

PRODUCT_PRODUCT_PROPERTIES += \
    persist.vendor.testing_battery_profile=2

# The default value of this variable is false and should only be set to true when
# the device allows users to retain eSIM profiles after factory reset of user data.
PRODUCT_PRODUCT_PROPERTIES += \
    masterclear.allow_retain_esim_profiles_after_fdr=true

# ZramWriteback
-include hardware/google/pixel/mm/device_gki.mk

# Set thermal warm reset
PRODUCT_PRODUCT_PROPERTIES += \
    ro.thermal_warmreset = true

# Set the max page size to 4096 (b/300367402)
PRODUCT_MAX_PAGE_SIZE_SUPPORTED := 4096

# Trigger fsck on upgrade (305658663)
PRODUCT_PRODUCT_PROPERTIES += \
    ro.preventative_fsck = 1

# Indicate that the bootloader supports the MTE developer option switch
# (MISC_MEMTAG_MODE_MEMTAG_ONCE), with the exception of _fullmte products that
# force enable MTE.
ifeq (,$(filter %_fullmte,$(TARGET_PRODUCT)))
PRODUCT_PRODUCT_PROPERTIES += ro.arm64.memtag.bootctl_supported=1
else
PRODUCT_PRODUCT_PROPERTIES += persist.arm64.memtag.app.com.android.chrome=off
endif
