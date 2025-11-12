#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Start HTTP API server and mDNS
 * 
 * Initializes mDNS with hostname "p3a", starts HTTP server on port 80,
 * creates command queue and worker task, and registers all REST endpoints.
 * 
 * Should be called after Wi-Fi STA has obtained an IP address.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t http_api_start(void);

/**
 * @brief Stop HTTP API server
 * 
 * Stops the HTTP server. Worker task and queue remain active.
 * 
 * @return ESP_OK on success
 */
esp_err_t http_api_stop(void);

/**
 * @brief Enqueue reboot command
 * 
 * @return true if queued successfully, false if queue full
 */
bool api_enqueue_reboot(void);

/**
 * @brief Enqueue swap_next command
 * 
 * @return true if queued successfully, false if queue full
 */
bool api_enqueue_swap_next(void);

/**
 * @brief Enqueue swap_back command
 * 
 * @return true if queued successfully, false if queue full
 */
bool api_enqueue_swap_back(void);

