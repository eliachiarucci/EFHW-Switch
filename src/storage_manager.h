// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 Elia Chiarucci

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/random/rand32.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>

#define STORAGE_THREAD_STACK_SIZE 1024
#define STORAGE_THREAD_PRIORITY 7
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(custom_storage)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(custom_storage)

#define DEVICE_HW_REVISION 1

#define HW_REV_KEY 1
#define ADV_NAME_KEY 0

static struct nvs_fs fs;

int storage_manager_init()
{
    int rc = 0;
    struct flash_pages_info info;
    /* define the nvs file system by settings with:
     *	sector_size equal to the pagesize,
     *	3 sectors
     *	starting at NVS_PARTITION_OFFSET
     */
    fs.flash_device = NVS_PARTITION_DEVICE;
    if (!device_is_ready(fs.flash_device))
    {
        LOG_ERR("Flash device %s is not ready\n", fs.flash_device->name);
        return -1;
    }
    fs.offset = NVS_PARTITION_OFFSET;
    rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
    if (rc)
    {
        LOG_ERR("Unable to get page info, rc=%d\n", rc);
        return -1;
    }
    fs.sector_size = info.size;
    fs.sector_count = 4U;

    rc = nvs_mount(&fs);
    if (rc)
    {
        LOG_ERR("Flash Init failed, rc=%d\n", rc);
        return -1;
    }

    return 0;
}

void write_adv_name(char *name, bool reboot)
{
    char buf[32];
    size_t name_len = strlen(name);

    if (name_len >= sizeof(buf))
    {
        LOG_ERR("Name too long (%u bytes), max allowed is %u",
                name_len, sizeof(buf) - 1);
        return;
    }

    strcpy(buf, name);
    LOG_INF("Writing %s to flash\n", buf);
    nvs_write(&fs, ADV_NAME_KEY, buf, name_len + 1);
    if (reboot)
    {
        sys_reboot(1);
    }
}

char *read_adv_name()
{
    static char buf[32];
    ssize_t err = nvs_read(&fs, ADV_NAME_KEY, buf, sizeof(buf));

    if (err < 0)
    {
        printk("Flash read failed, err=%zd\n", err);
        // Ensure the random number is within the 8-digit range
        uint32_t random_id = 10000000 + (sys_rand32_get() % 90000000);
        snprintf(buf, sizeof(buf), "BR1_%d", random_id);
        LOG_INF("New adv name created: %s\n", buf);
        write_adv_name(buf, false);
        return buf;
    }

    LOG_INF("Read back: %s\n", buf);
    return buf;
};

// Function to write hardware revision to NVS
int write_hw_revision(uint32_t revision)
{
    int rc = nvs_write(&fs, HW_REV_KEY, &revision, sizeof(revision));
    if (rc < 0)
    {
        LOG_ERR("Failed to write hardware revision to NVS (err %d)", rc);
        return rc;
    }

    LOG_INF("Hardware revision '%u' written to flash", revision);
    return 0;
}

// Function to read hardware revision from NVS
int read_hw_revision(uint32_t *revision)
{
    if (!revision)
    {
        return -EINVAL;
    }

    int rc = nvs_read(&fs, HW_REV_KEY, revision, sizeof(*revision));
    if (rc < 0 && rc != -ENOENT)
    {
        LOG_ERR("Failed to read hardware revision from NVS (err %d)", rc);
        return rc;
    }

    if (rc == -ENOENT)
    {
        LOG_WRN("No hardware revision found in NVS");
        *revision = 0; // Default to 0 if not found
        return rc;
    }

    LOG_INF("Read hardware revision: '%u'", *revision);
    return 0;
}

// This function should be called during device initialization
int init_hw_revision(void)
{
    uint32_t current_rev;
    int rc = read_hw_revision(&current_rev);

    // -ENOENT (never written, current_rev = 0) is the case that must go on and write it
    if (rc < 0 && rc != -ENOENT)
    {
        return rc;
    }

    // If hardware revision is not set (0), set it
    if (current_rev == 0)
    {
        // Define your hardware revision integer here or pass it as a build variable
        uint32_t hw_rev = DEVICE_HW_REVISION;
        if (hw_rev == 0)
        {
            hw_rev = 1; // Default value if not defined
        }

        rc = write_hw_revision(hw_rev);
        if (rc < 0)
        {
            LOG_ERR("Failed to initialize hardware revision");
            return rc;
        }

        LOG_INF("Hardware revision initialized to '%u'", hw_rev);
    }
    else
    {
        LOG_INF("Using existing hardware revision: '%u'", current_rev);
    }

    return 0;
}