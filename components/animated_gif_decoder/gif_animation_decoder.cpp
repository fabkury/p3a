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
    uint8_t *rgba_buffer;
    uint32_t canvas_width;
    uint32_t canvas_height;
    size_t frame_count;
    size_t current_frame;
    bool initialized;
    const uint8_t *file_data;
    size_t file_size;
    uint8_t *previous_frame; // For disposal method handling
    uint32_t current_frame_delay_ms;  // Delay of the last decoded frame
};

// GIF draw callback - converts GIF pixels to RGBA
static void gif_draw_callback(GIFDRAW *pDraw)
{
    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)pDraw->pUser;
    if (!impl || !impl->rgba_buffer) {
        return;
    }

    const int canvas_w = (int)impl->canvas_width;
    const int y = pDraw->y;
    const int frame_x = pDraw->iX;
    const int frame_y = pDraw->iY;
    const int frame_w = pDraw->iWidth;

    // Calculate destination row in RGBA buffer
    uint8_t *dst_row = impl->rgba_buffer + (size_t)(frame_y + y) * canvas_w * 4;

    // Get palette
    uint8_t *palette24 = pDraw->pPalette24;
    uint8_t transparent = pDraw->ucTransparent;
    bool has_transparency = pDraw->ucHasTransparency;

    // Convert 8-bit indexed pixels to RGBA
    // Yield periodically to prevent watchdog timeout (every 32 pixels)
    const int YIELD_INTERVAL = 32;
    for (int x = 0; x < frame_w; x++) {
        uint8_t pixel_index = pDraw->pPixels[x];
        uint8_t *dst_pixel = dst_row + (size_t)(frame_x + x) * 4;

        if (has_transparency && pixel_index == transparent) {
            // Transparent pixel - keep previous frame pixel if available
            if (impl->previous_frame) {
                memcpy(dst_pixel, impl->previous_frame + (size_t)(frame_y + y) * canvas_w * 4 + (size_t)(frame_x + x) * 4, 4);
            } else {
                dst_pixel[0] = 0; // R
                dst_pixel[1] = 0; // G
                dst_pixel[2] = 0; // B
                dst_pixel[3] = 0; // A
            }
        } else {
            // Opaque pixel from palette
            uint8_t *palette_entry = palette24 + pixel_index * 3;
            dst_pixel[0] = palette_entry[0]; // R
            dst_pixel[1] = palette_entry[1]; // G
            dst_pixel[2] = palette_entry[2]; // B
            dst_pixel[3] = 255; // A
        }
        
        // Yield periodically to allow other tasks (including idle task) to run
        if ((x % YIELD_INTERVAL) == (YIELD_INTERVAL - 1)) {
            taskYIELD();
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

    // Initialize with RGB888 palette (we'll convert to RGBA in callback)
    // This must be called BEFORE open() to set up the palette type
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

    // Allocate RGBA buffer for full canvas
    size_t rgba_size = (size_t)impl->canvas_width * impl->canvas_height * 4;
    impl->rgba_buffer = (uint8_t *)malloc(rgba_size);
    if (!impl->rgba_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGBA buffer");
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_NO_MEM;
    }

    // Allocate previous frame buffer for disposal method handling
    impl->previous_frame = (uint8_t *)malloc(rgba_size);
    if (!impl->previous_frame) {
        ESP_LOGE(TAG, "Failed to allocate previous frame buffer");
        free(impl->rgba_buffer);
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_NO_MEM;
    }
    memset(impl->previous_frame, 0, rgba_size);

    GIFINFO gif_info = {0};
    int info_result = impl->gif->getInfo(&gif_info);
    if (info_result != 1) {
        ESP_LOGE(TAG, "Failed to read GIF metadata via getInfo()");
        free(impl->rgba_buffer);
        free(impl->previous_frame);
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_INVALID_SIZE;
    }

    if (gif_info.iFrameCount <= 0) {
        ESP_LOGE(TAG, "GIF metadata reported zero frames");
        free(impl->rgba_buffer);
        free(impl->previous_frame);
        impl->gif->close();
        delete impl->gif;
        free(impl);
        return ESP_ERR_INVALID_SIZE;
    }

    impl->frame_count = (size_t)gif_info.iFrameCount;
    impl->gif->reset();

    // Ensure decode buffers start cleared before first frame decode
    memset(impl->rgba_buffer, 0, rgba_size);

    impl->current_frame = 0;
    impl->initialized = true;
    impl->current_frame_delay_ms = 1;  // Default minimum delay

    // Store impl pointer in decoder
    animation_decoder_t *dec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
    if (!dec) {
        ESP_LOGE(TAG, "Failed to allocate decoder");
        free(impl->rgba_buffer);
        free(impl->previous_frame);
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
    info->has_transparency = true; // GIFs can have transparency

    return ESP_OK;
}

esp_err_t gif_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    if (!decoder || !rgba_buffer || decoder->type != ANIMATION_DECODER_TYPE_GIF) {
        return ESP_ERR_INVALID_ARG;
    }

    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)decoder->impl.gif.gif_decoder;
    if (!impl || !impl->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Copy previous frame for disposal method handling
    size_t rgba_size = (size_t)impl->canvas_width * impl->canvas_height * 4;
    if (impl->previous_frame) {
        memcpy(impl->previous_frame, impl->rgba_buffer, rgba_size);
    }

    // Clear the RGBA buffer first
    memset(impl->rgba_buffer, 0, rgba_size);

    // Set user data for callback
    // Decode next frame
    int delay_ms = 0;
    int result = impl->gif->playFrame(false, &delay_ms, impl);
    
    if (result < 0) {
        // Error or end of animation
        return ESP_ERR_INVALID_STATE;
    }

    // Store frame delay, clamping to minimum 1 ms
    if (delay_ms < 1) {
        delay_ms = 1;
    }
    impl->current_frame_delay_ms = (uint32_t)delay_ms;

    // Copy from internal buffer to output buffer
    memcpy(rgba_buffer, impl->rgba_buffer, rgba_size);

    impl->current_frame++;
    if (impl->current_frame >= impl->frame_count) {
        impl->current_frame = 0;
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
    if (impl->previous_frame) {
        size_t rgba_size = (size_t)impl->canvas_width * impl->canvas_height * 4;
        memset(impl->previous_frame, 0, rgba_size);
    }
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

        if (impl->rgba_buffer) {
            free(impl->rgba_buffer);
            impl->rgba_buffer = NULL;
        }

        if (impl->previous_frame) {
            free(impl->previous_frame);
            impl->previous_frame = NULL;
        }

        free(impl);
    }

    free(dec);
    *decoder = NULL;
}

#ifdef __cplusplus
}
#endif
