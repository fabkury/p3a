// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file transport_recovery.c
 * @brief ESP-Hosted SDIO transport failure handling (Phase 0: orderly reboot)
 *
 * Live plan: docs/transport-recovery/PLAN.md
 *
 * esp_hosted's built-in reaction to an unrecoverable SDIO transport failure
 * is an immediate esp_restart() from inside the driver: no SD quiesce, no
 * on-screen notice, no telemetry. CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE
 * is disabled in sdkconfig, so the driver now only posts
 * ESP_HOSTED_EVENT_TRANSPORT_FAILURE (guaranteed regardless of that setting)
 * and this module owns the reaction:
 *
 * Phase 0 (this code): orderly reboot. Persist telemetry counters, show an
 * on-screen countdown, give in-flight network error paths a few seconds to
 * unwind file handles, then esp_restart(). A reboot-streak guard parks the
 * device in degraded (playback-only) mode instead of reboot-looping when the
 * transport fails on every boot; degraded mode quiesces the network stack
 * (netif down, MQTT stopped, health monitor parked) so nothing keeps feeding
 * the dead SDIO link. Identical behavior for every slave fw version (the
 * event is generated host-side; the stuck-2.7.0 fleet is safe).
 *
 * Phase 1 (planned): in-place recovery (esp_hosted_deinit/init, which
 * hard-resets the C6 via GPIO 54) for slave fw >= 2.9.x, escalating to this
 * Phase 0 path on failure. See the plan doc.
 */

#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_hosted_event.h"
#include "config_store.h"
#include "event_bus.h"
#include "makapix_mqtt.h"
#include "makapix_channel_events.h"
#include "wifi_manager_internal.h"

static const char *TAG = "transport_rec";

// Stop auto-rebooting after this many consecutive transport-failure reboots
// without an intervening successful connection (streak resets on GOT_IP).
// A device whose transport dies on every boot settles into degraded
// playback-only mode instead of reboot-looping.
#define TRANSPORT_REBOOT_STREAK_LIMIT 3

// Countdown before the reboot: lets in-flight downloads/MQTT/HTTP hit their
// error paths and close files before the reset, and tells a human watching
// the screen what is about to happen.
#define TRANSPORT_REBOOT_COUNTDOWN_S 5

// How long the degraded-mode notice holds the screen before playback resumes.
#define TRANSPORT_DEGRADED_NOTICE_MS 5000

// UI hooks (weak: resolved at link time; all defined in the main component).
// display_renderer_enter_ui_mode() is required for visibility: during
// playback the animation renderer wins and ugfx messages never composite
// (same pattern as the touch-recovery countdown in app_touch.c).
extern esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent) __attribute__((weak));
extern void ugfx_ui_hide_channel_message(void) __attribute__((weak));
extern esp_err_t display_renderer_enter_ui_mode(void) __attribute__((weak));
extern void display_renderer_exit_ui_mode(void) __attribute__((weak));

static bool s_handlers_registered = false;
static bool s_failure_handled = false;   // only the first TRANSPORT_FAILURE acts
static bool s_cp_init_seen = false;      // first CP_INIT is the normal boot announcement
static bool s_degraded = false;          // terminal until reboot; see transport_recovery_is_degraded()

bool transport_recovery_is_degraded(void)
{
    return s_degraded;
}

// Task argument: which UI flow to run.
typedef enum {
    TRANSPORT_UI_REBOOT,          // countdown, then esp_restart()
    TRANSPORT_UI_DEGRADED_NOTICE, // transient notice, then resume playback
} transport_ui_action_t;

// Quiesce the network stack once the transport is declared dead. Nothing can
// reach the radio anymore, so stop feeding the dead SDIO link:
// - netif down: lwIP drops IP/routes, sockets fail fast locally, and the
//   driver's CMD53 retry/log storm ends (it only errors when fed TX).
// - standard disconnect signals: the same calls app_wifi's
//   WIFI_EVENT_STA_DISCONNECTED handler makes — that handler can never fire
//   here, because the disconnect event would have to arrive over the dead
//   transport itself.
// - MQTT stop: ends the reconnect-backoff loop. Called after netif down so
//   its socket ops fail instantly instead of waiting out poll timeouts.
// The WiFi health monitor stands down separately via
// transport_recovery_is_degraded() (see wifi_health_monitor_task).
static void transport_degraded_quiesce(void)
{
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
    if (sta_netif) {
        esp_netif_action_disconnected(sta_netif, NULL, 0, NULL);
    }
    makapix_channel_signal_wifi_disconnected();
    event_bus_emit_simple(P3A_EVENT_WIFI_DISCONNECTED);
    makapix_mqtt_disconnect();
    ESP_LOGW(TAG, "Degraded mode: network stack quiesced (playback-only until reboot)");
}

static void transport_failure_ui_task(void *arg)
{
    transport_ui_action_t action = (transport_ui_action_t)(intptr_t)arg;

    // Switch the render pipeline to the UI source so the message is visible.
    // Safe on the reboot path: wait_for_render_mode times out after 500 ms,
    // so a wedged renderer cannot hold the reboot hostage.
    if (display_renderer_enter_ui_mode) {
        display_renderer_enter_ui_mode();
    }

    if (action == TRANSPORT_UI_REBOOT) {
        for (int i = TRANSPORT_REBOOT_COUNTDOWN_S; i > 0; i--) {
            char msg[64];
            snprintf(msg, sizeof(msg), "WiFi chip connection lost.\nRestarting in %d...", i);
            if (ugfx_ui_show_channel_message) {
                ugfx_ui_show_channel_message("p3a", msg, -1);
            }
            ESP_LOGW(TAG, "Transport-failure reboot in %d...", i);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        esp_restart();
        // Never reached
    }

    // Degraded notice: show briefly, then hand the screen back to playback.
    if (ugfx_ui_show_channel_message) {
        ugfx_ui_show_channel_message("p3a", "WiFi chip not responding.\nPlayback continues offline.", -1);
    }
    // Quiesce while the notice is on screen (MQTT stop may take a moment).
    transport_degraded_quiesce();
    vTaskDelay(pdMS_TO_TICKS(TRANSPORT_DEGRADED_NOTICE_MS));
    if (ugfx_ui_hide_channel_message) {
        ugfx_ui_hide_channel_message();
    }
    if (display_renderer_exit_ui_mode) {
        display_renderer_exit_ui_mode();
    }
    vTaskDelete(NULL);
}

static void transport_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    switch (event_id) {
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE: {
        if (s_failure_handled) {
            return; // reboot (or degraded-mode decision) already under way
        }
        s_failure_handled = true;

        ESP_LOGE(TAG, "ESP-Hosted transport failure (SDIO link to C6 is dead)");
        config_store_increment_transport_reboot_total();

        uint16_t streak = config_store_get_transport_reboot_streak();
        if (streak >= TRANSPORT_REBOOT_STREAK_LIMIT) {
            // Rebooting hasn't helped; keep playing from SD instead of
            // reboot-looping. The WiFi health monitor's own escalation (with
            // its own streak guard) remains active independently.
            ESP_LOGE(TAG, "Transport reboot streak=%u >= %d; staying in degraded mode",
                     streak, TRANSPORT_REBOOT_STREAK_LIMIT);
            // Parks the WiFi health monitor immediately (it polls this flag),
            // even if the notice task below cannot be created.
            s_degraded = true;
            // Notice + quiesce from a dedicated task: the UI-mode switch and
            // the MQTT stop both block, and the event loop must stay free.
            if (xTaskCreate(transport_failure_ui_task, "transport_ui", 4096,
                            (void *)(intptr_t)TRANSPORT_UI_DEGRADED_NOTICE,
                            CONFIG_P3A_APP_TASK_PRIORITY, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to create degraded-notice task");
                // Minimal inline fallback: netif down is quick and stops the
                // lwIP-fed SDIO retry storm; skip the blocking MQTT stop.
                esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
                if (sta_netif) {
                    esp_netif_action_disconnected(sta_netif, NULL, 0, NULL);
                }
            }
            return;
        }
        config_store_increment_transport_reboot_streak();

        // Reboot from a dedicated task: keeps the event loop free and gives
        // in-flight network error paths the countdown to unwind first.
        if (xTaskCreate(transport_failure_ui_task, "transport_rbt", 4096,
                        (void *)(intptr_t)TRANSPORT_UI_REBOOT,
                        CONFIG_P3A_APP_TASK_PRIORITY, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create reboot task; restarting inline");
            esp_restart();
        }
        break;
    }
    case ESP_HOSTED_EVENT_CP_INIT: {
        // The C6 announces itself once per boot (we reset it on every host
        // bootup). A later CP_INIT means the C6 rebooted underneath us.
        // Phase 0 records the evidence only; Phase 1 will act on it.
        // reset_reason is meaningful on >= 2.9.x slaves; reads 0 on 2.7.0.
        esp_hosted_event_init_t *init = (esp_hosted_event_init_t *)event_data;
        esp_reset_reason_t reason = init ? init->reason : ESP_RST_UNKNOWN;
        if (!s_cp_init_seen) {
            s_cp_init_seen = true;
            ESP_LOGI(TAG, "C6 co-processor up (reset_reason=%d)", (int)reason);
        } else {
            ESP_LOGW(TAG, "Unexpected C6 re-init (reset_reason=%d) - co-processor rebooted underneath us",
                     (int)reason);
        }
        break;
    }
    case ESP_HOSTED_EVENT_TRANSPORT_UP:
        ESP_LOGD(TAG, "Transport up");
        break;
    case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
        ESP_LOGD(TAG, "Transport down");
        break;
    default:
        break;
    }
}

void transport_recovery_register_events_once(void)
{
    if (s_handlers_registered) {
        return;
    }

    esp_err_t err = esp_event_handler_instance_register(ESP_HOSTED_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &transport_event_handler,
                                                        NULL,
                                                        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ESP_HOSTED_EVENT handler: %s", esp_err_to_name(err));
        return;
    }
    s_handlers_registered = true;
}

#if CONFIG_P3A_TRANSPORT_FAULT_INJECT
// ============================================================================
// TEMPORARY TEST HOOK (TRANSPORT_FAULT_INJECT) — DELETE AFTER PHASE 0 TESTING
// ============================================================================
// Called (via weak extern) by the temporary HTTP endpoint in
// http_api/http_api.c: POST /action/inject_transport_failure[?streak=N][&real=1].
//
// Synthetic mode (default): posts the same event the SDIO driver posts on a
// real failure — exercises the handler logic only.
//
// Real mode (real=1): pulses the C6 reset line (same pin, polarity, and
// 10 ms pulse width as the driver's own transport_gpio_reset). The C6
// reboots fresh while the host driver still believes the SDIO session is
// alive, so the next TX hits real CMD53 errors -> write retries ->
// "Unrecoverable host sdio state" -> TRANSPORT_FAILURE: the full production
// detection chain. Detection needs some SDIO traffic (downloads, MQTT
// keepalive, health-check DNS) — run it while the device is busy.
//
// ?streak=N presets the persisted streak first in both modes, so
// streak >= TRANSPORT_REBOOT_STREAK_LIMIT exercises the degraded branch.
//
// To remove every piece of this hook: grep -rn "TRANSPORT_FAULT_INJECT"
#include "driver/gpio.h"

// Same mapping as the driver's H_RESET_VAL_* (port_esp_hosted_host_config.h):
// with RESET_ACTIVE_HIGH the line idles HIGH (C6 running); LOW holds reset.
#if CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH
  #define C6_RESET_HOLD_LEVEL     0
  #define C6_RESET_RELEASE_LEVEL  1
#else
  #define C6_RESET_HOLD_LEVEL     1
  #define C6_RESET_RELEASE_LEVEL  0
#endif

esp_err_t transport_recovery_inject_failure_for_test(int streak_preset, bool real_gpio_reset);

esp_err_t transport_recovery_inject_failure_for_test(int streak_preset, bool real_gpio_reset)
{
    if (streak_preset >= 0) {
        config_store_reset_transport_reboot_streak();
        for (int i = 0; i < streak_preset && i < 10; i++) {
            config_store_increment_transport_reboot_streak();
        }
    }

    // Un-latch so repeated injections in one boot each act fresh (e.g. a
    // degraded-mode test with ?streak=3 followed by a plain reboot test).
    // Only undoes what a previous *injection* latched; on a clean boot it is
    // false anyway, so the handler still runs exactly its production logic.
    s_failure_handled = false;

    if (real_gpio_reset) {
        ESP_LOGW(TAG, "TEST HOOK: pulsing C6 reset GPIO[%d] for real driver-path detection (streak_preset=%d)",
                 CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE, streak_preset);
        gpio_set_level((gpio_num_t)CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE, C6_RESET_HOLD_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE, C6_RESET_RELEASE_LEVEL);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "TEST HOOK: posting synthetic TRANSPORT_FAILURE (streak_preset=%d)",
             streak_preset);
    return esp_event_post(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
                          NULL, 0, pdMS_TO_TICKS(1000));
}
#endif // CONFIG_P3A_TRANSPORT_FAULT_INJECT
