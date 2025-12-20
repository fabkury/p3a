/**
 * @file makapix_connection.c
 * @brief Makapix MQTT connection management
 * 
 * Handles MQTT connection lifecycle, reconnection, status publishing,
 * and channel switching tasks.
 */

#include "makapix_internal.h"

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
 * The blocking wait loop in makapix_switch_to_channel() runs here
 */
void makapix_channel_switch_task(void *pvParameters)
{
    (void)pvParameters;
    char channel[64];
    char user_handle[64];
    
    ESP_LOGI(MAKAPIX_TAG, "Channel switch task started");
    
    while (1) {
        // Wait for signal that a channel switch is requested
        if (xSemaphoreTake(s_channel_switch_sem, portMAX_DELAY) == pdTRUE) {
            // Get pending channel info
            if (makapix_get_pending_channel(channel, sizeof(channel), user_handle, sizeof(user_handle))) {
                makapix_clear_pending_channel();
                
                const char *user = user_handle[0] ? user_handle : NULL;
                ESP_LOGI(MAKAPIX_TAG, "Channel switch task: switching to %s", channel);
                
                // This can block for up to 60 seconds - that's OK, we're in our own task
                esp_err_t err = makapix_switch_to_channel(channel, user);
                
                if (err == ESP_ERR_INVALID_STATE) {
                    // Channel load was aborted - a different channel was requested
                    // The pending channel mechanism will handle it on next iteration
                    ESP_LOGI(MAKAPIX_TAG, "Channel switch aborted for new request");
                } else if (err != ESP_OK) {
                    ESP_LOGE(MAKAPIX_TAG, "Channel switch failed: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(MAKAPIX_TAG, "Channel switch completed successfully");
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
    ESP_LOGI(MAKAPIX_TAG, "=== MQTT CONNECTION CALLBACK ===");
    ESP_LOGI(MAKAPIX_TAG, "Connected: %s", connected ? "true" : "false");
    ESP_LOGI(MAKAPIX_TAG, "Previous state: %d", s_makapix_state);
    
    if (connected) {
        ESP_LOGI(MAKAPIX_TAG, "MQTT connected successfully");
        s_makapix_state = MAKAPIX_STATE_CONNECTED;
        ESP_LOGI(MAKAPIX_TAG, "New state: %d (CONNECTED)", s_makapix_state);
        
        // Publish initial status
        ESP_LOGI(MAKAPIX_TAG, "Publishing initial status...");
        makapix_mqtt_publish_status(makapix_get_current_post_id());

        // Create status publish task if it doesn't exist
        if (s_status_publish_task_handle == NULL) {
            ESP_LOGI(MAKAPIX_TAG, "Creating status publish task...");
            BaseType_t task_ret = xTaskCreate(makapix_status_publish_task, "status_pub", 4096, NULL, 5, &s_status_publish_task_handle);
            if (task_ret != pdPASS) {
                ESP_LOGE(MAKAPIX_TAG, "Failed to create status publish task");
                s_status_publish_task_handle = NULL;
            } else {
                ESP_LOGI(MAKAPIX_TAG, "Status publish task created successfully");
            }
        }

        // Create or restart periodic status timer
        if (!s_status_timer) {
            ESP_LOGI(MAKAPIX_TAG, "Creating status timer (interval: %d ms)", STATUS_PUBLISH_INTERVAL_MS);
            s_status_timer = xTimerCreate("status_timer", pdMS_TO_TICKS(STATUS_PUBLISH_INTERVAL_MS),
                                          pdTRUE, NULL, makapix_status_timer_callback);
            if (s_status_timer) {
                xTimerStart(s_status_timer, 0);
                ESP_LOGI(MAKAPIX_TAG, "Status timer created and started");
            } else {
                ESP_LOGE(MAKAPIX_TAG, "Failed to create status timer");
            }
        } else {
            // Timer exists but may have been stopped during disconnect - restart it
            xTimerStart(s_status_timer, 0);
            ESP_LOGI(MAKAPIX_TAG, "Status timer restarted");
        }
        
        // Trigger refresh on the current Makapix channel if one exists
        // This handles the boot-time case where the channel was loaded before MQTT connected
        if (s_current_channel && s_current_channel_id[0]) {
            ESP_LOGI(MAKAPIX_TAG, "Triggering refresh for current channel: %s", s_current_channel_id);
            channel_request_refresh(s_current_channel);
        }
    } else {
        ESP_LOGI(MAKAPIX_TAG, "MQTT disconnected");
        
        // Stop status timer to prevent publish attempts during reconnection
        if (s_status_timer) {
            xTimerStop(s_status_timer, 0);
            ESP_LOGI(MAKAPIX_TAG, "Status timer stopped");
        }
        
        // Delete status publish task to free resources
        if (s_status_publish_task_handle != NULL) {
            ESP_LOGI(MAKAPIX_TAG, "Deleting status publish task...");
            vTaskDelete(s_status_publish_task_handle);
            s_status_publish_task_handle = NULL;
            ESP_LOGI(MAKAPIX_TAG, "Status publish task deleted");
        }
        
        // Only transition to DISCONNECTED and start reconnection if we were connected
        // Don't interfere with provisioning or other states
        if (s_makapix_state == MAKAPIX_STATE_CONNECTED || s_makapix_state == MAKAPIX_STATE_CONNECTING) {
            s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
            ESP_LOGI(MAKAPIX_TAG, "New state: %d (DISCONNECTED)", s_makapix_state);
            
            // Start reconnection task if not already running
            // Only do this when transitioning to DISCONNECTED (not during provisioning, etc.)
            if (s_reconnect_task_handle == NULL) {
                ESP_LOGI(MAKAPIX_TAG, "Starting reconnection task...");
                BaseType_t task_ret = xTaskCreate(makapix_mqtt_reconnect_task, "mqtt_reconn", 16384, NULL, 5, &s_reconnect_task_handle);
                if (task_ret != pdPASS) {
                    ESP_LOGE(MAKAPIX_TAG, "Failed to create reconnection task");
                    s_reconnect_task_handle = NULL;
                } else {
                    ESP_LOGI(MAKAPIX_TAG, "Reconnection task created successfully");
                }
            } else {
                ESP_LOGI(MAKAPIX_TAG, "Reconnection task already running");
            }
        } else {
            ESP_LOGI(MAKAPIX_TAG, "State unchanged: %d (not starting reconnection)", s_makapix_state);
        }
    }
    ESP_LOGI(MAKAPIX_TAG, "=== END MQTT CONNECTION CALLBACK ===");
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
        vTaskDelay(pdMS_TO_TICKS(5000)); // Wait 5 seconds before retry

        // Check if WiFi has a valid IP before attempting MQTT reconnection
        // This prevents futile reconnection attempts when DHCP lease expired or WiFi is reconnecting
        if (app_wifi_get_local_ip(wifi_ip, sizeof(wifi_ip)) != ESP_OK || 
            strcmp(wifi_ip, "0.0.0.0") == 0) {
            ESP_LOGW(MAKAPIX_TAG, "WiFi has no valid IP address, skipping MQTT reconnection");
            continue;
        }

        // Get MQTT host/port from store or use CONFIG fallback
        if (makapix_store_get_mqtt_host(mqtt_host, sizeof(mqtt_host)) != ESP_OK) {
            snprintf(mqtt_host, sizeof(mqtt_host), "%s", CONFIG_MAKAPIX_CLUB_HOST);
        }
        if (makapix_store_get_mqtt_port(&mqtt_port) != ESP_OK) {
            mqtt_port = CONFIG_MAKAPIX_CLUB_MQTT_PORT;
        }
        
        if (makapix_store_get_player_key(player_key, sizeof(player_key)) == ESP_OK &&
            makapix_store_has_certificates()) {

            if (!makapix_mqtt_is_connected()) {
                ESP_LOGI(MAKAPIX_TAG, "=== MQTT RECONNECTION ATTEMPT ===");
                ESP_LOGI(MAKAPIX_TAG, "WiFi IP: %s", wifi_ip);
                ESP_LOGI(MAKAPIX_TAG, "Current state: %d", s_makapix_state);
                ESP_LOGI(MAKAPIX_TAG, "Player key: %s", player_key);
                ESP_LOGI(MAKAPIX_TAG, "MQTT host: %s", mqtt_host);
                ESP_LOGI(MAKAPIX_TAG, "MQTT port: %d", mqtt_port);
                s_makapix_state = MAKAPIX_STATE_CONNECTING;
                
                // Load certificates from SPIFFS
                char ca_cert[4096];
                char client_cert[4096];
                char client_key[4096];
                
                esp_err_t err = makapix_store_get_ca_cert(ca_cert, sizeof(ca_cert));
                if (err != ESP_OK) {
                    ESP_LOGE(MAKAPIX_TAG, "Failed to load CA cert: %s", esp_err_to_name(err));
                    s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                    continue;
                }
                
                err = makapix_store_get_client_cert(client_cert, sizeof(client_cert));
                if (err != ESP_OK) {
                    ESP_LOGE(MAKAPIX_TAG, "Failed to load client cert: %s", esp_err_to_name(err));
                    s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                    continue;
                }
                
                err = makapix_store_get_client_key(client_key, sizeof(client_key));
                if (err != ESP_OK) {
                    ESP_LOGE(MAKAPIX_TAG, "Failed to load client key: %s", esp_err_to_name(err));
                    s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                    continue;
                }
                
                // Deinit existing client before reinitializing to prevent resource leaks
                makapix_mqtt_deinit();
                
                err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port, ca_cert, client_cert, client_key);
                if (err == ESP_OK) {
                    ESP_LOGI(MAKAPIX_TAG, "MQTT init successful, attempting connect...");
                    err = makapix_mqtt_connect();
                    if (err != ESP_OK) {
                        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                        ESP_LOGW(MAKAPIX_TAG, "MQTT connection failed: %s (%d)", esp_err_to_name(err), err);
                    } else {
                        ESP_LOGI(MAKAPIX_TAG, "MQTT connect() returned OK, waiting for connection event...");
                    }
                    // State will be updated to CONNECTED by the connection callback when MQTT actually connects
                } else {
                    s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                    ESP_LOGW(MAKAPIX_TAG, "MQTT init failed: %s (%d)", esp_err_to_name(err), err);
                }
            } else {
                ESP_LOGI(MAKAPIX_TAG, "MQTT already connected, exiting reconnection task");
                // Already connected, exit task
                break;
            }
        } else {
            // No credentials or certificates, exit task
            if (!makapix_store_has_certificates()) {
                ESP_LOGW(MAKAPIX_TAG, "Certificates not found, cannot reconnect");
            }
            break;
        }
    }

    ESP_LOGI(MAKAPIX_TAG, "Reconnection task exiting");
    s_reconnect_task_handle = NULL;
    vTaskDelete(NULL);
}

