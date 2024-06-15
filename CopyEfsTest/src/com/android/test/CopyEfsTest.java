/*
 * Copyright (C) 2024 The Android Open Source Project
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

package com.android.test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import android.platform.test.annotations.AppModeFull;

import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.testtype.junit4.DeviceTestRunOptions;
import com.android.tradefed.util.RunUtil;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.BufferedReader;
import java.io.StringReader;

@RunWith(DeviceJUnit4ClassRunner.class)
public class CopyEfsTest extends BaseHostJUnit4Test {




    @Test
    @AppModeFull
    public void copyEfsTest() throws Exception {

        getDevice().enableAdbRoot();

        // This test can be run on OEM unlocked device only as unlocking bootloader requires
        // manual intervention.
        String result = getDevice().getProperty("ro.boot.flash.locked");
        assumeTrue("0".equals(result));
        final String dataFstype = getFsTypeFor("/data");
        assertTrue(!dataFstype.isEmpty());
        if (!dataFstype.equals("ext4")) {
            getDevice().executeShellCommand("cmd recovery wipe ext4");
            RunUtil.getDefault().sleep(10000);
            // Wait for 2 mins device to be online againg
            getDevice().waitForDeviceOnline(120000);
            getDevice().enableAdbRoot();
        }
        assertEquals("ext4", getFsTypeFor("/data"));
        String dataBlockDev = getBlockDevFor("/data");
        assertEquals(dataBlockDev, getBlockDevFor("/mnt/vendor/efs"));
        assertEquals(dataBlockDev, getBlockDevFor("/mnt/vendor/efs_backup"));
        assertEquals(dataBlockDev, getBlockDevFor("/mnt/vendor/modem_userdata"));
    }

    private String[] getMountInfo(String mountPoint) throws Exception {
        String result = getDevice().executeShellCommand("cat /proc/mounts");
        BufferedReader br = new BufferedReader(new StringReader(result));
        String line;
        while ((line = br.readLine()) != null) {
            final String[] fields = line.split(" ");
            final String device = fields[0];
            final String partition = fields[1];
            final String fsType = fields[2];
            if (partition.equals(mountPoint)) {
                return fields;
            }
        }
        return null;
    }
    private String getFsTypeFor(String mountPoint) throws Exception {
        String[] result = getMountInfo(mountPoint);
        if (result == null) {
            return "";
        }
        return result[2];
    }
    private String getBlockDevFor(String mountPoint) throws Exception {
        String[] result = getMountInfo(mountPoint);
        if (result == null) {
            return "";
        }
        return result[0];
    }
}
