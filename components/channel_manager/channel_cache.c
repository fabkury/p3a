// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "channel_cache.h"
#include "event_bus.h"
#include "makapix_channel_internal.h"
#include "vault_storage.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "channel_cache";

// ============================================================================
// Global State
// ============================================================================

#define MAX_REGISTERED_CACHES 8

static struct {
    bool initialized;
    TimerHandle_t save_timer;
    channel_cache_t *registered[MAX_REGISTERED_CACHES];
    size_t registered_count;
    char channels_path[128];  // Stored for timer callback
    SemaphoreHandle_t registry_mutex;
} s_cache_state = {0};

// ============================================================================
// CRC32 Implementation
// ============================================================================

// Standard CRC32 table (polynomial 0xEDB88320)
static uint32_t s_crc32_table[256];
static bool s_crc32_table_initialized = false;

static void crc32_init_table(void)
{
    if (s_crc32_table_initialized) return;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        s_crc32_table[i] = crc;
    }
    s_crc32_table_initialized = true;
}

uint32_t channel_cache_crc32(const void *data, size_t len)
{
    crc32_init_table();

    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        crc = s_crc32_table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }

    return ~crc;
}

// ============================================================================
// Ci Hash Table Management
// ============================================================================

/**
 * @brief Free Ci hash tables
 */
static void ci_hash_free(channel_cache_t *cache)
{
    if (!cache) return;

    // Free post_id hash
    ci_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->post_id_hash, node, tmp) {
        HASH_DEL(cache->post_id_hash, node);
        free(node);
    }
    cache->post_id_hash = NULL;

    // Free storage_key hash
    ci_storage_key_node_t *sk_node, *sk_tmp;
    HASH_ITER(hh, cache->storage_key_hash, sk_node, sk_tmp) {
        HASH_DEL(cache->storage_key_hash, sk_node);
        free(sk_node);
    }
    cache->storage_key_hash = NULL;
}

/**
 * @brief Add single entry to Ci hash tables
 */
static void ci_hash_add_entry(channel_cache_t *cache, uint32_t ci_index)
{
    if (!cache || !cache->entries || ci_index >= cache->entry_count) return;

    const makapix_channel_entry_t *entry = &cache->entries[ci_index];

    // Add to post_id hash
    ci_post_id_node_t *pid_node = malloc(sizeof(ci_post_id_node_t));
    if (pid_node) {
        pid_node->post_id = entry->post_id;
        pid_node->ci_index = ci_index;
        HASH_ADD_INT(cache->post_id_hash, post_id, pid_node);
    }

    // Add to storage_key hash
    ci_storage_key_node_t *sk_node = malloc(sizeof(ci_storage_key_node_t));
    if (sk_node) {
        memcpy(sk_node->storage_key_uuid, entry->storage_key_uuid, 16);
        sk_node->ci_index = ci_index;
        HASH_ADD(hh, cache->storage_key_hash, storage_key_uuid, 16, sk_node);
    }
}

/**
 * @brief Rebuild both Ci hash tables from entries array
 */
static void ci_rebuild_hash_tables(channel_cache_t *cache)
{
    if (!cache) return;

    // Free existing hash tables
    ci_hash_free(cache);

    if (!cache->entries || cache->entry_count == 0) return;

    // Build hash tables
    for (size_t i = 0; i < cache->entry_count; i++) {
        ci_hash_add_entry(cache, (uint32_t)i);
    }

    ESP_LOGD(TAG, "Ci hash tables rebuilt: %zu entries", cache->entry_count);
}

// ============================================================================
// LAi Hash Table Management
// ============================================================================

/**
 * @brief Free LAi hash table
 */
static void lai_hash_free(channel_cache_t *cache)
{
    if (!cache) return;

    lai_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->lai_hash, node, tmp) {
        HASH_DEL(cache->lai_hash, node);
        free(node);
    }
    cache->lai_hash = NULL;
}

/**
 * @brief Rebuild LAi hash from available_post_ids array
 */
static void lai_rebuild_hash(channel_cache_t *cache)
{
    if (!cache) return;

    // Free existing hash
    lai_hash_free(cache);

    if (!cache->available_post_ids || cache->available_count == 0) return;

    // Build hash from array
    for (size_t i = 0; i < cache->available_count; i++) {
        lai_post_id_node_t *node = malloc(sizeof(lai_post_id_node_t));
        if (node) {
            node->post_id = cache->available_post_ids[i];
            HASH_ADD_INT(cache->lai_hash, post_id, node);
        }
    }

    ESP_LOGD(TAG, "LAi hash rebuilt: %zu entries", cache->available_count);
}

// ============================================================================
// Path Building
// ============================================================================

void channel_cache_build_path(const char *channel_id,
                              const char *channels_path,
                              char *out, size_t out_len)
{
    // Sanitize channel_id (replace non-alphanumeric with underscore)
    char safe_id[64];
    size_t j = 0;
    for (size_t i = 0; channel_id[i] && j < sizeof(safe_id) - 1; i++) {
        char c = channel_id[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            safe_id[j++] = c;
        }
    }
    safe_id[j] = '\0';

    // Use .cache extension to avoid conflict with raw index .bin files
    // written by the refresh task. This allows the cache (with header + LAi)
    // to persist independently of the index file updates.
    snprintf(out, out_len, "%s/%s.cache", channels_path, safe_id);
}

// ============================================================================
// Legacy Format Detection and Migration
// ============================================================================

/**
 * @brief Check if file uses legacy format (no header, just raw entries)
 *
 * Legacy format: file size is multiple of 64 bytes (sizeof(makapix_channel_entry_t))
 * New format: starts with CHANNEL_CACHE_MAGIC
 */
static bool is_legacy_format(FILE *f)
{
    uint32_t magic;
    if (fread(&magic, sizeof(magic), 1, f) != 1) {
        return false;  // Empty or unreadable
    }
    fseek(f, 0, SEEK_SET);
    return magic != CHANNEL_CACHE_MAGIC;
}

/**
 * @brief Load legacy format (raw array of entries)
 */
static esp_err_t load_legacy_format(FILE *f, channel_cache_t *cache, const char *vault_path)
{
    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0 || file_size % sizeof(makapix_channel_entry_t) != 0) {
        ESP_LOGW(TAG, "Legacy file size %ld not aligned to entry size", file_size);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t entry_count = file_size / sizeof(makapix_channel_entry_t);
    if (entry_count > CHANNEL_CACHE_MAX_ENTRIES) {
        entry_count = CHANNEL_CACHE_MAX_ENTRIES;
    }

    if (entry_count == 0) {
        cache->entries = NULL;
        cache->entry_count = 0;
        cache->available_post_ids = NULL;
        cache->available_count = 0;
        return ESP_OK;
    }

    // Allocate entries
    cache->entries = malloc(entry_count * sizeof(makapix_channel_entry_t));
    if (!cache->entries) {
        return ESP_ERR_NO_MEM;
    }

    // Read entries
    size_t read = fread(cache->entries, sizeof(makapix_channel_entry_t), entry_count, f);
    if (read != entry_count) {
        free(cache->entries);
        cache->entries = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    cache->entry_count = entry_count;

    // Build Ci hash tables
    ci_rebuild_hash_tables(cache);

    // Allocate LAi (will be populated by lai_rebuild)
    cache->available_post_ids = malloc(entry_count * sizeof(int32_t));
    if (!cache->available_post_ids) {
        ci_hash_free(cache);
        free(cache->entries);
        cache->entries = NULL;
        return ESP_ERR_NO_MEM;
    }
    cache->available_count = 0;

    // Rebuild LAi from filesystem (now stores post_ids and builds hash)
    ESP_LOGI(TAG, "Migrating legacy cache, rebuilding LAi for %zu entries", entry_count);
    size_t available = lai_rebuild(cache, vault_path);
    ESP_LOGI(TAG, "LAi rebuild complete: %zu available", available);

    // Mark dirty to save in new format
    cache->dirty = true;

    return ESP_OK;
}

/**
 * @brief Load new format with header
 *
 * For version < 20: Return error to trigger LAi rebuild via legacy path
 * For version >= 20: Load LAi as post_ids and rebuild hash tables
 */
static esp_err_t load_new_format(FILE *f, channel_cache_t *cache)
{
    // Read header
    channel_cache_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Validate magic
    if (header.magic != CHANNEL_CACHE_MAGIC) {
        ESP_LOGE(TAG, "Invalid magic: 0x%08lX", (unsigned long)header.magic);
        return ESP_ERR_INVALID_STATE;
    }

    // Validate version - reject old versions to force LAi rebuild
    if (header.version < 20) {
        ESP_LOGI(TAG, "Cache version %u < 20, will rebuild LAi", header.version);
        return ESP_ERR_NOT_SUPPORTED;  // Triggers legacy path with rebuild
    }

    if (header.version > CHANNEL_CACHE_VERSION) {
        ESP_LOGE(TAG, "Unsupported version: %u", header.version);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Validate counts
    if (header.ci_count > CHANNEL_CACHE_MAX_ENTRIES ||
        header.lai_count > header.ci_count) {
        ESP_LOGE(TAG, "Invalid counts: ci=%lu lai=%lu",
                 (unsigned long)header.ci_count, (unsigned long)header.lai_count);
        return ESP_ERR_INVALID_STATE;
    }

    cache->cache_version = header.version;

    // Read and verify checksum
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *file_data = malloc(file_size);
    if (!file_data) {
        return ESP_ERR_NO_MEM;
    }

    if (fread(file_data, 1, file_size, f) != (size_t)file_size) {
        free(file_data);
        return ESP_ERR_INVALID_SIZE;
    }

    // Zero out checksum field for verification
    channel_cache_header_t *hdr = (channel_cache_header_t *)file_data;
    uint32_t stored_checksum = hdr->checksum;
    hdr->checksum = 0;

    uint32_t computed_checksum = channel_cache_crc32(file_data, file_size);
    if (computed_checksum != stored_checksum) {
        ESP_LOGW(TAG, "Checksum mismatch: stored=0x%08lX computed=0x%08lX",
                 (unsigned long)stored_checksum, (unsigned long)computed_checksum);
        free(file_data);
        return ESP_ERR_INVALID_CRC;
    }

    // Allocate and copy Ci entries
    if (header.ci_count > 0) {
        cache->entries = malloc(header.ci_count * sizeof(makapix_channel_entry_t));
        if (!cache->entries) {
            free(file_data);
            return ESP_ERR_NO_MEM;
        }
        memcpy(cache->entries, file_data + header.ci_offset,
               header.ci_count * sizeof(makapix_channel_entry_t));
        cache->entry_count = header.ci_count;
    } else {
        cache->entries = NULL;
        cache->entry_count = 0;
    }

    // Build Ci hash tables
    ci_rebuild_hash_tables(cache);

    // Allocate and copy LAi post_ids (v20+ stores post_ids, not ci_indices)
    if (header.lai_count > 0) {
        // Note: validation above ensures lai_count <= ci_count
        cache->available_post_ids = malloc(header.ci_count * sizeof(int32_t));
        if (!cache->available_post_ids) {
            ci_hash_free(cache);
            free(cache->entries);
            cache->entries = NULL;
            free(file_data);
            return ESP_ERR_NO_MEM;
        }
        memcpy(cache->available_post_ids, file_data + header.lai_offset,
               header.lai_count * sizeof(int32_t));
        cache->available_count = header.lai_count;
    } else {
        // Allocate space for future additions
        if (header.ci_count > 0) {
            cache->available_post_ids = malloc(header.ci_count * sizeof(int32_t));
            if (!cache->available_post_ids) {
                ci_hash_free(cache);
                free(cache->entries);
                cache->entries = NULL;
                free(file_data);
                return ESP_ERR_NO_MEM;
            }
        } else {
            cache->available_post_ids = NULL;
        }
        cache->available_count = 0;
    }

    // Build LAi hash table
    lai_rebuild_hash(cache);

    free(file_data);
    return ESP_OK;
}

// ============================================================================
// Cache Lifecycle
// ============================================================================

static void cache_flush_event_handler(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;

    if (!s_cache_state.initialized) return;

    channel_cache_flush_all(s_cache_state.channels_path);
}

static void save_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;

    if (!s_cache_state.initialized) return;

    event_bus_emit_simple(P3A_EVENT_CACHE_FLUSH);
}

esp_err_t channel_cache_init(void)
{
    if (s_cache_state.initialized) {
        return ESP_OK;
    }

    crc32_init_table();

    s_cache_state.registry_mutex = xSemaphoreCreateMutex();
    if (!s_cache_state.registry_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_cache_state.save_timer = xTimerCreate(
        "cache_save",
        pdMS_TO_TICKS(CHANNEL_CACHE_SAVE_DEBOUNCE_MS),
        pdFALSE,  // One-shot
        NULL,
        save_timer_callback
    );

    if (!s_cache_state.save_timer) {
        vSemaphoreDelete(s_cache_state.registry_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_cache_state.initialized = true;
    event_bus_subscribe(P3A_EVENT_CACHE_FLUSH, cache_flush_event_handler, NULL);
    ESP_LOGI(TAG, "Channel cache subsystem initialized");
    return ESP_OK;
}

void channel_cache_deinit(void)
{
    if (!s_cache_state.initialized) return;

    // Stop timer and flush
    if (s_cache_state.save_timer) {
        xTimerStop(s_cache_state.save_timer, portMAX_DELAY);
        xTimerDelete(s_cache_state.save_timer, portMAX_DELAY);
        s_cache_state.save_timer = NULL;
    }

    channel_cache_flush_all(s_cache_state.channels_path);

    if (s_cache_state.registry_mutex) {
        vSemaphoreDelete(s_cache_state.registry_mutex);
        s_cache_state.registry_mutex = NULL;
    }

    s_cache_state.initialized = false;
    ESP_LOGI(TAG, "Channel cache subsystem deinitialized");
}

/**
 * @brief Build legacy index file path ({channel_id}.bin)
 * Used for migration from old format to new .cache format.
 */
static void build_legacy_index_path(const char *channel_id,
                                    const char *channels_path,
                                    char *out, size_t out_len)
{
    // Sanitize channel_id (same as cache path)
    char safe_id[64];
    size_t j = 0;
    for (size_t i = 0; channel_id[i] && j < sizeof(safe_id) - 1; i++) {
        char c = channel_id[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            safe_id[j++] = c;
        }
    }
    safe_id[j] = '\0';

    snprintf(out, out_len, "%s/%s.bin", channels_path, safe_id);
}

esp_err_t channel_cache_load(const char *channel_id,
                             const char *channels_path,
                             const char *vault_path,
                             channel_cache_t *cache)
{
    if (!channel_id || !channels_path || !cache) {
        return ESP_ERR_INVALID_ARG;
    }

    // NOTE: Caller is responsible for freeing any existing cache data before calling this.
    // This function assumes cache points to either:
    // 1. A freshly allocated (uninitialized) structure, OR
    // 2. A zeroed structure
    // Do NOT call this on an already-loaded cache without freeing it first.

    // Initialize cache structure
    memset(cache, 0, sizeof(*cache));
    strncpy(cache->channel_id, channel_id, sizeof(cache->channel_id) - 1);
    cache->cache_version = CHANNEL_CACHE_VERSION;

    cache->mutex = xSemaphoreCreateMutex();
    if (!cache->mutex) {
        return ESP_ERR_NO_MEM;
    }

    // Store channels_path for debounced saves
    strncpy(s_cache_state.channels_path, channels_path,
            sizeof(s_cache_state.channels_path) - 1);

    // Build file paths
    char cache_path[256];
    char index_path[256];
    channel_cache_build_path(channel_id, channels_path, cache_path, sizeof(cache_path));
    build_legacy_index_path(channel_id, channels_path, index_path, sizeof(index_path));

    // Check if files exist
    struct stat cache_st = {0}, index_st = {0};
    bool have_cache = (stat(cache_path, &cache_st) == 0);
    bool have_index = (stat(index_path, &index_st) == 0);

    // NOTE: We do NOT compare mtimes here. The cache file (.cache) is authoritative once it exists.
    // The refresh completion handler updates the cache entries and saves it, so we don't need
    // to reload from the raw index (.bin) just because it's newer. This prevents repeated
    // LAi rebuilds which are expensive.

    // Try to open the new format cache file first (.cache)
    FILE *f = NULL;
    if (have_cache) {
        f = fopen(cache_path, "rb");
    }

    if (f) {
        // Cache file exists - check if it's valid new format
        if (!is_legacy_format(f)) {
            ESP_LOGI(TAG, "Loading cache (new format) for '%s'", channel_id);
            esp_err_t err = load_new_format(f, cache);
            fclose(f);
            
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Loaded cache '%s': %zu entries, %zu available",
                         channel_id, cache->entry_count, cache->available_count);
                return ESP_OK;
            }
            
            ESP_LOGW(TAG, "Cache file corrupt for '%s': %s, will try index",
                     channel_id, esp_err_to_name(err));
            // Fall through to try index file
        } else {
            // Shouldn't happen with .cache files, but handle gracefully
            ESP_LOGW(TAG, "Cache file '%s' has legacy format, migrating", cache_path);
            esp_err_t err = load_legacy_format(f, cache, vault_path);
            fclose(f);
            
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Loaded cache '%s': %zu entries, %zu available (migrated)",
                         channel_id, cache->entry_count, cache->available_count);
                return ESP_OK;
            }
            // Fall through to try index file
        }
    }

    // No .cache file, cache corrupt, or index is newer - load from .bin index file
    if (!have_index) {
        ESP_LOGI(TAG, "No cache or index for '%s', starting empty", channel_id);
        return ESP_OK;  // Empty cache is valid
    }

    f = fopen(index_path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "Cannot open index for '%s', starting empty", channel_id);
        return ESP_OK;  // Empty cache is valid
    }

    ESP_LOGI(TAG, "Loading from index for '%s' (will rebuild LAi)", channel_id);
    esp_err_t err = load_legacy_format(f, cache, vault_path);
    fclose(f);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load index for '%s': %s, starting empty",
                 channel_id, esp_err_to_name(err));
        // Clean up partial state (including hash tables)
        ci_hash_free(cache);
        lai_hash_free(cache);
        free(cache->entries);
        free(cache->available_post_ids);
        cache->entries = NULL;
        cache->available_post_ids = NULL;
        cache->entry_count = 0;
        cache->available_count = 0;
        cache->dirty = false;
        return ESP_OK;  // Return success with empty cache
    }

    ESP_LOGI(TAG, "Loaded cache '%s': %zu entries, %zu available (from index)",
             channel_id, cache->entry_count, cache->available_count);
    return ESP_OK;
}

esp_err_t channel_cache_save(const channel_cache_t *cache, const char *channels_path)
{
    if (!cache || !channels_path) {
        return ESP_ERR_INVALID_ARG;
    }

    // Ensure channels directory exists
    struct stat st;
    if (stat(channels_path, &st) != 0) {
        if (mkdir(channels_path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create channels directory: %s (errno=%d)", channels_path, errno);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Created channels directory: %s", channels_path);
    }

    // Build paths
    char path[256];
    char temp_path[260];
    channel_cache_build_path(cache->channel_id, channels_path, path, sizeof(path));
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    // Calculate sizes (LAi now stores int32_t post_ids)
    size_t ci_size = cache->entry_count * sizeof(makapix_channel_entry_t);
    size_t lai_size = cache->available_count * sizeof(int32_t);
    size_t total_size = sizeof(channel_cache_header_t) + ci_size + lai_size;

    // Allocate buffer for atomic write
    uint8_t *buffer = malloc(total_size);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    // Fill header
    channel_cache_header_t *header = (channel_cache_header_t *)buffer;
    memset(header, 0, sizeof(*header));
    header->magic = CHANNEL_CACHE_MAGIC;
    header->version = CHANNEL_CACHE_VERSION;
    header->flags = 0;
    header->ci_count = cache->entry_count;
    header->lai_count = cache->available_count;
    header->ci_offset = sizeof(channel_cache_header_t);
    header->lai_offset = sizeof(channel_cache_header_t) + ci_size;
    // Copy channel_id safely (header already zeroed by memset)
    size_t id_len = strlen(cache->channel_id);
    if (id_len > sizeof(header->channel_id) - 1) {
        id_len = sizeof(header->channel_id) - 1;
    }
    memcpy(header->channel_id, cache->channel_id, id_len);

    // Copy data
    if (cache->entries && ci_size > 0) {
        memcpy(buffer + header->ci_offset, cache->entries, ci_size);
    }
    if (cache->available_post_ids && lai_size > 0) {
        memcpy(buffer + header->lai_offset, cache->available_post_ids, lai_size);
    }

    // Compute checksum (with checksum field zeroed)
    header->checksum = 0;
    header->checksum = channel_cache_crc32(buffer, total_size);

    // Clean up any orphan temp file from previous interrupted save
    unlink(temp_path);

    // Write to temp file
    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create temp file: %s (errno=%d)", temp_path, errno);
        free(buffer);
        return ESP_FAIL;
    }

    size_t written = fwrite(buffer, 1, total_size, f);
    free(buffer);

    if (written != total_size) {
        ESP_LOGE(TAG, "Write failed: %zu/%zu bytes", written, total_size);
        fclose(f);
        unlink(temp_path);
        return ESP_ERR_INVALID_SIZE;
    }

    // Sync to disk
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // On FAT filesystems (SD card), rename() fails if destination exists.
    // Delete the destination first, then rename.
    if (stat(path, &st) == 0) {
        if (unlink(path) != 0) {
            ESP_LOGW(TAG, "Failed to remove old cache file: %s (errno=%d)", path, errno);
            // Continue anyway - rename might still work on some filesystems
        }
    }

    // Atomic rename (after deleting destination on FAT)
    if (rename(temp_path, path) != 0) {
        ESP_LOGE(TAG, "Rename failed: %s -> %s (errno=%d)", temp_path, path, errno);
        unlink(temp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved cache '%s': %zu entries, %zu available",
             cache->channel_id, cache->entry_count, cache->available_count);
    return ESP_OK;
}

void channel_cache_free(channel_cache_t *cache)
{
    if (!cache) return;

    if (cache->mutex) {
        xSemaphoreTake(cache->mutex, portMAX_DELAY);
    }

    // Free Ci hash tables
    ci_hash_free(cache);

    free(cache->entries);
    cache->entries = NULL;
    cache->entry_count = 0;

    // Free LAi hash table
    lai_hash_free(cache);

    free(cache->available_post_ids);
    cache->available_post_ids = NULL;
    cache->available_count = 0;

    if (cache->mutex) {
        SemaphoreHandle_t mutex = cache->mutex;
        cache->mutex = NULL;
        xSemaphoreGive(mutex);
        vSemaphoreDelete(mutex);
    }
}

// ============================================================================
// LAi Operations
// ============================================================================

bool lai_add_entry(channel_cache_t *cache, int32_t post_id)
{
    if (!cache) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Check membership via hash O(1)
    lai_post_id_node_t *existing;
    HASH_FIND_INT(cache->lai_hash, &post_id, existing);
    if (existing) {
        xSemaphoreGive(cache->mutex);
        return false;  // Already in LAi
    }

    // Ensure we have space
    if (!cache->available_post_ids) {
        size_t alloc_count = (cache->entry_count > 0) ? cache->entry_count : CHANNEL_CACHE_MAX_ENTRIES;
        cache->available_post_ids = malloc(alloc_count * sizeof(int32_t));
        if (!cache->available_post_ids) {
            xSemaphoreGive(cache->mutex);
            return false;
        }
    }

    // Add to array
    cache->available_post_ids[cache->available_count++] = post_id;

    // Add to hash
    lai_post_id_node_t *node = malloc(sizeof(lai_post_id_node_t));
    if (node) {
        node->post_id = post_id;
        HASH_ADD_INT(cache->lai_hash, post_id, node);
    }

    cache->dirty = true;

    xSemaphoreGive(cache->mutex);

    ESP_LOGD(TAG, "LAi add: post_id=%ld, count=%zu",
             (long)post_id, cache->available_count);
    return true;
}

bool lai_remove_entry(channel_cache_t *cache, int32_t post_id)
{
    if (!cache) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Find in hash O(1)
    lai_post_id_node_t *node;
    HASH_FIND_INT(cache->lai_hash, &post_id, node);
    if (!node) {
        xSemaphoreGive(cache->mutex);
        return false;  // Not found
    }

    // Remove from hash
    HASH_DEL(cache->lai_hash, node);
    free(node);

    // Find and swap-and-pop from array
    if (cache->available_post_ids) {
        for (size_t i = 0; i < cache->available_count; i++) {
            if (cache->available_post_ids[i] == post_id) {
                cache->available_post_ids[i] = cache->available_post_ids[--cache->available_count];
                break;
            }
        }
    }

    cache->dirty = true;

    xSemaphoreGive(cache->mutex);

    ESP_LOGD(TAG, "LAi remove: post_id=%ld, count=%zu",
             (long)post_id, cache->available_count);
    return true;
}

bool lai_contains(const channel_cache_t *cache, int32_t post_id)
{
    if (!cache) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    lai_post_id_node_t *node;
    HASH_FIND_INT(cache->lai_hash, &post_id, node);

    xSemaphoreGive(cache->mutex);
    return node != NULL;
}

size_t lai_rebuild(channel_cache_t *cache, const char *vault_path)
{
    if (!cache || !cache->entries || cache->entry_count == 0) {
        return 0;
    }

    // Free existing LAi hash
    lai_hash_free(cache);

    // Ensure LAi array is allocated
    if (!cache->available_post_ids) {
        cache->available_post_ids = malloc(cache->entry_count * sizeof(int32_t));
        if (!cache->available_post_ids) {
            return 0;
        }
    }

    cache->available_count = 0;
    size_t checked = 0;
    size_t found = 0;

    // Extension strings WITH leading dot (matches actual vault storage)
    static const char *ext_strings[] = {".webp", ".gif", ".png", ".jpg"};

    for (size_t i = 0; i < cache->entry_count; i++) {
        const makapix_channel_entry_t *entry = &cache->entries[i];

        // Skip playlists (they don't have direct files)
        if (entry->kind == MAKAPIX_INDEX_POST_KIND_PLAYLIST) {
            continue;
        }

        // Build vault path for this entry
        // Format: {vault_path}/{sha256[0]:02x}/{sha256[1]:02x}/{sha256[2]:02x}/{storage_key}.{ext}
        // This matches makapix_artwork_download() path format exactly
        char uuid_str[37];
        bytes_to_uuid(entry->storage_key_uuid, uuid_str, sizeof(uuid_str));

        uint8_t sha256[32];
        if (storage_key_sha256(uuid_str, sha256) != ESP_OK) {
            continue;
        }

        // Build 3-level directory path (matches actual download paths)
        char dir1[3], dir2[3], dir3[3];
        snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
        snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
        snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);

        int ext_idx = (entry->extension < 4) ? entry->extension : 0;

        char file_path[256];
        snprintf(file_path, sizeof(file_path), "%s/%s/%s/%s/%s%s",
                 vault_path, dir1, dir2, dir3, uuid_str, ext_strings[ext_idx]);

        checked++;

        // Check if file exists
        struct stat st;
        if (stat(file_path, &st) == 0 && S_ISREG(st.st_mode)) {
            // Also check for .404 marker
            char marker_path[264];
            snprintf(marker_path, sizeof(marker_path), "%s.404", file_path);
            if (stat(marker_path, &st) != 0) {
                // File exists and no 404 marker - store post_id (not ci_index)
                cache->available_post_ids[cache->available_count++] = entry->post_id;
                found++;
            }
        }

        // Yield periodically to avoid watchdog
        if (checked % 100 == 0) {
            taskYIELD();
        }
    }

    // Build LAi hash from array
    lai_rebuild_hash(cache);

    cache->dirty = true;
    ESP_LOGI(TAG, "LAi rebuild: checked %zu, found %zu available", checked, found);
    return found;
}

// ============================================================================
// Ci Operations
// ============================================================================

uint32_t ci_find_by_storage_key(const channel_cache_t *cache, const uint8_t *storage_key_uuid)
{
    if (!cache || !storage_key_uuid) {
        return UINT32_MAX;
    }

    // Use hash table for O(1) lookup
    ci_storage_key_node_t *node;
    HASH_FIND(hh, cache->storage_key_hash, storage_key_uuid, 16, node);
    return node ? node->ci_index : UINT32_MAX;
}

uint32_t ci_find_by_post_id(const channel_cache_t *cache, int32_t post_id)
{
    if (!cache) {
        return UINT32_MAX;
    }

    // Use hash table for O(1) lookup
    ci_post_id_node_t *node;
    HASH_FIND_INT(cache->post_id_hash, &post_id, node);
    return node ? node->ci_index : UINT32_MAX;
}

const makapix_channel_entry_t *ci_get_entry(const channel_cache_t *cache, uint32_t ci_index)
{
    if (!cache || !cache->entries || ci_index >= cache->entry_count) {
        return NULL;
    }
    return &cache->entries[ci_index];
}

// ============================================================================
// Persistence Scheduling
// ============================================================================

void channel_cache_schedule_save(channel_cache_t *cache)
{
    if (!cache || !s_cache_state.initialized) {
        return;
    }

    cache->dirty = true;

    // Reset the debounce timer
    if (s_cache_state.save_timer) {
        xTimerReset(s_cache_state.save_timer, 0);
    }
}

void channel_cache_flush_all(const char *channels_path)
{
    if (!s_cache_state.registry_mutex) return;

    xSemaphoreTake(s_cache_state.registry_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_cache_state.registered_count; i++) {
        channel_cache_t *cache = s_cache_state.registered[i];
        if (cache && cache->dirty) {
            esp_err_t err = channel_cache_save(cache, channels_path);
            if (err == ESP_OK) {
                cache->dirty = false;
            } else {
                ESP_LOGW(TAG, "Failed to flush cache '%s': %s",
                         cache->channel_id, esp_err_to_name(err));
            }
        }
    }

    xSemaphoreGive(s_cache_state.registry_mutex);
}

// ============================================================================
// Global Cache Registry
// ============================================================================

esp_err_t channel_cache_register(channel_cache_t *cache)
{
    if (!cache || !s_cache_state.registry_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_cache_state.registry_mutex, portMAX_DELAY);

    // Check if already registered
    for (size_t i = 0; i < s_cache_state.registered_count; i++) {
        if (s_cache_state.registered[i] == cache) {
            xSemaphoreGive(s_cache_state.registry_mutex);
            return ESP_OK;  // Already registered
        }
    }

    if (s_cache_state.registered_count >= MAX_REGISTERED_CACHES) {
        xSemaphoreGive(s_cache_state.registry_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_cache_state.registered[s_cache_state.registered_count++] = cache;

    xSemaphoreGive(s_cache_state.registry_mutex);
    return ESP_OK;
}

void channel_cache_unregister(channel_cache_t *cache)
{
    if (!cache || !s_cache_state.registry_mutex) {
        return;
    }

    xSemaphoreTake(s_cache_state.registry_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_cache_state.registered_count; i++) {
        if (s_cache_state.registered[i] == cache) {
            // Shift remaining entries
            for (size_t j = i; j < s_cache_state.registered_count - 1; j++) {
                s_cache_state.registered[j] = s_cache_state.registered[j + 1];
            }
            s_cache_state.registered_count--;
            break;
        }
    }

    xSemaphoreGive(s_cache_state.registry_mutex);
}

size_t channel_cache_get_total_available(void)
{
    if (!s_cache_state.registry_mutex) {
        return 0;
    }

    size_t total = 0;

    xSemaphoreTake(s_cache_state.registry_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_cache_state.registered_count; i++) {
        channel_cache_t *cache = s_cache_state.registered[i];
        if (cache) {
            total += cache->available_count;
        }
    }

    xSemaphoreGive(s_cache_state.registry_mutex);

    return total;
}

channel_cache_t *channel_cache_registry_find(const char *channel_id)
{
    if (!channel_id || !s_cache_state.registry_mutex) {
        return NULL;
    }

    channel_cache_t *found = NULL;

    xSemaphoreTake(s_cache_state.registry_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_cache_state.registered_count; i++) {
        channel_cache_t *cache = s_cache_state.registered[i];
        if (cache && strcmp(cache->channel_id, channel_id) == 0) {
            found = cache;
            break;
        }
    }

    xSemaphoreGive(s_cache_state.registry_mutex);

    return found;
}

esp_err_t channel_cache_get_next_missing(channel_cache_t *cache,
                                         uint32_t *cursor,
                                         makapix_channel_entry_t *out_entry)
{
    if (!cache || !cursor || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Iterate from cursor through entries
    while (*cursor < cache->entry_count) {
        const makapix_channel_entry_t *entry = &cache->entries[*cursor];
        (*cursor)++;

        // Skip non-artwork entries
        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            continue;
        }

        // Check if already in LAi using hash table (O(1) lookup, no mutex issue)
        lai_post_id_node_t *node;
        HASH_FIND_INT(cache->lai_hash, &entry->post_id, node);
        if (node) {
            continue;  // Already downloaded
        }

        // Found a missing entry - copy it out
        memcpy(out_entry, entry, sizeof(*out_entry));
        xSemaphoreGive(cache->mutex);
        return ESP_OK;
    }

    xSemaphoreGive(cache->mutex);
    return ESP_ERR_NOT_FOUND;
}
