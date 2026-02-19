// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_connection.c
 * @brief Makapix MQTT connection management
 * 
 * Handles MQTT connection lifecycle, reconnection, status publishing,
 * and channel switching tasks.
 */

#include "makapix_internal.h"
#include "makapix_channel_events.h"
#include "makapix_api.h"
#include "event_bus.h"
#include "esp_heap_caps.h"

/**
 * @brief Timer callback for periodic status publishing
 * Lightweight callback that just notifies the dedicated task to do the work
 * Note: Timer callbacks run from the timer service task, not ISR context
 */
void makapix_status_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    // Notify the dedicated task to publish status (non-blocking)
    if (s_status_publish_task_handle != NULL) {
        xTaskNotifyGive(s_status_publish_task_handle);
    }
}

// Watchdog timer: periodically checks if reconnect task needs re-spawning
#define RECONNECT_WATCHDOG_INTERVAL_MS (30000)
static TimerHandle_t s_reconnect_watchdog_timer = NULL;

/**
 * @brief Watchdog timer callback for reconnect task
 * If makapix is disconnected and no reconnect task is running, re-spawn one.
 */
static void reconnect_watchdog_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    if ((s_makapix_state == MAKAPIX_STATE_DISCONNECTED) &&
        (s_reconnect_task_handle == NULL)) {
        ESP_LOGW(MAKAPIX_TAG, "Reconnect watchdog: state is DISCONNECTED but no reconnect task running, re-spawning");
        if (xTaskCreate(makapix_mqtt_reconnect_task, "mqtt_reconn", 16384, NULL,
                        CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_reconnect_task_handle) != pdPASS) {
            s_reconnect_task_handle = NULL;
            ESP_LOGE(MAKAPIX_TAG, "Reconnect watchdog: failed to create reconnect task");
        }
    }
}

/**
 * @brief Start the reconnect watchdog timer (call once during init)
 */
void makapix_reconnect_watchdog_start(void)
{
    if (s_reconnect_watchdog_timer == NULL) {
        s_reconnect_watchdog_timer = xTimerCreate("reconn_wd",
            pdMS_TO_TICKS(RECONNECT_WATCHDOG_INTERVAL_MS),
            pdTRUE, NULL, reconnect_watchdog_callback);
        if (s_reconnect_watchdog_timer) {
            xTimerStart(s_reconnect_watchdog_timer, 0);
        }
    }
}

/**
 * @brief Dedicated task for status publishing
 * Runs in its own context with sufficient stack for JSON operations and logging
 */
void makapix_status_publish_task(void *pvParameters)
{
    (void)pvParameters;
    
    while (1) {
        // Wait for notification from timer callback
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Skip publishing if SDIO bus is locked (e.g., during OTA check/download)
        // MQTT publishing uses WiFi which could conflict with critical operations
        if (sdio_bus_is_locked()) {
            ESP_LOGD(MAKAPIX_TAG, "Skipping status publish: SDIO bus locked by %s",
                     sdio_bus_get_holder() ? sdio_bus_get_holder() : "unknown");
            continue;
        }
        
        // Publish status if MQTT is connected
        if (makapix_mqtt_is_connected()) {
            makapix_mqtt_publish_status(makapix_get_current_post_id());
        }
    }
}

/**
 * @brief Dedicated task for channel switching
 * Runs in its own context to avoid blocking HTTP/MQTT handlers
 */
void makapix_channel_switch_task(void *pvParameters)
{
    (void)pvParameters;
    char channel[64];
    char identifier[64];
    char display_handle[64];

    while (1) {
        if (xSemaphoreTake(s_channel_switch_sem, portMAX_DELAY) == pdTRUE) {
            if (makapix_get_pending_channel(channel, sizeof(channel),
                                             identifier, sizeof(identifier),
                                             display_handle, sizeof(display_handle))) {
                makapix_clear_pending_channel();

                const char *id = identifier[0] ? identifier : NULL;
                const char *disp = display_handle[0] ? display_handle : NULL;
                esp_err_t err = makapix_switch_to_channel(channel, id, disp);

                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGE(MAKAPIX_TAG, "Channel switch to %s failed: %s", channel, esp_err_to_name(err));
                }
            }
        }
    }
}

/**
 * @brief MQTT connection state change callback
 */
void makapix_mqtt_connection_callback(bool connected)
{
    if (connected) {
        makapix_set_state(MAKAPIX_STATE_CONNECTED);
        
        // Reinitialize API to load player_key (especially important after fresh registration)
        esp_err_t api_init_err = makapix_api_init();
        if (api_init_err != ESP_OK) {
            ESP_LOGW(MAKAPIX_TAG, "makapix_api_init failed after MQTT connect: %s", esp_err_to_name(api_init_err));
        }
        
        // Signal waiting refresh tasks
        makapix_channel_signal_mqtt_connected();
        
        // Update connectivity state
        event_bus_emit_simple(P3A_EVENT_MQTT_CONNECTED);
        
        // Publish initial status
        makapix_mqtt_publish_status(makapix_get_current_post_id());

        // Create status publish task if needed
        if (s_status_publish_task_handle == NULL) {
            if (xTaskCreate(makapix_status_publish_task, "status_pub", 4096, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_status_publish_task_handle) != pdPASS) {
                s_status_publish_task_handle = NULL;
            }
        }

        // Create or restart status timer
        if (!s_status_timer) {
            s_status_timer = xTimerCreate("status_timer", pdMS_TO_TICKS(STATUS_PUBLISH_INTERVAL_MS),
                                          pdTRUE, NULL, makapix_status_timer_callback);
            if (s_status_timer) {
                xTimerStart(s_status_timer, 0);
            }
        } else {
            xTimerStart(s_status_timer, 0);
        }
    } else {
        ESP_LOGW(MAKAPIX_TAG, "MQTT disconnected");
        
        makapix_channel_signal_mqtt_disconnected();
        
        // Update connectivity state
        event_bus_emit_simple(P3A_EVENT_MQTT_DISCONNECTED);
        
        if (s_status_timer) {
            xTimerStop(s_status_timer, 0);
        }
        
        if (s_status_publish_task_handle != NULL) {
            vTaskDelete(s_status_publish_task_handle);
            s_status_publish_task_handle = NULL;
        }
        
        // Start reconnection if we were connected
        if (s_makapix_state == MAKAPIX_STATE_CONNECTED || s_makapix_state == MAKAPIX_STATE_CONNECTING) {
            makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
            
            if (s_reconnect_task_handle == NULL) {
                if (xTaskCreate(makapix_mqtt_reconnect_task, "mqtt_reconn", 16384, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_reconnect_task_handle) != pdPASS) {
                    s_reconnect_task_handle = NULL;
                }
            }
        }
    }
}

// Maximum consecutive TLS auth failures before marking registration invalid
#define MAX_AUTH_FAILURES 3

// Exponential backoff parameters for reconnection
#define RECONNECT_DELAY_INITIAL_MS  5000
#define RECONNECT_DELAY_MAX_MS      60000

// Cert buffers allocated on heap to avoid 12KB stack usage
typedef struct {
    char ca[4096];
    char cert[4096];
    char key[4096];
} mqtt_certs_t;

/**
 * @brief MQTT reconnection task
 */
void makapix_mqtt_reconnect_task(void *pvParameters)
{
    (void)pvParameters;
    char player_key[37];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char wifi_ip[16];
    uint32_t delay_ms = RECONNECT_DELAY_INITIAL_MS;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        // Check if too many auth failures - registration is likely invalid
        if (makapix_mqtt_get_auth_failure_count() >= MAX_AUTH_FAILURES) {
            ESP_LOGE(MAKAPIX_TAG, "Too many TLS auth failures (%d) - registration appears invalid",
                     makapix_mqtt_get_auth_failure_count());
            ESP_LOGE(MAKAPIX_TAG, "Stopping reconnection attempts. Re-provision device to fix.");
            makapix_set_state(MAKAPIX_STATE_REGISTRATION_INVALID);
            ESP_LOGW(MAKAPIX_TAG, "Reconnect task exiting: registration invalid");
            break;
        }

        // Check if WiFi has a valid IP
        if (app_wifi_get_local_ip(wifi_ip, sizeof(wifi_ip)) != ESP_OK ||
            strcmp(wifi_ip, "0.0.0.0") == 0) {
            continue;  // No WiFi, wait silently
        }

        if (makapix_store_get_mqtt_host(mqtt_host, sizeof(mqtt_host)) != ESP_OK) {
            snprintf(mqtt_host, sizeof(mqtt_host), "%s", CONFIG_MAKAPIX_CLUB_HOST);
        }
        if (makapix_store_get_mqtt_port(&mqtt_port) != ESP_OK) {
            mqtt_port = CONFIG_MAKAPIX_CLUB_MQTT_PORT;
        }

        if (makapix_store_get_player_key(player_key, sizeof(player_key)) == ESP_OK &&
            makapix_store_has_certificates()) {

            if (!makapix_mqtt_is_connected()) {
                ESP_LOGI(MAKAPIX_TAG, "Reconnecting to MQTT (backoff: %lums)...", (unsigned long)delay_ms);
                makapix_set_state(MAKAPIX_STATE_CONNECTING);

                mqtt_certs_t *certs = heap_caps_malloc(sizeof(mqtt_certs_t), MALLOC_CAP_SPIRAM);
                if (!certs) {
                    ESP_LOGE(MAKAPIX_TAG, "Failed to allocate cert buffers from PSRAM");
                    makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }

                if (makapix_store_get_ca_cert(certs->ca, sizeof(certs->ca)) != ESP_OK ||
                    makapix_store_get_client_cert(certs->cert, sizeof(certs->cert)) != ESP_OK ||
                    makapix_store_get_client_key(certs->key, sizeof(certs->key)) != ESP_OK) {
                    ESP_LOGE(MAKAPIX_TAG, "Failed to load certificates, will retry");
                    free(certs);
                    makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
                    continue;
                }

                makapix_mqtt_deinit();

                esp_err_t err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port,
                                                  certs->ca, certs->cert, certs->key);
                free(certs);

                if (err == ESP_OK) {
                    err = makapix_mqtt_connect();
                    if (err == ESP_OK) {
                        // Successful connection attempt — reset backoff
                        delay_ms = RECONNECT_DELAY_INITIAL_MS;
                    } else {
                        ESP_LOGW(MAKAPIX_TAG, "MQTT connect failed: %s", esp_err_to_name(err));
                        makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
                        // Increase backoff on failure
                        delay_ms = (delay_ms * 2 > RECONNECT_DELAY_MAX_MS) ? RECONNECT_DELAY_MAX_MS : delay_ms * 2;
                    }
                } else {
                    ESP_LOGW(MAKAPIX_TAG, "MQTT init failed: %s", esp_err_to_name(err));
                    makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
                    delay_ms = (delay_ms * 2 > RECONNECT_DELAY_MAX_MS) ? RECONNECT_DELAY_MAX_MS : delay_ms * 2;
                }
            } else {
                ESP_LOGI(MAKAPIX_TAG, "Reconnect task exiting: already connected");
                break;
            }
        } else {
            ESP_LOGW(MAKAPIX_TAG, "No credentials available for MQTT reconnect, will retry");
            delay_ms = (delay_ms * 2 > RECONNECT_DELAY_MAX_MS) ? RECONNECT_DELAY_MAX_MS : delay_ms * 2;
            continue;
        }
    }

    s_reconnect_task_handle = NULL;
    vTaskDelete(NULL);
}

