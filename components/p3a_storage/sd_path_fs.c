// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// The file-system half of sd_path.h: creating directories.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "sd_health.h"
#include "sd_path.h"
#include "sd_repair.h"

static const char *TAG = "sd_path";

static esp_err_t ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "%s exists but is not a directory", path);
        return ESP_FAIL;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", path, strerror(errno));
        // mkdir is the first write most save paths attempt, ahead of
        // fs_atomic. Report it here, so that a failing card is noticed even
        // when the caller gives up before reaching the atomic write.
        sd_health_report_write_failure(path, errno);
        return ESP_FAIL;
    }
    sd_health_report_write_ok(path);
    return ESP_OK;
}

esp_err_t sd_path_ensure_directories(void)
{
    const char *root = sd_path_get_root();
    esp_err_t err = ensure_directory(root);
    if (err != ESP_OK) {
        return err;
    }
    static const char *const subdirs[] = {"animations", "temporary"};
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        char path[SD_PATH_ROOT_MAX_LEN + 16];
        snprintf(path, sizeof(path), "%s/%s", root, subdirs[i]);
        if (ensure_directory(path) != ESP_OK) {
            result = ESP_FAIL;  // keep going: the others may still be creatable
        }
    }
    return result;
}

esp_err_t sd_path_ensure_parent_dirs(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char tmp[256];
    if (snprintf(tmp, sizeof(tmp), "%s", filepath) >= (int)sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *slash = tmp + 1;  // skip the leading "/"
    while ((slash = strchr(slash, '/')) != NULL) {
        *slash = '\0';
        struct stat st;
        if (stat(tmp, &st) != 0) {
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                // ENOENT here means the parent hides its own entries (a
                // directory the card overwrote): sd_repair quarantines it and
                // recreates the whole missing chain.
                if ((errno == ENOENT || errno == EACCES) && sd_repair_heal_for_write(filepath) == ESP_OK) {
                    ESP_LOGW(TAG, "parent directory repaired for %s", filepath);
                    sd_health_report_write_ok(filepath);
                    return ESP_OK;
                }
                ESP_LOGE(TAG, "mkdir %s failed: %s", tmp, strerror(errno));
                // This mkdir runs ahead of the caller's fs_atomic write, so a
                // failing card must be reported here too.
                sd_health_report_write_failure(tmp, errno);
                return ESP_FAIL;
            }
        }
        *slash = '/';
        slash++;
    }
    // The whole chain is present: a healthy write, which resets the failure streak.
    sd_health_report_write_ok(filepath);
    return ESP_OK;
}
