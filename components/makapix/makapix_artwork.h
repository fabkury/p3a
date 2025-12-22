#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef void (*makapix_download_progress_cb)(size_t bytes_read, size_t content_length, void *user_ctx);

/**
 * @brief Download artwork from URL and save to vault
 * 
 * Downloads the artwork image and saves it to the vault with hash-derived folder structure.
 * The file path will be: <vault_dir>/{hash[0:2]}/{hash[2:4]}/{storage_key}
 * 
 * @param art_url Full URL to download the artwork from
 * @param storage_key UUID string identifying the artwork
 * @param out_path Buffer to receive the full file path (must be at least 256 bytes)
 * @param path_len Maximum length of out_path buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_artwork_download(const char *art_url, const char *storage_key, char *out_path, size_t path_len);

/**
 * @brief Download with progress reporting callback
 */
esp_err_t makapix_artwork_download_with_progress(const char *art_url, const char *storage_key,
                                                 char *out_path, size_t path_len,
                                                 makapix_download_progress_cb cb, void *user_ctx);

/**
 * @brief Ensure cache doesn't exceed limit by evicting oldest items
 * 
 * Scans the vault directory and deletes oldest files (by creation date) until
 * the total count is below max_items.
 * 
 * @param max_items Maximum number of items to keep in cache (default: 250)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_artwork_ensure_cache_limit(size_t max_items);

