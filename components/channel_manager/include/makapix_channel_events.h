// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_channel_events.h
 * @brief MQTT connection event signaling for Makapix channel refresh tasks
 * 
 * This module provides event-based coordination between MQTT connection state
 * and channel refresh tasks, ensuring refresh operations only occur when
 * MQTT is connected.
 */

#ifndef MAKAPIX_CHANNEL_EVENTS_H
#define MAKAPIX_CHANNEL_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Event bits
#define MAKAPIX_EVENT_MQTT_CONNECTED    (1 << 0)
#define MAKAPIX_EVENT_MQTT_DISCONNECTED (1 << 1)
#define MAKAPIX_EVENT_WIFI_CONNECTED    (1 << 2)
#define MAKAPIX_EVENT_WIFI_DISCONNECTED (1 << 3)
#define MAKAPIX_EVENT_REFRESH_DONE      (1 << 4)
#define MAKAPIX_EVENT_SD_AVAILABLE      (1 << 5)
#define MAKAPIX_EVENT_SD_UNAVAILABLE    (1 << 6)
#define MAKAPIX_EVENT_DOWNLOADS_NEEDED  (1 << 7)
#define MAKAPIX_EVENT_FILE_AVAILABLE    (1 << 8)
#define MAKAPIX_EVENT_REFRESH_SHUTDOWN  (1 << 9)
#define MAKAPIX_EVENT_PS_CHANNEL_REFRESH_DONE (1 << 10)  // Play Scheduler channel refresh complete

/**
 * @brief Initialize the Makapix channel events system
 * 
 * Must be called before any other functions in this module.
 * Creates the event group used for MQTT state signaling.
 */
void makapix_channel_events_init(void);

/**
 * @brief Deinitialize the events system
 */
void makapix_channel_events_deinit(void);

/**
 * @brief Signal that MQTT has connected
 * 
 * Wakes up all refresh tasks waiting for MQTT connection.
 * Called by the MQTT connection callback.
 */
void makapix_channel_signal_mqtt_connected(void);

/**
 * @brief Signal that MQTT has disconnected
 * 
 * Clears the connected flag so refresh tasks will wait on reconnection.
 */
void makapix_channel_signal_mqtt_disconnected(void);

/**
 * @brief Wait for MQTT connection with timeout
 * 
 * Blocks the calling task until MQTT is connected or timeout expires.
 * 
 * @param timeout_ms Timeout in milliseconds (use portMAX_DELAY for infinite wait)
 * @return true if MQTT connected, false if timeout or interrupted
 */
bool makapix_channel_wait_for_mqtt(uint32_t timeout_ms);

/**
 * @brief Check if MQTT is currently ready (non-blocking)
 *
 * @return true if MQTT is connected, false otherwise
 */
bool makapix_channel_is_mqtt_ready(void);

/**
 * @brief Wait for MQTT connection OR shutdown signal
 *
 * Blocks until MQTT is connected, shutdown is signaled, or timeout expires.
 * Used by refresh tasks to allow interruptible waits.
 *
 * @param timeout_ms Timeout in milliseconds (use portMAX_DELAY for infinite wait)
 * @return true if MQTT connected, false if shutdown or timeout
 */
bool makapix_channel_wait_for_mqtt_or_shutdown(uint32_t timeout_ms);

/**
 * @brief Signal refresh tasks to shutdown
 *
 * Wakes all refresh tasks waiting on MQTT so they can check their shutdown flag.
 */
void makapix_channel_signal_refresh_shutdown(void);

/**
 * @brief Clear the shutdown signal
 *
 * Called after task has exited to reset for next channel.
 */
void makapix_channel_clear_refresh_shutdown(void);

/**
 * @brief Signal that WiFi has connected and got IP
 * 
 * Wakes up all download tasks waiting for WiFi connection.
 * Called by the WiFi event handler when IP is obtained.
 */
void makapix_channel_signal_wifi_connected(void);

/**
 * @brief Signal that WiFi has disconnected
 * 
 * Clears the connected flag so download tasks will wait on reconnection.
 */
void makapix_channel_signal_wifi_disconnected(void);

/**
 * @brief Wait for WiFi connection with timeout
 * 
 * Blocks the calling task until WiFi is connected (has IP) or timeout expires.
 * 
 * @param timeout_ms Timeout in milliseconds (use portMAX_DELAY for infinite wait)
 * @return true if WiFi connected, false if timeout or interrupted
 */
bool makapix_channel_wait_for_wifi(uint32_t timeout_ms);

/**
 * @brief Check if WiFi is currently ready (non-blocking)
 * 
 * @return true if WiFi is connected and has IP, false otherwise
 */
bool makapix_channel_is_wifi_ready(void);

/**
 * @brief Signal that a channel refresh has completed
 * 
 * Wakes up download tasks waiting for refresh to complete.
 */
void makapix_channel_signal_refresh_done(void);

/**
 * @brief Reset the refresh done flag (for new channel)
 */
void makapix_channel_reset_refresh_done(void);

/**
 * @brief Wait for channel refresh to complete with timeout
 * 
 * @param timeout_ms Timeout in milliseconds (use portMAX_DELAY for infinite wait)
 * @return true if refresh completed, false if timeout
 */
bool makapix_channel_wait_for_refresh(uint32_t timeout_ms);

/**
 * @brief Check if channel refresh has completed (non-blocking)
 * 
 * @return true if refresh has completed at least once, false otherwise
 */
bool makapix_channel_is_refresh_done(void);

/**
 * @brief Signal that SD card is available for use
 * 
 * Called when SD card is returned to local control (USB export ended).
 * Wakes up download tasks waiting for SD access.
 */
void makapix_channel_signal_sd_available(void);

/**
 * @brief Signal that SD card is unavailable (exported over USB)
 * 
 * Called when SD card is exported to USB host.
 * Download tasks will pause and wait until SD becomes available again.
 */
void makapix_channel_signal_sd_unavailable(void);

/**
 * @brief Wait for SD card to become available with timeout
 * 
 * Blocks the calling task until SD card is available or timeout expires.
 * 
 * @param timeout_ms Timeout in milliseconds (use portMAX_DELAY for infinite wait)
 * @return true if SD available, false if timeout
 */
bool makapix_channel_wait_for_sd(uint32_t timeout_ms);

/**
 * @brief Check if SD card is currently available (non-blocking)
 * 
 * @return true if SD card is available, false if exported over USB
 */
bool makapix_channel_is_sd_available(void);

/**
 * @brief Signal that downloads may be needed
 * 
 * Called after channel refresh or when new files become available.
 * Wakes the download task to check for files to download.
 */
void makapix_channel_signal_downloads_needed(void);

/**
 * @brief Wait for download work to become available
 * 
 * Blocks until signaled that downloads may be needed.
 * 
 * @param timeout_ms Timeout in milliseconds (use portMAX_DELAY for infinite wait)
 * @return true if signaled, false if timeout
 */
bool makapix_channel_wait_for_downloads_needed(uint32_t timeout_ms);

/**
 * @brief Clear the downloads needed flag
 * 
 * Called after checking for work and finding nothing to do.
 */
void makapix_channel_clear_downloads_needed(void);

/**
 * @brief Signal that a file became available (download completed)
 * 
 * Called by download_manager when an artwork download finishes.
 * This wakes up any tasks waiting for the first file to become available.
 */
void makapix_channel_signal_file_available(void);

/**
 * @brief Wait for a file to become available
 * 
 * Blocks until any of these conditions occurs:
 * - A file download completes (MAKAPIX_EVENT_FILE_AVAILABLE)
 * - Channel refresh completes (MAKAPIX_EVENT_REFRESH_DONE)
 * - Timeout expires
 * 
 * This is useful when waiting for the first artwork in a channel.
 * 
 * @param timeout_ms Timeout in milliseconds (use portMAX_DELAY for infinite wait)
 * @return true if signaled, false if timeout
 */
bool makapix_channel_wait_for_file_available(uint32_t timeout_ms);

/**
 * @brief Clear the file available flag
 *
 * Called before starting to wait for a new file.
 */
void makapix_channel_clear_file_available(void);

/**
 * @brief Signal that a Play Scheduler channel refresh has completed
 *
 * Called from makapix.c when a PS-registered refresh finishes.
 * Wakes the Play Scheduler refresh task to check completion status.
 *
 * @param channel_id Channel ID that completed (for logging)
 */
void makapix_channel_signal_ps_refresh_done(const char *channel_id);

/**
 * @brief Wait for a Play Scheduler channel refresh to complete
 *
 * @param timeout_ms Timeout in milliseconds (0 for non-blocking poll)
 * @return true if signaled, false if timeout
 */
bool makapix_channel_wait_for_ps_refresh_done(uint32_t timeout_ms);

/**
 * @brief Clear the PS refresh done flag
 */
void makapix_channel_clear_ps_refresh_done(void);

#ifdef __cplusplus
}
#endif

#endif // MAKAPIX_CHANNEL_EVENTS_H

