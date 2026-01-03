// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_channel_utils.h
 * @brief Public utility helpers shared across Makapix channel users
 *
 * These helpers are implemented in `components/channel_manager/makapix_channel_utils.c`.
 *
 * Note: This header intentionally exposes ONLY stateless utility helpers (UUID
 * conversion, SHA256 sharding helper, etc.). It does not expose internal channel
 * structures.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse UUID string to 16 bytes (removes hyphens)
 *
 * Accepts common UUID formats like "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
 *
 * @param uuid_str Input UUID string
 * @param out_bytes Output 16 bytes
 * @return true on success
 */
bool uuid_to_bytes(const char *uuid_str, uint8_t *out_bytes);

/**
 * @brief Convert 16 bytes back to UUID string with hyphens
 *
 * @param bytes Input 16 bytes
 * @param out Output string buffer
 * @param out_len Output buffer length
 */
void bytes_to_uuid(const uint8_t *bytes, char *out, size_t out_len);

/**
 * @brief Compute SHA256(storage_key) for Makapix vault sharding
 *
 * @param storage_key Storage key string (UUID text)
 * @param out_sha256 Output 32 bytes
 * @return ESP_OK on success
 */
esp_err_t storage_key_sha256(const char *storage_key, uint8_t out_sha256[32]);

/**
 * @brief File extension strings used by Makapix vault path builders
 *
 * Order: { ".webp", ".gif", ".png", ".jpg" }
 */
extern const char *s_ext_strings[];

#ifdef __cplusplus
}
#endif


