#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize PICO-8 stream parser
 * 
 * Initializes the stream parser for WebSocket-based streaming.
 * Should be called during system initialization.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t pico8_stream_init(void);

/**
 * @brief Reset stream parser state
 * 
 * Resets the parser to initial state, discarding any partial frames.
 */
void pico8_stream_reset(void);

/**
 * @brief Feed a single, complete PICO-8 packet
 *
 * Validates packet header/payload sizes and submits directly.
 * Intended for WebSocket ingestion where each WS frame carries exactly one packet.
 * 
 * @param packet Pointer to complete packet buffer (header + payload)
 * @param len Total packet length in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t pico8_stream_feed_packet(const uint8_t *packet, size_t len);

/**
 * @brief Enter PICO-8 mode
 * 
 * Pauses animation playback and enables PICO-8 frame rendering.
 * Should be called when a WebSocket connection is established.
 */
void pico8_stream_enter_mode(void);

/**
 * @brief Exit PICO-8 mode
 * 
 * Resumes animation playback and disables PICO-8 frame rendering.
 * Should be called when WebSocket connection is closed or times out.
 */
void pico8_stream_exit_mode(void);

/**
 * @brief Check if PICO-8 mode is active
 * 
 * @return true if PICO-8 mode is active, false otherwise
 */
bool pico8_stream_is_active(void);

#ifdef __cplusplus
}
#endif


