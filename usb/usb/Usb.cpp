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

#define LOG_TAG "android.hardware.usb.aidl-service"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <assert.h>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <regex>
#include <thread>
#include <unordered_map>

#include <cutils/uevent.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>

#include "Usb.h"

#include <aidl/android/frameworks/stats/IStats.h>
#include <android_hardware_usb_flags.h>
#include <pixelusb/UsbGadgetAidlCommon.h>
#include <pixelstats/StatsHelper.h>

namespace usb_flags = android::hardware::usb::flags;

using aidl::android::frameworks::stats::IStats;
using android::base::GetProperty;
using android::base::Join;
using android::base::ParseUint;
using android::base::Tokenize;
using android::base::Trim;
using android::hardware::google::pixel::getStatsService;
using android::hardware::google::pixel::PixelAtoms::VendorUsbPortOverheat;
using android::hardware::google::pixel::reportUsbPortOverheat;

namespace aidl {
namespace android {
namespace hardware {
namespace usb {
// Set by the signal handler to destroy the thread
volatile bool destroyThread;
volatile bool destroyDisplayPortThread;

string enabledPath;
constexpr char kHsi2cPath[] = "/sys/devices/platform/10cb0000.hsi2c";
constexpr char kI2CPath[] = "/sys/devices/platform/10cb0000.hsi2c/i2c-";
constexpr char kContaminantDetectionPath[] = "-0025/contaminant_detection";
constexpr char kDisplayPortDrmPath[] = "/sys/devices/platform/110f0000.drmdp/drm-displayport/";
constexpr char kDisplayPortUsbPath[] = "/sys/class/typec/port0-partner/";
constexpr char kComplianceWarningsPath[] = "device/non_compliant_reasons";
constexpr char kComplianceWarningBC12[] = "bc12";
constexpr char kComplianceWarningDebugAccessory[] = "debug-accessory";
constexpr char kComplianceWarningMissingRp[] = "missing_rp";
constexpr char kComplianceWarningOther[] = "other";
constexpr char kComplianceWarningInputPowerLimited[] = "input_power_limited";
constexpr char kStatusPath[] = "-0025/contaminant_detection_status";
constexpr char kSinkLimitEnable[] = "-0025/usb_limit_sink_enable";
constexpr char kSourceLimitEnable[] = "-0025/usb_limit_source_enable";
constexpr char kSinkLimitCurrent[] = "-0025/usb_limit_sink_current";
constexpr char kCcToggleEnable[] = "-0025/cc_toggle_enable";
constexpr char kDataPathEnable[] = "-0025/data_path_enable";
constexpr char kTypecPath[] = "/sys/class/typec";
constexpr char kDisableContatminantDetection[] = "vendor.usb.contaminantdisable";
constexpr char kOverheatStatsPath[] = "/sys/devices/platform/google,usbc_port_cooling_dev/";
constexpr char kOverheatStatsDev[] = "DRIVER=google,usbc_port_cooling_dev";
constexpr char kThermalZoneForTrip[] = "VIRTUAL-USB-THROTTLING";
constexpr char kThermalZoneForTempReadPrimary[] = "usb_pwr_therm2";
constexpr char kThermalZoneForTempReadSecondary1[] = "usb_pwr_therm";
constexpr char kThermalZoneForTempReadSecondary2[] = "qi_therm";
constexpr char kPogoUsbActive[] = "/sys/devices/platform/google,pogo/pogo_usb_active";
constexpr char kPogoEnableUsb[] = "/sys/devices/platform/google,pogo/enable_usb";
constexpr char kPowerSupplyUsbType[] = "/sys/class/power_supply/usb/usb_type";
constexpr char kIrqHpdCounPath[] = "-0025/irq_hpd_count";
constexpr char kUdcUeventRegex[] =
    "/devices/platform/11210000.usb/11210000.dwc3/udc/11210000.dwc3";
constexpr char kUdcStatePath[] =
    "/sys/devices/platform/11210000.usb/11210000.dwc3/udc/11210000.dwc3/state";
constexpr char kHost1UeventRegex[] =
    "/devices/platform/11210000.usb/11210000.dwc3/xhci-hcd-exynos.[0-9].auto/usb1/1-0:1.0";
constexpr char kHost1StatePath[] = "/sys/bus/usb/devices/usb1/1-0:1.0/usb1-port1/state";
constexpr char kHost2UeventRegex[] =
    "/devices/platform/11210000.usb/11210000.dwc3/xhci-hcd-exynos.[0-9].auto/usb2/2-0:1.0";
constexpr char kHost2StatePath[] = "/sys/bus/usb/devices/usb2/2-0:1.0/usb2-port1/state";
constexpr char kDataRolePath[] = "/sys/devices/platform/11210000.usb/new_data_role";
constexpr int kSamplingIntervalSec = 5;
void queryVersionHelper(android::hardware::usb::Usb *usb,
                        std::vector<PortStatus> *currentPortStatus);
AltModeData::DisplayPortAltModeData constructAltModeData(string hpd, string pin_assignment,
                                                         string link_status, string vdo);

ScopedAStatus Usb::enableUsbData(const string& in_portName, bool in_enable,
        int64_t in_transactionId) {
    bool result = true;
    std::vector<PortStatus> currentPortStatus;
    string displayPortPartnerPath;

    ALOGI("Userspace turn %s USB data signaling. opID:%ld", in_enable ? "on" : "off",
            in_transactionId);

    if (in_enable) {
        if (!mUsbDataEnabled) {
            if (!WriteStringToFile("1", USB_DATA_PATH)) {
                ALOGE("Not able to turn on usb connection notification");
                result = false;
            }

            if (!WriteStringToFile(kGadgetName, PULLUP_PATH)) {
                ALOGE("Gadget cannot be pulled up");
                result = false;
            }

            if (!WriteStringToFile("1", DISPLAYPORT_ACTIVE_PATH)) {
                ALOGE("Failed to enable DisplayPort Alt Mode on port");
            } else {
                ALOGI("Successfully enabled DisplayPort Alt Mode on port");
            }

            if (getDisplayPortUsbPathHelper(&displayPortPartnerPath) == Status::SUCCESS) {
                size_t pos = displayPortPartnerPath.find("/displayport");
                if (pos != string::npos) {
                    displayPortPartnerPath = displayPortPartnerPath.substr(0, pos) + "/mode1/active";
                }
                if (!WriteStringToFile("1", displayPortPartnerPath)) {
                    ALOGE("Failed to enable DisplayPort Alt Mode on partner at %s",
                            displayPortPartnerPath.c_str());
                } else {
                    ALOGI("Successfully enabled DisplayPort Alt Mode on partner at %s",
                            displayPortPartnerPath.c_str());
                    setupDisplayPortPoll();
                }
            }
        }
    } else {
        if (!WriteStringToFile("1", ID_PATH)) {
            ALOGE("Not able to turn off host mode");
            result = false;
        }

        if (!WriteStringToFile("0", VBUS_PATH)) {
            ALOGE("Not able to set Vbus state");
            result = false;
        }

        if (!WriteStringToFile("0", USB_DATA_PATH)) {
            ALOGE("Not able to turn off usb connection notification");
            result = false;
        }

        if (!WriteStringToFile("none", PULLUP_PATH)) {
            ALOGE("Gadget cannot be pulled down");
            result = false;
        }

        if (getDisplayPortUsbPathHelper(&displayPortPartnerPath) == Status::SUCCESS) {
            size_t pos = displayPortPartnerPath.find("/displayport");
            if (pos != string::npos) {
                displayPortPartnerPath = displayPortPartnerPath.substr(0, pos) + "/mode1/active";
            }
            if (!WriteStringToFile("0", displayPortPartnerPath)) {
                ALOGE("Failed to disable DisplayPort Alt Mode on partner at %s",
                        displayPortPartnerPath.c_str());
            } else {
                ALOGI("Successfully disabled DisplayPort Alt Mode on partner at %s",
                        displayPortPartnerPath.c_str());
                shutdownDisplayPortPoll(true);
            }
        }

        if (!WriteStringToFile("0", DISPLAYPORT_ACTIVE_PATH)) {
            ALOGE("Failed to disable DisplayPort Alt Mode on port");
        } else {
            ALOGI("Successfully disabled DisplayPort Alt Mode on port");
        }
    }

    if (result) {
        mUsbDataEnabled = in_enable;
    }
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyEnableUsbDataStatus(
            in_portName, in_enable, result ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyEnableUsbDataStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    queryVersionHelper(this, &currentPortStatus);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::enableUsbDataWhileDocked(const string& in_portName,
        int64_t in_transactionId) {
    bool success = true;
    bool notSupported = true;
    std::vector<PortStatus> currentPortStatus;

    ALOGI("Userspace enableUsbDataWhileDocked  opID:%ld", in_transactionId);

    int flags = O_RDONLY;
    ::android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(kPogoEnableUsb, flags)));
    if (fd != -1) {
        notSupported = false;
        success = WriteStringToFile("1", kPogoEnableUsb);
        if (!success) {
            ALOGE("Write to enable_usb failed");
        }
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyEnableUsbDataWhileDockedStatus(
                in_portName, notSupported ? Status::NOT_SUPPORTED :
                success ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyEnableUsbDataStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    queryVersionHelper(this, &currentPortStatus);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::resetUsbPort(const std::string& in_portName, int64_t in_transactionId) {
    bool result = true;
    std::vector<PortStatus> currentPortStatus;

    ALOGI("Userspace reset USB Port. opID:%ld", in_transactionId);

    if (!WriteStringToFile("none", PULLUP_PATH)) {
        ALOGI("Gadget cannot be pulled down");
        result = false;
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ::ndk::ScopedAStatus ret = mCallback->notifyResetUsbPortStatus(
            in_portName, result ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyTransactionStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ::ndk::ScopedAStatus::ok();
}

Status getI2cBusHelper(string *name) {
    DIR *dp;

    dp = opendir(kHsi2cPath);
    if (dp != NULL) {
        struct dirent *ep;

        while ((ep = readdir(dp))) {
            if (ep->d_type == DT_DIR) {
                if (string::npos != string(ep->d_name).find("i2c-")) {
                    std::strtok(ep->d_name, "-");
                    *name = std::strtok(NULL, "-");
                }
            }
        }
        closedir(dp);
        return Status::SUCCESS;
    }

    ALOGE("Failed to open %s", kHsi2cPath);
    return Status::ERROR;
}

Status queryMoistureDetectionStatus(std::vector<PortStatus> *currentPortStatus) {
    string enabled, status, path, DetectedPath;

    (*currentPortStatus)[0].supportedContaminantProtectionModes
            .push_back(ContaminantProtectionMode::FORCE_DISABLE);
    (*currentPortStatus)[0].contaminantProtectionStatus = ContaminantProtectionStatus::NONE;
    (*currentPortStatus)[0].contaminantDetectionStatus = ContaminantDetectionStatus::DISABLED;
    (*currentPortStatus)[0].supportsEnableContaminantPresenceDetection = true;
    (*currentPortStatus)[0].supportsEnableContaminantPresenceProtection = false;

    getI2cBusHelper(&path);
    enabledPath = kI2CPath + path + "/" + path + kContaminantDetectionPath;
    if (!ReadFileToString(enabledPath, &enabled)) {
        ALOGE("Failed to open moisture_detection_enabled");
        return Status::ERROR;
    }

    enabled = Trim(enabled);
    if (enabled == "1") {
        DetectedPath = kI2CPath + path + "/" + path + kStatusPath;
        if (!ReadFileToString(DetectedPath, &status)) {
            ALOGE("Failed to open moisture_detected");
            return Status::ERROR;
        }
        status = Trim(status);
        if (status == "1") {
            (*currentPortStatus)[0].contaminantDetectionStatus =
                ContaminantDetectionStatus::DETECTED;
            (*currentPortStatus)[0].contaminantProtectionStatus =
                ContaminantProtectionStatus::FORCE_DISABLE;
        } else {
            (*currentPortStatus)[0].contaminantDetectionStatus =
                ContaminantDetectionStatus::NOT_DETECTED;
        }
    }

    ALOGI("ContaminantDetectionStatus:%d ContaminantProtectionStatus:%d",
            (*currentPortStatus)[0].contaminantDetectionStatus,
            (*currentPortStatus)[0].contaminantProtectionStatus);

    return Status::SUCCESS;
}

Status queryNonCompliantChargerStatus(std::vector<PortStatus> *currentPortStatus) {
    string reasons, path;

    for (int i = 0; i < currentPortStatus->size(); i++) {
        (*currentPortStatus)[i].supportsComplianceWarnings = true;
        path = string(kTypecPath) + "/" + (*currentPortStatus)[i].portName + "/" +
                string(kComplianceWarningsPath);
        if (ReadFileToString(path.c_str(), &reasons)) {
            std::vector<string> reasonsList = Tokenize(reasons.c_str(), "[], \n\0");
            for (string reason : reasonsList) {
                if (!strncmp(reason.c_str(), kComplianceWarningDebugAccessory,
                            strlen(kComplianceWarningDebugAccessory))) {
                    (*currentPortStatus)[i].complianceWarnings.push_back(ComplianceWarning::DEBUG_ACCESSORY);
                    continue;
                }
                if (!strncmp(reason.c_str(), kComplianceWarningBC12,
                            strlen(kComplianceWarningBC12))) {
                    (*currentPortStatus)[i].complianceWarnings.push_back(ComplianceWarning::BC_1_2);
                    continue;
                }
                if (!strncmp(reason.c_str(), kComplianceWarningMissingRp,
                            strlen(kComplianceWarningMissingRp))) {
                    (*currentPortStatus)[i].complianceWarnings.push_back(ComplianceWarning::MISSING_RP);
                    continue;
                }
                if (!strncmp(reason.c_str(), kComplianceWarningOther,
                             strlen(kComplianceWarningOther)) ||
                    !strncmp(reason.c_str(), kComplianceWarningInputPowerLimited,
                             strlen(kComplianceWarningInputPowerLimited))) {
                    if (usb_flags::enable_usb_data_compliance_warning() &&
                        usb_flags::enable_input_power_limited_warning()) {
                        ALOGI("Report through INPUT_POWER_LIMITED warning");
                        (*currentPortStatus)[i].complianceWarnings.push_back(
                            ComplianceWarning::INPUT_POWER_LIMITED);
                        continue;
                    } else {
                        (*currentPortStatus)[i].complianceWarnings.push_back(
                            ComplianceWarning::OTHER);
                        continue;
                    }
                }
            }
            if ((*currentPortStatus)[i].complianceWarnings.size() > 0 &&
                 (*currentPortStatus)[i].currentPowerRole == PortPowerRole::NONE) {
                (*currentPortStatus)[i].currentMode = PortMode::UFP;
                (*currentPortStatus)[i].currentPowerRole = PortPowerRole::SINK;
                (*currentPortStatus)[i].currentDataRole = PortDataRole::NONE;
                (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::CONNECTED;
            }
        }
    }
    return Status::SUCCESS;
}

string appendRoleNodeHelper(const string &portName, PortRole::Tag tag) {
    string node("/sys/class/typec/" + portName);

    switch (tag) {
        case PortRole::dataRole:
            return node + "/data_role";
        case PortRole::powerRole:
            return node + "/power_role";
        case PortRole::mode:
            return node + "/port_type";
        default:
            return "";
    }
}

string convertRoletoString(PortRole role) {
    if (role.getTag() == PortRole::powerRole) {
        if (role.get<PortRole::powerRole>() == PortPowerRole::SOURCE)
            return "source";
        else if (role.get<PortRole::powerRole>() == PortPowerRole::SINK)
            return "sink";
    } else if (role.getTag() == PortRole::dataRole) {
        if (role.get<PortRole::dataRole>() == PortDataRole::HOST)
            return "host";
        if (role.get<PortRole::dataRole>() == PortDataRole::DEVICE)
            return "device";
    } else if (role.getTag() == PortRole::mode) {
        if (role.get<PortRole::mode>() == PortMode::UFP)
            return "sink";
        if (role.get<PortRole::mode>() == PortMode::DFP)
            return "source";
    }
    return "none";
}

void extractRole(string *roleName) {
    std::size_t first, last;

    first = roleName->find("[");
    last = roleName->find("]");

    if (first != string::npos && last != string::npos) {
        *roleName = roleName->substr(first + 1, last - first - 1);
    }
}

void switchToDrp(const string &portName) {
    string filename = appendRoleNodeHelper(string(portName.c_str()), PortRole::mode);
    FILE *fp;

    if (filename != "") {
        fp = fopen(filename.c_str(), "w");
        if (fp != NULL) {
            int ret = fputs("dual", fp);
            fclose(fp);
            if (ret == EOF)
                ALOGE("Fatal: Error while switching back to drp");
        } else {
            ALOGE("Fatal: Cannot open file to switch back to drp");
        }
    } else {
        ALOGE("Fatal: invalid node type");
    }
}

bool switchMode(const string &portName, const PortRole &in_role, struct Usb *usb) {
    string filename = appendRoleNodeHelper(string(portName.c_str()), in_role.getTag());
    string written;
    FILE *fp;
    bool roleSwitch = false;

    if (filename == "") {
        ALOGE("Fatal: invalid node type");
        return false;
    }

    fp = fopen(filename.c_str(), "w");
    if (fp != NULL) {
        // Hold the lock here to prevent loosing connected signals
        // as once the file is written the partner added signal
        // can arrive anytime.
        pthread_mutex_lock(&usb->mPartnerLock);
        usb->mPartnerUp = false;
        int ret = fputs(convertRoletoString(in_role).c_str(), fp);
        fclose(fp);

        if (ret != EOF) {
            struct timespec to;
            struct timespec now;

        wait_again:
            clock_gettime(CLOCK_MONOTONIC, &now);
            to.tv_sec = now.tv_sec + PORT_TYPE_TIMEOUT;
            to.tv_nsec = now.tv_nsec;

            int err = pthread_cond_timedwait(&usb->mPartnerCV, &usb->mPartnerLock, &to);
            // There are no uevent signals which implies role swap timed out.
            if (err == ETIMEDOUT) {
                ALOGI("uevents wait timedout");
                // Validity check.
            } else if (!usb->mPartnerUp) {
                goto wait_again;
                // Role switch succeeded since usb->mPartnerUp is true.
            } else {
                roleSwitch = true;
            }
        } else {
            ALOGI("Role switch failed while wrting to file");
        }
        pthread_mutex_unlock(&usb->mPartnerLock);
    }

    if (!roleSwitch)
        switchToDrp(string(portName.c_str()));

    return roleSwitch;
}

void updatePortStatus(android::hardware::usb::Usb *usb) {
    std::vector<PortStatus> currentPortStatus;

    queryVersionHelper(usb, &currentPortStatus);
}

Usb::Usb()
    : mLock(PTHREAD_MUTEX_INITIALIZER),
      mRoleSwitchLock(PTHREAD_MUTEX_INITIALIZER),
      mPartnerLock(PTHREAD_MUTEX_INITIALIZER),
      mPartnerUp(false),
      mUsbDataSessionMonitor(kUdcUeventRegex, kUdcStatePath, kHost1UeventRegex, kHost1StatePath,
                             kHost2UeventRegex, kHost2StatePath, kDataRolePath,
                             std::bind(&updatePortStatus, this)),
      mOverheat(ZoneInfo(TemperatureType::USB_PORT, kThermalZoneForTrip,
                         ThrottlingSeverity::CRITICAL),
                {ZoneInfo(TemperatureType::UNKNOWN, kThermalZoneForTempReadPrimary,
                          ThrottlingSeverity::NONE),
                 ZoneInfo(TemperatureType::UNKNOWN, kThermalZoneForTempReadSecondary1,
                          ThrottlingSeverity::NONE),
                 ZoneInfo(TemperatureType::UNKNOWN, kThermalZoneForTempReadSecondary2,
                          ThrottlingSeverity::NONE)}, kSamplingIntervalSec),
      mUsbDataEnabled(true),
      mDisplayPortPollRunning(false),
      mDisplayPortPollStarting(false),
      mDisplayPortCVLock(PTHREAD_MUTEX_INITIALIZER),
      mDisplayPortLock(PTHREAD_MUTEX_INITIALIZER) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) {
        ALOGE("pthread_condattr_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
        ALOGE("pthread_condattr_setclock failed: %s", strerror(errno));
        abort();
    }
    if (pthread_cond_init(&mPartnerCV, &attr)) {
        ALOGE("pthread_cond_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_cond_init(&mDisplayPortCV, &attr)) {
        ALOGE("usbdp: pthread_cond_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_condattr_destroy(&attr)) {
        ALOGE("pthread_condattr_destroy failed: %s", strerror(errno));
        abort();
    }
    mDisplayPortEventPipe = eventfd(0, EFD_NONBLOCK);
    if (mDisplayPortEventPipe == -1) {
        ALOGE("mDisplayPortEventPipe eventfd failed: %s", strerror(errno));
        abort();
    }
    mDisplayPortDebounceTimer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (mDisplayPortDebounceTimer == -1) {
        ALOGE("mDisplayPortDebounceTimer timerfd failed: %s", strerror(errno));
        abort();
    }
    mDisplayPortActivateTimer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (mDisplayPortActivateTimer == -1) {
        ALOGE("mDisplayPortActivateTimer timerfd failed: %s", strerror(errno));
        abort();
    }
}

ScopedAStatus Usb::switchRole(const string& in_portName, const PortRole& in_role,
        int64_t in_transactionId) {
    string filename = appendRoleNodeHelper(string(in_portName.c_str()), in_role.getTag());
    string written;
    FILE *fp;
    bool roleSwitch = false;

    if (filename == "") {
        ALOGE("Fatal: invalid node type");
        return ScopedAStatus::ok();
    }

    pthread_mutex_lock(&mRoleSwitchLock);

    ALOGI("filename write: %s role:%s", filename.c_str(), convertRoletoString(in_role).c_str());

    if (in_role.getTag() == PortRole::mode) {
        roleSwitch = switchMode(in_portName, in_role, this);
    } else {
        fp = fopen(filename.c_str(), "w");
        if (fp != NULL) {
            int ret = fputs(convertRoletoString(in_role).c_str(), fp);
            fclose(fp);
            if ((ret != EOF) && ReadFileToString(filename, &written)) {
                written = Trim(written);
                extractRole(&written);
                ALOGI("written: %s", written.c_str());
                if (written == convertRoletoString(in_role)) {
                    roleSwitch = true;
                } else {
                    ALOGE("Role switch failed");
                }
            } else {
                ALOGE("failed to update the new role");
            }
        } else {
            ALOGE("fopen failed");
        }
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
         ScopedAStatus ret = mCallback->notifyRoleSwitchStatus(
            in_portName, in_role, roleSwitch ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("RoleSwitchStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    pthread_mutex_unlock(&mRoleSwitchLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::limitPowerTransfer(const string& in_portName, bool in_limit,
        int64_t in_transactionId) {
    bool sessionFail = false, success;
    std::vector<PortStatus> currentPortStatus;
    string path, sinkLimitEnablePath, currentLimitPath, sourceLimitEnablePath;

    getI2cBusHelper(&path);
    sinkLimitEnablePath = kI2CPath + path + "/" + path + kSinkLimitEnable;
    currentLimitPath = kI2CPath + path + "/" + path + kSinkLimitCurrent;
    sourceLimitEnablePath = kI2CPath + path + "/" + path + kSourceLimitEnable;

    pthread_mutex_lock(&mLock);
    if (in_limit) {
        success = WriteStringToFile("0", currentLimitPath);
        if (!success) {
            ALOGE("Failed to set sink current limit");
            sessionFail = true;
        }
    }
    success = WriteStringToFile(in_limit ? "1" : "0", sinkLimitEnablePath);
    if (!success) {
        ALOGE("Failed to %s sink current limit: %s", in_limit ? "enable" : "disable",
              sinkLimitEnablePath.c_str());
        sessionFail = true;
    }
    success = WriteStringToFile(in_limit ? "1" : "0", sourceLimitEnablePath);
    if (!success) {
        ALOGE("Failed to %s source current limit: %s", in_limit ? "enable" : "disable",
              sourceLimitEnablePath.c_str());
        sessionFail = true;
    }

    ALOGI("limitPowerTransfer limit:%c opId:%ld", in_limit ? 'y' : 'n', in_transactionId);
    if (mCallback != NULL && in_transactionId >= 0) {
        ScopedAStatus ret = mCallback->notifyLimitPowerTransferStatus(
                in_portName, in_limit, sessionFail ? Status::ERROR : Status::SUCCESS,
                in_transactionId);
        if (!ret.isOk())
            ALOGE("limitPowerTransfer error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }

    pthread_mutex_unlock(&mLock);
    queryVersionHelper(this, &currentPortStatus);

    return ScopedAStatus::ok();
}

Status queryPowerTransferStatus(std::vector<PortStatus> *currentPortStatus) {
    string limitedPath, enabled, path;

    getI2cBusHelper(&path);
    limitedPath = kI2CPath + path + "/" + path + kSinkLimitEnable;
    if (!ReadFileToString(limitedPath, &enabled)) {
        ALOGE("Failed to open limit_sink_enable");
        return Status::ERROR;
    }

    enabled = Trim(enabled);
    (*currentPortStatus)[0].powerTransferLimited = enabled == "1";

    ALOGI("powerTransferLimited:%d", (*currentPortStatus)[0].powerTransferLimited ? 1 : 0);
    return Status::SUCCESS;
}

Status getAccessoryConnected(const string &portName, string *accessory) {
    string filename = "/sys/class/typec/" + portName + "-partner/accessory_mode";

    if (!ReadFileToString(filename, accessory)) {
        ALOGE("getAccessoryConnected: Failed to open filesystem node: %s", filename.c_str());
        return Status::ERROR;
    }
    *accessory = Trim(*accessory);

    return Status::SUCCESS;
}

Status getCurrentRoleHelper(const string &portName, bool connected, PortRole *currentRole) {
    string filename;
    string roleName;
    string accessory;

    // Mode

    if (currentRole->getTag() == PortRole::powerRole) {
        filename = "/sys/class/typec/" + portName + "/power_role";
        currentRole->set<PortRole::powerRole>(PortPowerRole::NONE);
    } else if (currentRole->getTag() == PortRole::dataRole) {
        filename = "/sys/class/typec/" + portName + "/data_role";
        currentRole->set<PortRole::dataRole>(PortDataRole::NONE);
    } else if (currentRole->getTag() == PortRole::mode) {
        filename = "/sys/class/typec/" + portName + "/data_role";
        currentRole->set<PortRole::mode>(PortMode::NONE);
    } else {
        return Status::ERROR;
    }

    if (!connected)
        return Status::SUCCESS;

    if (currentRole->getTag() == PortRole::mode) {
        if (getAccessoryConnected(portName, &accessory) != Status::SUCCESS) {
            return Status::ERROR;
        }
        if (accessory == "analog_audio") {
            currentRole->set<PortRole::mode>(PortMode::AUDIO_ACCESSORY);
            return Status::SUCCESS;
        } else if (accessory == "debug") {
            currentRole->set<PortRole::mode>(PortMode::DEBUG_ACCESSORY);
            return Status::SUCCESS;
        }
    }

    if (!ReadFileToString(filename, &roleName)) {
        ALOGE("getCurrentRole: Failed to open filesystem node: %s", filename.c_str());
        return Status::ERROR;
    }

    roleName = Trim(roleName);
    extractRole(&roleName);

    if (roleName == "source") {
        currentRole->set<PortRole::powerRole>(PortPowerRole::SOURCE);
    } else if (roleName == "sink") {
        currentRole->set<PortRole::powerRole>(PortPowerRole::SINK);
    } else if (roleName == "host") {
        if (currentRole->getTag() == PortRole::dataRole)
            currentRole->set<PortRole::dataRole>(PortDataRole::HOST);
        else
            currentRole->set<PortRole::mode>(PortMode::DFP);
    } else if (roleName == "device") {
        if (currentRole->getTag() == PortRole::dataRole)
            currentRole->set<PortRole::dataRole>(PortDataRole::DEVICE);
        else
            currentRole->set<PortRole::mode>(PortMode::UFP);
    } else if (roleName != "none") {
        /* case for none has already been addressed.
         * so we check if the role isn't none.
         */
        return Status::UNRECOGNIZED_ROLE;
    }
    return Status::SUCCESS;
}

Status getTypeCPortNamesHelper(std::unordered_map<string, bool> *names) {
    DIR *dp;

    dp = opendir(kTypecPath);
    if (dp != NULL) {
        struct dirent *ep;

        while ((ep = readdir(dp))) {
            if (ep->d_type == DT_LNK) {
                if (string::npos == string(ep->d_name).find("-partner")) {
                    std::unordered_map<string, bool>::const_iterator portName =
                        names->find(ep->d_name);
                    if (portName == names->end()) {
                        names->insert({ep->d_name, false});
                    }
                } else {
                    (*names)[std::strtok(ep->d_name, "-")] = true;
                }
            }
        }
        closedir(dp);
        return Status::SUCCESS;
    }

    ALOGE("Failed to open /sys/class/typec");
    return Status::ERROR;
}

bool canSwitchRoleHelper(const string &portName) {
    string filename = "/sys/class/typec/" + portName + "-partner/supports_usb_power_delivery";
    string supportsPD;

    if (ReadFileToString(filename, &supportsPD)) {
        supportsPD = Trim(supportsPD);
        if (supportsPD == "yes") {
            return true;
        }
    }

    return false;
}

Status getPortStatusHelper(android::hardware::usb::Usb *usb,
        std::vector<PortStatus> *currentPortStatus) {
    std::unordered_map<string, bool> names;
    Status result = getTypeCPortNamesHelper(&names);
    int i = -1;

    if (result == Status::SUCCESS) {
        currentPortStatus->resize(names.size());
        for (std::pair<string, bool> port : names) {
            i++;
            ALOGI("%s", port.first.c_str());
            (*currentPortStatus)[i].portName = port.first;

            PortRole currentRole;
            currentRole.set<PortRole::powerRole>(PortPowerRole::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS){
                (*currentPortStatus)[i].currentPowerRole = currentRole.get<PortRole::powerRole>();
            } else {
                ALOGE("Error while retrieving portNames");
                goto done;
            }

            currentRole.set<PortRole::dataRole>(PortDataRole::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS) {
                (*currentPortStatus)[i].currentDataRole = currentRole.get<PortRole::dataRole>();
            } else {
                ALOGE("Error while retrieving current port role");
                goto done;
            }

            currentRole.set<PortRole::mode>(PortMode::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS) {
                (*currentPortStatus)[i].currentMode = currentRole.get<PortRole::mode>();
            } else {
                ALOGE("Error while retrieving current data role");
                goto done;
            }

            (*currentPortStatus)[i].canChangeMode = true;
            (*currentPortStatus)[i].canChangeDataRole =
                port.second ? canSwitchRoleHelper(port.first) : false;
            (*currentPortStatus)[i].canChangePowerRole =
                port.second ? canSwitchRoleHelper(port.first) : false;

            (*currentPortStatus)[i].supportedModes.push_back(PortMode::DRP);

            bool dataEnabled = true;
            string pogoUsbActive = "0";
            if (ReadFileToString(string(kPogoUsbActive), &pogoUsbActive) &&
                stoi(Trim(pogoUsbActive)) == 1) {
                (*currentPortStatus)[i].usbDataStatus.push_back(UsbDataStatus::DISABLED_DOCK);
                dataEnabled = false;
            }
            if (!usb->mUsbDataEnabled) {
                (*currentPortStatus)[i].usbDataStatus.push_back(UsbDataStatus::DISABLED_FORCE);
                dataEnabled = false;
            }
            if (dataEnabled) {
                (*currentPortStatus)[i].usbDataStatus.push_back(UsbDataStatus::ENABLED);
            }

            // When connected return powerBrickStatus
            if (port.second) {
                string usbType;
                if (ReadFileToString(string(kPowerSupplyUsbType), &usbType)) {
                    if (strstr(usbType.c_str(), "[D")) {
                        (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::CONNECTED;
                    } else if (strstr(usbType.c_str(), "[U")) {
                        (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::UNKNOWN;
                    } else {
                        (*currentPortStatus)[i].powerBrickStatus =
                                PowerBrickStatus::NOT_CONNECTED;
                    }
                } else {
                    ALOGE("Error while reading usb_type");
                }
            } else {
                (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::NOT_CONNECTED;
            }

            ALOGI("%d:%s connected:%d canChangeMode:%d canChagedata:%d canChangePower:%d "
                  "usbDataEnabled:%d",
                i, port.first.c_str(), port.second,
                (*currentPortStatus)[i].canChangeMode,
                (*currentPortStatus)[i].canChangeDataRole,
                (*currentPortStatus)[i].canChangePowerRole,
                dataEnabled ? 1 : 0);
        }

        return Status::SUCCESS;
    }
done:
    return Status::ERROR;
}

/* DisplayPort Helper Functions Start */

DisplayPortAltModePinAssignment parsePinAssignmentHelper(string pinAssignments) {
    size_t pos = pinAssignments.find("[");
    if (pos != string::npos) {
        pinAssignments = pinAssignments.substr(pos+1, 1);
        if (pinAssignments == "C") {
            return DisplayPortAltModePinAssignment::C;
        } else if (pinAssignments == "D") {
            return DisplayPortAltModePinAssignment::D;
        } else if (pinAssignments == "E") {
            return DisplayPortAltModePinAssignment::E;
        }
    }
    return DisplayPortAltModePinAssignment::NONE;
}

LinkTrainingStatus parseLinkTrainingStatusHelper(string linkTrainingStatus) {
    linkTrainingStatus = Trim(linkTrainingStatus);
    if (linkTrainingStatus == LINK_TRAINING_STATUS_SUCCESS) {
        return LinkTrainingStatus::SUCCESS;
    } else if (linkTrainingStatus == LINK_TRAINING_STATUS_FAILURE || \
               linkTrainingStatus == LINK_TRAINING_STATUS_FAILURE_SINK) {
        return LinkTrainingStatus::FAILURE;
    }
    return LinkTrainingStatus::UNKNOWN;
}

bool isDisplayPortPlugHelper(string vdoString) {
    unsigned long vdo;
    unsigned long receptacleFlag = 1 << DISPLAYPORT_CAPABILITIES_RECEPTACLE_BIT;

    vdoString = Trim(vdoString);
    if (ParseUint(vdoString.c_str(), &vdo)) {
        /* We check to see if receptacleFlag is 0, meaning that the DP interface is presented on a
         * USB-C plug.
         */
        return !(vdo & receptacleFlag);
    } else {
        ALOGE("usbdp: isDisplayPortPlugHelper: errno:%d", errno);
    }

    return false;
}

AltModeData::DisplayPortAltModeData constructAltModeData(string hpd, string pin_assignment,
                                                         string link_status, string vdo) {
    AltModeData::DisplayPortAltModeData dpData;

    // vdo
    if (isDisplayPortPlugHelper(vdo)) {
        dpData.cableStatus = DisplayPortAltModeStatus::CAPABLE;
    } else {
        dpData.partnerSinkStatus = DisplayPortAltModeStatus::CAPABLE;
    }

    // hpd, status
     if (!strncmp(hpd.c_str(), "1", strlen("1"))) {
        dpData.hpd = true;
    }

    // pin
    dpData.pinAssignment = parsePinAssignmentHelper(pin_assignment);

    // link training
    link_status = Trim(link_status);
    dpData.linkTrainingStatus = parseLinkTrainingStatusHelper(link_status);
    if (dpData.linkTrainingStatus == LinkTrainingStatus::SUCCESS) {
        dpData.partnerSinkStatus = dpData.partnerSinkStatus == DisplayPortAltModeStatus::CAPABLE ? \
                DisplayPortAltModeStatus::ENABLED : DisplayPortAltModeStatus::UNKNOWN;
        dpData.cableStatus = dpData.cableStatus == DisplayPortAltModeStatus::CAPABLE ? \
                DisplayPortAltModeStatus::ENABLED : DisplayPortAltModeStatus::UNKNOWN;
        if (dpData.partnerSinkStatus == DisplayPortAltModeStatus::ENABLED) {
            dpData.cableStatus = DisplayPortAltModeStatus::ENABLED;
        }
    } else if (dpData.linkTrainingStatus == LinkTrainingStatus::FAILURE &&
               dpData.partnerSinkStatus == DisplayPortAltModeStatus::CAPABLE) {
        // 2.0 cable that fails EDID reports not capable, other link training failures assume
        // 3.0 cable that fails in all other cases.
        dpData.cableStatus = (link_status == LINK_TRAINING_STATUS_FAILURE_SINK) ? \
                DisplayPortAltModeStatus::NOT_CAPABLE : DisplayPortAltModeStatus::CAPABLE;
    }

    return dpData;
}

Status queryPartnerSvids(std::vector<string> *svids) {
    DIR *dp;

    dp = opendir(kDisplayPortUsbPath);
    if (dp != NULL) {
        struct dirent *ep;
        // Iterate through directories for Alt Mode SVIDs
        while ((ep = readdir(dp))) {
            if (ep->d_type == DT_DIR) {
                string svid;
                string portPartnerPath = string(kDisplayPortUsbPath) + string(ep->d_name) + "/svid";
                if (ReadFileToString(portPartnerPath, &svid)) {
                    (*svids).push_back(Trim(svid));
                }
            }
        }
        closedir(dp);
    } else {
        return Status::ERROR;
    }
    return Status::SUCCESS;
}

/* DisplayPort Helper Functions End */

// Only care about first port which must support DisplayPortAltMode
Status queryDisplayPortStatus(android::hardware::usb::Usb *usb,
                              std::vector<PortStatus> *currentPortStatus) {
    string hpd, pinAssign, linkStatus, vdo;
    string path;
    AltModeData::DisplayPortAltModeData dpData;

    /*
    * We check if the DisplayPort Alt Mode sysfs nodes exist. If they don't, then it means that the
    * device has not entered Alt Mode with the port partner because of a source/sink role
    * incompatibility, pin assignment incompatibility, etc. So, we then check to see if the partner
    * supports Thunderbolt and DisplayPort SVIDs. If it supports DisplayPort, then we assume that
    * it must be a source device and Thunderbolt should operate similarly; we don't populate the
    * DisplayPortAltModeStatus. If it only supports Thunderbolt, then we cannot determine if it is
    * sink or source capable, and need to notify the user.
    */
    if (usb->getDisplayPortUsbPathHelper(&path) == Status::ERROR) {
        std::vector<string> svids;
        if (queryPartnerSvids(&svids) == Status::SUCCESS) {
            if (std::count(svids.begin(), svids.end(), SVID_THUNDERBOLT) &&
                !std::count(svids.begin(), svids.end(), SVID_DISPLAYPORT)) {
                dpData.cableStatus = DisplayPortAltModeStatus::NOT_CAPABLE;
            }
        }
    } else {
        usb->readDisplayPortAttribute("hpd", path, &hpd);
        usb->readDisplayPortAttribute("pin_assignment", path, &pinAssign);
        usb->readDisplayPortAttribute("vdo", path, &vdo);
        usb->readDisplayPortAttribute("link_status", path, &linkStatus);

        dpData = constructAltModeData(hpd, pinAssign, linkStatus, vdo);
    }

    (*currentPortStatus)[0].supportedAltModes.push_back(dpData);

    return Status::SUCCESS;
}

void queryUsbDataSession(android::hardware::usb::Usb *usb,
                          std::vector<PortStatus> *currentPortStatus) {
    std::vector<ComplianceWarning> warnings;

    usb->mUsbDataSessionMonitor.getComplianceWarnings(
        (*currentPortStatus)[0].currentDataRole, &warnings);
    (*currentPortStatus)[0].complianceWarnings.insert(
        (*currentPortStatus)[0].complianceWarnings.end(),
        warnings.begin(),
        warnings.end());
}

void queryVersionHelper(android::hardware::usb::Usb *usb,
                        std::vector<PortStatus> *currentPortStatus) {
    Status status;
    string displayPortUsbPath;

    pthread_mutex_lock(&usb->mLock);
    status = getPortStatusHelper(usb, currentPortStatus);
    queryMoistureDetectionStatus(currentPortStatus);
    queryPowerTransferStatus(currentPortStatus);
    queryNonCompliantChargerStatus(currentPortStatus);
    queryUsbDataSession(usb, currentPortStatus);
    pthread_mutex_lock(&usb->mDisplayPortLock);
    if (!usb->mDisplayPortFirstSetupDone &&
        usb->getDisplayPortUsbPathHelper(&displayPortUsbPath) == Status::SUCCESS) {

        ALOGI("usbdp: boot with display connected or usb hal restarted");
        usb->setupDisplayPortPoll();
    }
    pthread_mutex_unlock(&usb->mDisplayPortLock);
    queryDisplayPortStatus(usb, currentPortStatus);
    if (usb->mCallback != NULL) {
        ScopedAStatus ret = usb->mCallback->notifyPortStatusChange(*currentPortStatus,
            status);
        if (!ret.isOk())
            ALOGE("queryPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGI("Notifying userspace skipped. Callback is NULL");
    }
    pthread_mutex_unlock(&usb->mLock);
}

ScopedAStatus Usb::queryPortStatus(int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;

    queryVersionHelper(this, &currentPortStatus);
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyQueryPortStatus(
            "all", Status::SUCCESS, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyQueryPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::enableContaminantPresenceDetection(const string& in_portName,
        bool in_enable, int64_t in_transactionId) {
    string disable = GetProperty(kDisableContatminantDetection, "");
    std::vector<PortStatus> currentPortStatus;
    bool success = true;

    if (disable != "true")
        success = WriteStringToFile(in_enable ? "1" : "0", enabledPath);

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyContaminantEnabledStatus(
            in_portName, in_enable, success ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyContaminantEnabledStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    queryVersionHelper(this, &currentPortStatus);
    return ScopedAStatus::ok();
}

void report_overheat_event(android::hardware::usb::Usb *usb) {
    VendorUsbPortOverheat overheat_info;
    string contents;

    overheat_info.set_plug_temperature_deci_c(usb->mPluggedTemperatureCelsius * 10);
    overheat_info.set_max_temperature_deci_c(usb->mOverheat.getMaxOverheatTemperature() * 10);
    if (ReadFileToString(string(kOverheatStatsPath) + "trip_time", &contents)) {
        overheat_info.set_time_to_overheat_secs(stoi(Trim(contents)));
    } else {
        ALOGE("Unable to read trip_time");
        return;
    }
    if (ReadFileToString(string(kOverheatStatsPath) + "hysteresis_time", &contents)) {
        overheat_info.set_time_to_hysteresis_secs(stoi(Trim(contents)));
    } else {
        ALOGE("Unable to read hysteresis_time");
        return;
    }
    if (ReadFileToString(string(kOverheatStatsPath) + "cleared_time", &contents)) {
        overheat_info.set_time_to_inactive_secs(stoi(Trim(contents)));
    } else {
        ALOGE("Unable to read cleared_time");
        return;
    }

    const shared_ptr<IStats> stats_client = getStatsService();
    if (!stats_client) {
        ALOGE("Unable to get AIDL Stats service");
    } else {
        reportUsbPortOverheat(stats_client, overheat_info);
    }
}

struct data {
    int uevent_fd;
    ::aidl::android::hardware::usb::Usb *usb;
};

enum UeventType { UNKNOWN, BIND, CHANGE };

enum UeventType matchUeventType(char* str) {
    if (!strncmp(str, "ACTION=bind", strlen("ACTION=bind"))) {
        return UeventType::BIND;
    } else if (!strncmp(str, "ACTION=change", strlen("ACTION=change"))) {
        return UeventType::CHANGE;
    }
    return UeventType::UNKNOWN;
}

static void uevent_event(uint32_t /*epevents*/, struct data *payload) {
    char msg[UEVENT_MSG_LEN + 2];
    char *cp;
    int n;
    enum UeventType uevent_type = UeventType::UNKNOWN;

    n = uevent_kernel_multicast_recv(payload->uevent_fd, msg, UEVENT_MSG_LEN);
    if (n <= 0)
        return;
    if (n >= UEVENT_MSG_LEN) /* overflow -- discard */
        return;

    msg[n] = '\0';
    msg[n + 1] = '\0';
    cp = msg;

    while (*cp) {
        if (std::regex_match(cp, std::regex("(add)(.*)(-partner)"))) {
            ALOGI("partner added");
            pthread_mutex_lock(&payload->usb->mPartnerLock);
            payload->usb->mPartnerUp = true;
            pthread_cond_signal(&payload->usb->mPartnerCV);
            pthread_mutex_unlock(&payload->usb->mPartnerLock);
        } else if (!strncmp(cp, "DEVTYPE=typec_", strlen("DEVTYPE=typec_")) ||
                   !strncmp(cp, "DRIVER=max77759tcpc",
                            strlen("DRIVER=max77759tcpc")) ||
                   !strncmp(cp, "DRIVER=pogo-transport",
                            strlen("DRIVER=pogo-transport")) ||
                   !strncmp(cp, "POWER_SUPPLY_NAME=usb",
                            strlen("POWER_SUPPLY_NAME=usb"))) {
            std::vector<PortStatus> currentPortStatus;
            queryVersionHelper(payload->usb, &currentPortStatus);

            // Role switch is not in progress and port is in disconnected state
            if (!pthread_mutex_trylock(&payload->usb->mRoleSwitchLock)) {
                for (unsigned long i = 0; i < currentPortStatus.size(); i++) {
                    DIR *dp =
                        opendir(string("/sys/class/typec/" +
                                            string(currentPortStatus[i].portName.c_str()) +
                                            "-partner").c_str());
                    if (dp == NULL) {
                        switchToDrp(currentPortStatus[i].portName);
                    } else {
                        closedir(dp);
                    }
                }
                pthread_mutex_unlock(&payload->usb->mRoleSwitchLock);
            }
            if (!strncmp(cp, "DRIVER=max77759tcpc", strlen("DRIVER=max77759tcpc"))
                       && payload->usb->mDisplayPortPollRunning) {
                uint64_t flag = DISPLAYPORT_IRQ_HPD_COUNT_CHECK;

                ALOGI("usbdp: DISPLAYPORT_IRQ_HPD_COUNT_CHECK sent");
                write(payload->usb->mDisplayPortEventPipe, &flag, sizeof(flag));
            }
            /*if (!!strncmp(cp, "DEVTYPE=typec_alternate_mode", strlen("DEVTYPE=typec_alternate_mode"))) {
                break;
            }*/
        } else if (!strncmp(cp, kOverheatStatsDev, strlen(kOverheatStatsDev))) {
            ALOGV("Overheat Cooling device suez update");
            report_overheat_event(payload->usb);
        } else if (!(strncmp(cp, "ACTION=", strlen("ACTION=")))) {
            uevent_type = matchUeventType(cp);
        } else if (!strncmp(cp, "DRIVER=typec_displayport", strlen("DRIVER=typec_displayport"))) {
            if (uevent_type == UeventType::BIND) {
                pthread_mutex_lock(&payload->usb->mDisplayPortLock);
                payload->usb->setupDisplayPortPoll();
                pthread_mutex_unlock(&payload->usb->mDisplayPortLock);
            } else if (uevent_type == UeventType::CHANGE) {
                pthread_mutex_lock(&payload->usb->mDisplayPortLock);
                payload->usb->shutdownDisplayPortPoll(false);
                pthread_mutex_unlock(&payload->usb->mDisplayPortLock);
            }
            break;
        }
        /* advance to after the next \0 */
        while (*cp++) {
        }
    }
}

void *work(void *param) {
    int epoll_fd, uevent_fd;
    struct epoll_event ev;
    int nevents = 0;
    struct data payload;

    ALOGE("creating thread");

    uevent_fd = uevent_open_socket(64 * 1024, true);

    if (uevent_fd < 0) {
        ALOGE("uevent_init: uevent_open_socket failed\n");
        return NULL;
    }

    payload.uevent_fd = uevent_fd;
    payload.usb = (::aidl::android::hardware::usb::Usb *)param;

    fcntl(uevent_fd, F_SETFL, O_NONBLOCK);

    ev.events = EPOLLIN;
    ev.data.ptr = (void *)uevent_event;

    epoll_fd = epoll_create(64);
    if (epoll_fd == -1) {
        ALOGE("epoll_create failed; errno=%d", errno);
        goto error;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, uevent_fd, &ev) == -1) {
        ALOGE("epoll_ctl failed; errno=%d", errno);
        goto error;
    }

    while (!destroyThread) {
        struct epoll_event events[64];

        nevents = epoll_wait(epoll_fd, events, 64, -1);
        if (nevents == -1) {
            if (errno == EINTR)
                continue;
            ALOGE("usb epoll_wait failed; errno=%d", errno);
            break;
        }

        for (int n = 0; n < nevents; ++n) {
            if (events[n].data.ptr)
                (*(void (*)(int, struct data *payload))events[n].data.ptr)(events[n].events,
                                                                           &payload);
        }
    }

    ALOGI("exiting worker thread");
error:
    close(uevent_fd);

    if (epoll_fd >= 0)
        close(epoll_fd);

    return NULL;
}

void sighandler(int sig) {
    if (sig == SIGUSR1) {
        destroyThread = true;
        ALOGI("destroy set");
        return;
    }
    signal(SIGUSR1, sighandler);
}

ScopedAStatus Usb::setCallback(const shared_ptr<IUsbCallback>& in_callback) {
    pthread_mutex_lock(&mLock);
    if ((mCallback == NULL && in_callback == NULL) ||
            (mCallback != NULL && in_callback != NULL)) {
        mCallback = in_callback;
        pthread_mutex_unlock(&mLock);
        return ScopedAStatus::ok();
    }

    mCallback = in_callback;
    ALOGI("registering callback");

    if (mCallback == NULL) {
        if  (!pthread_kill(mPoll, SIGUSR1)) {
            pthread_join(mPoll, NULL);
            ALOGI("pthread destroyed");
        }
        pthread_mutex_unlock(&mLock);
        return ScopedAStatus::ok();
    }

    destroyThread = false;
    signal(SIGUSR1, sighandler);

    /*
     * Create a background thread if the old callback value is NULL
     * and being updated with a new value.
     */
    if (pthread_create(&mPoll, NULL, work, this)) {
        ALOGE("pthread creation failed %d", errno);
        mCallback = NULL;
    }

    pthread_mutex_unlock(&mLock);
    return ScopedAStatus::ok();
}

Status Usb::getDisplayPortUsbPathHelper(string *path) {
    DIR *dp;
    Status result = Status::ERROR;

    dp = opendir(kDisplayPortUsbPath);
    if (dp != NULL) {
        struct dirent *ep;
        // Iterate through all alt mode directories to find displayport driver
        while ((ep = readdir(dp))) {
            if (ep->d_type == DT_DIR) {
                DIR *displayPortDp;
                string portPartnerPath = string(kDisplayPortUsbPath) + string(ep->d_name)
                        + "/displayport/";
                displayPortDp = opendir(portPartnerPath.c_str());
                if (displayPortDp != NULL) {
                    *path = portPartnerPath;
                    closedir(displayPortDp);
                    result = Status::SUCCESS;
                    break;
                }
            }
        }
        closedir(dp);
    }
    return result;
}

Status Usb::readDisplayPortAttribute(string attribute, string usb_path, string* value) {
    string attrPath;

    if (!strncmp(attribute.c_str(), "hpd", strlen("hpd")) ||
        !strncmp(attribute.c_str(), "pin_assignment", strlen("pin_assignment"))) {
        attrPath = usb_path + attribute;
    } else if (!strncmp(attribute.c_str(), "link_status", strlen("link_status"))) {
        attrPath = string(kDisplayPortDrmPath) + "link_status";
    } else if (!strncmp(attribute.c_str(), "vdo", strlen("vdo"))) {
        attrPath = usb_path + "/../vdo";
    } else {
        goto error;
    }

    // Read Attribute
    if(ReadFileToString(attrPath.c_str(), value)) {
        return Status::SUCCESS;
    }

error:
    ALOGE("usbdp: Failed to read Type-C attribute %s", attribute.c_str());
    return Status::ERROR;
}

Status Usb::writeDisplayPortAttributeOverride(string attribute, string value) {
    string attrDrmPath;

    // Get Drm Path
    attrDrmPath = string(kDisplayPortDrmPath) + attribute;

    // Write to drm
    if(!WriteStringToFile(value, attrDrmPath)) {
        ALOGE("usbdp: Failed to write attribute %s to drm: %s", attribute.c_str(), value.c_str());
        return Status::ERROR;
    }
    ALOGI("usbdp: Successfully wrote attribute %s: %s to drm.", attribute.c_str(), value.c_str());
    return Status::SUCCESS;
}

Status Usb::writeDisplayPortAttribute(string attribute, string usb_path) {
    string attrUsb, attrDrm, attrDrmPath;

    // Get Drm Path
    attrDrmPath = string(kDisplayPortDrmPath) + attribute;

    // Read Attribute
    if(!ReadFileToString(usb_path, &attrUsb)) {
        ALOGE("usbdp: Failed to open or read Type-C attribute %s", attribute.c_str());
        return Status::ERROR;
    }

    // Separate Logic for hpd and pin_assignment
    if (!strncmp(attribute.c_str(), "hpd", strlen("hpd"))) {
        if (!strncmp(attrUsb.c_str(), "0", strlen("0"))) {
            // Read DRM attribute to compare
            if(!ReadFileToString(attrDrmPath, &attrDrm)) {
                ALOGE("usbdp: Failed to open or read hpd from drm");
                return Status::ERROR;
            }
            if (!strncmp(attrDrm.c_str(), "0", strlen("0"))) {
                ALOGI("usbdp: Skipping hpd write when drm and usb both equal 0");
                return Status::SUCCESS;
            }
        }
    } else if (!strncmp(attribute.c_str(), "irq_hpd_count", strlen("irq_hpd_count"))) {
        uint32_t temp;
        if (!::android::base::ParseUint(Trim(attrUsb), &temp)) {
            ALOGE("usbdp: failed parsing irq_hpd_count:%s", attrUsb.c_str());
            return Status::ERROR;
        }
        // Used to cache the values read from tcpci's irq_hpd_count.
        // Update drm driver when cached value is not the same as the read value.
        ALOGI("usbdp: mIrqHpdCountCache:%u irq_hpd_count:%u", mIrqHpdCountCache, temp);
        if (mIrqHpdCountCache == temp) {
            return Status::SUCCESS;
        } else {
            mIrqHpdCountCache = temp;
        }
        attrDrmPath = string(kDisplayPortDrmPath) + "irq_hpd";
    } else if (!strncmp(attribute.c_str(), "pin_assignment", strlen("pin_assignment"))) {
        size_t pos = attrUsb.find("[");
        if (pos != string::npos) {
            ALOGI("usbdp: Modifying Pin Config from %s", attrUsb.c_str());
            attrUsb = attrUsb.substr(pos+1, 1);
        } else {
            // Don't write anything
            ALOGI("usbdp: Pin config not yet chosen, nothing written.");
            return Status::ERROR;
        }
    }

    // Write to drm
    if(!WriteStringToFile(attrUsb, attrDrmPath)) {
        ALOGE("usbdp: Failed to write attribute %s to drm: %s", attribute.c_str(), attrUsb.c_str());
        return Status::ERROR;
    }
    ALOGI("usbdp: Successfully wrote attribute %s: %s to drm.", attribute.c_str(), attrUsb.c_str());
    return Status::SUCCESS;
}

bool Usb::determineDisplayPortRetry(string linkPath, string hpdPath) {
    string linkStatus, hpd;

    if(ReadFileToString(linkPath, &linkStatus) && ReadFileToString(hpdPath, &hpd)) {
        if (!strncmp(linkStatus.c_str(), "2", strlen("2")) &&
                !strncmp(hpd.c_str(), "1", strlen("1"))) {
            return true;
        }
    }

    return false;
}

static int displayPortPollOpenFileHelper(const char *file, int flags) {
    int fd = open(file, flags);
    if (fd == -1) {
        ALOGE("usbdp: worker: open at %s failed; errno=%d", file, errno);
    }
    return fd;
}

/*
 * armTimerFdHelper - Sets timerfd (fd) to trigger after (ms) milliseconds.
 * Setting ms to 0 disarms the timer.
 */
static int armTimerFdHelper(int fd, int ms) {
    struct itimerspec ts;

    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec = ms / 1000;
    ts.it_value.tv_nsec = (ms % 1000) * 1000000;

    return timerfd_settime(fd, 0, &ts, NULL);
}

void *displayPortPollWork(void *param) {
    /* USB Payload */
    ::aidl::android::hardware::usb::Usb *usb = (::aidl::android::hardware::usb::Usb *)param;
    /* Epoll fields */
    int epoll_fd;
    struct epoll_event ev_hpd, ev_pin, ev_orientation, ev_eventfd, ev_link, ev_debounce;
    struct epoll_event ev_activate;
    int nevents = 0;
    int hpd_fd, pin_fd, orientation_fd, link_training_status_fd;
    int file_flags = O_RDONLY;
    int epoll_flags;
    /* DisplayPort link statuses */
    bool orientationSet = false;
    bool pinSet = false;
    int activateRetryCount = 0;
    unsigned long res;
    int ret = 0;
    /* File paths */
    string displayPortUsbPath, irqHpdCountPath, hpdPath, pinAssignmentPath, orientationPath;
    string tcpcI2cBus, linkPath, partnerActivePath, portActivePath;

    usb->mDisplayPortPollRunning = true;
    usb->mDisplayPortPollStarting = false;

    /*---------- Setup ----------*/

    if (usb->getDisplayPortUsbPathHelper(&displayPortUsbPath) == Status::ERROR) {
        ALOGE("usbdp: worker: could not locate usb displayport directory");
        goto usb_path_error;
    }

    ALOGI("usbdp: worker: displayport usb path located at %s", displayPortUsbPath.c_str());
    hpdPath = displayPortUsbPath + "hpd";
    pinAssignmentPath = displayPortUsbPath + "pin_assignment";
    orientationPath = "/sys/class/typec/port0/orientation";
    linkPath = string(kDisplayPortDrmPath) + "link_status";

    partnerActivePath = displayPortUsbPath + "../mode1/active";
    portActivePath = "/sys/class/typec/port0/port0.0/mode1/active";

    getI2cBusHelper(&tcpcI2cBus);
    irqHpdCountPath = kI2CPath + tcpcI2cBus + "/" + tcpcI2cBus + kIrqHpdCounPath;
    ALOGI("usbdp: worker: irqHpdCountPath:%s", irqHpdCountPath.c_str());

    epoll_fd = epoll_create(64);
    if (epoll_fd == -1) {
        ALOGE("usbdp: worker: epoll_create failed; errno=%d", errno);
        goto epoll_fd_error;
    }

    if ((hpd_fd = displayPortPollOpenFileHelper(hpdPath.c_str(), file_flags)) == -1){
        goto hpd_fd_error;
    }
    if ((pin_fd = displayPortPollOpenFileHelper(pinAssignmentPath.c_str(), file_flags)) == -1){
        goto pin_fd_error;
    }
    if ((orientation_fd = displayPortPollOpenFileHelper(orientationPath.c_str(), file_flags))
            == -1){
        goto orientation_fd_error;
    }
    if ((link_training_status_fd = displayPortPollOpenFileHelper(linkPath.c_str(), file_flags)) == -1){
        goto link_training_status_fd_error;
    }

    // Set epoll_event events and flags
    epoll_flags = EPOLLIN | EPOLLET;
    ev_hpd.events = epoll_flags;
    ev_pin.events = epoll_flags;
    ev_orientation.events = epoll_flags;
    ev_eventfd.events = epoll_flags;
    ev_link.events = epoll_flags;
    ev_debounce.events = epoll_flags;
    ev_activate.events = epoll_flags;

    ev_hpd.data.fd = hpd_fd;
    ev_pin.data.fd = pin_fd;
    ev_orientation.data.fd = orientation_fd;
    ev_eventfd.data.fd = usb->mDisplayPortEventPipe;
    ev_link.data.fd = link_training_status_fd;
    ev_debounce.data.fd = usb->mDisplayPortDebounceTimer;
    ev_activate.data.fd = usb->mDisplayPortActivateTimer;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, hpd_fd, &ev_hpd) == -1) {
        ALOGE("usbdp: worker: epoll_ctl failed to add hpd; errno=%d", errno);
        goto error;
    }
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pin_fd, &ev_pin) == -1) {
        ALOGE("usbdp: worker: epoll_ctl failed to add pin; errno=%d", errno);
        goto error;
    }
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, orientation_fd, &ev_orientation) == -1) {
        ALOGE("usbdp: worker: epoll_ctl failed to add orientation; errno=%d", errno);
        goto error;
    }
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, link_training_status_fd, &ev_link) == -1) {
        ALOGE("usbdp: worker: epoll_ctl failed to add link status; errno=%d", errno);
        goto error;
    }
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, usb->mDisplayPortDebounceTimer, &ev_debounce) == -1) {
        ALOGE("usbdp: worker: epoll_ctl failed to add framework update debounce; errno=%d", errno);
        goto error;
    }
     if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, usb->mDisplayPortActivateTimer, &ev_activate) == -1) {
        ALOGE("usbdp: worker: epoll_ctl failed to add activate debounce; errno=%d", errno);
        goto error;
    }
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, usb->mDisplayPortEventPipe, &ev_eventfd) == -1) {
        ALOGE("usbdp: worker: epoll_ctl failed to add orientation; errno=%d", errno);
        goto error;
    }

    /* Arm timer to see if DisplayPort Alt Mode Activates */
    armTimerFdHelper(usb->mDisplayPortActivateTimer, DISPLAYPORT_ACTIVATE_DEBOUNCE_MS);

    while (!destroyDisplayPortThread) {
        struct epoll_event events[64];

        nevents = epoll_wait(epoll_fd, events, 64, -1);
        if (nevents == -1) {
            if (errno == EINTR)
                continue;
            ALOGE("usbdp: worker: epoll_wait failed; errno=%d", errno);
            break;
        }

        for (int n = 0; n < nevents; n++) {
            if (events[n].data.fd == hpd_fd) {
                if (!pinSet || !orientationSet) {
                    ALOGW("usbdp: worker: HPD may be set before pin_assignment and orientation");
                    if (!pinSet &&
                        usb->writeDisplayPortAttribute("pin_assignment", pinAssignmentPath) ==
                        Status::SUCCESS) {
                        pinSet = true;
                    }
                    if (!orientationSet &&
                        usb->writeDisplayPortAttribute("orientation", orientationPath) ==
                        Status::SUCCESS) {
                        orientationSet = true;
                    }
                }
                usb->writeDisplayPortAttribute("hpd", hpdPath);
                armTimerFdHelper(usb->mDisplayPortDebounceTimer, DISPLAYPORT_STATUS_DEBOUNCE_MS);
            } else if (events[n].data.fd == pin_fd) {
                if (usb->writeDisplayPortAttribute("pin_assignment", pinAssignmentPath) ==
                    Status::SUCCESS) {
                    pinSet = true;
                    armTimerFdHelper(usb->mDisplayPortDebounceTimer, DISPLAYPORT_STATUS_DEBOUNCE_MS);
                }
            } else if (events[n].data.fd == orientation_fd) {
                if (usb->writeDisplayPortAttribute("orientation", orientationPath) ==
                    Status::SUCCESS) {
                    orientationSet = true;
                    armTimerFdHelper(usb->mDisplayPortDebounceTimer, DISPLAYPORT_STATUS_DEBOUNCE_MS);
                }
            } else if (events[n].data.fd == link_training_status_fd) {
                armTimerFdHelper(usb->mDisplayPortDebounceTimer, DISPLAYPORT_STATUS_DEBOUNCE_MS);
            } else if (events[n].data.fd == usb->mDisplayPortDebounceTimer) {
                std::vector<PortStatus> currentPortStatus;
                ret = read(usb->mDisplayPortDebounceTimer, &res, sizeof(res));
                ALOGI("usbdp: dp debounce triggered, val:%lu ret:%d", res, ret);
                if (ret < 0)
                    ALOGE("usbdp: debounce read errno:%d", errno);
                queryVersionHelper(usb, &currentPortStatus);
            } else if (events[n].data.fd == usb->mDisplayPortActivateTimer) {
                string activePartner, activePort;

                if (ReadFileToString(partnerActivePath.c_str(), &activePartner) &&
                    ReadFileToString(portActivePath.c_str(), &activePort)) {
                    // Retry activate signal when DisplayPort Alt Mode is active on port but not
                    // partner.
                    if (!strncmp(activePartner.c_str(), "no", strlen("no")) &&
                        !strncmp(activePort.c_str(), "yes", strlen("yes")) &&
                        activateRetryCount < DISPLAYPORT_ACTIVATE_MAX_RETRIES) {
                        if (!WriteStringToFile("1", partnerActivePath)) {
                            ALOGE("usbdp: Failed to activate port partner Alt Mode");
                        } else {
                            ALOGI("usbdp: Attempting to activate port partner Alt Mode");
                        }
                        activateRetryCount++;
                        armTimerFdHelper(usb->mDisplayPortActivateTimer,
                                         DISPLAYPORT_ACTIVATE_DEBOUNCE_MS);
                    } else {
                        ALOGI("usbdp: DisplayPort Alt Mode is active, or disabled on port");
                    }
                } else {
                    activateRetryCount++;
                    armTimerFdHelper(usb->mDisplayPortActivateTimer,
                                     DISPLAYPORT_ACTIVATE_DEBOUNCE_MS);
                    ALOGE("usbdp: Failed to read active state from port or partner");
                }
            } else if (events[n].data.fd == usb->mDisplayPortEventPipe) {
                uint64_t flag = 0;
                if (!read(usb->mDisplayPortEventPipe, &flag, sizeof(flag))) {
                    if (errno == EAGAIN)
                        continue;
                    ALOGI("usbdp: worker: Shutdown eventfd read error");
                    goto error;
                }
                if (flag == DISPLAYPORT_SHUTDOWN_SET) {
                    ALOGI("usbdp: worker: Shutdown eventfd triggered");
                    destroyDisplayPortThread = true;
                    break;
                } else if (flag == DISPLAYPORT_IRQ_HPD_COUNT_CHECK) {
                    ALOGI("usbdp: worker: IRQ_HPD event through DISPLAYPORT_IRQ_HPD_COUNT_CHECK");
                    usb->writeDisplayPortAttribute("irq_hpd_count", irqHpdCountPath);
                }
            }
        }
    }

error:
    /* Need to disarm so new threads don't get old event */
    armTimerFdHelper(usb->mDisplayPortActivateTimer, 0);
    close(link_training_status_fd);
link_training_status_fd_error:
    close(orientation_fd);
orientation_fd_error:
    close(pin_fd);
pin_fd_error:
    close(hpd_fd);
hpd_fd_error:
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, usb->mDisplayPortDebounceTimer, &ev_debounce);
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, usb->mDisplayPortActivateTimer, &ev_activate);
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, usb->mDisplayPortEventPipe, &ev_eventfd);
    close(epoll_fd);
epoll_fd_error:
usb_path_error:
    usb->mDisplayPortPollRunning = false;
    ALOGI("usbdp: worker: exiting worker thread");
    return NULL;
}

static struct timespec setTimespecTimer(int debounceMs) {
    struct timespec to;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    to.tv_nsec = now.tv_nsec + ((debounceMs % 1000) * 1000000);
    to.tv_sec = now.tv_sec + (debounceMs / 1000);
    if (to.tv_nsec >= 1000000000) {
        to.tv_nsec -= 1000000000;
        to.tv_sec += 1;
    }

    return to;
}

void Usb::setupDisplayPortPoll() {
    uint64_t flag = DISPLAYPORT_SHUTDOWN_CLEAR;
    mDisplayPortFirstSetupDone = true;
    int ret;

    ALOGI("usbdp: setup: beginning setup for displayport poll thread");

    /*
     * If thread is currently starting, then it hasn't setup DisplayPort fd's, and we can abandon
     * this process.
     */
    if (mDisplayPortPollStarting) {
        ALOGI("usbdp: setup: abandoning poll thread because another startup is in progress");
        return;
    }

    /*
     * Check to see if thread is currently running. If it is, then we assume that it must have
     * invalid DisplayPort fd's and the new thread takes over.
     */
    if (mDisplayPortPollRunning) {
        shutdownDisplayPortPoll(true);
        pthread_mutex_lock(&mDisplayPortCVLock);
        struct timespec to = setTimespecTimer(DISPLAYPORT_POLL_WAIT_MS);
        ret = pthread_cond_timedwait(&mDisplayPortCV, &mDisplayPortCVLock, &to);
        if (ret == ETIMEDOUT) {
            ALOGI("usbdp: setup: Wait for poll to shutdown timed out, starting new poll anyways.");
        }
        pthread_mutex_unlock(&mDisplayPortCVLock);
    }

    // Indicate that startup procedure is initiated (mutex protects two threads running setup at
    // once)
    mDisplayPortPollStarting = true;

    // Reset shutdown signals because shutdown() does not perform self clean-up
    write(mDisplayPortEventPipe, &flag, sizeof(flag));
    destroyDisplayPortThread = false;

    /*
     * Create a background thread to poll DisplayPort system files
     */
    if (pthread_create(&mDisplayPortPoll, NULL, displayPortPollWork, this)) {
        ALOGE("usbdp: setup: failed to create displayport poll thread %d", errno);
        goto error;
    }
    ALOGI("usbdp: setup: successfully started displayport poll thread");
    return;

error:
    mDisplayPortPollStarting = false;
    return;
}

void Usb::shutdownDisplayPortPollHelper() {
    uint64_t flag = DISPLAYPORT_SHUTDOWN_SET;

    // Write shutdown signal to child thread.
    write(mDisplayPortEventPipe, &flag, sizeof(flag));
    pthread_join(mDisplayPortPoll, NULL);
    writeDisplayPortAttributeOverride("hpd", "0");
    pthread_mutex_lock(&mDisplayPortCVLock);
    pthread_cond_signal(&mDisplayPortCV);
    pthread_mutex_unlock(&mDisplayPortCVLock);
}

void *shutdownDisplayPortPollWork(void *param) {
    ::aidl::android::hardware::usb::Usb *usb = (::aidl::android::hardware::usb::Usb *)param;

    usb->shutdownDisplayPortPollHelper();
    ALOGI("usbdp: shutdown: displayport thread shutdown complete.");
    return NULL;
}

void Usb::shutdownDisplayPortPoll(bool force) {
    string displayPortUsbPath;

    ALOGI("usbdp: shutdown: beginning shutdown for displayport poll thread");

    /*
     * Determine if should shutdown thread
     *
     * getDisplayPortUsbPathHelper locates a DisplayPort directory, no need to double check
     * directory.
     *
     * Force is put in place to shutdown even when displayPortUsbPath is still present.
     * Happens when back to back BIND events are sent and fds are no longer current.
     */
    if (!mDisplayPortPollRunning ||
        (!force && getDisplayPortUsbPathHelper(&displayPortUsbPath) == Status::SUCCESS)) {
        return;
    }

    // Shutdown is nonblocking to let other usb operations continue
    if (pthread_create(&mDisplayPortShutdownHelper, NULL, shutdownDisplayPortPollWork, this)) {
        ALOGE("usbdp: shutdown: shutdown worker pthread creation failed %d", errno);
    }
}

using ext::PortSecurityState;
using ext::IUsbExt;

static int WriteStringToFileOrLog(string val, string path) {
    if (WriteStringToFile(val, path)) {
        ALOGD("written %s to %s", val.c_str(), path.c_str());
        return 1;
    }
    ALOGE("unable to write %s to %s", val.c_str(), path.c_str());
    return 0;
}

static ScopedAStatus setPortSecurityStateInner(PortSecurityState in_state) {
    string path;

    if (Status ret = getI2cBusHelper(&path); ret != Status::SUCCESS) {
        return ScopedAStatus::fromServiceSpecificError(IUsbExt::ERROR_NO_I2C_PATH);
    }

    string ccToggleEnablePath = kI2CPath + path + "/" + path + kCcToggleEnable;
    string dataPathEnablePath = kI2CPath + path + "/" + path + kDataPathEnable;

    // '&' is used instead of '&&' intentionally to disable short-circuit evaluation

    switch (in_state) {
        case PortSecurityState::DISABLED: {
            if (WriteStringToFileOrLog("0", ccToggleEnablePath)
                    & WriteStringToFileOrLog("0", dataPathEnablePath)) {
                return ScopedAStatus::ok();
            }
            return ScopedAStatus::fromServiceSpecificError(IUsbExt::ERROR_FILE_WRITE);
        }
        case PortSecurityState::CHARGING_ONLY_IMMEDIATE: {
            if (WriteStringToFileOrLog("0", dataPathEnablePath)
                    & WriteStringToFileOrLog("1", ccToggleEnablePath)) {
                return ScopedAStatus::ok();
            }
            return ScopedAStatus::fromServiceSpecificError(IUsbExt::ERROR_FILE_WRITE);
        }
        case PortSecurityState::CHARGING_ONLY: {
            if (WriteStringToFileOrLog("-1", dataPathEnablePath)
                    & WriteStringToFileOrLog("1", ccToggleEnablePath)) {
                return ScopedAStatus::ok();
            }
            return ScopedAStatus::fromServiceSpecificError(IUsbExt::ERROR_FILE_WRITE);
        }
        case PortSecurityState::ENABLED: {
            if (WriteStringToFileOrLog("1", dataPathEnablePath)
                    & WriteStringToFileOrLog("1", ccToggleEnablePath)) {
                return ScopedAStatus::ok();
            }
            return ScopedAStatus::fromServiceSpecificError(IUsbExt::ERROR_FILE_WRITE);
        }
    }

    return ScopedAStatus::ok();
}

// keep in sync with frameworks/base/core/java/android/ext/settings/UsbPortSecurity.java
static const int MODE_DISABLED = 0;
static const int MODE_CHARGING_ONLY = 1;
static const int MODE_CHARGING_ONLY_WHEN_LOCKED = 2;
static const int MODE_CHARGING_ONLY_WHEN_LOCKED_AFU = 3;
static const int MODE_ENABLED = 4;

UsbExt::UsbExt(std::shared_ptr<Usb> usb) : mUsb(usb) {
    int initialMode = ::android::base::GetIntProperty("persist.security.usb_mode", MODE_CHARGING_ONLY_WHEN_LOCKED_AFU);
    ALOGD("initial persist.security.usb_mode: %i", initialMode);

    switch (initialMode) {
        case MODE_CHARGING_ONLY:
        case MODE_CHARGING_ONLY_WHEN_LOCKED:
            setPortSecurityStateInner(PortSecurityState::CHARGING_ONLY_IMMEDIATE);
            break;
        case MODE_CHARGING_ONLY_WHEN_LOCKED_AFU:
        case MODE_ENABLED:
            setPortSecurityStateInner(PortSecurityState::ENABLED);
            break;
    }
}

ScopedAStatus UsbExt::setPortSecurityState(const std::string& in_portName,
        PortSecurityState in_state) {
    return setPortSecurityStateInner(in_state);
}

} // namespace usb
} // namespace hardware
} // namespace android
} // aidl
