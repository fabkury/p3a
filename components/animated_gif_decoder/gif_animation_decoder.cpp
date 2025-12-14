/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "animation_decoder.h"
#include "animation_decoder_internal.h"
#include "AnimatedGIF.h"
#include "esp_log.h"
#include "esp_err.h"
#include "config_store.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAG "gif_decoder"

struct gif_decoder_impl {
    AnimatedGIF *gif;
    uint32_t canvas_width;
    uint32_t canvas_height;
    size_t frame_count;
    size_t current_frame;
    bool initialized;
    const uint8_t *file_data;
    size_t file_size;
    uint32_t current_frame_delay_ms;  // Delay of the last decoded frame

    // Opaque canvas output (RGB888) - maintained across frames for GIF disposal modes
    uint8_t *canvas_rgb;
    size_t canvas_rgb_size;

    // Previous frame disposal info (applied BEFORE decoding the next frame)
    uint8_t prev_disposal_method;
    int prev_x, prev_y, prev_w, prev_h;
    bool have_prev_rect;

    // Current frame info (captured during decode via callback)
    uint8_t cur_disposal_method;
    int cur_x, cur_y, cur_w, cur_h;
    bool have_cur_rect;

    // Background color cache (runtime-configurable)
    uint8_t bg_r, bg_g, bg_b;
    uint32_t bg_generation;

    // Latched once we observe any transparent pixels in any frame.
    // Used to skip full-canvas clears for fully-opaque GIFs.
    bool has_transparency_any;

    // Loop boundary handling: when the decoder reaches the last frame, restart from a known baseline
    // on the NEXT decode so frame 0 does not inherit disposal state from the last frame.
    bool loop_restart_pending;
};

static inline void fill_rect_rgb(uint8_t *dst_rgb, int canvas_w,
                                 int x0, int y0, int w, int h,
                                 uint8_t r, uint8_t g, uint8_t b)
{
    if (!dst_rgb || canvas_w <= 0 || w <= 0 || h <= 0) return;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (w <= 0 || h <= 0) return;

    for (int y = 0; y < h; y++) {
        uint8_t *row = dst_rgb + ((size_t)(y0 + y) * (size_t)canvas_w + (size_t)x0) * 3U;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
        }
    }
}

static inline void refresh_bg(struct gif_decoder_impl *impl)
{
    if (!impl) return;
    const uint32_t gen = config_store_get_background_color_generation();
    if (impl->bg_generation != gen) {
        config_store_get_background_color(&impl->bg_r, &impl->bg_g, &impl->bg_b);
        impl->bg_generation = gen;
    }
}

static void apply_prev_disposal(struct gif_decoder_impl *impl)
{
    if (!impl || !impl->canvas_rgb) return;

    // Disposal method affects what happens AFTER a frame is displayed,
    // so we apply the previous frame's disposal BEFORE decoding the next frame.
    uint8_t d = impl->prev_disposal_method;
    if (d == 3) {
        // Not supported by design; treat as "restore to background"
        d = 2;
    }
    if (d == 2 && impl->have_prev_rect) {
        refresh_bg(impl);
        fill_rect_rgb(impl->canvas_rgb, (int)impl->canvas_width,
                      impl->prev_x, impl->prev_y, impl->prev_w, impl->prev_h,
                      impl->bg_r, impl->bg_g, impl->bg_b);
    }
}

// GIF draw callback - merges decoded pixels into the persistent RGB canvas
static void gif_draw_callback(GIFDRAW *pDraw)
{
    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)pDraw->pUser;
    if (!impl || !impl->canvas_rgb) {
        return;
    }

    const int canvas_w = (int)impl->canvas_width;
    const int y = pDraw->y;
    const int frame_x = pDraw->iX;
    const int frame_y = pDraw->iY;
    const int frame_w = pDraw->iWidth;

    // Get palette
    uint8_t *palette24 = pDraw->pPalette24;
    uint8_t transparent = pDraw->ucTransparent;
    const bool has_transparency = pDraw->ucHasTransparency;
    if (has_transparency) {
        impl->has_transparency_any = true;
    }

    // Capture current frame info (same for all scanlines of this frame)
    impl->cur_disposal_method = pDraw->ucDisposalMethod;
    impl->cur_x = frame_x;
    impl->cur_y = frame_y;
    impl->cur_w = pDraw->iWidth;
    impl->cur_h = pDraw->iHeight;
    impl->have_cur_rect = true;

    // Merge 8-bit indexed pixels to RGB canvas
    uint8_t *dst_row = impl->canvas_rgb + ((size_t)(frame_y + y) * (size_t)canvas_w + (size_t)frame_x) * 3U;

    // Yield periodically to prevent watchdog timeout (every 256 pixels)
    const int YIELD_INTERVAL = 256;
    if (!has_transparency) {
        for (int x = 0; x < frame_w; x++) {
            const uint8_t pixel_index = pDraw->pPixels[x];
            const uint8_t *palette_entry = palette24 + (size_t)pixel_index * 3U;
            uint8_t *dst = dst_row + (size_t)x * 3U;
            dst[0] = palette_entry[0]; // R
            dst[1] = palette_entry[1]; // G
            dst[2] = palette_entry[2]; // B

            if ((x % YIELD_INTERVAL) == (YIELD_INTERVAL - 1)) {
                taskYIELD();
            }
        }
    } else {
        for (int x = 0; x < frame_w; x++) {
            const uint8_t pixel_index = pDraw->pPixels[x];
            if (pixel_index != transparent) {
                const uint8_t *palette_entry = palette24 + (size_t)pixel_index * 3U;
                uint8_t *dst = dst_row + (size_t)x * 3U;
                dst[0] = palette_entry[0]; // R
                dst[1] = palette_entry[1]; // G
                dst[2] = palette_entry[2]; // B
            }
            // else: transparent pixel -> leave underlying canvas as-is

            if ((x % YIELD_INTERVAL) == (YIELD_INTERVAL - 1)) {
                taskYIELD();
            }
        }
    }
}

// Export functions for dispatcher
esp_err_t gif_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size)
{
    if (!decoder || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)calloc(1, sizeof(struct gif_decoder_impl));
    if (!impl) {
        ESP_LOGE(TAG, "Failed to allocate decoder impl");
        return ESP_ERR_NO_MEM;
    }

    impl->gif = new AnimatedGIF();
    if (!impl->gif) {
        ESP_LOGE(TAG, "Failed to allocate AnimatedGIF");
        free(impl);
        return ESP_ERR_NO_MEM;
    }

    impl->file_data = data;
    impl->file_size = size;

    // Initialize with RGB888 palette (we'll merge to RGB canvas in callback)
    // Must be called BEFORE open() to set up palette type.
    impl->gif->begin(GIF_PALETTE_RGB888);
    int begin_error = impl->gif->getLastError();
    if (begin_error != GIF_SUCCESS) {
        ESP_LOGE(TAG, "begin() failed with error: %d", begin_error);
        delete impl->gif;
        free(impl);
        return ESP_FAIL;
    }

    // Open GIF from memory
    // Note: open() returns 1 on success, 0 on failure (not GIF_SUCCESS which is 0)
    int result = impl->gif->open((uint8_t *)data, (int)size, gif_draw_callback);
    if (result == 0) {
        int last_error = impl->gif->getLastError();
        ESP_LOGE(TAG, "Failed to open GIF: error=%d", last_error);
        delete impl->gif;
        free(impl);
        return ESP_FAIL;
    }

    // Get canvas dimensions
    impl->canvas_width = (uint32_t)impl->gif->getCanvasWidth();
    impl->canvas_height = (uint32_t)impl->gif->getCanvasHeight();

    if (impl->canvas_width == 0 || impl->canvas_height == 0) {
        ESP_LOGE(TAG, "Invalid GIF dimensions");
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_INVALID_SIZE;
    }

    // Allocate RGB canvas buffer for full canvas
    impl->canvas_rgb_size = (size_t)impl->canvas_width * (size_t)impl->canvas_height * 3U;
    impl->canvas_rgb = (uint8_t *)malloc(impl->canvas_rgb_size);
    if (!impl->canvas_rgb) {
        ESP_LOGE(TAG, "Failed to allocate RGB canvas buffer");
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_NO_MEM;
    }

    GIFINFO gif_info = {0};
    int info_result = impl->gif->getInfo(&gif_info);
    if (info_result != 1) {
        ESP_LOGE(TAG, "Failed to read GIF metadata via getInfo()");
        free(impl->canvas_rgb);
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_INVALID_SIZE;
    }

    if (gif_info.iFrameCount <= 0) {
        ESP_LOGE(TAG, "GIF metadata reported zero frames");
        free(impl->canvas_rgb);
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_INVALID_SIZE;
    }

    impl->frame_count = (size_t)gif_info.iFrameCount;
    impl->gif->reset();

    // Initialize background and clear canvas
    impl->bg_generation = config_store_get_background_color_generation();
    config_store_get_background_color(&impl->bg_r, &impl->bg_g, &impl->bg_b);
    fill_rect_rgb(impl->canvas_rgb, (int)impl->canvas_width,
                  0, 0, (int)impl->canvas_width, (int)impl->canvas_height,
                  impl->bg_r, impl->bg_g, impl->bg_b);

    impl->prev_disposal_method = 0;
    impl->have_prev_rect = false;
    impl->have_cur_rect = false;
    impl->has_transparency_any = false;
    impl->loop_restart_pending = false;

    impl->current_frame = 0;
    impl->initialized = true;
    impl->current_frame_delay_ms = 1;  // Default minimum delay

    // Store impl pointer in decoder
    animation_decoder_t *dec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
    if (!dec) {
        ESP_LOGE(TAG, "Failed to allocate decoder");
        free(impl->canvas_rgb);
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_NO_MEM;
    }
    dec->type = ANIMATION_DECODER_TYPE_GIF;
    dec->impl.gif.gif_decoder = impl;

    *decoder = dec;

    return ESP_OK;
}

esp_err_t gif_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info)
{
    if (!decoder || !info || decoder->type != ANIMATION_DECODER_TYPE_GIF) {
        return ESP_ERR_INVALID_ARG;
    }

    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)decoder->impl.gif.gif_decoder;
    if (!impl || !impl->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    info->canvas_width = impl->canvas_width;
    info->canvas_height = impl->canvas_height;
    info->frame_count = impl->frame_count;
    info->has_transparency = true; // Conservative: GIFs may have transparency
    info->pixel_format = ANIMATION_PIXEL_FORMAT_RGB888;

    return ESP_OK;
}

static esp_err_t gif_decode_next_internal(struct gif_decoder_impl *impl)
{
    if (!impl || !impl->initialized || !impl->gif) {
        return ESP_ERR_INVALID_STATE;
    }

    // If we reached end-of-stream on the previous call, restart cleanly so frame 0
    // doesn't accidentally inherit disposal state from the last frame.
    if (impl->loop_restart_pending) {
        if (impl->has_transparency_any) {
            refresh_bg(impl);
            fill_rect_rgb(impl->canvas_rgb, (int)impl->canvas_width,
                          0, 0, (int)impl->canvas_width, (int)impl->canvas_height,
                          impl->bg_r, impl->bg_g, impl->bg_b);
        }
        impl->have_prev_rect = false;
        impl->prev_disposal_method = 0;
        impl->gif->reset();
        impl->current_frame = 0;
        impl->loop_restart_pending = false;
    }

    // Apply previous frame disposal before decoding the next frame.
    apply_prev_disposal(impl);

    // Reset current-frame capture
    impl->have_cur_rect = false;
    impl->cur_disposal_method = 0;

    int delay_ms = 0;
    int result = impl->gif->playFrame(false, &delay_ms, impl);
    // result==0 indicates end-of-stream (this was the last frame); we'll restart cleanly on the next decode.

    if (delay_ms < 1) delay_ms = 1;
    impl->current_frame_delay_ms = (uint32_t)delay_ms;

    // Promote current frame info to "previous" for next call
    if (impl->have_cur_rect) {
        impl->prev_disposal_method = impl->cur_disposal_method;
        impl->prev_x = impl->cur_x;
        impl->prev_y = impl->cur_y;
        impl->prev_w = impl->cur_w;
        impl->prev_h = impl->cur_h;
        impl->have_prev_rect = true;
    } else {
        // No draw callback triggered (empty frame); keep previous rect/disposal unchanged
    }

    impl->current_frame++;
    if (impl->current_frame >= impl->frame_count) {
        impl->current_frame = 0;
    }

    if (result == 0) {
        impl->loop_restart_pending = true;
    }

    return ESP_OK;
}

esp_err_t gif_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer)
{
    if (!decoder || !rgb_buffer || decoder->type != ANIMATION_DECODER_TYPE_GIF) {
        return ESP_ERR_INVALID_ARG;
    }

    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)decoder->impl.gif.gif_decoder;
    if (!impl || !impl->initialized || !impl->canvas_rgb) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gif_decode_next_internal(impl);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(rgb_buffer, impl->canvas_rgb, impl->canvas_rgb_size);
    return ESP_OK;
}

esp_err_t gif_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    if (!decoder || !rgba_buffer || decoder->type != ANIMATION_DECODER_TYPE_GIF) {
        return ESP_ERR_INVALID_ARG;
    }

    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)decoder->impl.gif.gif_decoder;
    if (!impl || !impl->initialized || !impl->canvas_rgb) {
        return ESP_ERR_INVALID_STATE;
    }

    // Legacy API: decode and expand RGB canvas -> RGBA output
    esp_err_t err = gif_decode_next_internal(impl);
    if (err != ESP_OK) return err;

    const size_t pixel_count = (size_t)impl->canvas_width * (size_t)impl->canvas_height;
    const uint8_t *src = impl->canvas_rgb;
    for (size_t i = 0; i < pixel_count; i++) {
        rgba_buffer[i * 4 + 0] = src[i * 3 + 0];
        rgba_buffer[i * 4 + 1] = src[i * 3 + 1];
        rgba_buffer[i * 4 + 2] = src[i * 3 + 2];
        rgba_buffer[i * 4 + 3] = 255;
    }
    return ESP_OK;
}

esp_err_t gif_decoder_reset(animation_decoder_t *decoder)
{
    if (!decoder || decoder->type != ANIMATION_DECODER_TYPE_GIF) {
        return ESP_ERR_INVALID_ARG;
    }

    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)decoder->impl.gif.gif_decoder;
    if (!impl || !impl->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    impl->gif->reset();
    impl->current_frame = 0;
    impl->current_frame_delay_ms = 1;  // Reset timing state
    refresh_bg(impl);
    fill_rect_rgb(impl->canvas_rgb, (int)impl->canvas_width,
                  0, 0, (int)impl->canvas_width, (int)impl->canvas_height,
                  impl->bg_r, impl->bg_g, impl->bg_b);
    impl->have_prev_rect = false;
    impl->prev_disposal_method = 0;
    impl->loop_restart_pending = false;
    impl->has_transparency_any = false;
    return ESP_OK;
}

esp_err_t gif_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms)
{
    if (!decoder || !delay_ms || decoder->type != ANIMATION_DECODER_TYPE_GIF) {
        return ESP_ERR_INVALID_ARG;
    }

    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)decoder->impl.gif.gif_decoder;
    if (!impl || !impl->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *delay_ms = impl->current_frame_delay_ms;
    return ESP_OK;
}

void gif_decoder_unload(animation_decoder_t **decoder)
{
    if (!decoder || !*decoder) {
        return;
    }

    animation_decoder_t *dec = *decoder;
    if (dec->type != ANIMATION_DECODER_TYPE_GIF) {
        return;
    }

    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)dec->impl.gif.gif_decoder;
    if (impl) {
        if (impl->gif) {
            impl->gif->close();
            delete impl->gif;
            impl->gif = NULL;
        }

        if (impl->canvas_rgb) {
            free(impl->canvas_rgb);
            impl->canvas_rgb = NULL;
        }

        free(impl);
    }

    free(dec);
    *decoder = NULL;
}

#ifdef __cplusplus
}
#endif
