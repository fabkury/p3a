// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// The board: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B. 720x720 IPS panel over
// MIPI-DSI, GT911 touch, BOOT button, backlight. p3a 2.0 targets this one
// board (docs/2.0/DECISIONS.md, C1), so this is a description of it rather
// than an abstraction over several: there are no capability flags.
//
// Layer: board. Depends only on util.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_BOARD_NAME      "EP44B"
#define P3A_BOARD_FULL_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-4B"

// --- display ------------------------------------------------------------------

#define P3A_DISPLAY_WIDTH  720
#define P3A_DISPLAY_HEIGHT 720
// 24-bit, stored blue, green, red in memory. The only format 2.0 supports.
#define P3A_DISPLAY_BPP             24
#define P3A_DISPLAY_BYTES_PER_PIXEL 3
// Framebuffers owned by the DPI panel driver (sdkconfig.defaults sets 3: the
// display pipeline's triple buffering).
#define P3A_DISPLAY_BUFFERS CONFIG_BSP_LCD_DPI_BUFFER_NUMS

// Brings up the panel and its framebuffers, paints every framebuffer with the
// given colour, and only then turns the backlight on at full brightness, so
// that the panel never lights up on a black or garbage frame.
esp_err_t p3a_board_display_init(uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);

// NULL before a successful p3a_board_display_init().
esp_lcd_panel_handle_t p3a_board_get_panel(void);

// Framebuffer `index` (0 to count - 1), or NULL. Present one with
// esp_lcd_panel_draw_bitmap(panel, 0, 0, width, height, buffer).
uint8_t *p3a_board_get_buffer(int index);
uint8_t p3a_board_get_buffer_count(void);

// Bytes from one row to the next. At least width * 3; use this, not the
// constant, when walking a framebuffer.
size_t p3a_board_get_row_stride(void);
size_t p3a_board_get_buffer_bytes(void);

// --- backlight ----------------------------------------------------------------

// 0 to 100, clamped. The value is eased (cubed) on its way to the hardware, so
// that the low end of the range is usable. 0 turns the backlight off.
esp_err_t p3a_board_set_brightness(int percent);

// The last value set, as given (not the eased one).
int p3a_board_get_brightness(void);

// --- BOOT button --------------------------------------------------------------

// Called once per debounced press, on the FreeRTOS timer task: be quick, and
// hand anything slow to a job.
typedef void (*p3a_board_button_cb_t)(void *ctx);

// Starts watching the BOOT button (GPIO 35, active low). The board only
// reports the press; what a press means is the caller's business.
esp_err_t p3a_board_button_init(p3a_board_button_cb_t on_press, void *ctx);

#ifdef __cplusplus
}
#endif
