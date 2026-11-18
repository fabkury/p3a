// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// Sets the system clock over SNTP and tells p3a_clock when it holds real time.

#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "net_internal.h"
#include "p3a_clock.h"

static const char *TAG = "net_time";

static void on_time_set(struct timeval *tv)
{
    bool first = !p3a_clock_is_set();
    p3a_clock_mark_set();
    char now[P3A_CLOCK_ISO8601_LEN];
    if (p3a_clock_format_iso8601(tv->tv_sec, now, sizeof(now)) == ESP_OK) {
        ESP_LOGI(TAG, "clock %s: %s", first ? "set" : "corrected", now);
    }
}

// Called once, at the first address. The IDF asks again at every new address
// and every CONFIG_LWIP_SNTP_UPDATE_DELAY afterwards.
void net_time_start(void)
{
    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    config.sync_cb = on_time_set;
    config.start = true;
    config.renew_servers_after_new_IP = true;
    config.index_of_first_server = 0;
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    config.smooth_sync = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP did not start: %s", esp_err_to_name(err));
    }
}
