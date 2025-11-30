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
 * @brief Request provisioning from Makapix Club API
 * 
 * Sends POST request to the provisioning endpoint with device model and firmware version.
 * 
 * @param result Pointer to structure to receive provisioning details
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_provision_request(makapix_provision_result_t *result);

