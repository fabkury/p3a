#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize the Makapix store module
 * 
 * Must be called before any other makapix_store functions.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_store_init(void);

/**
 * @brief Check if a player_key is stored
 * 
 * @return true if player_key exists, false otherwise
 */
bool makapix_store_has_player_key(void);

/**
 * @brief Get the stored player_key
 * 
 * @param out_key Buffer to receive the UUID string (must be at least 37 bytes)
 * @param max_len Maximum length of out_key buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no key stored, error code otherwise
 */
esp_err_t makapix_store_get_player_key(char *out_key, size_t max_len);

/**
 * @brief Get the stored MQTT broker hostname
 * 
 * @param out_host Buffer to receive the hostname (must be at least 64 bytes)
 * @param max_len Maximum length of out_host buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored, error code otherwise
 */
esp_err_t makapix_store_get_mqtt_host(char *out_host, size_t max_len);

/**
 * @brief Get the stored MQTT broker port
 * 
 * @param out_port Pointer to receive the port number
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored, error code otherwise
 */
esp_err_t makapix_store_get_mqtt_port(uint16_t *out_port);

/**
 * @brief Save Makapix credentials to NVS
 * 
 * @param player_key UUID string (36 chars + null terminator)
 * @param host MQTT broker hostname
 * @param port MQTT broker port
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_store_save_credentials(const char *player_key, const char *host, uint16_t port);

/**
 * @brief Check if TLS certificates are stored in SPIFFS
 * 
 * @return true if all certificates exist, false otherwise
 */
bool makapix_store_has_certificates(void);

/**
 * @brief Save TLS certificates to SPIFFS
 * 
 * Saves CA certificate, client certificate, and client private key to SPIFFS filesystem.
 * 
 * @param ca_pem CA certificate PEM string
 * @param cert_pem Client certificate PEM string
 * @param key_pem Client private key PEM string
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_store_save_certificates(const char *ca_pem, const char *cert_pem, const char *key_pem);

/**
 * @brief Get CA certificate from SPIFFS
 * 
 * @param buffer Buffer to receive certificate (must be at least max_len bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored, error code otherwise
 */
esp_err_t makapix_store_get_ca_cert(char *buffer, size_t max_len);

/**
 * @brief Get client certificate from SPIFFS
 * 
 * @param buffer Buffer to receive certificate (must be at least max_len bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored, error code otherwise
 */
esp_err_t makapix_store_get_client_cert(char *buffer, size_t max_len);

/**
 * @brief Get client private key from SPIFFS
 * 
 * @param buffer Buffer to receive private key (must be at least max_len bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored, error code otherwise
 */
esp_err_t makapix_store_get_client_key(char *buffer, size_t max_len);

/**
 * @brief Clear all stored Makapix credentials
 * 
 * Clears NVS credentials and deletes certificate files from SPIFFS.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_store_clear(void);

