// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file p3a_touch_router.c
 * @brief State-aware touch event routing implementation
 */

#include "p3a_touch_router.h"
#include "p3a_state.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>

// Forward declaration for gBool type (from µGFX)
typedef int gBool;

static const char *TAG = "p3a_touch_router";

// ============================================================================
// External function declarations (to be called by handlers)
// These are implemented in other modules
// ============================================================================

// Animation playback handlers (from app_lcd.c via weak symbols)
extern void app_lcd_cycle_animation(void) __attribute__((weak));
extern void app_lcd_cycle_animation_backward(void) __attribute__((weak));
extern esp_err_t app_lcd_set_brightness(int brightness_percent) __attribute__((weak));
extern int app_lcd_get_brightness(void) __attribute__((weak));
extern esp_err_t app_set_screen_rotation(int rotation) __attribute__((weak));
extern int app_get_screen_rotation(void) __attribute__((weak));

// Provisioning handlers (from makapix.c via weak symbols)
extern esp_err_t makapix_start_provisioning(void) __attribute__((weak));
extern void makapix_cancel_provisioning(void) __attribute__((weak));

// PICO-8 handlers (from playback_controller.c via weak symbols)
extern void playback_controller_exit_pico8_mode(void) __attribute__((weak));

// WiFi/AP handlers (from app_wifi.c via weak symbols)
extern bool app_wifi_is_captive_portal_active(void) __attribute__((weak));

// USB touch forwarding (from app_usb.c via weak symbols)
typedef struct {
    uint8_t report_id;
    uint8_t flags;
    uint16_t x;
    uint16_t y;
    uint8_t pressure;
    uint8_t reserved;
} pico8_touch_report_t;
extern void app_usb_report_touch(const pico8_touch_report_t *report) __attribute__((weak));

// ============================================================================
// Internal state
// ============================================================================

static bool s_initialized = false;

// ============================================================================
// State-specific handlers
// ============================================================================

/**
 * @brief Handle touch events in ANIMATION_PLAYBACK state
 */
static esp_err_t handle_animation_playback(const p3a_touch_event_t *event)
{
    switch (event->type) {
        case P3A_TOUCH_EVENT_TAP_LEFT:
            if (app_lcd_cycle_animation_backward) {
                app_lcd_cycle_animation_backward();
            }
            return ESP_OK;
            
        case P3A_TOUCH_EVENT_TAP_RIGHT:
            if (app_lcd_cycle_animation) {
                app_lcd_cycle_animation();
            }
            return ESP_OK;
            
        case P3A_TOUCH_EVENT_BRIGHTNESS: {
            if (app_lcd_get_brightness && app_lcd_set_brightness) {
                int current = app_lcd_get_brightness();
                int target = current + event->brightness.delta_percent;
                if (target < 0) target = 0;
                if (target > 100) target = 100;
                app_lcd_set_brightness(target);
            }
            return ESP_OK;
        }
            
        case P3A_TOUCH_EVENT_LONG_PRESS:
            // Check if captive portal is active - toggle AP info display
            if (app_wifi_is_captive_portal_active && app_wifi_is_captive_portal_active()) {
                // Toggle AP info screen
                extern gBool ugfx_ui_is_active(void) __attribute__((weak));
                extern void ugfx_ui_hide_registration(void) __attribute__((weak));
                extern esp_err_t ugfx_ui_show_captive_ap_info(void) __attribute__((weak));
                extern esp_err_t app_lcd_enter_ui_mode(void) __attribute__((weak));
                extern esp_err_t app_lcd_exit_ui_mode(void) __attribute__((weak));
                
                if (ugfx_ui_is_active && ugfx_ui_is_active()) {
                    // AP info screen is showing - hide it and exit UI mode
                    ESP_LOGI(TAG, "Long press with AP info showing - hiding");
                    if (ugfx_ui_hide_registration) ugfx_ui_hide_registration();
                    if (app_lcd_exit_ui_mode) app_lcd_exit_ui_mode();
                } else {
                    // Show AP info screen
                    ESP_LOGI(TAG, "Long press in captive portal mode - showing AP info");
                    if (app_lcd_enter_ui_mode) app_lcd_enter_ui_mode();
                    if (ugfx_ui_show_captive_ap_info) ugfx_ui_show_captive_ap_info();
                }
            } else {
                // Start provisioning - but only if we have internet
                ESP_LOGI(TAG, "Long press detected - attempting to start provisioning");
                
                // Check for internet connectivity before provisioning
                if (!p3a_state_has_internet()) {
                    ESP_LOGW(TAG, "Cannot start provisioning - no internet connectivity");
                    // Show error message to user
                    extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                               int progress_percent, const char *detail) __attribute__((weak));
                    if (p3a_render_set_channel_message) {
                        const char *conn_detail = p3a_state_get_connectivity_detail();
                        p3a_render_set_channel_message("Provisioning Unavailable", 6 /* P3A_CHANNEL_MSG_ERROR */, -1, 
                                                       conn_detail ? conn_detail : "No internet connection");
                    }
                    return ESP_ERR_NOT_FINISHED;
                }
                
                if (!makapix_start_provisioning) {
                    ESP_LOGW(TAG, "Provisioning not available (makapix_start_provisioning is NULL)");
                } else {
                    esp_err_t err = p3a_state_enter_provisioning();
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "State transition to provisioning successful, starting provisioning");
                        makapix_start_provisioning();
                    } else {
                        ESP_LOGW(TAG, "State transition to provisioning denied: %s (current state: %s)",
                                esp_err_to_name(err), p3a_state_get_name(p3a_state_get()));
                        // Force start provisioning anyway if we're in animation playback state
                        // This handles the edge case where substate might be blocking
                        if (p3a_state_get() == P3A_STATE_ANIMATION_PLAYBACK) {
                            ESP_LOGI(TAG, "Forcing provisioning start from animation playback state");
                            makapix_start_provisioning();
                        }
                    }
                }
            }
            return ESP_OK;
            
        case P3A_TOUCH_EVENT_ROTATION_CW:
            if (app_get_screen_rotation && app_set_screen_rotation) {
                int current = app_get_screen_rotation();
                int next = (current + 90) % 360;
                app_set_screen_rotation(next);
            }
            return ESP_OK;
            
        case P3A_TOUCH_EVENT_ROTATION_CCW:
            if (app_get_screen_rotation && app_set_screen_rotation) {
                int current = app_get_screen_rotation();
                int next = (current + 270) % 360;
                app_set_screen_rotation(next);
            }
            return ESP_OK;
            
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

// UI mode handlers for synchronous cleanup (from app_lcd.c via weak symbols)
extern esp_err_t app_lcd_exit_ui_mode(void) __attribute__((weak));
extern void ugfx_ui_hide_registration(void) __attribute__((weak));

/**
 * @brief Handle touch events in PROVISIONING state
 */
static esp_err_t handle_provisioning(const p3a_touch_event_t *event)
{
    switch (event->type) {
        case P3A_TOUCH_EVENT_LONG_PRESS:
            // Cancel provisioning and return to playback
            ESP_LOGI(TAG, "Long press during provisioning - cancelling");

            // Step 1: Cancel provisioning (sets makapix state to IDLE)
            if (makapix_cancel_provisioning) {
                makapix_cancel_provisioning();
            }

            // Step 2: Exit UI mode SYNCHRONOUSLY before state transition
            // This ensures no black frame between UI hide and animation resume
            if (app_lcd_exit_ui_mode) {
                app_lcd_exit_ui_mode();
            }
            if (ugfx_ui_hide_registration) {
                ugfx_ui_hide_registration();
            }

            // Step 3: Transition state
            p3a_state_exit_to_playback();

            return ESP_OK;

        default:
            // All other gestures ignored during provisioning
            return ESP_ERR_NOT_SUPPORTED;
    }
}

/**
 * @brief Handle touch events in OTA state
 */
static esp_err_t handle_ota(const p3a_touch_event_t *event)
{
    (void)event;
    // All gestures ignored during OTA - can't interrupt firmware update
    ESP_LOGD(TAG, "Touch ignored during OTA");
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Handle touch events in PICO8_STREAMING state
 */
static esp_err_t handle_pico8_streaming(const p3a_touch_event_t *event)
{
    switch (event->type) {
        case P3A_TOUCH_EVENT_LONG_PRESS:
            // Exit PICO-8 mode
            ESP_LOGI(TAG, "Long press during PICO-8 - exiting streaming mode");
            if (playback_controller_exit_pico8_mode) {
                playback_controller_exit_pico8_mode();
            }
            p3a_state_exit_to_playback();
            return ESP_OK;
            
        case P3A_TOUCH_EVENT_RAW:
            // Forward raw touch to USB HID
            if (app_usb_report_touch) {
                pico8_touch_report_t report = {
                    .report_id = 1,
                    .flags = event->raw.pressed ? 0x01 : 0x04,  // Down/Up
                    .x = event->raw.x,
                    .y = event->raw.y,
                    .pressure = event->raw.pressure,
                    .reserved = 0
                };
                app_usb_report_touch(&report);
            }
            return ESP_OK;
            
        default:
            // Brightness and rotation are disabled during PICO-8
            return ESP_ERR_NOT_SUPPORTED;
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t p3a_touch_router_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    
    s_initialized = true;
    ESP_LOGI(TAG, "Touch router initialized");
    return ESP_OK;
}

esp_err_t p3a_touch_router_handle_event(const p3a_touch_event_t *event)
{
    if (!event) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    p3a_state_t current_state = p3a_state_get();
    
    switch (current_state) {
        case P3A_STATE_ANIMATION_PLAYBACK:
            return handle_animation_playback(event);
            
        case P3A_STATE_PROVISIONING:
            return handle_provisioning(event);
            
        case P3A_STATE_OTA:
            return handle_ota(event);
            
        case P3A_STATE_PICO8_STREAMING:
            return handle_pico8_streaming(event);
            
        default:
            ESP_LOGW(TAG, "Unknown state %d", current_state);
            return ESP_ERR_INVALID_STATE;
    }
}

bool p3a_touch_router_is_gesture_enabled(p3a_touch_event_type_t event_type)
{
    if (!s_initialized) return false;
    
    p3a_state_t current_state = p3a_state_get();
    
    switch (current_state) {
        case P3A_STATE_ANIMATION_PLAYBACK:
            // All gestures enabled during animation playback
            return true;
            
        case P3A_STATE_PROVISIONING:
            // Only long press enabled during provisioning
            return (event_type == P3A_TOUCH_EVENT_LONG_PRESS);
            
        case P3A_STATE_OTA:
            // No gestures during OTA
            return false;
            
        case P3A_STATE_PICO8_STREAMING:
            // Long press and raw touch enabled
            return (event_type == P3A_TOUCH_EVENT_LONG_PRESS ||
                    event_type == P3A_TOUCH_EVENT_RAW);
            
        default:
            return false;
    }
}

