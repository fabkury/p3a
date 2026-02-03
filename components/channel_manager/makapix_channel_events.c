// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_channel_events.c
 * @brief MQTT connection event signaling implementation
 */

#include "makapix_channel_events.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "makapix_events";

// FreeRTOS event group for MQTT state signaling
static EventGroupHandle_t s_mqtt_event_group = NULL;

void makapix_channel_events_init(void)
{
    if (s_mqtt_event_group) {
        ESP_LOGW(TAG, "Events already initialized");
        return;
    }
    
    s_mqtt_event_group = xEventGroupCreate();
    if (!s_mqtt_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }
    
    // Initially, MQTT and WiFi are disconnected, but SD is available
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_MQTT_CONNECTED | MAKAPIX_EVENT_WIFI_CONNECTED | MAKAPIX_EVENT_SD_UNAVAILABLE);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_MQTT_DISCONNECTED | MAKAPIX_EVENT_WIFI_DISCONNECTED | MAKAPIX_EVENT_SD_AVAILABLE);
    
    ESP_LOGD(TAG, "Event signaling initialized (MQTT + WiFi + SD)");
}

void makapix_channel_events_deinit(void)
{
    if (s_mqtt_event_group) {
        vEventGroupDelete(s_mqtt_event_group);
        s_mqtt_event_group = NULL;
        ESP_LOGD(TAG, "MQTT event signaling deinitialized");
    }
}

void makapix_channel_signal_mqtt_connected(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling MQTT connected - waking refresh tasks");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_MQTT_DISCONNECTED);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_MQTT_CONNECTED);
}

void makapix_channel_signal_mqtt_disconnected(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling MQTT disconnected");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_MQTT_CONNECTED);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_MQTT_DISCONNECTED);
}

bool makapix_channel_wait_for_mqtt(uint32_t timeout_ms)
{
    if (!s_mqtt_event_group) {
        ESP_LOGE(TAG, "Event group not initialized");
        return false;
    }
    
    // Check if already connected
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    if (bits & MAKAPIX_EVENT_MQTT_CONNECTED) {
        return true;
    }
    
    // Wait for connected event
    TickType_t timeout_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MAKAPIX_EVENT_MQTT_CONNECTED,
        pdFALSE,  // Don't clear on exit
        pdFALSE,  // Wait for any bit (only waiting for one)
        timeout_ticks
    );
    
    return (bits & MAKAPIX_EVENT_MQTT_CONNECTED) != 0;
}

bool makapix_channel_is_mqtt_ready(void)
{
    if (!s_mqtt_event_group) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    return (bits & MAKAPIX_EVENT_MQTT_CONNECTED) != 0;
}

bool makapix_channel_wait_for_mqtt_or_shutdown(uint32_t timeout_ms)
{
    if (!s_mqtt_event_group) {
        ESP_LOGE(TAG, "Event group not initialized");
        return false;
    }

    // Check if already connected or shutdown requested
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    if (bits & MAKAPIX_EVENT_MQTT_CONNECTED) {
        return true;
    }
    if (bits & MAKAPIX_EVENT_REFRESH_SHUTDOWN) {
        return false;  // Shutdown requested
    }

    // Wait for connected OR shutdown event
    TickType_t timeout_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MAKAPIX_EVENT_MQTT_CONNECTED | MAKAPIX_EVENT_REFRESH_SHUTDOWN,
        pdFALSE,  // Don't clear on exit
        pdFALSE,  // Wait for any bit
        timeout_ticks
    );

    // Return true only if MQTT connected (not shutdown)
    return (bits & MAKAPIX_EVENT_MQTT_CONNECTED) != 0 &&
           (bits & MAKAPIX_EVENT_REFRESH_SHUTDOWN) == 0;
}

void makapix_channel_signal_refresh_shutdown(void)
{
    if (!s_mqtt_event_group) {
        return;
    }
    ESP_LOGD(TAG, "Signaling refresh shutdown");
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_REFRESH_SHUTDOWN);
}

void makapix_channel_clear_refresh_shutdown(void)
{
    if (!s_mqtt_event_group) {
        return;
    }
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_REFRESH_SHUTDOWN);
}

void makapix_channel_signal_wifi_connected(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling WiFi connected - waking download tasks");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_WIFI_DISCONNECTED);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_WIFI_CONNECTED);
}

void makapix_channel_signal_wifi_disconnected(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling WiFi disconnected");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_WIFI_CONNECTED);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_WIFI_DISCONNECTED);
}

bool makapix_channel_wait_for_wifi(uint32_t timeout_ms)
{
    if (!s_mqtt_event_group) {
        ESP_LOGE(TAG, "Event group not initialized");
        return false;
    }
    
    // Check if already connected
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    if (bits & MAKAPIX_EVENT_WIFI_CONNECTED) {
        return true;
    }
    
    // Wait for connected event
    TickType_t timeout_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MAKAPIX_EVENT_WIFI_CONNECTED,
        pdFALSE,  // Don't clear on exit
        pdFALSE,  // Wait for any bit (only waiting for one)
        timeout_ticks
    );
    
    return (bits & MAKAPIX_EVENT_WIFI_CONNECTED) != 0;
}

bool makapix_channel_is_wifi_ready(void)
{
    if (!s_mqtt_event_group) {
        return false;
    }
    
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    return (bits & MAKAPIX_EVENT_WIFI_CONNECTED) != 0;
}

void makapix_channel_signal_refresh_done(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling channel refresh done - waking download tasks");
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_REFRESH_DONE);
}

void makapix_channel_reset_refresh_done(void)
{
    if (!s_mqtt_event_group) {
        return;
    }
    
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_REFRESH_DONE);
}

bool makapix_channel_wait_for_refresh(uint32_t timeout_ms)
{
    if (!s_mqtt_event_group) {
        ESP_LOGE(TAG, "Event group not initialized");
        return false;
    }
    
    // Check if already done
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    if (bits & MAKAPIX_EVENT_REFRESH_DONE) {
        return true;
    }
    
    // Wait for refresh done event
    TickType_t timeout_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MAKAPIX_EVENT_REFRESH_DONE,
        pdFALSE,  // Don't clear on exit
        pdFALSE,  // Wait for any bit
        timeout_ticks
    );
    
    return (bits & MAKAPIX_EVENT_REFRESH_DONE) != 0;
}

bool makapix_channel_is_refresh_done(void)
{
    if (!s_mqtt_event_group) {
        return false;
    }
    
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    return (bits & MAKAPIX_EVENT_REFRESH_DONE) != 0;
}

void makapix_channel_signal_sd_available(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling SD card available - waking download tasks");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_SD_UNAVAILABLE);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_SD_AVAILABLE);
}

void makapix_channel_signal_sd_unavailable(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling SD card unavailable (USB export) - pausing downloads");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_SD_AVAILABLE);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_SD_UNAVAILABLE);
}

bool makapix_channel_wait_for_sd(uint32_t timeout_ms)
{
    if (!s_mqtt_event_group) {
        ESP_LOGE(TAG, "Event group not initialized");
        return false;
    }
    
    // Check if already available
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    if (bits & MAKAPIX_EVENT_SD_AVAILABLE) {
        return true;
    }
    
    // Wait for SD available event
    TickType_t timeout_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MAKAPIX_EVENT_SD_AVAILABLE,
        pdFALSE,  // Don't clear on exit
        pdFALSE,  // Wait for any bit
        timeout_ticks
    );
    
    return (bits & MAKAPIX_EVENT_SD_AVAILABLE) != 0;
}

bool makapix_channel_is_sd_available(void)
{
    if (!s_mqtt_event_group) {
        return true;  // Assume available if not initialized
    }
    
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    return (bits & MAKAPIX_EVENT_SD_AVAILABLE) != 0;
}

void makapix_channel_signal_downloads_needed(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling downloads needed - waking download task");
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_DOWNLOADS_NEEDED);
}

bool makapix_channel_wait_for_downloads_needed(uint32_t timeout_ms)
{
    if (!s_mqtt_event_group) {
        ESP_LOGE(TAG, "Event group not initialized");
        return false;
    }
    
    // Wait for downloads needed event
    TickType_t timeout_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MAKAPIX_EVENT_DOWNLOADS_NEEDED,
        pdFALSE,  // Don't clear on exit - caller must manually clear after consuming work
        pdFALSE,  // Wait for any bit
        timeout_ticks
    );
    
    return (bits & MAKAPIX_EVENT_DOWNLOADS_NEEDED) != 0;
}

void makapix_channel_clear_downloads_needed(void)
{
    if (!s_mqtt_event_group) {
        return;
    }
    
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_DOWNLOADS_NEEDED);
}

void makapix_channel_signal_file_available(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGD(TAG, "Signaling file available - waking waiting tasks");
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_FILE_AVAILABLE);
}

bool makapix_channel_wait_for_file_available(uint32_t timeout_ms)
{
    if (!s_mqtt_event_group) {
        ESP_LOGE(TAG, "Event group not initialized");
        return false;
    }
    
    // Wait for either file_available or refresh_done (which may add new files)
    TickType_t timeout_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MAKAPIX_EVENT_FILE_AVAILABLE | MAKAPIX_EVENT_REFRESH_DONE,
        pdFALSE,  // Don't clear on exit - let caller handle state
        pdFALSE,  // Wait for any bit
        timeout_ticks
    );
    
    return (bits & (MAKAPIX_EVENT_FILE_AVAILABLE | MAKAPIX_EVENT_REFRESH_DONE)) != 0;
}

void makapix_channel_clear_file_available(void)
{
    if (!s_mqtt_event_group) {
        return;
    }

    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_FILE_AVAILABLE);
}

void makapix_channel_signal_ps_refresh_done(const char *channel_id)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }

    ESP_LOGD(TAG, "Signaling PS channel refresh done: %s", channel_id ? channel_id : "(null)");
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_PS_CHANNEL_REFRESH_DONE);
}

bool makapix_channel_wait_for_ps_refresh_done(uint32_t timeout_ms)
{
    if (!s_mqtt_event_group) {
        return false;
    }

    TickType_t timeout_ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MAKAPIX_EVENT_PS_CHANNEL_REFRESH_DONE,
        pdTRUE,   // Clear on exit (auto-reset)
        pdFALSE,
        timeout_ticks
    );

    return (bits & MAKAPIX_EVENT_PS_CHANNEL_REFRESH_DONE) != 0;
}

void makapix_channel_clear_ps_refresh_done(void)
{
    if (!s_mqtt_event_group) {
        return;
    }

    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_PS_CHANNEL_REFRESH_DONE);
}

void makapix_channel_signal_refresh_immediate(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }

    ESP_LOGI(TAG, "Signaling immediate channel refresh requested");
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_REFRESH_IMMEDIATE);
}bool makapix_channel_check_and_clear_refresh_immediate(void)
{
    if (!s_mqtt_event_group) {
        return false;
    }    // Get current bits and clear the immediate flag atomically
    EventBits_t bits = xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_REFRESH_IMMEDIATE);
    return (bits & MAKAPIX_EVENT_REFRESH_IMMEDIATE) != 0;
}