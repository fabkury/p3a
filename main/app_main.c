#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

#include "board.h"
#include "p3a_hal/display.h"
#include "p3a_hal/touch.h"
#include "storage.h"
#include "storage/kv.h"
#include "storage/fs.h"
#include "storage/cache.h"
#include "nvs.h"
#include "net.h"
#include "file_transfer.h"
#include "graphics_mode.h"
#include "sd_ring.h"

static const char *TAG = "app_main";

static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "Chip model: %s", "ESP32-P4");
    ESP_LOGI(TAG, "Cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Revision: %d", chip_info.revision);

    ESP_LOGI(TAG, "Features:%s%s%s%s%s",
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? " Wi-Fi" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? " BLE" : "",
             (chip_info.features & CHIP_FEATURE_BT) ? " BT" : "",
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? " Embedded-Flash" : "",
             (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? " Embedded-PSRAM" : "");
}

void app_main(void)
{
    ESP_LOGI(TAG, "P3A firmware bring-up starting...");
    
    // Log initial heap state
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Initial heap: free=%zu bytes, min_free=%zu bytes", free_heap, min_free_heap);
    
    log_chip_info();
    ESP_ERROR_CHECK(board_init());

    ESP_LOGI(TAG, "Initialising storage subsystem");
    ESP_ERROR_CHECK(storage_init());

    // ESP_LOGI(TAG, "Initialising file transfer");
    // ESP_ERROR_CHECK(file_transfer_init());  // Temporarily disabled - conflicts with console UART

    ESP_LOGI(TAG, "Initialising networking subsystem");
    ESP_ERROR_CHECK(net_init());

    // Test KV store: boot counter
    void *kv_handle = storage_kv_open_namespace("system", "rw");
    if (kv_handle) {
        int32_t boot_count = 0;
        esp_err_t ret = storage_kv_get_i32(kv_handle, "boot_count", &boot_count);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            boot_count = 1;
            ESP_LOGI(TAG, "First boot detected");
        } else {
            boot_count++;
        }
        ESP_ERROR_CHECK(storage_kv_set_i32(kv_handle, "boot_count", boot_count));
        ESP_LOGI(TAG, "Boot count: %" PRId32, boot_count);
        storage_kv_close_namespace(kv_handle);
    }

    // Log filesystem status
    storage_fs_status_t fs_status;
    if (storage_fs_get_status(&fs_status) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: %s", fs_status.spiffs_mounted ? "mounted" : "not mounted");
        if (fs_status.sd_mounted) {
            ESP_LOGI(TAG, "SD card: mounted, total: %llu MB, free: %llu MB",
                     fs_status.sd_total_bytes / (1024 * 1024),
                     fs_status.sd_free_bytes / (1024 * 1024));
        } else {
            ESP_LOGI(TAG, "SD card: not mounted (will monitor for insertion)");
        }
    }

    // Log storage subsystem status including cache
    storage_status_t storage_status;
    if (storage_get_status(&storage_status) == ESP_OK) {
        ESP_LOGI(TAG, "Storage status: KV=%s, FS=%s, Cache=%s",
                 storage_status.kv_initialized ? "ok" : "fail",
                 storage_status.fs_initialized ? "ok" : "fail",
                 storage_status.cache_initialized ? "ok" : "fail");
        if (storage_status.cache_initialized) {
            ESP_LOGI(TAG, "Cache stats: entries=%u/%u, size=%llu/%llu MB, hits=%u, misses=%u",
                     storage_status.cache_stats.total_entries,
                     storage_status.cache_stats.max_entries,
                     storage_status.cache_stats.total_size_bytes / (1024 * 1024),
                     storage_status.cache_stats.max_size_bytes / (1024 * 1024),
                     storage_status.cache_stats.hit_count,
                     storage_status.cache_stats.miss_count);
        }
    }

    ESP_LOGI(TAG, "Initialising display stack");
    ESP_ERROR_CHECK(p3a_hal_display_init());
    
    // Set brightness immediately after display init (display_init sets it to 10%)
    ESP_ERROR_CHECK(p3a_hal_display_set_brightness(90));
    ESP_ERROR_CHECK(p3a_hal_touch_init());

    // Initialize SD ring buffer early (before player)
    ESP_LOGI(TAG, "Initializing SD ring buffer");
    ESP_ERROR_CHECK(sd_ring_init(256 * 1024, 3));  // 3 chunks × 256 KiB

    ESP_LOGI(TAG, "Starting graphics mode controller");
    graphics_mode_init();
    
    // Log heap state after graphics initialization
    size_t free_heap_after = esp_get_free_heap_size();
    size_t min_free_heap_after = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Heap after graphics init: free=%zu bytes, min_free=%zu bytes", 
             free_heap_after, min_free_heap_after);
    
    // Give LVGL time to render and flush to display
    vTaskDelay(pdMS_TO_TICKS(200));

    // Attempt Wi-Fi connection
    ESP_LOGI(TAG, "Attempting Wi-Fi connection...");
    esp_err_t wifi_ret = net_wifi_connect();
    if (wifi_ret == ESP_OK) {
        char ssid[64] = {0};
        if (net_wifi_get_ssid(ssid, sizeof(ssid)) == ESP_OK) {
            ESP_LOGI(TAG, "Connected to Wi-Fi: %s", ssid);
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi connection failed or provisioning started (ret=%s)", esp_err_to_name(wifi_ret));
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
