// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file display_fps_overlay.c
 * @brief FPS overlay rendering for display
 */

#include "display_renderer_priv.h"
#include "config_store.h"

// FPS tracking state
static int64_t s_fps_last_time_us = 0;
static uint32_t s_fps_frame_count = 0;
static uint32_t s_fps_current = 0;
static bool s_fps_show_cached = true;
static int64_t s_fps_config_check_time = 0;

// Simple 5x7 bitmap font for digits 0-9 and space
// Each digit is 5 pixels wide, 7 pixels tall, stored as 7 bytes (1 byte per row)
static const uint8_t s_font_5x7[11][7] = {
    // '0'
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    // '1'
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // '2'
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    // '3'
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    // '4'
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    // '5'
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    // '6'
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    // '7'
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    // '8'
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    // '9'
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    // ' ' (space)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

// Draw a single pixel at (x, y) with color (r, g, b)
static inline void fps_draw_pixel(uint8_t *buffer, int x, int y, uint8_t r, uint8_t g, uint8_t b)
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

// Draw a character at position (x, y) with given colors, scale factor
static void fps_draw_char(uint8_t *buffer, int x, int y, int ch_idx, int scale,
                          uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                          uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    if (ch_idx < 0 || ch_idx > 10) return;
    
    for (int row = 0; row < 7; row++) {
        uint8_t bits = s_font_5x7[ch_idx][row];
        for (int col = 0; col < 5; col++) {
            bool pixel_on = (bits >> (4 - col)) & 1;
            uint8_t r = pixel_on ? fg_r : bg_r;
            uint8_t g = pixel_on ? fg_g : bg_g;
            uint8_t b = pixel_on ? fg_b : bg_b;
            
            // Draw scaled pixel
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    fps_draw_pixel(buffer, x + col * scale + sx, y + row * scale + sy, r, g, b);
                }
            }
        }
    }
}

// Draw FPS overlay on top-right of screen
static void fps_draw_overlay(uint8_t *buffer, uint32_t fps)
{
    // Format: "XXX" (up to 3 digits)
    char fps_str[8];
    int len = snprintf(fps_str, sizeof(fps_str), "%lu", (unsigned long)fps);
    if (len < 0 || len >= (int)sizeof(fps_str)) len = 3;
    
    const int scale = 2;  // 2x scale for better visibility
    const int char_w = 5 * scale + scale;  // Character width + spacing
    const int char_h = 7 * scale;
    const int padding = 6;
    
    // Position at top-right
    int total_width = len * char_w - scale;  // Remove trailing space
    int x = EXAMPLE_LCD_H_RES - total_width - padding;
    int y = padding;
    
    // Draw background rectangle (semi-transparent effect via darker bg)
    int bg_x = x - 4;
    int bg_y = y - 2;
    int bg_w = total_width + 8;
    int bg_h = char_h + 4;
    
    for (int by = bg_y; by < bg_y + bg_h && by < EXAMPLE_LCD_V_RES; by++) {
        for (int bx = bg_x; bx < bg_x + bg_w && bx < EXAMPLE_LCD_H_RES; bx++) {
            if (bx >= 0 && by >= 0) {
                fps_draw_pixel(buffer, bx, by, 0, 0, 0);
            }
        }
    }
    
    // Draw each digit
    for (int i = 0; i < len; i++) {
        int ch_idx = 10;  // space by default
        if (fps_str[i] >= '0' && fps_str[i] <= '9') {
            ch_idx = fps_str[i] - '0';
        }
        fps_draw_char(buffer, x + i * char_w, y, ch_idx, scale,
                      255, 255, 255,  // White foreground
                      0, 0, 0);       // Black background
    }
}

// Update FPS counter and optionally draw overlay
void fps_update_and_draw(uint8_t *buffer)
{
    int64_t now_us = esp_timer_get_time();
    
    // Check config every second to avoid frequent NVS reads
    if (now_us - s_fps_config_check_time > 1000000) {
        s_fps_show_cached = config_store_get_show_fps();
        s_fps_config_check_time = now_us;
    }
    
    // Always track FPS (for debugging/logging)
    s_fps_frame_count++;
    
    if (s_fps_last_time_us == 0) {
        s_fps_last_time_us = now_us;
        return;
    }
    
    int64_t elapsed_us = now_us - s_fps_last_time_us;
    if (elapsed_us >= 1000000) {
        // Calculate FPS
        s_fps_current = (uint32_t)((uint64_t)s_fps_frame_count * 1000000ULL / (uint64_t)elapsed_us);
        s_fps_frame_count = 0;
        s_fps_last_time_us = now_us;
    }
    
    // Draw overlay if enabled
    if (s_fps_show_cached && buffer && s_fps_current > 0) {
        fps_draw_overlay(buffer, s_fps_current);
    }
}

