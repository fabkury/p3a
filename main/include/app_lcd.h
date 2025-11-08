/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_LCD_H
#define ESP_LCD_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
#undef BSP_LCD_COLOR_FORMAT
#define BSP_LCD_COLOR_FORMAT (ESP_LCD_COLOR_FORMAT_RGB888)
#undef BSP_LCD_BITS_PER_PIXEL
#define BSP_LCD_BITS_PER_PIXEL (24)
#elif CONFIG_BSP_LCD_COLOR_FORMAT_RGB565
#undef BSP_LCD_COLOR_FORMAT
#define BSP_LCD_COLOR_FORMAT (ESP_LCD_COLOR_FORMAT_RGB565)
#undef BSP_LCD_BITS_PER_PIXEL
#define BSP_LCD_BITS_PER_PIXEL (16)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define EXAMPLE_LCD_H_RES                   (BSP_LCD_H_RES)
#define EXAMPLE_LCD_V_RES                   (BSP_LCD_V_RES)

#define EXAMPLE_LCD_BUF_NUM                 (CONFIG_BSP_LCD_DPI_BUFFER_NUMS)

#if CONFIG_P3A_MAX_SPEED_PLAYBACK
#define APP_LCD_MAX_SPEED_PLAYBACK_ENABLED 1
#else
#define APP_LCD_MAX_SPEED_PLAYBACK_ENABLED 0
#endif

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
#define EXAMPLE_LCD_BIT_PER_PIXEL           (16)
#elif CONFIG_LCD_PIXEL_FORMAT_RGB888
#define EXAMPLE_LCD_BIT_PER_PIXEL           (24)
#endif

#define EXAMPLE_LCD_BUF_LEN                 EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * EXAMPLE_LCD_BIT_PER_PIXEL / 8

/**
 * @brief Initialize the LCD panel.
 *
 * This function initializes the LCD panel with the provided panel handle. It powers on the LCD,
 * installs the LCD driver, configures the bus, and sets up the panel.
 *
 * @return
 *    - ESP_OK: Success
 *    - ESP_FAIL: Failure
 */
esp_err_t app_lcd_init(void);

void app_lcd_draw(uint8_t *buf, uint32_t len, uint16_t width, uint16_t height);

void app_lcd_set_animation_paused(bool paused);

void app_lcd_toggle_animation_pause(void);

bool app_lcd_is_animation_paused(void);

void app_lcd_cycle_animation(void);

#ifdef __cplusplus
}
#endif

#endif
