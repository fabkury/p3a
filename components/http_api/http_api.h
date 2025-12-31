// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Action callback function type
 */
typedef void (*action_callback_t)(void);

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
 * @brief Set action handler callbacks for swap operations
 * 
 * Registers callback functions that will be invoked when swap_next or swap_back
 * commands are processed. These callbacks should perform the actual animation swap.
 * 
 * @param swap_next Callback function for swap_next action (can be NULL)
 * @param swap_back Callback function for swap_back action (can be NULL)
 */
void http_api_set_action_handlers(action_callback_t swap_next, action_callback_t swap_back);

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

