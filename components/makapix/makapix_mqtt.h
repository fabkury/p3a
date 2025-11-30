#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize MQTT client
 * 
 * Must be called before connect. Sets up client configuration.
 * 
 * @param player_key UUID string to use as MQTT username
 * @param host MQTT broker hostname
 * @param port MQTT broker port (typically 8883 for TLS)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_mqtt_init(const char *player_key, const char *host, uint16_t port);

/**
 * @brief Connect to MQTT broker
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_mqtt_connect(void);

/**
 * @brief Disconnect from MQTT broker
 */
void makapix_mqtt_disconnect(void);

/**
 * @brief Check if MQTT client is connected
 * 
 * @return true if connected, false otherwise
 */
bool makapix_mqtt_is_connected(void);

/**
 * @brief Publish status message to broker
 * 
 * @param current_post_id Post ID of currently displayed artwork (0 for local animations)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_mqtt_publish_status(int32_t current_post_id);

/**
 * @brief Set callback function for received commands
 * 
 * @param cb Callback function: void callback(const char *command_type, cJSON *payload)
 */
void makapix_mqtt_set_command_callback(void (*cb)(const char *command_type, cJSON *payload));

