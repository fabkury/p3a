// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_rest_settings.c
 * @brief Configuration and settings REST handlers
 *
 * Contains handlers for:
 * - GET/PUT /config - Configuration management
 * - GET/PUT /settings/dwell_time - Dwell time settings
 * - GET/POST /rotation - Screen rotation
 * - GET/PUT /settings/refresh_override - Refresh override
 */

#include "http_api_internal.h"
#include "config_store.h"
#include "animation_player.h"
#include "play_scheduler.h"
#include "app_lcd.h"

// ---------- Config Handlers ----------

/**
 * GET /config
 * Returns current configuration as JSON object
 */
esp_err_t h_get_config(httpd_req_t *req) {
    char *json;
    size_t len;
    esp_err_t err = config_store_get_serialized(&json, &len);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"CONFIG_READ_FAIL\",\"code\":\"CONFIG_READ_FAIL\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(json);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *data = cJSON_ParseWithLength(json, len);
    if (!data) {
        data = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(root, "data", data);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(json);

    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

/**
 * PUT /config
 * Accepts JSON config object (max 32 KB), validates, and saves to NVS
 */
esp_err_t h_put_config(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        if (err_status == 413) {
            send_json(req, 413, "{\"ok\":false,\"error\":\"Payload too large\",\"code\":\"PAYLOAD_TOO_LARGE\"}");
        } else {
            send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        }
        return ESP_OK;
    }

    cJSON *o = cJSON_ParseWithLength(body, len);
    free(body);

    if (!o || !cJSON_IsObject(o)) {
        if (o) cJSON_Delete(o);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    // Merge mode: load current config and merge incoming fields on top
    bool merge_mode = (strstr(req->uri, "merge=true") != NULL);
    cJSON *merged = NULL;
    cJSON *to_save = o;

    if (merge_mode) {
        char *cur_json = NULL;
        size_t cur_len = 0;
        esp_err_t load_err = config_store_get_serialized(&cur_json, &cur_len);
        if (load_err == ESP_OK && cur_json) {
            merged = cJSON_ParseWithLength(cur_json, cur_len);
            free(cur_json);
        }
        if (!merged) {
            merged = cJSON_CreateObject();
        }
        // Shallow-merge incoming keys into current config
        cJSON *child = o->child;
        while (child) {
            cJSON *dup = cJSON_Duplicate(child, true);
            if (cJSON_HasObjectItem(merged, child->string)) {
                cJSON_ReplaceItemInObject(merged, child->string, dup);
            } else {
                cJSON_AddItemToObject(merged, child->string, dup);
            }
            child = child->next;
        }
        to_save = merged;
    }

    esp_err_t e = config_store_save(to_save);

    if (merged) {
        cJSON_Delete(merged);
    }

    if (e != ESP_OK) {
        cJSON_Delete(o);
        send_json(req, 500, "{\"ok\":false,\"error\":\"CONFIG_SAVE_FAIL\",\"code\":\"CONFIG_SAVE_FAIL\"}");
        return ESP_OK;
    }

    // Apply dwell_time_ms change at runtime (for auto-swap interval)
    cJSON *dwell_item = cJSON_GetObjectItem(o, "dwell_time_ms");
    if (dwell_item && cJSON_IsNumber(dwell_item)) {
        uint32_t dwell_ms = (uint32_t)cJSON_GetNumberValue(dwell_item);
        play_scheduler_set_dwell_time(dwell_ms / 1000);
    }

    // Invalidate in-memory caches for giphy settings so getters re-read from NVS
    cJSON *cs = cJSON_GetObjectItem(o, "giphy_cache_size");
    if (cs && cJSON_IsNumber(cs)) {
        config_store_invalidate_giphy_cache_size();
    }
    cJSON *ri = cJSON_GetObjectItem(o, "giphy_refresh_interval");
    if (ri && cJSON_IsNumber(ri)) {
        config_store_invalidate_giphy_refresh_interval();
    }
    cJSON *gpd = cJSON_GetObjectItem(o, "giphy_prefer_downsized");
    if (gpd && cJSON_IsBool(gpd)) {
        config_store_invalidate_giphy_prefer_downsized();
    }
    cJSON *va = cJSON_GetObjectItem(o, "view_ack");
    if (va && cJSON_IsBool(va)) {
        config_store_invalidate_view_ack();
    }
    cJSON *dt = cJSON_GetObjectItem(o, "dwell_time_ms");
    if (dt && cJSON_IsNumber(dt)) {
        config_store_invalidate_dwell_time();
    }
    cJSON *ris = cJSON_GetObjectItem(o, "refresh_interval_sec");
    if (ris && cJSON_IsNumber(ris)) {
        config_store_invalidate_refresh_interval_sec();
    }

    cJSON *csm = cJSON_GetObjectItem(o, "channel_select_mode");
    if (csm && cJSON_IsNumber(csm)) {
        int mode = (int)cJSON_GetNumberValue(csm);
        if (mode >= 0 && mode <= 1) {
            play_scheduler_set_channel_select_mode((ps_channel_select_mode_t)mode);
        }
    }

    cJSON_Delete(o);
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

// ---------- Settings Handlers ----------

/**
 * GET /settings/dwell_time
 */
esp_err_t h_get_dwell_time(httpd_req_t *req) {
    uint32_t dwell_time = animation_player_get_dwell_time();
    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":true,\"data\":{\"dwell_time\":%lu}}", (unsigned long)dwell_time);
    send_json(req, 200, response);
    return ESP_OK;
}

/**
 * PUT /settings/dwell_time
 */
esp_err_t h_put_dwell_time(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        if (err_status == 413) {
            send_json(req, 413, "{\"ok\":false,\"error\":\"Payload too large\",\"code\":\"PAYLOAD_TOO_LARGE\"}");
        } else {
            send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        }
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);

    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *dwell_item = cJSON_GetObjectItem(root, "dwell_time");
    if (!dwell_item || !cJSON_IsNumber(dwell_item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'dwell_time' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    uint32_t dwell_time = (uint32_t)cJSON_GetNumberValue(dwell_item);
    cJSON_Delete(root);

    if (dwell_time > 100000) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid dwell_time (must be 0-100000 seconds)\",\"code\":\"INVALID_DWELL_TIME\"}");
        return ESP_OK;
    }

    esp_err_t err = animation_player_set_dwell_time(dwell_time);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to set dwell_time\",\"code\":\"SET_DWELL_TIME_FAILED\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

// ---------- Rotation Handlers ----------

/**
 * GET /rotation
 */
esp_err_t h_get_rotation(httpd_req_t *req) {
    screen_rotation_t rotation = app_get_screen_rotation();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "rotation", (double)rotation);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, json_str);
    free(json_str);
    return ESP_OK;
}

/**
 * POST /rotation
 */
esp_err_t h_post_rotation(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        free(buf);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *rotation_item = cJSON_GetObjectItem(root, "rotation");
    if (!rotation_item || !cJSON_IsNumber(rotation_item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'rotation' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    int rotation_value = (int)cJSON_GetNumberValue(rotation_item);
    cJSON_Delete(root);

    if (rotation_value != 0 && rotation_value != 90 && rotation_value != 180 && rotation_value != 270) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid rotation angle (must be 0, 90, 180, or 270)\",\"code\":\"INVALID_ROTATION\"}");
        return ESP_OK;
    }

    esp_err_t err = app_set_screen_rotation((screen_rotation_t)rotation_value);
    if (err == ESP_ERR_INVALID_STATE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Rotation operation already in progress\",\"code\":\"ROTATION_IN_PROGRESS\"}");
        return ESP_OK;
    } else if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to set rotation\",\"code\":\"ROTATION_FAILED\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true,\"data\":{\"rotation\":null}}");
    return ESP_OK;
}

// ---------- Refresh Override ----------

esp_err_t h_get_refresh_override(httpd_req_t *req)
{
    bool val = config_store_get_refresh_allow_override();
    char response[128];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"data\":{\"refresh_allow_override\":%s}}",
             val ? "true" : "false");
    send_json(req, 200, response);
    return ESP_OK;
}

esp_err_t h_put_refresh_override(httpd_req_t *req)
{
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        if (err_status == 413) {
            send_json(req, 413, "{\"ok\":false,\"error\":\"Payload too large\",\"code\":\"PAYLOAD_TOO_LARGE\"}");
        } else {
            send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        }
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *item = cJSON_GetObjectItem(root, "refresh_allow_override");
    if (!item || !cJSON_IsBool(item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'refresh_allow_override' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    bool val = cJSON_IsTrue(item);
    cJSON_Delete(root);

    config_store_set_refresh_allow_override(val);
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}
