// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_rest_status.c
 * @brief Read-only device/system status REST handlers
 *
 * Contains handlers for:
 * - GET /api/ui-config - UI configuration (LCD dimensions, feature flags)
 * - GET /api/init - Combined init payload for page load
 * - GET /api/network-status - Network connection info
 * - GET /status - Device status (uptime, heap, RSSI, firmware)
 * - GET /api/state - Lightweight state snapshot
 * - GET /channels/stats - Channel cache statistics
 * - POST /debug (dev mode only)
 */

#include "http_api_internal.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_netif.h"
#include "app_wifi.h"
#include "config_store.h"
#include "makapix.h"
#include "makapix_store.h"
#include "play_scheduler.h"
#include "playback_service.h"
#include "version.h"
#include "p3a_state.h"
#include "freertos/semphr.h"

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

// ---------- Debug Handler (Dev Mode) ----------

#if CONFIG_OTA_DEV_MODE
#include "playlist_manager.h"

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

    // No debug ops currently defined
    cJSON_Delete(root);
    send_json(req, 400, "{\"ok\":false,\"error\":\"Unknown op\",\"code\":\"UNKNOWN_OP\"}");
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

/**
 * GET /api/init
 * Combined init endpoint for index.html - returns all data needed on page load
 * in a single request: ui_config, channel_stats, active_playset, play_order
 */
esp_err_t h_get_api_init(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *data = cJSON_AddObjectToObject(root, "data");

    // ui_config: LCD dimensions + feature flags
    cJSON *ui = cJSON_AddObjectToObject(data, "ui_config");
    cJSON_AddNumberToObject(ui, "lcd_width", LCD_MAX_WIDTH);
    cJSON_AddNumberToObject(ui, "lcd_height", LCD_MAX_HEIGHT);
#if CONFIG_P3A_PICO8_ENABLE
    cJSON_AddBoolToObject(ui, "pico8_enabled", true);
#else
    cJSON_AddBoolToObject(ui, "pico8_enabled", false);
#endif

    // channel_stats: per-channel cached/total counts (O(1) from LAi)
    size_t all_total = 0, all_cached = 0;
    size_t promoted_total = 0, promoted_cached = 0;
    size_t giphy_trending_total = 0, giphy_trending_cached = 0;
    play_scheduler_get_channel_stats("all", &all_total, &all_cached);
    play_scheduler_get_channel_stats("promoted", &promoted_total, &promoted_cached);
    play_scheduler_get_channel_stats("giphy_trending", &giphy_trending_total, &giphy_trending_cached);
    bool is_registered = makapix_store_has_player_key();

    cJSON *stats = cJSON_AddObjectToObject(data, "channel_stats");
    cJSON *s_all = cJSON_AddObjectToObject(stats, "all");
    cJSON_AddNumberToObject(s_all, "total", (double)all_total);
    cJSON_AddNumberToObject(s_all, "cached", (double)all_cached);
    cJSON *s_prom = cJSON_AddObjectToObject(stats, "promoted");
    cJSON_AddNumberToObject(s_prom, "total", (double)promoted_total);
    cJSON_AddNumberToObject(s_prom, "cached", (double)promoted_cached);
    cJSON *s_giphy = cJSON_AddObjectToObject(stats, "giphy_trending");
    cJSON_AddNumberToObject(s_giphy, "total", (double)giphy_trending_total);
    cJSON_AddNumberToObject(s_giphy, "cached", (double)giphy_trending_cached);
    cJSON_AddBoolToObject(stats, "registered", is_registered);

    // active_playset: current channel/playset name
    const char *playset = p3a_state_get_active_playset();
    cJSON_AddStringToObject(data, "active_playset", playset ? playset : "");

    // play_order: shuffle mode
    uint8_t play_order = config_store_get_play_order();
    cJSON_AddNumberToObject(data, "play_order", (double)play_order);

    // paused: current pause state
    cJSON_AddBoolToObject(data, "paused", playback_service_is_paused());

    // playset_info: active playset details
    ps_stats_t ps_stats;
    if (play_scheduler_get_stats(&ps_stats) == ESP_OK) {
        cJSON *pi = cJSON_AddObjectToObject(data, "playset_info");
        cJSON_AddNumberToObject(pi, "channel_count", (double)ps_stats.channel_count);
        cJSON_AddNumberToObject(pi, "total_cached", (double)ps_stats.total_available);
        cJSON_AddNumberToObject(pi, "total_entries", (double)ps_stats.total_entries);
        cJSON_AddStringToObject(pi, "exposure_mode", exposure_mode_str(ps_stats.exposure_mode));
        cJSON_AddStringToObject(pi, "pick_mode", pick_mode_str(ps_stats.pick_mode));
    }

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

// ---------- Network Status Handler ----------

/**
 * GET /api/network-status
 * Returns network connection information (IP, SSID, RSSI)
 */
esp_err_t h_get_network_status(httpd_req_t *req) {
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_RMT");
    }

    esp_netif_ip_info_t ip_info;
    bool has_ip = false;
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
        has_ip = (ip_info.ip.addr != 0);
    }

    wifi_ap_record_t ap = {0};
    bool has_rssi = (esp_wifi_remote_sta_get_ap_info(&ap) == ESP_OK);

    char saved_ssid[33] = {0};
    bool has_ssid = (app_wifi_get_saved_ssid(saved_ssid, sizeof(saved_ssid)) == ESP_OK);

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(data, "connected", has_ip);

    if (has_ssid && strlen(saved_ssid) > 0) {
        cJSON_AddStringToObject(data, "ssid", saved_ssid);
    }

    if (has_ip) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(data, "ip", ip_str);

        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.gw));
        cJSON_AddStringToObject(data, "gateway", ip_str);

        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.netmask));
        cJSON_AddStringToObject(data, "netmask", ip_str);
    }

    if (has_rssi) {
        cJSON_AddNumberToObject(data, "rssi", ap.rssi);
    }

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

    cJSON_AddStringToObject(data, "state", p3a_state_get_app_status_name(p3a_state_get_app_status()));
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

    // API version for compatibility checking
#ifdef P3A_API_VERSION
    cJSON_AddNumberToObject(data, "api_version", P3A_API_VERSION);
#else
    cJSON_AddNumberToObject(data, "api_version", 1);
#endif

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

    cJSON_AddStringToObject(data, "state", p3a_state_get_app_status_name(p3a_state_get_app_status()));
    cJSON_AddNumberToObject(data, "uptime_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(data, "heap_free", (double)esp_get_free_heap_size());
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
 *
 * Uses LAi (Locally Available index) for O(1) stats lookup.
 * No filesystem scanning - returns cached counts from Play Scheduler.
 */
esp_err_t h_get_channels_stats(httpd_req_t *req) {
    // Get stats from Play Scheduler (O(1) lookup from LAi)
    size_t all_total = 0, all_cached = 0;
    size_t promoted_total = 0, promoted_cached = 0;
    size_t giphy_trending_total = 0, giphy_trending_cached = 0;

    // Use new LAi-based API for instant stats
    play_scheduler_get_channel_stats("all", &all_total, &all_cached);
    play_scheduler_get_channel_stats("promoted", &promoted_total, &promoted_cached);
    play_scheduler_get_channel_stats("giphy_trending", &giphy_trending_total, &giphy_trending_cached);

    // Check if Makapix is registered (has player_key)
    bool is_registered = makapix_store_has_player_key();

    char response[400];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"data\":{"
             "\"all\":{\"total\":%zu,\"cached\":%zu},"
             "\"promoted\":{\"total\":%zu,\"cached\":%zu},"
             "\"giphy_trending\":{\"total\":%zu,\"cached\":%zu},"
             "\"registered\":%s"
             "}}",
             all_total, all_cached, promoted_total, promoted_cached,
             giphy_trending_total, giphy_trending_cached,
             is_registered ? "true" : "false");
    send_json(req, 200, response);
    return ESP_OK;
}
