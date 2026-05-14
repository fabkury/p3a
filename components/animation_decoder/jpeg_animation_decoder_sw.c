// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file jpeg_animation_decoder_sw.c
 * @brief libjpeg-turbo software JPEG decoder fallback for the ESP32-P4 HW path.
 *
 * Used when the IDF v5.5.1 jpeg_driver hardware path returns any error:
 *   - progressive JPEGs (SOF2) the silicon cannot parse
 *   - files where (W*H) % 8 != 0, rejected at the IDF v5.5.1 SOF gate
 *   - files whose RGB output buffer would exceed PSRAM
 *   - any other malformed/edge-case file the HW driver refuses
 *
 * libjpeg-turbo's scaled IDCT lets us decode oversized sources directly to
 * a small intermediate by picking M/8 (M in 1..8) such that the decoded
 * dimensions are the smallest that remain >= the display panel on both
 * axes. The display pipeline takes it from there with hardware-accelerated
 * bilinear interpolation.
 *
 * libjpeg-turbo's default jerror.c handler calls exit() on fatal errors.
 * We replace it with a setjmp/longjmp pair so a corrupt JPEG returns
 * ESP_FAIL instead of taking the device down.
 */

#include "jpeg_decoder_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeglib.h"

#define TAG "jpeg_sw"

// Native panel resolution. The Waveshare ESP32-P4-WIFI6-Touch-LCD-4B is 720x720.
// The SW decoder picks the smallest scaled-IDCT ratio whose output remains at
// least this big on both axes; if a different panel is ever wired up, update
// this constant (or thread it through from display state).
#define DISPLAY_DIM_PX 720u

struct sw_jpeg_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void sw_jpeg_error_exit(j_common_ptr cinfo)
{
    struct sw_jpeg_error_mgr *err = (struct sw_jpeg_error_mgr *)cinfo->err;
    char buffer[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buffer);
    ESP_LOGE(TAG, "libjpeg-turbo: %s", buffer);
    longjmp(err->setjmp_buffer, 1);
}

static void sw_jpeg_output_message(j_common_ptr cinfo)
{
    char buffer[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buffer);
    ESP_LOGW(TAG, "libjpeg-turbo: %s", buffer);
}

// Pick the smallest M in {1..8} such that ceil(native * M / 8) >= DISPLAY_DIM_PX
// on BOTH axes. Falls back to M=8 (native) when at least one native axis is
// already smaller than the display - we can't downscale below the screen and
// still satisfy the rule, so let the display pipeline upscale from native.
static uint8_t pick_scale_num(uint32_t native_w, uint32_t native_h)
{
    if (native_w < DISPLAY_DIM_PX || native_h < DISPLAY_DIM_PX) {
        return 8;
    }
    for (uint8_t m = 1; m <= 8; m++) {
        const uint32_t scaled_w = (native_w * m + 7u) / 8u;
        const uint32_t scaled_h = (native_h * m + 7u) / 8u;
        if (scaled_w >= DISPLAY_DIM_PX && scaled_h >= DISPLAY_DIM_PX) {
            return m;
        }
    }
    return 8;  // unreachable when both native dims >= display
}

esp_err_t jpeg_decode_sw_to_rgb888(const uint8_t *data, size_t size,
                                   uint8_t **out_rgb,
                                   uint32_t *out_width,
                                   uint32_t *out_height)
{
    if (!data || !out_rgb || !out_width || !out_height || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct jpeg_decompress_struct cinfo;
    struct sw_jpeg_error_mgr jerr;
    // volatile: rgb_buffer is assigned after setjmp(); without volatile the
    // value seen by the longjmp cleanup path is indeterminate per C11 7.13.2.1.
    uint8_t * volatile rgb_buffer = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = sw_jpeg_error_exit;
    jerr.pub.output_message = sw_jpeg_output_message;

    if (setjmp(jerr.setjmp_buffer)) {
        // libjpeg-turbo signaled a fatal error via longjmp. cinfo may be in
        // any state from "allocated but uninitialised" to "mid-decompress";
        // jpeg_destroy_decompress is documented as safe in all of these.
        if (rgb_buffer) {
            free(rgb_buffer);
        }
        jpeg_destroy_decompress(&cinfo);
        return ESP_FAIL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, size);

    const int header_status = jpeg_read_header(&cinfo, TRUE);
    if (header_status != JPEG_HEADER_OK) {
        ESP_LOGE(TAG, "jpeg_read_header returned %d", header_status);
        jpeg_destroy_decompress(&cinfo);
        return ESP_FAIL;
    }

    const uint32_t native_w = cinfo.image_width;
    const uint32_t native_h = cinfo.image_height;
    const uint8_t scale_num = pick_scale_num(native_w, native_h);

    cinfo.scale_num = scale_num;
    cinfo.scale_denom = 8;
    cinfo.out_color_space = JCS_RGB;

    if (!jpeg_start_decompress(&cinfo)) {
        // Only happens with a suspending data source; jpeg_mem_src isn't one.
        ESP_LOGE(TAG, "jpeg_start_decompress refused to begin");
        jpeg_destroy_decompress(&cinfo);
        return ESP_FAIL;
    }

    const uint32_t out_w = cinfo.output_width;
    const uint32_t out_h = cinfo.output_height;
    const int components = cinfo.output_components;
    if (components != 3 || out_w == 0 || out_h == 0) {
        ESP_LOGE(TAG, "Unexpected output: %ux%u, %d components",
                 (unsigned)out_w, (unsigned)out_h, components);
        jpeg_destroy_decompress(&cinfo);
        return ESP_FAIL;
    }

    const size_t row_bytes = (size_t)out_w * 3u;
    const size_t buf_bytes = row_bytes * out_h;
    rgb_buffer = (uint8_t *)heap_caps_malloc(buf_bytes,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb_buffer) {
        rgb_buffer = (uint8_t *)malloc(buf_bytes);
    }
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate %zu-byte RGB output (scale=%u/8, %ux%u)",
                 buf_bytes, (unsigned)scale_num, (unsigned)out_w, (unsigned)out_h);
        jpeg_destroy_decompress(&cinfo);
        return ESP_ERR_NO_MEM;
    }

    // Yield every 32 scanlines: a full SW JPEG decode can hold a core for
    // seconds, and when this task lands on CPU 1 alongside the display
    // pipeline IDLE1 starves and the task watchdog fires.
    // See docs/cpu1-saturation-wdt-tabled.md.
    JSAMPROW row_pointer[1];
    while (cinfo.output_scanline < cinfo.output_height) {
        row_pointer[0] = rgb_buffer + (size_t)cinfo.output_scanline * row_bytes;
        if (jpeg_read_scanlines(&cinfo, row_pointer, 1) != 1) {
            // With jpeg_mem_src this should only happen if the data source
            // signalled suspension, which it doesn't. Defensive guard.
            ESP_LOGE(TAG, "jpeg_read_scanlines stalled at row %u/%u",
                     (unsigned)cinfo.output_scanline,
                     (unsigned)cinfo.output_height);
            free(rgb_buffer);
            jpeg_destroy_decompress(&cinfo);
            return ESP_FAIL;
        }
        if ((cinfo.output_scanline % 32u) == 0u) {
            vTaskDelay(1);
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *out_rgb = rgb_buffer;
    *out_width = out_w;
    *out_height = out_h;
    ESP_LOGD(TAG, "SW JPEG decoded: native=%ux%u scale=%u/8 out=%ux%u (%zu bytes)",
             (unsigned)native_w, (unsigned)native_h,
             (unsigned)scale_num,
             (unsigned)out_w, (unsigned)out_h,
             buf_bytes);
    return ESP_OK;
}
