// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "sdkconfig.h"

#if CONFIG_P3A_PPA_UPSCALE_ENABLE

#include "esp_err.h"
#include "display_renderer.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Upscale an RGB888 source frame to a display-sized destination buffer
 * using the PPA SRM engine (bilinear interpolation + rotation + R<->B swap).
 * Fills only the border strips (not the artwork region) when borders are needed.
 *
 * @param src_rgb   Source RGB888 buffer ([R,G,B] byte order)
 * @param src_w     Source width in pixels
 * @param src_h     Source height in pixels
 * @param dst_buffer Destination RGB888 display buffer (PSRAM-backed, full frame)
 * @param dst_w     Destination width (e.g. 720)
 * @param dst_h     Destination height (e.g. 720)
 * @param rotation  Current display rotation
 * @return ESP_OK on success, or an error code (caller should fall back to CPU)
 */
esp_err_t display_ppa_upscale_rgb(
    const uint8_t *src_rgb, int src_w, int src_h,
    uint8_t *dst_buffer, int dst_w, int dst_h,
    display_rotation_t rotation);

/**
 * @brief Deinitialize PPA upscaler, releasing PPA client handles.
 */
void display_ppa_upscale_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_P3A_PPA_UPSCALE_ENABLE
