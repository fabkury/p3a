/**
 * @file http_api_rest.c
 * @brief HTTP API REST handlers
 * 
 * Contains handlers for:
 * - GET /status, GET /api/state - Device status endpoints
 * - GET/PUT /config - Configuration management
 * - POST /channel - Channel switching
 * - GET/PUT /settings/dwell_time - Dwell time settings
 * - GET/PUT /settings/global_seed - Global seed settings
 * - GET/PUT /settings/play_order - Play order settings
 * - GET /channels/stats - Channel cache statistics
 * - POST /action/reboot, swap_next, swap_back, pause, resume - Actions
 * - GET/POST /rotation - Screen rotation
 * - POST /debug (dev mode only)
 */

#include "http_api_internal.h"
#include "sd_path.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "app_state.h"
#include "config_store.h"
#include "makapix.h"
#include "makapix_store.h"
#include "makapix_channel_impl.h"
#include "animation_player.h"
#include "channel_player.h"
#include "app_lcd.h"
#include "version.h"
#include "p3a_state.h"
#include "freertos/semphr.h"

// ---------- Debug Handler (Dev Mode) ----------

#if CONFIG_OTA_DEV_MODE
#include "swap_future.h"
#include "playlist_manager.h"
#include <sys/time.h>

// External: wakes the auto_swap_task in main/p3a_main.c.
extern void auto_swap_reset_timer(void);

static uint64_t wall_clock_ms_http(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

/**
 * POST /debug  (CONFIG_OTA_DEV_MODE only)
 */
esp_err_t h_post_debug(httpd_req_t *req)
{
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *op = cJSON_GetObjectItem(root, "op");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    const char *op_s = (op && cJSON_IsString(op)) ? cJSON_GetStringValue(op) : NULL;

    if (!op_s || !*op_s) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'op'\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "op", op_s);

    if (strcmp(op_s, "swap_future_cancel") == 0) {
        swap_future_cancel();
        auto_swap_reset_timer();
        cJSON_AddStringToObject(resp, "result", "cancelled");
    } else if (strcmp(op_s, "live_mode_enter") == 0 || strcmp(op_s, "live_mode_exit") == 0) {
        void *nav = channel_player_get_navigator();
        if (!nav) {
            cJSON_Delete(root);
            cJSON_Delete(resp);
            send_json(req, 409, "{\"ok\":false,\"error\":\"No navigator\",\"code\":\"NO_NAV\"}");
            return ESP_OK;
        }
        esp_err_t e = (strcmp(op_s, "live_mode_enter") == 0) ? live_mode_enter(nav) : (live_mode_exit(nav), ESP_OK);
        cJSON_AddNumberToObject(resp, "esp_err", (double)e);
    } else if (strcmp(op_s, "swap_future_test") == 0) {
        // Build swap_future targeting the current file.
        const sdcard_post_t *post = NULL;
        if (channel_player_get_current_post(&post) != ESP_OK || !post || !post->filepath) {
            cJSON_Delete(root);
            cJSON_Delete(resp);
            send_json(req, 409, "{\"ok\":false,\"error\":\"No current post\",\"code\":\"NO_CURRENT\"}");
            return ESP_OK;
        }

        uint32_t delay_ms = 1000;
        uint32_t start_offset_ms = 0;
        uint32_t start_frame = 0;

        if (data && cJSON_IsObject(data)) {
            cJSON *d = cJSON_GetObjectItem(data, "delay_ms");
            if (d && cJSON_IsNumber(d) && cJSON_GetNumberValue(d) >= 0) {
                delay_ms = (uint32_t)cJSON_GetNumberValue(d);
            }
            cJSON *o = cJSON_GetObjectItem(data, "start_offset_ms");
            if (o && cJSON_IsNumber(o) && cJSON_GetNumberValue(o) >= 0) {
                start_offset_ms = (uint32_t)cJSON_GetNumberValue(o);
            }
            cJSON *sf = cJSON_GetObjectItem(data, "start_frame");
            if (sf && cJSON_IsNumber(sf) && cJSON_GetNumberValue(sf) >= 0) {
                start_frame = (uint32_t)cJSON_GetNumberValue(sf);
            }
        }

        uint64_t now_ms = wall_clock_ms_http();
        uint64_t target_ms = now_ms + (uint64_t)delay_ms;
        uint64_t start_ms = (start_offset_ms <= delay_ms) ? (target_ms - (uint64_t)start_offset_ms) : target_ms;

        artwork_ref_t art = {0};
        strlcpy(art.filepath, post->filepath, sizeof(art.filepath));
        art.type = post->type;
        art.dwell_time_ms = post->dwell_time_ms;
        art.downloaded = true;

        swap_future_t sf = {0};
        sf.valid = true;
        sf.target_time_ms = target_ms;
        sf.start_time_ms = start_ms;
        sf.start_frame = start_frame;
        sf.artwork = art;
        sf.is_live_mode_swap = false;
        sf.is_automated = true;

        swap_future_cancel();
        esp_err_t e = swap_future_schedule(&sf);
        auto_swap_reset_timer();

        cJSON_AddNumberToObject(resp, "esp_err", (double)e);
        cJSON_AddNumberToObject(resp, "now_ms", (double)now_ms);
        cJSON_AddNumberToObject(resp, "target_time_ms", (double)target_ms);
        cJSON_AddNumberToObject(resp, "start_time_ms", (double)start_ms);
        cJSON_AddNumberToObject(resp, "start_frame", (double)start_frame);
        cJSON_AddStringToObject(resp, "filepath", post->filepath);
    } else {
        cJSON_Delete(root);
        cJSON_Delete(resp);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Unknown op\",\"code\":\"UNKNOWN_OP\"}");
        return ESP_OK;
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(root);
    cJSON_Delete(resp);
    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }
    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}
#endif // CONFIG_OTA_DEV_MODE

// ---------- UI Configuration Handler ----------

/**
 * GET /api/ui-config
 * Returns configuration needed by web UI (LCD dimensions, feature flags)
 */
esp_err_t h_get_ui_config(httpd_req_t *req) {
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddNumberToObject(data, "lcd_width", LCD_MAX_WIDTH);
    cJSON_AddNumberToObject(data, "lcd_height", LCD_MAX_HEIGHT);
#if CONFIG_P3A_PICO8_ENABLE
    cJSON_AddBoolToObject(data, "pico8_enabled", true);
#else
    cJSON_AddBoolToObject(data, "pico8_enabled", false);
#endif

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(data);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, json);
    free(json);
    return ESP_OK;
}

// ---------- Status Handlers ----------

/**
 * GET /status
 * Returns device status including state, uptime, heap, RSSI, firmware info, and queue depth
 */
esp_err_t h_get_status(httpd_req_t *req) {
    wifi_ap_record_t ap = {0};
    int rssi_ok = (esp_wifi_remote_sta_get_ap_info(&ap) == ESP_OK);

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddStringToObject(data, "state", app_state_str(app_state_get()));
    cJSON_AddNumberToObject(data, "uptime_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(data, "heap_free", (double)esp_get_free_heap_size());
    
    if (rssi_ok) {
        cJSON_AddNumberToObject(data, "rssi", ap.rssi);
    } else {
        cJSON_AddNullToObject(data, "rssi");
    }

    cJSON *fw = cJSON_CreateObject();
    if (fw) {
        cJSON_AddStringToObject(fw, "version", FW_VERSION);
        cJSON_AddStringToObject(fw, "idf", IDF_VER);
        cJSON_AddItemToObject(data, "fw", fw);
    }

    uint32_t queue_depth = s_cmdq ? uxQueueMessagesWaiting(s_cmdq) : 0;
    cJSON_AddNumberToObject(data, "queue_depth", (double)queue_depth);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(data);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
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

/**
 * GET /api/state
 * Lightweight state snapshot for UI/automation.
 */
esp_err_t h_get_api_state(httpd_req_t *req)
{
    wifi_ap_record_t ap = {0};
    int rssi_ok = (esp_wifi_remote_sta_get_ap_info(&ap) == ESP_OK);

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddStringToObject(data, "state", app_state_str(app_state_get()));
    cJSON_AddNumberToObject(data, "uptime_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(data, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddBoolToObject(data, "live_mode", channel_player_is_live_mode_active());

    if (rssi_ok) {
        cJSON_AddNumberToObject(data, "rssi", ap.rssi);
    } else {
        cJSON_AddNullToObject(data, "rssi");
    }

    // Current Makapix post_id if available; NULL for SD card or unknown.
    int32_t post_id = makapix_get_current_post_id();
    if (post_id > 0) {
        cJSON_AddNumberToObject(data, "current_post_id", (double)post_id);
    } else {
        cJSON_AddNullToObject(data, "current_post_id");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(data);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
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

/**
 * GET /channels/stats
 * Get cached artwork counts for each Makapix channel
 */
esp_err_t h_get_channels_stats(httpd_req_t *req) {
    // Cache + serialize stats computation
    static SemaphoreHandle_t s_stats_mu = NULL;
    static int64_t s_last_us = 0;
    static size_t s_all_total = 0, s_all_cached = 0;
    static size_t s_promoted_total = 0, s_promoted_cached = 0;

    if (!s_stats_mu) {
        s_stats_mu = xSemaphoreCreateMutex();
    }

    const int64_t now_us = esp_timer_get_time();
    const bool should_refresh = (s_last_us == 0) || ((now_us - s_last_us) > 2 * 1000 * 1000);

    size_t all_total = s_all_total, all_cached = s_all_cached;
    size_t promoted_total = s_promoted_total, promoted_cached = s_promoted_cached;

    bool have_lock = false;
    if (s_stats_mu) {
        have_lock = (xSemaphoreTake(s_stats_mu, pdMS_TO_TICKS(250)) == pdTRUE);
    }

    if (have_lock || !s_stats_mu) {
        if (should_refresh) {
            char channel_path[128], vault_path[128];
            sd_path_get_channel(channel_path, sizeof(channel_path));
            sd_path_get_vault(vault_path, sizeof(vault_path));
            
            makapix_channel_count_cached("all", channel_path, vault_path, &all_total, &all_cached);
            makapix_channel_count_cached("promoted", channel_path, vault_path, &promoted_total, &promoted_cached);

            s_all_total = all_total;
            s_all_cached = all_cached;
            s_promoted_total = promoted_total;
            s_promoted_cached = promoted_cached;
            s_last_us = now_us;
        }
        if (have_lock) xSemaphoreGive(s_stats_mu);
    } else {
        all_total = s_all_total;
        all_cached = s_all_cached;
        promoted_total = s_promoted_total;
        promoted_cached = s_promoted_cached;
    }

    // Check if Makapix is registered (has player_key)
    bool is_registered = makapix_store_has_player_key();
    
    char response[300];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"data\":{"
             "\"all\":{\"total\":%zu,\"cached\":%zu},"
             "\"promoted\":{\"total\":%zu,\"cached\":%zu},"
             "\"registered\":%s"
             "}}",
             all_total, all_cached, promoted_total, promoted_cached,
             is_registered ? "true" : "false");
    send_json(req, 200, response);
    return ESP_OK;
}

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

    esp_err_t e = config_store_save(o);

    if (e != ESP_OK) {
        cJSON_Delete(o);
        send_json(req, 500, "{\"ok\":false,\"error\":\"CONFIG_SAVE_FAIL\",\"code\":\"CONFIG_SAVE_FAIL\"}");
        return ESP_OK;
    }

    // Apply dwell_time_ms change at runtime (for auto-swap interval)
    cJSON *dwell_item = cJSON_GetObjectItem(o, "dwell_time_ms");
    if (dwell_item && cJSON_IsNumber(dwell_item)) {
        uint32_t dwell_ms = (uint32_t)cJSON_GetNumberValue(dwell_item);
        channel_player_set_dwell_time(dwell_ms / 1000);
    }

    cJSON_Delete(o);
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

// ---------- Channel Handler ----------

/**
 * POST /channel
 * Switch to a channel
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

    // Parse new format: exactly ONE of channel_name, hashtag, or user_sqid
    cJSON *channel_name = cJSON_GetObjectItem(root, "channel_name");
    cJSON *hashtag = cJSON_GetObjectItem(root, "hashtag");
    cJSON *user_sqid = cJSON_GetObjectItem(root, "user_sqid");

    const char *ch_name = NULL;
    const char *identifier = NULL;

    if (channel_name && cJSON_IsString(channel_name)) {
        ch_name = cJSON_GetStringValue(channel_name);
    } else if (hashtag && cJSON_IsString(hashtag)) {
        ch_name = "hashtag";
        identifier = cJSON_GetStringValue(hashtag);
    } else if (user_sqid && cJSON_IsString(user_sqid)) {
        ch_name = "by_user";
        identifier = cJSON_GetStringValue(user_sqid);
    } else {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing channel_name, hashtag, or user_sqid\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    esp_err_t err;
    
    // Handle sdcard channel separately
    if (strcmp(ch_name, "sdcard") == 0) {
        if (makapix_is_channel_loading(NULL, 0)) {
            ESP_LOGI(HTTP_API_TAG, "Aborting Makapix channel load for SD card switch");
            makapix_abort_channel_load();
        }
        
        // Clear Makapix current channel state so switching back to same channel works
        makapix_clear_current_channel();
        
        err = p3a_state_switch_channel(P3A_CHANNEL_SDCARD, NULL);
        if (err == ESP_OK) {
            channel_player_switch_to_sdcard_channel();
            channel_player_load_channel();
            channel_player_swap_to(0, 0);  // Start playback at first item
        }
        cJSON_Delete(root);
        if (err != ESP_OK) {
            send_json(req, 500, "{\"ok\":false,\"error\":\"Channel switch failed\",\"code\":\"CHANNEL_SWITCH_FAILED\"}");
            return ESP_OK;
        }
        send_json(req, 200, "{\"ok\":true}");
        return ESP_OK;
    }
    
    // Handle Makapix channels asynchronously
    err = makapix_request_channel_switch(ch_name, identifier);
    
    cJSON_Delete(root);

    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Channel switch request failed\",\"code\":\"CHANNEL_SWITCH_FAILED\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"message\":\"Channel switch initiated\"}");
    return ESP_OK;
}

/**
 * GET /channel
 * Get current channel information
 * Returns: {"ok": true, "data": {"channel_name": "all"|"promoted"|"sdcard"|"other"}}
 */
esp_err_t h_get_channel(httpd_req_t *req) {
    p3a_channel_info_t channel_info;
    esp_err_t err = p3a_state_get_channel_info(&channel_info);
    
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to get channel info\",\"code\":\"GET_CHANNEL_FAILED\"}");
        return ESP_OK;
    }
    
    // Map channel type to channel_name string for Web UI
    const char *channel_name = "other";
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
        case P3A_CHANNEL_MAKAPIX_USER:
        case P3A_CHANNEL_MAKAPIX_BY_USER:
        case P3A_CHANNEL_MAKAPIX_HASHTAG:
        case P3A_CHANNEL_MAKAPIX_ARTWORK:
        default:
            channel_name = "other";
            break;
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

    // Hot-swap the order for the currently active channel
    err = channel_player_set_play_order((uint8_t)order);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // Log but don't fail - config was saved, hot-swap just couldn't happen
        ESP_LOGW(HTTP_API_TAG, "Config saved but hot-swap failed: %s", esp_err_to_name(err));
    }

    send_json(req, 200, "{\"ok\":true}");
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
 * POST /action/swap_next (Phase 4: Direct call, no queue)
 */
esp_err_t h_post_swap_next(httpd_req_t *req) {
    if (app_state_get() == STATE_ERROR) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Bad state\",\"code\":\"BAD_STATE\"}");
        return ESP_OK;
    }

    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    // Direct call to channel_player
    esp_err_t err = channel_player_swap_next();
    if (err == ESP_ERR_INVALID_STATE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Command in progress\",\"code\":\"BUSY\"}");
    } else if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Navigation failed\",\"code\":\"NAV_ERROR\"}");
    } else {
        send_json(req, 200, "{\"ok\":true,\"data\":{\"action\":\"swap_next\"}}");
    }
    
    return ESP_OK;
}

/**
 * POST /action/swap_back (Phase 4: Direct call, no queue)
 */
esp_err_t h_post_swap_back(httpd_req_t *req) {
    if (app_state_get() == STATE_ERROR) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Bad state\",\"code\":\"BAD_STATE\"}");
        return ESP_OK;
    }

    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    // Direct call to channel_player
    esp_err_t err = channel_player_swap_back();
    if (err == ESP_ERR_INVALID_STATE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Command in progress\",\"code\":\"BUSY\"}");
    } else if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Navigation failed\",\"code\":\"NAV_ERROR\"}");
    } else {
        send_json(req, 200, "{\"ok\":true,\"data\":{\"action\":\"swap_back\"}}");
    }
    
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

