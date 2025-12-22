/**
 * @file makapix_connection.c
 * @brief Makapix MQTT connection management
 * 
 * Handles MQTT connection lifecycle, reconnection, status publishing,
 * and channel switching tasks.
 */

#include "makapix_internal.h"
#include "makapix_channel_events.h"

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
    char user_handle[64];
    
    while (1) {
        if (xSemaphoreTake(s_channel_switch_sem, portMAX_DELAY) == pdTRUE) {
            if (makapix_get_pending_channel(channel, sizeof(channel), user_handle, sizeof(user_handle))) {
                makapix_clear_pending_channel();
                
                const char *user = user_handle[0] ? user_handle : NULL;
                esp_err_t err = makapix_switch_to_channel(channel, user);
                
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
        s_makapix_state = MAKAPIX_STATE_CONNECTED;
        
        // Signal waiting refresh tasks
        makapix_channel_signal_mqtt_connected();
        
        // Publish initial status
        makapix_mqtt_publish_status(makapix_get_current_post_id());

        // Create status publish task if needed
        if (s_status_publish_task_handle == NULL) {
            if (xTaskCreate(makapix_status_publish_task, "status_pub", 4096, NULL, 5, &s_status_publish_task_handle) != pdPASS) {
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
        
        if (s_status_timer) {
            xTimerStop(s_status_timer, 0);
        }
        
        if (s_status_publish_task_handle != NULL) {
            vTaskDelete(s_status_publish_task_handle);
            s_status_publish_task_handle = NULL;
        }
        
        // Start reconnection if we were connected
        if (s_makapix_state == MAKAPIX_STATE_CONNECTED || s_makapix_state == MAKAPIX_STATE_CONNECTING) {
            s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
            
            if (s_reconnect_task_handle == NULL) {
                if (xTaskCreate(makapix_mqtt_reconnect_task, "mqtt_reconn", 16384, NULL, 5, &s_reconnect_task_handle) != pdPASS) {
                    s_reconnect_task_handle = NULL;
                }
            }
        }
    }
}

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

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));

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
                ESP_LOGI(MAKAPIX_TAG, "Reconnecting to MQTT...");
                s_makapix_state = MAKAPIX_STATE_CONNECTING;
                
                char ca_cert[4096];
                char client_cert[4096];
                char client_key[4096];
                
                if (makapix_store_get_ca_cert(ca_cert, sizeof(ca_cert)) != ESP_OK ||
                    makapix_store_get_client_cert(client_cert, sizeof(client_cert)) != ESP_OK ||
                    makapix_store_get_client_key(client_key, sizeof(client_key)) != ESP_OK) {
                    ESP_LOGE(MAKAPIX_TAG, "Failed to load certificates");
                    s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                    continue;
                }
                
                makapix_mqtt_deinit();
                
                esp_err_t err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port, ca_cert, client_cert, client_key);
                if (err == ESP_OK) {
                    err = makapix_mqtt_connect();
                    if (err != ESP_OK) {
                        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                    }
                } else {
                    s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                }
            } else {
                break;  // Already connected
            }
        } else {
            break;  // No credentials
        }
    }

    s_reconnect_task_handle = NULL;
    vTaskDelete(NULL);
}

