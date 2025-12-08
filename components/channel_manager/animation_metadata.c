#include "animation_metadata.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "anim_metadata";

void animation_metadata_init(animation_metadata_t *meta)
{
    if (!meta) {
        return;
    }
    
    memset(meta, 0, sizeof(animation_metadata_t));
    meta->filepath = NULL;
    meta->has_metadata = false;
    meta->field1 = NULL;
    meta->field2 = 0;
    meta->field3 = false;
}

void animation_metadata_free(animation_metadata_t *meta)
{
    if (!meta) {
        return;
    }
    
    if (meta->filepath) {
        free(meta->filepath);
        meta->filepath = NULL;
    }
    
    if (meta->field1) {
        free(meta->field1);
        meta->field1 = NULL;
    }
    
    meta->has_metadata = false;
    meta->field2 = 0;
    meta->field3 = false;
}

esp_err_t animation_metadata_set_filepath(animation_metadata_t *meta, const char *filepath)
{
    if (!meta || !filepath) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Free existing filepath if any
    if (meta->filepath) {
        free(meta->filepath);
        meta->filepath = NULL;
    }
    
    // Duplicate the filepath
    size_t len = strlen(filepath);
    meta->filepath = (char *)malloc(len + 1);
    if (!meta->filepath) {
        ESP_LOGE(TAG, "Failed to allocate filepath");
        return ESP_ERR_NO_MEM;
    }
    
    strcpy(meta->filepath, filepath);
    return ESP_OK;
}

bool animation_metadata_has_filepath(const animation_metadata_t *meta)
{
    return meta && meta->filepath && meta->filepath[0] != '\0';
}

const char *animation_metadata_get_filepath(const animation_metadata_t *meta)
{
    return meta ? meta->filepath : NULL;
}

/**
 * @brief Build the sidecar JSON path from animation filepath
 * 
 * Replaces the file extension with .json
 * e.g. /sdcard/animations/art.webp -> /sdcard/animations/art.json
 */
static char *build_sidecar_path(const char *filepath)
{
    if (!filepath) {
        return NULL;
    }
    
    size_t len = strlen(filepath);
    
    // Find the last dot for extension
    const char *dot = strrchr(filepath, '.');
    const char *slash = strrchr(filepath, '/');
    
    // Make sure dot is after the last slash (part of filename, not directory)
    if (dot && slash && dot < slash) {
        dot = NULL;
    }
    
    size_t stem_len;
    if (dot) {
        stem_len = (size_t)(dot - filepath);
    } else {
        stem_len = len;
    }
    
    // Allocate space for stem + ".json" + null
    size_t sidecar_len = stem_len + 5 + 1;
    char *sidecar_path = (char *)malloc(sidecar_len);
    if (!sidecar_path) {
        return NULL;
    }
    
    // Copy stem and append .json
    memcpy(sidecar_path, filepath, stem_len);
    strcpy(sidecar_path + stem_len, ".json");
    
    return sidecar_path;
}

/**
 * @brief Read entire file into memory
 */
static char *read_file_contents(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > (64 * 1024)) {  // Limit to 64KB
        fclose(f);
        return NULL;
    }
    
    char *buffer = (char *)malloc((size_t)file_size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    
    size_t bytes_read = fread(buffer, 1, (size_t)file_size, f);
    fclose(f);
    
    if (bytes_read != (size_t)file_size) {
        free(buffer);
        return NULL;
    }
    
    buffer[file_size] = '\0';
    
    if (out_size) {
        *out_size = (size_t)file_size;
    }
    
    return buffer;
}

esp_err_t animation_metadata_load_sidecar(animation_metadata_t *meta)
{
    if (!meta) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!animation_metadata_has_filepath(meta)) {
        ESP_LOGW(TAG, "Cannot load sidecar: filepath not set");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build sidecar path
    char *sidecar_path = build_sidecar_path(meta->filepath);
    if (!sidecar_path) {
        ESP_LOGE(TAG, "Failed to build sidecar path");
        return ESP_ERR_NO_MEM;
    }
    
    // Check if sidecar file exists
    struct stat st;
    if (stat(sidecar_path, &st) != 0) {
        ESP_LOGD(TAG, "No sidecar file found: %s", sidecar_path);
        free(sidecar_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Read sidecar file contents
    size_t json_size = 0;
    char *json_content = read_file_contents(sidecar_path, &json_size);
    if (!json_content) {
        ESP_LOGW(TAG, "Failed to read sidecar file: %s", sidecar_path);
        free(sidecar_path);
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Loading metadata from: %s (%zu bytes)", sidecar_path, json_size);
    free(sidecar_path);
    
    // Parse JSON
    cJSON *root = cJSON_Parse(json_content);
    free(json_content);
    
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            ESP_LOGW(TAG, "JSON parse error near: %s", error_ptr);
        }
        return ESP_ERR_INVALID_STATE;
    }
    
    // Clear existing metadata fields (but keep filepath)
    if (meta->field1) {
        free(meta->field1);
        meta->field1 = NULL;
    }
    meta->field2 = 0;
    meta->field3 = false;
    
    // Extract field1 (string)
    cJSON *field1_json = cJSON_GetObjectItem(root, "field1");
    if (cJSON_IsString(field1_json) && field1_json->valuestring) {
        size_t str_len = strlen(field1_json->valuestring);
        meta->field1 = (char *)malloc(str_len + 1);
        if (meta->field1) {
            strcpy(meta->field1, field1_json->valuestring);
        }
    }
    
    // Extract field2 (integer)
    cJSON *field2_json = cJSON_GetObjectItem(root, "field2");
    if (cJSON_IsNumber(field2_json)) {
        meta->field2 = (int32_t)field2_json->valueint;
    }
    
    // Extract field3 (boolean)
    cJSON *field3_json = cJSON_GetObjectItem(root, "field3");
    if (cJSON_IsBool(field3_json)) {
        meta->field3 = cJSON_IsTrue(field3_json);
    }
    
    cJSON_Delete(root);
    
    meta->has_metadata = true;
    
    ESP_LOGI(TAG, "Loaded metadata - field1: %s, field2: %d, field3: %s",
             meta->field1 ? meta->field1 : "(null)",
             (int)meta->field2,
             meta->field3 ? "true" : "false");
    
    return ESP_OK;
}

esp_err_t animation_metadata_copy(animation_metadata_t *dst, const animation_metadata_t *src)
{
    if (!dst || !src) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Free existing destination data
    animation_metadata_free(dst);
    
    // Copy filepath
    if (src->filepath) {
        dst->filepath = strdup(src->filepath);
        if (!dst->filepath) {
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Copy metadata state
    dst->has_metadata = src->has_metadata;
    
    // Copy field1 (string)
    if (src->field1) {
        dst->field1 = strdup(src->field1);
        if (!dst->field1) {
            free(dst->filepath);
            dst->filepath = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Copy field2 and field3
    dst->field2 = src->field2;
    dst->field3 = src->field3;
    
    return ESP_OK;
}

