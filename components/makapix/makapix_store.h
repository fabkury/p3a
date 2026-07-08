// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_store.h
 * @brief Makapix credential storage interface (NVS-backed)
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Maximum size of a single PEM item (cert or private key), incl. null terminator
 *
 * Single source of truth for the three layers that must agree: the
 * provisioning response parser (makapix_credentials_result_t), the NVS
 * read-back buffers in makapix.c, and the MQTT reconnect path
 * (mqtt_certs_t). A PEM larger than this is rejected at acquisition, so
 * NVS never holds an item the read paths cannot load.
 *
 * Measured server PEMs as of 2026-06-05: 1823/1514/1705 bytes (CA/cert/key),
 * so 4096 gives ~2x headroom.
 *
 * NOTE: the credential-polling task stack (makapix_provision_flow.c) holds
 * one makapix_credentials_result_t (3x this constant); revisit that stack
 * size before raising this value.
 */
#define MAKAPIX_PEM_MAX_LEN 4096

/**
 * @brief Maximum size of the device HTTPS bearer token, incl. null terminator
 *
 * Server tokens are "mpx_live_" + token_urlsafe(32) ≈ 52 chars today; 128
 * leaves room for the server to grow the entropy without a firmware change.
 */
#define MAKAPIX_API_TOKEN_MAX_LEN 128

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
 * @brief Persist a complete Makapix registration to NVS in one transaction
 *
 * Writes player_key, MQTT broker info, and all three TLS certificates under a
 * single NVS handle with a single commit. Existing values are overwritten in
 * place, so a prior registration does not need to be cleared first. Writing
 * everything in one transaction minimizes the window where NVS could hold a
 * partial registration (e.g. certificates without a player_key).
 *
 * @param player_key UUID string (36 chars + null terminator)
 * @param host MQTT broker hostname
 * @param port MQTT broker port
 * @param ca_pem CA certificate PEM string
 * @param cert_pem Client certificate PEM string
 * @param key_pem Client private key PEM string
 * @param api_token Device HTTPS bearer token ("mpx_live_..."), or NULL if the
 *                  server did not return one (it is only minted on the very
 *                  first credentials fetch). NULL leaves any stored token
 *                  untouched.
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_store_save_registration(const char *player_key, const char *host, uint16_t port,
                                          const char *ca_pem, const char *cert_pem, const char *key_pem,
                                          const char *api_token);

/**
 * @brief Persist a renewed certificate set to NVS in one transaction
 *
 * Overwrites the CA certificate, client certificate, and client private key
 * under a single NVS handle with a single commit; player_key, broker info and
 * api_token are untouched. Used by device-initiated certificate renewal
 * (make-before-break: the previous certificate remains in use until this
 * commit succeeds, so a power loss mid-save leaves a working credential set).
 *
 * @param ca_pem CA certificate PEM string
 * @param cert_pem Client certificate PEM string
 * @param key_pem Client private key PEM string
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_store_save_renewed_certs(const char *ca_pem, const char *cert_pem, const char *key_pem);

/**
 * @brief Get the stored device HTTPS bearer token
 *
 * @param buffer Buffer to receive the token (MAKAPIX_API_TOKEN_MAX_LEN bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no token stored, error code otherwise
 */
esp_err_t makapix_store_get_api_token(char *buffer, size_t max_len);

/**
 * @brief Persist the device HTTPS bearer token
 *
 * The server revokes the previous token when a new one is issued
 * (token/rotate), so callers must persist the fresh token before relying on
 * it and must never re-rotate just because this write failed — retry the
 * write with the in-RAM token instead.
 *
 * @param token Token string ("mpx_live_...")
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_store_set_api_token(const char *token);

/**
 * @brief Check if TLS certificates are stored in NVS
 *
 * @return true if all certificates exist, false otherwise
 */
bool makapix_store_has_certificates(void);

/**
 * @brief Get CA certificate from NVS
 * 
 * @param buffer Buffer to receive certificate (must be at least max_len bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored, error code otherwise
 */
esp_err_t makapix_store_get_ca_cert(char *buffer, size_t max_len);

/**
 * @brief Get client certificate from NVS
 * 
 * @param buffer Buffer to receive certificate (must be at least max_len bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored, error code otherwise
 */
esp_err_t makapix_store_get_client_cert(char *buffer, size_t max_len);

/**
 * @brief Get client private key from NVS
 * 
 * @param buffer Buffer to receive private key (must be at least max_len bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored, error code otherwise
 */
esp_err_t makapix_store_get_client_key(char *buffer, size_t max_len);

/**
 * @brief Clear all stored Makapix credentials
 * 
 * Clears all NVS credentials including player_key, MQTT broker info, and certificates.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_store_clear(void);

