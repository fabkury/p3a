#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize MQTT client with mTLS authentication
 * 
 * Must be called before connect. Sets up client configuration with mutual TLS.
 * Authentication is via client certificate (no username/password needed).
 * 
 * @param player_key UUID string for building topics and client ID
 * @param host MQTT broker hostname
 * @param port MQTT broker port (typically 8883 for mTLS)
 * @param ca_cert CA certificate PEM string for server verification
 * @param client_cert Client certificate PEM string for authentication
 * @param client_key Client private key PEM string for authentication
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_mqtt_init(const char *player_key, const char *host, uint16_t port,
                            const char *ca_cert, const char *client_cert, const char *client_key);

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
 * @brief Deinitialize and destroy MQTT client
 * 
 * Stops the client, destroys it, and frees all resources.
 * Should be called before reinitializing with new configuration.
 */
void makapix_mqtt_deinit(void);

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

/**
 * @brief Set callback function for connection state changes
 * 
 * @param cb Callback function: void callback(bool connected)
 */
void makapix_mqtt_set_connection_callback(void (*cb)(bool connected));

/**
 * @brief Set callback for response messages (makapix/player/{player_key}/response/#)
 * 
 * @param cb Callback: void cb(const char *topic, const char *data, int data_len)
 */
void makapix_mqtt_set_response_callback(void (*cb)(const char *topic, const char *data, int data_len));

/**
 * @brief Publish raw payload to a topic with QoS
 * 
 * @param topic Topic string
 * @param payload Null-terminated payload (JSON)
 * @param qos QoS level (0 or 1)
 * @return ESP_OK on success
 */
esp_err_t makapix_mqtt_publish_raw(const char *topic, const char *payload, int qos);

/**
 * @brief Subscribe to a topic
 * 
 * @param topic Topic string (may include wildcards)
 * @param qos QoS level
 * @return ESP_OK on success
 */
esp_err_t makapix_mqtt_subscribe(const char *topic, int qos);

