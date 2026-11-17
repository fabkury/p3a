// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// The board: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B. 720x720 IPS panel over
// MIPI-DSI, GT911 capacitive touch, BOOT button, backlight, microSD. p3a 2.0 targets
// this one board (docs/2.0/DECISIONS.md, C1), so this is a description of it
// rather than an abstraction over several: there are no capability flags.
//
// Layer: board. Depends only on util.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "sd_protocol_types.h"
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

// --- touch ---------------------------------------------------------------------

// Brings up the GT911 touch controller, waiting at most timeout_ms. The I2C
// bus is first freed if a previous boot left the controller holding it.
//
// ESP_ERR_TIMEOUT means the controller did not answer and the bus is probably
// wedged; touch stays unavailable for this boot. What to do about a failure
// (retry by rebooting, carry on without touch) is the caller's decision.
esp_err_t p3a_board_touch_init(uint32_t timeout_ms);

// The controller, for esp_lcd_touch_read_data() and esp_lcd_touch_get_data().
// NULL when touch is not available.
esp_lcd_touch_handle_t p3a_board_get_touch(void);

// --- microSD card ---------------------------------------------------------------

#define P3A_SD_MOUNT_POINT "/sdcard"

// Mounts the card's FAT volume at P3A_SD_MOUNT_POINT (SDMMC slot 0, 4-bit).
// Never formats: a card that does not mount is reported, not erased.
esp_err_t p3a_board_sd_mount(void);
esp_err_t p3a_board_sd_unmount(void);
bool p3a_board_sd_is_mounted(void);

// The card, for raw sector access (sd_repair, USB mass storage). NULL unless mounted.
sdmmc_card_t *p3a_board_sd_card(void);

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
