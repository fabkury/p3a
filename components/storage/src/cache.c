#include "storage/cache.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "mbedtls/sha256.h"

#include "storage/fs.h"
#include "ff.h"

static const char *TAG = "storage_cache";

#define CACHE_DIR_BASE "/sdcard/cache"
#define CACHE_META_DIR "/sdcard/cache/meta"
#define CACHE_DATA_DIR "/sdcard/cache/data"
#define CACHE_INDEX_FILE "/sdcard/cache/meta/index.txt"

// Default limits (can be overridden via Kconfig)
#define DEFAULT_MAX_SIZE_BYTES (256 * 1024 * 1024)  // 256 MB
#define DEFAULT_MAX_ENTRIES 1024

static bool s_cache_initialized = false;
static storage_cache_stats_t s_cache_stats = {0};

/**
 * @brief Compute SHA256 hash of a string and return as hex string
 */
static esp_err_t compute_sha256_hex(const char *input, char *output)
{
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // SHA256 (not SHA224)
    mbedtls_sha256_update(&ctx, (const unsigned char *)input, strlen(input));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    
    // Convert to hex string
    for (int i = 0; i < 32; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[64] = '\0';
    
    return ESP_OK;
}

/**
 * @brief Ensure cache directory structure exists
 */
static esp_err_t ensure_cache_dirs(void)
{
    FRESULT res;
    
    // Create base cache directory using FATFS API
    res = f_mkdir("0:/cache");
    if (res != FR_OK && res != FR_EXIST) {
        ESP_LOGE(TAG, "Failed to create cache directory: FRESULT=%d", res);
        return ESP_FAIL;
    }
    if (res == FR_OK) {
        ESP_LOGI(TAG, "Created cache directory: /sdcard/cache");
    }
    
    // Create metadata directory
    res = f_mkdir("0:/cache/meta");
    if (res != FR_OK && res != FR_EXIST) {
        ESP_LOGE(TAG, "Failed to create metadata directory: FRESULT=%d", res);
        return ESP_FAIL;
    }
    if (res == FR_OK) {
        ESP_LOGI(TAG, "Created metadata directory: /sdcard/cache/meta");
    }
    
    // Create data directory
    res = f_mkdir("0:/cache/data");
    if (res != FR_OK && res != FR_EXIST) {
        ESP_LOGE(TAG, "Failed to create data directory: FRESULT=%d", res);
        return ESP_FAIL;
    }
    if (res == FR_OK) {
        ESP_LOGI(TAG, "Created data directory: /sdcard/cache/data");
    }
    
    return ESP_OK;
}

/**
 * @brief Build cache file path from hash
 */
static void build_cache_file_path(const char *hash, char *out_path, size_t max_len)
{
    snprintf(out_path, max_len, "%s/%s.bin", CACHE_DATA_DIR, hash);
}

/**
 * @brief Build metadata file path from hash
 */
static void build_meta_file_path(const char *hash, char *out_path, size_t max_len)
{
    snprintf(out_path, max_len, "%s/%s.meta", CACHE_META_DIR, hash);
}

/**
 * @brief Load cache entry metadata from file
 */
static esp_err_t load_cache_entry(const char *hash, storage_cache_entry_t *entry)
{
    char meta_path[256];
    build_meta_file_path(hash, meta_path, sizeof(meta_path));
    
    FILE *f = fopen(meta_path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // Simple text format: url_hash|original_url|file_size|timestamp|access_count
    char line[1024];
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }
    
    fclose(f);
    
    // Parse line
    strncpy(entry->url_hash, hash, sizeof(entry->url_hash) - 1);
    entry->url_hash[sizeof(entry->url_hash) - 1] = '\0';
    
    char *token = strtok(line, "|");
    if (!token) return ESP_ERR_INVALID_STATE;
    // url_hash already set
    
    token = strtok(NULL, "|");
    if (token) {
        strncpy(entry->original_url, token, sizeof(entry->original_url) - 1);
        entry->original_url[sizeof(entry->original_url) - 1] = '\0';
    }
    
    token = strtok(NULL, "|");
    if (token) {
        entry->file_size = strtoull(token, NULL, 10);
    }
    
    token = strtok(NULL, "|");
    if (token) {
        entry->timestamp = strtoull(token, NULL, 10);
    }
    
    token = strtok(NULL, "|");
    if (token) {
        entry->access_count = strtoul(token, NULL, 10);
    }
    
    return ESP_OK;
}

/**
 * @brief Save cache entry metadata to file
 */
static esp_err_t save_cache_entry(const storage_cache_entry_t *entry)
{
    char meta_path[256];
    build_meta_file_path(entry->url_hash, meta_path, sizeof(meta_path));
    
    FILE *f = fopen(meta_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open metadata file for writing");
        return ESP_FAIL;
    }
    
    fprintf(f, "%s|%s|%" PRIu64 "|%" PRIu64 "|%" PRIu32 "\n",
            entry->url_hash,
            entry->original_url,
            entry->file_size,
            entry->timestamp,
            entry->access_count);
    
    fclose(f);
    return ESP_OK;
}

/**
 * @brief Scan cache directory and rebuild statistics
 */
static esp_err_t rebuild_cache_stats(void)
{
    DIR *dir = opendir(CACHE_META_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cache metadata directory not found");
        return ESP_OK;
    }
    
    struct dirent *entry;
    uint32_t count = 0;
    uint64_t total_size = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".meta") == NULL) {
            continue;
        }
        
        // Extract hash from filename (xxxx.meta -> xxxx)
        char hash[65];
        strncpy(hash, entry->d_name, 64);
        hash[64] = '\0';
        char *dot = strrchr(hash, '.');
        if (dot) {
            *dot = '\0';
        }
        
        storage_cache_entry_t cache_entry;
        if (load_cache_entry(hash, &cache_entry) == ESP_OK) {
            count++;
            total_size += cache_entry.file_size;
        }
    }
    
    closedir(dir);
    
    s_cache_stats.total_entries = count;
    s_cache_stats.total_size_bytes = total_size;
    s_cache_stats.max_entries = DEFAULT_MAX_ENTRIES;
    s_cache_stats.max_size_bytes = DEFAULT_MAX_SIZE_BYTES;
    
    return ESP_OK;
}

esp_err_t storage_cache_init(void)
{
    if (s_cache_initialized && storage_fs_is_sd_present()) {
        ESP_LOGW(TAG, "Cache already initialized");
        return ESP_OK;
    }
    
    if (!storage_fs_is_sd_present()) {
        ESP_LOGW(TAG, "SD card not present, cache initialization deferred");
        s_cache_initialized = false;  // Reset flag if SD removed
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing cache subsystem...");
    
    esp_err_t ret = ensure_cache_dirs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create cache directories (SD may not be ready)");
        s_cache_initialized = false;
        return ESP_OK;  // Don't fail - allow retry later
    }
    
    // Rebuild statistics from existing cache
    rebuild_cache_stats();
    
    s_cache_initialized = true;
    ESP_LOGI(TAG, "Cache initialized: %u entries, %llu bytes", 
             s_cache_stats.total_entries, s_cache_stats.total_size_bytes);
    
    return ESP_OK;
}

bool storage_cache_lookup(const char *url, char *out_hash, uint64_t *out_size)
{
    if (!s_cache_initialized || !storage_fs_is_sd_present()) {
        return false;
    }
    
    char hash[65];
    if (compute_sha256_hex(url, hash) != ESP_OK) {
        return false;
    }
    
    if (out_hash) {
        strncpy(out_hash, hash, 64);
        out_hash[64] = '\0';
    }
    
    char file_path[256];
    build_cache_file_path(hash, file_path, sizeof(file_path));
    
    struct stat st;
    if (stat(file_path, &st) != 0) {
        s_cache_stats.miss_count++;
        return false;
    }
    
    // Update access time (LRU)
    storage_cache_entry_t entry;
    if (load_cache_entry(hash, &entry) == ESP_OK) {
        entry.access_count++;
        entry.timestamp = time(NULL);
        save_cache_entry(&entry);
    }
    
    if (out_size) {
        *out_size = st.st_size;
    }
    
    s_cache_stats.hit_count++;
    return true;
}

esp_err_t storage_cache_insert(const char *url, const char *source_path, const char *expected_hash)
{
    if (!s_cache_initialized || !storage_fs_is_sd_present()) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Compute hash from URL
    char hash[65];
    if (compute_sha256_hex(url, hash) != ESP_OK) {
        return ESP_FAIL;
    }
    
    // Verify expected hash matches (if provided)
    if (expected_hash && strncmp(hash, expected_hash, 64) != 0) {
        ESP_LOGE(TAG, "Hash mismatch: computed %s, expected %s", hash, expected_hash);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if already cached
    char cache_path[256];
    build_cache_file_path(hash, cache_path, sizeof(cache_path));
    
    struct stat st;
    if (stat(cache_path, &st) == 0) {
        ESP_LOGI(TAG, "Entry already cached: %s", hash);
        return ESP_OK;
    }
    
    // Copy file
    FILE *src = fopen(source_path, "rb");
    if (!src) {
        ESP_LOGE(TAG, "Failed to open source file: %s", source_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    FILE *dst = fopen(cache_path, "wb");
    if (!dst) {
        fclose(src);
        ESP_LOGE(TAG, "Failed to create cache file: %s", cache_path);
        return ESP_FAIL;
    }
    
    uint64_t file_size = 0;
    char buffer[4096];
    size_t n;
    
    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            unlink(cache_path);
            ESP_LOGE(TAG, "Failed to write cache file");
            return ESP_FAIL;
        }
        file_size += n;
    }
    
    fclose(src);
    fclose(dst);
    
    // Create metadata entry
    storage_cache_entry_t entry;
    strncpy(entry.url_hash, hash, sizeof(entry.url_hash) - 1);
    entry.url_hash[sizeof(entry.url_hash) - 1] = '\0';
    strncpy(entry.original_url, url, sizeof(entry.original_url) - 1);
    entry.original_url[sizeof(entry.original_url) - 1] = '\0';
    entry.file_size = file_size;
    entry.timestamp = time(NULL);
    entry.access_count = 1;
    
    if (save_cache_entry(&entry) != ESP_OK) {
        unlink(cache_path);
        return ESP_FAIL;
    }
    
    // Update statistics
    s_cache_stats.total_entries++;
    s_cache_stats.total_size_bytes += file_size;
    
    // Check limits and purge if needed
    if (s_cache_stats.total_entries > s_cache_stats.max_entries ||
        s_cache_stats.total_size_bytes > s_cache_stats.max_size_bytes) {
        ESP_LOGI(TAG, "Cache limits exceeded, purging oldest entries");
        storage_cache_purge(0, 0);
    }
    
    ESP_LOGI(TAG, "Cached entry: %s (%llu bytes)", hash, file_size);
    return ESP_OK;
}

esp_err_t storage_cache_get_path(const char *url, char *out_path, size_t max_len)
{
    if (!s_cache_initialized || !storage_fs_is_sd_present()) {
        return ESP_ERR_INVALID_STATE;
    }
    
    char hash[65];
    if (compute_sha256_hex(url, hash) != ESP_OK) {
        return ESP_FAIL;
    }
    
    char cache_path[256];
    build_cache_file_path(hash, cache_path, sizeof(cache_path));
    
    struct stat st;
    if (stat(cache_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    strncpy(out_path, cache_path, max_len - 1);
    out_path[max_len - 1] = '\0';
    
    return ESP_OK;
}

esp_err_t storage_cache_remove(const char *url)
{
    if (!s_cache_initialized || !storage_fs_is_sd_present()) {
        return ESP_ERR_INVALID_STATE;
    }
    
    char hash[65];
    if (compute_sha256_hex(url, hash) != ESP_OK) {
        return ESP_FAIL;
    }
    
    char cache_path[256];
    build_cache_file_path(hash, cache_path, sizeof(cache_path));
    
    char meta_path[256];
    build_meta_file_path(hash, meta_path, sizeof(meta_path));
    
    // Load entry to get file size for stats
    storage_cache_entry_t entry;
    uint64_t file_size = 0;
    if (load_cache_entry(hash, &entry) == ESP_OK) {
        file_size = entry.file_size;
    }
    
    // Remove files
    unlink(cache_path);
    unlink(meta_path);
    
    // Update statistics
    if (s_cache_stats.total_entries > 0) {
        s_cache_stats.total_entries--;
    }
    if (s_cache_stats.total_size_bytes >= file_size) {
        s_cache_stats.total_size_bytes -= file_size;
    }
    
    return ESP_OK;
}

esp_err_t storage_cache_get_stats(storage_cache_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(stats, &s_cache_stats, sizeof(storage_cache_stats_t));
    return ESP_OK;
}

esp_err_t storage_cache_purge(uint64_t target_size_bytes, uint32_t target_count)
{
    if (!s_cache_initialized || !storage_fs_is_sd_present()) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint64_t size_limit = (target_size_bytes > 0) ? target_size_bytes : s_cache_stats.max_size_bytes;
    uint32_t count_limit = (target_count > 0) ? target_count : s_cache_stats.max_entries;
    
    // Simple LRU: collect all entries, sort by timestamp, remove oldest
    DIR *dir = opendir(CACHE_META_DIR);
    if (!dir) {
        return ESP_OK;
    }
    
    struct dirent *entry;
    storage_cache_entry_t entries[1024];
    uint32_t entry_count = 0;
    
    // Collect all entries
    while ((entry = readdir(dir)) != NULL && entry_count < 1024) {
        if (strstr(entry->d_name, ".meta") == NULL) {
            continue;
        }
        
        char hash[65];
        strncpy(hash, entry->d_name, 64);
        hash[64] = '\0';
        char *dot = strrchr(hash, '.');
        if (dot) {
            *dot = '\0';
        }
        
        if (load_cache_entry(hash, &entries[entry_count]) == ESP_OK) {
            entry_count++;
        }
    }
    closedir(dir);
    
    // Simple bubble sort by timestamp (oldest first)
    for (uint32_t i = 0; i < entry_count - 1; i++) {
        for (uint32_t j = 0; j < entry_count - i - 1; j++) {
            if (entries[j].timestamp > entries[j + 1].timestamp) {
                storage_cache_entry_t temp = entries[j];
                entries[j] = entries[j + 1];
                entries[j + 1] = temp;
            }
        }
    }
    
    // Remove oldest entries until under limits
    uint32_t removed = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (s_cache_stats.total_entries <= count_limit &&
            s_cache_stats.total_size_bytes <= size_limit) {
            break;
        }
        
        // Build original URL from entry for removal
        storage_cache_remove(entries[i].original_url);
        removed++;
    }
    
    if (removed > 0) {
        ESP_LOGI(TAG, "Purged %u cache entries", removed);
    }
    
    return ESP_OK;
}

esp_err_t storage_cache_clear(void)
{
    if (!s_cache_initialized || !storage_fs_is_sd_present()) {
        return ESP_ERR_INVALID_STATE;
    }
    
    DIR *dir = opendir(CACHE_META_DIR);
    if (!dir) {
        return ESP_OK;
    }
    
    struct dirent *entry;
    uint32_t removed = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".meta") == NULL) {
            continue;
        }
        
        char hash[65];
        strncpy(hash, entry->d_name, 64);
        hash[64] = '\0';
        char *dot = strrchr(hash, '.');
        if (dot) {
            *dot = '\0';
        }
        
        storage_cache_entry_t cache_entry;
        if (load_cache_entry(hash, &cache_entry) == ESP_OK) {
            storage_cache_remove(cache_entry.original_url);
            removed++;
        }
    }
    
    closedir(dir);
    
    s_cache_stats.total_entries = 0;
    s_cache_stats.total_size_bytes = 0;
    
    ESP_LOGI(TAG, "Cleared %u cache entries", removed);
    return ESP_OK;
}

bool storage_cache_is_initialized(void)
{
    return s_cache_initialized && storage_fs_is_sd_present();
}

