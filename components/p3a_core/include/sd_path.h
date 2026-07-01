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
 * @brief Number of shard directory levels used for SD-card cache fan-out
 *
 * Sharded caches (vault, giphy, museum) store files under
 *   {base}/{d0}/{d1}/{name}{ext}
 * where d_i = (shard_hash >> (8*i)) & SD_SHARD_MASK, rendered in DECIMAL
 * (directory names "0".."63"). shard_hash is a 64-bit FNV-1a over the
 * SANITIZED leaf filename (see sd_path_sanitize_filename()), finished with
 * the fmix64 avalanche mix — exact constants live in sd_path.c. Because the
 * hash input is the sanitized leaf name as it lands on disk, a cached
 * file's shard location is always re-derivable from its filename alone.
 *
 * This layout is the v1.0 ON-DISK FORMAT CONTRACT. The hash constants, the
 * 6-bit mask, the decimal directory names, and the depth are all frozen:
 * changing any of them re-homes every already-cached file (the firmware
 * would compute different paths and re-download everything; the stranded
 * files would drain away only through age-based eviction). Pre-1.0
 * firmware used a 3-level SHA256-based layout; those trees were orphaned
 * without migration and are reclaimed by eviction like any cache debris
 * (or may be deleted manually for immediate space). The Makapix server URL
 * shard is a SEPARATE constant (MAKAPIX_REMOTE_SHARD_DEPTH, SHA256-based
 * server contract) and is not affected by this value.
 *
 * sd_path_build_sharded() is the only consumer. The eviction walker
 * (storage_eviction.c) is deliberately layout-UNAWARE — it judges files by
 * extension and age wherever they sit and does not read this constant.
 */
#define SD_SHARD_DEPTH 2

/**
 * @brief Bit mask applied to each shard hash byte (6 bits -> 64 dirs/level)
 *
 * Part of the v1.0 on-disk contract — see SD_SHARD_DEPTH.
 */
#define SD_SHARD_MASK 0x3F

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
 *   /sdcard/p3a/museum/{museum_id}/{d0}/{d1}/{iiif_key}.{ext}
 * This helper returns the common parent /sdcard/p3a/museum.
 *
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_museum(char *out_path, size_t out_len);

/**
 * @brief Get the klipy directory path (for Klipy artwork cache)
 *
 * The Klipy vault is sharded per product:
 *   /sdcard/p3a/klipy/{gif|sticker}/{d0}/{d1}/{klipy_id}.{ext}
 * This helper returns the common parent /sdcard/p3a/klipy.
 *
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_klipy(char *out_path, size_t out_len);

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
 * @brief Validate a candidate SD root path value
 *
 * Accepts either:
 * - User-friendly path: "/p3a", "/data", "/myproject" (recommended;
 *   "/sdcard" is prepended when resolved at boot)
 * - Full path: "/sdcard/p3a", "/sdcard/data" (compatibility form, used as-is)
 *
 * Rules: must start with '/', must name at least one folder (not bare "/"),
 * must not contain "..", and the resolved form (after any /sdcard prepend)
 * must fit SD_PATH_ROOT_MAX_LEN.
 *
 * The value itself is written through the settings JSON ("sdcard_root" key,
 * PUT /config): the HTTP handler rejects values failing this check, and
 * sd_path_init() applies the same check at boot, falling back to
 * SD_PATH_DEFAULT_ROOT for invalid stored values (older firmware, raw NVS
 * edits). Root changes always require a reboot — the root is read from NVS
 * exactly once, at init, and cached for the session.
 *
 * @param root_path Candidate root path (e.g., "/p3a" or "/sdcard/p3a")
 * @return ESP_OK if valid; ESP_ERR_INVALID_ARG on a malformed path;
 *         ESP_ERR_INVALID_SIZE if the resolved path is too long
 */
esp_err_t sd_path_validate_root(const char *root_path);

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
 * idempotent, so it is safe to apply unconditionally.
 * sd_path_build_sharded() applies the same per-character mapping
 * internally to both the shard-hash input and the emitted leaf name, so
 * callers that pre-sanitize and callers that pass raw identifiers
 * produce the same on-disk path.
 *
 * Silently truncates if `out_len` is smaller than the input; callers
 * that need length-checking should size `out_len ≥ strlen(in) + 1`.
 *
 * @param in     Null-terminated input string
 * @param out    Output buffer (may equal `in` for in-place rewrite)
 * @param out_len Size of out buffer (incl. NUL terminator)
 */
void sd_path_sanitize_filename(const char *in, char *out, size_t out_len);

/**
 * @brief Build a hash-sharded SD-card path (v1.0 on-disk layout)
 *
 * Produces:
 *   {base}/{d0}/{d1}/{sanitized leaf_name}{ext}    (SD_SHARD_DEPTH levels)
 * where d_i = (shard_hash >> (8*i)) & SD_SHARD_MASK rendered in decimal
 * ("0".."63"), and shard_hash is a 64-bit FNV-1a + fmix64 over the
 * SANITIZED leaf name. The same sanitized form is emitted as the filename,
 * so a cached file's shard location is always re-derivable from its
 * on-disk name alone. The extension is NOT part of the hash input.
 *
 * This is the single place the local shard layout is constructed; every
 * vault/giphy/museum path builder routes through it so writers and readers
 * can never disagree on the on-disk layout.
 *
 * Callers may pass `leaf_name` raw (e.g. a museum iiif_key with colons) or
 * pre-sanitized — sanitization is idempotent, so both produce the same
 * path. `ext` is appended verbatim; pass it including the leading dot
 * (e.g. ".webp") or "" for none.
 *
 * @param base      Base directory (e.g. "/sdcard/p3a/vault" or
 *                  "/sdcard/p3a/museum/{museum_id}")
 * @param leaf_name Filename stem (without extension); sanitized internally
 *                  and hashed to derive the shard directories
 * @param ext       Extension including the dot, or "" for none
 * @param out_path  Output buffer for the full path
 * @param out_len   Size of output buffer
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on null args or empty
 *         leaf_name, ESP_ERR_INVALID_SIZE if the buffer is too small
 */
esp_err_t sd_path_build_sharded(const char *base, const char *leaf_name,
                                const char *ext,
                                char *out_path, size_t out_len);

#ifdef __cplusplus
}
#endif

