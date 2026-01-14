// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "load_tracker.h"
#include "makapix_channel_internal.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "load_tracker";

// ============================================================================
// Path Building
// ============================================================================

void ltf_build_path(const char *storage_key, const char *vault_path,
                    char *out, size_t out_len)
{
    // Compute SHA256 of storage_key for vault sharding
    uint8_t sha256[32];
    if (storage_key_sha256(storage_key, sha256) != ESP_OK) {
        // Fallback: use storage_key directly
        snprintf(out, out_len, "%s/%s.ltf", vault_path, storage_key);
        return;
    }

    // Format path: {vault_path}/{sha[0:2]}/{sha[2:4]}/{storage_key}.ltf
    char sha_hex[5];
    snprintf(sha_hex, sizeof(sha_hex), "%02x%02x", sha256[0], sha256[1]);

    snprintf(out, out_len, "%s/%c%c/%c%c/%s.ltf",
             vault_path,
             sha_hex[0], sha_hex[1],
             sha_hex[2], sha_hex[3],
             storage_key);
}

// ============================================================================
// LTF File Operations
// ============================================================================

/**
 * @brief Parse LTF JSON content
 */
static esp_err_t parse_ltf_json(const char *json_str, load_tracker_t *out)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));

    cJSON *attempts = cJSON_GetObjectItem(root, "attempts");
    if (cJSON_IsNumber(attempts)) {
        out->attempts = (uint8_t)attempts->valueint;
    }

    cJSON *terminal = cJSON_GetObjectItem(root, "terminal");
    if (cJSON_IsBool(terminal)) {
        out->terminal = cJSON_IsTrue(terminal);
    }

    cJSON *last_failure = cJSON_GetObjectItem(root, "last_failure");
    if (cJSON_IsNumber(last_failure)) {
        out->last_failure = (time_t)last_failure->valuedouble;
    }

    cJSON *reason = cJSON_GetObjectItem(root, "reason");
    if (cJSON_IsString(reason) && reason->valuestring) {
        strncpy(out->reason, reason->valuestring, sizeof(out->reason) - 1);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief Generate LTF JSON content
 */
static char *generate_ltf_json(const load_tracker_t *ltf)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "attempts", ltf->attempts);
    cJSON_AddBoolToObject(root, "terminal", ltf->terminal);
    cJSON_AddNumberToObject(root, "last_failure", (double)ltf->last_failure);
    cJSON_AddStringToObject(root, "reason", ltf->reason);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

/**
 * @brief Ensure parent directories exist
 */
static void ensure_parent_dirs(const char *path)
{
    char dir_path[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';

    // Find last slash
    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) return;
    *last_slash = '\0';

    // Try to create parent directory (may already exist)
    struct stat st;
    if (stat(dir_path, &st) != 0) {
        // Need to create - first ensure grandparent exists
        char *prev_slash = strrchr(dir_path, '/');
        if (prev_slash) {
            *prev_slash = '\0';
            if (stat(dir_path, &st) != 0) {
                mkdir(dir_path, 0755);
            }
            *prev_slash = '/';
        }
        mkdir(dir_path, 0755);
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t ltf_load(const char *storage_key, const char *vault_path, load_tracker_t *out)
{
    if (!storage_key || !vault_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[256];
    ltf_build_path(storage_key, vault_path, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    // Read file content
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024) {  // LTF files should be small
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buffer, 1, size, f);
    fclose(f);
    buffer[read] = '\0';

    esp_err_t err = parse_ltf_json(buffer, out);
    free(buffer);

    return err;
}

bool ltf_can_download(const char *storage_key, const char *vault_path)
{
    load_tracker_t ltf;
    esp_err_t err = ltf_load(storage_key, vault_path, &ltf);

    if (err == ESP_ERR_NOT_FOUND) {
        return true;  // No LTF = can download
    }

    if (err != ESP_OK) {
        // Error reading LTF - allow download (conservative)
        return true;
    }

    return !ltf.terminal;
}

bool ltf_is_terminal(const char *storage_key, const char *vault_path)
{
    load_tracker_t ltf;
    esp_err_t err = ltf_load(storage_key, vault_path, &ltf);

    if (err != ESP_OK) {
        return false;  // No LTF or error = not terminal
    }

    return ltf.terminal;
}

int ltf_get_remaining_attempts(const char *storage_key, const char *vault_path)
{
    load_tracker_t ltf;
    esp_err_t err = ltf_load(storage_key, vault_path, &ltf);

    if (err == ESP_ERR_NOT_FOUND) {
        return LTF_MAX_ATTEMPTS;  // No LTF = all attempts available
    }

    if (err != ESP_OK || ltf.terminal) {
        return 0;  // Error or terminal = no attempts
    }

    int remaining = LTF_MAX_ATTEMPTS - ltf.attempts;
    return (remaining > 0) ? remaining : 0;
}

esp_err_t ltf_record_failure(const char *storage_key, const char *vault_path, const char *reason)
{
    if (!storage_key || !vault_path) {
        return ESP_ERR_INVALID_ARG;
    }

    // Load existing LTF or create new
    load_tracker_t ltf;
    esp_err_t err = ltf_load(storage_key, vault_path, &ltf);

    if (err == ESP_ERR_NOT_FOUND) {
        // First failure
        memset(&ltf, 0, sizeof(ltf));
        ltf.attempts = 1;
    } else if (err == ESP_OK) {
        // Increment attempts
        ltf.attempts++;
    } else {
        // Error reading - start fresh
        memset(&ltf, 0, sizeof(ltf));
        ltf.attempts = 1;
    }

    // Update fields
    ltf.last_failure = time(NULL);
    if (reason) {
        strncpy(ltf.reason, reason, sizeof(ltf.reason) - 1);
    }

    // Check for terminal state
    if (ltf.attempts >= LTF_MAX_ATTEMPTS) {
        ltf.terminal = true;
        ESP_LOGW(TAG, "LTF terminal for '%s' after %d attempts: %s",
                 storage_key, ltf.attempts, ltf.reason);
    } else {
        ESP_LOGI(TAG, "LTF recorded failure %d/%d for '%s': %s",
                 ltf.attempts, LTF_MAX_ATTEMPTS, storage_key,
                 ltf.reason[0] ? ltf.reason : "unknown");
    }

    // Generate JSON
    char *json_str = generate_ltf_json(&ltf);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    // Write to file
    char path[256];
    ltf_build_path(storage_key, vault_path, path, sizeof(path));

    // Ensure parent directories exist
    ensure_parent_dirs(path);

    // Write atomically via temp file
    char temp_path[260];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    FILE *f = fopen(temp_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create LTF temp file: %s", temp_path);
        free(json_str);
        return ESP_FAIL;
    }

    size_t written = fwrite(json_str, 1, strlen(json_str), f);
    free(json_str);

    if (written == 0) {
        fclose(f);
        unlink(temp_path);
        return ESP_FAIL;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // Atomic rename
    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ltf_clear(const char *storage_key, const char *vault_path)
{
    if (!storage_key || !vault_path) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[256];
    ltf_build_path(storage_key, vault_path, path, sizeof(path));

    // Try to delete the file
    if (unlink(path) == 0) {
        ESP_LOGD(TAG, "Cleared LTF for '%s'", storage_key);
    }
    // Ignore errors - file may not exist

    return ESP_OK;
}
