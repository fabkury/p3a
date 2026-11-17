// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

// The panel, its framebuffers and the backlight. Touch joins this file's
// neighbours in a later step of M1.2.

#include "p3a_board.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include <string.h>

static const char *TAG = "p3a_board_display";

// Display state
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static uint8_t *s_buffers[P3A_DISPLAY_BUFFERS] = { NULL };
static uint8_t s_buffer_count = 0;
static size_t s_row_stride = 0;
static size_t s_buffer_bytes = 0;
static int s_brightness = 100;
static bool s_initialized = false;

// Fill every framebuffer with a solid color (BGR888) before the backlight is
// enabled, so the panel never lights up showing a black or garbage frame.
// Fills the first visible row pixelwise, then replicates it via memcpy for speed.
static void prepaint_buffers(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_buffer_count == 0 || !s_buffers[0]) {
        return;
    }

    uint8_t *b0 = s_buffers[0];

    // First visible row (BGR byte order, matching the render pipeline).
    for (int x = 0; x < P3A_DISPLAY_WIDTH; x++) {
        b0[x * 3 + 0] = b;
        b0[x * 3 + 1] = g;
        b0[x * 3 + 2] = r;
    }

    // Replicate that row down buffer 0, then copy buffer 0 into the others.
    const size_t visible_row_bytes = (size_t)P3A_DISPLAY_WIDTH * 3;
    for (int y = 1; y < P3A_DISPLAY_HEIGHT; y++) {
        memcpy(b0 + (size_t)y * s_row_stride, b0, visible_row_bytes);
    }
    for (int i = 1; i < s_buffer_count; i++) {
        if (s_buffers[i]) {
            memcpy(s_buffers[i], b0, s_buffer_bytes);
        }
    }
}

esp_err_t p3a_board_display_init(uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Display already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing %s display (%dx%d, %d-bit)",
             P3A_BOARD_NAME, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT, P3A_DISPLAY_BPP);

    // Initialize display panel using BSP
    bsp_display_config_t config = { 0 };
    esp_err_t err = bsp_display_new(&config, &s_panel, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create display: %s", esp_err_to_name(err));
        return err;
    }

    // Get framebuffers from DPI panel (variadic API requires compile-time switch)
    switch (P3A_DISPLAY_BUFFERS) {
        case 1:
            err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1,
                (void **)&s_buffers[0]);
            break;
        case 2:
            err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2,
                (void **)&s_buffers[0], (void **)&s_buffers[1]);
            break;
        case 3:
            err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3,
                (void **)&s_buffers[0], (void **)&s_buffers[1],
                (void **)&s_buffers[2]);
            break;
        default:
            ESP_LOGE(TAG, "Unsupported buffer count: %d", P3A_DISPLAY_BUFFERS);
            return ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get framebuffers: %s", esp_err_to_name(err));
        return err;
    }

    s_buffer_count = P3A_DISPLAY_BUFFERS;

    // Calculate row stride and buffer size
    s_row_stride = (size_t)P3A_DISPLAY_WIDTH * P3A_DISPLAY_BYTES_PER_PIXEL;
    s_buffer_bytes = s_row_stride * P3A_DISPLAY_HEIGHT;

    // Check if hardware uses padding (detect from buffer spacing)
    if (s_buffer_count > 1 && s_buffers[0] && s_buffers[1] && s_buffers[1] > s_buffers[0]) {
        const size_t spacing = (size_t)(s_buffers[1] - s_buffers[0]);
        if (spacing > 0 && (spacing % P3A_DISPLAY_HEIGHT) == 0) {
            const size_t detected_stride = spacing / P3A_DISPLAY_HEIGHT;
            if (detected_stride >= s_row_stride) {
                s_row_stride = detected_stride;
                s_buffer_bytes = spacing;
            }
        }
    }

    // Prepaint every framebuffer to the background color BEFORE enabling the
    // backlight. This guarantees the first thing the user sees when the panel
    // lights up is the configured background — never a black or garbage frame.
    // The render pipeline (boot logo, then animations) takes over once started.
    prepaint_buffers(bg_r, bg_g, bg_b);

    // Enable the backlight last, now that the framebuffers show the background.
    err = bsp_display_brightness_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Brightness init failed: %s (continuing without)", esp_err_to_name(err));
    } else {
        s_brightness = 100;
        bsp_display_brightness_set(s_brightness);
    }

    ESP_LOGI(TAG, "Display initialized: %d buffers, stride=%zu, size=%zu bytes",
             s_buffer_count, s_row_stride, s_buffer_bytes);

    s_initialized = true;
    return ESP_OK;
}

esp_lcd_panel_handle_t p3a_board_get_panel(void)
{
    return s_panel;
}

uint8_t* p3a_board_get_buffer(int index)
{
    if (index < 0 || index >= s_buffer_count) {
        return NULL;
    }
    return s_buffers[index];
}

uint8_t p3a_board_get_buffer_count(void)
{
    return s_buffer_count;
}

size_t p3a_board_get_row_stride(void)
{
    return s_row_stride;
}

size_t p3a_board_get_buffer_bytes(void)
{
    return s_buffer_bytes;
}

// ============================================================================
// Brightness control
// ============================================================================

esp_err_t p3a_board_set_brightness(int percent)
{
    // Clamp to valid range
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    // Apply cubic easing (ease-in): slow near 0%, faster near 100%
    // This provides a more natural brightness curve
    float normalized = (float)percent / 100.0f;
    float eased = normalized * normalized * normalized;
    int hw_brightness = (int)(eased * 100.0f + 0.5f);

    if (hw_brightness < 0) hw_brightness = 0;
    if (hw_brightness > 100) hw_brightness = 100;

    esp_err_t err = bsp_display_brightness_set(hw_brightness);
    if (err == ESP_OK) {
        s_brightness = percent;
    }
    return err;
}

int p3a_board_get_brightness(void)
{
    return s_brightness;
}
