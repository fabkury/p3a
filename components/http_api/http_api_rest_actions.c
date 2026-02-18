// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_rest_actions.c
 * @brief Playback action and channel switching REST handlers
 *
 * Contains handlers for:
 * - POST /channel, GET /channel - Channel switching
 * - POST /action/reboot, swap_next, swap_back, pause, resume - Actions
 * - POST /action/show_url - Play artwork from URL
 * - POST /action/swap_to - Swap to specific artwork
 */

#include "http_api_internal.h"
#include "sd_path.h"
#include "playback_service.h"
#include "play_scheduler.h"
#include "p3a_state.h"
#include "show_url.h"
#include <sys/stat.h>

// Processing notification (from display_renderer_priv.h via weak symbol)
extern void proc_notif_start(void) __attribute__((weak));

// ---------- Channel Handlers ----------

/**
 * POST /channel
 * Switch to a channel using Play Scheduler
 * Body: {"channel_name": "all"|"promoted"|"sdcard"} or {"hashtag": "..."} or {"user_sqid": "..."}
 */
esp_err_t h_post_channel(httpd_req_t *req) {
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

    // Parse: exactly ONE of channel_name, hashtag, or user_sqid
    cJSON *channel_name = cJSON_GetObjectItem(root, "channel_name");
    cJSON *hashtag = cJSON_GetObjectItem(root, "hashtag");
    cJSON *user_sqid = cJSON_GetObjectItem(root, "user_sqid");

    esp_err_t err = ESP_FAIL;

    if (channel_name && cJSON_IsString(channel_name)) {
        // Named channel: "all", "promoted", or "sdcard"
        const char *name = cJSON_GetStringValue(channel_name);
        err = playback_service_play_channel(name);
    } else if (hashtag && cJSON_IsString(hashtag)) {
        // Hashtag channel
        const char *tag = cJSON_GetStringValue(hashtag);
        err = playback_service_play_hashtag_channel(tag);
    } else if (user_sqid && cJSON_IsString(user_sqid)) {
        // User channel
        const char *sqid = cJSON_GetStringValue(user_sqid);
        err = playback_service_play_user_channel(sqid);
    } else {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing channel_name, hashtag, or user_sqid\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid channel\",\"code\":\"INVALID_CHANNEL\"}");
        return ESP_OK;
    }

    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Channel switch failed\",\"code\":\"CHANNEL_SWITCH_FAILED\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/**
 * GET /channel
 * Get current channel/playset information
 * Returns: {"ok": true, "data": {"playset": "channel_recent"|"channel_promoted"|"channel_sdcard"|"followed_artists"|...}}
 *
 * For backwards compatibility, also includes "channel_name" mapped from playset.
 *
 * @deprecated Use GET /playsets/active instead. This endpoint will be removed in a future version.
 */
esp_err_t h_get_channel(httpd_req_t *req) {
    // Get the active playset name (primary source of truth)
    const char *playset = p3a_state_get_active_playset();

    // For backwards compatibility, also get channel info
    p3a_channel_info_t channel_info;
    esp_err_t err = p3a_state_get_channel_info(&channel_info);

    // Map playset to channel_name for backwards compatibility
    const char *channel_name = "other";
    if (playset && playset[0] != '\0') {
        if (strcmp(playset, "channel_recent") == 0) {
            channel_name = "all";
        } else if (strcmp(playset, "channel_promoted") == 0) {
            channel_name = "promoted";
        } else if (strcmp(playset, "channel_sdcard") == 0) {
            channel_name = "sdcard";
        } else if (strcmp(playset, "followed_artists") == 0) {
            channel_name = "followed_artists";
        } else if (strcmp(playset, "giphy_trending") == 0) {
            channel_name = "giphy_trending";
        }
    } else if (err == ESP_OK) {
        // Fallback to channel type if no playset set (legacy)
        switch (channel_info.type) {
            case P3A_CHANNEL_SDCARD:
                channel_name = "sdcard";
                break;
            case P3A_CHANNEL_MAKAPIX_ALL:
                channel_name = "all";
                break;
            case P3A_CHANNEL_MAKAPIX_PROMOTED:
                channel_name = "promoted";
                break;
            case P3A_CHANNEL_GIPHY_TRENDING:
                channel_name = "giphy_trending";
                break;
            default:
                channel_name = "other";
                break;
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    // Primary: playset name
    cJSON_AddStringToObject(data, "playset", playset ? playset : "");
    // Backwards compatibility: channel_name
    cJSON_AddStringToObject(data, "channel_name", channel_name);
    cJSON_AddItemToObject(root, "data", data);

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

// ---------- Action Handlers ----------

/**
 * POST /action/reboot
 */
esp_err_t h_post_reboot(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    if (!api_enqueue_reboot()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"reboot\"}}");
    return ESP_OK;
}

/**
 * POST /action/swap_next
 */
esp_err_t h_post_swap_next(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }
    if (!api_enqueue_swap_next()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }
    // Start processing notification after confirming swap was queued
    if (proc_notif_start) {
        proc_notif_start();
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"swap_next\"}}");
    return ESP_OK;
}

/**
 * POST /action/swap_back
 */
esp_err_t h_post_swap_back(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }
    if (!api_enqueue_swap_back()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }
    // Start processing notification after confirming swap was queued
    if (proc_notif_start) {
        proc_notif_start();
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"swap_back\"}}");
    return ESP_OK;
}

/**
 * POST /action/pause
 */
esp_err_t h_post_pause(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    if (!api_enqueue_pause()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"pause\"}}");
    return ESP_OK;
}

/**
 * POST /action/resume
 */
esp_err_t h_post_resume(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    if (!api_enqueue_resume()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"resume\"}}");
    return ESP_OK;
}

// ---------- Show URL Handler ----------

/**
 * POST /action/show_url
 * Download artwork from URL and play it.
 * JSON body: { "artwork_url": "...", "blocking": true/false }
 * blocking defaults to true if not provided.
 */
esp_err_t h_post_show_url(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status = 0;
    size_t body_len = 0;
    char *body = recv_body_json(req, &body_len, &err_status);
    if (!body) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"ok\":false,\"error\":\"Failed to read body\",\"code\":\"BODY_READ_ERROR\"}");
        send_json(req, err_status ? err_status : 400, err_msg);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *artwork_url = cJSON_GetObjectItem(root, "artwork_url");
    if (!artwork_url || !cJSON_IsString(artwork_url) || cJSON_GetStringValue(artwork_url)[0] == '\0') {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or empty 'artwork_url'\",\"code\":\"MISSING_FIELD\"}");
        return ESP_OK;
    }

    cJSON *blocking_item = cJSON_GetObjectItem(root, "blocking");
    bool blocking = true; // Default
    if (blocking_item && cJSON_IsBool(blocking_item)) {
        blocking = cJSON_IsTrue(blocking_item);
    }

    const char *url = cJSON_GetStringValue(artwork_url);
    esp_err_t err = show_url_start(url, blocking);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"ok\":false,\"error\":\"Failed to start download: %s\",\"code\":\"START_FAILED\"}",
                 esp_err_to_name(err));
        send_json(req, 500, err_msg);
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"show_url\"}}");
    return ESP_OK;
}

// ---------- Swap To Handler ----------

/**
 * POST /action/swap_to
 * Swap to a specific artwork.
 * For sdcard: { "channel": "sdcard", "filename": "art.gif" }
 * For Makapix: { "channel": "<name>", "post_id": 123, "storage_key": "...", "art_url": "..." }
 */
esp_err_t h_post_swap_to(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status = 0;
    size_t body_len = 0;
    char *body = recv_body_json(req, &body_len, &err_status);
    if (!body) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"ok\":false,\"error\":\"Failed to read body\",\"code\":\"BODY_READ_ERROR\"}");
        send_json(req, err_status ? err_status : 400, err_msg);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *channel = cJSON_GetObjectItem(root, "channel");
    if (!channel || !cJSON_IsString(channel)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing 'channel'\",\"code\":\"MISSING_FIELD\"}");
        return ESP_OK;
    }

    const char *ch = cJSON_GetStringValue(channel);
    esp_err_t err;

    if (strcmp(ch, "sdcard") == 0) {
        // SD card channel: swap by filename
        cJSON *filename_item = cJSON_GetObjectItem(root, "filename");
        if (!filename_item || !cJSON_IsString(filename_item) || cJSON_GetStringValue(filename_item)[0] == '\0') {
            cJSON_Delete(root);
            send_json(req, 400, "{\"ok\":false,\"error\":\"Missing 'filename' for sdcard channel\",\"code\":\"MISSING_FIELD\"}");
            return ESP_OK;
        }

        const char *fname = cJSON_GetStringValue(filename_item);

        // Build full path and verify file exists
        char animations_dir[128];
        if (sd_path_get_animations(animations_dir, sizeof(animations_dir)) != ESP_OK) {
            cJSON_Delete(root);
            send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to get animations path\",\"code\":\"PATH_ERROR\"}");
            return ESP_OK;
        }

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", animations_dir, fname);

        struct stat st;
        if (stat(filepath, &st) != 0) {
            cJSON_Delete(root);
            send_json(req, 404, "{\"ok\":false,\"error\":\"File not found in animations directory\",\"code\":\"NOT_FOUND\"}");
            return ESP_OK;
        }

        err = play_scheduler_play_local_file(filepath);
    } else {
        // Makapix channel: swap by post_id
        cJSON *post_id_item = cJSON_GetObjectItem(root, "post_id");
        cJSON *storage_key_item = cJSON_GetObjectItem(root, "storage_key");
        cJSON *art_url_item = cJSON_GetObjectItem(root, "art_url");

        if (!post_id_item || !cJSON_IsNumber(post_id_item)) {
            cJSON_Delete(root);
            send_json(req, 400, "{\"ok\":false,\"error\":\"Missing 'post_id' for Makapix channel\",\"code\":\"MISSING_FIELD\"}");
            return ESP_OK;
        }
        if (!storage_key_item || !cJSON_IsString(storage_key_item)) {
            cJSON_Delete(root);
            send_json(req, 400, "{\"ok\":false,\"error\":\"Missing 'storage_key' for Makapix channel\",\"code\":\"MISSING_FIELD\"}");
            return ESP_OK;
        }
        if (!art_url_item || !cJSON_IsString(art_url_item)) {
            cJSON_Delete(root);
            send_json(req, 400, "{\"ok\":false,\"error\":\"Missing 'art_url' for Makapix channel\",\"code\":\"MISSING_FIELD\"}");
            return ESP_OK;
        }

        int32_t post_id = (int32_t)cJSON_GetNumberValue(post_id_item);
        const char *storage_key = cJSON_GetStringValue(storage_key_item);
        const char *art_url = cJSON_GetStringValue(art_url_item);

        err = play_scheduler_play_artwork(post_id, storage_key, art_url);
    }

    cJSON_Delete(root);

    if (err != ESP_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"ok\":false,\"error\":\"Swap failed: %s\",\"code\":\"SWAP_FAILED\"}",
                 esp_err_to_name(err));
        send_json(req, 500, err_msg);
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true,\"data\":{\"action\":\"swap_to\"}}");
    return ESP_OK;
}
