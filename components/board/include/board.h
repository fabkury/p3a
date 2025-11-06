#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * @brief Initialise board level resources (power rails, key GPIO defaults, backlight PWM).
 *
 * This routine is idempotent. Subsequent calls return immediately.
 */
esp_err_t board_init(void);

/**
 * @brief Emit the static pin mapping table to the log (INFO level).
 */
void board_print_pin_map(void);

/**
 * @brief Set LCD backlight brightness in percent (0-100).
 */
esp_err_t board_backlight_set_percent(int percent);

/**
 * @brief Convenience helper to toggle the backlight fully on/off.
 */
esp_err_t board_backlight_set_enabled(bool on);
