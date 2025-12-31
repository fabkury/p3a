// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"

/**
 * @brief Provisioning result structure
 */
typedef struct {
    char player_key[37];           // UUID string (36 chars + null)
    char registration_code[7];      // 6 chars + null
    char expires_at[32];           // ISO 8601 timestamp
    char mqtt_host[64];            // Broker hostname
    uint16_t mqtt_port;             // Broker port
} makapix_provision_result_t;

/**
 * @brief Credentials result structure
 */
typedef struct {
    char ca_pem[4096];      // CA certificate
    char cert_pem[4096];    // Client certificate  
    char key_pem[4096];     // Client private key
    char mqtt_host[64];
    uint16_t mqtt_port;
} makapix_credentials_result_t;

/**
 * @brief Request provisioning from Makapix Club API
 * 
 * Sends POST request to the provisioning endpoint with device model and firmware version.
 * 
 * @param result Pointer to structure to receive provisioning details
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_provision_request(makapix_provision_result_t *result);

/**
 * @brief Poll for TLS credentials after registration
 * 
 * Polls GET /api/player/{player_key}/credentials endpoint. Returns ESP_OK when
 * credentials are available, ESP_ERR_NOT_FOUND if registration not complete yet.
 * 
 * @param player_key The player_key UUID received during provisioning
 * @param result Pointer to structure to receive credentials
 * @return ESP_OK when credentials ready, ESP_ERR_NOT_FOUND if still pending, error code otherwise
 */
esp_err_t makapix_poll_credentials(const char *player_key, makapix_credentials_result_t *result);

