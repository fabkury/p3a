// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "sd_path.h"
#include "config_store.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "sd_path";

// Cached root path (loaded once at init, changes require reboot)
static char s_root_path[SD_PATH_ROOT_MAX_LEN] = SD_PATH_DEFAULT_ROOT;
static bool s_initialized = false;

esp_err_t sd_path_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // Try to load from config store
    char *stored_path = NULL;
    esp_err_t err = config_store_get_sdcard_root(&stored_path);
    
    if (err == ESP_OK && stored_path != NULL && stored_path[0] != '\0') {
        // Check if path already has /sdcard prefix (for backward compatibility)
        if (strncmp(stored_path, "/sdcard/", 8) == 0) {
            // Already has /sdcard prefix, use as-is
            strlcpy(s_root_path, stored_path, sizeof(s_root_path));
            ESP_LOGI(TAG, "Using configured root: %s", s_root_path);
        } else if (stored_path[0] == '/' && strlen(stored_path) > 1) {
            // User-friendly path (e.g., /p3a), prepend /sdcard
            // Check if the combined path will fit
            size_t stored_len = strlen(stored_path);
            size_t total_len = 7 + stored_len; // "/sdcard" (7) + user path
            if (total_len >= sizeof(s_root_path)) {
                ESP_LOGW(TAG, "Configured root path too long after prepending /sdcard: %s", stored_path);
                strlcpy(s_root_path, SD_PATH_DEFAULT_ROOT, sizeof(s_root_path));
            } else {
                snprintf(s_root_path, sizeof(s_root_path), "/sdcard%s", stored_path);
                ESP_LOGI(TAG, "Using configured root: %s (from user path: %s)", s_root_path, stored_path);
            }
        } else {
            ESP_LOGW(TAG, "Invalid root path in config (must start with / and not be empty): %s", stored_path);
            strlcpy(s_root_path, SD_PATH_DEFAULT_ROOT, sizeof(s_root_path));
        }
        free(stored_path);
    } else {
        strlcpy(s_root_path, SD_PATH_DEFAULT_ROOT, sizeof(s_root_path));
        ESP_LOGI(TAG, "Using default root: %s", s_root_path);
    }

    s_initialized = true;
    return ESP_OK;
}

const char *sd_path_get_root(void)
{
    if (!s_initialized) {
        sd_path_init();
    }
    return s_root_path;
}

esp_err_t sd_path_get_subdir(const char *subdir, char *out_path, size_t out_len)
{
    if (!subdir || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *root = sd_path_get_root();
    int written = snprintf(out_path, out_len, "%s/%s", root, subdir);
    
    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t sd_path_get_animations(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("animations", out_path, out_len);
}

esp_err_t sd_path_get_vault(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("vault", out_path, out_len);
}

esp_err_t sd_path_get_channel(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("channel", out_path, out_len);
}

esp_err_t sd_path_get_playlists(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("playlists", out_path, out_len);
}

esp_err_t sd_path_get_downloads(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("downloads", out_path, out_len);
}

esp_err_t sd_path_get_giphy(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("giphy", out_path, out_len);
}

esp_err_t sd_path_set_root(const char *root_path)
{
    if (!root_path || root_path[0] == '\0') {
        ESP_LOGE(TAG, "Root path cannot be empty");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate path starts with /
    if (root_path[0] != '/') {
        ESP_LOGE(TAG, "Root path must start with /");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate path is not just "/"
    if (strlen(root_path) == 1) {
        ESP_LOGE(TAG, "Root path cannot be just '/' - must specify at least one folder");
        return ESP_ERR_INVALID_ARG;
    }

    // Check for directory traversal attempts
    if (strstr(root_path, "..") != NULL) {
        ESP_LOGE(TAG, "Root path cannot contain '..'");
        return ESP_ERR_INVALID_ARG;
    }

    // If path already starts with /sdcard/, strip it for storage (user-friendly format)
    const char *path_to_store = root_path;
    if (strncmp(root_path, "/sdcard/", 8) == 0) {
        path_to_store = root_path + 7; // Skip "/sdcard", keep the leading "/"
        ESP_LOGI(TAG, "Stripping /sdcard prefix for storage: %s -> %s", root_path, path_to_store);
    }

    // Validate final length (after potential stripping)
    if (strlen(path_to_store) >= SD_PATH_ROOT_MAX_LEN) {
        ESP_LOGE(TAG, "Root path too long (max %d chars)", SD_PATH_ROOT_MAX_LEN - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    // Save to config store (user-friendly format: /p3a not /sdcard/p3a)
    esp_err_t err = config_store_set_sdcard_root(path_to_store);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save root path: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Root path saved: %s (reboot required)", path_to_store);
    return ESP_OK;
}

static esp_err_t ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Path exists but is not a directory: %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Creating directory: %s", path);
    if (mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create directory %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t sd_path_ensure_directories(void)
{
    const char *root = sd_path_get_root();
    char path[128];
    esp_err_t err;

    // Create root directory first
    err = ensure_directory(root);
    if (err != ESP_OK) {
        return err;
    }

    // Create subdirectories
    const char *subdirs[] = {"animations", "vault", "channel", "playlists", "downloads", "giphy"};
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, subdirs[i]);
        err = ensure_directory(path);
        if (err != ESP_OK) {
            // Log but continue - some directories might already exist
            ESP_LOGW(TAG, "Could not create %s", path);
        }
    }

    ESP_LOGI(TAG, "SD directories ensured under %s", root);
    return ESP_OK;
}

