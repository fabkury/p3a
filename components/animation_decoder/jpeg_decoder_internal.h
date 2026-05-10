// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file jpeg_decoder_internal.h
 * @brief Private interface between the JPEG HW decoder front-end
 *        (jpeg_animation_decoder.c) and the libjpeg-turbo SW fallback
 *        (jpeg_animation_decoder_sw.c).
 */

#ifndef JPEG_DECODER_INTERNAL_H
#define JPEG_DECODER_INTERNAL_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode a JPEG via libjpeg-turbo into a freshly-allocated RGB888 buffer.
 *
 * Picks a scaled-IDCT ratio M/8 (M in 1..8) such that the decoded dimensions
 * remain >= the display panel on both axes. Sources whose native dimensions
 * are below the panel decode at native (M=8); the display pipeline then
 * upscales to panel size with hardware-accelerated bilinear interpolation.
 *
 * The output buffer is allocated in PSRAM with internal-heap fallback. On
 * success, ownership transfers to the caller, who must free() it. On
 * failure, nothing is allocated and the output pointers are untouched.
 *
 * Used as the universal fallback when jpeg_animation_decoder.c's HW path
 * returns any ESP_ERR_*: progressive JPEGs (SOF2), files where (W*H) % 8 != 0
 * (rejected by IDF v5.5.1's SOF gate), files whose RGB output buffer would
 * exceed PSRAM, etc.
 *
 * @param data        Pointer to the JPEG bitstream
 * @param size        Length of @p data in bytes
 * @param out_rgb     Receives the freshly-allocated RGB888 buffer on success
 * @param out_width   Receives the decoded (scaled) width
 * @param out_height  Receives the decoded (scaled) height
 * @return ESP_OK on success; ESP_ERR_NO_MEM on allocation failure;
 *         ESP_FAIL on any libjpeg-turbo decode error.
 */
esp_err_t jpeg_decode_sw_to_rgb888(const uint8_t *data, size_t size,
                                   uint8_t **out_rgb,
                                   uint32_t *out_width,
                                   uint32_t *out_height);

#ifdef __cplusplus
}
#endif

#endif // JPEG_DECODER_INTERNAL_H
