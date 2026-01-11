// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize MQTT client with mTLS authentication
 * 
 * Must be called before connect. Sets up client configuration with mutual TLS.
 * Authentication uses both mTLS (client certificate) and username (player_key).
 * 
 * @param player_key UUID string for building topics, client ID, and MQTT username
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
 * @brief Check if MQTT client is ready to send/receive requests
 * 
 * Ready means connected AND response topic subscription has been confirmed.
 * Use this before sending API requests to ensure responses can be received.
 * 
 * @return true if ready to send requests, false otherwise
 */
bool makapix_mqtt_is_ready(void);

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
 * Ownership contract:
 * - `data` is heap-allocated and NUL-terminated.
 * - The callback takes ownership of `data` and MUST free it (or transfer ownership)
 *   once done.
 *
 * @param cb Callback: void cb(const char *topic, char *data, int data_len)
 */
void makapix_mqtt_set_response_callback(void (*cb)(const char *topic, char *data, int data_len));

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

/**
 * @brief Publish view event
 * 
 * Sends a view event to the Makapix Club server with metadata about the artwork viewing.
 * 
 * Topic: makapix/player/{player_key}/view
 * 
 * Payload includes:
 * - post_id: Artwork post ID
 * - timestamp: ISO 8601 UTC timestamp
 * - timezone: Empty string (reserved for future use)
 * - intent: "artwork" (show_artwork command) or "channel" (channel playback)
 * - play_order: 0=server, 1=created, 2=random
 * - channel: Channel name (e.g., "promoted", "all", "by_user", "hashtag", "artwork")
 * - player_key: Device player key
 * - channel_user_sqid: User sqid (for "by_user" channel) or NULL
 * - channel_hashtag: Hashtag without # (for "hashtag" channel) or NULL
 * - request_ack: Whether to request acknowledgment from server
 * 
 * @param post_id Post ID of the artwork
 * @param intent "artwork" or "channel"
 * @param play_order Play order mode (0-2)
 * @param channel_name Channel name string
 * @param player_key Player key UUID string
 * @param channel_user_sqid User sqid for "by_user" channel, NULL otherwise
 * @param channel_hashtag Hashtag (without #) for "hashtag" channel, NULL otherwise
 * @param request_ack Whether to request acknowledgment
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_mqtt_publish_view(int32_t post_id, const char *intent,
                                     uint8_t play_order, const char *channel_name,
                                     const char *player_key, const char *channel_user_sqid,
                                     const char *channel_hashtag, bool request_ack);

/**
 * @brief Get count of consecutive TLS authentication failures
 *
 * Used to detect when server is persistently rejecting client certificate,
 * indicating invalid/revoked registration.
 *
 * @return Number of consecutive TLS auth failures since last success
 */
int makapix_mqtt_get_auth_failure_count(void);

/**
 * @brief Reset TLS authentication failure counter
 *
 * Call when starting fresh provisioning to clear previous failure state.
 */
void makapix_mqtt_reset_auth_failure_count(void);

