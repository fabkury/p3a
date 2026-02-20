// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_rest_settings.c
 * @brief Configuration and settings REST handlers
 *
 * Contains handlers for:
 * - GET/PUT /config - Configuration management
 * - GET/PUT /settings/dwell_time - Dwell time settings
 * - GET/PUT /settings/global_seed - Global seed settings
 * - GET/PUT /settings/play_order - Play order settings
 * - GET/POST /rotation - Screen rotation
 * - GET/PUT /settings/giphy_refresh_override - Giphy refresh override
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
    cJSON *fr = cJSON_GetObjectItem(o, "giphy_full_refresh");
    if (fr && cJSON_IsBool(fr)) {
        config_store_invalidate_giphy_full_refresh();
    }
    cJSON *ppa = cJSON_GetObjectItem(o, "ppa_upscale");
    if (ppa && cJSON_IsBool(ppa)) {
        config_store_invalidate_ppa_upscale();
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

/**
 * GET /settings/global_seed
 */
esp_err_t h_get_global_seed(httpd_req_t *req)
{
    uint32_t seed = config_store_get_global_seed();
    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":true,\"data\":{\"global_seed\":%lu}}", (unsigned long)seed);
    send_json(req, 200, response);
    return ESP_OK;
}

/**
 * PUT /settings/global_seed
 */
esp_err_t h_put_global_seed(httpd_req_t *req)
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

    cJSON *seed_item = cJSON_GetObjectItem(root, "global_seed");
    if (!seed_item || !cJSON_IsNumber(seed_item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'global_seed' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    uint32_t seed = (uint32_t)cJSON_GetNumberValue(seed_item);
    cJSON_Delete(root);

    esp_err_t err = config_store_set_global_seed(seed);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to set global_seed\",\"code\":\"SET_GLOBAL_SEED_FAILED\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/**
 * GET /settings/play_order
 */
esp_err_t h_get_play_order(httpd_req_t *req)
{
    uint8_t play_order = config_store_get_play_order();
    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":true,\"data\":{\"play_order\":%u}}", (unsigned)play_order);
    send_json(req, 200, response);
    return ESP_OK;
}

/**
 * PUT /settings/play_order
 * Sets play order and hot-swaps it for the current channel
 * Body: {"play_order": 1|2}  (1=created/date, 2=random)
 */
esp_err_t h_put_play_order(httpd_req_t *req)
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

    cJSON *order_item = cJSON_GetObjectItem(root, "play_order");
    if (!order_item || !cJSON_IsNumber(order_item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'play_order' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    int order = (int)cJSON_GetNumberValue(order_item);
    cJSON_Delete(root);

    if (order < 0 || order > 2) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid play_order (must be 0-2)\",\"code\":\"INVALID_PLAY_ORDER\"}");
        return ESP_OK;
    }

    // Save to config store (persists across reboots)
    esp_err_t err = config_store_set_play_order((uint8_t)order);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to save play_order\",\"code\":\"SET_PLAY_ORDER_FAILED\"}");
        return ESP_OK;
    }

    // Set shuffle override based on play_order
    // order 2 (random) enables shuffle override, else disables
    play_scheduler_set_shuffle_override(order == 2);

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

// ---------- Giphy Refresh Override ----------

esp_err_t h_get_giphy_refresh_override(httpd_req_t *req)
{
    bool val = config_store_get_giphy_refresh_allow_override();
    char response[128];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"data\":{\"giphy_refresh_allow_override\":%s}}",
             val ? "true" : "false");
    send_json(req, 200, response);
    return ESP_OK;
}

esp_err_t h_put_giphy_refresh_override(httpd_req_t *req)
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

    cJSON *item = cJSON_GetObjectItem(root, "giphy_refresh_allow_override");
    if (!item || !cJSON_IsBool(item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'giphy_refresh_allow_override' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    bool val = cJSON_IsTrue(item);
    cJSON_Delete(root);

    config_store_set_giphy_refresh_allow_override(val);
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}
