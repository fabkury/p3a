// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "fresh_boot.h"
#include "sd_path.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "fresh_boot";

/**
 * @brief Recursively delete a directory and all its contents
 * 
 * @param path Path to directory to delete
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t recursive_rmdir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        if (errno == ENOENT) {
            // Directory doesn't exist, nothing to delete
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to open directory %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    struct dirent *entry;
    esp_err_t result = ESP_OK;

    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Build full path
        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (ret < 0 || ret >= sizeof(full_path)) {
            ESP_LOGE(TAG, "Path too long: %s/%s", path, entry->d_name);
            result = ESP_ERR_INVALID_SIZE;
            continue;
        }

        // Check if it's a directory
        struct stat st;
        if (stat(full_path, &st) != 0) {
            ESP_LOGW(TAG, "Failed to stat %s: %s", full_path, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            // Recursively delete subdirectory
            esp_err_t err = recursive_rmdir(full_path);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to delete subdirectory %s", full_path);
                result = err;
            }
        } else {
            // Delete file
            if (unlink(full_path) != 0) {
                ESP_LOGW(TAG, "Failed to delete file %s: %s", full_path, strerror(errno));
                result = ESP_FAIL;
            } else {
                ESP_LOGD(TAG, "Deleted file: %s", full_path);
            }
        }
    }

    closedir(dir);

    // Delete the now-empty directory
    if (rmdir(path) != 0) {
        if (errno != ENOENT) {
            ESP_LOGE(TAG, "Failed to delete directory %s: %s", path, strerror(errno));
            return ESP_FAIL;
        }
    } else {
        ESP_LOGD(TAG, "Deleted directory: %s", path);
    }

    return result;
}

/**
 * @brief Erase a single NVS namespace
 * 
 * @param namespace_name Name of the NVS namespace to erase
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t erase_nvs_namespace(const char *namespace_name)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(namespace_name, NVS_READWRITE, &handle);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist, nothing to erase
        ESP_LOGD(TAG, "NVS namespace '%s' does not exist, skipping", namespace_name);
        return ESP_OK;
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", namespace_name, esp_err_to_name(err));
        return err;
    }

    // Erase all keys in the namespace
    err = nvs_erase_all(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS namespace '%s': %s", namespace_name, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    // Commit the changes
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS namespace '%s': %s", namespace_name, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Erased NVS namespace: %s", namespace_name);
    return ESP_OK;
}

esp_err_t fresh_boot_erase_nvs(void)
{
    ESP_LOGW(TAG, "Starting fresh boot NVS erase...");

    esp_err_t result = ESP_OK;

    // List of p3a NVS namespaces to erase
    const char *namespaces[] = {
        "p3a_boot",
        "appcfg",
        "p3a_state",
        "makapix"
    };

    // Erase each namespace
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); i++) {
        esp_err_t err = erase_nvs_namespace(namespaces[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to erase namespace '%s'", namespaces[i]);
            result = err;
        }
    }

#if CONFIG_P3A_FORCE_FRESH_WIFI
    // Also erase WiFi credentials if configured
    esp_err_t err = erase_nvs_namespace("wifi_config");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to erase WiFi namespace");
        result = err;
    }
#else
    ESP_LOGI(TAG, "WiFi credentials preserved (CONFIG_P3A_FORCE_FRESH_WIFI not enabled)");
#endif

    if (result == ESP_OK) {
        ESP_LOGW(TAG, "Fresh boot NVS erase completed successfully");
    } else {
        ESP_LOGW(TAG, "Fresh boot NVS erase completed with some errors");
    }

    return result;
}

esp_err_t fresh_boot_erase_sdcard(void)
{
    ESP_LOGW(TAG, "Starting fresh boot SD card erase...");

    // Use the default root path
    const char *p3a_root = SD_PATH_DEFAULT_ROOT;
    
    // Check if directory exists
    struct stat st;
    if (stat(p3a_root, &st) != 0) {
        if (errno == ENOENT) {
            ESP_LOGI(TAG, "Directory %s does not exist, nothing to delete", p3a_root);
            // Create the empty directory
            if (mkdir(p3a_root, 0755) != 0) {
                ESP_LOGE(TAG, "Failed to create directory %s: %s", p3a_root, strerror(errno));
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Created fresh directory: %s", p3a_root);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to stat %s: %s", p3a_root, strerror(errno));
        return ESP_FAIL;
    }

    // Recursively delete the directory
    ESP_LOGW(TAG, "Deleting directory tree: %s", p3a_root);
    esp_err_t err = recursive_rmdir(p3a_root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete directory tree: %s", p3a_root);
        return err;
    }

    // Recreate the empty root directory
    if (mkdir(p3a_root, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to recreate directory %s: %s", p3a_root, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Fresh boot SD card erase completed: %s", p3a_root);
    return ESP_OK;
}

