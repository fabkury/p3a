/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_lcd.h"
#include "app_touch.h"

static const char *TAG = "p3a";

#define AUTO_SWAP_INTERVAL_SECONDS 30

static TaskHandle_t s_auto_swap_task_handle = NULL;

static void auto_swap_task(void *arg)
{
    (void)arg;
    const TickType_t delay_ticks = pdMS_TO_TICKS(AUTO_SWAP_INTERVAL_SECONDS * 1000);
    
    ESP_LOGI(TAG, "Auto-swap task started: will cycle to random animations every %d seconds", AUTO_SWAP_INTERVAL_SECONDS);
    
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
        ESP_LOGD(TAG, "Auto-swap: cycling to random animation");
        app_lcd_cycle_to_random();
    }
}

void auto_swap_reset_timer(void)
{
    if (s_auto_swap_task_handle != NULL) {
        xTaskNotifyGive(s_auto_swap_task_handle);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Physical Player of Pixel Art (P3A)");

    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_touch_init());

    // Create auto-swap task
    const BaseType_t created = xTaskCreate(auto_swap_task, "auto_swap", 2048, NULL, 
                                           tskIDLE_PRIORITY + 1, &s_auto_swap_task_handle);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-swap task");
    }

    ESP_LOGI(TAG, "P3A ready: tap the display to cycle animations (auto-swap to random every %d seconds)", AUTO_SWAP_INTERVAL_SECONDS);
}
