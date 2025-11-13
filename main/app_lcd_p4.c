/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_heap_caps.h"

#include "sdkconfig.h"
#include "app_lcd.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "animation_player.h"

// Forward declaration for auto-swap timer reset
extern void auto_swap_reset_timer(void);

#define TAG "app_lcd"

static esp_lcd_panel_handle_t display_handle = NULL;
static uint8_t *lcd_buffer[EXAMPLE_LCD_BUF_NUM] = { NULL };
static size_t s_frame_buffer_bytes = (size_t)EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * (EXAMPLE_LCD_BIT_PER_PIXEL / 8);
static size_t s_frame_row_stride_bytes = (size_t)EXAMPLE_LCD_H_RES * (EXAMPLE_LCD_BIT_PER_PIXEL / 8);
static uint8_t s_buffer_count = EXAMPLE_LCD_BUF_NUM;
static int s_current_brightness = 100;  // Track current brightness (0-100)

void app_lcd_draw(uint8_t *buf, uint32_t len, uint16_t width, uint16_t height)
{
    (void)buf;
    (void)len;
    (void)width;
    (void)height;
    // The animation owns the display pipeline; external draw requests are ignored in this demo.
}

esp_err_t app_lcd_init(void)
{
    bsp_display_config_t disp_config = { 0 };
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;

    ESP_LOGI(TAG, "P3A: Initialize MIPI DSI bus");

    ESP_ERROR_CHECK(bsp_display_new(&disp_config, &display_handle, &mipi_dbi_io));
    
    // Initialize brightness control
    esp_err_t err = bsp_display_brightness_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Brightness init failed: %s", esp_err_to_name(err));
    } else {
        // Set initial brightness to 100%
        s_current_brightness = 100;
        err = bsp_display_brightness_set(s_current_brightness);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Initial brightness set failed: %s", esp_err_to_name(err));
        }
    }

#if EXAMPLE_LCD_BUF_NUM == 1
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_handle, 1, (void **)&lcd_buffer[0]));
#elif EXAMPLE_LCD_BUF_NUM == 2
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_handle, 2, (void **)&lcd_buffer[0], (void **)&lcd_buffer[1]));
#else
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_handle, 3, (void **)&lcd_buffer[0], (void **)&lcd_buffer[1], (void **)&lcd_buffer[2]));
#endif

    s_buffer_count = EXAMPLE_LCD_BUF_NUM;
    const size_t bytes_per_pixel = EXAMPLE_LCD_BIT_PER_PIXEL / 8;
    if (bytes_per_pixel == 0) {
        ESP_LOGE(TAG, "Invalid bytes per pixel configuration");
        return ESP_ERR_INVALID_STATE;
    }
    s_frame_row_stride_bytes = (size_t)EXAMPLE_LCD_H_RES * bytes_per_pixel;
    s_frame_buffer_bytes = s_frame_row_stride_bytes * EXAMPLE_LCD_V_RES;

    if (s_buffer_count > 1 && lcd_buffer[0] && lcd_buffer[1] && lcd_buffer[1] > lcd_buffer[0]) {
        const size_t spacing_bytes = (size_t)(lcd_buffer[1] - lcd_buffer[0]);
        if (spacing_bytes > 0 && (spacing_bytes % EXAMPLE_LCD_V_RES) == 0) {
            const size_t candidate_row_stride = spacing_bytes / EXAMPLE_LCD_V_RES;
            if (candidate_row_stride >= s_frame_row_stride_bytes) {
                s_frame_row_stride_bytes = candidate_row_stride;
                s_frame_buffer_bytes = spacing_bytes;
            }
        }
    }

    ESP_LOGI(TAG, "Frame buffer stride: %zu bytes, size: %zu bytes", s_frame_row_stride_bytes, s_frame_buffer_bytes);
    
    // Initialize animation player
    err = animation_player_init(display_handle, lcd_buffer, s_buffer_count,
                                s_frame_buffer_bytes, s_frame_row_stride_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize animation player: %s", esp_err_to_name(err));
        return err;
    }

    // Start animation player task
    err = animation_player_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start animation player: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
    }

void app_lcd_set_animation_paused(bool paused)
{
    bool was_paused = animation_player_is_paused();
    animation_player_set_paused(paused);
    // Reset auto-swap timer when resuming playback
    if (was_paused && !paused) {
        auto_swap_reset_timer();
    }
}

void app_lcd_toggle_animation_pause(void)
{
    animation_player_toggle_pause();
}

bool app_lcd_is_animation_paused(void)
{
    return animation_player_is_paused();
}

void app_lcd_cycle_animation(void)
{
    animation_player_cycle_animation(true);
    auto_swap_reset_timer();  // Reset auto-swap timer on any swap
}

void app_lcd_cycle_animation_backward(void)
{
    animation_player_cycle_animation(false);
    auto_swap_reset_timer();  // Reset auto-swap timer on any swap
}

int app_lcd_get_brightness(void)
{
    return s_current_brightness;
}

esp_err_t app_lcd_set_brightness(int brightness_percent)
{
    if (brightness_percent < 0) {
        brightness_percent = 0;
    } else if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    
    esp_err_t err = bsp_display_brightness_set(brightness_percent);
    if (err == ESP_OK) {
        s_current_brightness = brightness_percent;
    }
    return err;
}

esp_err_t app_lcd_adjust_brightness(int delta_percent)
{
    int new_brightness = s_current_brightness + delta_percent;
    return app_lcd_set_brightness(new_brightness);
}
