// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_channel_utils.h
 * @brief Public utility helpers shared across Makapix channel users
 *
 * These helpers are implemented in `components/channel_manager/makapix_channel_utils.c`.
 *
 * Note: This header intentionally exposes ONLY stateless utility helpers (UUID
 * conversion, vault/remote path helpers, etc.). It does not expose internal
 * channel structures.
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
 * @brief Compute SHA256(storage_key) for the Makapix server's URL shard
 *
 * Used only by makapix_build_remote_shard() — the server's /api/vault/ URL
 * layout is SHA256-based. The LOCAL SD-card vault uses the FNV-1a shard
 * scheme in sd_path_build_sharded() and does not involve SHA256.
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

/**
 * @brief Number of SHA256 bytes in the Makapix Club server's vault URL shard
 *
 * The server hosts artwork at `/api/vault/{aa}/{bb}/{key}.{ext}`, where each
 * level is the low 6 bits (MAKAPIX_REMOTE_SHARD_MASK) of one SHA256 byte,
 * rendered as two lowercase hex digits ("00".."3f"). This is a SERVER
 * CONTRACT and is intentionally a SEPARATE constant from the local SD-card
 * SD_SHARD_DEPTH — the device cannot change the server's URL layout, so this
 * must not follow a local depth change. See makapix_build_remote_shard().
 *
 * History: before 2026-06 the server used a 3-level shard of full (unmasked)
 * SHA256 bytes. The server still serves those URLs but they are deprecated
 * and will eventually stop working.
 */
#define MAKAPIX_REMOTE_SHARD_DEPTH 2

/**
 * @brief Bit mask applied to each SHA256 shard byte (6 bits -> 64 dirs/level)
 *
 * Part of the Makapix server URL contract — see MAKAPIX_REMOTE_SHARD_DEPTH.
 */
#define MAKAPIX_REMOTE_SHARD_MASK 0x3F

/**
 * @brief Build the local SD-card vault path for a Makapix artwork
 *
 * Thin wrapper over sd_path_build_sharded() that maps the extension index to a
 * string and uses the storage_key (UUID text) as the leaf filename (which is
 * also the shard-hash input):
 *   {vault_base}/{d0}/{d1}/{storage_key}{ext}
 * where the d_i are the 6-bit decimal shard dirs (see SD_SHARD_DEPTH).
 *
 * @param vault_base Vault root (e.g. sd_path_get_vault() or ch->vault_path)
 * @param storage_key UUID text
 * @param ext_index Extension index (0=webp,1=gif,2=png,3=jpg; out-of-range -> webp)
 * @param out Output buffer
 * @param out_len Output buffer length
 * @return ESP_OK on success, or the sd_path_build_sharded() error
 */
esp_err_t makapix_build_vault_path(const char *vault_base, const char *storage_key,
                                   uint8_t ext_index, char *out, size_t out_len);

/**
 * @brief Build the Makapix server vault shard prefix "aa/bb" for a storage_key
 *
 * Writes MAKAPIX_REMOTE_SHARD_DEPTH bytes of SHA256(storage_key), each masked
 * to its low 6 bits (MAKAPIX_REMOTE_SHARD_MASK), as a slash-separated
 * lowercase-hex string (no leading or trailing slash), for use in the remote
 * URL `https://{host}/api/vault/{shard}/{key}.{ext}`. This is the single
 * place the remote URL shard is computed.
 *
 * @param storage_key UUID text
 * @param out Output buffer (>= 3*MAKAPIX_REMOTE_SHARD_DEPTH bytes)
 * @param out_len Output buffer length
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG/SIZE/ESP_FAIL otherwise
 */
esp_err_t makapix_build_remote_shard(const char *storage_key, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif


