// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// Wi-Fi station through the ESP32-C6: credentials, joining, staying joined.
// See p3a_net.h for the shape of the whole; the heavier remedies are in
// net_wifi_recovery.c and net_link_recovery.c.

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "frame_trace.h"
#include "mdns.h"
#include "net_backoff.h"
#include "net_internal.h"
#include "nvs.h"
#include "p3a_clock.h"
#include "p3a_event.h"
#include "p3a_http.h"
#include "p3a_recovery.h"
#include "p3a_settings.h"
#include "p3a_task.h"

static const char *TAG = "net";

#define NVS_NAMESPACE    "wifi_config"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASSWORD "password"

net_shared_t g_net = {.health_interval_ms = NET_HEALTH_INTERVAL_NORMAL_MS};

static char s_hostname[24] = "p3a";
static char s_ssid[NET_MAX_SSID_LEN + 1];
static bool s_has_credentials;
static bool s_handlers_registered;
static bool s_mdns_service_added;
static bool s_time_started;

// --- state ----------------------------------------------------------------------

const char *p3a_net_state_name(p3a_net_state_t state)
{
    switch (state) {
    case P3A_NET_SETUP:
        return "setup";
    case P3A_NET_CONNECTING:
        return "connecting";
    case P3A_NET_UP:
        return "up";
    case P3A_NET_DEGRADED:
        return "degraded";
    default:
        return "off";
    }
}

// Moves to a state and announces the two transitions others care about.
// DEGRADED is final: nothing leaves it.
void net_set_state(p3a_net_state_t state)
{
    int old = atomic_load(&g_net.state);
    do {
        if (old == (int)state || old == P3A_NET_DEGRADED) {
            return;
        }
    } while (!atomic_compare_exchange_weak(&g_net.state, &old, (int)state));

    ESP_LOGI(TAG, "%s -> %s", p3a_net_state_name((p3a_net_state_t)old), p3a_net_state_name(state));
    if (state == P3A_NET_UP) {
        p3a_event_emit(P3A_EVENT_NET_UP);
    } else if (old == P3A_NET_UP) {
        p3a_event_emit(P3A_EVENT_NET_DOWN);
    }
}

const char *net_hostname(void)
{
    return s_hostname;
}

bool p3a_net_is_up(void)
{
    return atomic_load(&g_net.state) == P3A_NET_UP;
}

bool net_sta_has_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey(NET_STA_NETIF_KEY);
    esp_netif_ip_info_t info;
    return sta != NULL && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0;
}

void p3a_net_status(p3a_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = (p3a_net_state_t)atomic_load(&g_net.state);
    out->has_credentials = s_has_credentials;
    out->time_set = p3a_clock_is_set();
    out->drops = atomic_load(&g_net.drops);
    out->reinits = atomic_load(&g_net.reinits);
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    strlcpy(out->hostname, s_hostname, sizeof(out->hostname));
    if (out->state == P3A_NET_UP) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey(NET_STA_NETIF_KEY);
        esp_netif_ip_info_t info;
        if (sta != NULL && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
            snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&info.ip));
        }
        // Asks the co-processor; skipped unless there is a connection to ask about.
        int rssi = 0;
        if (esp_wifi_remote_sta_get_rssi(&rssi) == ESP_OK) {
            out->rssi = rssi;
        }
    }
}

// --- credentials ----------------------------------------------------------------

esp_err_t net_load_credentials(char *ssid, char *password)
{
    ssid[0] = '\0';
    password[0] = '\0';
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = NET_MAX_SSID_LEN + 1;
    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = NET_MAX_PASSWORD_LEN + 1;
        err = nvs_get_str(nvs, NVS_KEY_PASSWORD, password, &len);
    }
    nvs_close(nvs);
    if (err != ESP_OK || ssid[0] == '\0') {
        ssid[0] = '\0';
        password[0] = '\0';
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t p3a_net_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || ssid[0] == '\0' || strlen(ssid) > NET_MAX_SSID_LEN ||
        strlen(password) > NET_MAX_PASSWORD_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    frame_trace_mark(FT_MARK_NVS_COMMIT, FT_PHASE_BEGIN, 7);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, NVS_KEY_PASSWORD, password);
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    frame_trace_mark(FT_MARK_NVS_COMMIT, FT_PHASE_END, 7);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not store the Wi-Fi credentials: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi credentials stored; they apply at the next restart");
    }
    return err;
}

esp_err_t p3a_net_erase_credentials(void)
{
    frame_trace_mark(FT_MARK_NVS_COMMIT, FT_PHASE_BEGIN, 8);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        esp_err_t a = nvs_erase_key(nvs, NVS_KEY_SSID);
        esp_err_t b = nvs_erase_key(nvs, NVS_KEY_PASSWORD);
        err = (a != ESP_OK && a != ESP_ERR_NVS_NOT_FOUND) ? a : (b != ESP_OK && b != ESP_ERR_NVS_NOT_FOUND) ? b : ESP_OK;
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    frame_trace_mark(FT_MARK_NVS_COMMIT, FT_PHASE_END, 8);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not erase the Wi-Fi credentials: %s", esp_err_to_name(err));
    }
    return err;
}

// --- the station ----------------------------------------------------------------

// WPA2 as the floor, WPA3 accepted with either way of deriving its key: what
// 1.x shipped with.
void net_fill_sta_config(wifi_config_t *config, const char *ssid, const char *password)
{
    memset(config, 0, sizeof(*config));
    config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    size_t ssid_len = strnlen(ssid, sizeof(config->sta.ssid));
    size_t password_len = strnlen(password, sizeof(config->sta.password));
    memcpy(config->sta.ssid, ssid, ssid_len);
    memcpy(config->sta.password, password, password_len);
}

// Best effort, after the driver has started: no power save (the device is
// mains powered and reliability matters more), and Wi-Fi 6 where offered.
void net_tune_sta(void)
{
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_remote_set_protocol(WIFI_IF_STA,
                                 WIFI_PROTOCOL_11AX | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11B);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (g_net.reinit_in_progress) {
        return;
    }
    esp_err_t err = esp_wifi_remote_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
    }
}

// What the radio says when it could not find the network at all.
static bool reason_is_absent(int reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_BEACON_TIMEOUT;
}

static void on_disconnected(int reason)
{
    // Stopping the station to become the setup access point also ends here.
    if (atomic_load(&g_net.state) == P3A_NET_SETUP) {
        return;
    }
    atomic_fetch_add(&g_net.drops, 1);
    ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d, ever connected: %d, attempt %d)", reason, g_net.ever_connected,
             g_net.attempt);
    if (g_net.events != NULL) {
        xEventGroupClearBits(g_net.events, NET_CONNECTED_BIT);
    }

    // During a re-initialisation the recovery worker owns reconnection.
    if (g_net.reinit_in_progress) {
        return;
    }
    net_set_state(P3A_NET_CONNECTING);

    // Stuck cycling: tear the driver down and start over. Not before the
    // first connection: there the question is whether to offer setup.
    g_net.consecutive_drops++;
    if (g_net.ever_connected && g_net.consecutive_drops >= NET_MAX_CONSECUTIVE_DROPS) {
        ESP_LOGE(TAG, "%d drops in a row: re-initialising Wi-Fi", g_net.consecutive_drops);
        g_net.consecutive_drops = 0;
        net_schedule_reinit();
        return;
    }

    // Before the first connection of this boot, decide whether the stored
    // network is worth more tries (net_backoff.h). After a first success,
    // keep trying for as long as it takes.
    bool absent = reason_is_absent(reason);
    if (!g_net.ever_connected) {
        if (!absent) {
            g_net.refusals++;
        }
        uint32_t since_ms = (uint32_t)((esp_timer_get_time() - g_net.join_started_us) / 1000);
        if (net_first_join_gives_up(g_net.refusals, since_ms, absent)) {
            ESP_LOGW(TAG, "could not join the stored network (%s): offering setup",
                     absent ? "it is not in range" : "it refused the connection");
            net_enter_setup();
            return;
        }
    }
    g_net.attempt++;
    // A network that is not there is retried at the reconnection pace.
    uint32_t delay_ms = net_backoff_delay_ms(g_net.attempt, g_net.ever_connected || absent);
    if (g_net.reconnect_timer != NULL) {
        esp_timer_stop(g_net.reconnect_timer);
        // Never connect from inside the event handler: go through the timer
        // even for "now".
        esp_timer_start_once(g_net.reconnect_timer, delay_ms > 0 ? (uint64_t)delay_ms * 1000 : 1000);
    }
}

static void on_got_ip(const ip_event_got_ip_t *event)
{
    ESP_LOGI(TAG, "address " IPSTR, IP2STR(&event->ip_info.ip));
    g_net.attempt = 0;
    g_net.consecutive_drops = 0;
    g_net.no_ip_cycles = 0;
    g_net.health_interval_ms = NET_HEALTH_INTERVAL_NORMAL_MS;
    g_net.ever_connected = true;
    if (g_net.reconnect_timer != NULL) {
        esp_timer_stop(g_net.reconnect_timer);
    }
    // A working connection ends any run of recovery restarts.
    p3a_recovery_report(P3A_RECOVERY_WIFI, true, NET_WIFI_RECOVERY_REBOOTS);
    p3a_recovery_report(P3A_RECOVERY_LINK, true, NET_LINK_RECOVERY_REBOOTS);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey(NET_STA_NETIF_KEY);
    if (sta != NULL) {
        mdns_netif_action(sta, (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
    }
    if (!s_time_started) {
        s_time_started = true;
        net_time_start();
    }
    xEventGroupSetBits(g_net.events, NET_CONNECTED_BIT);
    net_set_state(P3A_NET_UP);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_REMOTE_EVENT && id == WIFI_EVENT_STA_START) {
        if (!g_net.reinit_in_progress && atomic_load(&g_net.state) != P3A_NET_SETUP) {
            esp_err_t err = esp_wifi_remote_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
            }
        }
    } else if (base == WIFI_REMOTE_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        on_disconnected(event != NULL ? event->reason : 0);
    } else if (base == WIFI_REMOTE_EVENT && (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED)) {
        // Someone joined or left the setup network: the setup screen says so.
        static int clients;
        clients += id == WIFI_EVENT_AP_STACONNECTED ? 1 : -1;
        if (clients < 0) {
            clients = 0;
        }
        net_setup_clients_changed(clients);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip((const ip_event_got_ip_t *)data);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "address lost: reconnecting");
        if (!g_net.reinit_in_progress) {
            esp_wifi_remote_disconnect();  // arrives back here as a disconnection
        }
    }
}

static esp_err_t register_handlers_once(void)
{
    if (s_handlers_registered) {
        return ESP_OK;
    }
    esp_err_t err = esp_event_handler_instance_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL);
    }
    s_handlers_registered = err == ESP_OK;
    return err;
}

// Makes <hostname>.local resolve on the LAN and advertises the web interface.
static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS did not start: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set(s_hostname);
    if (!s_mdns_service_added) {
        err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_mdns_service_added = err == ESP_OK || err == ESP_ERR_INVALID_ARG;  // INVALID_ARG: already there
        if (!s_mdns_service_added) {
            ESP_LOGE(TAG, "mDNS service not added: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t start_station(const char *ssid, const char *password)
{
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (sta == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Also offered to DHCP, which some routers publish in their own DNS.
    esp_err_t err = esp_netif_set_hostname(sta, s_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hostname not set: %s", esp_err_to_name(err));
    }
    start_mdns();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t config;
    net_fill_sta_config(&config, ssid, password);
    if ((err = esp_wifi_remote_init(&init)) != ESP_OK) {
        return err;
    }
    g_net.driver_started = true;
    g_net.join_started_us = esp_timer_get_time();
    if ((err = register_handlers_once()) != ESP_OK ||
        (err = esp_wifi_remote_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_remote_set_config(WIFI_IF_STA, &config)) != ESP_OK ||
        (err = esp_wifi_remote_start()) != ESP_OK) {
        return err;
    }
    net_tune_sta();
    return ESP_OK;
}

esp_err_t p3a_net_start(void)
{
    if (g_net.events != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    static p3a_settings_t settings;  // too large for a task stack
    p3a_settings_get(&settings);
    if (settings.device_name[0] != '\0') {
        snprintf(s_hostname, sizeof(s_hostname), "p3a-%s", settings.device_name);
    }

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;  // someone created it already
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    // Before anything can bring the link to the co-processor up, so that a
    // link failure never goes unhandled.
    net_link_recovery_register();

    g_net.events = xEventGroupCreate();
    const esp_timer_create_args_t timer = {.callback = reconnect_timer_cb, .name = "net_reconnect"};
    if (g_net.events == NULL || esp_timer_create(&timer, &g_net.reconnect_timer) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    p3a_task_config_t recovery = P3A_TASK_CONFIG("net_recovery", net_recovery_task, NULL, 4096, P3A_TASK_APP);
    p3a_task_config_t health = P3A_TASK_CONFIG("net_health", net_health_task, NULL, 4096, P3A_TASK_NETWORK);
    if ((err = p3a_task_create(&recovery, &g_net.recovery_task)) != ESP_OK ||
        (err = p3a_task_create(&health, &g_net.health_task)) != ESP_OK) {
        return err;
    }

    // The server is up whatever happens to Wi-Fi: it also serves setup.
    err = p3a_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no HTTP server: %s", esp_err_to_name(err));
    }

    char password[NET_MAX_PASSWORD_LEN + 1];
    s_has_credentials = net_load_credentials(s_ssid, password) == ESP_OK;
    if (!s_has_credentials) {
        ESP_LOGW(TAG, "no Wi-Fi network stored: the device needs setting up");
        net_enter_setup();
        return ESP_OK;
    }

    net_set_state(P3A_NET_CONNECTING);
    err = start_station(s_ssid, password);
    memset(password, 0, sizeof(password));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the Wi-Fi station did not start: %s", esp_err_to_name(err));
    }
    return err;
}
