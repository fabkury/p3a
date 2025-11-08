/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ANIMATION_DECODER_H
#define ANIMATION_DECODER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration - opaque decoder handle
typedef struct animation_decoder_s animation_decoder_t;

// Decoder type enumeration
typedef enum {
    ANIMATION_DECODER_TYPE_WEBP,
    ANIMATION_DECODER_TYPE_GIF,
} animation_decoder_type_t;

// Decoder information structure
typedef struct {
    uint32_t canvas_width;
    uint32_t canvas_height;
    size_t frame_count;
    bool has_transparency;
} animation_decoder_info_t;

/**
 * @brief Initialize an animation decoder
 *
 * @param decoder Pointer to decoder handle (output)
 * @param type Decoder type (WebP or GIF)
 * @param data Pointer to animation file data
 * @param size Size of animation file data in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t animation_decoder_init(animation_decoder_t **decoder, animation_decoder_type_t type, const uint8_t *data, size_t size);

/**
 * @brief Get decoder information
 *
 * @param decoder Decoder handle
 * @param info Pointer to info structure (output)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t animation_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);

/**
 * @brief Decode the next frame
 *
 * @param decoder Decoder handle
 * @param rgba_buffer Buffer to store decoded RGBA frame (must be at least canvas_width * canvas_height * 4 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t animation_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);

/**
 * @brief Get the delay (duration) of the last decoded frame in milliseconds
 *
 * @param decoder Decoder handle
 * @param delay_ms Pointer to store frame delay in milliseconds (output)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t animation_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);

/**
 * @brief Reset decoder to beginning
 *
 * @param decoder Decoder handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t animation_decoder_reset(animation_decoder_t *decoder);

/**
 * @brief Unload and free decoder resources
 *
 * @param decoder Pointer to decoder handle (will be set to NULL)
 */
void animation_decoder_unload(animation_decoder_t **decoder);

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_DECODER_H
