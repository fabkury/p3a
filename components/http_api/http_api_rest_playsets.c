// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_rest_playsets.c
 * @brief Playset execution and CRUD REST handlers
 *
 * Contains handlers for:
 * - POST /playset/{name} - Load and execute a named playset
 * - GET /playsets/active - Get active playset name
 * - GET /playsets - List all saved playsets
 * - GET /playsets/{name} - Read a playset (optionally activate)
 * - POST /playsets/{name} - Create/update a playset
 * - DELETE /playsets/{name} - Delete a playset
 */

#include "http_api_internal.h"
#include "config_store.h"
#include "makapix_api.h"
#include "makapix_channel_utils.h"
#include "makapix_mqtt.h"
#include "makapix_store.h"
#include "play_scheduler.h"
#include "playset_store.h"
#include "playset_json.h"
#include "p3a_state.h"

// ---------- Playset Name Validation ----------

static bool is_valid_playset_name(const char *name, size_t len)
{
    if (!name || len == 0 || len > PLAYSET_MAX_NAME_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)name[i] < 0x20) return false;
    }
    return true;
}

// ---------- Playset Mode String Helpers ----------

static const char *exposure_mode_str(ps_exposure_mode_t m) {
    switch (m) {
        case PS_EXPOSURE_EQUAL:        return "equal";
        case PS_EXPOSURE_MANUAL:       return "manual";
        case PS_EXPOSURE_PROPORTIONAL: return "proportional";
        default:                       return "unknown";
    }
}

static const char *pick_mode_str(ps_pick_mode_t m) {
    switch (m) {
        case PS_PICK_RECENCY: return "recency";
        case PS_PICK_RANDOM:  return "random";
        default:              return "unknown";
    }
}

// ---------- Current Artwork Helper ----------

static const char *asset_type_ext(asset_type_t t) {
    switch (t) {
        case ASSET_TYPE_WEBP: return ".webp";
        case ASSET_TYPE_GIF:  return ".gif";
        case ASSET_TYPE_PNG:  return ".png";
        case ASSET_TYPE_JPEG: return ".jpg";
        default:              return ".webp";
    }
}

/**
 * Build a "current_artwork" cJSON object from the play scheduler state.
 * Returns NULL if no artwork is playing or URL cannot be built.
 * Caller must cJSON_Delete the result.
 */
cJSON *build_current_artwork_json(void)
{
    ps_artwork_t artwork;
    if (play_scheduler_current(&artwork) != ESP_OK) {
        return NULL;
    }

    char url[300] = "";

    switch (artwork.channel_type) {
        case PS_CHANNEL_TYPE_GIPHY:
            snprintf(url, sizeof(url),
                     "https://i.giphy.com/media/%s/giphy.webp",
                     artwork.storage_key);
            break;

        case PS_CHANNEL_TYPE_NAMED:
        case PS_CHANNEL_TYPE_USER:
        case PS_CHANNEL_TYPE_HASHTAG: {
            uint8_t sha[32];
            if (storage_key_sha256(artwork.storage_key, sha) == ESP_OK) {
                snprintf(url, sizeof(url),
                         "https://%s/api/vault/%02x/%02x/%02x/%s%s",
                         CONFIG_MAKAPIX_CLUB_HOST,
                         (unsigned)sha[0], (unsigned)sha[1], (unsigned)sha[2],
                         artwork.storage_key, asset_type_ext(artwork.type));
            }
            break;
        }

        case PS_CHANNEL_TYPE_SDCARD:
        case PS_CHANNEL_TYPE_ARTWORK:
        default:
            break;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "url", url);
    cJSON_AddStringToObject(obj, "channel_type",
                            playset_channel_type_str(artwork.channel_type));
    cJSON_AddBoolToObject(obj, "ppa_upscale", config_store_get_ppa_upscale());

    return obj;
}

// ---------- Playset Execute Handler ----------

/**
 * POST /playset/{name}
 * Load and execute a named playset
 *
 * Flow:
 * 1. Check if it's a built-in playset (channel_recent, channel_promoted, channel_sdcard)
 * 2. If MQTT connected: fetch from server, save to SD, execute
 * 3. If not connected: load from SD cache if exists
 * 4. Execute via play_scheduler_execute_command()
 * 5. Persist playset name to NVS for boot restore
 */
esp_err_t h_post_playset(httpd_req_t *req)
{
    // Extract playset name from URI: /playset/{name}
    const char *uri = req->uri;
    const char *prefix = "/playset/";
    size_t prefix_len = strlen(prefix);

    if (strncmp(uri, prefix, prefix_len) != 0) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid playset path\",\"code\":\"INVALID_PATH\"}");
        return ESP_OK;
    }

    // Copy and URL-decode the name (URI contains percent-encoded characters)
    char name[PLAYSET_MAX_NAME_LEN + 1];
    strlcpy(name, uri + prefix_len, sizeof(name));
    url_decode_in_place(name);

    size_t name_len = strlen(name);
    if (!is_valid_playset_name(name, name_len)) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid playset name\",\"code\":\"INVALID_NAME\"}");
        return ESP_OK;
    }

    ps_scheduler_command_t *command = calloc(1, sizeof(ps_scheduler_command_t));
    if (!command) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    esp_err_t err;
    bool from_cache = false;
    bool is_builtin = false;

    // 1. Check built-in playsets (no I/O needed)
    err = ps_create_channel_playset(name, command);
    if (err == ESP_OK) {
        is_builtin = true;
        ESP_LOGI("http_api", "Using built-in playset: %s", name);
    } else {
        // 2. Try local cache first (instant, avoids 30s MQTT timeout for user-created playsets)
        err = playset_store_load(name, command);
        if (err == ESP_OK) {
            from_cache = true;
        } else if (makapix_mqtt_is_connected()) {
            // 3. Not cached locally -- try fetching from server
            err = makapix_api_get_playset(name, command);
            if (err == ESP_OK) {
                strlcpy(command->name, name, sizeof(command->name));
                esp_err_t save_err = playset_store_save(name, command);
                if (save_err != ESP_OK) {
                    ESP_LOGW("http_api", "Failed to cache playset '%s': %s", name, esp_err_to_name(save_err));
                }
            } else {
                free(command);
                if (err == ESP_ERR_TIMEOUT) {
                    send_json(req, 504, "{\"ok\":false,\"error\":\"Request timed out\",\"code\":\"MQTT_TIMEOUT\"}");
                } else {
                    send_json(req, 404, "{\"ok\":false,\"error\":\"Playset not found\",\"code\":\"PLAYSET_NOT_FOUND\"}");
                }
                return ESP_OK;
            }
        } else {
            free(command);
            send_json(req, 503, "{\"ok\":false,\"error\":\"Not connected and no cached playset\",\"code\":\"NOT_CONNECTED\"}");
            return ESP_OK;
        }
    }

    // Execute the playset
    err = play_scheduler_execute_command(command);
    if (err != ESP_OK) {
        free(command);
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "{\"ok\":false,\"error\":\"Failed to execute playset: %s\",\"code\":\"EXECUTE_ERROR\"}",
                 esp_err_to_name(err));
        send_json(req, 500, error_msg);
        return ESP_OK;
    }

    // Persist playset name to NVS for boot restore
    esp_err_t persist_err = p3a_state_set_active_playset(name);
    if (persist_err != ESP_OK) {
        ESP_LOGW("http_api", "Failed to persist playset name: %s", esp_err_to_name(persist_err));
    }

    // Build response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(command);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "playset", name);
    cJSON_AddNumberToObject(root, "channel_count", (double)command->channel_count);
    cJSON_AddBoolToObject(root, "from_cache", from_cache);
    cJSON_AddBoolToObject(root, "builtin", is_builtin);
    cJSON_AddStringToObject(root, "exposure_mode", exposure_mode_str(command->exposure_mode));
    cJSON_AddStringToObject(root, "pick_mode", pick_mode_str(command->pick_mode));

    // Compute artwork sums from live scheduler state (caches loaded by execute_command)
    ps_stats_t ps_stats;
    if (play_scheduler_get_stats(&ps_stats) == ESP_OK) {
        cJSON_AddNumberToObject(root, "total_cached", (double)ps_stats.total_available);
        cJSON_AddNumberToObject(root, "total_entries", (double)ps_stats.total_entries);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!out) {
        free(command);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    free(command);
    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

// ---------- Playset CRUD Handlers ----------

/**
 * GET /playsets/active
 * Returns the currently active playset name.
 * This is the playset-centric replacement for GET /channel's playset field.
 * (GET /channel is deprecated and will be removed in a future version.)
 */
esp_err_t h_get_active_playset(httpd_req_t *req)
{
    const char *playset = p3a_state_get_active_playset();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *data = cJSON_AddObjectToObject(root, "data");
    cJSON_AddStringToObject(data, "name", playset ? playset : "");
    cJSON_AddBoolToObject(data, "registered", makapix_store_has_player_key());

    ps_stats_t ps_stats;
    if (play_scheduler_get_stats(&ps_stats) == ESP_OK) {
        cJSON *pi = cJSON_AddObjectToObject(data, "playset_info");
        cJSON_AddNumberToObject(pi, "channel_count", (double)ps_stats.channel_count);
        cJSON_AddNumberToObject(pi, "total_cached", (double)ps_stats.total_available);
        cJSON_AddNumberToObject(pi, "total_entries", (double)ps_stats.total_entries);
        cJSON_AddStringToObject(pi, "exposure_mode", exposure_mode_str(ps_stats.exposure_mode));
        cJSON_AddStringToObject(pi, "pick_mode", pick_mode_str(ps_stats.pick_mode));

        ps_channel_detail_t *ch_details = calloc(PS_MAX_CHANNELS, sizeof(ps_channel_detail_t));
        size_t ch_count = 0;
        if (ch_details &&
            play_scheduler_get_channel_details(ch_details, PS_MAX_CHANNELS, &ch_count) == ESP_OK &&
            ch_count > 0) {
            cJSON *ch_arr = cJSON_AddArrayToObject(pi, "channels");
            for (size_t i = 0; i < ch_count; i++) {
                cJSON *ch_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(ch_obj, "display_name", ch_details[i].display_name);
                cJSON_AddStringToObject(ch_obj, "type", playset_channel_type_str(ch_details[i].type));
                cJSON_AddStringToObject(ch_obj, "name", ch_details[i].spec_name);
                cJSON_AddStringToObject(ch_obj, "identifier", ch_details[i].identifier);
                cJSON_AddNumberToObject(ch_obj, "available", (double)ch_details[i].available_count);
                cJSON_AddNumberToObject(ch_obj, "total", (double)ch_details[i].entry_count);
                cJSON_AddBoolToObject(ch_obj, "refreshing", ch_details[i].refreshing);
                cJSON_AddNumberToObject(ch_obj, "last_refresh", (double)ch_details[i].last_refresh);
                cJSON_AddItemToArray(ch_arr, ch_obj);
            }
        }
        free(ch_details);
    }

    cJSON *ca = build_current_artwork_json();
    if (ca) {
        cJSON_AddItemToObject(data, "current_artwork", ca);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

/**
 * GET /playsets
 * List all saved playsets
 */
esp_err_t h_get_playsets(httpd_req_t *req)
{
    playset_list_entry_t *entries = calloc(32, sizeof(playset_list_entry_t));
    if (!entries) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    size_t count = 0;
    esp_err_t err = playset_store_list(entries, 32, &count);
    if (err != ESP_OK) {
        free(entries);
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to list playsets\",\"code\":\"LIST_ERROR\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(entries);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *data = cJSON_AddObjectToObject(root, "data");
    cJSON *arr = cJSON_AddArrayToObject(data, "playsets");

    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddNumberToObject(item, "channel_count", (double)entries[i].channel_count);
        cJSON_AddStringToObject(item, "exposure_mode", playset_exposure_mode_str(entries[i].exposure_mode));
        cJSON_AddStringToObject(item, "pick_mode", playset_pick_mode_str(entries[i].pick_mode));
        cJSON_AddItemToArray(arr, item);
    }

    free(entries);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

/**
 * GET /playsets/{name}[?activate=true]
 * Read a playset; optionally activate it
 */
esp_err_t h_get_playset_by_name(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *prefix = "/playsets/";
    size_t prefix_len = 10; // strlen("/playsets/")

    if (strncmp(uri, prefix, prefix_len) != 0 || uri[prefix_len] == '\0') {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid path\",\"code\":\"INVALID_PATH\"}");
        return ESP_OK;
    }

    const char *name_start = uri + prefix_len;
    const char *qmark = strchr(name_start, '?');
    size_t raw_len = qmark ? (size_t)(qmark - name_start) : strlen(name_start);

    char name[PLAYSET_MAX_NAME_LEN + 1];
    if (raw_len >= sizeof(name)) raw_len = sizeof(name) - 1;
    memcpy(name, name_start, raw_len);
    name[raw_len] = '\0';
    url_decode_in_place(name);

    size_t name_len = strlen(name);
    if (!is_valid_playset_name(name, name_len)) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid playset name\",\"code\":\"INVALID_NAME\"}");
        return ESP_OK;
    }

    bool activate = false;
    if (qmark) {
        activate = (strstr(qmark, "activate=true") != NULL);
    }

    ps_scheduler_command_t *cmd = calloc(1, sizeof(ps_scheduler_command_t));
    if (!cmd) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    esp_err_t err = playset_store_load(name, cmd);
    if (err == ESP_ERR_NOT_FOUND) {
        free(cmd);
        send_json(req, 404, "{\"ok\":false,\"error\":\"Playset not found\",\"code\":\"NOT_FOUND\"}");
        return ESP_OK;
    } else if (err != ESP_OK) {
        free(cmd);
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to load playset\",\"code\":\"LOAD_ERROR\"}");
        return ESP_OK;
    }

    bool activated = false;
    if (activate) {
        esp_err_t exec_err = play_scheduler_execute_command(cmd);
        if (exec_err == ESP_OK) {
            p3a_state_set_active_playset(name);
            activated = true;
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(cmd);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *data_obj = playset_json_serialize(cmd);
    free(cmd);

    if (!data_obj) {
        cJSON_Delete(root);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddItemToObject(root, "data", data_obj);
    cJSON_AddBoolToObject(root, "activated", activated);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

/**
 * POST /playsets/{name}
 * Create/update a playset; optionally activate it
 */
esp_err_t h_post_playset_crud(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *prefix = "/playsets/";
    size_t prefix_len = 10;

    if (strncmp(uri, prefix, prefix_len) != 0 || uri[prefix_len] == '\0') {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid path\",\"code\":\"INVALID_PATH\"}");
        return ESP_OK;
    }

    const char *name_start = uri + prefix_len;
    const char *qmark = strchr(name_start, '?');
    size_t raw_len = qmark ? (size_t)(qmark - name_start) : strlen(name_start);

    char name[PLAYSET_MAX_NAME_LEN + 1];
    if (raw_len >= sizeof(name)) raw_len = sizeof(name) - 1;
    memcpy(name, name_start, raw_len);
    name[raw_len] = '\0';
    url_decode_in_place(name);

    size_t name_len = strlen(name);
    if (!is_valid_playset_name(name, name_len)) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid playset name\",\"code\":\"INVALID_NAME\"}");
        return ESP_OK;
    }

    // Protected playsets cannot be overwritten via REST API
    static const char *protected_playsets[] = { "followed_artists" };
    for (size_t i = 0; i < sizeof(protected_playsets) / sizeof(protected_playsets[0]); i++) {
        if (strcmp(name, protected_playsets[i]) == 0) {
            send_json(req, 403, "{\"ok\":false,\"error\":\"Cannot overwrite protected playset\",\"code\":\"PROTECTED_PLAYSET\"}");
            return ESP_OK;
        }
    }

    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t body_len;
    char *body = recv_body_json(req, &body_len, &err_status);
    if (!body) {
        send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    // Extract optional "activate" boolean
    bool activate = false;
    cJSON *activate_item = cJSON_GetObjectItem(root, "activate");
    if (activate_item && cJSON_IsBool(activate_item)) {
        activate = cJSON_IsTrue(activate_item);
    }

    // Extract optional "rename_from" string (must copy before cJSON_Delete)
    char rename_from[PLAYSET_MAX_NAME_LEN + 1] = {0};
    cJSON *rename_item = cJSON_GetObjectItem(root, "rename_from");
    if (rename_item && cJSON_IsString(rename_item) && rename_item->valuestring[0] != '\0') {
        strncpy(rename_from, rename_item->valuestring, PLAYSET_MAX_NAME_LEN);
        rename_from[PLAYSET_MAX_NAME_LEN] = '\0';
    }

    // Parse playset
    ps_scheduler_command_t *cmd = calloc(1, sizeof(ps_scheduler_command_t));
    if (!cmd) {
        cJSON_Delete(root);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    esp_err_t parse_err = playset_json_parse(root, cmd);
    cJSON_Delete(root);

    if (parse_err != ESP_OK) {
        free(cmd);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid playset definition\",\"code\":\"INVALID_PLAYSET\"}");
        return ESP_OK;
    }

    strlcpy(cmd->name, name, sizeof(cmd->name));
    esp_err_t save_err = playset_store_save(name, cmd);
    if (save_err != ESP_OK) {
        free(cmd);
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"ok\":false,\"error\":\"Failed to save playset: %s\",\"code\":\"SAVE_ERROR\"}",
                 esp_err_to_name(save_err));
        send_json(req, 500, err_msg);
        return ESP_OK;
    }

    // Activate if requested
    bool activated = false;
    if (activate) {
        esp_err_t exec_err = play_scheduler_execute_command(cmd);
        if (exec_err == ESP_OK) {
            p3a_state_set_active_playset(name);
            activated = true;
        }
    }

    free(cmd);

    // Handle rename: delete old file if rename_from is set and different from target name
    bool renamed = false;
    if (rename_from[0] != '\0' && strcmp(rename_from, name) != 0) {
        // Don't allow renaming from a protected playset
        bool rename_protected = false;
        for (size_t i = 0; i < sizeof(protected_playsets) / sizeof(protected_playsets[0]); i++) {
            if (strcmp(rename_from, protected_playsets[i]) == 0) {
                rename_protected = true;
                break;
            }
        }
        if (!rename_protected) {
            playset_store_delete(rename_from);
            // Update active playset reference if it pointed to the old name
            const char *active = p3a_state_get_active_playset();
            if (active && strcmp(active, rename_from) == 0) {
                p3a_state_set_active_playset(name);
            }
            renamed = true;
        }
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON *data = cJSON_AddObjectToObject(resp, "data");
    cJSON_AddBoolToObject(data, "saved", true);
    cJSON_AddBoolToObject(data, "activated", activated);
    cJSON_AddBoolToObject(data, "renamed", renamed);

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

/**
 * DELETE /playsets/{name}
 * Delete a saved playset
 */
esp_err_t h_delete_playset(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *prefix = "/playsets/";
    size_t prefix_len = 10;

    if (strncmp(uri, prefix, prefix_len) != 0 || uri[prefix_len] == '\0') {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid path\",\"code\":\"INVALID_PATH\"}");
        return ESP_OK;
    }

    const char *name_start = uri + prefix_len;
    const char *qmark = strchr(name_start, '?');
    size_t raw_len = qmark ? (size_t)(qmark - name_start) : strlen(name_start);

    char name[PLAYSET_MAX_NAME_LEN + 1];
    if (raw_len >= sizeof(name)) raw_len = sizeof(name) - 1;
    memcpy(name, name_start, raw_len);
    name[raw_len] = '\0';
    url_decode_in_place(name);

    size_t name_len = strlen(name);
    if (!is_valid_playset_name(name, name_len)) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid playset name\",\"code\":\"INVALID_NAME\"}");
        return ESP_OK;
    }

    // Protected playsets cannot be deleted
    static const char *protected_playsets[] = { "followed_artists" };
    for (size_t i = 0; i < sizeof(protected_playsets) / sizeof(protected_playsets[0]); i++) {
        if (strcmp(name, protected_playsets[i]) == 0) {
            send_json(req, 403, "{\"ok\":false,\"error\":\"Cannot delete protected playset\",\"code\":\"PROTECTED_PLAYSET\"}");
            return ESP_OK;
        }
    }

    if (!playset_store_exists(name)) {
        send_json(req, 404, "{\"ok\":false,\"error\":\"Playset not found\",\"code\":\"NOT_FOUND\"}");
        return ESP_OK;
    }

    esp_err_t err = playset_store_delete(name);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to delete playset\",\"code\":\"DELETE_ERROR\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}
