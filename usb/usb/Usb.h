/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <android-base/file.h>
#include <aidl/android/hardware/usb/BnUsb.h>
#include <aidl/android/hardware/usb/BnUsbCallback.h>
#include <aidl/android/hardware/usb/ext/BnUsbExt.h>
#include <pixelusb/UsbOverheatEvent.h>
#include <sys/eventfd.h>
#include <utils/Log.h>

#define UEVENT_MSG_LEN 2048
// The type-c stack waits for 4.5 - 5.5 secs before declaring a port non-pd.
// The -partner directory would not be created until this is done.
// Having a margin of ~3 secs for the directory and other related bookeeping
// structures created and uvent fired.
#define PORT_TYPE_TIMEOUT 8
#define DISPLAYPORT_CAPABILITIES_RECEPTACLE_BIT 6
#define DISPLAYPORT_STATUS_DEBOUNCE_MS 2000

namespace aidl {
namespace android {
namespace hardware {
namespace usb {

using ::aidl::android::hardware::usb::IUsbCallback;
using ::aidl::android::hardware::usb::PortRole;
using ::android::base::ReadFileToString;
using ::android::base::WriteStringToFile;
using ::android::base::unique_fd;
using ::android::hardware::google::pixel::usb::UsbOverheatEvent;
using ::android::hardware::google::pixel::usb::ZoneInfo;
using ::android::hardware::thermal::V2_0::TemperatureType;
using ::android::hardware::thermal::V2_0::ThrottlingSeverity;
using ::android::sp;
using ::ndk::ScopedAStatus;
using ::std::shared_ptr;
using ::std::string;
using ::std::thread;

constexpr char kGadgetName[] = "11210000.dwc3";
#define NEW_UDC_PATH "/sys/devices/platform/11210000.usb/"

#define ID_PATH NEW_UDC_PATH "dwc3_exynos_otg_id"
#define VBUS_PATH NEW_UDC_PATH "dwc3_exynos_otg_b_sess"
#define USB_DATA_PATH NEW_UDC_PATH "usb_data_enabled"

#define LINK_TRAINING_STATUS_UNKNOWN "0"
#define LINK_TRAINING_STATUS_SUCCESS "1"
#define LINK_TRAINING_STATUS_FAILURE "2"
#define LINK_TRAINING_STATUS_FAILURE_SINK "3"

#define DISPLAYPORT_SHUTDOWN_CLEAR 0
#define DISPLAYPORT_SHUTDOWN_SET 1
#define DISPLAYPORT_IRQ_HPD_COUNT_CHECK 3

#define DISPLAYPORT_POLL_WAIT_MS 100

struct Usb : public BnUsb {
    Usb();

    ScopedAStatus enableContaminantPresenceDetection(const std::string& in_portName,
            bool in_enable, int64_t in_transactionId) override;
    ScopedAStatus queryPortStatus(int64_t in_transactionId) override;
    ScopedAStatus setCallback(const shared_ptr<IUsbCallback>& in_callback) override;
    ScopedAStatus switchRole(const string& in_portName, const PortRole& in_role,
            int64_t in_transactionId) override;
    ScopedAStatus enableUsbData(const string& in_portName, bool in_enable,
            int64_t in_transactionId) override;
    ScopedAStatus enableUsbDataWhileDocked(const string& in_portName,
            int64_t in_transactionId) override;
    ScopedAStatus limitPowerTransfer(const string& in_portName, bool in_limit,
        int64_t in_transactionId) override;
    ScopedAStatus resetUsbPort(const string& in_portName, int64_t in_transactionId) override;

    Status getDisplayPortUsbPathHelper(string *path);
    Status readDisplayPortAttribute(string attribute, string usb_path, string* value);
    Status writeDisplayPortAttributeOverride(string attribute, string value);
    Status writeDisplayPortAttribute(string attribute, string usb_path);
    bool determineDisplayPortRetry(string linkPath, string hpdPath);
    void setupDisplayPortPoll();
    void shutdownDisplayPortPollHelper();
    void shutdownDisplayPortPoll(bool force);

    std::shared_ptr<::aidl::android::hardware::usb::IUsbCallback> mCallback;
    // Protects mCallback variable
    pthread_mutex_t mLock;
    // Protects roleSwitch operation
    pthread_mutex_t mRoleSwitchLock;
    // Threads waiting for the partner to come back wait here
    pthread_cond_t mPartnerCV;
    // lock protecting mPartnerCV
    pthread_mutex_t mPartnerLock;
    // Variable to signal partner coming back online after type switch
    bool mPartnerUp;

    // Usb Overheat object for push suez event
    UsbOverheatEvent mOverheat;
    // Temperature when connected
    float mPluggedTemperatureCelsius;
    // Usb Data status
    bool mUsbDataEnabled;
    // True when mDisplayPortPoll pthread is running
    volatile bool mDisplayPortPollRunning;
    volatile bool mDisplayPortPollStarting;
    pthread_cond_t mDisplayPortCV;
    pthread_mutex_t mDisplayPortCVLock;
    volatile bool mDisplayPortFirstSetupDone;
    // Used to cache the values read from tcpci's irq_hpd_count.
    // Update drm driver when cached value is not the same as the read value.
    uint32_t mIrqHpdCountCache;

    // Protects writeDisplayPortToExynos(), setupDisplayPortPoll(), and
    // shutdownDisplayPortPoll()
    pthread_mutex_t mDisplayPortLock;
    // eventfd to signal DisplayPort thread
    int mDisplayPortEventPipe;

    /*
     * eventfd to set DisplayPort framework update debounce timer. Debounce timer is necessary for
     *     1) allowing enough time for each sysfs node needed to set HPD high in the drm to populate
     *     2) preventing multiple IRQs that trigger link training failures from continuously
     *        sending notifications to the frameworks layer.
     */
    int mDisplayPortDebounceTimer;
  private:
    pthread_t mPoll;
    pthread_t mDisplayPortPoll;
    pthread_t mDisplayPortShutdownHelper;
};

using ext::PortSecurityState;

struct UsbExt : public ext::BnUsbExt {
    UsbExt(std::shared_ptr<Usb> usb);

    ScopedAStatus setPortSecurityState(const std::string& in_portName,
                                       PortSecurityState in_state) override;

    std::shared_ptr<Usb> mUsb;
};

} // namespace usb
} // namespace hardware
} // namespace android
} // aidl
