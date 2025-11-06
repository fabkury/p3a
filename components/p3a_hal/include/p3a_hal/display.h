#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Bring up the ST7703 display stack and LVGL port.
 */
esp_err_t p3a_hal_display_init(void);

/**
 * @brief Set LCD backlight brightness percentage (0-100).
 */
esp_err_t p3a_hal_display_set_brightness(int percent);

/**
 * @brief Fill the active screen with a solid RGB color (0xRRGGBB).
 */
esp_err_t p3a_hal_display_fill_color(uint32_t rgb888);

/**
 * @brief Obtain the cached LVGL display handle; returns NULL if not initialised.
 */
struct _lv_display_t *p3a_hal_display_get_handle(void);

