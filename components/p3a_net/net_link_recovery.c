// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// What to do when the SDIO link to the ESP32-C6 dies: an orderly restart, a
// bounded number of times, then life without a network.
//
// The failure, in the UART log:
//   E H_SDIO_DRV: Dropping packet(s) from stream
//   E sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x109  (CRC)
//   E sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x107  (timeout)
//   E H_SDIO_DRV: Unrecoverable host sdio state
// Seen after hours of sustained two-way SDIO load. The receive stream loses
// framing, then writes fail, and the driver declares the link dead. It is a
// known open esp-hosted-mcu bug (issues #121, #167, #184) that neither a newer
// esp_hosted nor a slower SDIO clock fixes, so it is contained here.
//
// esp_hosted's own reaction would be an immediate esp_restart() from inside
// the driver. CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE is off in
// sdkconfig.defaults, so the driver only posts
// ESP_HOSTED_EVENT_TRANSPORT_FAILURE and this file owns the reaction: count
// the restart (p3a_recovery.h), give transfers in flight a few seconds to
// reach their error paths and close their files, then restart. After
// NET_LINK_RECOVERY_REBOOTS restarts in a row with no connection in between,
// the device stays up, takes the network interface down so that nothing keeps
// feeding the dead link, and plays on. That state lasts until a restart.
//
// Rules that must survive:
// - The ladder (restart, bounded, then degraded) never depends on the
//   co-processor's firmware version. The failure event is raised on this side
//   alone, so the ladder also covers units whose co-processor cannot be
//   updated.
// - Do not try to recover in place (esp_hosted_deinit and init, which
//   hard-resets the C6). It was built and dropped on 2026-06-09: the C6 and
//   the microSD share the one SDMMC controller, so the re-initialisation
//   kills the SD card under the player.
// - A later CP_INIT event (the C6 announcing itself again) is logged, not
//   acted on: acting on it risks restarts for nothing.

#include <stdint.h>

#include "esp_event.h"
#include "esp_hosted_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "net_internal.h"
#include "p3a_recovery.h"
#include "p3a_task.h"

static const char *TAG = "net_link";

static bool s_registered;
static atomic_bool s_failure_handled;  // only the first failure of a boot acts
static bool s_cp_init_seen;
static atomic_bool s_dead;

bool net_link_is_dead(void)
{
    return atomic_load(&s_dead);
}

// The screen says nothing yet: the message and its countdown return with the
// UI (M3). Until then the log is the only notice.
void net_restart_after_countdown(const char *why)
{
    for (int i = NET_RESTART_COUNTDOWN_S; i > 0; i--) {
        ESP_LOGW(TAG, "%s. Restarting in %d...", why, i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_restart();
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void restart_task(void *arg)
{
    (void)arg;
    net_restart_after_countdown("Connection to the Wi-Fi chip lost");
}

// Nothing can reach the radio any more. Taking the interface down makes lwIP
// drop its address and routes, so sockets fail at once and the driver stops
// retrying writes into the dead link.
static void go_degraded(void)
{
    atomic_store(&s_dead, true);
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey(NET_STA_NETIF_KEY);
    if (sta != NULL) {
        esp_netif_action_disconnected(sta, NULL, 0, NULL);
    }
    // The disconnect event cannot arrive (it would have to cross the dead
    // link), so the state change and its NET_DOWN come from here.
    net_set_state(P3A_NET_DEGRADED);
    ESP_LOGE(TAG, "restarting has not helped: playing on without a network until the next restart");
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE: {
        if (atomic_exchange(&s_failure_handled, true)) {
            return;
        }
        ESP_LOGE(TAG, "the SDIO link to the Wi-Fi chip is dead");
        if (p3a_recovery_report(P3A_RECOVERY_LINK, false, NET_LINK_RECOVERY_REBOOTS) != P3A_RECOVERY_REBOOT) {
            go_degraded();
            return;
        }
        // From a task of its own: the event loop stays free, and the
        // countdown gives transfers in flight time to unwind.
        p3a_task_config_t task = P3A_TASK_CONFIG("net_restart", restart_task, NULL, 4096, P3A_TASK_APP);
        if (p3a_task_create(&task, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "no task for the countdown; restarting now");
            esp_restart();
        }
        break;
    }
    case ESP_HOSTED_EVENT_CP_INIT: {
        // Once per boot is the co-processor saying hello. The reset reason is
        // meaningful from its firmware 2.9 on and reads 0 on 2.7.0.
        const esp_hosted_event_init_t *init = data;
        int reason = init != NULL ? (int)init->reason : -1;
        if (!s_cp_init_seen) {
            s_cp_init_seen = true;
            ESP_LOGI(TAG, "co-processor up (reset reason %d)", reason);
        } else {
            ESP_LOGW(TAG, "the co-processor restarted underneath us (reset reason %d)", reason);
        }
        break;
    }
    default:
        break;
    }
}

void net_link_recovery_register(void)
{
    if (s_registered) {
        return;
    }
    esp_err_t err = esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot watch the co-processor link: %s", esp_err_to_name(err));
        return;
    }
    s_registered = true;
}
