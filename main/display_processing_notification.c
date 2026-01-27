// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

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
#define PROC_NOTIF_TIMEOUT_MS       5000    // Timeout for swap (triggers failure)
#define PROC_NOTIF_FAIL_DISPLAY_MS  3000    // How long to show red triangle

// Cached config values (re-read periodically)
static bool s_enabled_cached = true;
static uint8_t s_size_cached = 32;
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
static void draw_checkerboard_triangle(uint8_t *buffer, uint8_t r, uint8_t g, uint8_t b, uint8_t size)
{
    if (!buffer || size < 8) return;
    
    // Calculate top-left corner of the bounding box in screen coordinates
    int base_x = EXAMPLE_LCD_H_RES - size;
    int base_y = EXAMPLE_LCD_V_RES - size;
    
    // Iterate over local coordinates within the bounding box
    for (int ly = 0; ly < size; ly++) {
        for (int lx = 0; lx < size; lx++) {
            // Triangle condition: (N - 1 - lx) <= ly
            // This fills the lower-right triangle
            if ((size - 1 - lx) <= ly) {
                // Checkerboard pattern
                if ((lx + ly) % 2 == 0) {
                    int screen_x = base_x + lx;
                    int screen_y = base_y + ly;
                    pn_draw_pixel(buffer, screen_x, screen_y, r, g, b);
                }
            }
        }
    }
}

void proc_notif_start(void)
{
    // Only start if not already in PROCESSING or FAILED state
    if (g_proc_notif_state == PROC_NOTIF_STATE_IDLE) {
        g_proc_notif_start_time_us = esp_timer_get_time();
        g_proc_notif_state = PROC_NOTIF_STATE_PROCESSING;
        ESP_LOGD(TAG, "Processing notification started");
    }
}

void proc_notif_success(void)
{
    // Clear immediately on successful swap
    if (g_proc_notif_state == PROC_NOTIF_STATE_PROCESSING) {
        g_proc_notif_state = PROC_NOTIF_STATE_IDLE;
        g_proc_notif_start_time_us = 0;
        ESP_LOGD(TAG, "Processing notification cleared (success)");
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
        s_config_check_time_us = now_us;
    }
    
    // Check if feature is enabled
    if (!s_enabled_cached) {
        return;
    }
    
    // State machine
    switch (g_proc_notif_state) {
        case PROC_NOTIF_STATE_IDLE:
            // Nothing to draw
            return;
            
        case PROC_NOTIF_STATE_PROCESSING:
            // Check for timeout (5 seconds)
            if ((now_us - g_proc_notif_start_time_us) > ((int64_t)PROC_NOTIF_TIMEOUT_MS * 1000LL)) {
                g_proc_notif_state = PROC_NOTIF_STATE_FAILED;
                g_proc_notif_fail_time_us = now_us;
                ESP_LOGW(TAG, "Processing notification timed out - swap failed");
                // Fall through to draw red triangle
            } else {
                // Draw blue triangle (processing)
                draw_checkerboard_triangle(buffer, 0, 0, 255, s_size_cached);
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
            draw_checkerboard_triangle(buffer, 255, 0, 0, s_size_cached);
            break;
    }
}
