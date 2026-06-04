// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file active_playset_store.h
 * @brief Persistence for the single "currently active" playset (boot-restore snapshot)
 *
 * Separate concern from playset_store.c (which owns the user-named playset
 * library). The active-playset snapshot is the device's single record of
 * "what was last playing" — overwritten on every play_scheduler_execute_playset()
 * and consulted exactly once on boot to resume playback.
 *
 * On-disk format: {sd-root}/active_playset.bin (root resolved at runtime via
 * sd_path_get_root(), default /sdcard/p3a — so switching the configured root
 * cold-starts the playset too)
 *   - 64-byte header (magic, version, CRC32, channel_count, playset name)
 *   - N × active_playset_channel_entry_t (one per channel; carries the full
 *     ps_channel_spec_t including the artwork sub-struct, unlike playset_store
 *     which omits artwork fields)
 *
 * Atomic write: write to .tmp, fsync, rename.
 *
 * On version mismatch / corruption, the file is deleted and the call returns
 * ESP_ERR_INVALID_VERSION / ESP_ERR_INVALID_CRC. The boot-restore path treats
 * these the same as "no snapshot" — falls back to the Makapix Promoted default.
 */

#ifndef ACTIVE_PLAYSET_STORE_H
#define ACTIVE_PLAYSET_STORE_H

#include "esp_err.h"
#include "play_scheduler_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Save the active playset to {sd-root}/active_playset.bin
 *
 * @param playset Non-NULL playset with channel_count in [1, PS_MAX_CHANNELS].
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG, or ESP_FAIL on IO error.
 */
esp_err_t active_playset_save(const ps_playset_t *playset);

/**
 * @brief Load the active playset snapshot.
 *
 * @param out_playset Receives the deserialized playset on success.
 * @return ESP_OK on success;
 *         ESP_ERR_NOT_FOUND if the file does not exist;
 *         ESP_ERR_INVALID_VERSION on version mismatch (file deleted);
 *         ESP_ERR_INVALID_CRC on magic/checksum failure (file deleted);
 *         ESP_FAIL on IO error.
 */
esp_err_t active_playset_load(ps_playset_t *out_playset);

/**
 * @brief Delete the active-playset snapshot file.
 *
 * @return ESP_OK if the file was deleted or did not exist; ESP_FAIL on IO error.
 */
esp_err_t active_playset_clear(void);

#ifdef __cplusplus
}
#endif

#endif // ACTIVE_PLAYSET_STORE_H
