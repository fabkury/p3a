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
    
    ESP_LOGI(TAG, "Event signaling initialized (MQTT + WiFi + SD)");
}

void makapix_channel_events_deinit(void)
{
    if (s_mqtt_event_group) {
        vEventGroupDelete(s_mqtt_event_group);
        s_mqtt_event_group = NULL;
        ESP_LOGI(TAG, "MQTT event signaling deinitialized");
    }
}

void makapix_channel_signal_mqtt_connected(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Signaling MQTT connected - waking refresh tasks");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_MQTT_DISCONNECTED);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_MQTT_CONNECTED);
}

void makapix_channel_signal_mqtt_disconnected(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Signaling MQTT disconnected");
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

void makapix_channel_signal_wifi_connected(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Signaling WiFi connected - waking download tasks");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_WIFI_DISCONNECTED);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_WIFI_CONNECTED);
}

void makapix_channel_signal_wifi_disconnected(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Signaling WiFi disconnected");
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
    
    ESP_LOGI(TAG, "Signaling channel refresh done - waking download tasks");
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
    
    ESP_LOGI(TAG, "Signaling SD card available - waking download tasks");
    xEventGroupClearBits(s_mqtt_event_group, MAKAPIX_EVENT_SD_UNAVAILABLE);
    xEventGroupSetBits(s_mqtt_event_group, MAKAPIX_EVENT_SD_AVAILABLE);
}

void makapix_channel_signal_sd_unavailable(void)
{
    if (!s_mqtt_event_group) {
        ESP_LOGW(TAG, "Event group not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Signaling SD card unavailable (USB export) - pausing downloads");
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
        pdTRUE,   // Clear on exit (auto-reset)
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

