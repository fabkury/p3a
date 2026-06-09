// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

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
#include "makapix.h"
#include "makapix_store.h"
#include "config_store.h"
#include "event_bus.h"
#include "p3a_current_post.h"
#include "p3a_reaction_dispatcher.h"
#include "p3a_pin_dispatcher.h"
#include "slave_ota.h"
#include <sys/stat.h>
#include <string.h>

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
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    if (slave_ota_is_in_progress()) {
        send_json_error(req, 503, "OTA_IN_PROGRESS", "Firmware update in progress");
        return ESP_OK;
    }

    cJSON *root = recv_json_object(req);
    if (!root) return ESP_OK;

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
        send_json_error(req, 400, "INVALID_REQUEST", "Missing channel_name, hashtag, or user_sqid");
        return ESP_OK;
    }

    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) {
        send_json_error(req, 400, "INVALID_CHANNEL", "Invalid channel");
        return ESP_OK;
    }

    if (err != ESP_OK) {
        send_json_error(req, 500, "CHANNEL_SWITCH_FAILED", "Channel switch failed");
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
    /* Deprecated. Derives the "playset" and "channel_name" strings from the
       structured active playset for any 3rd-party consumers still polling
       this endpoint. The fields are best-effort — only single-channel
       built-ins get a stable string; everything else returns ""/"other". */
    const char *playset = "";
    const char *channel_name = "other";
    ps_playset_t *active = calloc(1, sizeof(ps_playset_t));

    if (active && play_scheduler_get_active_playset(active) == ESP_OK) {
        /* Prefer the playset's stored name (user-saved / followed_artists). */
        if (active->name[0] != '\0') {
            playset = active->name;
            if (strcmp(active->name, "followed_artists") == 0) channel_name = "followed_artists";
        }
        if (active->channel_count == 1) {
            const ps_channel_spec_t *ch = &active->channels[0];
            switch (ch->type) {
                case PS_CHANNEL_TYPE_NAMED:
                    if (strcmp(ch->name, "all") == 0) {
                        if (!*playset) playset = "channel_recent";
                        channel_name = "all";
                    } else if (strcmp(ch->name, "promoted") == 0) {
                        if (!*playset) playset = "channel_promoted";
                        channel_name = "promoted";
                    }
                    break;
                case PS_CHANNEL_TYPE_SDCARD:
                    if (!*playset) playset = "channel_sdcard";
                    channel_name = "sdcard";
                    break;
                case PS_CHANNEL_TYPE_GIPHY:
                    if (!*playset) playset = "giphy_trending";
                    channel_name = "giphy_trending";
                    break;
                default:
                    break;
            }
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(active);
        send_json_oom(req);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        free(active);
        cJSON_Delete(root);
        send_json_oom(req);
        return ESP_OK;
    }

    cJSON_AddStringToObject(data, "playset", playset);
    cJSON_AddStringToObject(data, "channel_name", channel_name);
    cJSON_AddItemToObject(root, "data", data);
    free(active);

    send_json_root(req, 200, root);
    return ESP_OK;
}

// ---------- Action Handlers ----------

/**
 * POST /action/reboot
 */
esp_err_t h_post_reboot(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    if (!api_enqueue_reboot()) {
        send_json_error(req, 503, "QUEUE_FULL", "Queue full");
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
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }
    if (!api_enqueue_swap_next()) {
        send_json_error(req, 503, "QUEUE_FULL", "Queue full");
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
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }
    if (!api_enqueue_swap_back()) {
        send_json_error(req, 503, "QUEUE_FULL", "Queue full");
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
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    if (!api_enqueue_pause()) {
        send_json_error(req, 503, "QUEUE_FULL", "Queue full");
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
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    if (!api_enqueue_resume()) {
        send_json_error(req, 503, "QUEUE_FULL", "Queue full");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"resume\"}}");
    return ESP_OK;
}

/**
 * POST /action/reset_dwell_timer
 *
 * Restart the auto-swap dwell timer from zero. The web UI calls this when
 * the user opens the title-view panel so the artwork doesn't swap out from
 * under them while they're reading the title.
 */
esp_err_t h_post_reset_dwell_timer(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }
    play_scheduler_reset_timer();
    send_json(req, 200, "{\"ok\":true}");
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
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    cJSON *root = recv_json_object(req);
    if (!root) return ESP_OK;

    cJSON *artwork_url = cJSON_GetObjectItem(root, "artwork_url");
    if (!artwork_url || !cJSON_IsString(artwork_url) || cJSON_GetStringValue(artwork_url)[0] == '\0') {
        cJSON_Delete(root);
        send_json_error(req, 400, "MISSING_FIELD", "Missing or empty 'artwork_url'");
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
        send_json_errorf(req, 500, "START_FAILED", "Failed to start download: %s",
                         esp_err_to_name(err));
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
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    if (slave_ota_is_in_progress()) {
        send_json_error(req, 503, "OTA_IN_PROGRESS", "Firmware update in progress");
        return ESP_OK;
    }

    cJSON *root = recv_json_object(req);
    if (!root) return ESP_OK;

    cJSON *channel = cJSON_GetObjectItem(root, "channel");
    if (!channel || !cJSON_IsString(channel)) {
        cJSON_Delete(root);
        send_json_error(req, 400, "MISSING_FIELD", "Missing 'channel'");
        return ESP_OK;
    }

    const char *ch = cJSON_GetStringValue(channel);
    esp_err_t err;

    if (strcmp(ch, "sdcard") == 0) {
        // SD card channel: swap by filename
        cJSON *filename_item = cJSON_GetObjectItem(root, "filename");
        if (!filename_item || !cJSON_IsString(filename_item) || cJSON_GetStringValue(filename_item)[0] == '\0') {
            cJSON_Delete(root);
            send_json_error(req, 400, "MISSING_FIELD", "Missing 'filename' for sdcard channel");
            return ESP_OK;
        }

        const char *fname = cJSON_GetStringValue(filename_item);

        // Build full path and verify file exists
        char animations_dir[128];
        if (sd_path_get_animations(animations_dir, sizeof(animations_dir)) != ESP_OK) {
            cJSON_Delete(root);
            send_json_error(req, 500, "PATH_ERROR", "Failed to get animations path");
            return ESP_OK;
        }

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", animations_dir, fname);

        struct stat st;
        if (stat(filepath, &st) != 0) {
            cJSON_Delete(root);
            send_json_error(req, 404, "NOT_FOUND", "File not found in animations directory");
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
            send_json_error(req, 400, "MISSING_FIELD", "Missing 'post_id' for Makapix channel");
            return ESP_OK;
        }
        if (!storage_key_item || !cJSON_IsString(storage_key_item)) {
            cJSON_Delete(root);
            send_json_error(req, 400, "MISSING_FIELD", "Missing 'storage_key' for Makapix channel");
            return ESP_OK;
        }
        if (!art_url_item || !cJSON_IsString(art_url_item)) {
            cJSON_Delete(root);
            send_json_error(req, 400, "MISSING_FIELD", "Missing 'art_url' for Makapix channel");
            return ESP_OK;
        }

        int32_t post_id = (int32_t)cJSON_GetNumberValue(post_id_item);
        const char *storage_key = cJSON_GetStringValue(storage_key_item);
        const char *art_url = cJSON_GetStringValue(art_url_item);

        // REST swap_to has no post-title context.
        err = play_scheduler_play_artwork(post_id, storage_key, art_url, NULL);
    }

    cJSON_Delete(root);

    if (err != ESP_OK) {
        send_json_errorf(req, 500, "SWAP_FAILED", "Swap failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true,\"data\":{\"action\":\"swap_to\"}}");
    return ESP_OK;
}

// ---------- Provisioning Handler ----------

/**
 * POST /action/provision
 * Enter or exit Makapix provisioning mode.
 * Body: { "enable": true } or { "enable": false }
 */
esp_err_t h_post_provision(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    cJSON *root = recv_json_object(req);
    if (!root) return ESP_OK;

    cJSON *enable_item = cJSON_GetObjectItem(root, "enable");
    if (!enable_item || !cJSON_IsBool(enable_item)) {
        cJSON_Delete(root);
        send_json_error(req, 400, "MISSING_FIELD", "Missing 'enable' boolean");
        return ESP_OK;
    }

    bool enable = cJSON_IsTrue(enable_item);
    cJSON_Delete(root);

    if (enable) {
        // Enter provisioning
        p3a_state_t current = p3a_state_get();
        if (current != P3A_STATE_ANIMATION_PLAYBACK) {
            send_json_error(req, 409, "INVALID_STATE", "Device must be in playback state");
            return ESP_OK;
        }

        esp_err_t err = p3a_state_enter_provisioning();
        if (err != ESP_OK) {
            // Force start if we're in animation playback
            if (current == P3A_STATE_ANIMATION_PLAYBACK) {
                ESP_LOGW("HTTP", "State transition denied, forcing provisioning from playback");
            } else {
                send_json_error(req, 409, "INVALID_STATE", "State transition denied");
                return ESP_OK;
            }
        }

        makapix_start_provisioning();
        send_json(req, 200, "{\"ok\":true,\"data\":{\"action\":\"provision\",\"enabled\":true}}");
    } else {
        // Exit provisioning
        makapix_cancel_provisioning();
        p3a_state_exit_to_playback();
        event_bus_emit_simple(P3A_EVENT_SWAP_NEXT);
        send_json(req, 200, "{\"ok\":true,\"data\":{\"action\":\"provision\",\"enabled\":false}}");
    }

    return ESP_OK;
}

// ---------- Makapix Unregister Handler ----------

/**
 * POST /action/makapix_unregister
 * Remove Makapix Club registration (disconnect MQTT, wipe credentials)
 */
esp_err_t h_post_makapix_unregister(httpd_req_t *req) {
    if (!makapix_store_has_player_key()) {
        send_json_error(req, 409, "NOT_REGISTERED", "Not registered");
        return ESP_OK;
    }

    esp_err_t err = makapix_unregister();
    if (err != ESP_OK) {
        send_json_error(req, 500, "UNREGISTER_FAILED", "Unregister failed");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

// ---------- Reaction Handler ----------

/**
 * POST /action/reaction
 * Submit or revoke a reaction to the currently displayed artwork.
 *
 * Body (Makapix):  {"action":"submit"|"revoke", "post_id": <int>}
 * Body (Giphy):    {"action":"submit", "giphy_id": "<string>"}
 *
 * The handler validates that the supplied identifier matches the currently
 * displayed post and that the action is valid for the source. On success it
 * dispatches the network call asynchronously and returns 200 immediately.
 * Failures of the underlying network call surface via the next poll of
 * /playsets/active (the reaction_submitted flag reverts).
 */
esp_err_t h_post_action_reaction(httpd_req_t *req)
{
    if (!ensure_json_content(req)) {
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    cJSON *root = recv_json_object(req);
    if (!root) return ESP_OK;

    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    cJSON *post_id_item = cJSON_GetObjectItem(root, "post_id");
    cJSON *giphy_id_item = cJSON_GetObjectItem(root, "giphy_id");

    if (!action_item || !cJSON_IsString(action_item)) {
        cJSON_Delete(root);
        send_json_error(req, 400, "INVALID_ACTION", "Missing or invalid 'action'");
        return ESP_OK;
    }
    const char *action = cJSON_GetStringValue(action_item);
    bool is_submit;
    if (strcmp(action, "submit") == 0) {
        is_submit = true;
    } else if (strcmp(action, "revoke") == 0) {
        is_submit = false;
    } else {
        cJSON_Delete(root);
        send_json_error(req, 400, "INVALID_ACTION", "action must be 'submit' or 'revoke'");
        return ESP_OK;
    }

    bool has_post_id = post_id_item && cJSON_IsNumber(post_id_item);
    bool has_giphy_id = giphy_id_item && cJSON_IsString(giphy_id_item) &&
                        cJSON_GetStringValue(giphy_id_item)[0] != '\0';
    if (has_post_id == has_giphy_id) {
        cJSON_Delete(root);
        send_json_error(req, 400, "INVALID_REQUEST", "Provide exactly one of post_id or giphy_id");
        return ESP_OK;
    }

    int current_source = p3a_current_post_get_source();
    esp_err_t derr;

    if (has_post_id) {
        int32_t post_id = (int32_t)cJSON_GetNumberValue(post_id_item);
        cJSON_Delete(root);
        if (post_id <= 0) {
            send_json_error(req, 400, "INVALID_REQUEST", "post_id must be positive");
            return ESP_OK;
        }
        if (current_source != POST_SOURCE_MAKAPIX || p3a_current_post_get_id() != post_id) {
            send_json_error(req, 409, "STALE_POST", "post_id does not match the currently displayed artwork");
            return ESP_OK;
        }
        derr = is_submit ? p3a_reaction_dispatch_makapix_submit(post_id)
                         : p3a_reaction_dispatch_makapix_revoke(post_id);
        if (derr == ESP_ERR_INVALID_STATE) {
            send_json_error(req, 503, "MQTT_NOT_CONNECTED", "Makapix MQTT not connected");
            return ESP_OK;
        }
        if (derr != ESP_OK) {
            send_json_error(req, 500, "DISPATCH_FAILED", "Dispatch failed");
            return ESP_OK;
        }
        send_json(req, 200, "{\"ok\":true}");
        return ESP_OK;
    }

    // Giphy path
    if (!is_submit) {
        cJSON_Delete(root);
        send_json_error(req, 400, "INVALID_ACTION", "revoke is not supported for Giphy");
        return ESP_OK;
    }
    char giphy_id[24];
    strlcpy(giphy_id, cJSON_GetStringValue(giphy_id_item), sizeof(giphy_id));
    cJSON_Delete(root);

    if (current_source != POST_SOURCE_GIPHY) {
        send_json_error(req, 409, "STALE_POST", "Current artwork is not from Giphy");
        return ESP_OK;
    }
    char current_giphy[24];
    p3a_current_post_get_giphy_id(current_giphy, sizeof(current_giphy));
    if (strcmp(current_giphy, giphy_id) != 0) {
        send_json_error(req, 409, "STALE_POST", "giphy_id does not match the currently displayed artwork");
        return ESP_OK;
    }
    derr = p3a_reaction_dispatch_giphy_click(giphy_id);
    if (derr == ESP_ERR_INVALID_STATE) {
        send_json_error(req, 503, "GIPHY_NOT_CONFIGURED", "Giphy API key or random_id not configured");
        return ESP_OK;
    }
    if (derr != ESP_OK) {
        send_json_error(req, 500, "DISPATCH_FAILED", "Dispatch failed");
        return ESP_OK;
    }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

// ---------- Giphy Reset Random ID Handler ----------

/**
 * POST /action/giphy_reset_random_id
 * Delete the persisted Giphy random_id so a new one is obtained on next refresh
 */
esp_err_t h_post_giphy_reset_random_id(httpd_req_t *req) {
    esp_err_t err = config_store_delete_giphy_random_id();
    if (err != ESP_OK) {
        send_json_error(req, 500, "DELETE_FAILED", "Failed to delete random_id");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

// ---------- Pin / Unpin (convenience: target the currently displayed post) ----------

/**
 * POST /action/pin   /   POST /action/unpin
 * Body (one of):
 *   { "post_id":  <int>,    "list"?: "<slug>" }  // makapix or museum
 *   { "giphy_id": "<str>",  "list"?: "<slug>" }  // giphy
 *
 * Validates the identifier against the currently displayed artwork (409
 * STALE_POST otherwise) and dispatches asynchronously. The response is HTTP
 * 200 as soon as the dispatch succeeds; the actual SD-card copy + index
 * update happens in a worker task.
 */
static esp_err_t pin_or_unpin_handler(httpd_req_t *req, bool is_unpin)
{
    if (!ensure_json_content(req)) {
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }
    cJSON *root = recv_json_object(req);
    if (!root) return ESP_OK;
    cJSON *post_id_item  = cJSON_GetObjectItem(root, "post_id");
    cJSON *giphy_id_item = cJSON_GetObjectItem(root, "giphy_id");
    cJSON *list_item     = cJSON_GetObjectItem(root, "list");

    char slug[12] = {0};
    if (list_item && cJSON_IsString(list_item)) {
        strlcpy(slug, cJSON_GetStringValue(list_item), sizeof(slug));
    }

    int current_source = p3a_current_post_get_source();
    bool has_post_id  = post_id_item && cJSON_IsNumber(post_id_item);
    bool has_giphy_id = giphy_id_item && cJSON_IsString(giphy_id_item) &&
                        cJSON_GetStringValue(giphy_id_item)[0] != '\0';
    if (has_post_id == has_giphy_id) {
        cJSON_Delete(root);
        send_json_error(req, 400, "INVALID_REQUEST", "Provide exactly one of post_id or giphy_id");
        return ESP_OK;
    }

    if (has_giphy_id) {
        char body_gid[24];
        strlcpy(body_gid, cJSON_GetStringValue(giphy_id_item), sizeof(body_gid));
        cJSON_Delete(root);
        if (current_source != POST_SOURCE_GIPHY) {
            send_json_error(req, 409, "STALE_POST", "Current artwork is not from Giphy");
            return ESP_OK;
        }
        char cur_gid[24];
        p3a_current_post_get_giphy_id(cur_gid, sizeof(cur_gid));
        if (strcmp(cur_gid, body_gid) != 0) {
            send_json_error(req, 409, "STALE_POST", "giphy_id does not match the currently displayed artwork");
            return ESP_OK;
        }
    } else {
        int32_t post_id = (int32_t)cJSON_GetNumberValue(post_id_item);
        cJSON_Delete(root);
        if (post_id <= 0) {
            send_json_error(req, 400, "INVALID_REQUEST", "post_id must be positive");
            return ESP_OK;
        }
        if ((current_source != POST_SOURCE_MAKAPIX && current_source != POST_SOURCE_INSTITUTION) ||
            p3a_current_post_get_id() != post_id) {
            send_json_error(req, 409, "STALE_POST", "post_id does not match the currently displayed artwork");
            return ESP_OK;
        }
    }

    /* All sources route through the from_current dispatcher; the dispatcher
       reads p3a_current_post directly to resolve source-specific identifiers. */
    esp_err_t derr = is_unpin
        ? p3a_pin_dispatch_unpin_from_current(slug[0] ? slug : NULL)
        : p3a_pin_dispatch_from_current(slug[0] ? slug : NULL);
    if (derr == ESP_ERR_NOT_SUPPORTED) {
        send_json_error(req, 501, "NOT_SUPPORTED", "Source not yet supported");
        return ESP_OK;
    }
    if (derr != ESP_OK) {
        send_json_error(req, 500, "DISPATCH_FAILED", "Dispatch failed");
        return ESP_OK;
    }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t h_post_action_pin(httpd_req_t *req)
{
    return pin_or_unpin_handler(req, false);
}

esp_err_t h_post_action_unpin(httpd_req_t *req)
{
    return pin_or_unpin_handler(req, true);
}
