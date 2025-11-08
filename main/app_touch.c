/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"
#include "app_lcd.h"
#include "app_touch.h"
#include "bsp/display.h"

static const char *TAG = "app_touch";

static esp_lcd_touch_handle_t tp = NULL;

static void app_touch_task(void *arg)
{
    const TickType_t poll_delay = pdMS_TO_TICKS(CONFIG_P3A_TOUCH_POLL_INTERVAL_MS);
    uint16_t x[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint16_t y[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint16_t strength[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t touch_count = 0;
    bool touch_active = false;

    while (true) {
        esp_lcd_touch_read_data(tp);
        bool pressed = esp_lcd_touch_get_coordinates(tp, x, y, strength, &touch_count,
                                                     CONFIG_ESP_LCD_TOUCH_MAX_POINTS);

        if (pressed && touch_count > 0) {
            if (!touch_active) {
                ESP_LOGD(TAG, "touch press @(%u,%u) strength %u", x[0], y[0], strength[0]);
                // Check if touch is on left or right half of screen
                // Screen width is BSP_LCD_H_RES (720 pixels), so midpoint is 360
                const uint16_t screen_midpoint = BSP_LCD_H_RES / 2;
                if (x[0] < screen_midpoint) {
                    // Left half: cycle backward
                    app_lcd_cycle_animation_backward();
                } else {
                    // Right half: cycle forward
                    app_lcd_cycle_animation();
                }
            }
            touch_active = true;
        } else if (touch_active) {
            ESP_LOGD(TAG, "touch release");
            touch_active = false;
        }

        vTaskDelay(poll_delay);
    }
}

esp_err_t app_touch_init(void)
{
    esp_err_t err = bsp_touch_new(NULL, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
        return err;
    }

    const BaseType_t created = xTaskCreate(app_touch_task, "app_touch_task", 4096, NULL,
                                           CONFIG_P3A_TOUCH_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "touch task creation failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}
