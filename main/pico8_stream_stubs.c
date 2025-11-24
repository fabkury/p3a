/**
 * @file pico8_stream_stubs.c
 * @brief Stub implementations for PICO-8 functions when feature is disabled
 *
 * This file provides no-op implementations of the PICO-8 API to prevent
 * link errors when CONFIG_P3A_PICO8_ENABLE is disabled.
 */

#include "pico8_stream.h"
#include "animation_player.h"

esp_err_t pico8_stream_init(void)
{
    return ESP_OK;
}

void pico8_stream_reset(void)
{
    // No-op
}

esp_err_t pico8_stream_feed_packet(const uint8_t *packet, size_t len)
{
    (void)packet;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

void pico8_stream_enter_mode(void)
{
    // No-op
}

void pico8_stream_exit_mode(void)
{
    // No-op
}

bool pico8_stream_is_active(void)
{
    return false;
}

esp_err_t animation_player_submit_pico8_frame(const uint8_t *palette_rgb, size_t palette_len,
                                              const uint8_t *pixel_data, size_t pixel_len)
{
    (void)palette_rgb;
    (void)palette_len;
    (void)pixel_data;
    (void)pixel_len;
    return ESP_ERR_NOT_SUPPORTED;
}

