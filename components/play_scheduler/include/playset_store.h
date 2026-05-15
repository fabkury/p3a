// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file playset_store.h
 * @brief Playset storage API for persisting named playsets to SD card
 *
 * Provides binary file storage for playsets with CRC32
 * validation. Playsets are stored in /sdcard/p3a/channel/ps_{hash}.playset
 * where {hash} is a DJB2 hash of the playset name. The name is stored inside
 * the file header, decoupling human-readable names from filesystem constraints.
 *
 * File format: 64-byte header + N * 144-byte channel entries
 */

#ifndef PLAYSET_STORE_H
#define PLAYSET_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "play_scheduler_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Magic number: 'P3PS' (P3a PlaySet)
#define PLAYSET_MAGIC 0x50335053

// Current file format version (11: hash-based filenames, name in header)
#define PLAYSET_VERSION 11

// Maximum playset name length (same as PS_PLAYSET_NAME_MAX in play_scheduler_types.h)
#define PLAYSET_MAX_NAME_LEN PS_PLAYSET_NAME_MAX

/**
 * @brief Playset file header (64 bytes)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;            // 0x50335053 ('P3PS')
    uint16_t version;          // File format version (11)
    uint16_t flags;            // Reserved (0)
    uint8_t  _reserved_exposure_mode; // Legacy field (was ps_exposure_mode_t in v11); preserved for binary compat, always written as 0, ignored on read
    uint8_t  _reserved_pick_mode; // Legacy field (was ps_pick_mode_t); pick_mode moved to config_store global; preserved for binary compat, always written as 0, ignored on read
    uint16_t channel_count;    // 1-64
    uint32_t checksum;         // CRC32 (zeroed during calculation)
    char     name[33];         // Playset name (stored in file, not filename)
    uint8_t  reserved[15];     // Future use
} playset_header_t;

_Static_assert(sizeof(playset_header_t) == 64, "Playset header must be 64 bytes");

/**
 * @brief Playset channel entry (144 bytes)
 *
 * Note: `offset` consumes 4 of what were originally 8 reserved bytes in v11.
 * v11 files have those bytes zero-filled, so they load as offset=0 (the
 * correct default for the legacy "no offset" behavior). No format version
 * bump is needed; CRC stays sound because pre-offset save paths still emit
 * zero in those positions.
 */
typedef struct __attribute__((packed)) {
    uint8_t  type;             // ps_channel_type_t
    char     name[33];         // e.g., "all", "promoted"
    char     identifier[33];   // For USER/HASHTAG
    char     display_name[65]; // Human-readable
    uint32_t weight;           // Channel weight; if all channels are 0, the scheduler distributes equally
    uint32_t offset;           // Per-playset starting offset into the channel's source listing.
                               // Reads as 0 for v11 files (legacy zero-filled reserved bytes).
    uint8_t  reserved[4];
} playset_channel_entry_t;

_Static_assert(sizeof(playset_channel_entry_t) == 144, "Playset channel entry must be 144 bytes");

/**
 * @brief Save a playset to SD card
 *
 * Uses atomic write pattern: write to .tmp, fsync, unlink old, rename.
 *
 * @param name Playset name (e.g., "followed_artists")
 * @param playset Playset to save
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid params,
 *         ESP_FAIL on file system error
 */
esp_err_t playset_store_save(const char *name, const ps_playset_t *playset);

/**
 * @brief Load a playset from SD card
 *
 * Validates magic, version, and CRC32. On version mismatch, deletes the
 * file and returns ESP_ERR_INVALID_VERSION.
 *
 * @param name Playset name (e.g., "followed_artists")
 * @param out_playset Output playset
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file doesn't exist,
 *         ESP_ERR_INVALID_VERSION if version mismatch,
 *         ESP_ERR_INVALID_CRC if checksum mismatch,
 *         ESP_FAIL on file system error
 */
esp_err_t playset_store_load(const char *name, ps_playset_t *out_playset);

/**
 * @brief Check if a playset exists on SD card
 *
 * @param name Playset name
 * @return true if the playset file exists
 */
bool playset_store_exists(const char *name);

/**
 * @brief Delete a playset from SD card
 *
 * @param name Playset name
 * @return ESP_OK on success (or if file didn't exist),
 *         ESP_FAIL on file system error
 */
esp_err_t playset_store_delete(const char *name);

/**
 * @brief Playset list entry (summary metadata for listing)
 */
typedef struct {
    char name[PLAYSET_MAX_NAME_LEN + 1];
    size_t channel_count;
} playset_list_entry_t;

/**
 * @brief List all saved playsets on SD card
 *
 * Scans the playset directory for *.playset files and loads metadata
 * for each. Files that fail to load (corrupt, version mismatch) are skipped.
 *
 * @param out       Output array of list entries
 * @param max       Maximum number of entries to return
 * @param out_count Output: actual number of entries populated
 * @return ESP_OK on success (even if directory doesn't exist, returns count=0)
 */
esp_err_t playset_store_list(playset_list_entry_t *out, size_t max, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // PLAYSET_STORE_H
