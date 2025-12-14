/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STATIC_IMAGE_DECODER_COMMON_H
#define STATIC_IMAGE_DECODER_COMMON_H

#include "animation_decoder.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Common frame delay constant for static image decoders
// Uses the component-specific Kconfig variable
#define STATIC_IMAGE_FRAME_DELAY_MS CONFIG_ANIMATION_DECODER_STATIC_FRAME_DELAY_MS

// Forward declarations for decoder cross-references
extern esp_err_t gif_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t gif_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t gif_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t gif_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t gif_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t gif_decoder_reset(animation_decoder_t *decoder);
extern void gif_decoder_unload(animation_decoder_t **decoder);

extern esp_err_t png_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t png_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t png_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t png_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t png_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t png_decoder_reset(animation_decoder_t *decoder);
extern void png_decoder_unload(animation_decoder_t **decoder);

extern esp_err_t jpeg_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t jpeg_decoder_get_info_wrapper(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t jpeg_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t jpeg_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t jpeg_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t jpeg_decoder_reset(animation_decoder_t *decoder);
extern void jpeg_decoder_unload(animation_decoder_t **decoder);

extern esp_err_t webp_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t webp_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t webp_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t webp_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t webp_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t webp_decoder_reset(animation_decoder_t *decoder);
extern void webp_decoder_unload(animation_decoder_t **decoder);

#ifdef __cplusplus
}
#endif

#endif // STATIC_IMAGE_DECODER_COMMON_H

