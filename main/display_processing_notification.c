// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file display_processing_notification.c
 * @brief Processing notification overlay for display
 * 
 * Displays a checkerboard triangle in the bottom-right corner when a user
 * initiates an animation swap. Blue during processing, red on failure.
 */

#include "display_renderer_priv.h"
#include "config_store.h"

static const char *TAG = "proc_notif";

// Timing constants
//
// PROC_NOTIF_WATCHDOG_MS is a safety net, not a primary failure trigger.
// Terminal swap-failure sites in the animation_player and play_scheduler
// signal proc_notif_fail_if_processing() explicitly. If a future code path
// adds a failure without that signal, the watchdog turns the triangle red
// after this interval and logs a warning so the gap is observable.
#define PROC_NOTIF_WATCHDOG_MS      30000   // Sanity cap on BLUE state
#define PROC_NOTIF_FAIL_DISPLAY_MS  3000    // How long to show red triangle

// Cached config values (re-read periodically)
static bool s_enabled_cached = true;
static uint16_t s_size_cached = 64;
static int64_t s_config_check_time_us = 0;

// Draw a single pixel at (x, y) with color (r, g, b)
static inline void pn_draw_pixel(uint8_t *buffer, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= EXAMPLE_LCD_H_RES || y < 0 || y >= EXAMPLE_LCD_V_RES) return;
    
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
    uint16_t *row = (uint16_t *)(buffer + (size_t)y * g_display_row_stride);
    row[x] = rgb565(r, g, b);
#else
    uint8_t *row = buffer + (size_t)y * g_display_row_stride;
    size_t idx = (size_t)x * 3U;
    row[idx + 0] = b;
    row[idx + 1] = g;
    row[idx + 2] = r;
#endif
}

/**
 * @brief Draw a checkerboard triangle in the bottom-right corner
 * 
 * The triangle is a 45-45-90 isosceles right triangle with the right angle
 * at the bottom-right corner. For size N, the triangle fills pixels where
 * (N - 1 - lx) <= ly (where lx, ly ∈ [0, N-1]).
 * 
 * Checkerboard pattern: draw pixel if (lx + ly) % 2 == 0
 * 
 * @param buffer Frame buffer to draw into
 * @param r Red component (0-255)
 * @param g Green component (0-255)
 * @param b Blue component (0-255)
 * @param size Triangle size in pixels
 */
static void draw_checkerboard_triangle(uint8_t *buffer, uint8_t r, uint8_t g, uint8_t b,
                                       uint16_t size, display_rotation_t rotation)
{
    if (!buffer || size < 16) return;

    // Precompute coordinate mapping so the triangle appears in the user's
    // bottom-right corner regardless of rotation: screen = base + dir * local
    int base_x, base_y, dir_x, dir_y;
    switch (rotation) {
        default:
        case DISPLAY_ROTATION_0:
            base_x = EXAMPLE_LCD_H_RES - size; dir_x =  1;
            base_y = EXAMPLE_LCD_V_RES - size; dir_y =  1;
            break;
        case DISPLAY_ROTATION_90:
            base_x = size - 1;                 dir_x = -1;
            base_y = EXAMPLE_LCD_V_RES - size; dir_y =  1;
            break;
        case DISPLAY_ROTATION_180:
            base_x = size - 1;                 dir_x = -1;
            base_y = size - 1;                 dir_y = -1;
            break;
        case DISPLAY_ROTATION_270:
            base_x = EXAMPLE_LCD_H_RES - size; dir_x =  1;
            base_y = size - 1;                 dir_y = -1;
            break;
    }

    // Iterate over local coordinates within the bounding box
    for (int ly = 0; ly < size; ly++) {
        for (int lx = 0; lx < size; lx++) {
            // Triangle condition: (N - 1 - lx) <= ly
            // This fills the lower-right triangle in local space
            if ((size - 1 - lx) <= ly) {
                // Checkerboard pattern
                if ((lx + ly) % 2 == 0) {
                    pn_draw_pixel(buffer, base_x + dir_x * lx,
                                  base_y + dir_y * ly, r, g, b);
                }
            }
        }
    }
}

void proc_notif_start(void)
{
    // Start (or restart) on every user-initiated swap. IDLE→PROCESSING is the
    // common case; FAILED→PROCESSING lets a rapid retap clear the red flash
    // and reflect that a new swap is now underway. From PROCESSING we no-op
    // so a stacked second tap during an in-flight swap doesn't reset the
    // clock — the in-flight swap will signal terminal state on its own.
    if (g_proc_notif_state != PROC_NOTIF_STATE_PROCESSING) {
        g_proc_notif_start_time_us = esp_timer_get_time();
        g_proc_notif_fail_time_us = 0;
        g_proc_notif_state = PROC_NOTIF_STATE_PROCESSING;
        ESP_LOGD(TAG, "Processing notification started");
    }
}

void proc_notif_success(void)
{
    // Clear immediately on successful swap - regardless of current state
    // This handles race conditions where timeout fires just before success
    if (g_proc_notif_state != PROC_NOTIF_STATE_IDLE) {
        g_proc_notif_state = PROC_NOTIF_STATE_IDLE;
        g_proc_notif_start_time_us = 0;
        g_proc_notif_fail_time_us = 0;
        ESP_LOGD(TAG, "Processing notification cleared (success)");
    }
}

void proc_notif_fail(void)
{
    // Directly transition to FAILED state (red triangle for 3 seconds)
    g_proc_notif_fail_time_us = esp_timer_get_time();
    g_proc_notif_state = PROC_NOTIF_STATE_FAILED;
    g_proc_notif_start_time_us = 0;
    ESP_LOGD(TAG, "Processing notification set to failed");
}

void proc_notif_fail_if_processing(void)
{
    if (g_proc_notif_state == PROC_NOTIF_STATE_PROCESSING) {
        g_proc_notif_fail_time_us = esp_timer_get_time();
        g_proc_notif_state = PROC_NOTIF_STATE_FAILED;
        g_proc_notif_start_time_us = 0;
        ESP_LOGD(TAG, "Processing notification set to failed (was processing)");
    }
}

void processing_notification_update_and_draw(uint8_t *buffer)
{
    if (!buffer) return;
    
    int64_t now_us = esp_timer_get_time();
    
    // Check config periodically (every 1 second) to avoid frequent NVS reads
    if (now_us - s_config_check_time_us > 1000000) {
        s_enabled_cached = config_store_get_proc_notif_enabled();
        s_size_cached = config_store_get_proc_notif_size();
        // Defensive bounds check on size (config_store validates, but be safe)
        // Size 0 is valid (disabled), otherwise clamp to 16-256
        if (s_size_cached != 0) {
            if (s_size_cached < 16) s_size_cached = 16;
            if (s_size_cached > 256) s_size_cached = 256;
        }
        s_config_check_time_us = now_us;
    }
    
    // Check if feature is enabled - if disabled or size is 0, reset state
    if (!s_enabled_cached || s_size_cached == 0) {
        if (g_proc_notif_state != PROC_NOTIF_STATE_IDLE) {
            g_proc_notif_state = PROC_NOTIF_STATE_IDLE;
            g_proc_notif_start_time_us = 0;
            g_proc_notif_fail_time_us = 0;
        }
        return;
    }
    
    // State machine
    switch (g_proc_notif_state) {
        case PROC_NOTIF_STATE_IDLE:
            // Nothing to draw
            return;
            
        case PROC_NOTIF_STATE_PROCESSING:
            // Watchdog: terminal swap sites are expected to signal explicitly
            // via proc_notif_success / proc_notif_fail_if_processing. This
            // branch should never fire under correct operation; if it does,
            // some failure path is missing its terminal signal.
            if ((now_us - g_proc_notif_start_time_us) > ((int64_t)PROC_NOTIF_WATCHDOG_MS * 1000LL)) {
                g_proc_notif_state = PROC_NOTIF_STATE_FAILED;
                g_proc_notif_fail_time_us = now_us;
                ESP_LOGW(TAG, "Processing notification watchdog fired after %d ms — swap pipeline did not signal terminal state",
                         PROC_NOTIF_WATCHDOG_MS);
                // Fall through to draw red triangle
            } else {
                // Draw blue triangle (processing)
                draw_checkerboard_triangle(buffer, 0, 0, 255, s_size_cached, g_screen_rotation);
                return;
            }
            // Fall through to FAILED case
            __attribute__((fallthrough));
            
        case PROC_NOTIF_STATE_FAILED:
            // Check if 3 seconds have elapsed since failure
            if ((now_us - g_proc_notif_fail_time_us) > ((int64_t)PROC_NOTIF_FAIL_DISPLAY_MS * 1000LL)) {
                g_proc_notif_state = PROC_NOTIF_STATE_IDLE;
                g_proc_notif_start_time_us = 0;
                g_proc_notif_fail_time_us = 0;
                ESP_LOGD(TAG, "Processing notification cleared (failure timeout)");
                return;
            }
            // Draw red triangle (failed)
            draw_checkerboard_triangle(buffer, 255, 0, 0, s_size_cached, g_screen_rotation);
            break;
    }
}
