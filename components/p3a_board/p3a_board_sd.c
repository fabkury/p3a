// SPDX-License-Identifier: Apache-2.0
// The microSD card: mounting and unmounting the FAT volume.
//
// This is only the mount. Whether the card is healthy, repairing what it
// damaged, and where p3a keeps its files on it are the storage layer's
// business (sd_health, sd_repair, sd_path).

#include "p3a_board.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"

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

sdmmc_card_t *p3a_board_sd_card(void)
{
    return s_mounted ? bsp_sdcard : NULL;
}
