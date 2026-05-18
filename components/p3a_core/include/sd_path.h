// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_path.h
 * @brief SD card path configuration and directory accessor interface
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Default SD card root folder for p3a data
 * 
 * All p3a data is stored under this folder on the SD card.
 * This is the full path including the SD mount point (/sdcard).
 * Users configure a user-friendly path (e.g., /p3a) which gets
 * prepended with /sdcard internally.
 * 
 * This can be configured via the web UI, but requires a reboot to take effect.
 */
#define SD_PATH_DEFAULT_ROOT "/sdcard/p3a"

/**
 * @brief Maximum length of the root path
 */
#define SD_PATH_ROOT_MAX_LEN 64

/**
 * @brief Initialize the SD path module
 * 
 * Loads the configured root path from NVS. If not set, uses the default.
 * This should be called once during startup, before any SD card operations.
 * 
 * @return ESP_OK on success
 */
esp_err_t sd_path_init(void);

/**
 * @brief Get the SD card root folder for p3a
 * 
 * @return Pointer to the root path string (e.g., "/sdcard/p3a")
 */
const char *sd_path_get_root(void);

/**
 * @brief Build a full path for a subdirectory under the p3a root
 * 
 * @param subdir Subdirectory name (e.g., "animations", "vault", "channel")
 * @param out_path Output buffer for the full path
 * @param out_len Size of output buffer
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if buffer too small
 */
esp_err_t sd_path_get_subdir(const char *subdir, char *out_path, size_t out_len);

/**
 * @brief Get the animations directory path
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_animations(char *out_path, size_t out_len);

/**
 * @brief Get the vault directory path
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_vault(char *out_path, size_t out_len);

/**
 * @brief Get the channel directory path
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_channel(char *out_path, size_t out_len);

/**
 * @brief Get the playlists directory path
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_playlists(char *out_path, size_t out_len);

/**
 * @brief Get the temporary directory path (staging area for uploads/downloads)
 *
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_temporary(char *out_path, size_t out_len);

/**
 * @brief Get the giphy directory path (for Giphy artwork cache)
 *
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_giphy(char *out_path, size_t out_len);

/**
 * @brief Get the museum directory path (for art-institution artwork cache)
 *
 * The institution vault is sharded per museum:
 *   /sdcard/p3a/museum/{museum_id}/{sha[0]}/{sha[1]}/{sha[2]}/{iiif_key}.{ext}
 * This helper returns the common parent /sdcard/p3a/museum.
 *
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_museum(char *out_path, size_t out_len);

/**
 * @brief Get the pinned-artworks root directory path
 *
 * Layout under this root:
 *   /sdcard/p3a/pinned/state.bin
 *   /sdcard/p3a/pinned/lists/{slug}/manifest.json
 *   /sdcard/p3a/pinned/lists/{slug}/order.bin
 *   /sdcard/p3a/pinned/lists/{slug}/entries/{source}_{hash}.bin
 *   /sdcard/p3a/pinned/lists/{slug}/{source}/...artwork files
 *
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_pinned(char *out_path, size_t out_len);

/**
 * @brief Set the SD card root folder (persisted to NVS, requires reboot)
 * 
 * Accepts either:
 * - User-friendly path: "/p3a", "/data", "/myproject" (recommended)
 * - Full path: "/sdcard/p3a", "/sdcard/data" (for compatibility)
 * 
 * The user-friendly format is stored in NVS, and /sdcard is prepended
 * internally at runtime.
 * 
 * @param root_path New root path (e.g., "/p3a" or "/sdcard/p3a")
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if validation fails
 */
esp_err_t sd_path_set_root(const char *root_path);

/**
 * @brief Create all required subdirectories under the p3a root
 *
 * Creates: animations, vault, channel, playlists, temporary, giphy, museum, pinned
 *
 * @return ESP_OK on success, or error if directory creation fails
 */
esp_err_t sd_path_ensure_directories(void);

/**
 * @brief Create all intermediate directories on the way to `filepath`
 *
 * Walks the directory components of `filepath` and `mkdir`s each missing
 * one. Stops at the final slash (which delimits the filename); the file
 * itself is not created. `EEXIST` is treated as success — a parallel
 * creator already did the work.
 *
 * @param filepath A full path including the filename (the filename is not created)
 * @return ESP_OK on success, ESP_FAIL on mkdir failure
 */
esp_err_t sd_path_ensure_parent_dirs(const char *filepath);

/**
 * @brief Copy a string into out, replacing FAT-reserved chars with '_'
 *
 * FAT/exFAT (the filesystem on the SD card) forbids these characters in
 * filenames: `:/\?*"<>|`. Most p3a identifiers (Makapix UUIDs, Giphy
 * IDs, IIIF image keys for AIC/Rijks/V&A/Wellcome/SMK, SD card filenames)
 * already use only FAT-safe characters, but HAM's URN-shaped iiif_key
 * (`urn-3:HUAM:{id}_dynmc`) carries two colons that need substitution
 * before the identifier can land on disk as a filename component.
 *
 * The substitution is length-preserving (one-byte → one-byte) and
 * idempotent, so it is safe to apply unconditionally. SHA-shard
 * computations should be performed against the un-sanitized identifier
 * so the shard tree stays stable regardless of what the sanitizer does
 * to the leaf name.
 *
 * Silently truncates if `out_len` is smaller than the input; callers
 * that need length-checking should size `out_len ≥ strlen(in) + 1`.
 *
 * @param in     Null-terminated input string
 * @param out    Output buffer (may equal `in` for in-place rewrite)
 * @param out_len Size of out buffer (incl. NUL terminator)
 */
void sd_path_sanitize_filename(const char *in, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

