/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"

#include "app_lcd.h"
#include "app_touch.h"

static const char *TAG = "p3a";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Physical Player of Pixel Art (P3A)");

    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_touch_init());

    ESP_LOGI(TAG, "P3A ready: tap the display to cycle animations");
}
