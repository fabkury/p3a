// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

// What each format file provides to the dispatcher in animation_decoder.c.
// Not a public header.

#ifndef ANIMATION_DECODER_FORMATS_H
#define ANIMATION_DECODER_FORMATS_H

#include "animation_decoder.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "p3a_err.h"

// What to answer for a picture whose declared size cannot be used. Over the
// limit, the file may be perfectly sound and is simply not for this player
// (P3A_ERR_NOT_PLAYABLE); a size of zero is damage.
static inline esp_err_t animation_decoder_size_error(uint32_t width, uint32_t height)
{
    return (width > ANIMATION_DECODER_MAX_DIMENSION || height > ANIMATION_DECODER_MAX_DIMENSION)
               ? (esp_err_t)P3A_ERR_NOT_PLAYABLE
               : ESP_ERR_INVALID_SIZE;
}

#ifdef __cplusplus
extern "C" {
#endif

extern esp_err_t gif_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t gif_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t gif_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t gif_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t gif_decoder_reset(animation_decoder_t *decoder);
extern void gif_decoder_unload(animation_decoder_t **decoder);

extern esp_err_t png_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t png_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t png_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t png_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t png_decoder_reset(animation_decoder_t *decoder);
extern void png_decoder_unload(animation_decoder_t **decoder);

extern esp_err_t jpeg_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t jpeg_decoder_get_info_wrapper(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t jpeg_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t jpeg_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t jpeg_decoder_reset(animation_decoder_t *decoder);
extern void jpeg_decoder_unload(animation_decoder_t **decoder);

extern esp_err_t bmp_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t bmp_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t bmp_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t bmp_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t bmp_decoder_reset(animation_decoder_t *decoder);
extern void bmp_decoder_unload(animation_decoder_t **decoder);

extern esp_err_t webp_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t webp_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t webp_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
extern esp_err_t webp_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t webp_decoder_reset(animation_decoder_t *decoder);
extern void webp_decoder_unload(animation_decoder_t **decoder);

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_DECODER_FORMATS_H

