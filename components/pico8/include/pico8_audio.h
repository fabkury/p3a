// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pico8_audio.h
 * @brief PICO-8 audio streaming to on-board speaker
 *
 * Receives PCM audio from WebSocket (browser Fake-08 emulator) and plays it
 * through the ES8311 DAC → NS4150B amplifier → speaker chain.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize audio hardware (ES8311 + I2S via BSP)
 *
 * Called lazily on first pico8_audio_start(). Safe to call multiple times.
 *
 * @return ESP_OK on success
 */
esp_err_t pico8_audio_init(void);

/**
 * @brief Start audio playback
 *
 * Creates the audio feed task and prepares the ring buffer.
 * Actual sound begins when samples arrive via pico8_audio_feed().
 *
 * @return ESP_OK on success
 */
esp_err_t pico8_audio_start(void);

/**
 * @brief Submit PCM samples for playback
 *
 * Thread-safe. Copies samples into a ring buffer consumed by the feed task.
 * Called from the HTTP server task (WebSocket handler).
 *
 * @param samples  Pointer to mono 16-bit signed LE PCM samples
 * @param num_samples  Number of samples (not bytes)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not started
 */
esp_err_t pico8_audio_feed(const int16_t *samples, size_t num_samples);

/**
 * @brief Stop audio playback
 *
 * Drains the ring buffer, stops the feed task, and mutes the codec.
 */
void pico8_audio_stop(void);

/**
 * @brief Check if audio playback is currently active
 */
bool pico8_audio_is_active(void);

#ifdef __cplusplus
}
#endif
