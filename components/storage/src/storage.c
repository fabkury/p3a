#include "storage.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "storage";
static bool s_initialized = false;

esp_err_t storage_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Storage already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing storage subsystem...");

    ESP_RETURN_ON_ERROR(storage_kv_init(), TAG, "NVS initialization failed");
    ESP_RETURN_ON_ERROR(storage_fs_init(), TAG, "Filesystem initialization failed");
    
    // Initialize cache after filesystem (cache depends on SD mount)
    ESP_RETURN_ON_ERROR(storage_cache_init(), TAG, "Cache initialization failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Storage subsystem initialized successfully");
    return ESP_OK;
}

esp_err_t storage_get_status(storage_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    status->kv_initialized = s_initialized;
    status->fs_initialized = s_initialized;
    status->cache_initialized = storage_cache_is_initialized();
    
    if (s_initialized) {
        ESP_RETURN_ON_ERROR(storage_fs_get_status(&status->fs_status), TAG, "Failed to get FS status");
        ESP_RETURN_ON_ERROR(storage_cache_get_stats(&status->cache_stats), TAG, "Failed to get cache stats");
    }

    return ESP_OK;
}

