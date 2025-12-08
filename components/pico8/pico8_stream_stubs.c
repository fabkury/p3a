/**
 * @file pico8_stream_stubs.c
 * @brief Stub implementations for PICO-8 functions when feature is disabled
 *
 * This file provides no-op implementations of the PICO-8 API to prevent
 * link errors when CONFIG_P3A_PICO8_ENABLE is disabled.
 * 
 * Note: animation_player_submit_pico8_frame is defined in animation_player.c
 * and returns ESP_ERR_NOT_SUPPORTED when PICO-8 is disabled.
 */

#include "pico8_stream.h"

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
