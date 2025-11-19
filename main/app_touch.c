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
#include "app_usb.h"
#include "app_touch.h"
#include "bsp/display.h"
#include "sdkconfig.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "app_touch";


static esp_lcd_touch_handle_t tp = NULL;

/**
 * @brief Gesture state machine states
 * 
 * The touch handler distinguishes between tap gestures (for animation swapping)
 * and swipe gestures (for brightness control) based on vertical movement distance.
 */
typedef enum {
    GESTURE_STATE_IDLE,        // No active touch
    GESTURE_STATE_TAP,        // Potential tap/swap gesture (minimal movement)
    GESTURE_STATE_BRIGHTNESS  // Brightness control gesture (vertical swipe detected)
} gesture_state_t;

/**
 * @brief Touch task implementing gesture recognition
 * 
 * This task polls the touch controller and implements a state machine to distinguish
 * between:
 * - Tap gestures: Used for animation swapping (left/right half of screen)
 * - Vertical swipe gestures: Used for brightness control
 * 
 * Gesture classification:
 * - If vertical movement >= CONFIG_P3A_TOUCH_SWIPE_MIN_HEIGHT_PERCENT, it's a brightness gesture
 * - Otherwise, on release it's treated as a tap gesture for animation swapping
 * 
 * Brightness control:
 * - Swipe up (lower to higher Y) increases brightness
 * - Swipe down (higher to lower Y) decreases brightness
 * - Brightness change is proportional to vertical distance
 * - Maximum change per full-screen swipe is CONFIG_P3A_TOUCH_BRIGHTNESS_MAX_DELTA_PERCENT
 * - Brightness updates continuously as finger moves during swipe
 * - Auto-swap timer is reset when brightness gesture starts
 */
static uint16_t scale_to_pico8(uint16_t value, uint16_t max_src, uint16_t max_dst)
{
    if (max_src == 0) {
        return 0;
    }
    const uint32_t denom = (max_src > 1) ? (uint32_t)(max_src - 1) : 1U;
    uint32_t scaled = ((uint32_t)value * (uint32_t)max_dst) / denom;
    if (scaled > max_dst) {
        scaled = max_dst;
    }
    return (uint16_t)scaled;
}

static void app_touch_task(void *arg)
{
    const TickType_t poll_delay = pdMS_TO_TICKS(CONFIG_P3A_TOUCH_POLL_INTERVAL_MS);
    uint16_t x[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint16_t y[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint16_t strength[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t touch_count = 0;
    
    gesture_state_t gesture_state = GESTURE_STATE_IDLE;
    uint16_t touch_start_x = 0;
    uint16_t touch_start_y = 0;
    int brightness_start = 100;  // Brightness at gesture start
    const uint16_t screen_height = BSP_LCD_V_RES;
    const uint16_t min_swipe_height = (screen_height * CONFIG_P3A_TOUCH_SWIPE_MIN_HEIGHT_PERCENT) / 100;
    const int max_brightness_delta = CONFIG_P3A_TOUCH_BRIGHTNESS_MAX_DELTA_PERCENT;

    bool last_touch_valid = false;
    uint16_t last_scaled_x = 0;
    uint16_t last_scaled_y = 0;

    while (true) {
        esp_lcd_touch_read_data(tp);
        bool pressed = esp_lcd_touch_get_coordinates(tp, x, y, strength, &touch_count,
                                                     CONFIG_ESP_LCD_TOUCH_MAX_POINTS);

        if (pressed && touch_count > 0) {
            uint16_t scaled_x = scale_to_pico8(x[0], BSP_LCD_H_RES, 127);
            uint16_t scaled_y = scale_to_pico8(y[0], BSP_LCD_V_RES, 127);
            bool coords_changed = (!last_touch_valid) || (scaled_x != last_scaled_x) || (scaled_y != last_scaled_y);

            if (gesture_state == GESTURE_STATE_IDLE) {
                // Touch just started
                touch_start_x = x[0];
                touch_start_y = y[0];
                brightness_start = app_lcd_get_brightness();
                gesture_state = GESTURE_STATE_TAP;
                ESP_LOGD(TAG, "touch start @(%u,%u)", touch_start_x, touch_start_y);
            } else {
                // Touch is active, check for gesture classification
                int16_t delta_y = (int16_t)y[0] - (int16_t)touch_start_y;
                uint16_t abs_delta_y = (delta_y < 0) ? -delta_y : delta_y;
                
                // Transition to brightness control if vertical distance exceeds threshold
                if (gesture_state == GESTURE_STATE_TAP && abs_delta_y >= min_swipe_height) {
                    gesture_state = GESTURE_STATE_BRIGHTNESS;
                    brightness_start = app_lcd_get_brightness();
                    touch_start_y = y[0]; // reset baseline to current finger position
                    delta_y = 0;
                    ESP_LOGD(TAG, "brightness gesture started @(%u,%u)", touch_start_x, touch_start_y);
                }

                if (gesture_state == GESTURE_STATE_BRIGHTNESS) {
                    // Recompute delta against brightness baseline
                    delta_y = (int16_t)y[0] - (int16_t)touch_start_y;

                    // Calculate brightness change based on vertical distance
                    // Formula: brightness_delta = (-delta_y * max_brightness_delta) / screen_height
                    // - Full screen height (screen_height pixels) = max_brightness_delta percent change
                    // - delta_y is positive when swiping down (y increases), negative when swiping up (y decreases)
                    // - We negate delta_y so swipe up (negative delta_y) increases brightness
                    // - Result is proportional: half screen swipe = half max delta
                    int brightness_delta = (-delta_y * max_brightness_delta) / screen_height;
                    int target_brightness = brightness_start + brightness_delta;
                    
                    // Clamp to valid range
                    if (target_brightness < 0) {
                        target_brightness = 0;
                    } else if (target_brightness > 100) {
                        target_brightness = 100;
                    }
                    
                    // Update brightness if it changed
                    int current_brightness = app_lcd_get_brightness();
                    if (target_brightness != current_brightness) {
                        app_lcd_set_brightness(target_brightness);
                        ESP_LOGD(TAG, "brightness: %d%% (delta_y=%d)", target_brightness, delta_y);
                    }
                }
            }

            if (!last_touch_valid) {
                pico8_touch_report_t report = {
                    .report_id = 1,
                    .flags = 0x01,
                    .x = scaled_x,
                    .y = scaled_y,
                    .pressure = (uint8_t)MIN(strength[0], 255),
                    .reserved = 0,
                };
                app_usb_report_touch(&report);
                last_touch_valid = true;
                last_scaled_x = scaled_x;
                last_scaled_y = scaled_y;
            } else if (coords_changed) {
                pico8_touch_report_t report = {
                    .report_id = 1,
                    .flags = 0x02,
                    .x = scaled_x,
                    .y = scaled_y,
                    .pressure = (uint8_t)MIN(strength[0], 255),
                    .reserved = 0,
                };
                app_usb_report_touch(&report);
                last_scaled_x = scaled_x;
                last_scaled_y = scaled_y;
            }
        } else {
            // Touch released
            if (gesture_state != GESTURE_STATE_IDLE) {
                if (gesture_state == GESTURE_STATE_TAP) {
                    // It was a tap, perform swap gesture
                    const uint16_t screen_midpoint = BSP_LCD_H_RES / 2;
                    if (touch_start_x < screen_midpoint) {
                        // Left half: cycle backward
                        app_lcd_cycle_animation_backward();
                    } else {
                        // Right half: cycle forward
                        app_lcd_cycle_animation();
                    }
                    ESP_LOGD(TAG, "tap gesture: swap animation");
                } else {
                    // It was a brightness gesture, already handled
                    ESP_LOGD(TAG, "brightness gesture ended");
                }
                gesture_state = GESTURE_STATE_IDLE;
            }
            if (last_touch_valid) {
                pico8_touch_report_t report = {
                    .report_id = 1,
                    .flags = 0x04,
                    .x = last_scaled_x,
                    .y = last_scaled_y,
                    .pressure = 0,
                    .reserved = 0,
                };
                app_usb_report_touch(&report);
                last_touch_valid = false;
            }
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
