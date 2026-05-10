// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file jpeg_animation_decoder.c
 * @brief JPEG decoder using ESP32-P4 hardware JPEG engine with RGB888 output
 */

#include "animation_decoder.h"
#include "animation_decoder_internal.h"
#include "jpeg_decoder_internal.h"
#include "static_image_decoder_common.h"
#include "driver/jpeg_types.h"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>

#define TAG "jpeg_decoder"

// JPEG decoder implementation structure
typedef struct {
    jpeg_decoder_handle_t decoder_engine;
    const uint8_t *file_data;
    size_t file_size;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint8_t *rgb_buffer;      // RGB888 buffer from hardware decoder
    size_t rgb_buffer_size;
    uint32_t aligned_width;   // width padded up to 16 (hardware MCU alignment)
    bool initialized;
    uint32_t current_frame_delay_ms;
    jpeg_dec_output_format_t output_format;  // RGB888
} jpeg_decoder_data_t;

// Decode @p data via the ESP32-P4 HW JPEG peripheral, populating @p jd on
// success. On error, fully cleans up anything it allocated and returns the
// error - jd is left in its incoming state. Per-step failure logs are at
// DEBUG level because ANY HW error is recoverable via the SW fallback;
// the dispatcher logs a single WARN with the err code if the HW path fails.
static esp_err_t decode_via_hw(jpeg_decoder_data_t *jd, const uint8_t *data, size_t size)
{
    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };
    jpeg_decoder_handle_t engine = NULL;
    esp_err_t err = jpeg_new_decoder_engine(&decode_eng_cfg, &engine);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "jpeg_new_decoder_engine: %s", esp_err_to_name(err));
        return err;
    }

    jpeg_decode_picture_info_t info;
    err = jpeg_decoder_get_info(data, (uint32_t)size, &info);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "jpeg_decoder_get_info: %s", esp_err_to_name(err));
        jpeg_del_decoder_engine(engine);
        return err;
    }

    if (info.width == 0 || info.height == 0) {
        ESP_LOGD(TAG, "HW reports invalid dimensions: %ux%u",
                 (unsigned)info.width, (unsigned)info.height);
        jpeg_del_decoder_engine(engine);
        return ESP_ERR_INVALID_SIZE;
    }

    // HW decoder pads output dimensions up to 16-byte MCU boundaries; allocate
    // for the padded size or jpeg_decoder_process returns ESP_ERR_INVALID_ARG
    // on inputs that aren't already 16-aligned.
    const uint32_t aligned_w = (info.width  + 15u) & ~15u;
    const uint32_t aligned_h = (info.height + 15u) & ~15u;

    size_t rgb_buffer_size = (size_t)aligned_w * aligned_h * 3;
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated_size = 0;
    uint8_t *rgb_buffer = (uint8_t *)jpeg_alloc_decoder_mem(rgb_buffer_size, &mem_cfg, &allocated_size);
    if (!rgb_buffer) {
        ESP_LOGD(TAG, "jpeg_alloc_decoder_mem(%zu) failed", rgb_buffer_size);
        jpeg_del_decoder_engine(engine);
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
    };
    uint32_t out_size = 0;
    err = jpeg_decoder_process(engine, &decode_cfg,
                               (uint8_t *)data, (uint32_t)size,
                               rgb_buffer, (uint32_t)allocated_size,
                               &out_size);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "jpeg_decoder_process: %s", esp_err_to_name(err));
        free(rgb_buffer);
        jpeg_del_decoder_engine(engine);
        return err;
    }

    jd->decoder_engine = engine;
    jd->rgb_buffer = rgb_buffer;
    jd->rgb_buffer_size = allocated_size;
    jd->canvas_width = info.width;
    jd->canvas_height = info.height;
    jd->aligned_width = aligned_w;
    jd->output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    return ESP_OK;
}

// Decode @p data via libjpeg-turbo (SW), populating @p jd on success.
// SW output is contiguous, so aligned_width == canvas_width and the
// row-stripping path in jpeg_decoder_decode_next_rgb collapses to a
// single memcpy. canvas_width/height are the *scaled* dimensions; the
// loader reads those via animation_decoder_get_info to size buffers
// and upscale LUTs, so the rest of the pipeline is unaware of scaling.
static esp_err_t decode_via_sw(jpeg_decoder_data_t *jd, const uint8_t *data, size_t size)
{
    uint8_t *rgb_buffer = NULL;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    esp_err_t err = jpeg_decode_sw_to_rgb888(data, size, &rgb_buffer, &out_w, &out_h);
    if (err != ESP_OK) {
        return err;
    }
    jd->decoder_engine = NULL;
    jd->rgb_buffer = rgb_buffer;
    jd->rgb_buffer_size = (size_t)out_w * out_h * 3u;
    jd->canvas_width = out_w;
    jd->canvas_height = out_h;
    jd->aligned_width = out_w;
    jd->output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    return ESP_OK;
}

esp_err_t jpeg_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size)
{
    if (!decoder || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

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
    jpeg_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

    // HW first; on any error fall through to SW. The SW path handles every
    // file class the HW decoder rejects: progressive JPEGs (SOF2), files
    // where IDF v5.5.1's (W*H) % 8 SOF gate trips, oversized files whose
    // RGB output exceeds PSRAM, and corrupt-but-recoverable bitstreams.
    bool used_sw = false;
    esp_err_t err = decode_via_hw(jpeg_data, data, size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HW decode failed (%s); trying SW fallback (%zu-byte file)",
                 esp_err_to_name(err), size);
        err = decode_via_sw(jpeg_data, data, size);
        used_sw = true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed on both HW and SW paths: %s", esp_err_to_name(err));
        free(jpeg_data);
        return err;
    }

    jpeg_data->initialized = true;

    animation_decoder_t *dec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
    if (!dec) {
        ESP_LOGE(TAG, "Failed to allocate decoder");
        if (jpeg_data->rgb_buffer) {
            free(jpeg_data->rgb_buffer);
        }
        if (jpeg_data->decoder_engine) {
            jpeg_del_decoder_engine(jpeg_data->decoder_engine);
        }
        free(jpeg_data);
        return ESP_ERR_NO_MEM;
    }

    dec->type = ANIMATION_DECODER_TYPE_JPEG;
    dec->impl.jpeg.jpeg_decoder = jpeg_data;
    *decoder = dec;

    ESP_LOGI(TAG, "JPEG decoder initialized: %ux%u (route=%s)",
             (unsigned)jpeg_data->canvas_width,
             (unsigned)jpeg_data->canvas_height,
             used_sw ? "SW" : "HW");
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
    info->pixel_format = ANIMATION_PIXEL_FORMAT_RGB888;

    return ESP_OK;
}

esp_err_t jpeg_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    if (!decoder || !rgba_buffer || decoder->type != ANIMATION_DECODER_TYPE_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decoder_data_t *jpeg_data = (jpeg_decoder_data_t *)decoder->impl.jpeg.jpeg_decoder;
    if (!jpeg_data || !jpeg_data->initialized || !jpeg_data->rgb_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    // Convert RGB888 -> RGBA8888 on demand (legacy API).
    // Source rows are at aligned_width stride (hardware pads to MCU boundary);
    // only the first canvas_width pixels of each row are real image data.
    const uint32_t w = jpeg_data->canvas_width;
    const uint32_t h = jpeg_data->canvas_height;
    const size_t src_stride = (size_t)jpeg_data->aligned_width * 3;
    const uint8_t *src_row = jpeg_data->rgb_buffer;
    uint8_t *dst = rgba_buffer;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            dst[x * 4 + 0] = src_row[x * 3 + 0];
            dst[x * 4 + 1] = src_row[x * 3 + 1];
            dst[x * 4 + 2] = src_row[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
        src_row += src_stride;
        dst += (size_t)w * 4;
    }
    jpeg_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t jpeg_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer)
{
    if (!decoder || !rgb_buffer || decoder->type != ANIMATION_DECODER_TYPE_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decoder_data_t *jpeg_data = (jpeg_decoder_data_t *)decoder->impl.jpeg.jpeg_decoder;
    if (!jpeg_data || !jpeg_data->initialized || !jpeg_data->rgb_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    // Hardware buffer is padded to aligned_width; copy row-by-row to strip the
    // trailing padding columns when the JPEG width isn't 16-aligned.
    const uint32_t w = jpeg_data->canvas_width;
    const uint32_t h = jpeg_data->canvas_height;
    const size_t row_bytes = (size_t)w * 3U;
    const size_t src_stride = (size_t)jpeg_data->aligned_width * 3U;
    if (src_stride == row_bytes) {
        memcpy(rgb_buffer, jpeg_data->rgb_buffer, row_bytes * h);
    } else {
        const uint8_t *src = jpeg_data->rgb_buffer;
        uint8_t *dst = rgb_buffer;
        for (uint32_t y = 0; y < h; y++) {
            memcpy(dst, src, row_bytes);
            src += src_stride;
            dst += row_bytes;
        }
    }
    jpeg_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
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
    jpeg_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

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

