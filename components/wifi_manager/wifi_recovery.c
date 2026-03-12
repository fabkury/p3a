// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "config_store.h"
#include "wifi_manager_internal.h"

#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static const char *TAG = "wifi_recovery";

static const uint32_t s_wifi_health_interval_ms = 120000; // 120 seconds

// UI function for showing countdown before hard reboot (weak: resolved at link time)
extern esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent) __attribute__((weak));

void wifi_recovery_task(void *arg)
{
    (void)arg;
    char saved_ssid[MAX_SSID_LEN] = {0};
    char saved_password[MAX_PASSWORD_LEN] = {0};
    const int max_recovery_attempts = 5;

    while (true) {
        // Wait until we are notified to perform a full reinit
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Reset retry counter so post-recovery disconnect events use fresh backoff
        s_retry_num = 0;

        bool recovered = false;
        bool c6_stack_broken = false;

        for (int attempt = 0; attempt < max_recovery_attempts && !recovered; attempt++) {
            if (attempt > 0) {
                int backoff_ms = 5000 * (1 << (attempt - 1)); // 5s, 10s, 20s, 40s
                ESP_LOGW(TAG, "WiFi recovery: retry %d/%d (backoff %dms)",
                         attempt + 1, max_recovery_attempts, backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            }

            ESP_LOGW(TAG, "WiFi recovery: performing full WiFi re-initialization (attempt %d/%d)",
                     attempt + 1, max_recovery_attempts);

            // Best-effort stop/deinit
            esp_err_t err = esp_wifi_remote_stop();
            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
                ESP_LOGW(TAG, "esp_wifi_remote_stop failed: %s", esp_err_to_name(err));
            }

            err = esp_wifi_remote_deinit();
            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
                ESP_LOGW(TAG, "esp_wifi_remote_deinit failed: %s", esp_err_to_name(err));
            }

            vTaskDelay(pdMS_TO_TICKS(1000));

            // Re-init WiFi remote driver
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            err = esp_wifi_remote_init(&cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_remote_init failed during recovery: %s", esp_err_to_name(err));
                c6_stack_broken = true;
                continue;
            }

            // Ensure handlers stay registered (registering twice is prevented)
            wifi_register_event_handlers_once();

            // Reload credentials from NVS (user preference)
            bool has_credentials = (wifi_load_credentials(saved_ssid, saved_password) == ESP_OK) && (strlen(saved_ssid) > 0);
            if (!has_credentials) {
                ESP_LOGE(TAG, "WiFi recovery: no saved credentials; cannot restart STA");
                break;  // Non-retryable: no point retrying without credentials
            }

            // Reconfigure STA and restart
            wifi_config_t wifi_config = {
                .sta = {
                    .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
                    .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
                    .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
                },
            };

            size_t ssid_len = strlen(saved_ssid);
            size_t password_len = strlen(saved_password);
            if (ssid_len >= sizeof(wifi_config.sta.ssid)) {
                ssid_len = sizeof(wifi_config.sta.ssid) - 1;
            }
            if (password_len >= sizeof(wifi_config.sta.password)) {
                password_len = sizeof(wifi_config.sta.password) - 1;
            }
            memcpy((char*)wifi_config.sta.ssid, saved_ssid, ssid_len);
            wifi_config.sta.ssid[ssid_len] = '\0';
            memcpy((char*)wifi_config.sta.password, saved_password, password_len);
            wifi_config.sta.password[password_len] = '\0';

            // Clear connection bits before restarting
            if (s_wifi_event_group) {
                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
            }

            err = esp_wifi_remote_set_mode(WIFI_MODE_STA);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_remote_set_mode failed during recovery: %s", esp_err_to_name(err));
                c6_stack_broken = true;
                continue;
            }

            err = esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_remote_set_config failed during recovery: %s", esp_err_to_name(err));
                continue;
            }

            err = esp_wifi_remote_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_remote_start failed during recovery: %s", esp_err_to_name(err));
                continue;
            }

            wifi_disable_power_save_best_effort();
            wifi_set_protocol_11ax(WIFI_IF_STA);

            // Kick connection attempt
            err = esp_wifi_remote_connect();
            if (err == ESP_OK) {
                recovered = true;
            } else {
                ESP_LOGW(TAG, "esp_wifi_remote_connect failed during recovery: %s (attempt %d/%d)",
                         esp_err_to_name(err), attempt + 1, max_recovery_attempts);
            }
        }

        if (recovered) {
            s_total_recovery_failures = 0;
            ESP_LOGW(TAG, "WiFi recovery: reinit complete; reconnect will proceed via events");
        } else if (c6_stack_broken) {
            s_total_recovery_failures++;
            ESP_LOGE(TAG, "WiFi recovery: all %d attempts failed (C6 stack broken, failure #%d)",
                     max_recovery_attempts, s_total_recovery_failures);

            if (s_total_recovery_failures >= 2) {
                uint16_t streak = config_store_get_wifi_reboot_streak();
                if (streak >= 1) {
                    ESP_LOGE(TAG, "WiFi recovery: reboot streak=%u, staying in degraded mode to prevent loop",
                             streak);
                } else {
                    ESP_LOGE(TAG, "WiFi recovery: escalating to hard reboot (streak=%u)", streak);
                    config_store_increment_wifi_reboot_total();
                    config_store_increment_wifi_reboot_streak();

                    for (int i = 10; i > 0; i--) {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "WiFi chip not responding\nRestarting in %d...", i);
                        if (ugfx_ui_show_channel_message) {
                            ugfx_ui_show_channel_message("p3a", msg, -1);
                        }
                        ESP_LOGW(TAG, "Hard reboot in %d...", i);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                    esp_restart();
                }
            }
        } else {
            ESP_LOGE(TAG, "WiFi recovery: all %d attempts failed; health monitor will retry later",
                     max_recovery_attempts);
        }
        s_reinit_in_progress = false;
    }
}

void wifi_schedule_full_reinit(void)
{
    if (s_reinit_in_progress) {
        ESP_LOGW(TAG, "WiFi recovery: reinit already in progress; ignoring request");
        return;
    }
    if (!s_wifi_recovery_task) {
        ESP_LOGE(TAG, "WiFi recovery: recovery task not running; cannot reinit");
        return;
    }

    s_reinit_in_progress = true;
    xTaskNotifyGive(s_wifi_recovery_task);
}

void wifi_health_monitor_task(void *arg)
{
    (void)arg;
    const char *HTAG = "wifi_health";

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(s_wifi_health_interval_ms));

        // Only monitor after we have been successfully connected at least once.
        if (!s_initial_connection_done) {
            continue;
        }

        // Skip monitoring while captive portal is active (AP mode)
        if (s_captive_portal_server != NULL) {
            continue;
        }

        // Skip monitoring if we're already performing a recovery reinit
        if (s_reinit_in_progress) {
            continue;
        }

        // Detect prolonged IP loss after initial connection (safety net for failed recovery)
        if (!wifi_sta_has_ip()) {
            s_no_ip_health_cycles++;
            ESP_LOGW(HTAG, "No IP for %d health cycle(s) after initial connection",
                     s_no_ip_health_cycles);
            if (s_no_ip_health_cycles >= 3) {
                ESP_LOGW(HTAG, "Forcing WiFi recovery after prolonged IP loss");
                s_no_ip_health_cycles = 0;
                wifi_schedule_full_reinit();
            }
            continue;
        }
        s_no_ip_health_cycles = 0;

        // DNS-based reachability check (requires internet; chosen by user).
        // Retry up to 3 times to avoid forcing a WiFi reconnect on transient DNS blips.
        struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
        };
        const int max_dns_attempts = 3;
        bool dns_ok = false;
        int last_err = 0;

        for (int attempt = 0; attempt < max_dns_attempts && !dns_ok; attempt++) {
            struct addrinfo *res = NULL;
            last_err = getaddrinfo("google.com", "80", &hints, &res);
            if (last_err == 0 && res != NULL) {
                dns_ok = true;
            }
            if (res) {
                freeaddrinfo(res);
            }
            if (!dns_ok && attempt < max_dns_attempts - 1) {
                ESP_LOGW(HTAG, "Health check DNS attempt %d/%d failed (err=%d); retrying in 2s",
                         attempt + 1, max_dns_attempts, last_err);
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }

        if (!dns_ok) {
            ESP_LOGW(HTAG, "Health check failed after %d attempts (last err=%d); forcing WiFi reconnect",
                     max_dns_attempts, last_err);
            esp_wifi_remote_disconnect(); // triggers WIFI_EVENT_STA_DISCONNECTED -> reconnect logic
        } else {
            ESP_LOGD(HTAG, "Health check OK");
        }
    }
}
