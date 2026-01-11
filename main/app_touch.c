/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_touch.h"
#include "p3a_board.h"
#include "app_lcd.h"
#include "app_usb.h"
#include "app_touch.h"
#include "app_wifi.h"
#include "sdkconfig.h"
#include "makapix.h"
#include "animation_player.h"
#include "ugfx_ui.h"
#include "p3a_state.h"
#include "p3a_touch_router.h"
#include <math.h>

// Debug provisioning mode - when enabled, long press doesn't trigger real provisioning
#define DEBUG_PROVISIONING_ENABLED 0
#define MATH_PI 3.14159265f

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "app_touch";


static esp_lcd_touch_handle_t tp = NULL;

/**
 * @brief Gesture state machine states
 * 
 * The touch handler distinguishes between tap gestures (for animation swapping),
 * swipe gestures (for brightness control), and two-finger rotation gestures
 * (for screen rotation) based on touch count and movement patterns.
 */
typedef enum {
    GESTURE_STATE_IDLE,              // No active touch
    GESTURE_STATE_TAP,              // Potential tap/swap gesture (minimal movement)
    GESTURE_STATE_BRIGHTNESS,       // Brightness control gesture (vertical swipe detected)
    GESTURE_STATE_LONG_PRESS_PENDING, // Finger down, counting to 4s for provisioning
    GESTURE_STATE_ROTATION          // Two-finger rotation gesture
} gesture_state_t;

// Rotation gesture configuration
#define ROTATION_ANGLE_THRESHOLD_DEG  45.0f  // Degrees needed to trigger rotation
#define ROTATION_ANGLE_THRESHOLD_RAD  (ROTATION_ANGLE_THRESHOLD_DEG * MATH_PI / 180.0f)

/**
 * @brief Calculate angle between two touch points
 * @return Angle in radians (-π to π)
 */
static float calculate_two_finger_angle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    float dx = (float)x2 - (float)x1;
    float dy = (float)y2 - (float)y1;
    return atan2f(dy, dx);
}

/**
 * @brief Normalize angle difference to range (-π, π)
 */
static float normalize_angle(float angle)
{
    while (angle > MATH_PI) angle -= 2.0f * MATH_PI;
    while (angle < -MATH_PI) angle += 2.0f * MATH_PI;
    return angle;
}

/**
 * @brief Get next rotation value (clockwise: 0→90→180→270→0)
 */
static screen_rotation_t get_next_rotation_cw(screen_rotation_t current)
{
    switch (current) {
        case ROTATION_0:   return ROTATION_90;
        case ROTATION_90:  return ROTATION_180;
        case ROTATION_180: return ROTATION_270;
        case ROTATION_270: return ROTATION_0;
        default:           return ROTATION_0;
    }
}

/**
 * @brief Get next rotation value (counter-clockwise: 0→270→180→90→0)
 */
static screen_rotation_t get_next_rotation_ccw(screen_rotation_t current)
{
    switch (current) {
        case ROTATION_0:   return ROTATION_270;
        case ROTATION_90:  return ROTATION_0;
        case ROTATION_180: return ROTATION_90;
        case ROTATION_270: return ROTATION_180;
        default:           return ROTATION_0;
    }
}

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
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
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
#endif

/**
 * @brief Transform touch coordinates based on current screen rotation
 * 
 * @param x Pointer to X coordinate (modified in place)
 * @param y Pointer to Y coordinate (modified in place)
 * @param rotation Current screen rotation
 */
static void transform_touch_coordinates(uint16_t *x, uint16_t *y, screen_rotation_t rotation)
{
    const uint16_t screen_w = P3A_DISPLAY_WIDTH;
    const uint16_t screen_h = P3A_DISPLAY_HEIGHT;
    uint16_t temp;
    
    switch (rotation) {
        case ROTATION_0:
            // No transformation
            break;
            
        case ROTATION_90:  // 90° CW
            // (x, y) → (y, height - 1 - x)
            temp = *x;
            *x = *y;
            *y = screen_h - 1 - temp;
            break;
            
        case ROTATION_180:
            // (x, y) → (width - 1 - x, height - 1 - y)
            *x = screen_w - 1 - *x;
            *y = screen_h - 1 - *y;
            break;
            
        case ROTATION_270:  // 270° CW (90° CCW)
            // (x, y) → (width - 1 - y, x)
            temp = *x;
            *x = screen_w - 1 - *y;
            *y = temp;
            break;
    }
}

static void app_touch_task(void *arg)
{
    const TickType_t poll_delay = pdMS_TO_TICKS(CONFIG_P3A_TOUCH_POLL_INTERVAL_MS);
    uint16_t x[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint16_t y[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint16_t strength[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t touch_count = 0;
    
    gesture_state_t gesture_state = GESTURE_STATE_IDLE;
    uint16_t touch_start_x = 0;      // Raw coordinate for tap position detection
    uint16_t touch_start_y = 0;      // Raw coordinate for tap position detection
    uint16_t brightness_start_y = 0; // Visual Y coordinate for brightness baseline
    TickType_t touch_start_time = 0;
    int brightness_start = 100;  // Brightness at gesture start
    const uint16_t screen_height = P3A_DISPLAY_HEIGHT;
    const uint16_t screen_width = P3A_DISPLAY_WIDTH;
    const uint16_t min_swipe_height = (screen_height * CONFIG_P3A_TOUCH_SWIPE_MIN_HEIGHT_PERCENT) / 100;
    const int max_brightness_delta = CONFIG_P3A_TOUCH_BRIGHTNESS_MAX_DELTA_PERCENT;
    const TickType_t long_press_duration = pdMS_TO_TICKS(4000); // 4 seconds
    const uint32_t long_press_movement_threshold = ( (uint32_t)MIN(screen_height, screen_width) * 65U + 500U ) / 1000U; // 6.5% of smallest dimension, rounded
    
    // Two-finger rotation gesture state
    float rotation_start_angle = 0.0f;      // Initial angle between two fingers
    float rotation_cumulative = 0.0f;       // Cumulative rotation since gesture start
    float rotation_last_angle = 0.0f;       // Last measured angle (for delta calculation)
    bool rotation_triggered = false;        // Whether rotation was already triggered this gesture
    uint8_t prev_touch_count = 0;           // Previous frame's touch count

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    bool last_touch_valid = false;
    uint16_t last_scaled_x = 0;
    uint16_t last_scaled_y = 0;
#endif

    while (true) {
        esp_lcd_touch_read_data(tp);
        bool pressed = esp_lcd_touch_get_coordinates(tp, x, y, strength, &touch_count,
                                                     CONFIG_ESP_LCD_TOUCH_MAX_POINTS);

        // NOTE: We use RAW (untransformed) coordinates for gesture detection
        // (swipe direction, brightness control) because gestures should work
        // in physical screen space. Only transform for position-based actions
        // (tap location for left/right half detection).

        // Two-finger rotation gesture detection (use raw coordinates)
        if (pressed && touch_count >= 2) {
            float current_angle = calculate_two_finger_angle(x[0], y[0], x[1], y[1]);
            
            if (prev_touch_count < 2) {
                // Just transitioned to 2 fingers - start rotation tracking
                rotation_start_angle = current_angle;
                rotation_last_angle = current_angle;
                rotation_cumulative = 0.0f;
                rotation_triggered = false;
                gesture_state = GESTURE_STATE_ROTATION;
                ESP_LOGD(TAG, "rotation gesture started, initial angle=%.2f deg", 
                         rotation_start_angle * 180.0f / MATH_PI);
            } else if (gesture_state == GESTURE_STATE_ROTATION) {
                // Continue tracking rotation
                float angle_delta = normalize_angle(current_angle - rotation_last_angle);
                rotation_cumulative += angle_delta;
                rotation_last_angle = current_angle;
                
                // Check if rotation threshold exceeded
                if (!rotation_triggered && fabsf(rotation_cumulative) >= ROTATION_ANGLE_THRESHOLD_RAD) {
                    // Determine rotation type
                    p3a_touch_event_type_t rot_type = (rotation_cumulative > 0) ?
                        P3A_TOUCH_EVENT_ROTATION_CW : P3A_TOUCH_EVENT_ROTATION_CCW;

                    // Check if rotation gestures are enabled in current state
                    if (!p3a_touch_router_is_gesture_enabled(rot_type)) {
                        ESP_LOGD(TAG, "rotation gesture ignored - not enabled in current state");
                        rotation_triggered = true;  // Prevent repeated log spam
                    } else {
                        p3a_touch_event_t touch_event = { .type = rot_type };

                        ESP_LOGI(TAG, "rotation gesture: %s, cumulative=%.2f deg",
                                 (rotation_cumulative > 0) ? "CW" : "CCW",
                                     rotation_cumulative * 180.0f / MATH_PI);

                        if (p3a_touch_router_handle_event(&touch_event) == ESP_OK) {
                            rotation_triggered = true;
                        }
                    }
                }
            }
            prev_touch_count = touch_count;
            vTaskDelay(poll_delay);
            continue;  // Skip single-finger processing when 2 fingers are down
        }
        
        // Reset rotation state when fewer than 2 fingers
        if (prev_touch_count >= 2 && touch_count < 2) {
            if (gesture_state == GESTURE_STATE_ROTATION) {
                ESP_LOGD(TAG, "rotation gesture ended");
                gesture_state = GESTURE_STATE_IDLE;
            }
        }
        prev_touch_count = touch_count;

        if (pressed && touch_count > 0) {
            // Transform coordinates to visual space for brightness gesture detection
            // (rotation gesture uses raw coordinates, brightness uses visual coordinates)
            uint16_t visual_x = x[0];
            uint16_t visual_y = y[0];
            screen_rotation_t rotation = app_get_screen_rotation();
            transform_touch_coordinates(&visual_x, &visual_y, rotation);
            
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
            uint16_t scaled_x = scale_to_pico8(x[0], P3A_DISPLAY_WIDTH, 127);
            uint16_t scaled_y = scale_to_pico8(y[0], P3A_DISPLAY_HEIGHT, 127);
            bool coords_changed = (!last_touch_valid) || (scaled_x != last_scaled_x) || (scaled_y != last_scaled_y);
#endif

            if (gesture_state == GESTURE_STATE_IDLE) {
                // Touch just started - store raw coordinates (for tap position detection)
                touch_start_x = x[0];
                touch_start_y = y[0];
                touch_start_time = xTaskGetTickCount();
                brightness_start = app_lcd_get_brightness();
                gesture_state = GESTURE_STATE_TAP;
                ESP_LOGD(TAG, "touch start @(%u,%u)", touch_start_x, touch_start_y);
            } else {
                // Touch is active, check for gesture classification
                // Transform start coordinates to visual space for brightness gesture detection
                uint16_t visual_start_x = touch_start_x;
                uint16_t visual_start_y = touch_start_y;
                transform_touch_coordinates(&visual_start_x, &visual_start_y, rotation);
                
                // Calculate deltas in visual space for brightness gesture
                int16_t delta_x = (int16_t)visual_x - (int16_t)visual_start_x;
                int16_t delta_y = (int16_t)visual_y - (int16_t)visual_start_y;
                uint16_t abs_delta_x = (delta_x < 0) ? -delta_x : delta_x;
                uint16_t abs_delta_y = (delta_y < 0) ? -delta_y : delta_y;
                
                // Check for long press: finger held at same position for 4 seconds
                TickType_t elapsed = xTaskGetTickCount() - touch_start_time;
                uint16_t total_movement = abs_delta_x + abs_delta_y;
                
                if (gesture_state == GESTURE_STATE_TAP || gesture_state == GESTURE_STATE_LONG_PRESS_PENDING) {
                    if (total_movement <= long_press_movement_threshold) {
                        // Finger hasn't moved much, check for long press
                        if (elapsed >= long_press_duration) {
                            if (gesture_state != GESTURE_STATE_LONG_PRESS_PENDING) {
                                gesture_state = GESTURE_STATE_LONG_PRESS_PENDING;
                                
                                // Route long press through state-aware touch router
                                p3a_touch_event_t touch_event = {
                                    .type = P3A_TOUCH_EVENT_LONG_PRESS
                                };
                                p3a_touch_router_handle_event(&touch_event);
                                ESP_LOGI(TAG, "Long press detected, routed to state handler");
                            }
                        } else {
                            // Still counting, but not long enough yet
                            gesture_state = GESTURE_STATE_TAP;
                        }
                    } else {
                        // Finger moved, reset long press detection (don't cancel provisioning)
                        // Cancellation only happens on completed 4-second press
                        gesture_state = GESTURE_STATE_TAP;
                    }
                }
                
                // Transition to brightness control if vertical distance exceeds threshold
                // Use visual coordinates so brightness gesture rotates with screen
                if (gesture_state == GESTURE_STATE_TAP && abs_delta_y >= min_swipe_height) {
                    gesture_state = GESTURE_STATE_BRIGHTNESS;
                    brightness_start = app_lcd_get_brightness();
                    // Store visual Y as baseline for brightness calculation
                    brightness_start_y = visual_y;
                    delta_y = 0;
                    ESP_LOGD(TAG, "brightness gesture started @(%u,%u) visual", visual_x, visual_y);
                }

                if (gesture_state == GESTURE_STATE_BRIGHTNESS) {
                    // Check if brightness gestures are enabled in current state
                    if (!p3a_touch_router_is_gesture_enabled(P3A_TOUCH_EVENT_BRIGHTNESS)) {
                        // Reset to tap state if brightness not allowed
                        gesture_state = GESTURE_STATE_TAP;
                    } else {
                    // Recompute delta against brightness baseline using visual coordinates
                    delta_y = (int16_t)visual_y - (int16_t)brightness_start_y;

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
            }

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
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
#endif
        } else {
            // Touch released
            if (gesture_state != GESTURE_STATE_IDLE) {
                if (gesture_state == GESTURE_STATE_LONG_PRESS_PENDING) {
                    // Long press was triggered, provisioning is in progress
                    ESP_LOGD(TAG, "Long press gesture ended (provisioning in progress)");
                } else if (gesture_state == GESTURE_STATE_TAP) {
                    // It was a tap, route through state-aware handler
                    // Transform tap position to determine left/right in user's visual space
                    uint16_t tap_x = touch_start_x;
                    uint16_t tap_y = touch_start_y;
                    transform_touch_coordinates(&tap_x, &tap_y, app_get_screen_rotation());

                    const uint16_t screen_midpoint = P3A_DISPLAY_WIDTH / 2;
                    p3a_touch_event_type_t tap_type = (tap_x < screen_midpoint) ?
                        P3A_TOUCH_EVENT_TAP_LEFT : P3A_TOUCH_EVENT_TAP_RIGHT;

                    // Check if tap gestures are enabled in current state
                    if (!p3a_touch_router_is_gesture_enabled(tap_type)) {
                        ESP_LOGD(TAG, "tap gesture ignored - not enabled in current state");
                    } else {
                        p3a_touch_event_t touch_event = { .type = tap_type };
                        p3a_touch_router_handle_event(&touch_event);
                        ESP_LOGD(TAG, "tap gesture: routed to state handler (tap_x=%u)", tap_x);
                    }
                } else if (gesture_state == GESTURE_STATE_ROTATION) {
                    // Rotation gesture ended (action already taken if threshold was reached)
                    ESP_LOGD(TAG, "rotation gesture ended");
                } else {
                    // It was a brightness gesture, already handled
                    ESP_LOGD(TAG, "brightness gesture ended");
                }
                gesture_state = GESTURE_STATE_IDLE;
            }
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
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
#endif
        }

        vTaskDelay(poll_delay);
    }
}

esp_err_t app_touch_init(void)
{
#if P3A_HAS_TOUCH
    esp_err_t err = p3a_board_touch_init(&tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Initialize touch router (for state-aware gesture routing)
    err = p3a_touch_router_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch router init failed: %s (continuing anyway)", esp_err_to_name(err));
    }

    // NOTE: Touch task stack must be large enough for gesture routing + logging.
    // Some tap paths can indirectly trigger deeper call chains (e.g. Live Mode / channel rebuild),
    // and 4KB has proven insufficient (stack overflow faults).
    const BaseType_t created = xTaskCreate(app_touch_task, "app_touch_task", 8192, NULL,
                                           CONFIG_P3A_TOUCH_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "touch task creation failed");
        return ESP_FAIL;
    }

    return ESP_OK;
#else
    ESP_LOGW(TAG, "Touch not available on this board");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
