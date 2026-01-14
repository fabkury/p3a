// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "channel_cache.h"
#include "makapix_channel_internal.h"
#include "vault_storage.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

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

    snprintf(out, out_len, "%s/%s.bin", channels_path, safe_id);
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
        cache->available_indices = NULL;
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

    // Allocate LAi (will be populated by lai_rebuild)
    cache->available_indices = malloc(entry_count * sizeof(uint32_t));
    if (!cache->available_indices) {
        free(cache->entries);
        cache->entries = NULL;
        return ESP_ERR_NO_MEM;
    }
    cache->available_count = 0;

    // Rebuild LAi from filesystem
    ESP_LOGI(TAG, "Migrating legacy cache, rebuilding LAi for %zu entries", entry_count);
    size_t available = lai_rebuild(cache, vault_path);
    ESP_LOGI(TAG, "LAi rebuild complete: %zu available", available);

    // Mark dirty to save in new format
    cache->dirty = true;

    return ESP_OK;
}

/**
 * @brief Load new format with header
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

    // Validate version
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

    // Allocate and copy LAi indices
    if (header.lai_count > 0) {
        cache->available_indices = malloc(header.ci_count * sizeof(uint32_t));
        if (!cache->available_indices) {
            free(cache->entries);
            cache->entries = NULL;
            free(file_data);
            return ESP_ERR_NO_MEM;
        }
        memcpy(cache->available_indices, file_data + header.lai_offset,
               header.lai_count * sizeof(uint32_t));
        cache->available_count = header.lai_count;
    } else {
        // Allocate space for future additions
        if (header.ci_count > 0) {
            cache->available_indices = malloc(header.ci_count * sizeof(uint32_t));
            if (!cache->available_indices) {
                free(cache->entries);
                cache->entries = NULL;
                free(file_data);
                return ESP_ERR_NO_MEM;
            }
        } else {
            cache->available_indices = NULL;
        }
        cache->available_count = 0;
    }

    free(file_data);
    return ESP_OK;
}

// ============================================================================
// Cache Lifecycle
// ============================================================================

static void save_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;

    if (!s_cache_state.initialized) return;

    // Save all dirty caches
    channel_cache_flush_all(s_cache_state.channels_path);
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

esp_err_t channel_cache_load(const char *channel_id,
                             const char *channels_path,
                             const char *vault_path,
                             channel_cache_t *cache)
{
    if (!channel_id || !channels_path || !cache) {
        return ESP_ERR_INVALID_ARG;
    }

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

    // Build file path
    char path[256];
    channel_cache_build_path(channel_id, channels_path, path, sizeof(path));

    // Try to open cache file
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "No cache file for '%s', starting empty", channel_id);
        return ESP_OK;  // Empty cache is valid
    }

    esp_err_t err;
    if (is_legacy_format(f)) {
        ESP_LOGI(TAG, "Loading legacy format for '%s'", channel_id);
        err = load_legacy_format(f, cache, vault_path);
    } else {
        ESP_LOGD(TAG, "Loading new format for '%s'", channel_id);
        err = load_new_format(f, cache);
    }

    fclose(f);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load cache for '%s': %s, starting empty",
                 channel_id, esp_err_to_name(err));
        // Clean up partial state
        free(cache->entries);
        free(cache->available_indices);
        cache->entries = NULL;
        cache->available_indices = NULL;
        cache->entry_count = 0;
        cache->available_count = 0;
        cache->dirty = false;
        return ESP_OK;  // Return success with empty cache
    }

    ESP_LOGI(TAG, "Loaded cache '%s': %zu entries, %zu available",
             channel_id, cache->entry_count, cache->available_count);
    return ESP_OK;
}

esp_err_t channel_cache_save(const channel_cache_t *cache, const char *channels_path)
{
    if (!cache || !channels_path) {
        return ESP_ERR_INVALID_ARG;
    }

    // Build paths
    char path[256];
    char temp_path[260];
    channel_cache_build_path(cache->channel_id, channels_path, path, sizeof(path));
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    // Calculate sizes
    size_t ci_size = cache->entry_count * sizeof(makapix_channel_entry_t);
    size_t lai_size = cache->available_count * sizeof(uint32_t);
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
    if (cache->available_indices && lai_size > 0) {
        memcpy(buffer + header->lai_offset, cache->available_indices, lai_size);
    }

    // Compute checksum (with checksum field zeroed)
    header->checksum = 0;
    header->checksum = channel_cache_crc32(buffer, total_size);

    // Write to temp file
    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create temp file: %s", temp_path);
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

    // Atomic rename
    if (rename(temp_path, path) != 0) {
        ESP_LOGE(TAG, "Rename failed: %s -> %s", temp_path, path);
        unlink(temp_path);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Saved cache '%s': %zu entries, %zu available",
             cache->channel_id, cache->entry_count, cache->available_count);
    return ESP_OK;
}

void channel_cache_free(channel_cache_t *cache)
{
    if (!cache) return;

    if (cache->mutex) {
        xSemaphoreTake(cache->mutex, portMAX_DELAY);
    }

    free(cache->entries);
    cache->entries = NULL;
    cache->entry_count = 0;

    free(cache->available_indices);
    cache->available_indices = NULL;
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

bool lai_add_entry(channel_cache_t *cache, uint32_t ci_index)
{
    if (!cache || ci_index >= cache->entry_count) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Check if already present
    for (size_t i = 0; i < cache->available_count; i++) {
        if (cache->available_indices[i] == ci_index) {
            xSemaphoreGive(cache->mutex);
            return false;  // Already in LAi
        }
    }

    // Ensure we have space
    if (!cache->available_indices) {
        cache->available_indices = malloc(cache->entry_count * sizeof(uint32_t));
        if (!cache->available_indices) {
            xSemaphoreGive(cache->mutex);
            return false;
        }
    }

    // Add to end
    cache->available_indices[cache->available_count++] = ci_index;
    cache->dirty = true;

    xSemaphoreGive(cache->mutex);

    ESP_LOGD(TAG, "LAi add: ci_index=%lu, count=%zu",
             (unsigned long)ci_index, cache->available_count);
    return true;
}

bool lai_remove_entry(channel_cache_t *cache, uint32_t ci_index)
{
    if (!cache || !cache->available_indices) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Find the entry
    for (size_t i = 0; i < cache->available_count; i++) {
        if (cache->available_indices[i] == ci_index) {
            // Swap with last (O(1) removal)
            cache->available_indices[i] = cache->available_indices[cache->available_count - 1];
            cache->available_count--;
            cache->dirty = true;

            xSemaphoreGive(cache->mutex);

            ESP_LOGD(TAG, "LAi remove: ci_index=%lu, count=%zu",
                     (unsigned long)ci_index, cache->available_count);
            return true;
        }
    }

    xSemaphoreGive(cache->mutex);
    return false;  // Not found
}

bool lai_contains(const channel_cache_t *cache, uint32_t ci_index)
{
    if (!cache || !cache->available_indices) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    for (size_t i = 0; i < cache->available_count; i++) {
        if (cache->available_indices[i] == ci_index) {
            xSemaphoreGive(cache->mutex);
            return true;
        }
    }

    xSemaphoreGive(cache->mutex);
    return false;
}

uint32_t lai_find_slot(const channel_cache_t *cache, uint32_t ci_index)
{
    if (!cache || !cache->available_indices) {
        return UINT32_MAX;
    }

    for (size_t i = 0; i < cache->available_count; i++) {
        if (cache->available_indices[i] == ci_index) {
            return (uint32_t)i;
        }
    }

    return UINT32_MAX;
}

size_t lai_rebuild(channel_cache_t *cache, const char *vault_path)
{
    if (!cache || !cache->entries || cache->entry_count == 0) {
        return 0;
    }

    // Ensure LAi array is allocated
    if (!cache->available_indices) {
        cache->available_indices = malloc(cache->entry_count * sizeof(uint32_t));
        if (!cache->available_indices) {
            return 0;
        }
    }

    cache->available_count = 0;
    size_t checked = 0;
    size_t found = 0;

    for (size_t i = 0; i < cache->entry_count; i++) {
        const makapix_channel_entry_t *entry = &cache->entries[i];

        // Skip playlists (they don't have direct files)
        if (entry->kind == MAKAPIX_INDEX_POST_KIND_PLAYLIST) {
            continue;
        }

        // Build vault path for this entry
        // Format: {vault_path}/{hash[0:2]}/{hash[2:4]}/{storage_key}.{ext}
        char uuid_str[37];
        bytes_to_uuid(entry->storage_key_uuid, uuid_str, sizeof(uuid_str));

        uint8_t sha256[32];
        if (storage_key_sha256(uuid_str, sha256) != ESP_OK) {
            continue;
        }

        const char *ext_strings[] = {"webp", "gif", "png", "jpg"};
        const char *ext = (entry->extension < 4) ? ext_strings[entry->extension] : "webp";

        char sha_hex[65];
        for (int j = 0; j < 32; j++) {
            sprintf(sha_hex + j * 2, "%02x", sha256[j]);
        }

        char file_path[256];
        snprintf(file_path, sizeof(file_path), "%s/%c%c/%c%c/%s.%s",
                 vault_path,
                 sha_hex[0], sha_hex[1],
                 sha_hex[2], sha_hex[3],
                 uuid_str, ext);

        checked++;

        // Check if file exists
        struct stat st;
        if (stat(file_path, &st) == 0) {
            // Also check for .404 marker
            char marker_path[264];
            snprintf(marker_path, sizeof(marker_path), "%s.404", file_path);
            if (stat(marker_path, &st) != 0) {
                // File exists and no 404 marker
                cache->available_indices[cache->available_count++] = i;
                found++;
            }
        }

        // Yield periodically to avoid watchdog
        if (checked % 100 == 0) {
            taskYIELD();
        }
    }

    cache->dirty = true;
    ESP_LOGI(TAG, "LAi rebuild: checked %zu, found %zu available", checked, found);
    return found;
}

// ============================================================================
// Ci Operations
// ============================================================================

uint32_t ci_find_by_storage_key(const channel_cache_t *cache, const uint8_t *storage_key_uuid)
{
    if (!cache || !cache->entries || !storage_key_uuid) {
        return UINT32_MAX;
    }

    for (size_t i = 0; i < cache->entry_count; i++) {
        if (memcmp(cache->entries[i].storage_key_uuid, storage_key_uuid, 16) == 0) {
            return (uint32_t)i;
        }
    }

    return UINT32_MAX;
}

uint32_t ci_find_by_post_id(const channel_cache_t *cache, int32_t post_id)
{
    if (!cache || !cache->entries) {
        return UINT32_MAX;
    }

    for (size_t i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].post_id == post_id) {
            return (uint32_t)i;
        }
    }

    return UINT32_MAX;
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
