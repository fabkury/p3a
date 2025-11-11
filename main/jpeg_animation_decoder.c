/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "animation_decoder.h"
#include "animation_decoder_internal.h"
#include "driver/jpeg_types.h"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>

#define TAG "jpeg_decoder"
#define JPEG_STATIC_FRAME_DELAY_MS 100U

// Forward declarations for other decoders
extern esp_err_t gif_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t gif_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t gif_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t gif_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t gif_decoder_reset(animation_decoder_t *decoder);
extern void gif_decoder_unload(animation_decoder_t **decoder);

extern esp_err_t png_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t png_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t png_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t png_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t png_decoder_reset(animation_decoder_t *decoder);
extern void png_decoder_unload(animation_decoder_t **decoder);

// JPEG decoder implementation structure
typedef struct {
    jpeg_decoder_handle_t decoder_engine;
    const uint8_t *file_data;
    size_t file_size;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint8_t *rgb_buffer;      // RGB888 buffer from hardware decoder
    size_t rgb_buffer_size;
    uint8_t *rgba_buffer;     // RGBA buffer for output (converted from RGB)
    size_t rgba_buffer_size;
    bool initialized;
    uint32_t current_frame_delay_ms;
    jpeg_dec_output_format_t output_format;  // RGB888 or RGB565
} jpeg_decoder_data_t;

esp_err_t jpeg_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size)
{
    if (!decoder || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Verify JPEG signature (starts with FF D8)
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPEG signature");
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decoder_data_t *jpeg_data = (jpeg_decoder_data_t *)calloc(1, sizeof(jpeg_decoder_data_t));
    if (!jpeg_data) {
        ESP_LOGE(TAG, "Failed to allocate JPEG decoder data");
        return ESP_ERR_NO_MEM;
    }

    jpeg_data->file_data = data;
    jpeg_data->file_size = size;
    jpeg_data->current_frame_delay_ms = JPEG_STATIC_FRAME_DELAY_MS;

    // Configure decoder engine
    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,  // Reasonable timeout for decoding
    };

    esp_err_t err = jpeg_new_decoder_engine(&decode_eng_cfg, &jpeg_data->decoder_engine);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder engine: %s", esp_err_to_name(err));
        free(jpeg_data);
        return err;
    }

    // Get JPEG image info first (this function doesn't need decoder engine)
    jpeg_decode_picture_info_t info;
    err = jpeg_decoder_get_info(data, (uint32_t)size, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get JPEG info: %s", esp_err_to_name(err));
        jpeg_del_decoder_engine(jpeg_data->decoder_engine);
        free(jpeg_data);
        return err;
    }

    if (info.width == 0 || info.height == 0) {
        ESP_LOGE(TAG, "Invalid JPEG dimensions: %u x %u", (unsigned)info.width, (unsigned)info.height);
        jpeg_del_decoder_engine(jpeg_data->decoder_engine);
        free(jpeg_data);
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_data->canvas_width = info.width;
    jpeg_data->canvas_height = info.height;

    // Determine output format based on LCD configuration
    // JPEG hardware can output RGB888 or RGB565
    // We'll use RGB888 and convert to RGBA for the interface
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
    jpeg_data->output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    jpeg_data->rgb_buffer_size = (size_t)info.width * info.height * 2;  // RGB565 = 2 bytes per pixel
#else
    jpeg_data->output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    jpeg_data->rgb_buffer_size = (size_t)info.width * info.height * 3;  // RGB888 = 3 bytes per pixel
#endif

    // Allocate RGB buffer for hardware decoder output
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated_size = 0;
    jpeg_data->rgb_buffer = (uint8_t *)jpeg_alloc_decoder_mem(jpeg_data->rgb_buffer_size, &mem_cfg, &allocated_size);
    if (!jpeg_data->rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer (%zu bytes)", jpeg_data->rgb_buffer_size);
        jpeg_del_decoder_engine(jpeg_data->decoder_engine);
        free(jpeg_data);
        return ESP_ERR_NO_MEM;
    }
    jpeg_data->rgb_buffer_size = allocated_size;  // Use actual allocated size

    // Allocate RGBA buffer for output (always RGBA for interface compatibility)
    jpeg_data->rgba_buffer_size = (size_t)info.width * info.height * 4;
    jpeg_data->rgba_buffer = (uint8_t *)malloc(jpeg_data->rgba_buffer_size);
    if (!jpeg_data->rgba_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGBA buffer (%zu bytes)", jpeg_data->rgba_buffer_size);
        free(jpeg_data->rgb_buffer);
        jpeg_del_decoder_engine(jpeg_data->decoder_engine);
        free(jpeg_data);
        return ESP_ERR_NO_MEM;
    }

    // Configure decode parameters
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = jpeg_data->output_format,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
    };

    // Decode JPEG image
    uint32_t out_size = 0;
    err = jpeg_decoder_process(jpeg_data->decoder_engine, &decode_cfg, 
                               (uint8_t *)data, (uint32_t)size,
                               jpeg_data->rgb_buffer, (uint32_t)jpeg_data->rgb_buffer_size,
                               &out_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode JPEG: %s", esp_err_to_name(err));
        free(jpeg_data->rgba_buffer);
        free(jpeg_data->rgb_buffer);
        jpeg_del_decoder_engine(jpeg_data->decoder_engine);
        free(jpeg_data);
        return err;
    }

    // Convert RGB to RGBA
    const size_t pixel_count = (size_t)info.width * info.height;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
    // Convert RGB565 to RGBA8888
    uint16_t *rgb565_src = (uint16_t *)jpeg_data->rgb_buffer;
    uint8_t *rgba_dst = jpeg_data->rgba_buffer;
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = rgb565_src[i];
        // Extract RGB565 components
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;
        // Expand to full 8-bit range
        r |= r >> 5;
        g |= g >> 6;
        b |= b >> 5;
        // Store as RGBA
        rgba_dst[i * 4 + 0] = r;
        rgba_dst[i * 4 + 1] = g;
        rgba_dst[i * 4 + 2] = b;
        rgba_dst[i * 4 + 3] = 255;  // Alpha = opaque
    }
#else
    // Convert RGB888 to RGBA8888
    uint8_t *rgb_src = jpeg_data->rgb_buffer;
    uint8_t *rgba_dst = jpeg_data->rgba_buffer;
    for (size_t i = 0; i < pixel_count; i++) {
        rgba_dst[i * 4 + 0] = rgb_src[i * 3 + 0];  // R
        rgba_dst[i * 4 + 1] = rgb_src[i * 3 + 1];  // G
        rgba_dst[i * 4 + 2] = rgb_src[i * 3 + 2];  // B
        rgba_dst[i * 4 + 3] = 255;  // Alpha = opaque
    }
#endif

    jpeg_data->initialized = true;

    // Create decoder structure
    animation_decoder_t *dec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
    if (!dec) {
        ESP_LOGE(TAG, "Failed to allocate decoder");
        free(jpeg_data->rgba_buffer);
        free(jpeg_data->rgb_buffer);
        jpeg_del_decoder_engine(jpeg_data->decoder_engine);
        free(jpeg_data);
        return ESP_ERR_NO_MEM;
    }

    dec->type = ANIMATION_DECODER_TYPE_JPEG;
    dec->impl.jpeg.jpeg_decoder = jpeg_data;

    *decoder = dec;

    ESP_LOGI(TAG, "JPEG decoder initialized: %ux%u (hardware accelerated)",
             (unsigned)jpeg_data->canvas_width,
             (unsigned)jpeg_data->canvas_height);

    return ESP_OK;
}

esp_err_t jpeg_decoder_get_info_wrapper(animation_decoder_t *decoder, animation_decoder_info_t *info)
{
    if (!decoder || !info || decoder->type != ANIMATION_DECODER_TYPE_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decoder_data_t *jpeg_data = (jpeg_decoder_data_t *)decoder->impl.jpeg.jpeg_decoder;
    if (!jpeg_data || !jpeg_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    info->canvas_width = jpeg_data->canvas_width;
    info->canvas_height = jpeg_data->canvas_height;
    info->frame_count = 1; // JPEG is always single frame
    info->has_transparency = false; // JPEG doesn't support transparency

    return ESP_OK;
}

esp_err_t jpeg_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    if (!decoder || !rgba_buffer || decoder->type != ANIMATION_DECODER_TYPE_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decoder_data_t *jpeg_data = (jpeg_decoder_data_t *)decoder->impl.jpeg.jpeg_decoder;
    if (!jpeg_data || !jpeg_data->initialized || !jpeg_data->rgba_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    // Copy pre-decoded RGBA frame
    memcpy(rgba_buffer, jpeg_data->rgba_buffer, jpeg_data->rgba_buffer_size);
    jpeg_data->current_frame_delay_ms = JPEG_STATIC_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t jpeg_decoder_reset(animation_decoder_t *decoder)
{
    if (!decoder || decoder->type != ANIMATION_DECODER_TYPE_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decoder_data_t *jpeg_data = (jpeg_decoder_data_t *)decoder->impl.jpeg.jpeg_decoder;
    if (!jpeg_data || !jpeg_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // JPEG is static, so reset just restores the delay
    jpeg_data->current_frame_delay_ms = JPEG_STATIC_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t jpeg_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms)
{
    if (!decoder || !delay_ms || decoder->type != ANIMATION_DECODER_TYPE_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decoder_data_t *jpeg_data = (jpeg_decoder_data_t *)decoder->impl.jpeg.jpeg_decoder;
    if (!jpeg_data || !jpeg_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *delay_ms = jpeg_data->current_frame_delay_ms;
    return ESP_OK;
}

void jpeg_decoder_unload(animation_decoder_t **decoder)
{
    if (!decoder || !*decoder) {
        return;
    }

    animation_decoder_t *dec = *decoder;
    if (dec->type != ANIMATION_DECODER_TYPE_JPEG) {
        return;
    }

    jpeg_decoder_data_t *jpeg_data = (jpeg_decoder_data_t *)dec->impl.jpeg.jpeg_decoder;
    if (jpeg_data) {
        if (jpeg_data->rgba_buffer) {
            free(jpeg_data->rgba_buffer);
            jpeg_data->rgba_buffer = NULL;
        }
        if (jpeg_data->rgb_buffer) {
            free(jpeg_data->rgb_buffer);
            jpeg_data->rgb_buffer = NULL;
        }
        if (jpeg_data->decoder_engine) {
            jpeg_del_decoder_engine(jpeg_data->decoder_engine);
            jpeg_data->decoder_engine = NULL;
        }
        free(jpeg_data);
    }

    free(dec);
    *decoder = NULL;
}

