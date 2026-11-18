// SPDX-License-Identifier: Apache-2.0
// Bringing the SD card into service, and the cached free-space figure.

#include "p3a_storage.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "p3a_board.h"
#include "p3a_err.h"
#include "p3a_job.h"
#include "sd_health.h"
#include "sd_path.h"
#include "sd_repair.h"

static const char *TAG = "p3a_storage";

static portMUX_TYPE s_space_lock = portMUX_INITIALIZER_UNLOCKED;
static p3a_storage_space_t s_space;

p3a_storage_space_t p3a_storage_space(void)
{
    taskENTER_CRITICAL(&s_space_lock);
    const p3a_storage_space_t copy = s_space;
    taskEXIT_CRITICAL(&s_space_lock);
    return copy;
}

esp_err_t p3a_storage_space_refresh(void)
{
    if (!p3a_board_sd_is_mounted()) {
        return P3A_ERR_MISSING;
    }
    uint64_t total = 0, free_bytes = 0;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_vfs_fat_info(P3A_SD_MOUNT_POINT, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not measure free space: %s", esp_err_to_name(err));
        return err;
    }
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_space_lock);
    s_space = (p3a_storage_space_t){.valid = true, .total_bytes = total, .free_bytes = free_bytes, .measured_at_us = now};
    taskEXIT_CRITICAL(&s_space_lock);
    ESP_LOGI(TAG, "card: %llu MB free of %llu MB (measured in %d ms)", (unsigned long long)(free_bytes >> 20),
             (unsigned long long)(total >> 20), (int)((now - t0) / 1000));
    return ESP_OK;
}

static esp_err_t space_job(void *arg)
{
    (void)arg;
    return p3a_storage_space_refresh();
}

esp_err_t p3a_storage_start(const char *configured_root)
{
    sd_health_init();

    // 1. Mount.
    esp_err_t err = p3a_board_sd_mount();
    if (err != ESP_OK) {
        sd_health_report_mount_failed();
        return err;
    }

    // 2. The FAT mirror check must run before anything reads the FAT. FatFS
    //    reads only the first copy, so a sector the card overwrote there would
    //    send every chain through it astray. After a repair, remount so that
    //    nothing stale survives in FatFS's window.
    sd_repair_fat_stats_t fat = {0};
    sd_repair_fat_mirror(p3a_board_sd_card(), true, &fat);
    if (fat.repaired_fat1 || fat.repaired_fat2) {
        ESP_LOGW(TAG, "FAT repaired (%u sectors in copy 1, %u in copy 2); remounting", (unsigned)fat.repaired_fat1,
                 (unsigned)fat.repaired_fat2);
        p3a_board_sd_unmount();
        err = p3a_board_sd_mount();
        if (err != ESP_OK) {
            sd_health_report_mount_failed();
            return err;
        }
    }

    // 3. The root folder.
    if (sd_path_init(configured_root) != ESP_OK) {
        ESP_LOGW(TAG, "the stored SD root \"%s\" is not valid; using %s", configured_root, sd_path_get_root());
    }
    ESP_LOGI(TAG, "SD root: %s", sd_path_get_root());

    // 4. Folders, then 5. the probe. The probe latches the failure itself.
    sd_path_ensure_directories();
    err = sd_health_boot_probe();
    if (err != ESP_OK) {
        return P3A_ERR_SD_FAILED;
    }

    // Free space is slow to count; do it off the boot path.
    if (p3a_job_submit(P3A_JOBS_IO, "storage_space", space_job, NULL, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "free space will be unknown until something measures it");
    }
    return ESP_OK;
}
