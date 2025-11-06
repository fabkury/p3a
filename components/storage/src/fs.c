#include "storage/fs.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "board/pins.h"
#include "storage/cache.h"
#include "ff.h"

static const char *TAG = "storage_fs";

#define SPIFFS_MOUNT_POINT "/spiffs"
#define SD_MOUNT_POINT "/sdcard"

static bool s_spiffs_mounted = false;
static bool s_sd_mounted = false;
static sdmmc_card_t *s_sd_card = NULL;

const char *storage_fs_get_spiffs_path(void)
{
    return SPIFFS_MOUNT_POINT;
}

const char *storage_fs_get_sd_path(void)
{
    return SD_MOUNT_POINT;
}

static esp_err_t mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format SPIFFS filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS partition size: total: %d KB, used: %d KB", total / 1024, used / 1024);
    }

    s_spiffs_mounted = true;
    return ESP_OK;
}

static esp_err_t unmount_sd(void)
{
    if (s_sd_card) {
        esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_sd_card);
        s_sd_card = NULL;
        s_sd_mounted = false;
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card unmounted");
        }
        return ret;
    }
    return ESP_OK;
}

static esp_err_t mount_sd(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,  // Enable auto-format if filesystem not found
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;  // Explicitly set slot
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // Use high speed mode

    // Use default slot config - Slot 0 uses IO MUX pins automatically
    // Don't override pins as they may conflict with IO MUX routing
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;  // 4-bit mode
    slot_config.cd = SDMMC_SLOT_NO_CD;  // No card detect pin
    slot_config.wp = SDMMC_SLOT_NO_WP;  // No write protect pin

    ESP_LOGI(TAG, "Mounting SD card on Slot 0 (4-bit mode, auto-format enabled)...");
    
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Card may not be inserted or filesystem may need formatting");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "SD card not found");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SD card already mounted");
        }
        s_sd_mounted = false;
        s_sd_card = NULL;
        return ret;
    }

    sdmmc_card_print_info(stdout, s_sd_card);
    
    s_sd_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

static void sd_hotplug_task(void *pvParameters)
{
    const TickType_t poll_delay = pdMS_TO_TICKS(1000);
    bool last_state = false;

    while (1) {
        bool current_state = storage_fs_is_sd_present();
        
        if (current_state != last_state) {
            if (current_state) {
                ESP_LOGI(TAG, "SD card insertion detected");
                mount_sd();
                // Reinitialize cache when SD is inserted
                storage_cache_init();
            } else {
                ESP_LOGI(TAG, "SD card removal detected");
                unmount_sd();
            }
            last_state = current_state;
        }
        
        vTaskDelay(poll_delay);
    }
}

bool storage_fs_is_sd_present(void)
{
    return s_sd_mounted && (s_sd_card != NULL);
}

esp_err_t storage_fs_init(void)
{
    esp_err_t ret = mount_spiffs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed");
        return ret;
    }

    ret = mount_sd();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available at init (will monitor for insertion)");
    }

    xTaskCreate(sd_hotplug_task, "sd_hotplug", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Filesystem initialization complete");
    
    return ESP_OK;
}

esp_err_t storage_fs_get_status(storage_fs_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    status->spiffs_mounted = s_spiffs_mounted;
    status->sd_mounted = s_sd_mounted;

    if (s_sd_card && s_sd_mounted) {
        // Get card info from sdmmc_card_t
        // csd.capacity is in 512-byte sectors
        status->sd_total_bytes = (uint64_t)s_sd_card->csd.capacity * 512;
        
        // Use FATFS API to get free space
        // FATFS volume is typically "0:" for first mount
        FATFS *fs;
        DWORD fre_clust, fre_sect;
        
        // Get filesystem info using FATFS f_getfree
        // Mount point "/sdcard" maps to drive "0:" in FATFS
        FRESULT res = f_getfree("0:", &fre_clust, &fs);
        if (res == FR_OK && fs) {
            // Calculate free sectors: free clusters * sectors per cluster
            fre_sect = fre_clust * fs->csize;
            status->sd_free_bytes = (uint64_t)fre_sect * 512;
        } else {
            // Fallback: use total capacity as placeholder
            status->sd_free_bytes = status->sd_total_bytes;
        }
    }

    return ESP_OK;
}

