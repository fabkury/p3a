/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "animation_decoder.h"
#include "animation_decoder_internal.h"
#include "png.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>

#define TAG "png_decoder"
#define PNG_STATIC_FRAME_DELAY_MS 100U

// Forward declarations for other decoders
extern esp_err_t gif_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t gif_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t gif_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t gif_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t gif_decoder_reset(animation_decoder_t *decoder);
extern void gif_decoder_unload(animation_decoder_t **decoder);

// PNG decoder implementation structure
typedef struct {
    const uint8_t *file_data;
    size_t file_size;
    size_t read_offset;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint8_t *rgba_buffer;
    size_t rgba_buffer_size;
    bool has_transparency;
    bool initialized;
    uint32_t current_frame_delay_ms;
} png_decoder_data_t;

// Custom read function for libpng to read from memory buffer
static void png_read_from_memory(png_structp png_ptr, png_bytep data, png_size_t length)
{
    png_decoder_data_t *png_data = (png_decoder_data_t *)png_get_io_ptr(png_ptr);
    if (!png_data || !png_data->file_data) {
        png_error(png_ptr, "Invalid PNG data pointer");
        return;
    }

    if (png_data->read_offset + length > png_data->file_size) {
        png_error(png_ptr, "Read beyond end of PNG data");
        return;
    }

    memcpy(data, png_data->file_data + png_data->read_offset, length);
    png_data->read_offset += length;
}

esp_err_t png_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size)
{
    if (!decoder || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Verify PNG signature
    if (size < 8 || png_sig_cmp((png_const_bytep)data, 0, 8) != 0) {
        ESP_LOGE(TAG, "Invalid PNG signature");
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)calloc(1, sizeof(png_decoder_data_t));
    if (!png_data) {
        ESP_LOGE(TAG, "Failed to allocate PNG decoder data");
        return ESP_ERR_NO_MEM;
    }

    png_data->file_data = data;
    png_data->file_size = size;
    png_data->read_offset = 0;
    png_data->current_frame_delay_ms = PNG_STATIC_FRAME_DELAY_MS;

    // Create PNG read structure
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG read structure");
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    // Create PNG info structure
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info structure");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    // Set error handling
    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decoding error");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(png_data);
        return ESP_FAIL;
    }

    // Set custom read function
    png_set_read_fn(png_ptr, png_data, png_read_from_memory);

    // Read PNG info
    png_read_info(png_ptr, info_ptr);

    // Get image dimensions
    png_uint_32 width = png_get_image_width(png_ptr, info_ptr);
    png_uint_32 height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    if (width == 0 || height == 0) {
        ESP_LOGE(TAG, "Invalid PNG dimensions: %u x %u", (unsigned)width, (unsigned)height);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(png_data);
        return ESP_ERR_INVALID_SIZE;
    }

    png_data->canvas_width = width;
    png_data->canvas_height = height;
    png_data->has_transparency = (color_type & PNG_COLOR_MASK_ALPHA) != 0 || png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS);

    // Transform to RGBA format
    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
        png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
    }

    // Update info after transformations
    png_read_update_info(png_ptr, info_ptr);

    // Allocate RGBA buffer
    png_data->rgba_buffer_size = (size_t)width * height * 4;
    png_data->rgba_buffer = (uint8_t *)malloc(png_data->rgba_buffer_size);
    if (!png_data->rgba_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGBA buffer (%zu bytes)", png_data->rgba_buffer_size);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    // Allocate row pointers
    png_bytep *row_pointers = (png_bytep *)malloc(height * sizeof(png_bytep));
    if (!row_pointers) {
        ESP_LOGE(TAG, "Failed to allocate row pointers");
        free(png_data->rgba_buffer);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    // Set up row pointers
    for (png_uint_32 y = 0; y < height; y++) {
        row_pointers[y] = png_data->rgba_buffer + (size_t)y * width * 4;
    }

    // Read image data
    png_read_image(png_ptr, row_pointers);
    png_read_end(png_ptr, NULL);

    // Clean up libpng structures
    free(row_pointers);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    png_data->initialized = true;

    // Create decoder structure
    animation_decoder_t *dec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
    if (!dec) {
        ESP_LOGE(TAG, "Failed to allocate decoder");
        free(png_data->rgba_buffer);
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    dec->type = ANIMATION_DECODER_TYPE_PNG;
    dec->impl.png.png_decoder = png_data;

    *decoder = dec;

    ESP_LOGI(TAG, "PNG decoder initialized: %ux%u, transparency=%d",
             (unsigned)png_data->canvas_width,
             (unsigned)png_data->canvas_height,
             png_data->has_transparency);

    return ESP_OK;
}

esp_err_t png_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info)
{
    if (!decoder || !info || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    info->canvas_width = png_data->canvas_width;
    info->canvas_height = png_data->canvas_height;
    info->frame_count = 1; // PNG is always single frame
    info->has_transparency = png_data->has_transparency;

    return ESP_OK;
}

esp_err_t png_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    if (!decoder || !rgba_buffer || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized || !png_data->rgba_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    // Copy pre-decoded frame
    memcpy(rgba_buffer, png_data->rgba_buffer, png_data->rgba_buffer_size);
    png_data->current_frame_delay_ms = PNG_STATIC_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t png_decoder_reset(animation_decoder_t *decoder)
{
    if (!decoder || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // PNG is static, so reset just restores the delay
    png_data->current_frame_delay_ms = PNG_STATIC_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t png_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms)
{
    if (!decoder || !delay_ms || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *delay_ms = png_data->current_frame_delay_ms;
    return ESP_OK;
}

void png_decoder_unload(animation_decoder_t **decoder)
{
    if (!decoder || !*decoder) {
        return;
    }

    animation_decoder_t *dec = *decoder;
    if (dec->type != ANIMATION_DECODER_TYPE_PNG) {
        return;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)dec->impl.png.png_decoder;
    if (png_data) {
        if (png_data->rgba_buffer) {
            free(png_data->rgba_buffer);
            png_data->rgba_buffer = NULL;
        }
        free(png_data);
    }

    free(dec);
    *decoder = NULL;
}

