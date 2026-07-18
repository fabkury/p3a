// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file app_touch.h
 * @brief Touch input initialization interface
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "animation_player.h"  // For screen_rotation_t

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_touch_init(void);

/**
 * @brief Transform raw touch coordinates into visual space for a rotation
 *
 * Pure function, safe to call before touch init (used by the SD-format
 * fatal-screen loop, which polls the touch controller directly).
 *
 * @param x Pointer to X coordinate (modified in place)
 * @param y Pointer to Y coordinate (modified in place)
 * @param rotation Current screen rotation
 */
void app_touch_transform_coordinates(uint16_t *x, uint16_t *y, screen_rotation_t rotation);

#ifdef __cplusplus
}
#endif

