// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists_internal.h
 * @brief Cross-file helpers private to the pin_lists component
 */

#pragma once

#include "pin_lists.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/*  Path builders                                                            */
/* ------------------------------------------------------------------------- */

/* /sdcard/p3a/pinned */
esp_err_t pl_paths_root(char *out, size_t out_len);

/* /sdcard/p3a/pinned/state.bin */
esp_err_t pl_paths_state(char *out, size_t out_len);

/* /sdcard/p3a/pinned/lists */
esp_err_t pl_paths_lists_root(char *out, size_t out_len);

/* /sdcard/p3a/pinned/lists/{slug} */
esp_err_t pl_paths_list_dir(const char *slug, char *out, size_t out_len);

/* /sdcard/p3a/pinned/lists/{slug}/manifest.json */
esp_err_t pl_paths_manifest(const char *slug, char *out, size_t out_len);

/* /sdcard/p3a/pinned/lists/{slug}/order.bin */
esp_err_t pl_paths_order(const char *slug, char *out, size_t out_len);

/* /sdcard/p3a/pinned/lists/{slug}/entries/{source}_{key_hash}.bin */
esp_err_t pl_paths_entry(const char *slug, pinned_source_t src, const char *source_id,
                         char *out, size_t out_len);

/* ------------------------------------------------------------------------- */
/*  state.bin                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Load state.bin (with .bak fallback)
 *
 * @return ESP_OK if loaded; ESP_ERR_NOT_FOUND if neither primary nor .bak exists;
 *         ESP_ERR_INVALID_CRC on corruption that recovery couldn't fix.
 */
esp_err_t pl_state_load(pinned_state_t *out);

/**
 * @brief Save state.bin atomically (temp + fsync + rename + bak rotation)
 *
 * Fills in `magic`, `version`, and CRC. Caller sets `active_slug`.
 */
esp_err_t pl_state_save(const pinned_state_t *state);

/* ------------------------------------------------------------------------- */
/*  manifest.json                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t version;
    char     slug[PIN_LIST_SLUG_LEN];
    char     name[PIN_LIST_NAME_MAX];
    uint32_t created_at;
    int32_t  next_post_id;
    uint32_t count_cache;     /* mirrors order.bin header.count for cheap reads */
} pl_manifest_t;

esp_err_t pl_manifest_load(const char *slug, pl_manifest_t *out);
esp_err_t pl_manifest_save(const char *slug, const pl_manifest_t *m);

/* ------------------------------------------------------------------------- */
/*  order.bin                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Read the header (count, CRC) without loading entries
 */
esp_err_t pl_order_read_header(const char *slug, pinned_order_header_t *out);

/**
 * @brief Read a contiguous range of entries from order.bin (random-access via seek)
 *
 * @param slug    Target list
 * @param offset  Starting index
 * @param limit   Maximum number of entries to read
 * @param out     Caller buffer sized for at least `limit` entries
 * @param out_n   Number of entries actually read
 */
esp_err_t pl_order_read_range(const char *slug, size_t offset, size_t limit,
                              pinned_order_entry_t *out, size_t *out_n);

/**
 * @brief Read all entries from order.bin into a SPIRAM-allocated buffer
 *
 * Caller must heap_caps_free() the returned buffer.
 */
esp_err_t pl_order_read_all(const char *slug, pinned_order_entry_t **out_entries,
                            size_t *out_n);

/**
 * @brief Atomically replace order.bin with the given array
 *
 * Computes CRC, writes temp + fsync + rename, rotates .bak.
 */
esp_err_t pl_order_replace(const char *slug, const pinned_order_entry_t *entries,
                           size_t n);

/* ------------------------------------------------------------------------- */
/*  entry file                                                               */
/* ------------------------------------------------------------------------- */

esp_err_t pl_entry_write(const char *slug, const pinned_entry_file_t *e);
esp_err_t pl_entry_read(const char *slug, pinned_source_t src, const char *source_id,
                        pinned_entry_file_t *out);
bool      pl_entry_exists(const char *slug, pinned_source_t src, const char *source_id);
esp_err_t pl_entry_delete(const char *slug, pinned_source_t src, const char *source_id);

/* ------------------------------------------------------------------------- */
/*  Artwork copy                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Copy a file from src_path to dest_path with atomic temp+rename
 *
 * Uses 32 KB chunks. Creates parent dirs of dest_path if missing.
 */
esp_err_t pl_artwork_copy(const char *src_path, const char *dest_path);

/* ------------------------------------------------------------------------- */
/*  Helpers                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Validate that a slug is well-formed (exactly 8 lowercase hex chars).
 */
bool pl_slug_is_valid(const char *slug);

/**
 * @brief Generate a fresh random 8-hex-char slug into `out_slug`.
 *
 * @param out_slug Buffer of at least PIN_LIST_SLUG_LEN bytes.
 */
void pl_slug_generate(char out_slug[PIN_LIST_SLUG_LEN]);

/**
 * @brief Hash (source, source_id) into a 12-hex-char filename component.
 *
 * Used to build entry-file filenames. Stable per (source, source_id).
 */
void pl_source_id_hash(pinned_source_t src, const char *source_id,
                       char out_hex[13]);

/**
 * @brief Recursively delete a directory and its contents.
 */
esp_err_t pl_rmtree(const char *path);

/**
 * @brief Compute CRC32 over a byte buffer.
 *
 * Wrapper around esp_rom_crc32_le so callers don't depend on the ROM header.
 */
uint32_t pl_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
