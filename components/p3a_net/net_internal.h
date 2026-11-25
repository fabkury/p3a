// SPDX-License-Identifier: Apache-2.0
// Shared between the files of p3a_net. Not a public header.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "p3a_net.h"

#define NET_CONNECTED_BIT BIT0

// The IDF's interface key for the Wi-Fi station (esp_netif_create_default_wifi_sta).
#define NET_STA_NETIF_KEY "WIFI_STA_DEF"

#define NET_MAX_SSID_LEN     32
#define NET_MAX_PASSWORD_LEN 64  // a passphrase is at most 63; 64 is a key in hex

// Drops in a row (no address in between) before the driver is re-initialised.
#define NET_MAX_CONSECUTIVE_DROPS 10

// The health monitor's period: slow while all is well, fast while recovering.
#define NET_HEALTH_INTERVAL_NORMAL_MS 120000
#define NET_HEALTH_INTERVAL_FAST_MS   30000

// Restarts in a row allowed for each remedy (see p3a_recovery.h).
#define NET_WIFI_RECOVERY_REBOOTS 1
#define NET_LINK_RECOVERY_REBOOTS 3

// Seconds between deciding to restart and restarting, so that transfers in
// flight reach their error paths and close their files.
#define NET_RESTART_COUNTDOWN_S 5

// What the event handlers, the recovery worker and the health monitor share.
// Written from the system event task and two of our own; the fields are
// simple flags and counters, as in 1.x.
typedef struct {
    EventGroupHandle_t events;
    esp_timer_handle_t reconnect_timer;
    TaskHandle_t recovery_task;
    TaskHandle_t health_task;
    atomic_int state;             // p3a_net_state_t
    int attempt;                  // reconnect tries since the last address
    int consecutive_drops;
    bool ever_connected;          // an address was obtained at least once this boot
    bool driver_started;          // esp_wifi_remote_init() has succeeded
    int refusals;                 // first join: failures where the network was there
    int64_t join_started_us;      // first join: when it began
    bool reinit_in_progress;      // the recovery worker owns reconnection
    TickType_t reinit_started;
    int no_ip_cycles;             // health checks in a row without an address
    uint32_t health_interval_ms;
    atomic_uint drops;
    atomic_uint reinits;
} net_shared_t;

extern net_shared_t g_net;

// net_wifi.c
esp_err_t net_load_credentials(char *ssid, char *password);  // buffers of NET_MAX_*_LEN + 1
void net_fill_sta_config(wifi_config_t *config, const char *ssid, const char *password);
void net_tune_sta(void);
bool net_sta_has_ip(void);
void net_set_state(p3a_net_state_t state);

// net_wifi_recovery.c
void net_recovery_task(void *arg);
void net_health_task(void *arg);
void net_schedule_reinit(void);

// net_link_recovery.c
void net_link_recovery_register(void);
bool net_link_is_dead(void);
// Logs a countdown and restarts. Never returns.
void net_restart_after_countdown(const char *why);

// net_time.c
void net_time_start(void);

// net_setup.c: the setup access point.
// Moves to the setup state and brings the access point up (as a job).
void net_enter_setup(void);
bool net_setup_active(void);

// net_wifi.c: the name the device takes on the LAN, "p3a" or "p3a-<name>".
const char *net_hostname(void);

// net_api.c: adds the built-in /api/v2 routes.
void net_api_register(void);

// net_player_api.c: adds the player, playset and upload routes.
void net_player_api_register(void);
void net_sources_api_register(void);
void net_ui_api_register(void);

// net_trace_api.c: adds the frame trace's routes. Only in builds with CONFIG_P3A_FRAME_TRACE.
void net_trace_api_register(void);
