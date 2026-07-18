// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_touch_router.c
 * @brief State-aware touch event routing implementation
 */

#include "p3a_touch_router.h"
#include "p3a_state.h"
#include "p3a_current_post.h"
#include "p3a_reaction_dispatcher.h"
#include "p3a_pin_dispatcher.h"
#include "event_bus.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

// Processing notification (from display_renderer_priv.h via weak symbol)
extern void proc_notif_start(void) __attribute__((weak));

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
// Note: provisioning is started from the Settings web UI, not from any
// touch gesture; only the cancel path is reachable from the router.
extern void makapix_cancel_provisioning(void) __attribute__((weak));

// PICO-8 handlers (from playback_controller.c via weak symbols)
extern void playback_controller_exit_pico8_mode(void) __attribute__((weak));

// UI mode handlers (from app_lcd.c / ugfx_ui via weak symbols)
extern esp_err_t app_lcd_enter_ui_mode(void) __attribute__((weak));
extern esp_err_t app_lcd_exit_ui_mode(void) __attribute__((weak));
extern bool app_lcd_is_ui_mode(void) __attribute__((weak));
extern gBool ugfx_ui_is_active(void) __attribute__((weak));
extern void ugfx_ui_hide_registration(void) __attribute__((weak));
extern esp_err_t ugfx_ui_show_captive_ap_info(void) __attribute__((weak));
extern esp_err_t ugfx_ui_show_connectivity_error(void) __attribute__((weak));
extern esp_err_t ugfx_ui_show_info_screen(void) __attribute__((weak));
extern void ugfx_ui_hide_info_screen(void) __attribute__((weak));
extern void ugfx_ui_hide_captive_ap_info(void) __attribute__((weak));
extern bool app_wifi_is_captive_portal_active(void) __attribute__((weak));

// SD-format flow handlers (from sd_format.c / ugfx_ui.c via weak symbols).
// While the format flow is active it consumes all UI-mode touch input;
// otherwise UI-mode taps are offered to the info screen's format button.
extern bool sd_format_is_active(void) __attribute__((weak));
extern bool sd_format_handle_tap(uint16_t x, uint16_t y) __attribute__((weak));
extern void sd_format_handle_long_press(void) __attribute__((weak));
extern bool ugfx_ui_info_screen_handle_tap(uint16_t x, uint16_t y) __attribute__((weak));

// Playback service (from playback_service.c via weak symbol)
extern bool playback_service_is_paused(void) __attribute__((weak));

// Reaction overlay error icon (used for non-Makapix / no-post fallback paths
// where the dispatcher is not invoked at all). Submit/revoke/click overlays
// are shown by the dispatcher itself.
extern void reaction_overlay_show_error(void) __attribute__((weak));

// POST_SOURCE_{MAKAPIX,GIPHY} (from play_scheduler_types.h). Kept as macros here
// so p3a_core does not take a dependency on play_scheduler.
#define REACTION_POST_SOURCE_MAKAPIX     1
#define REACTION_POST_SOURCE_GIPHY       2
#define REACTION_POST_SOURCE_SDCARD      3
#define REACTION_POST_SOURCE_INSTITUTION 4
#define REACTION_POST_SOURCE_KLIPY       5

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
    // While paused, only long-press (to unpause) is allowed
    if (playback_service_is_paused && playback_service_is_paused()) {
        if (event->type == P3A_TOUCH_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "Long press while paused - resuming playback");
            event_bus_emit_simple(P3A_EVENT_RESUME);
            return ESP_OK;
        }
        return ESP_OK;  // Silently ignore all other gestures
    }

    // While in UI mode, only long-press (to dismiss) is allowed — except for
    // the SD-format flow, which owns all input while active, and the info
    // screen's "Format SD card" button, which consumes its own taps.
    if (app_lcd_is_ui_mode && app_lcd_is_ui_mode()) {
        if (sd_format_is_active && sd_format_is_active()) {
            if (event->type == P3A_TOUCH_EVENT_TAP_LEFT ||
                event->type == P3A_TOUCH_EVENT_TAP_RIGHT) {
                if (sd_format_handle_tap) {
                    sd_format_handle_tap(event->tap.x, event->tap.y);
                }
            } else if (event->type == P3A_TOUCH_EVENT_LONG_PRESS) {
                // Cancels the warning panel; deliberately a no-op during the
                // format itself — never falls through to overlay-dismiss.
                if (sd_format_handle_long_press) {
                    sd_format_handle_long_press();
                }
            }
            return ESP_OK;
        }
        if ((event->type == P3A_TOUCH_EVENT_TAP_LEFT ||
             event->type == P3A_TOUCH_EVENT_TAP_RIGHT) &&
            ugfx_ui_info_screen_handle_tap &&
            ugfx_ui_info_screen_handle_tap(event->tap.x, event->tap.y)) {
            return ESP_OK;
        }
        if (event->type != P3A_TOUCH_EVENT_LONG_PRESS) {
            return ESP_OK;  // Silently ignore taps, brightness, rotation
        }
    }

    switch (event->type) {
        case P3A_TOUCH_EVENT_TAP_LEFT:
            if (proc_notif_start) {
                proc_notif_start();
            }
            if (app_lcd_cycle_animation_backward) {
                app_lcd_cycle_animation_backward();
            }
            return ESP_OK;
            
        case P3A_TOUCH_EVENT_TAP_RIGHT:
            if (proc_notif_start) {
                proc_notif_start();
            }
            if (app_lcd_cycle_animation) {
                app_lcd_cycle_animation();
            }
            return ESP_OK;
            
        case P3A_TOUCH_EVENT_SWIPE_UP: {
            // Branch by current post source. Sources without reaction support
            // (or with a missing post identifier) get only the error overlay
            // so the user always sees that the gesture was recognized; the
            // dispatcher handles overlays and state updates for the rest.
            int source = p3a_current_post_get_source();

            if (source == REACTION_POST_SOURCE_GIPHY) {
                char giphy_id[24];
                p3a_current_post_get_giphy_id(giphy_id, sizeof(giphy_id));
                if (giphy_id[0] == '\0') {
                    ESP_LOGI(TAG, "Swipe up on Giphy post with no giphy_id - showing error");
                    if (reaction_overlay_show_error) reaction_overlay_show_error();
                    return ESP_OK;
                }
                p3a_reaction_dispatch_giphy_click(giphy_id);
                /* Pin runs alongside the click registration. */
                p3a_pin_dispatch_from_current(NULL);
                return ESP_OK;
            }

            if (source == REACTION_POST_SOURCE_INSTITUTION) {
                /* Museum artworks have no reaction/click counterpart; swipe-up
                   just pins. The pin dispatcher handles its own overlays. */
                p3a_pin_dispatch_from_current(NULL);
                return ESP_OK;
            }

            if (source == REACTION_POST_SOURCE_KLIPY) {
                /* Klipy artworks have no reaction/click counterpart; swipe-up
                   just pins (same as museum). */
                p3a_pin_dispatch_from_current(NULL);
                return ESP_OK;
            }

            if (source != REACTION_POST_SOURCE_MAKAPIX) {
                ESP_LOGI(TAG, "Swipe up on non-Makapix post - showing error");
                if (reaction_overlay_show_error) reaction_overlay_show_error();
                return ESP_OK;
            }
            int32_t post_id = p3a_current_post_get_id();
            if (post_id <= 0) {
                ESP_LOGI(TAG, "Swipe up with no valid post_id - showing error");
                if (reaction_overlay_show_error) reaction_overlay_show_error();
                return ESP_OK;
            }
            p3a_reaction_dispatch_makapix_submit(post_id);
            /* Pin is decoupled from the reaction — fire-and-forget; failure
               surfaces via the top-left pin overlay only. */
            p3a_pin_dispatch_from_current(NULL);
            return ESP_OK;
        }

        case P3A_TOUCH_EVENT_SWIPE_DOWN: {
            int source = p3a_current_post_get_source();

            if (source == REACTION_POST_SOURCE_GIPHY ||
                source == REACTION_POST_SOURCE_INSTITUTION ||
                source == REACTION_POST_SOURCE_KLIPY) {
                /* No reaction-revoke for these sources — swipe-down is unpin-only. */
                p3a_pin_dispatch_unpin_from_current(NULL);
                return ESP_OK;
            }

            if (source != REACTION_POST_SOURCE_MAKAPIX) {
                ESP_LOGI(TAG, "Swipe down on non-Makapix post - showing error");
                if (reaction_overlay_show_error) reaction_overlay_show_error();
                return ESP_OK;
            }
            int32_t post_id = p3a_current_post_get_id();
            if (post_id <= 0) {
                ESP_LOGI(TAG, "Swipe down with no valid post_id - showing error");
                if (reaction_overlay_show_error) reaction_overlay_show_error();
                return ESP_OK;
            }
            p3a_reaction_dispatch_makapix_revoke(post_id);
            /* Unpin runs in parallel with reaction-revoke; either may
               independently no-op (e.g., not pinned in the active list). */
            p3a_pin_dispatch_unpin_from_current(NULL);
            return ESP_OK;
        }

        case P3A_TOUCH_EVENT_BRIGHTNESS:
            // Brightness is now controlled via web UI only
            return ESP_ERR_NOT_SUPPORTED;
            
        case P3A_TOUCH_EVENT_LONG_PRESS: {
            // If already in UI mode (info-screen or other overlay), dismiss it
            if (app_lcd_is_ui_mode && app_lcd_is_ui_mode()) {
                ESP_LOGI(TAG, "Long press while in UI mode - dismissing overlay");
                if (ugfx_ui_hide_info_screen) ugfx_ui_hide_info_screen();
                if (ugfx_ui_hide_captive_ap_info) ugfx_ui_hide_captive_ap_info();
                if (ugfx_ui_hide_registration) ugfx_ui_hide_registration();
                if (app_lcd_exit_ui_mode) app_lcd_exit_ui_mode();
                return ESP_OK;
            }

            // Enter UI mode: show captive AP info if in softAP, otherwise info screen
            bool in_captive = app_wifi_is_captive_portal_active && app_wifi_is_captive_portal_active();
            if (in_captive) {
                ESP_LOGI(TAG, "Long press detected - showing captive AP info");
                if (app_lcd_enter_ui_mode) app_lcd_enter_ui_mode();
                if (ugfx_ui_show_captive_ap_info) ugfx_ui_show_captive_ap_info();
            } else {
                ESP_LOGI(TAG, "Long press detected - showing info screen");
                if (app_lcd_enter_ui_mode) app_lcd_enter_ui_mode();
                if (ugfx_ui_show_info_screen) ugfx_ui_show_info_screen();
            }
            return ESP_OK;
        }
            
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

            // Step 4: Resume animation playback immediately.
            // Without this, the screen stays blank (or shows stale channel
            // messages) until the next dwell timer fires, which can be up to
            // 30 seconds. Emitting SWAP_NEXT triggers play_scheduler_next()
            // to pick and load an animation right away.
            event_bus_emit_simple(P3A_EVENT_SWAP_NEXT);

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
            // While paused, only long-press (to unpause) is allowed
            if (playback_service_is_paused && playback_service_is_paused()) {
                return (event_type == P3A_TOUCH_EVENT_LONG_PRESS);
            }
            // All gestures enabled during normal animation playback
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

