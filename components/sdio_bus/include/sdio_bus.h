/**
 * @file sdio_bus.h
 * @brief SDIO Bus Coordinator for ESP32-P4
 * 
 * The ESP32-P4 shares the SDMMC controller between WiFi (SDIO Slot 1 via ESP-Hosted)
 * and SD card (SDMMC Slot 0). Simultaneous high-bandwidth operations on both slots
 * can cause "SDIO slave unresponsive" crashes.
 * 
 * This module provides a coordination mechanism to ensure exclusive access during
 * critical operations like OTA updates that require sustained WiFi traffic.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the SDIO bus coordinator
 * 
 * Must be called once during system initialization, before any SD card or
 * WiFi operations that might conflict.
 * 
 * @return ESP_OK on success, ESP_ERR_NO_MEM if mutex creation fails
 */
esp_err_t sdio_bus_init(void);

/**
 * @brief Acquire exclusive SDIO bus access for WiFi-heavy operations
 * 
 * Call this before operations that require sustained high-bandwidth WiFi traffic
 * (e.g., OTA updates, large HTTPS downloads). While acquired, SD card operations
 * should be avoided to prevent bus contention.
 * 
 * @param timeout_ms Maximum time to wait for acquisition (0 = no wait, portMAX_DELAY for forever)
 * @param requester Tag for logging (e.g., "OTA", "HTTPS_DOWNLOAD")
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if lock not acquired within timeout
 */
esp_err_t sdio_bus_acquire(uint32_t timeout_ms, const char *requester);

/**
 * @brief Release SDIO bus access
 * 
 * Must be called after the WiFi-heavy operation completes to allow SD card
 * operations to resume.
 */
void sdio_bus_release(void);

/**
 * @brief Check if SDIO bus is currently locked (non-blocking)
 * 
 * Use this to check if SD card operations should be skipped/deferred.
 * 
 * @return true if bus is locked by another operation, false if available
 */
bool sdio_bus_is_locked(void);

/**
 * @brief Get the current lock holder's tag (for debugging)
 * 
 * @return Tag string of current holder, or NULL if not locked
 */
const char *sdio_bus_get_holder(void);

#ifdef __cplusplus
}
#endif

