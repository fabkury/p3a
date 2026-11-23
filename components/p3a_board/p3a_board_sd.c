// SPDX-License-Identifier: Apache-2.0
// The microSD card: mounting and unmounting the FAT volume.
//
// This is only the mount. Whether the card is healthy, repairing what it
// damaged, and where p3a keeps its files on it are the storage layer's
// business (sd_health, sd_repair, sd_path).

#include "p3a_board.h"

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
// After ff.h: it uses FatFS's types without including them.
#include "diskio_sdmmc.h"

static const char *TAG = "p3a_board_sd";

static bool s_mounted;

esp_err_t p3a_board_sd_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    // The BSP mounts slot 0, 4-bit, at BSP_SD_MOUNT_POINT. It never formats a
    // card that fails to mount (CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL is off on
    // purpose: an unreadable card gets the explicit on-device format flow).
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    return ESP_OK;
}

esp_err_t p3a_board_sd_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    esp_err_t err = bsp_sdcard_unmount();
    if (err == ESP_OK) {
        s_mounted = false;
    }
    return err;
}

bool p3a_board_sd_is_mounted(void)
{
    return s_mounted;
}

// Mounts a card that would not mount, by making a file system on it: the
// BSP's own mount with "format if the mount fails" set. The host and slot
// settings are the BSP's (slot 0, 4-bit, its pins). This is the only path
// that also writes a partition table, which a blank card needs.
static esp_err_t mount_formatting(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    const sdmmc_slot_config_t slot = {.cd = SDMMC_SLOT_NO_CD, .wp = SDMMC_SLOT_NO_WP, .width = 4, .flags = 0};
    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount, &bsp_sdcard);
}

esp_err_t p3a_board_sd_format(void)
{
    esp_err_t err;
    if (s_mounted) {
        err = esp_vfs_fat_sdcard_format(BSP_SD_MOUNT_POINT, bsp_sdcard);
        if (err != ESP_OK) {
            // A failed format may have recycled the mount; the handle is not
            // to be used again.
            bsp_sdcard = NULL;
            s_mounted = false;
        }
    } else {
        // The LDO that powers the card is on: the boot's mount attempt went
        // through the BSP, which takes it.
        err = mount_formatting();
        s_mounted = err == ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the card could not be formatted: %s", esp_err_to_name(err));
        return err;
    }
#if FF_USE_LABEL
    const BYTE drive = ff_diskio_get_pdrv_card(bsp_sdcard);
    if (drive != 0xFF) {
        char label[8];
        snprintf(label, sizeof(label), "%u:P3A", (unsigned)drive);
        if (f_setlabel(label) != FR_OK) {
            ESP_LOGW(TAG, "the volume label was not set");  // the format itself is good
        }
    }
#endif
    ESP_LOGW(TAG, "the card was formatted");
    return p3a_board_sd_unmount();
}

sdmmc_card_t *p3a_board_sd_card(void)
{
    return s_mounted ? bsp_sdcard : NULL;
}
