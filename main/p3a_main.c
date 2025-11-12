/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_lcd.h"
#include "app_touch.h"
#include "app_wifi.h"
#include "http_api.h"

static const char *TAG = "p3a";

#define AUTO_SWAP_INTERVAL_SECONDS CONFIG_P3A_AUTO_SWAP_INTERVAL_SECONDS

static TaskHandle_t s_auto_swap_task_handle = NULL;

static void auto_swap_task(void *arg)
{
    (void)arg;
    const TickType_t delay_ticks = pdMS_TO_TICKS(AUTO_SWAP_INTERVAL_SECONDS * 1000);
    
    ESP_LOGI(TAG, "Auto-swap task started: will cycle forward every %d seconds", AUTO_SWAP_INTERVAL_SECONDS);
    
    // Wait a bit for system to initialize before first swap
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    while (true) {
        // Wait for interval or notification (which resets the timer)
        uint32_t notified = ulTaskNotifyTake(pdTRUE, delay_ticks);
        if (notified > 0) {
            ESP_LOGD(TAG, "Auto-swap timer reset by user interaction");
            continue;  // Timer was reset, start waiting again
        }
        // Timeout occurred, perform auto-swap
        ESP_LOGD(TAG, "Auto-swap: cycling forward");
        app_lcd_cycle_animation();
    }
}

void auto_swap_reset_timer(void)
{
    if (s_auto_swap_task_handle != NULL) {
        xTaskNotifyGive(s_auto_swap_task_handle);
    }
}

static void register_rest_action_handlers(void)
{
    // Register action handlers for HTTP API swap commands
    http_api_set_action_handlers(
        app_lcd_cycle_animation,           // swap_next callback
        app_lcd_cycle_animation_backward   // swap_back callback
    );
    ESP_LOGI(TAG, "REST action handlers registered");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Physical Player of Pixel Art (P3A)");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network interface and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize LCD and touch
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_touch_init());

    // Create auto-swap task
    const BaseType_t created = xTaskCreate(auto_swap_task, "auto_swap", 2048, NULL, 
                                           tskIDLE_PRIORITY + 1, &s_auto_swap_task_handle);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-swap task");
    }

    // Initialize Wi-Fi (will start captive portal if needed, or connect to saved network)
    ESP_ERROR_CHECK(app_wifi_init(register_rest_action_handlers));

    ESP_LOGI(TAG, "P3A ready: tap the display to cycle animations (auto-swap forward every %d seconds)", AUTO_SWAP_INTERVAL_SECONDS);
}
