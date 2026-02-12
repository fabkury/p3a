// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "channel_settings.h"
#include "sd_path.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>

static const char *TAG = "ch_settings";

static esp_err_t load_json_file(const char *path, cJSON **out_json)
{
    if (!path || !out_json) return ESP_ERR_INVALID_ARG;
    *out_json = NULL;

    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_json = root;
    return ESP_OK;
}

static void parse_settings(cJSON *json, channel_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!json) return;

    cJSON *order = cJSON_GetObjectItem(json, "play_order");
    if (cJSON_IsNumber(order)) {
        int v = (int)cJSON_GetNumberValue(order);
        if (v >= 0 && v <= 2) {
            out->play_order_present = true;
            out->play_order = (uint8_t)v;
        }
    }

    cJSON *rp = cJSON_GetObjectItem(json, "randomize_playlist");
    if (cJSON_IsBool(rp)) {
        out->randomize_playlist_present = true;
        out->randomize_playlist = cJSON_IsTrue(rp);
    }

    cJSON *dwell = cJSON_GetObjectItem(json, "dwell_time_ms");
    if (cJSON_IsNumber(dwell)) {
        double v = cJSON_GetNumberValue(dwell);
        if (v >= 0 && v <= 100000000) {
            out->channel_dwell_time_present = true;
            out->channel_dwell_time_ms = (uint32_t)v; // 0 allowed ("off")
        }
    }
}

static esp_err_t load_settings_path(const char *path, channel_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    cJSON *json = NULL;
    esp_err_t err = load_json_file(path, &json);
    if (err != ESP_OK) {
        return err;
    }

    parse_settings(json, out);
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t channel_settings_load_for_channel_id(const char *channel_id, channel_settings_t *out)
{
    if (!channel_id || !out) return ESP_ERR_INVALID_ARG;

    char channel_dir[128];
    if (sd_path_get_channel(channel_dir, sizeof(channel_dir)) != ESP_OK) {
        return ESP_FAIL;
    }

    char path[256];
    // Layout: <channel_dir>/<channel_id>.settings.json (flat; no per-channel directory)
    snprintf(path, sizeof(path), "%s/%s.settings.json", channel_dir, channel_id);
    esp_err_t err = load_settings_path(path, out);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load %s: %s", path, esp_err_to_name(err));
    }
    return err;
}

esp_err_t channel_settings_load_for_sdcard(channel_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    
    char channel_dir[128];
    if (sd_path_get_channel(channel_dir, sizeof(channel_dir)) != ESP_OK) {
        return ESP_FAIL;
    }
    
    char path[256];
    // Layout: <channel_dir>/sdcard-channel.settings.json
    snprintf(path, sizeof(path), "%s/sdcard-channel.settings.json", channel_dir);
    esp_err_t err = load_settings_path(path, out);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load %s: %s", path, esp_err_to_name(err));
    }
    return err;
}

