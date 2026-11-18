// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// The heavier remedies for Wi-Fi that will not stay up: tearing the driver
// down and bringing it back (the recovery worker), and noticing that it is
// needed (the health monitor).

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "lwip/netdb.h"
#include "net_internal.h"
#include "p3a_recovery.h"

static const char *TAG = "net_recovery";

#define REINIT_ATTEMPTS        3
#define REINIT_WAIT_FOR_IP_MS  15000
#define REINIT_STUCK_MS        120000
#define NO_IP_CYCLES_BEFORE_REINIT 3
#define REACHABILITY_HOST      "google.com"
#define REACHABILITY_TRIES     3

// One full re-initialisation of the Wi-Fi driver. Sets *driver_broken when the
// co-processor refused to initialise at all, which no retry from this side
// will fix.
static bool reinit_once(int attempt, bool *driver_broken)
{
    esp_err_t err = esp_wifi_remote_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "stop failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_remote_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "deinit failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_remote_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        *driver_broken = true;
        return false;
    }

    // Read again each time: the user may have changed the network meanwhile.
    char ssid[NET_MAX_SSID_LEN + 1];
    char password[NET_MAX_PASSWORD_LEN + 1];
    if (net_load_credentials(ssid, password) != ESP_OK) {
        ESP_LOGE(TAG, "no stored network; cannot restart the station");
        return false;
    }
    wifi_config_t config;
    net_fill_sta_config(&config, ssid, password);
    memset(password, 0, sizeof(password));

    xEventGroupClearBits(g_net.events, NET_CONNECTED_BIT);
    err = esp_wifi_remote_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode failed: %s", esp_err_to_name(err));
        *driver_broken = true;
        return false;
    }
    if ((err = esp_wifi_remote_set_config(WIFI_IF_STA, &config)) != ESP_OK ||
        (err = esp_wifi_remote_start()) != ESP_OK) {
        ESP_LOGE(TAG, "restart failed: %s", esp_err_to_name(err));
        return false;
    }
    net_tune_sta();

    err = esp_wifi_remote_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s (attempt %d/%d)", esp_err_to_name(err), attempt, REINIT_ATTEMPTS);
        return false;
    }
    // Association and DHCP take a while.
    EventBits_t bits =
        xEventGroupWaitBits(g_net.events, NET_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(REINIT_WAIT_FOR_IP_MS));
    if (!(bits & NET_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "no address after %d s (attempt %d/%d)", REINIT_WAIT_FOR_IP_MS / 1000, attempt, REINIT_ATTEMPTS);
        return false;
    }
    return true;
}

void net_recovery_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        atomic_fetch_add(&g_net.reinits, 1);

        // Fresh counters, so that drops after the recovery get a fresh backoff
        // and do not immediately ask for another one.
        g_net.attempt = 0;
        g_net.consecutive_drops = 0;

        bool recovered = false;
        bool driver_broken = false;
        for (int attempt = 1; attempt <= REINIT_ATTEMPTS && !recovered; attempt++) {
            if (attempt > 1) {
                int backoff_ms = 5000 * (1 << (attempt - 2));  // 5 s, 10 s
                ESP_LOGW(TAG, "retrying in %d ms", backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            }
            ESP_LOGW(TAG, "re-initialising Wi-Fi (attempt %d/%d)", attempt, REINIT_ATTEMPTS);
            recovered = reinit_once(attempt, &driver_broken);
        }

        if (recovered) {
            ESP_LOGW(TAG, "Wi-Fi is back");
        } else if (driver_broken) {
            // The co-processor's Wi-Fi stack is not answering. Only a restart
            // of the whole device resets it; allow that a bounded number of
            // times, then live without a network.
            ESP_LOGE(TAG, "the co-processor's Wi-Fi stack is not responding");
            if (p3a_recovery_report(P3A_RECOVERY_WIFI, false, NET_WIFI_RECOVERY_REBOOTS) == P3A_RECOVERY_REBOOT) {
                net_restart_after_countdown("Wi-Fi chip not responding");
            }
        } else {
            ESP_LOGE(TAG, "Wi-Fi did not come back; the health monitor will try again");
        }

        g_net.reinit_in_progress = false;
        // Let the health monitor sleep a full period before judging the result.
        g_net.health_interval_ms =
            (recovered && net_sta_has_ip()) ? NET_HEALTH_INTERVAL_NORMAL_MS : NET_HEALTH_INTERVAL_FAST_MS;
    }
}

void net_schedule_reinit(void)
{
    if (g_net.reinit_in_progress || g_net.recovery_task == NULL) {
        return;
    }
    g_net.reinit_in_progress = true;
    g_net.reinit_started = xTaskGetTickCount();
    g_net.no_ip_cycles = 0;
    if (g_net.reconnect_timer != NULL) {
        esp_timer_stop(g_net.reconnect_timer);  // the worker owns reconnection now
    }
    xTaskNotifyGive(g_net.recovery_task);
}

// True when a name on the internet resolves. Tried a few times, so that one
// lost DNS packet does not cost a reconnection.
static bool internet_reachable(void)
{
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    for (int attempt = 1; attempt <= REACHABILITY_TRIES; attempt++) {
        struct addrinfo *result = NULL;
        int err = getaddrinfo(REACHABILITY_HOST, "80", &hints, &result);
        bool ok = err == 0 && result != NULL;
        if (result != NULL) {
            freeaddrinfo(result);
        }
        if (ok) {
            return true;
        }
        ESP_LOGW(TAG, "reachability check %d/%d failed (%d)", attempt, REACHABILITY_TRIES, err);
        if (attempt < REACHABILITY_TRIES) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    return false;
}

void net_health_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_net.health_interval_ms));

        // Nothing to watch before the first connection of this boot, and
        // nothing to do once the link to the co-processor is dead: recovery
        // would only throw requests at it.
        if (!g_net.ever_connected || net_link_is_dead()) {
            continue;
        }
        if (g_net.reinit_in_progress) {
            if (xTaskGetTickCount() - g_net.reinit_started > pdMS_TO_TICKS(REINIT_STUCK_MS)) {
                ESP_LOGE(TAG, "a re-initialisation has been running for over %d s; clearing it",
                         REINIT_STUCK_MS / 1000);
                g_net.reinit_in_progress = false;
            } else {
                continue;
            }
        }

        if (!net_sta_has_ip()) {
            g_net.no_ip_cycles++;
            ESP_LOGW(TAG, "no address for %d check(s)", g_net.no_ip_cycles);
            if (g_net.no_ip_cycles >= NO_IP_CYCLES_BEFORE_REINIT) {
                net_schedule_reinit();
            }
            continue;
        }
        g_net.no_ip_cycles = 0;

        if (!internet_reachable()) {
            ESP_LOGW(TAG, "connected but nothing resolves: reconnecting");
            esp_wifi_remote_disconnect();  // the disconnect handler takes it from here
        }
    }
}
