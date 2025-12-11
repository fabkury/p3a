/**
 * @file sdio_bus.c
 * @brief SDIO Bus Coordinator implementation
 */

#include "sdio_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "sdio_bus";

static SemaphoreHandle_t s_bus_mutex = NULL;
static volatile bool s_initialized = false;
static volatile const char *s_current_holder = NULL;

esp_err_t sdio_bus_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    s_bus_mutex = xSemaphoreCreateMutex();
    if (!s_bus_mutex) {
        ESP_LOGE(TAG, "Failed to create SDIO bus mutex");
        return ESP_ERR_NO_MEM;
    }
    
    s_initialized = true;
    s_current_holder = NULL;
    
    ESP_LOGI(TAG, "SDIO bus coordinator initialized");
    return ESP_OK;
}

esp_err_t sdio_bus_acquire(uint32_t timeout_ms, const char *requester)
{
    if (!s_initialized || !s_bus_mutex) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    const char *tag = requester ? requester : "UNKNOWN";
    
    // Check if already held
    if (s_current_holder != NULL) {
        ESP_LOGI(TAG, "[%s] Waiting for SDIO bus (held by %s)...", tag, s_current_holder);
    }
    
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    if (xSemaphoreTake(s_bus_mutex, ticks) != pdTRUE) {
        ESP_LOGW(TAG, "[%s] Failed to acquire SDIO bus (timeout after %lu ms, held by %s)", 
                 tag, (unsigned long)timeout_ms, s_current_holder ? s_current_holder : "unknown");
        return ESP_ERR_TIMEOUT;
    }
    
    s_current_holder = tag;
    ESP_LOGI(TAG, "[%s] SDIO bus acquired", tag);
    
    return ESP_OK;
}

void sdio_bus_release(void)
{
    if (!s_initialized || !s_bus_mutex) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }
    
    const char *holder = s_current_holder;
    s_current_holder = NULL;
    
    xSemaphoreGive(s_bus_mutex);
    
    ESP_LOGI(TAG, "[%s] SDIO bus released", holder ? holder : "UNKNOWN");
}

bool sdio_bus_is_locked(void)
{
    if (!s_initialized || !s_bus_mutex) {
        return false;
    }
    
    // Try to take with zero timeout - if we get it, release immediately and return false
    // If we don't get it, the bus is locked
    if (xSemaphoreTake(s_bus_mutex, 0) == pdTRUE) {
        xSemaphoreGive(s_bus_mutex);
        return false;
    }
    
    return true;
}

const char *sdio_bus_get_holder(void)
{
    return s_current_holder;
}

