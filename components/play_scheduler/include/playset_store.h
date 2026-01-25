// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file playset_store.h
 * @brief Playset storage API for persisting named playsets to SD card
 *
 * Provides binary file storage for playsets (scheduler commands) with CRC32
 * validation. Playsets are stored in /sdcard/p3a/channel/{name}.playset
 *
 * File format: 32-byte header + N * 144-byte channel entries
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

// Current file format version
#define PLAYSET_VERSION 10

// Maximum playset name length (excluding .playset extension)
#define PLAYSET_MAX_NAME_LEN 32

/**
 * @brief Playset file header (32 bytes)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;            // 0x50335053 ('P3PS')
    uint16_t version;          // File format version (10)
    uint16_t flags;            // Reserved (0)
    uint8_t  exposure_mode;    // ps_exposure_mode_t
    uint8_t  pick_mode;        // ps_pick_mode_t
    uint16_t channel_count;    // 1-64
    uint32_t checksum;         // CRC32 (zeroed during calculation)
    uint8_t  reserved[16];     // Future use
} playset_header_t;

_Static_assert(sizeof(playset_header_t) == 32, "Playset header must be 32 bytes");

/**
 * @brief Playset channel entry (144 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t  type;             // ps_channel_type_t
    char     name[33];         // e.g., "all", "promoted"
    char     identifier[33];   // For USER/HASHTAG
    char     display_name[65]; // Human-readable
    uint32_t weight;           // For MANUAL mode
    uint8_t  reserved[8];
} playset_channel_entry_t;

_Static_assert(sizeof(playset_channel_entry_t) == 144, "Playset channel entry must be 144 bytes");

/**
 * @brief Save a playset to SD card
 *
 * Uses atomic write pattern: write to .tmp, fsync, unlink old, rename.
 *
 * @param name Playset name (e.g., "followed_artists")
 * @param cmd Scheduler command to save
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid params,
 *         ESP_FAIL on file system error
 */
esp_err_t playset_store_save(const char *name, const ps_scheduler_command_t *cmd);

/**
 * @brief Load a playset from SD card
 *
 * Validates magic, version, and CRC32. On version mismatch, deletes the
 * file and returns ESP_ERR_INVALID_VERSION.
 *
 * @param name Playset name (e.g., "followed_artists")
 * @param out_cmd Output scheduler command
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file doesn't exist,
 *         ESP_ERR_INVALID_VERSION if version mismatch,
 *         ESP_ERR_INVALID_CRC if checksum mismatch,
 *         ESP_FAIL on file system error
 */
esp_err_t playset_store_load(const char *name, ps_scheduler_command_t *out_cmd);

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

#ifdef __cplusplus
}
#endif

#endif // PLAYSET_STORE_H
