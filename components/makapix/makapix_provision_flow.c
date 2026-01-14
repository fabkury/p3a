// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_provision_flow.c
 * @brief Makapix provisioning flow and credential polling
 * 
 * Handles device registration, registration code display, and 
 * credential polling after user completes registration on web.
 */

#include "makapix_internal.h"
#include "connectivity_state.h"

/**
 * @brief Provisioning task
 */
void makapix_provisioning_task(void *pvParameters)
{
    (void)pvParameters;
    makapix_provision_result_t result;
    
    // Set status to "Querying endpoint" right before making the HTTP request
    snprintf(s_provisioning_status, sizeof(s_provisioning_status), "Querying endpoint");
    
    esp_err_t err = makapix_provision_request(&result);

    // Check if provisioning was cancelled while we were waiting
    if (s_provisioning_cancelled) {
        ESP_LOGD(MAKAPIX_TAG, "Provisioning was cancelled, aborting");
        s_provisioning_cancelled = false;  // Reset flag
        vTaskDelete(NULL);
        return;
    }

    if (err == ESP_OK) {
        // Double-check cancellation before saving (race condition protection)
        if (s_provisioning_cancelled) {
            ESP_LOGD(MAKAPIX_TAG, "Provisioning was cancelled after request completed, aborting");
            s_provisioning_cancelled = false;
            vTaskDelete(NULL);
            return;
        }

        // Save credentials (player_key and broker info)
        // Note: Old registration data will be cleared later when credentials are successfully received
        err = makapix_store_save_credentials(result.player_key, result.mqtt_host, result.mqtt_port);
        if (err == ESP_OK) {
            // Final check before updating state
            if (!s_provisioning_cancelled) {
                // Store registration code for display
                snprintf(s_registration_code, sizeof(s_registration_code), "%s", result.registration_code);
                snprintf(s_registration_expires, sizeof(s_registration_expires), "%s", result.expires_at);
                
                s_makapix_state = MAKAPIX_STATE_SHOW_CODE;
                ESP_LOGD(MAKAPIX_TAG, "Provisioning successful, registration code: %s", s_registration_code);
                ESP_LOGD(MAKAPIX_TAG, "Starting credential polling task...");
                
                // Start credential polling task
                // Stack size needs to be large enough for makapix_credentials_result_t (3x 4096 byte arrays = ~12KB)
                BaseType_t poll_ret = xTaskCreate(makapix_credentials_poll_task, "cred_poll", 16384, NULL, 5, &s_poll_task_handle);
                if (poll_ret != pdPASS) {
                    ESP_LOGE(MAKAPIX_TAG, "Failed to create credential polling task");
                    s_makapix_state = MAKAPIX_STATE_IDLE;
                    s_poll_task_handle = NULL;
                }
            } else {
                ESP_LOGD(MAKAPIX_TAG, "Provisioning was cancelled, discarding results");
                s_provisioning_cancelled = false;
            }
        } else {
            ESP_LOGE(MAKAPIX_TAG, "Failed to save credentials: %s", esp_err_to_name(err));
            s_makapix_state = MAKAPIX_STATE_IDLE;
        }
    } else {
        ESP_LOGE(MAKAPIX_TAG, "Provisioning failed: %s", esp_err_to_name(err));
        if (!s_provisioning_cancelled) {
            s_makapix_state = MAKAPIX_STATE_IDLE;
        }
    }

    s_provisioning_cancelled = false;  // Reset flag
    vTaskDelete(NULL);
}

/**
 * @brief Credentials polling task
 * 
 * Polls for TLS certificates after registration code is displayed.
 * Runs while state is SHOW_CODE.
 */
void makapix_credentials_poll_task(void *pvParameters)
{
    (void)pvParameters;
    char player_key[37];
    
    // Get player_key from store
    if (makapix_store_get_player_key(player_key, sizeof(player_key)) != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to get player_key for credential polling");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGD(MAKAPIX_TAG, "Starting credential polling for player_key: %s", player_key);

    makapix_credentials_result_t creds;
    int poll_count = 0;
    const int max_polls = 300; // 300 * 3 seconds = 15 minutes (registration code expiry)

    while (s_makapix_state == MAKAPIX_STATE_SHOW_CODE && poll_count < max_polls) {
        vTaskDelay(pdMS_TO_TICKS(3000)); // Poll every 3 seconds
        
        if (s_provisioning_cancelled) {
            ESP_LOGD(MAKAPIX_TAG, "Provisioning cancelled, stopping credential polling");
            break;
        }

        poll_count++;
        ESP_LOGD(MAKAPIX_TAG, "Polling for credentials (attempt %d/%d)...", poll_count, max_polls);

        esp_err_t err = makapix_poll_credentials(player_key, &creds);
        if (err == ESP_OK) {
            ESP_LOGD(MAKAPIX_TAG, "Credentials received! Saving to NVS...");
            
            // Preserve broker info before clearing (from initial provisioning response)
            // The credentials response may not include broker info, so we need to keep what we have
            char preserved_mqtt_host[64] = {0};
            uint16_t preserved_mqtt_port = 0;
            bool has_preserved_broker = false;
            if (makapix_store_get_mqtt_host(preserved_mqtt_host, sizeof(preserved_mqtt_host)) == ESP_OK &&
                makapix_store_get_mqtt_port(&preserved_mqtt_port) == ESP_OK) {
                has_preserved_broker = true;
                ESP_LOGD(MAKAPIX_TAG, "Preserved broker info: %s:%d", preserved_mqtt_host, preserved_mqtt_port);
            }
            
            // Clear old registration data before saving new credentials (only if re-registering)
            // This ensures old data is only cleared upon successful, complete registration
            if (makapix_store_has_player_key() || makapix_store_has_certificates()) {
                ESP_LOGD(MAKAPIX_TAG, "Clearing old registration data before saving new credentials");
                makapix_store_clear();
            }
            
            // Save certificates to SPIFFS
            err = makapix_store_save_certificates(creds.ca_pem, creds.cert_pem, creds.key_pem);
            if (err == ESP_OK) {
                // Determine which broker info to use:
                // 1. Use credentials response if provided
                // 2. Otherwise use preserved broker info from provisioning
                // 3. Fall back to CONFIG values as last resort
                const char *mqtt_host_to_save;
                uint16_t mqtt_port_to_save;
                
                if (strlen(creds.mqtt_host) > 0 && creds.mqtt_port > 0) {
                    mqtt_host_to_save = creds.mqtt_host;
                    mqtt_port_to_save = creds.mqtt_port;
                    ESP_LOGD(MAKAPIX_TAG, "Using broker info from credentials response: %s:%d", mqtt_host_to_save, mqtt_port_to_save);
                } else if (has_preserved_broker) {
                    mqtt_host_to_save = preserved_mqtt_host;
                    mqtt_port_to_save = preserved_mqtt_port;
                    ESP_LOGD(MAKAPIX_TAG, "Using preserved broker info: %s:%d", mqtt_host_to_save, mqtt_port_to_save);
                } else {
                    mqtt_host_to_save = CONFIG_MAKAPIX_CLUB_HOST;
                    mqtt_port_to_save = CONFIG_MAKAPIX_CLUB_MQTT_PORT;
                    ESP_LOGD(MAKAPIX_TAG, "Using CONFIG broker info: %s:%d", mqtt_host_to_save, mqtt_port_to_save);
                }
                
                // Save broker info (player_key is still valid from this task's scope)
                makapix_store_save_credentials(player_key, mqtt_host_to_save, mqtt_port_to_save);
                
                ESP_LOGD(MAKAPIX_TAG, "Certificates saved successfully, initiating MQTT connection");
                s_makapix_state = MAKAPIX_STATE_CONNECTING;
                
                // Update connectivity state - device is now registered
                connectivity_state_on_registration_changed(true);
                
                // Initiate MQTT connection using the determined broker info
                char mqtt_host[64];
                uint16_t mqtt_port;
                snprintf(mqtt_host, sizeof(mqtt_host), "%s", mqtt_host_to_save);
                mqtt_port = mqtt_port_to_save;
                
                // Use certificates directly from creds struct (no need to reload from SPIFFS)
                err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port, 
                                       creds.ca_pem, creds.cert_pem, creds.key_pem);
                if (err == ESP_OK) {
                    err = makapix_mqtt_connect();
                    if (err != ESP_OK) {
                        ESP_LOGE(MAKAPIX_TAG, "MQTT connect failed: %s", esp_err_to_name(err));
                        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                    }
                } else {
                    ESP_LOGE(MAKAPIX_TAG, "MQTT init failed: %s", esp_err_to_name(err));
                    s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
                }
                
                break; // Exit polling task
            } else {
                ESP_LOGE(MAKAPIX_TAG, "Failed to save certificates: %s", esp_err_to_name(err));
                // Continue polling in case of transient error
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            // Registration not complete yet - continue polling
            ESP_LOGD(MAKAPIX_TAG, "Credentials not ready yet (404), continuing to poll...");
        } else {
            ESP_LOGW(MAKAPIX_TAG, "Credential polling error: %s, will retry", esp_err_to_name(err));
            // Continue polling on error
        }
    }

    if (poll_count >= max_polls) {
        ESP_LOGW(MAKAPIX_TAG, "Credential polling timed out after %d attempts", max_polls);
        s_makapix_state = MAKAPIX_STATE_IDLE;
    }

    ESP_LOGD(MAKAPIX_TAG, "Credential polling task exiting");
    s_poll_task_handle = NULL;
    vTaskDelete(NULL);
}

