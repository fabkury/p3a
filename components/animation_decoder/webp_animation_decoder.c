// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// WebP, still and animated, through libwebp.
//
// A still image is decoded whole at init. An animation is decoded a frame at
// a time by libwebp's WebPAnimDecoder, which keeps reading the file's bytes
// and composites each frame onto its own RGBA canvas.

#include <stdlib.h>
#include <string.h>

#include "animation_decoder.h"
#include "animation_decoder_formats.h"
#include "animation_decoder_internal.h"
#include "esp_err.h"
#include "esp_log.h"
#include "psram_alloc.h"
#include "webp/decode.h"
#include "webp/demux.h"

#define TAG "webp_decoder"

typedef struct {
    WebPAnimDecoder *decoder;
    WebPAnimInfo info;
    int last_timestamp_ms;            // of the previous frame; delays are differences
    uint32_t current_frame_delay_ms;  // of the frame last decoded
    bool is_animation;
    bool has_alpha_any;
    uint8_t *still_rgba;  // a still image with transparency, kept to re-blend when the background changes
    size_t still_rgba_size;
    uint8_t *still_rgb;   // a still image without; freed once handed over
    size_t still_rgb_size;
} webp_decoder_data_t;

static inline uint8_t div255_u16(uint16_t x)
{
    uint16_t t = (uint16_t)(x + 128);
    return (uint8_t)((t + (t >> 8)) >> 8);
}

static inline uint8_t blend_chan(uint8_t src, uint8_t bg, uint8_t a)
{
    const uint16_t inv = (uint16_t)(255U - (uint16_t)a);
    const uint16_t x = (uint16_t)src * (uint16_t)a + (uint16_t)bg * inv;
    return div255_u16(x);
}

// For a colour that already carries its alpha: src + bg * (1 - a). Only the
// background term is divided by 255. (1.x divided the sum, which threw the
// pixel's own colour away: soft edges in animated WebP came out as darkened
// background. Found by the decoder tests against Pillow.)
static inline uint8_t blend_chan_premul(uint8_t src_premul, uint8_t bg, uint8_t a)
{
    const uint16_t inv = (uint16_t)(255U - (uint16_t)a);
    const uint16_t x = (uint16_t)src_premul + div255_u16((uint16_t)((uint16_t)bg * inv));
    return (uint8_t)(x > 255 ? 255 : x);
}

static void free_data(webp_decoder_data_t *d)
{
    if (d == NULL) {
        return;
    }
    if (d->decoder != NULL) {
        WebPAnimDecoderDelete(d->decoder);
    }
    free(d->still_rgba);
    free(d->still_rgb);
    free(d);
}

static esp_err_t open_animation(webp_decoder_data_t *d, const uint8_t *data, size_t size)
{
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) {
        return ESP_FAIL;
    }
    // WebPAnimDecoder offers only the four RGBA orders; plain RGB is not one
    // of them. Premultiplied alpha saves a multiply per channel in the blend
    // below: src + bg * (255 - a) instead of src * a + bg * (255 - a).
    options.color_mode = MODE_rgbA;
    // libwebp's threads help only lossy frames at least 512 pixels wide.
    // p3a's content is mostly lossless and at most 256 by 256.
    options.use_threads = 0;

    const WebPData wrapped = {.bytes = data, .size = size};
    d->decoder = WebPAnimDecoderNew(&wrapped, &options);
    if (d->decoder == NULL) {
        ESP_LOGE(TAG, "could not open the animation (%u bytes)", (unsigned)size);
        return ESP_FAIL;
    }
    if (!WebPAnimDecoderGetInfo(d->decoder, &d->info)) {
        return ESP_FAIL;
    }
    if (d->info.frame_count == 0 || d->info.canvas_width == 0 || d->info.canvas_height == 0 ||
        d->info.canvas_width > ANIMATION_DECODER_MAX_DIMENSION ||
        d->info.canvas_height > ANIMATION_DECODER_MAX_DIMENSION) {
        ESP_LOGE(TAG, "unusable animation: %u frames, %u x %u", (unsigned)d->info.frame_count,
                 (unsigned)d->info.canvas_width, (unsigned)d->info.canvas_height);
        return ESP_ERR_INVALID_SIZE;
    }
    d->last_timestamp_ms = 0;
    d->current_frame_delay_ms = 1;
    return ESP_OK;
}

static esp_err_t decode_still(webp_decoder_data_t *d, const uint8_t *data, size_t size, int width, int height)
{
    const int channels = d->has_alpha_any ? 4 : 3;
    const size_t frame_size = (size_t)width * (size_t)height * (size_t)channels;
    uint8_t *pixels = psram_malloc(frame_size);
    if (pixels == NULL) {
        ESP_LOGE(TAG, "no memory for a %d x %d still (%u bytes)", width, height, (unsigned)frame_size);
        return ESP_ERR_NO_MEM;
    }
    const uint8_t *done = d->has_alpha_any ? WebPDecodeRGBAInto(data, size, pixels, frame_size, width * channels)
                                           : WebPDecodeRGBInto(data, size, pixels, frame_size, width * channels);
    if (done == NULL) {
        ESP_LOGE(TAG, "the still image did not decode");
        free(pixels);
        return ESP_FAIL;
    }
    if (d->has_alpha_any) {
        d->still_rgba = pixels;
        d->still_rgba_size = frame_size;
    } else {
        d->still_rgb = pixels;
        d->still_rgb_size = frame_size;
    }
    d->info.canvas_width = (uint32_t)width;
    d->info.canvas_height = (uint32_t)height;
    d->info.frame_count = 1;
    d->current_frame_delay_ms = ANIMATION_DECODER_STILL_FRAME_DELAY_MS;
    return ESP_OK;
}

esp_err_t webp_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size)
{
    WebPBitstreamFeatures features;
    VP8StatusCode status = WebPGetFeatures(data, size, &features);
    if (status != VP8_STATUS_OK) {
        ESP_LOGE(TAG, "not a readable WebP file (status %d)", status);
        return ESP_FAIL;
    }
    if (features.width <= 0 || features.height <= 0 || features.width > ANIMATION_DECODER_MAX_DIMENSION ||
        features.height > ANIMATION_DECODER_MAX_DIMENSION) {
        ESP_LOGE(TAG, "unusable dimensions: %d x %d", features.width, features.height);
        return ESP_ERR_INVALID_SIZE;
    }

    animation_decoder_t *dec = psram_calloc(1, sizeof(*dec));
    webp_decoder_data_t *d = psram_calloc(1, sizeof(*d));
    if (dec == NULL || d == NULL) {
        free(dec);
        free(d);
        return ESP_ERR_NO_MEM;
    }
    d->is_animation = features.has_animation != 0;
    d->has_alpha_any = features.has_alpha != 0;

    esp_err_t err = d->is_animation ? open_animation(d, data, size)
                                    : decode_still(d, data, size, features.width, features.height);
    if (err != ESP_OK) {
        free_data(d);
        free(dec);
        return err;
    }

    dec->type = ANIMATION_DECODER_TYPE_WEBP;
    dec->impl.webp.data = data;
    dec->impl.webp.data_size = size;
    dec->impl.webp.decoder = d;
    dec->impl.webp.initialized = true;
    *decoder = dec;
    return ESP_OK;
}

static webp_decoder_data_t *data_of(animation_decoder_t *decoder)
{
    return decoder->impl.webp.initialized ? (webp_decoder_data_t *)decoder->impl.webp.decoder : NULL;
}

esp_err_t webp_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info)
{
    webp_decoder_data_t *d = data_of(decoder);
    if (d == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    info->canvas_width = d->info.canvas_width;
    info->canvas_height = d->info.canvas_height;
    info->frame_count = d->info.frame_count;
    info->has_transparency = d->has_alpha_any;
    // A still was decoded whole at init. An animation keeps reading the file.
    info->source_consumed = !d->is_animation;
    return ESP_OK;
}

// Blends RGBA pixels onto the background into RGB. `premultiplied` says
// whether the colour channels already carry the alpha.
static void blend_onto_background(const uint8_t *src, uint8_t *dst, size_t pixel_count, bool premultiplied)
{
    uint8_t bg_r, bg_g, bg_b;
    animation_decoder_get_background(&bg_r, &bg_g, &bg_b);
    for (size_t i = 0; i < pixel_count; i++) {
        const uint8_t r = src[i * 4 + 0];
        const uint8_t g = src[i * 4 + 1];
        const uint8_t b = src[i * 4 + 2];
        const uint8_t a = src[i * 4 + 3];
        if (a == 255) {
            dst[i * 3 + 0] = r;
            dst[i * 3 + 1] = g;
            dst[i * 3 + 2] = b;
        } else if (a == 0) {
            dst[i * 3 + 0] = bg_r;
            dst[i * 3 + 1] = bg_g;
            dst[i * 3 + 2] = bg_b;
        } else if (premultiplied) {
            dst[i * 3 + 0] = blend_chan_premul(r, bg_r, a);
            dst[i * 3 + 1] = blend_chan_premul(g, bg_g, a);
            dst[i * 3 + 2] = blend_chan_premul(b, bg_b, a);
        } else {
            dst[i * 3 + 0] = blend_chan(r, bg_r, a);
            dst[i * 3 + 1] = blend_chan(g, bg_g, a);
            dst[i * 3 + 2] = blend_chan(b, bg_b, a);
        }
    }
}

esp_err_t webp_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer)
{
    webp_decoder_data_t *d = data_of(decoder);
    if (d == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t pixel_count = (size_t)d->info.canvas_width * (size_t)d->info.canvas_height;

    if (d->is_animation) {
        uint8_t *frame = NULL;
        int timestamp_ms = 0;
        if (!WebPAnimDecoderGetNext(d->decoder, &frame, &timestamp_ms)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (frame == NULL) {
            return ESP_FAIL;
        }
        // Timestamps are cumulative; a frame's delay is the difference.
        int delay = timestamp_ms - d->last_timestamp_ms;
        d->current_frame_delay_ms = (uint32_t)(delay < 1 ? 1 : delay);
        d->last_timestamp_ms = timestamp_ms;

        // The canvas is RGBA even for an opaque animation.
        if (d->has_alpha_any) {
            blend_onto_background(frame, rgb_buffer, pixel_count, true);
        } else {
            for (size_t i = 0; i < pixel_count; i++) {
                rgb_buffer[i * 3 + 0] = frame[i * 4 + 0];
                rgb_buffer[i * 3 + 1] = frame[i * 4 + 1];
                rgb_buffer[i * 3 + 2] = frame[i * 4 + 2];
            }
        }
        return ESP_OK;
    }

    d->current_frame_delay_ms = ANIMATION_DECODER_STILL_FRAME_DELAY_MS;
    if (d->has_alpha_any) {
        // Kept for as long as the decoder lives: the frame is asked for again
        // when the background colour changes, to be blended onto the new one.
        if (d->still_rgba == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        blend_onto_background(d->still_rgba, rgb_buffer, pixel_count, false);
        return ESP_OK;
    }
    // An opaque still is handed over once and never needed again: the caller
    // now holds the pixels, so the copy here is dead weight.
    if (d->still_rgb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(rgb_buffer, d->still_rgb, d->still_rgb_size);
    free(d->still_rgb);
    d->still_rgb = NULL;
    d->still_rgb_size = 0;
    return ESP_OK;
}

esp_err_t webp_decoder_reset(animation_decoder_t *decoder)
{
    webp_decoder_data_t *d = data_of(decoder);
    if (d == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (d->is_animation) {
        WebPAnimDecoderReset(d->decoder);
        d->last_timestamp_ms = 0;
        d->current_frame_delay_ms = 1;
    } else {
        d->current_frame_delay_ms = ANIMATION_DECODER_STILL_FRAME_DELAY_MS;
    }
    return ESP_OK;
}

esp_err_t webp_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms)
{
    webp_decoder_data_t *d = data_of(decoder);
    if (d == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *delay_ms = d->current_frame_delay_ms;
    return ESP_OK;
}

void webp_decoder_unload(animation_decoder_t **decoder)
{
    animation_decoder_t *dec = *decoder;
    free_data((webp_decoder_data_t *)dec->impl.webp.decoder);
    free(dec);
    *decoder = NULL;
}
