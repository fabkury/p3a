#include "p3a_board.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include <string.h>

static const char *TAG = "p3a_board_fs";
static bool s_spiffs_mounted = false;

esp_err_t p3a_board_spiffs_mount(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    s_spiffs_mounted = true;
    ESP_LOGI(TAG, "LittleFS mounted successfully");
    return ESP_OK;
}

bool p3a_board_spiffs_is_mounted(void)
{
    return s_spiffs_mounted;
}

