// SPDX-License-Identifier: Apache-2.0
// The decoder interface: one table of operations per format, and the
// background colour the formats blend onto. See animation_decoder.h.

#include "animation_decoder.h"

#include <stdatomic.h>
#include <string.h>

#include "animation_decoder_formats.h"
#include "animation_decoder_internal.h"
#include "p3a_event.h"
#include "p3a_settings.h"

typedef struct {
    esp_err_t (*init)(animation_decoder_t **decoder, const uint8_t *data, size_t size);
    esp_err_t (*get_info)(animation_decoder_t *decoder, animation_decoder_info_t *info);
    esp_err_t (*decode_next_rgb)(animation_decoder_t *decoder, uint8_t *rgb_buffer);
    esp_err_t (*get_frame_delay)(animation_decoder_t *decoder, uint32_t *delay_ms);
    esp_err_t (*reset)(animation_decoder_t *decoder);
    void (*unload)(animation_decoder_t **decoder);
} format_ops_t;

static const format_ops_t s_formats[ANIMATION_DECODER_TYPE_COUNT_] = {
    [ANIMATION_DECODER_TYPE_WEBP] = {webp_decoder_init, webp_decoder_get_info, webp_decoder_decode_next_rgb,
                                     webp_decoder_get_frame_delay, webp_decoder_reset, webp_decoder_unload},
    [ANIMATION_DECODER_TYPE_GIF] = {gif_decoder_init, gif_decoder_get_info, gif_decoder_decode_next_rgb,
                                    gif_decoder_get_frame_delay, gif_decoder_reset, gif_decoder_unload},
    [ANIMATION_DECODER_TYPE_PNG] = {png_decoder_init, png_decoder_get_info, png_decoder_decode_next_rgb,
                                    png_decoder_get_frame_delay, png_decoder_reset, png_decoder_unload},
    [ANIMATION_DECODER_TYPE_JPEG] = {jpeg_decoder_init, jpeg_decoder_get_info_wrapper, jpeg_decoder_decode_next_rgb,
                                     jpeg_decoder_get_frame_delay, jpeg_decoder_reset, jpeg_decoder_unload},
    [ANIMATION_DECODER_TYPE_BMP] = {bmp_decoder_init, bmp_decoder_get_info, bmp_decoder_decode_next_rgb,
                                    bmp_decoder_get_frame_delay, bmp_decoder_reset, bmp_decoder_unload},
};

static const format_ops_t *ops_of(const animation_decoder_t *decoder)
{
    if (decoder == NULL || (int)decoder->type < 0 || decoder->type >= ANIMATION_DECODER_TYPE_COUNT_) {
        return NULL;
    }
    return &s_formats[decoder->type];
}

esp_err_t animation_decoder_init(animation_decoder_t **decoder, animation_decoder_type_t type, const uint8_t *data,
                                 size_t size)
{
    if (decoder == NULL || data == NULL || size == 0 || (int)type < 0 || type >= ANIMATION_DECODER_TYPE_COUNT_) {
        return ESP_ERR_INVALID_ARG;
    }
    *decoder = NULL;
    return s_formats[type].init(decoder, data, size);
}

esp_err_t animation_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info)
{
    const format_ops_t *ops = ops_of(decoder);
    if (ops == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // Zeroed first, so that a format which does not set source_consumed gets
    // the safe answer: the decoder still reads the file.
    memset(info, 0, sizeof(*info));
    return ops->get_info(decoder, info);
}

esp_err_t animation_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer)
{
    const format_ops_t *ops = ops_of(decoder);
    if (ops == NULL || rgb_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ops->decode_next_rgb(decoder, rgb_buffer);
}

esp_err_t animation_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms)
{
    const format_ops_t *ops = ops_of(decoder);
    if (ops == NULL || delay_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ops->get_frame_delay(decoder, delay_ms);
    if (err == ESP_OK) {
        *delay_ms = animation_decoder_effective_delay_ms(*delay_ms);
    }
    return err;
}

esp_err_t animation_decoder_reset(animation_decoder_t *decoder)
{
    const format_ops_t *ops = ops_of(decoder);
    if (ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ops->reset(decoder);
}

void animation_decoder_unload(animation_decoder_t **decoder)
{
    if (decoder == NULL || *decoder == NULL) {
        return;
    }
    const format_ops_t *ops = ops_of(*decoder);
    if (ops != NULL) {
        ops->unload(decoder);
    }
    *decoder = NULL;
}

// --- the background colour ------------------------------------------------------

static atomic_uint s_background;  // 0xRRGGBB
static atomic_uint s_generation;

void animation_decoder_set_background(uint8_t r, uint8_t g, uint8_t b)
{
    unsigned rgb = ((unsigned)r << 16) | ((unsigned)g << 8) | b;
    if (atomic_exchange(&s_background, rgb) != rgb) {
        atomic_fetch_add(&s_generation, 1);
    }
}

void animation_decoder_get_background(uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned rgb = atomic_load(&s_background);
    *r = (uint8_t)(rgb >> 16);
    *g = (uint8_t)(rgb >> 8);
    *b = (uint8_t)rgb;
}

uint32_t animation_decoder_background_generation(void)
{
    return atomic_load(&s_generation);
}

static void take_background_from_settings(void)
{
    static p3a_settings_t settings;  // too large for a task stack; only ever used by one caller at a time
    p3a_settings_get(&settings);
    animation_decoder_set_background((uint8_t)(settings.background_color >> 16),
                                     (uint8_t)(settings.background_color >> 8), (uint8_t)settings.background_color);
}

static void on_setting_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->payload.i32 == P3A_SET_BACKGROUND_COLOR) {
        take_background_from_settings();
    }
}

esp_err_t animation_decoder_start(void)
{
    take_background_from_settings();
    return p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_setting_changed, NULL);
}
