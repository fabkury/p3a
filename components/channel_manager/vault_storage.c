// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "vault_storage.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

static const char *TAG = "vault_storage";

/**
 * @brief Internal vault structure
 */
struct vault_storage_s {
    char *base_path;
};

// Extension strings
static const char *s_ext_strings[] = {
    [VAULT_FILE_WEBP] = "webp",
    [VAULT_FILE_GIF]  = "gif",
    [VAULT_FILE_PNG]  = "png",
    [VAULT_FILE_JPEG] = "jpg",
};

// Helper: hex char to nibble
static int hex_to_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Helper: ensure directory exists
static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;  // Exists but not a directory
    }
    
    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) {
            return ESP_OK;  // Race condition, directory now exists
        }
        ESP_LOGE(TAG, "Failed to create directory: %s (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Helper: build path for SHA256
static void build_file_path(vault_handle_t handle, const uint8_t *sha256, 
                            const char *ext, char *out, size_t out_len)
{
    char hex[65];
    vault_format_sha256(sha256, hex, sizeof(hex));
    
    snprintf(out, out_len, "%s/%02x/%02x/%s.%s",
             handle->base_path, sha256[0], sha256[1], hex, ext);
}

// Note: build_dir_path removed - unused. Keeping comment for future reference.
// Directory path can be built as: "%s/%02x/%02x" with base_path, sha256[0], sha256[1]

// Helper: ensure shard directories exist
static esp_err_t ensure_shard_dirs(vault_handle_t handle, const uint8_t *sha256)
{
    char path[256];
    
    // Ensure base path exists
    if (ensure_dir(handle->base_path) != ESP_OK) {
        return ESP_FAIL;
    }
    
    // Ensure first level (ab)
    snprintf(path, sizeof(path), "%s/%02x", handle->base_path, sha256[0]);
    if (ensure_dir(path) != ESP_OK) {
        return ESP_FAIL;
    }
    
    // Ensure second level (ab/cd)
    snprintf(path, sizeof(path), "%s/%02x/%02x", handle->base_path, sha256[0], sha256[1]);
    if (ensure_dir(path) != ESP_OK) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Helper: clean up orphan .tmp file for a given path (lazy cleanup)
static void cleanup_tmp_file(const char *path)
{
    if (!path) return;
    
    // Build temp file path
    char tmp_path[280];
    size_t path_len = strlen(path);
    if (path_len + 4 >= sizeof(tmp_path)) {
        return;  // Path too long
    }
    
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    
    // Check if .tmp file exists and remove it
    struct stat st;
    if (stat(tmp_path, &st) == 0 && S_ISREG(st.st_mode)) {
        ESP_LOGD(TAG, "Removing orphan temp file: %s", tmp_path);
        if (unlink(tmp_path) != 0) {
            ESP_LOGW(TAG, "Failed to remove temp file: %s (errno=%d)", tmp_path, errno);
        }
    }
}

// Helper: atomic write (write to .tmp, fsync, rename)
static esp_err_t atomic_write(const char *final_path, const void *data, size_t len)
{
    char tmp_path[280];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    
    // Write to temporary file
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open temp file: %s (errno=%d)", tmp_path, errno);
        return ESP_FAIL;
    }
    
    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        ESP_LOGE(TAG, "Failed to write data: %zu/%zu bytes", written, len);
        fclose(f);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    
    // Flush and sync
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    
    // Atomic rename
    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGE(TAG, "Failed to rename: %s -> %s (errno=%d)", 
                 tmp_path, final_path, errno);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Public API implementation

esp_err_t vault_init(const char *base_path, vault_handle_t *out_handle)
{
    if (!base_path || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    struct vault_storage_s *vault = calloc(1, sizeof(struct vault_storage_s));
    if (!vault) {
        return ESP_ERR_NO_MEM;
    }
    
    vault->base_path = strdup(base_path);
    if (!vault->base_path) {
        free(vault);
        return ESP_ERR_NO_MEM;
    }
    
    // Ensure base directory exists
    if (ensure_dir(base_path) != ESP_OK) {
        free(vault->base_path);
        free(vault);
        return ESP_FAIL;
    }
    
    *out_handle = vault;
    ESP_LOGI(TAG, "Vault initialized at: %s", base_path);
    return ESP_OK;
}

void vault_deinit(vault_handle_t handle)
{
    if (!handle) return;
    
    free(handle->base_path);
    free(handle);
}

bool vault_file_exists(vault_handle_t handle, const uint8_t *sha256, vault_file_type_t type)
{
    if (!handle || !sha256 || type > VAULT_FILE_JPEG) {
        return false;
    }
    
    char path[280];
    build_file_path(handle, sha256, s_ext_strings[type], path, sizeof(path));
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    cleanup_tmp_file(path);
    
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t vault_get_file_path(vault_handle_t handle, 
                               const uint8_t *sha256, 
                               vault_file_type_t type,
                               char *out_path, 
                               size_t out_path_len)
{
    if (!handle || !sha256 || type > VAULT_FILE_JPEG || !out_path || out_path_len < 100) {
        return ESP_ERR_INVALID_ARG;
    }
    
    build_file_path(handle, sha256, s_ext_strings[type], out_path, out_path_len);
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    cleanup_tmp_file(out_path);
    
    return ESP_OK;
}

esp_err_t vault_store_file(vault_handle_t handle,
                           const uint8_t *sha256,
                           vault_file_type_t type,
                           const void *data,
                           size_t data_len)
{
    if (!handle || !sha256 || type > VAULT_FILE_JPEG || !data || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Ensure directories exist
    esp_err_t err = ensure_shard_dirs(handle, sha256);
    if (err != ESP_OK) {
        return err;
    }
    
    // Build path
    char path[280];
    build_file_path(handle, sha256, s_ext_strings[type], path, sizeof(path));
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    cleanup_tmp_file(path);
    
    // Check if already exists
    struct stat st;
    if (stat(path, &st) == 0) {
        ESP_LOGD(TAG, "File already exists: %s", path);
        return ESP_OK;  // Deduplicated
    }
    
    // Write atomically
    err = atomic_write(path, data, data_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Stored: %s (%zu bytes)", path, data_len);
    }
    
    return err;
}

esp_err_t vault_delete_file(vault_handle_t handle,
                            const uint8_t *sha256,
                            vault_file_type_t type)
{
    if (!handle || !sha256 || type > VAULT_FILE_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char path[280];
    build_file_path(handle, sha256, s_ext_strings[type], path, sizeof(path));
    
    if (unlink(path) != 0) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "Failed to delete: %s (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Deleted: %s", path);
    return ESP_OK;
}

bool vault_sidecar_exists(vault_handle_t handle, const uint8_t *sha256)
{
    if (!handle || !sha256) {
        return false;
    }
    
    char path[280];
    build_file_path(handle, sha256, "json", path, sizeof(path));
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    cleanup_tmp_file(path);
    
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t vault_get_sidecar_path(vault_handle_t handle,
                                  const uint8_t *sha256,
                                  char *out_path,
                                  size_t out_path_len)
{
    if (!handle || !sha256 || !out_path || out_path_len < 100) {
        return ESP_ERR_INVALID_ARG;
    }
    
    build_file_path(handle, sha256, "json", out_path, out_path_len);
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    cleanup_tmp_file(out_path);
    
    return ESP_OK;
}

esp_err_t vault_store_sidecar(vault_handle_t handle,
                               const uint8_t *sha256,
                               const char *json_str)
{
    if (!handle || !sha256 || !json_str) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Ensure directories exist
    esp_err_t err = ensure_shard_dirs(handle, sha256);
    if (err != ESP_OK) {
        return err;
    }
    
    // Build path
    char path[280];
    build_file_path(handle, sha256, "json", path, sizeof(path));
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    cleanup_tmp_file(path);
    
    // Write atomically
    return atomic_write(path, json_str, strlen(json_str));
}

esp_err_t vault_read_sidecar(vault_handle_t handle,
                              const uint8_t *sha256,
                              char *out_json,
                              size_t out_json_len)
{
    if (!handle || !sha256 || !out_json || out_json_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char path[280];
    build_file_path(handle, sha256, "json", path, sizeof(path));
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    cleanup_tmp_file(path);
    
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_FAIL;
    }
    
    size_t read = fread(out_json, 1, out_json_len - 1, f);
    out_json[read] = '\0';
    fclose(f);
    
    return ESP_OK;
}

esp_err_t vault_delete_sidecar(vault_handle_t handle, const uint8_t *sha256)
{
    if (!handle || !sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char path[280];
    build_file_path(handle, sha256, "json", path, sizeof(path));
    
    if (unlink(path) != 0 && errno != ENOENT) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t vault_get_stats(vault_handle_t handle, vault_stats_t *out_stats)
{
    if (!handle || !out_stats) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(out_stats, 0, sizeof(*out_stats));
    
    // This is a simplified implementation - scanning all directories
    // In production, you might want to cache these stats
    ESP_LOGW(TAG, "vault_get_stats: scanning directories (may be slow)");
    
    // Iterate through first level directories (00-ff)
    DIR *d1 = opendir(handle->base_path);
    if (!d1) {
        return ESP_OK;  // Empty vault
    }
    
    struct dirent *e1;
    char path_l2[320];
    
    while ((e1 = readdir(d1)) != NULL) {
        if (e1->d_name[0] == '.') continue;
        if (strlen(e1->d_name) != 2) continue;
        
        int ret = snprintf(path_l2, sizeof(path_l2), "%s/%s", handle->base_path, e1->d_name);
        if (ret < 0 || ret >= (int)sizeof(path_l2)) continue;
        
        DIR *d2 = opendir(path_l2);
        if (!d2) continue;
        
        struct dirent *e2;
        char path_l3[320];
        
        while ((e2 = readdir(d2)) != NULL) {
            if (e2->d_name[0] == '.') continue;
            if (strlen(e2->d_name) != 2) continue;
            
            ret = snprintf(path_l3, sizeof(path_l3), "%s/%s", path_l2, e2->d_name);
            if (ret < 0 || ret >= (int)sizeof(path_l3)) continue;
            
            DIR *d3 = opendir(path_l3);
            if (!d3) continue;
            
            struct dirent *e3;
            
            while ((e3 = readdir(d3)) != NULL) {
                if (e3->d_name[0] == '.') continue;
                
                size_t len = strlen(e3->d_name);
                if (len > 5 && strcmp(e3->d_name + len - 5, ".json") == 0) {
                    out_stats->total_sidecars++;
                } else if (len > 4) {
                    out_stats->total_files++;
                }
            }
            
            closedir(d3);
        }
        
        closedir(d2);
    }
    
    closedir(d1);
    
    return ESP_OK;
}

esp_err_t vault_parse_sha256(const char *hex_str, uint8_t *out_sha256)
{
    if (!hex_str || !out_sha256 || strlen(hex_str) != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    
    for (int i = 0; i < 32; i++) {
        int hi = hex_to_nibble(hex_str[i*2]);
        int lo = hex_to_nibble(hex_str[i*2 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out_sha256[i] = (hi << 4) | lo;
    }
    
    return ESP_OK;
}

esp_err_t vault_format_sha256(const uint8_t *sha256, char *out_hex, size_t out_len)
{
    if (!sha256 || !out_hex || out_len < 65) {
        return ESP_ERR_INVALID_ARG;
    }
    
    for (int i = 0; i < 32; i++) {
        snprintf(out_hex + i*2, 3, "%02x", sha256[i]);
    }
    
    return ESP_OK;
}

// ============================================================================
// Power-loss recovery: lazy .tmp file cleanup
// ============================================================================
// 
// Instead of scanning all files on boot (which doesn't scale to 32K+ files),
// we use lazy cleanup: when accessing file X, we check if X.tmp exists and
// remove it before proceeding. This approach:
// - Zero boot-time cost
// - O(1) per file access
// - Scales to any number of files
// - Self-healing during normal operation
//
// The cleanup_tmp_file() helper is called by:
// - vault_file_exists()
// - vault_get_file_path()
// - vault_store_file()
// - vault_sidecar_exists()
// - vault_get_sidecar_path()
// - vault_store_sidecar()
// - vault_read_sidecar()
// ============================================================================

