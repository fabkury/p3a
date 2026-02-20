#include "p3a_board.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "p3a_board_fs";
static bool s_littlefs_mounted = false;
static bool s_webui_healthy = false;

esp_err_t p3a_board_littlefs_mount(void)
{
    if (s_littlefs_mounted) {
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/webui",
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

    s_littlefs_mounted = true;
    ESP_LOGI(TAG, "LittleFS mounted successfully");
    return ESP_OK;
}

bool p3a_board_littlefs_is_mounted(void)
{
    return s_littlefs_mounted;
}

/**
 * @brief Check if web UI partition is healthy
 *
 * This should be called after mounting LittleFS to verify:
 * 1. NVS storage_partition_invalid flag is not set
 * 2. version.txt exists and is readable
 *
 * @return true if partition is healthy
 */
static bool check_webui_partition_health(void)
{
    // Check NVS flag for partition invalid marker
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ota", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t invalid = 0;
        if (nvs_get_u8(nvs, "webui_invalid", &invalid) == ESP_OK) {
            if (invalid != 0) {
                ESP_LOGW(TAG, "Web UI partition marked invalid in NVS");
                nvs_close(nvs);
                return false;
            }
        }
        nvs_close(nvs);
    }

    // Check if version.txt exists and is readable
    FILE *f = fopen("/webui/version.txt", "r");
    if (!f) {
        ESP_LOGW(TAG, "Web UI version.txt not found");
        return false;
    }

    char version[32] = {0};
    if (fgets(version, sizeof(version), f) == NULL) {
        ESP_LOGW(TAG, "Failed to read version.txt");
        fclose(f);
        return false;
    }
    fclose(f);

    // Trim and validate
    size_t len = strlen(version);
    while (len > 0 && (version[len-1] == '\n' || version[len-1] == '\r')) {
        version[--len] = '\0';
    }

    if (len == 0) {
        ESP_LOGW(TAG, "Web UI version.txt is empty");
        return false;
    }

    ESP_LOGI(TAG, "Web UI partition healthy, version: %s", version);
    return true;
}

/**
 * @brief Set needs_recovery flag in NVS
 */
static void set_webui_needs_recovery(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ota", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, "webui_recover", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGW(TAG, "Web UI recovery flagged for next OTA check");
    }
}

bool p3a_board_webui_is_healthy(void)
{
    return s_webui_healthy;
}

esp_err_t p3a_board_littlefs_check_health(void)
{
    if (!s_littlefs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    s_webui_healthy = check_webui_partition_health();

    if (!s_webui_healthy) {
        set_webui_needs_recovery();
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

