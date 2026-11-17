// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file slave_ota.h
 * @brief ESP32-C6 Co-processor OTA Update via ESP-Hosted
 * 
 * This component updates the ESP32-C6 co-processor firmware using
 * ESP-Hosted transport OTA. The slave firmware is stored in a
 * dedicated partition and flashed to the C6 at boot if needed.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check and update ESP32-C6 co-processor firmware if needed
 * 
 * This function should be called early in boot, after ESP-Hosted
 * transport is initialized. It:
 * 1. Gets the current co-processor firmware version
 * 2. Compares with the embedded firmware version
 * 3. Performs OTA update if versions differ
 * 
 * @return ESP_OK on success (no update needed or update completed)
 *         ESP_FAIL if update failed
 *         ESP_ERR_NOT_FOUND if slave firmware partition not found
 */
esp_err_t slave_ota_check_and_update(void);

/**
 * @brief Get the embedded slave firmware version
 * 
 * @param[out] major Major version number
 * @param[out] minor Minor version number  
 * @param[out] patch Patch version number
 * @return ESP_OK on success
 */
esp_err_t slave_ota_get_embedded_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

/**
 * @brief Get the slave firmware version actually running on the C6
 *
 * Returns the version the co-processor reported when queried during
 * slave_ota_check_and_update at boot. Never falls back to the embedded
 * version — on a device where the OTA cannot run (e.g. the 2.7.0 lock,
 * or a USB-gated skip) the two legitimately differ.
 *
 * @param[out] major Major version number
 * @param[out] minor Minor version number
 * @param[out] patch Patch version number
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_STATE if not queried yet, the query failed,
 *         or the C6 reported 0.0.0
 */
esp_err_t slave_ota_get_running_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

/**
 * @brief Whether the running slave firmware is OTA-locked
 *
 * True when the C6 runs esp_hosted 2.7.0, whose confirmed OTA bug
 * prevents any upgrade (https://github.com/espressif/esp-hosted-mcu/issues/143).
 *
 * @return true if the running version cannot be updated
 */
bool slave_ota_is_version_locked(void);

/**
 * @brief Query whether a slave OTA is currently in progress
 *
 * Set true once slave_ota_check_and_update commits to running the OTA
 * (after the USB gate clears, before any UI is shown). Cleared again
 * only on the failure path; on success the device reboots, so callers
 * never see this go false again from inside that boot.
 *
 * Used by input-arbitration sites (BOOT button, HTTP command queue,
 * MQTT command handler, channel/swap_to POST handlers) to silently
 * ignore user input while the OTA owns the screen.
 *
 * @return true if a slave OTA is currently running
 */
bool slave_ota_is_in_progress(void);

/**
 * @brief Callback type for the USB-busy gate
 *
 * Should return true when a USB-HS cable is plugged in and USB-MSC is
 * (or may be) exposing the SD card to a host — i.e. when running the
 * slave OTA would contend with USB-MSC for the shared SDIO bus.
 */
typedef bool (*slave_ota_usb_busy_fn_t)(void);

/**
 * @brief Register the USB-busy predicate used by the OTA gate
 *
 * Decouples slave_ota from the TinyUSB / app_usb modules. Call this
 * from app_main before invoking slave_ota_check_and_update with a
 * function like app_usb_is_stream_active (which evaluates
 * "s_usb_active && tud_ready()"). If no predicate is registered, the
 * OTA proceeds — the absence of a check is treated as "USB not busy".
 */
void slave_ota_set_usb_busy_check(slave_ota_usb_busy_fn_t fn);

#if CONFIG_P3A_SLAVE_OTA_MOCK
/**
 * @brief Mock slave OTA — exercises the UX flow without touching the C6
 *
 * Walks through ui_enter / per-percent ui_update / verifying / success
 * countdown / reboot, but never calls esp_hosted_slave_ota_*. Used to
 * verify screen-owning behavior, input lockout, and the USB gate on a
 * device without performing a real OTA. Enabled via Kconfig.
 *
 * Only present in the build when CONFIG_P3A_SLAVE_OTA_MOCK=y, so this
 * prototype and its callsite vanish when the flag is off.
 *
 * @return ESP_OK (does not actually return — calls esp_restart on success)
 */
esp_err_t slave_ota_run_mock(void);
#endif

#ifdef __cplusplus
}
#endif

