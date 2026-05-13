// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists.h
 * @brief Pinned-artworks vault: multiple named lists, each a first-class channel
 *
 * Layout under /sdcard/p3a/pinned/:
 *   state.bin               active list slug + version
 *   state.bin.bak
 *   lists/{slug}/manifest.json
 *   lists/{slug}/manifest.json.bak
 *   lists/{slug}/order.bin                  64 B per entry, newest-first
 *   lists/{slug}/order.bin.bak
 *   lists/{slug}/entries/{source}_{key_hash}.bin   rich metadata, on demand
 *   lists/{slug}/makapix/{uuid}.{ext}       artwork bytes (flat)
 *   lists/{slug}/giphy/{giphy_id}.{ext}
 *   lists/{slug}/museum/{museum_id}/{iiif_key}.{ext}
 *
 * There is always exactly one ACTIVE list. New pins from swipe-up gestures
 * (and from `/action/pin` without a list slug) go to the active list. The
 * default list "My Pins" is auto-created on first init.
 */

#pragma once

#include "pin_lists_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/*  Lifecycle                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Initialize the pin-lists module
 *
 * - Ensures /sdcard/p3a/pinned/ exists.
 * - Loads state.bin (active list pointer), recovering from .bak if needed.
 * - On first boot (no state.bin), creates a default list named "My Pins"
 *   with a freshly-generated slug and marks it active.
 *
 * Safe to call multiple times (subsequent calls return ESP_OK without redoing work).
 */
esp_err_t pin_lists_init(void);

/* ------------------------------------------------------------------------- */
/*  Active list pointer                                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief Get the active list's slug
 *
 * @param out_slug Buffer of at least PIN_LIST_SLUG_LEN bytes
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no active list set
 */
esp_err_t pin_lists_get_active(char out_slug[PIN_LIST_SLUG_LEN]);

/**
 * @brief Set the active list
 *
 * @param slug Slug of an existing list
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no such list
 */
esp_err_t pin_lists_set_active(const char *slug);

/* ------------------------------------------------------------------------- */
/*  List management                                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief Create a new list with the given name
 *
 * Generates an 8-hex-char slug via esp_random(), creates the directory
 * structure, writes an empty manifest.json and empty order.bin.
 *
 * @param name        User-facing display name (1..PIN_LIST_NAME_MAX-1 chars)
 * @param out_slug    Buffer of at least PIN_LIST_SLUG_LEN bytes (filled on success)
 * @return ESP_OK, or ESP_ERR_NO_MEM if PIN_LISTS_MAX_LISTS already exist
 */
esp_err_t pin_lists_create(const char *name, char out_slug[PIN_LIST_SLUG_LEN]);

/**
 * @brief Rename a list (slug unchanged; only the display name changes)
 */
esp_err_t pin_lists_rename(const char *slug, const char *new_name);

/**
 * @brief Delete a list (recursively removes lists/{slug}/)
 *
 * Refuses to delete the active list. If this would remove the last list,
 * a fresh "My Pins" is auto-created and marked active.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND, or ESP_ERR_INVALID_STATE (active list)
 */
esp_err_t pin_lists_delete(const char *slug);

/**
 * @brief Enumerate all lists
 *
 * @param out      Caller-provided array
 * @param cap      Capacity of `out`
 * @param out_n    Number of entries written (may exceed cap; check)
 */
esp_err_t pin_lists_enumerate(pin_list_info_t *out, size_t cap, size_t *out_n);

/**
 * @brief Get info for a single list
 */
esp_err_t pin_lists_get_info(const char *slug, pin_list_info_t *out);

/* ------------------------------------------------------------------------- */
/*  Per-list pin operations                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Get the current pin count for a list (cached after first read).
 *
 * Returns 0 if the list does not exist or its order.bin is corrupt.
 */
size_t pin_list_count(const char *slug);

/**
 * @brief Pin an artwork into a list
 *
 * Performs:
 *   1. Cap check (PIN_LIST_MAX_ENTRIES).
 *   2. Dedup check (no-op success if (source, source_id) is already pinned).
 *   3. Copy bytes from `src_artwork_path` to the list's vault subdirectory.
 *   4. Write the entry file under entries/.
 *   5. Insert into order.bin at index 0 (newest-first).
 *   6. Bump manifest.next_post_id and update cached count.
 *
 * The caller pre-builds `order_e` and `file_e` with source-specific fields
 * populated, but leaves `post_id` at 0 in both — pin_list_pin assigns the
 * next monotonic id from the manifest and writes the same value into both
 * records.
 *
 * @param slug             Target list (or NULL/empty for active)
 * @param order_e          Slim record (source_id, extension, pinned_at, source variant)
 * @param file_e           Rich record (title, creator, original_post_id, etc.)
 * @param src_artwork_path Absolute path to the source-vault file to copy
 */
esp_err_t pin_list_pin(const char *slug,
                       const pinned_order_entry_t *order_e,
                       const pinned_entry_file_t *file_e,
                       const char *src_artwork_path);

/**
 * @brief Remove a pin from a list
 *
 * No-op success if not present.
 */
esp_err_t pin_list_unpin(const char *slug, pinned_source_t src, const char *source_id);

/**
 * @brief Check whether a pin is present in a list
 *
 * O(1) via stat() on the entry file.
 */
bool pin_list_is_pinned(const char *slug, pinned_source_t src, const char *source_id);

/**
 * @brief Read a page of order entries
 *
 * @param slug    Target list
 * @param offset  Starting index (0 = newest)
 * @param limit   Maximum number of entries to copy into `out`
 * @param out     Caller-provided array sized for at least `limit` entries
 * @param out_n   Number of entries actually copied
 * @param out_total Optional; receives total count in the list
 */
esp_err_t pin_list_list(const char *slug, size_t offset, size_t limit,
                        pinned_order_entry_t *out, size_t *out_n, size_t *out_total);

/**
 * @brief Read the rich metadata file for a single pin
 *
 * Returns ESP_ERR_NOT_FOUND if the pin is not in the list.
 */
esp_err_t pin_list_get_entry(const char *slug, pinned_source_t src, const char *source_id,
                             pinned_entry_file_t *out);

/**
 * @brief Build the local artwork file path for a pinned order entry
 *
 * E.g. /sdcard/p3a/pinned/lists/{slug}/giphy/{giphy_id}.{ext}
 */
esp_err_t pin_list_build_artwork_path(const char *slug, const pinned_order_entry_t *e,
                                      char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
