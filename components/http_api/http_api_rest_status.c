// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

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
#include "pin_lists.h"
#include "storage_eviction.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_netif.h"
#include "app_wifi.h"
#include "config_store.h"
#include "art_institution.h"
#include "makapix.h"
#include "makapix_store.h"
#include "p3a_current_post.h"
#include "play_scheduler.h"
#include "playset_json.h"
#include "playback_service.h"
#include "version.h"
#include "p3a_state.h"
#include "freertos/semphr.h"

// ---------- Playset Mode String Helpers ----------

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
    // Compute channel_id hashes to look up stats
    // Stats lookup uses the canonical offset=0 channel_id. Per-playset
    // channels with non-zero offsets are addressable via channel-specific
    // endpoints, not this aggregate.
    char all_id[17], promoted_id[17], giphy_id[17];
    ps_compute_channel_id(PS_CHANNEL_TYPE_NAMED, "all", "", 0, all_id, sizeof(all_id));
    ps_compute_channel_id(PS_CHANNEL_TYPE_NAMED, "promoted", "", 0, promoted_id, sizeof(promoted_id));
    ps_compute_channel_id(PS_CHANNEL_TYPE_GIPHY, "trending", "", 0, giphy_id, sizeof(giphy_id));
    size_t all_total = 0, all_cached = 0;
    size_t promoted_total = 0, promoted_cached = 0;
    size_t giphy_trending_total = 0, giphy_trending_cached = 0;
    play_scheduler_get_channel_stats(all_id, &all_total, &all_cached);
    play_scheduler_get_channel_stats(promoted_id, &promoted_total, &promoted_cached);
    play_scheduler_get_channel_stats(giphy_id, &giphy_trending_total, &giphy_trending_cached);
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

    // active_playset: structured object describing what's currently playing.
    // Carries the full ps_playset_t shape (channels with type/name/identifier/
    // display_name/weight/offset and the artwork sub-struct for ARTWORK
    // channels), so the WebUI can derive pill highlighting and now-playing
    // metadata without auxiliary fields. Absent when nothing is playing.
    {
        ps_playset_t *active = calloc(1, sizeof(ps_playset_t));
        if (active && play_scheduler_get_active_playset(active) == ESP_OK) {
            cJSON *ap = playset_json_serialize(active);
            if (ap) cJSON_AddItemToObject(data, "active_playset", ap);
        }
        free(active);
    }

    // paused: current pause state
    cJSON_AddBoolToObject(data, "paused", playback_service_is_paused());

    // ai_refresh_sec: per-museum channel refresh interval. The index page
    // uses this to compute isDueForRefresh() for institution channels
    // without an extra /config round trip on every page load.
    cJSON_AddNumberToObject(data, "ai_refresh_sec",
                            (double)config_store_get_ai_refresh_sec());

    // museum_rate_limits: same shape /playsets/active emits. Seeded here so
    // the institution-channel "rate limited" badge can light up immediately
    // on page load instead of waiting for the first 4s playset poll.
    cJSON *mrl = cJSON_AddObjectToObject(data, "museum_rate_limits");
    if (mrl) {
        for (size_t i = 0; i < ART_INSTITUTION_MUSEUM_COUNT; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddNumberToObject(o, "remaining_sec",
                (double)art_institution_rate_limit_remaining(
                    ART_INSTITUTION_MUSEUMS[i].id));
            cJSON_AddItemToObject(mrl, ART_INSTITUTION_MUSEUMS[i].id, o);
        }
    }

    // playset_info: active playset details
    ps_stats_t ps_stats;
    if (play_scheduler_get_stats(&ps_stats) == ESP_OK) {
        cJSON *pi = cJSON_AddObjectToObject(data, "playset_info");
        cJSON_AddNumberToObject(pi, "channel_count", (double)ps_stats.channel_count);
        cJSON_AddNumberToObject(pi, "total_cached", (double)ps_stats.total_available);
        cJSON_AddNumberToObject(pi, "total_entries", (double)ps_stats.total_entries);
        cJSON_AddStringToObject(pi, "pick_mode", pick_mode_str(ps_stats.pick_mode));
    }

    cJSON *ca = build_current_artwork_json();
    if (ca) {
        cJSON_AddItemToObject(data, "current_artwork", ca);
    }

    // pin_lists: enumerate all pin lists + active slug so the web UI can
    // render a pill per list without an extra request.
    // The infos buffer is heap-allocated (PSRAM) — at 64 lists * sizeof(pin_list_info_t)
    // it's ~5.6 KB, which overflows the ~8 KB http worker task stack when combined
    // with the rest of this handler's cJSON allocations.
    {
        pin_list_info_t *infos = heap_caps_malloc(
            PIN_LISTS_MAX_LISTS * sizeof(pin_list_info_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (infos) {
            size_t n = 0;
            if (pin_lists_enumerate(infos, PIN_LISTS_MAX_LISTS, &n) == ESP_OK) {
                cJSON *pl = cJSON_AddObjectToObject(data, "pin_lists");
                cJSON *arr = cJSON_AddArrayToObject(pl, "lists");
                for (size_t i = 0; i < n; i++) {
                    cJSON *o = cJSON_CreateObject();
                    if (!o) continue;
                    cJSON_AddStringToObject(o, "slug", infos[i].slug);
                    cJSON_AddStringToObject(o, "name", infos[i].name);
                    cJSON_AddNumberToObject(o, "count", (double)infos[i].count);
                    cJSON_AddBoolToObject(o, "is_active", infos[i].is_active);
                    cJSON_AddItemToArray(arr, o);
                }
                char active[PIN_LIST_SLUG_LEN] = {0};
                pin_lists_get_active(active);
                cJSON_AddStringToObject(pl, "active", active);
            }
            free(infos);
        }
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

    uint16_t wifi_reboots = config_store_get_wifi_reboot_total();
    if (wifi_reboots > 0) {
        cJSON_AddNumberToObject(data, "wifi_recovery_reboots", (double)wifi_reboots);
    }

    uint16_t touch_reboots = config_store_get_touch_reboot_total();
    if (touch_reboots > 0) {
        cJSON_AddNumberToObject(data, "touch_recovery_reboots", (double)touch_reboots);
    }

    uint16_t transport_reboots = config_store_get_transport_reboot_total();
    if (transport_reboots > 0) {
        cJSON_AddNumberToObject(data, "transport_recovery_reboots", (double)transport_reboots);
    }

    // Device identity
    {
        char device_name[CONFIG_STORE_MAX_DEVICE_NAME_LEN + 1];
        char hostname[24];
        config_store_get_device_name(device_name, sizeof(device_name));
        config_store_get_hostname(hostname, sizeof(hostname));
        cJSON_AddStringToObject(data, "device_name", device_name);
        cJSON_AddStringToObject(data, "hostname", hostname);
    }

    // Network info
    {
        cJSON *net = cJSON_CreateObject();
        if (net) {
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (!sta) sta = esp_netif_get_handle_from_ifkey("WIFI_STA_RMT");
            esp_netif_ip_info_t ip_info;
            bool has_ip = (sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0);
            cJSON_AddBoolToObject(net, "connected", has_ip);
            char saved_ssid[33] = {0};
            if (app_wifi_get_saved_ssid(saved_ssid, sizeof(saved_ssid)) == ESP_OK && strlen(saved_ssid) > 0)
                cJSON_AddStringToObject(net, "ssid", saved_ssid);
            if (has_ip) {
                char s[16];
                snprintf(s, sizeof(s), IPSTR, IP2STR(&ip_info.ip));
                cJSON_AddStringToObject(net, "ip", s);
                snprintf(s, sizeof(s), IPSTR, IP2STR(&ip_info.gw));
                cJSON_AddStringToObject(net, "gateway", s);
                snprintf(s, sizeof(s), IPSTR, IP2STR(&ip_info.netmask));
                cJSON_AddStringToObject(net, "netmask", s);
            }
            if (rssi_ok) cJSON_AddNumberToObject(net, "rssi", ap.rssi);
            cJSON_AddItemToObject(data, "net", net);
        }
    }

    // Makapix state
    {
        cJSON *mkx = cJSON_CreateObject();
        if (mkx) {
            cJSON_AddStringToObject(mkx, "p3a_state", p3a_state_get_name(p3a_state_get()));
            bool registered = makapix_store_has_player_key();
            cJSON_AddBoolToObject(mkx, "registered", registered);
            if (registered) {
                char pk[40];
                if (makapix_store_get_player_key(pk, sizeof(pk)) == ESP_OK)
                    cJSON_AddStringToObject(mkx, "player_key", pk);
            }
            makapix_state_t ms = makapix_get_state();
            const char *mqtt = "disconnected";
            if (ms == MAKAPIX_STATE_CONNECTED) mqtt = "connected";
            else if (ms == MAKAPIX_STATE_CONNECTING) mqtt = "connecting";
            else if (ms == MAKAPIX_STATE_REGISTRATION_INVALID) mqtt = "invalid";
            cJSON_AddStringToObject(mkx, "mqtt_status", mqtt);
            // Connectivity ladder (no_wifi < no_internet < no_registration <
            // no_mqtt < online) so the web UI can mirror the info screen's
            // nuance (e.g. "no internet" vs "server unreachable").
            static const char *conn_names[] = {"no_wifi", "no_internet", "no_registration", "no_mqtt", "online"};
            p3a_connectivity_level_t lvl = p3a_state_get_connectivity();
            if ((unsigned)lvl <= P3A_CONNECTIVITY_ONLINE) {
                cJSON_AddStringToObject(mkx, "connectivity", conn_names[lvl]);
            }
            cJSON_AddItemToObject(data, "makapix", mkx);
        }
    }

    // Storage info
    {
        uint64_t total = 0, avail = 0;
        if (storage_eviction_get_storage_info(&total, &avail) == ESP_OK) {
            cJSON *st = cJSON_CreateObject();
            if (st) {
                cJSON_AddNumberToObject(st, "total_bytes", (double)total);
                cJSON_AddNumberToObject(st, "free_bytes", (double)avail);
                cJSON_AddItemToObject(data, "storage", st);
            }
        }
    }

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
    cJSON_AddStringToObject(data, "p3a_state", p3a_state_get_name(p3a_state_get()));
    cJSON_AddNumberToObject(data, "uptime_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(data, "heap_free", (double)esp_get_free_heap_size());
    if (rssi_ok) {
        cJSON_AddNumberToObject(data, "rssi", ap.rssi);
    } else {
        cJSON_AddNullToObject(data, "rssi");
    }

    // Current Makapix post_id if available; NULL for non-Makapix or unknown.
    int32_t post_id = p3a_current_post_get_id();
    if (p3a_current_post_get_source() == POST_SOURCE_MAKAPIX && post_id != 0) {
        cJSON_AddNumberToObject(data, "current_post_id", (double)post_id);
    } else {
        cJSON_AddNullToObject(data, "current_post_id");
    }

    // Makapix Club status
    bool registered = makapix_store_has_player_key();
    cJSON_AddBoolToObject(data, "makapix_registered", registered);

    if (registered) {
        char player_key[40];
        if (makapix_store_get_player_key(player_key, sizeof(player_key)) == ESP_OK) {
            cJSON_AddStringToObject(data, "makapix_player_key", player_key);
        }
    }

    makapix_state_t mstate = makapix_get_state();
    const char *mqtt_status = "disconnected";
    if (mstate == MAKAPIX_STATE_CONNECTED) mqtt_status = "connected";
    else if (mstate == MAKAPIX_STATE_CONNECTING) mqtt_status = "connecting";
    else if (mstate == MAKAPIX_STATE_REGISTRATION_INVALID) mqtt_status = "invalid";
    cJSON_AddStringToObject(data, "makapix_mqtt_status", mqtt_status);

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
    // Compute channel_id hashes to look up stats
    char all_id[17], promoted_id[17], giphy_id[17];
    ps_compute_channel_id(PS_CHANNEL_TYPE_NAMED, "all", "", 0, all_id, sizeof(all_id));
    ps_compute_channel_id(PS_CHANNEL_TYPE_NAMED, "promoted", "", 0, promoted_id, sizeof(promoted_id));
    ps_compute_channel_id(PS_CHANNEL_TYPE_GIPHY, "trending", "", 0, giphy_id, sizeof(giphy_id));
    size_t all_total = 0, all_cached = 0;
    size_t promoted_total = 0, promoted_cached = 0;
    size_t giphy_trending_total = 0, giphy_trending_cached = 0;

    play_scheduler_get_channel_stats(all_id, &all_total, &all_cached);
    play_scheduler_get_channel_stats(promoted_id, &promoted_total, &promoted_cached);
    play_scheduler_get_channel_stats(giphy_id, &giphy_trending_total, &giphy_trending_cached);

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

// ---------- Device Name Handlers ----------

/**
 * GET /api/device-name
 * Returns device name and effective hostname
 */
esp_err_t h_get_device_name(httpd_req_t *req)
{
    char device_name[CONFIG_STORE_MAX_DEVICE_NAME_LEN + 1];
    char hostname[24];
    config_store_get_device_name(device_name, sizeof(device_name));
    config_store_get_hostname(hostname, sizeof(hostname));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddStringToObject(root, "hostname", hostname);

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
 * POST /api/device-name
 * Sets device name. Accepts {"device_name": "bedroom"} or {"device_name": ""} to clear.
 * Takes effect after reboot.
 */
esp_err_t h_post_device_name(httpd_req_t *req)
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

    cJSON *name_item = cJSON_GetObjectItem(root, "device_name");
    if (!name_item || !cJSON_IsString(name_item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing 'device_name' string\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    const char *name = cJSON_GetStringValue(name_item);
    esp_err_t err = config_store_set_device_name(name);
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid device name. Use [a-z0-9-], max 16 chars, no leading/trailing hyphen.\",\"code\":\"INVALID_NAME\"}");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"SAVE_FAILED\",\"code\":\"SAVE_FAILED\"}");
        return ESP_OK;
    }

    // Return the new effective hostname
    char hostname[24];
    config_store_get_hostname(hostname, sizeof(hostname));

    char response[128];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"device_name\":\"%s\",\"hostname\":\"%s\",\"reboot_required\":true}",
             name, hostname);
    send_json(req, 200, response);
    return ESP_OK;
}

/**
 * GET /api/storage
 * Returns SD card storage information (total bytes, free bytes)
 */
esp_err_t h_get_storage_info(httpd_req_t *req)
{
    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t err = storage_eviction_get_storage_info(&total_bytes, &free_bytes);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"STORAGE_QUERY\",\"code\":\"STORAGE_QUERY\"}");
        return ESP_OK;
    }

    char response[128];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"data\":{\"total_bytes\":%llu,\"free_bytes\":%llu}}",
             (unsigned long long)total_bytes,
             (unsigned long long)free_bytes);
    send_json(req, 200, response);
    return ESP_OK;
}
