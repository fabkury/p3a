#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * @brief Cache entry metadata
 */
typedef struct {
    char url_hash[65];        // SHA256 hex string (64 chars + null)
    char original_url[512];   // Original URL for reference
    uint64_t file_size;      // Size of cached file in bytes
    uint64_t timestamp;       // Last access time (Unix timestamp)
    uint32_t access_count;   // Number of times accessed
} storage_cache_entry_t;

/**
 * @brief Cache statistics
 */
typedef struct {
    uint32_t total_entries;
    uint32_t max_entries;
    uint64_t total_size_bytes;
    uint64_t max_size_bytes;
    uint32_t hit_count;
    uint32_t miss_count;
} storage_cache_stats_t;

/**
 * @brief Initialize cache subsystem
 * 
 * Creates cache directory structure if needed. Must be called after filesystem init.
 * 
 * @return ESP_OK on success
 */
esp_err_t storage_cache_init(void);

/**
 * @brief Check if an entry exists in cache by URL
 * 
 * @param url Original URL to look up
 * @param out_hash Optional buffer to receive SHA256 hash (64 chars + null)
 * @param out_size Optional buffer to receive file size
 * @return true if found, false otherwise
 */
bool storage_cache_lookup(const char *url, char *out_hash, uint64_t *out_size);

/**
 * @brief Insert a new entry into cache
 * 
 * Copies file from source_path to cache location. Updates LRU order.
 * Automatically evicts oldest entries if limits exceeded.
 * 
 * @param url Original URL
 * @param source_path Path to source file to cache
 * @param expected_hash Expected SHA256 hash (hex string, 64 chars)
 * @return ESP_OK on success
 */
esp_err_t storage_cache_insert(const char *url, const char *source_path, const char *expected_hash);

/**
 * @brief Get cached file path
 * 
 * @param url Original URL
 * @param out_path Buffer to receive file path (must be at least 256 bytes)
 * @param max_len Maximum length of out_path buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t storage_cache_get_path(const char *url, char *out_path, size_t max_len);

/**
 * @brief Remove an entry from cache
 * 
 * @param url Original URL
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t storage_cache_remove(const char *url);

/**
 * @brief Get cache statistics
 * 
 * @param stats Pointer to structure to fill
 * @return ESP_OK on success
 */
esp_err_t storage_cache_get_stats(storage_cache_stats_t *stats);

/**
 * @brief Purge cache entries until under size/count limits
 * 
 * Removes oldest entries first (LRU eviction).
 * 
 * @param target_size_bytes Target total size in bytes (0 = use max_size_bytes)
 * @param target_count Target entry count (0 = use max_entries)
 * @return ESP_OK on success
 */
esp_err_t storage_cache_purge(uint64_t target_size_bytes, uint32_t target_count);

/**
 * @brief Clear all cache entries
 * 
 * @return ESP_OK on success
 */
esp_err_t storage_cache_clear(void);

/**
 * @brief Check if cache is initialized
 */
bool storage_cache_is_initialized(void);

