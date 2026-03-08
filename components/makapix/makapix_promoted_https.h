// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Refresh the promoted channel via HTTPS (public REST endpoint)
 *
 * Fetches promoted artworks from the Makapix public API when MQTT is not
 * available (e.g., device has no Makapix Club registration). Synchronous —
 * blocks the caller until all pages are fetched or an error occurs.
 *
 * Uses the same channel cache and merge path as the MQTT refresh, so the
 * resulting cache is identical regardless of transport.
 *
 * @param channel_id Channel identifier (hex hash of "1:promoted:")
 * @return ESP_OK on success, error code on failure
 */
esp_err_t makapix_promoted_https_refresh(const char *channel_id);

/**
 * @brief Cancel an in-progress HTTPS promoted refresh
 *
 * Sets a flag checked between page fetches. The current page will complete
 * but no further pages will be fetched.
 */
void makapix_promoted_https_cancel(void);

/**
 * @brief Check if HTTPS promoted refresh has been cancelled
 *
 * @return true if cancel was requested
 */
bool makapix_promoted_https_is_cancelled(void);

#ifdef __cplusplus
}
#endif
