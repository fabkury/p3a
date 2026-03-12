// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "channel_cache.h"
#include "channel_cache_internal.h"
#include "freertos/task.h"
#include "config_store.h"
#include "event_bus.h"
#include "makapix_channel_internal.h"
#include "makapix_channel_utils.h"
#include "playlist_manager.h"
#include "play_scheduler_types.h"   // PS_MAX_CHANNELS, ps_channel_type_t
#include "play_scheduler.h"        // ps_get_display_name()
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "channel_cache";

// Note: psram_malloc, psram_calloc, psram_strdup are provided by psram_alloc.h
// (included via channel_cache.h)

// ============================================================================
// Configurable Max Entries
// ============================================================================

uint32_t channel_cache_get_max_entries(void)
{
    return config_store_get_channel_cache_size();
}

// ============================================================================
// Global State
// ============================================================================

#define MAX_REGISTERED_CACHES PS_MAX_CHANNELS

static struct {
    bool initialized;
    TimerHandle_t save_timer;
    channel_cache_t *registered[MAX_REGISTERED_CACHES];
    size_t registered_count;
    char channels_path[128];  // Stored for timer callback
    SemaphoreHandle_t registry_mutex;
    TickType_t first_dirty_tick;  // Tick when first cache became dirty (0 = none)
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
    // channel_id is a hex hash — always filesystem-safe, no sanitization needed
    snprintf(out, out_len, "%s/%s.cache", channels_path, channel_id);
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

    // Validate version - reject old versions to force complete rebuild (no migration)
    if (header.version != CHANNEL_CACHE_VERSION) {
        ESP_LOGI(TAG, "Cache version %u != %u, discarding and rebuilding from scratch",
                 header.version, CHANNEL_CACHE_VERSION);
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

    uint8_t *file_data = psram_malloc(file_size);
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
        cache->entries = psram_malloc(header.ci_count * sizeof(makapix_channel_entry_t));
        if (!cache->entries) {
            free(file_data);
            return ESP_ERR_NO_MEM;
        }
        memcpy(cache->entries, file_data + header.ci_offset,
               header.ci_count * sizeof(makapix_channel_entry_t));
        cache->entry_count = header.ci_count;

        // Validate loaded entries for corruption
        for (size_t i = 0; i < cache->entry_count; i++) {
            makapix_channel_entry_t *e = &cache->entries[i];

            // Zero post_id is always invalid (reserved for "no post_id")
            if (e->post_id == 0) {
                ESP_LOGW(TAG, "Corrupt entry[%zu]: invalid post_id=0", i);
                free(cache->entries);
                cache->entries = NULL;
                cache->entry_count = 0;
                free(file_data);
                return ESP_ERR_INVALID_STATE;
            }

            // Validate kind is in valid range (0=ARTWORK, 1=PLAYLIST)
            if (e->kind > 1) {
                ESP_LOGW(TAG, "Corrupt entry[%zu]: invalid kind=%d", i, e->kind);
                free(cache->entries);
                cache->entries = NULL;
                cache->entry_count = 0;
                free(file_data);
                return ESP_ERR_INVALID_STATE;
            }

            // Validate extension is in valid range (0-4 are valid)
            if (e->extension > 4) {
                ESP_LOGW(TAG, "Corrupt entry[%zu]: invalid extension=%d", i, e->extension);
                free(cache->entries);
                cache->entries = NULL;
                cache->entry_count = 0;
                free(file_data);
                return ESP_ERR_INVALID_STATE;
            }
        }
    } else {
        cache->entries = NULL;
        cache->entry_count = 0;
    }

    // Build Ci hash table
    ci_rebuild_hash_tables(cache);

    // Allocate and copy LAi post_ids (v20+ stores post_ids, not ci_indices)
    if (header.lai_count > 0) {
        // Note: validation above ensures lai_count <= ci_count
        cache->available_post_ids = psram_malloc(header.ci_count * sizeof(int32_t));
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
        cache->available_capacity = header.ci_count;
    } else {
        // Allocate space for future additions
        if (header.ci_count > 0) {
            cache->available_post_ids = psram_malloc(header.ci_count * sizeof(int32_t));
            if (!cache->available_post_ids) {
                ci_hash_free(cache);
                free(cache->entries);
                cache->entries = NULL;
                free(file_data);
                return ESP_ERR_NO_MEM;
            }
            cache->available_capacity = header.ci_count;
        } else {
            cache->available_post_ids = NULL;
            cache->available_capacity = 0;
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

esp_err_t channel_cache_load(const char *channel_id,
                             uint8_t channel_type,
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
    cache->channel_type = channel_type;
    cache->cache_version = CHANNEL_CACHE_VERSION;

    cache->mutex = xSemaphoreCreateMutex();
    if (!cache->mutex) {
        return ESP_ERR_NO_MEM;
    }

    // Store channels_path for debounced saves
    strncpy(s_cache_state.channels_path, channels_path,
            sizeof(s_cache_state.channels_path) - 1);

    // Build cache file path (.cache is the only persistence format)
    char cache_path[256];
    channel_cache_build_path(channel_id, channels_path, cache_path, sizeof(cache_path));

    // Try to open cache file
    FILE *f = fopen(cache_path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "No cache for '%s', starting empty (server refresh will populate)", channel_id);

        // Pre-allocate LAi array to support incremental batch updates
        // This allows channel_cache_merge_posts() to work correctly on empty cache
        size_t max_entries = CHANNEL_CACHE_MAX_ENTRIES;
        cache->available_post_ids = psram_malloc(max_entries * sizeof(int32_t));  // Match configured limit
        cache->available_count = 0;
        cache->available_capacity = cache->available_post_ids ? max_entries : 0;

        return ESP_OK;  // Empty cache is valid
    }

    {
        char _dn[64];
        ps_get_display_name(channel_id, _dn, sizeof(_dn));
        ESP_LOGI(TAG, "Loading cache for '%s'", _dn);
    }
    esp_err_t err = load_new_format(f, cache);
    fclose(f);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded cache '%s': %zu entries, %zu available",
                 channel_id, cache->entry_count, cache->available_count);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Cache file invalid for '%s': %s, deleting and starting fresh",
             channel_id, esp_err_to_name(err));
    unlink(cache_path);

    // Cache corrupt or migration failed - start empty
    // Pre-allocate LAi array for batch updates
    size_t max_entries_fallback = CHANNEL_CACHE_MAX_ENTRIES;
    cache->available_post_ids = psram_malloc(max_entries_fallback * sizeof(int32_t));
    cache->available_count = 0;
    cache->available_capacity = cache->available_post_ids ? max_entries_fallback : 0;

    return ESP_OK;  // Empty cache is valid
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
    uint8_t *buffer = psram_malloc(total_size);
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

    {
        char _dn[64];
        ps_get_display_name(cache->channel_id, _dn, sizeof(_dn));
        ESP_LOGI(TAG, "Saved cache '%s': %zu entries, %zu available",
                 _dn, cache->entry_count, cache->available_count);
    }
    return ESP_OK;
}

void channel_cache_free(channel_cache_t *cache)
{
    if (!cache) return;

    if (cache->mutex) {
        xSemaphoreTake(cache->mutex, portMAX_DELAY);
    }

    // Free Ci hash table
    ci_hash_free(cache);

    free(cache->entries);
    cache->entries = NULL;
    cache->entry_count = 0;

    // Free LAi hash table
    lai_hash_free(cache);

    free(cache->available_post_ids);
    cache->available_post_ids = NULL;
    cache->available_count = 0;
    cache->available_capacity = 0;

    if (cache->mutex) {
        SemaphoreHandle_t mutex = cache->mutex;
        cache->mutex = NULL;
        xSemaphoreGive(mutex);
        vSemaphoreDelete(mutex);
    }
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

    // Record tick of first dirty mark (since last flush)
    if (s_cache_state.first_dirty_tick == 0) {
        s_cache_state.first_dirty_tick = xTaskGetTickCount() | 1;  // Ensure non-zero
    }

    // If max delay exceeded, flush immediately instead of resetting debounce
    TickType_t elapsed = xTaskGetTickCount() - s_cache_state.first_dirty_tick;
    if (elapsed >= pdMS_TO_TICKS(CHANNEL_CACHE_SAVE_MAX_DELAY_MS)) {
        event_bus_emit_simple(P3A_EVENT_CACHE_FLUSH);
        return;
    }

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

    // Reset max-delay tracker after flush
    s_cache_state.first_dirty_tick = 0;

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

    // Flush dirty cache before unregistering to prevent data loss on channel switch.
    // Done outside registry mutex to avoid holding it during SD card I/O.
    if (cache->dirty && s_cache_state.channels_path[0] != '\0') {
        esp_err_t err = channel_cache_save(cache, s_cache_state.channels_path);
        if (err == ESP_OK) {
            cache->dirty = false;
            ESP_LOGD(TAG, "Flushed dirty cache '%s' before unregister", cache->channel_id);
        } else {
            ESP_LOGW(TAG, "Failed to flush cache '%s' before unregister: %s",
                     cache->channel_id, esp_err_to_name(err));
        }
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
