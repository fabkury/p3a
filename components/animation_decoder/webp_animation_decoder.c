/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "animation_decoder.h"
#include "animation_decoder_internal.h"
#include "static_image_decoder_common.h"
#include "config_store.h"
#include "webp/demux.h"
#include "webp/decode.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>

#define TAG "webp_decoder"

// WebP-specific structure to hold WebP types
typedef struct {
    WebPAnimDecoder *decoder;
    WebPAnimInfo info;
    int last_timestamp_ms;      // Previous frame timestamp for delay calculation
    uint32_t current_frame_delay_ms;  // Delay of the last decoded frame
    bool is_animation;
    bool has_alpha_any;         // Any alpha present in the file (init-time check)
    uint8_t *still_rgba;        // RGBA for still images with alpha
    size_t still_rgba_size;
    uint8_t *still_rgb;         // RGB for still images without alpha
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

esp_err_t animation_decoder_init(animation_decoder_t **decoder, animation_decoder_type_t type, const uint8_t *data, size_t size)
{
    if (!decoder || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (type == ANIMATION_DECODER_TYPE_WEBP) {
        animation_decoder_t *dec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
        if (!dec) {
            ESP_LOGE(TAG, "Failed to allocate decoder");
            return ESP_ERR_NO_MEM;
        }

        webp_decoder_data_t *webp_data = (webp_decoder_data_t *)calloc(1, sizeof(webp_decoder_data_t));
        if (!webp_data) {
            ESP_LOGE(TAG, "Failed to allocate WebP decoder data");
            free(dec);
            return ESP_ERR_NO_MEM;
        }

        dec->type = type;
        dec->impl.webp.data = data;
        dec->impl.webp.data_size = size;

        WebPBitstreamFeatures features;
        VP8StatusCode feature_status = WebPGetFeatures(data, size, &features);
        if (feature_status != VP8_STATUS_OK) {
            ESP_LOGE(TAG, "Failed to parse WebP features (status=%d)", feature_status);
            free(webp_data);
            free(dec);
            return ESP_FAIL;
        }

        if (features.width <= 0 || features.height <= 0) {
            ESP_LOGE(TAG, "Invalid WebP dimensions: %d x %d", features.width, features.height);
            free(webp_data);
            free(dec);
            return ESP_ERR_INVALID_SIZE;
        }

        webp_data->is_animation = (features.has_animation != 0);
        webp_data->has_alpha_any = (features.has_alpha != 0);

        if (webp_data->is_animation) {
            WebPAnimDecoderOptions dec_opts;
            if (!WebPAnimDecoderOptionsInit(&dec_opts)) {
                ESP_LOGE(TAG, "Failed to initialize WebP decoder options");
                free(webp_data);
                free(dec);
                return ESP_FAIL;
            }
            // NOTE: WebPAnimDecoder only supports RGBA-based modes (MODE_RGBA, MODE_BGRA,
            // MODE_rgbA, MODE_bgrA). MODE_RGB is NOT supported and causes buffer misreads.
            // Always use MODE_RGBA and convert to RGB in decode_next_rgb if needed.
            dec_opts.color_mode = MODE_RGBA;
            dec_opts.use_threads = 0;

            WebPData webp_data_wrapped = {
                .bytes = data,
                .size = size,
            };

            webp_data->decoder = WebPAnimDecoderNew(&webp_data_wrapped, &dec_opts);
            if (!webp_data->decoder) {
                ESP_LOGE(TAG, "Failed to create WebP animation decoder (file size: %zu bytes)", size);
                free(webp_data);
                free(dec);
                return ESP_FAIL;
            }

            if (!WebPAnimDecoderGetInfo(webp_data->decoder, &webp_data->info)) {
                ESP_LOGE(TAG, "Failed to query WebP animation info");
                WebPAnimDecoderDelete(webp_data->decoder);
                free(webp_data);
                free(dec);
                return ESP_FAIL;
            }

            if (webp_data->info.frame_count == 0 || webp_data->info.canvas_width == 0 || webp_data->info.canvas_height == 0) {
                ESP_LOGE(TAG, "Invalid WebP animation metadata");
                WebPAnimDecoderDelete(webp_data->decoder);
                free(webp_data);
                free(dec);
                return ESP_ERR_INVALID_SIZE;
            }

            webp_data->last_timestamp_ms = 0;
            webp_data->current_frame_delay_ms = 1;  // Default minimum delay
        } else {
            if (webp_data->has_alpha_any) {
                const size_t frame_size = (size_t)features.width * features.height * 4;
                webp_data->still_rgba = (uint8_t *)malloc(frame_size);
                if (!webp_data->still_rgba) {
                    ESP_LOGE(TAG, "Failed to allocate buffer for still WebP RGBA frame (%zu bytes)", frame_size);
                    free(webp_data);
                    free(dec);
                    return ESP_ERR_NO_MEM;
                }

                const int stride = features.width * 4;
                if (!WebPDecodeRGBAInto(data, size, webp_data->still_rgba, frame_size, stride)) {
                    ESP_LOGE(TAG, "Failed to decode still WebP image (RGBA)");
                    free(webp_data->still_rgba);
                    free(webp_data);
                    free(dec);
                    return ESP_FAIL;
                }
                webp_data->still_rgba_size = frame_size;
            } else {
                const size_t frame_size = (size_t)features.width * features.height * 3;
                webp_data->still_rgb = (uint8_t *)malloc(frame_size);
                if (!webp_data->still_rgb) {
                    ESP_LOGE(TAG, "Failed to allocate buffer for still WebP RGB frame (%zu bytes)", frame_size);
                    free(webp_data);
                    free(dec);
                    return ESP_ERR_NO_MEM;
                }

                const int stride = features.width * 3;
                if (!WebPDecodeRGBInto(data, size, webp_data->still_rgb, frame_size, stride)) {
                    ESP_LOGE(TAG, "Failed to decode still WebP image (RGB)");
                    free(webp_data->still_rgb);
                    free(webp_data);
                    free(dec);
                    return ESP_FAIL;
                }
                webp_data->still_rgb_size = frame_size;
            }

            webp_data->info.canvas_width = (uint32_t)features.width;
            webp_data->info.canvas_height = (uint32_t)features.height;
            webp_data->info.frame_count = 1;
            webp_data->info.loop_count = 0;
            webp_data->info.bgcolor = webp_data->has_alpha_any ? 0x00000000 : 0xFF000000;
            webp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
            webp_data->last_timestamp_ms = 0;
        }

        dec->impl.webp.decoder = webp_data;
        dec->impl.webp.initialized = true;
        *decoder = dec;

        return ESP_OK;
    } else if (type == ANIMATION_DECODER_TYPE_GIF) {
        return gif_decoder_init(decoder, data, size);
    } else if (type == ANIMATION_DECODER_TYPE_PNG) {
        return png_decoder_init(decoder, data, size);
    } else if (type == ANIMATION_DECODER_TYPE_JPEG) {
        return jpeg_decoder_init(decoder, data, size);
    } else {
        ESP_LOGE(TAG, "Unknown decoder type: %d", type);
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t animation_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info)
{
    if (!decoder || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decoder->type == ANIMATION_DECODER_TYPE_WEBP) {
        if (!decoder->impl.webp.initialized) {
            return ESP_ERR_INVALID_STATE;
        }

        webp_decoder_data_t *webp_data = (webp_decoder_data_t *)decoder->impl.webp.decoder;
        info->canvas_width = webp_data->info.canvas_width;
        info->canvas_height = webp_data->info.canvas_height;
        info->frame_count = webp_data->info.frame_count;
        info->has_transparency = webp_data->has_alpha_any;
        info->pixel_format = ANIMATION_PIXEL_FORMAT_RGB888;

        return ESP_OK;
    } else if (decoder->type == ANIMATION_DECODER_TYPE_GIF) {
        return gif_decoder_get_info(decoder, info);
    } else if (decoder->type == ANIMATION_DECODER_TYPE_PNG) {
        return png_decoder_get_info(decoder, info);
    } else if (decoder->type == ANIMATION_DECODER_TYPE_JPEG) {
        return jpeg_decoder_get_info_wrapper(decoder, info);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t animation_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    if (!decoder || !rgba_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decoder->type == ANIMATION_DECODER_TYPE_WEBP) {
        if (!decoder->impl.webp.initialized) {
            return ESP_ERR_INVALID_STATE;
        }

        webp_decoder_data_t *webp_data = (webp_decoder_data_t *)decoder->impl.webp.decoder;
        if (webp_data->is_animation) {
            uint8_t *frame_rgba = NULL;
            int timestamp_ms = 0;

            if (!WebPAnimDecoderGetNext(webp_data->decoder, &frame_rgba, &timestamp_ms)) {
                return ESP_ERR_INVALID_STATE;
            }

            if (!frame_rgba) {
                return ESP_FAIL;
            }

            // Calculate frame delay: WebP timestamps are cumulative, so delay = current - previous
            int frame_delay = timestamp_ms - webp_data->last_timestamp_ms;
            if (frame_delay < 1) {
                frame_delay = 1;  // Clamp to minimum 1 ms
            }
            webp_data->current_frame_delay_ms = (uint32_t)frame_delay;
            webp_data->last_timestamp_ms = timestamp_ms;

            // WebPAnimDecoder always outputs RGBA (4 bytes per pixel)
            const size_t frame_size = (size_t)webp_data->info.canvas_width * webp_data->info.canvas_height * 4;
            memcpy(rgba_buffer, frame_rgba, frame_size);
        } else {
            if (webp_data->has_alpha_any) {
                if (!webp_data->still_rgba || webp_data->still_rgba_size == 0) {
                    return ESP_ERR_INVALID_STATE;
                }
                memcpy(rgba_buffer, webp_data->still_rgba, webp_data->still_rgba_size);
            } else {
                if (!webp_data->still_rgb || webp_data->still_rgb_size == 0) {
                    return ESP_ERR_INVALID_STATE;
                }
                const size_t pixel_count = (size_t)webp_data->info.canvas_width * (size_t)webp_data->info.canvas_height;
                const uint8_t *rgb = webp_data->still_rgb;
                for (size_t i = 0; i < pixel_count; i++) {
                    rgba_buffer[i * 4 + 0] = rgb[i * 3 + 0];
                    rgba_buffer[i * 4 + 1] = rgb[i * 3 + 1];
                    rgba_buffer[i * 4 + 2] = rgb[i * 3 + 2];
                    rgba_buffer[i * 4 + 3] = 255;
                }
            }
            webp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
        }

        return ESP_OK;
    } else if (decoder->type == ANIMATION_DECODER_TYPE_GIF) {
        return gif_decoder_decode_next(decoder, rgba_buffer);
    } else if (decoder->type == ANIMATION_DECODER_TYPE_PNG) {
        return png_decoder_decode_next(decoder, rgba_buffer);
    } else if (decoder->type == ANIMATION_DECODER_TYPE_JPEG) {
        return jpeg_decoder_decode_next(decoder, rgba_buffer);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t animation_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer)
{
    if (!decoder || !rgb_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decoder->type == ANIMATION_DECODER_TYPE_WEBP) {
        if (!decoder->impl.webp.initialized) {
            return ESP_ERR_INVALID_STATE;
        }

        webp_decoder_data_t *webp_data = (webp_decoder_data_t *)decoder->impl.webp.decoder;
        if (webp_data->is_animation) {
            uint8_t *frame = NULL;
            int timestamp_ms = 0;
            if (!WebPAnimDecoderGetNext(webp_data->decoder, &frame, &timestamp_ms)) {
                return ESP_ERR_INVALID_STATE;
            }
            if (!frame) return ESP_FAIL;

            // Delay
            int frame_delay = timestamp_ms - webp_data->last_timestamp_ms;
            if (frame_delay < 1) frame_delay = 1;
            webp_data->current_frame_delay_ms = (uint32_t)frame_delay;
            webp_data->last_timestamp_ms = timestamp_ms;

            // WebPAnimDecoder always outputs RGBA (4 bytes per pixel), even for opaque animations.
            // Convert to RGB here.
            const size_t pixel_count = (size_t)webp_data->info.canvas_width * (size_t)webp_data->info.canvas_height;
            const uint8_t *src = frame;
            uint8_t *dst = rgb_buffer;

            if (!webp_data->has_alpha_any) {
                // Opaque animation: just copy RGB channels, skip alpha
                for (size_t i = 0; i < pixel_count; i++) {
                    dst[i * 3 + 0] = src[i * 4 + 0];
                    dst[i * 3 + 1] = src[i * 4 + 1];
                    dst[i * 3 + 2] = src[i * 4 + 2];
                }
                return ESP_OK;
            }

            // Animation with transparency: blend against background
            uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
            config_store_get_background_color(&bg_r, &bg_g, &bg_b);

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
                } else {
                    dst[i * 3 + 0] = blend_chan(r, bg_r, a);
                    dst[i * 3 + 1] = blend_chan(g, bg_g, a);
                    dst[i * 3 + 2] = blend_chan(b, bg_b, a);
                }
            }
            return ESP_OK;
        }

        // Still image
        if (!webp_data->has_alpha_any) {
            if (!webp_data->still_rgb || webp_data->still_rgb_size == 0) return ESP_ERR_INVALID_STATE;
            memcpy(rgb_buffer, webp_data->still_rgb, webp_data->still_rgb_size);
            webp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
            return ESP_OK;
        }

        if (!webp_data->still_rgba || webp_data->still_rgba_size == 0) return ESP_ERR_INVALID_STATE;

        uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
        config_store_get_background_color(&bg_r, &bg_g, &bg_b);

        const size_t pixel_count = (size_t)webp_data->info.canvas_width * (size_t)webp_data->info.canvas_height;
        const uint8_t *src = webp_data->still_rgba;
        uint8_t *dst = rgb_buffer;
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
            } else {
                dst[i * 3 + 0] = blend_chan(r, bg_r, a);
                dst[i * 3 + 1] = blend_chan(g, bg_g, a);
                dst[i * 3 + 2] = blend_chan(b, bg_b, a);
            }
        }
        webp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
        return ESP_OK;
    }

    if (decoder->type == ANIMATION_DECODER_TYPE_GIF) {
        return gif_decoder_decode_next_rgb(decoder, rgb_buffer);
    }
    if (decoder->type == ANIMATION_DECODER_TYPE_PNG) {
        return png_decoder_decode_next_rgb(decoder, rgb_buffer);
    }
    if (decoder->type == ANIMATION_DECODER_TYPE_JPEG) {
        return jpeg_decoder_decode_next_rgb(decoder, rgb_buffer);
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t animation_decoder_reset(animation_decoder_t *decoder)
{
    if (!decoder) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decoder->type == ANIMATION_DECODER_TYPE_WEBP) {
        if (!decoder->impl.webp.initialized) {
            return ESP_ERR_INVALID_STATE;
        }
        webp_decoder_data_t *webp_data = (webp_decoder_data_t *)decoder->impl.webp.decoder;
        if (webp_data->is_animation) {
            if (webp_data->decoder) {
                WebPAnimDecoderReset(webp_data->decoder);
            }
            webp_data->last_timestamp_ms = 0;
            webp_data->current_frame_delay_ms = 1;
        } else {
            // Static images simply reuse the pre-decoded frame
            webp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
        }
        return ESP_OK;
    } else if (decoder->type == ANIMATION_DECODER_TYPE_GIF) {
        return gif_decoder_reset(decoder);
    } else if (decoder->type == ANIMATION_DECODER_TYPE_PNG) {
        return png_decoder_reset(decoder);
    } else if (decoder->type == ANIMATION_DECODER_TYPE_JPEG) {
        return jpeg_decoder_reset(decoder);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t animation_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms)
{
    if (!decoder || !delay_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decoder->type == ANIMATION_DECODER_TYPE_WEBP) {
        if (!decoder->impl.webp.initialized) {
            return ESP_ERR_INVALID_STATE;
        }
        webp_decoder_data_t *webp_data = (webp_decoder_data_t *)decoder->impl.webp.decoder;
        *delay_ms = webp_data->current_frame_delay_ms;
        return ESP_OK;
    } else if (decoder->type == ANIMATION_DECODER_TYPE_GIF) {
        return gif_decoder_get_frame_delay(decoder, delay_ms);
    } else if (decoder->type == ANIMATION_DECODER_TYPE_PNG) {
        return png_decoder_get_frame_delay(decoder, delay_ms);
    } else if (decoder->type == ANIMATION_DECODER_TYPE_JPEG) {
        return jpeg_decoder_get_frame_delay(decoder, delay_ms);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
}

void animation_decoder_unload(animation_decoder_t **decoder)
{
    if (!decoder || !*decoder) {
        return;
    }

    animation_decoder_t *dec = *decoder;

    if (dec->type == ANIMATION_DECODER_TYPE_WEBP) {
        if (dec->impl.webp.decoder) {
            webp_decoder_data_t *webp_data = (webp_decoder_data_t *)dec->impl.webp.decoder;
            if (webp_data->decoder) {
                WebPAnimDecoderDelete(webp_data->decoder);
            }
            if (webp_data->still_rgba) {
                free(webp_data->still_rgba);
                webp_data->still_rgba = NULL;
            }
            if (webp_data->still_rgb) {
                free(webp_data->still_rgb);
                webp_data->still_rgb = NULL;
            }
            free(webp_data);
            dec->impl.webp.decoder = NULL;
        }
        free(dec);
    } else if (dec->type == ANIMATION_DECODER_TYPE_GIF) {
        gif_decoder_unload(decoder);
    } else if (dec->type == ANIMATION_DECODER_TYPE_PNG) {
        png_decoder_unload(decoder);
    } else if (dec->type == ANIMATION_DECODER_TYPE_JPEG) {
        jpeg_decoder_unload(decoder);
    } else {
        free(dec);
    }
    
    *decoder = NULL;
}

