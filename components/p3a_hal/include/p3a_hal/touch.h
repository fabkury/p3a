#pragma once

#include "esp_err.h"

/**
 * @brief Initialise GT911 touch handling and simple LVGL demo overlay.
 */
esp_err_t p3a_hal_touch_init(void);

/**
 * @brief Access the cached LVGL touch input device handle.
 */
struct _lv_indev_t *p3a_hal_touch_get_indev(void);

