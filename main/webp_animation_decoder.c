/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "animation_decoder.h"
#include "animation_decoder_internal.h"
#include "webp/demux.h"
#include "webp/decode.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>

#define TAG "webp_decoder"
#define WEBP_STATIC_FRAME_DELAY_MS 100U

// Forward declarations for GIF decoder
extern esp_err_t gif_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t gif_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t gif_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t gif_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t gif_decoder_reset(animation_decoder_t *decoder);
extern void gif_decoder_unload(animation_decoder_t **decoder);

// Forward declarations for PNG decoder
extern esp_err_t png_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t png_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t png_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t png_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t png_decoder_reset(animation_decoder_t *decoder);
extern void png_decoder_unload(animation_decoder_t **decoder);

// Forward declarations for JPEG decoder
extern esp_err_t jpeg_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size);
extern esp_err_t jpeg_decoder_get_info_wrapper(animation_decoder_t *decoder, animation_decoder_info_t *info);
extern esp_err_t jpeg_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer);
extern esp_err_t jpeg_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);
extern esp_err_t jpeg_decoder_reset(animation_decoder_t *decoder);
extern void jpeg_decoder_unload(animation_decoder_t **decoder);

// WebP-specific structure to hold WebP types
typedef struct {
    WebPAnimDecoder *decoder;
    WebPAnimInfo info;
    int last_timestamp_ms;      // Previous frame timestamp for delay calculation
    uint32_t current_frame_delay_ms;  // Delay of the last decoded frame
    bool is_animation;
    uint8_t *still_rgba;
    size_t still_frame_size;
    bool still_has_alpha;
} webp_decoder_data_t;

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

        if (webp_data->is_animation) {
            WebPAnimDecoderOptions dec_opts;
            if (!WebPAnimDecoderOptionsInit(&dec_opts)) {
                ESP_LOGE(TAG, "Failed to initialize WebP decoder options");
                free(webp_data);
                free(dec);
                return ESP_FAIL;
            }
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
            const size_t frame_size = (size_t)features.width * features.height * 4;
            webp_data->still_rgba = (uint8_t *)malloc(frame_size);
            if (!webp_data->still_rgba) {
                ESP_LOGE(TAG, "Failed to allocate buffer for still WebP frame (%zu bytes)", frame_size);
                free(webp_data);
                free(dec);
                return ESP_ERR_NO_MEM;
            }

            const int stride = features.width * 4;
            if (!WebPDecodeRGBAInto(data, size, webp_data->still_rgba, frame_size, stride)) {
                ESP_LOGE(TAG, "Failed to decode still WebP image");
                free(webp_data->still_rgba);
                free(webp_data);
                free(dec);
                return ESP_FAIL;
            }

            webp_data->info.canvas_width = (uint32_t)features.width;
            webp_data->info.canvas_height = (uint32_t)features.height;
            webp_data->info.frame_count = 1;
            webp_data->info.loop_count = 0;
            webp_data->info.bgcolor = features.has_alpha ? 0x00000000 : 0xFF000000;
            webp_data->still_has_alpha = (features.has_alpha != 0);
            webp_data->still_frame_size = frame_size;
            webp_data->current_frame_delay_ms = WEBP_STATIC_FRAME_DELAY_MS;
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
        if (webp_data->is_animation) {
            info->has_transparency = (webp_data->info.bgcolor & 0xff000000) == 0;
        } else {
            info->has_transparency = webp_data->still_has_alpha;
        }

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

            const size_t frame_size = (size_t)webp_data->info.canvas_width * webp_data->info.canvas_height * 4;
            memcpy(rgba_buffer, frame_rgba, frame_size);
        } else {
            if (!webp_data->still_rgba || webp_data->still_frame_size == 0) {
                return ESP_ERR_INVALID_STATE;
            }
            memcpy(rgba_buffer, webp_data->still_rgba, webp_data->still_frame_size);
            webp_data->current_frame_delay_ms = WEBP_STATIC_FRAME_DELAY_MS;
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
            webp_data->current_frame_delay_ms = WEBP_STATIC_FRAME_DELAY_MS;
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
