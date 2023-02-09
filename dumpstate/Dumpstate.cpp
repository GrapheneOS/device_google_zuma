/*
 * Copyright 2016 The Android Open Source Project
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

#define LOG_TAG "dumpstate_device"

#include <inttypes.h>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <log/log.h>
#include <sys/stat.h>

#include "Dumpstate.h"

#include "DumpstateUtil.h"

#define MODEM_LOG_DIRECTORY "/data/vendor/radio/logs/always-on"
#define MODEM_EXTENDED_LOG_DIRECTORY "/data/vendor/radio/extended_logs"
#define RIL_LOG_DIRECTORY "/data/vendor/radio"
#define RIL_LOG_DIRECTORY_PROPERTY "persist.vendor.ril.log.base_dir"
#define RIL_LOG_NUMBER_PROPERTY "persist.vendor.ril.log.num_file"
#define MODEM_LOGGING_PERSIST_PROPERTY "persist.vendor.sys.modem.logging.enable"
#define MODEM_LOGGING_PROPERTY "vendor.sys.modem.logging.enable"
#define MODEM_LOGGING_STATUS_PROPERTY "vendor.sys.modem.logging.status"
#define MODEM_LOGGING_NUMBER_BUGREPORT_PROPERTY "persist.vendor.sys.modem.logging.br_num"
#define MODEM_LOGGING_PATH_PROPERTY "vendor.sys.modem.logging.log_path"
#define GPS_LOG_DIRECTORY "/data/vendor/gps/logs"
#define GPS_LOG_NUMBER_PROPERTY "persist.vendor.gps.aol.log_num"
#define GPS_LOGGING_STATUS_PROPERTY "vendor.gps.aol.enabled"

#define TCPDUMP_LOG_DIRECTORY "/data/vendor/tcpdump_logger/logs"
#define TCPDUMP_NUMBER_BUGREPORT "persist.vendor.tcpdump.log.br_num"
#define TCPDUMP_PERSIST_PROPERTY "persist.vendor.tcpdump.log.alwayson"

#define HW_REVISION "ro.boot.hardware.revision"

using android::os::dumpstate::CommandOptions;
using android::os::dumpstate::DumpFileToFd;
using android::os::dumpstate::PropertiesHelper;
using android::os::dumpstate::RunCommandToFd;

namespace aidl {
namespace android {
namespace hardware {
namespace dumpstate {

#define GPS_LOG_PREFIX "gl-"
#define GPS_MCU_LOG_PREFIX "esw-"
#define MODEM_LOG_PREFIX "sbuff_"
#define EXTENDED_LOG_PREFIX "extended_log_"
#define RIL_LOG_PREFIX "rild.log."
#define BUFSIZE 65536
#define TCPDUMP_LOG_PREFIX "tcpdump"

typedef std::chrono::time_point<std::chrono::steady_clock> timepoint_t;

const char kVerboseLoggingProperty[] = "persist.vendor.verbose_logging_enabled";

void Dumpstate::dumpLogs(int fd, std::string srcDir, std::string destDir, int maxFileNum,
                               const char *logPrefix) {
    struct dirent **dirent_list = NULL;
    int num_entries = scandir(srcDir.c_str(),
                              &dirent_list,
                              0,
                              (int (*)(const struct dirent **, const struct dirent **)) alphasort);
    if (!dirent_list) {
        return;
    } else if (num_entries <= 0) {
        return;
    }

    int copiedFiles = 0;

    for (int i = num_entries - 1; i >= 0; i--) {
        ALOGD("Found %s\n", dirent_list[i]->d_name);

        if (0 != strncmp(dirent_list[i]->d_name, logPrefix, strlen(logPrefix))) {
            continue;
        }

        if ((copiedFiles >= maxFileNum) && (maxFileNum != -1)) {
            ALOGD("Skipped %s\n", dirent_list[i]->d_name);
            continue;
        }

        copiedFiles++;

        CommandOptions options = CommandOptions::WithTimeout(120).Build();
        std::string srcLogFile = srcDir + "/" + dirent_list[i]->d_name;
        std::string destLogFile = destDir + "/" + dirent_list[i]->d_name;

        std::string copyCmd = "/vendor/bin/cp " + srcLogFile + " " + destLogFile;

        ALOGD("Copying %s to %s\n", srcLogFile.c_str(), destLogFile.c_str());
        RunCommandToFd(fd, "CP LOGS", { "/vendor/bin/sh", "-c", copyCmd.c_str() }, options);
    }

    while (num_entries--) {
        free(dirent_list[num_entries]);
    }

    free(dirent_list);
}

void Dumpstate::dumpRilLogs(int fd, std::string destDir) {
    std::string rilLogDir =
            ::android::base::GetProperty(RIL_LOG_DIRECTORY_PROPERTY, RIL_LOG_DIRECTORY);

    int maxFileNum = ::android::base::GetIntProperty(RIL_LOG_NUMBER_PROPERTY, 50);

    const std::string currentLogDir = rilLogDir + "/cur";
    const std::string previousLogDir = rilLogDir + "/prev";
    const std::string currentDestDir = destDir + "/cur";
    const std::string previousDestDir = destDir + "/prev";

    RunCommandToFd(fd, "MKDIR RIL CUR LOG", {"/vendor/bin/mkdir", "-p", currentDestDir.c_str()},
                   CommandOptions::WithTimeout(2).Build());
    RunCommandToFd(fd, "MKDIR RIL PREV LOG", {"/vendor/bin/mkdir", "-p", previousDestDir.c_str()},
                   CommandOptions::WithTimeout(2).Build());

    dumpLogs(fd, currentLogDir, currentDestDir, maxFileNum, RIL_LOG_PREFIX);
    dumpLogs(fd, previousLogDir, previousDestDir, maxFileNum, RIL_LOG_PREFIX);
}

void copyFile(std::string srcFile, std::string destFile) {
    uint8_t buffer[BUFSIZE];
    ssize_t size;

    int fdSrc = open(srcFile.c_str(), O_RDONLY);
    if (fdSrc < 0) {
        ALOGD("Failed to open source file %s\n", srcFile.c_str());
        return;
    }

    int fdDest = open(destFile.c_str(), O_WRONLY | O_CREAT, 0666);
    if (fdDest < 0) {
        ALOGD("Failed to open destination file %s\n", destFile.c_str());
        close(fdSrc);
        return;
    }

    ALOGD("Copying %s to %s\n", srcFile.c_str(), destFile.c_str());
    while ((size = TEMP_FAILURE_RETRY(read(fdSrc, buffer, BUFSIZE))) > 0) {
        TEMP_FAILURE_RETRY(write(fdDest, buffer, size));
    }

    close(fdDest);
    close(fdSrc);
}

void dumpNetmgrLogs(std::string destDir) {
    const std::vector <std::string> netmgrLogs
        {
            "/data/vendor/radio/metrics_data",
            "/data/vendor/radio/omadm_logs.txt",
            "/data/vendor/radio/power_anomaly_data.txt",
        };
    for (const auto& logFile : netmgrLogs) {
        copyFile(logFile, destDir + "/" + basename(logFile.c_str()));
    }
}

/** Dumps last synced NV data into bugreports */
void dumpModemEFS(std::string destDir) {
    const std::string EFS_DIRECTORY = "/mnt/vendor/efs/";
    const std::vector <std::string> nv_files
        {
            EFS_DIRECTORY+"nv_normal.bin",
            EFS_DIRECTORY+"nv_protected.bin",
        };
    for (const auto& logFile : nv_files) {
        copyFile(logFile, destDir + "/" + basename(logFile.c_str()));
    }
}

timepoint_t startSection(int fd, const std::string &sectionName) {
    ::android::base::WriteStringToFd(
            "\n"
            "------ Section start: " + sectionName + " ------\n"
            "\n", fd);
    return std::chrono::steady_clock::now();
}

void endSection(int fd, const std::string &sectionName, timepoint_t startTime) {
    auto endTime = std::chrono::steady_clock::now();
    auto elapsedMsec = std::chrono::duration_cast<std::chrono::milliseconds>
            (endTime - startTime).count();

    ::android::base::WriteStringToFd(
            "\n"
            "------ Section end: " + sectionName + " ------\n"
            "Elapsed msec: " + std::to_string(elapsedMsec) + "\n"
            "\n", fd);
}

// If you are adding a single RunCommandToFd() or DumpFileToFd() call, please
// add it to dumpMiscSection().  But if you are adding multiple items that are
// related to each other - for instance, for a Foo peripheral - please add them
// to a new dump function and include it in this table so it can be accessed from the
// command line, e.g.:
//   dumpsys android.hardware.dumpstate.IDumpstateDevice/default foo
//
// However, if your addition generates attachments and/or binary data for the
// bugreport (i.e. if it requires two file descriptors to execute), it must not be
// added to this table and should instead be added to dumpstateBoard() below.

Dumpstate::Dumpstate()
  : mTextSections{
        { "memory", [this](int fd) { dumpMemorySection(fd); } },
        { "Devfreq", [this](int fd) { dumpDevfreqSection(fd); } },
        { "display", [this](int fd) { dumpDisplaySection(fd); } },
        { "misc", [this](int fd) { dumpMiscSection(fd); } },
        { "led", [this](int fd) { dumpLEDSection(fd); } },
    },
  mLogSections{
        { "modem", [this](int fd, const std::string &destDir) { dumpModemLogs(fd, destDir); } },
        { "radio", [this](int fd, const std::string &destDir) { dumpRadioLogs(fd, destDir); } },
        { "camera", [this](int fd, const std::string &destDir) { dumpCameraLogs(fd, destDir); } },
        { "gps", [this](int fd, const std::string &destDir) { dumpGpsLogs(fd, destDir); } },
        { "gxp", [this](int fd, const std::string &destDir) { dumpGxpLogs(fd, destDir); } },
  } {
}

// Dump data requested by an argument to the "dump" interface, or help info
// if the specified section is not supported.
void Dumpstate::dumpTextSection(int fd, const std::string &sectionName) {
    bool dumpAll = (sectionName == kAllSections);
    std::string dumpFiles;

    for (const auto &section : mTextSections) {
        if (dumpAll || sectionName == section.first) {
            auto startTime = startSection(fd, section.first);
            section.second(fd);
            endSection(fd, section.first, startTime);

            if (!dumpAll) {
                return;
            }
        }
    }

    // Execute all or designated programs under vendor/bin/dump/
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir("/vendor/bin/dump"), closedir);
    if (!dir) {
        ALOGE("Fail To Open Dir vendor/bin/dump/");
        ::android::base::WriteStringToFd("Fail To Open Dir vendor/bin/dump/\n", fd);
        return;
    }
    dirent *entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        // Skip '.', '..'
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string bin(entry->d_name);
        dumpFiles = dumpFiles + " " + bin;
        if (dumpAll || sectionName == bin) {
            auto startTime = startSection(fd, bin);
            RunCommandToFd(fd, "/vendor/bin/dump/"+bin, {"/vendor/bin/dump/"+bin});
            endSection(fd, bin, startTime);
            if (!dumpAll) {
                return;
            }
        }
    }

    if (dumpAll) {
        return;
    }

    // An unsupported section was requested on the command line
    ::android::base::WriteStringToFd("Unrecognized text section: " + sectionName + "\n", fd);
    ::android::base::WriteStringToFd("Try \"" + kAllSections + "\" or one of the following:", fd);
    for (const auto &section : mTextSections) {
        ::android::base::WriteStringToFd(" " + section.first, fd);
    }
    ::android::base::WriteStringToFd(dumpFiles, fd);
    ::android::base::WriteStringToFd("\nNote: sections with attachments (e.g. modem) are"
                                   "not avalable from the command line.\n", fd);
}

// Dump items related to Devfreq & BTS
void Dumpstate::dumpDevfreqSection(int fd) {
    DumpFileToFd(fd, "MIF DVFS",
                 "/sys/devices/platform/17000010.devfreq_mif/devfreq/17000010.devfreq_mif/time_in_state");
    DumpFileToFd(fd, "INT DVFS",
                 "/sys/devices/platform/17000020.devfreq_int/devfreq/17000020.devfreq_int/time_in_state");
    DumpFileToFd(fd, "INTCAM DVFS",
                 "/sys/devices/platform/17000030.devfreq_intcam/devfreq/17000030.devfreq_intcam/time_in_state");
    DumpFileToFd(fd, "DISP DVFS",
                 "/sys/devices/platform/17000040.devfreq_disp/devfreq/17000040.devfreq_disp/time_in_state");
    DumpFileToFd(fd, "CAM DVFS",
                 "/sys/devices/platform/17000050.devfreq_cam/devfreq/17000050.devfreq_cam/time_in_state");
    DumpFileToFd(fd, "TNR DVFS",
                 "/sys/devices/platform/17000060.devfreq_tnr/devfreq/17000060.devfreq_tnr/time_in_state");
    DumpFileToFd(fd, "MFC DVFS",
                 "/sys/devices/platform/17000070.devfreq_mfc/devfreq/17000070.devfreq_mfc/time_in_state");
    DumpFileToFd(fd, "BO DVFS",
                 "/sys/devices/platform/17000080.devfreq_bo/devfreq/17000080.devfreq_bo/time_in_state");
    DumpFileToFd(fd, "BTS stats", "/sys/devices/platform/exynos-bts/bts_stats");
}

// Dump items related to memory
void Dumpstate::dumpMemorySection(int fd) {
    RunCommandToFd(fd, "ION HEAPS", {"/vendor/bin/sh", "-c",
                   "for d in $(ls -d /d/ion/*); do "
                       "if [ -f $d ]; then "
                           "echo --- $d; cat $d; "
                       "else "
                           "for f in $(ls $d); do "
                               "echo --- $d/$f; cat $d/$f; "
                               "done; "
                        "fi; "
                        "done"});
    DumpFileToFd(fd, "dmabuf info", "/d/dma_buf/bufinfo");
    DumpFileToFd(fd, "Page Pinner - longterm pin", "/sys/kernel/debug/page_pinner/buffer");
    RunCommandToFd(fd, "CMA info", {"/vendor/bin/sh", "-c",
                       "for d in $(ls -d /d/cma/*); do "
                         "echo --- $d;"
                         "echo --- count; cat $d/count; "
                         "echo --- used; cat $d/used; "
                         "echo --- bitmap; cat $d/bitmap; "
                       "done"});
}

// Dump items related to display
void Dumpstate::dumpDisplaySection(int fd) {
    // Dump counters for decon drivers
    const std::string decon_device_sysfs_path("/sys/class/drm/card0/device/");
    for(int i = 0; i <= 2; ++i){
        const std::string decon_num_str = std::to_string(i);
        const std::string decon_counter_path = decon_device_sysfs_path +
                                              "decon" + decon_num_str +
                                              "/counters";
        if (access(decon_counter_path.c_str(), R_OK) == 0){
            DumpFileToFd(fd, "DECON-" + decon_num_str + " counters",
                         decon_counter_path);
        }
        else{
            ::android::base::WriteStringToFd("No counters for DECON-" +
                decon_num_str + " found at path (" + decon_counter_path + ")\n",
                fd);
        }
    }
    DumpFileToFd(fd, "CRTC-0 event log", "/sys/kernel/debug/dri/0/crtc-0/event");
    DumpFileToFd(fd, "CRTC-1 event log", "/sys/kernel/debug/dri/0/crtc-1/event");
    RunCommandToFd(fd, "libdisplaycolor", {"/vendor/bin/dumpsys", "displaycolor", "-v"},
                   CommandOptions::WithTimeout(2).Build());
    DumpFileToFd(fd, "Primary panel name", "/sys/devices/platform/exynos-drm/primary-panel/panel_name");
    DumpFileToFd(fd, "Primary panel extra info", "/sys/devices/platform/exynos-drm/primary-panel/panel_extinfo");
    DumpFileToFd(fd, "Secondary panel name", "/sys/devices/platform/exynos-drm/secondary-panel/panel_name");
    DumpFileToFd(fd, "Secondary panel extra info", "/sys/devices/platform/exynos-drm/secondary-panel/panel_extinfo");
    if (!PropertiesHelper::IsUserBuild()) {
        RunCommandToFd(fd, "HWC Fence States", {"/vendor/bin/sh", "-c",
                           "for f in $(ls /data/vendor/log/hwc/*_hwc_fence_state*.txt); do "
                           "echo $f ; cat $f ; done"},
                           CommandOptions::WithTimeout(2).Build());
        RunCommandToFd(fd, "HWC Error Logs", {"/vendor/bin/sh", "-c",
                           "for f in $(ls /data/vendor/log/hwc/*_hwc_error_log*.txt); do "
                           "echo $f ; cat $f ; done"},
                           CommandOptions::WithTimeout(2).Build());
        RunCommandToFd(fd, "HWC Debug Dumps", {"/vendor/bin/sh", "-c",
                           "for f in $(ls /data/vendor/log/hwc/*_hwc_debug*.dump); do "
                           "echo $f ; cat $f ; done"},
                           CommandOptions::WithTimeout(2).Build());
    }
}

// Dump items that don't fit well into any other section
void Dumpstate::dumpMiscSection(int fd) {
    RunCommandToFd(fd, "VENDOR PROPERTIES", {"/vendor/bin/getprop"});
    DumpFileToFd(fd, "VENDOR PROC DUMP", "/proc/vendor_sched/dump_task");
}

// Dump items related to LED
void Dumpstate::dumpLEDSection(int fd) {
    struct stat buffer;

    if (!PropertiesHelper::IsUserBuild()) {
        if (!stat("/sys/class/leds/green", &buffer)) {
            DumpFileToFd(fd, "Green LED Brightness", "/sys/class/leds/green/brightness");
            DumpFileToFd(fd, "Green LED Max Brightness", "/sys/class/leds/green/max_brightness");
        }
        if (!stat("/mnt/vendor/persist/led/led_calibration_LUT.txt", &buffer)) {
            DumpFileToFd(fd, "LED Calibration Data", "/mnt/vendor/persist/led/led_calibration_LUT.txt");
        }
    }
}

void Dumpstate::dumpModemLogs(int fd, const std::string &destDir) {
    std::string extendedLogDir = MODEM_EXTENDED_LOG_DIRECTORY;

    dumpLogs(fd, extendedLogDir, destDir, 20, EXTENDED_LOG_PREFIX);
    dumpModemEFS(destDir);
}

void Dumpstate::dumpRadioLogs(int fd, const std::string &destDir) {
    std::string tcpdumpLogDir = TCPDUMP_LOG_DIRECTORY;
    bool tcpdumpEnabled = ::android::base::GetBoolProperty(TCPDUMP_PERSIST_PROPERTY, false);

    if (tcpdumpEnabled) {
        dumpLogs(fd, tcpdumpLogDir, destDir, ::android::base::GetIntProperty(TCPDUMP_NUMBER_BUGREPORT, 5), TCPDUMP_LOG_PREFIX);
    }
    dumpRilLogs(fd, destDir);
    dumpNetmgrLogs(destDir);
}

void Dumpstate::dumpGpsLogs(int fd, const std::string &destDir) {
    bool gpsLogEnabled = ::android::base::GetBoolProperty(GPS_LOGGING_STATUS_PROPERTY, false);
    if (!gpsLogEnabled) {
        ALOGD("gps logging is not running\n");
        return;
    }
    const std::string gpsLogDir = GPS_LOG_DIRECTORY;
    const std::string gpsTmpLogDir = gpsLogDir + "/.tmp";
    const std::string gpsDestDir = destDir + "/gps";

    int maxFileNum = ::android::base::GetIntProperty(GPS_LOG_NUMBER_PROPERTY, 20);

    RunCommandToFd(fd, "MKDIR GPS LOG", {"/vendor/bin/mkdir", "-p", gpsDestDir.c_str()},
                   CommandOptions::WithTimeout(2).Build());

    dumpLogs(fd, gpsTmpLogDir, gpsDestDir, 1, GPS_LOG_PREFIX);
    dumpLogs(fd, gpsLogDir, gpsDestDir, 3, GPS_MCU_LOG_PREFIX);
    dumpLogs(fd, gpsLogDir, gpsDestDir, maxFileNum, GPS_LOG_PREFIX);
}

void Dumpstate::dumpCameraLogs(int fd, const std::string &destDir) {
    bool cameraLogsEnabled = ::android::base::GetBoolProperty(
            "vendor.camera.debug.camera_performance_analyzer.attach_to_bugreport", true);
    if (!cameraLogsEnabled) {
        return;
    }

    static const std::string kCameraLogDir = "/data/vendor/camera/profiler";
    const std::string cameraDestDir = destDir + "/camera";

    RunCommandToFd(fd, "MKDIR CAMERA LOG", {"/vendor/bin/mkdir", "-p", cameraDestDir.c_str()},
                   CommandOptions::WithTimeout(2).Build());
    // Attach multiple latest sessions (in case the user is running concurrent
    // sessions or starts a new session after the one with performance issues).
    dumpLogs(fd, kCameraLogDir, cameraDestDir, 10, "session-ended-");
    dumpLogs(fd, kCameraLogDir, cameraDestDir, 5, "high-drop-rate-");
    dumpLogs(fd, kCameraLogDir, cameraDestDir, 5, "watchdog-");
    dumpLogs(fd, kCameraLogDir, cameraDestDir, 5, "camera-ended-");
}

void Dumpstate::dumpGxpLogs(int fd, const std::string &destDir) {
    bool gxpDumpEnabled = ::android::base::GetBoolProperty("vendor.gxp.attach_to_bugreport", false);

    if (gxpDumpEnabled) {
        const int maxGxpDebugDumps = 8;
        const std::string gxpCoredumpOutputDir = destDir + "/gxp_ssrdump";
        const std::string gxpCoredumpInputDir = "/data/vendor/ssrdump";

        RunCommandToFd(fd, "MKDIR GXP COREDUMP", {"/vendor/bin/mkdir", "-p", gxpCoredumpOutputDir}, CommandOptions::WithTimeout(2).Build());

        // Copy GXP coredumps and crashinfo to the output directory.
        dumpLogs(fd, gxpCoredumpInputDir + "/coredump", gxpCoredumpOutputDir, maxGxpDebugDumps, "coredump_gxp_platform");
        dumpLogs(fd, gxpCoredumpInputDir, gxpCoredumpOutputDir, maxGxpDebugDumps, "crashinfo_gxp_platform");
    }
}

void Dumpstate::dumpLogSection(int fd, int fd_bin)
{
    std::string logDir = MODEM_LOG_DIRECTORY;
    const std::string logCombined = logDir + "/combined_logs.tar";
    const std::string logAllDir = logDir + "/all_logs";

    RunCommandToFd(fd, "MKDIR LOG", {"/vendor/bin/mkdir", "-p", logAllDir.c_str()}, CommandOptions::WithTimeout(2).Build());

    static const std::string sectionName = "modem DM log";
    auto startTime = startSection(fd, sectionName);
    bool modemLogEnabled = ::android::base::GetBoolProperty(MODEM_LOGGING_PERSIST_PROPERTY, false);
    if (modemLogEnabled && ::android::base::GetProperty(MODEM_LOGGING_PATH_PROPERTY, "") == MODEM_LOG_DIRECTORY) {
        bool modemLogStarted = ::android::base::GetBoolProperty(MODEM_LOGGING_STATUS_PROPERTY, false);
        int maxFileNum = ::android::base::GetIntProperty(MODEM_LOGGING_NUMBER_BUGREPORT_PROPERTY, 100);

        if (modemLogStarted) {
            ::android::base::SetProperty(MODEM_LOGGING_PROPERTY, "false");
            ALOGD("Stopping modem logging...\n");
        } else {
            ALOGD("modem logging is not running\n");
        }

        for (int i = 0; i < 15; i++) {
            if (!::android::base::GetBoolProperty(MODEM_LOGGING_STATUS_PROPERTY, false)) {
                ALOGD("modem logging stopped\n");
                sleep(1);
                break;
            }
            sleep(1);
        }

        dumpLogs(fd, logDir, logAllDir, maxFileNum, MODEM_LOG_PREFIX);

        if (modemLogStarted) {
            ALOGD("Restarting modem logging...\n");
            ::android::base::SetProperty(MODEM_LOGGING_PROPERTY, "true");
        }
    }
    endSection(fd, sectionName, startTime);

    // Dump all module logs
    if (!PropertiesHelper::IsUserBuild()) {
        for (const auto &section : mLogSections) {
            auto startTime = startSection(fd, section.first);
            section.second(fd, logAllDir);
            endSection(fd, section.first, startTime);
        }
    }

    RunCommandToFd(fd, "TAR LOG", {"/vendor/bin/tar", "cvf", logCombined.c_str(), "-C", logAllDir.c_str(), "."}, CommandOptions::WithTimeout(20).Build());
    RunCommandToFd(fd, "CHG PERM", {"/vendor/bin/chmod", "a+w", logCombined.c_str()}, CommandOptions::WithTimeout(2).Build());

    std::vector<uint8_t> buffer(65536);
    ::android::base::unique_fd fdLog(TEMP_FAILURE_RETRY(open(logCombined.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)));

    if (fdLog >= 0) {
        while (1) {
            ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fdLog, buffer.data(), buffer.size()));

            if (bytes_read == 0) {
                break;
            } else if (bytes_read < 0) {
                ALOGD("read(%s): %s\n", logCombined.c_str(), strerror(errno));
                break;
            }

            ssize_t result = TEMP_FAILURE_RETRY(write(fd_bin, buffer.data(), bytes_read));

            if (result != bytes_read) {
                ALOGD("Failed to write %ld bytes, actually written: %ld", bytes_read, result);
                break;
            }
        }
    }

    RunCommandToFd(fd, "RM LOG DIR", { "/vendor/bin/rm", "-r", logAllDir.c_str()}, CommandOptions::WithTimeout(2).Build());
    RunCommandToFd(fd, "RM LOG", { "/vendor/bin/rm", logCombined.c_str()}, CommandOptions::WithTimeout(2).Build());
}

ndk::ScopedAStatus Dumpstate::dumpstateBoard(const std::vector<::ndk::ScopedFileDescriptor>& in_fds,
                                             IDumpstateDevice::DumpstateMode in_mode,
                                             int64_t in_timeoutMillis) {
    // Unused arguments.
    (void) in_timeoutMillis;

    if (in_fds.size() < 1) {
        ALOGE("no FDs\n");
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                "No file descriptor");
    }

    int fd = in_fds[0].get();
    if (fd < 0) {
        ALOGE("invalid FD: %d\n", fd);
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                "Invalid file descriptor");
    }

    if (in_mode == IDumpstateDevice::DumpstateMode::WEAR) {
        // We aren't a Wear device.
        ALOGE("Unsupported mode: %d\n", in_mode);
        return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(ERROR_UNSUPPORTED_MODE,
                                                                           "Unsupported mode");
    } else if (in_mode < IDumpstateDevice::DumpstateMode::FULL || in_mode > IDumpstateDevice::DumpstateMode::PROTO) {
        ALOGE("Invalid mode: %d\n", in_mode);
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "Invalid mode");
    }

    if (in_fds.size() < 2) {
          ALOGE("no FD for dumpstate_board binary\n");
    } else {
          int fd_bin = in_fds[1].get();
          dumpLogSection(fd, fd_bin);
    }

    dumpTextSection(fd, kAllSections);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Dumpstate::setVerboseLoggingEnabled(bool in_enable) {
    ::android::base::SetProperty(kVerboseLoggingProperty, in_enable ? "true" : "false");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Dumpstate::getVerboseLoggingEnabled(bool* _aidl_return) {
    *_aidl_return = ::android::base::GetBoolProperty(kVerboseLoggingProperty, false);
    return ndk::ScopedAStatus::ok();
}

// Since AIDLs that support the dump() interface are automatically invoked during
// bugreport generation and we don't want to generate a second copy of the same
// data that will go into dumpstate_board.txt, this function will only do
// something if it is called with an option, e.g.
//   dumpsys android.hardware.dumpstate.IDumpstateDevice/default all
//
// Also, note that sections which generate attachments and/or binary data when
// included in a bugreport are not available through the dump() interface.
binder_status_t Dumpstate::dump(int fd, const char** args, uint32_t numArgs) {

    if (numArgs != 1) {
        return STATUS_OK;
    }

    dumpTextSection(fd, static_cast<std::string>(args[0]));

    fsync(fd);
    return STATUS_OK;
}

}  // namespace dumpstate
}  // namespace hardware
}  // namespace android
}  // namespace aidl
