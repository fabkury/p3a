#include "swap_future.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "swap_future";

// Global state for swap_future system
static struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    swap_future_t pending;
} s_swap_future = {0};

esp_err_t swap_future_init(void)
{
    if (s_swap_future.initialized) {
        ESP_LOGW(TAG, "swap_future already initialized");
        return ESP_OK;
    }
    
    // Create mutex for thread-safe access
    s_swap_future.mutex = xSemaphoreCreateMutex();
    if (!s_swap_future.mutex) {
        ESP_LOGE(TAG, "Failed to create swap_future mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize pending swap as invalid
    memset(&s_swap_future.pending, 0, sizeof(swap_future_t));
    s_swap_future.pending.valid = false;
    
    s_swap_future.initialized = true;
    ESP_LOGI(TAG, "swap_future system initialized");
    
    return ESP_OK;
}

void swap_future_deinit(void)
{
    if (!s_swap_future.initialized) {
        return;
    }
    
    if (s_swap_future.mutex) {
        vSemaphoreDelete(s_swap_future.mutex);
        s_swap_future.mutex = NULL;
    }
    
    memset(&s_swap_future.pending, 0, sizeof(swap_future_t));
    s_swap_future.initialized = false;
    
    ESP_LOGI(TAG, "swap_future system deinitialized");
}

esp_err_t swap_future_schedule(const swap_future_t *swap)
{
    if (!s_swap_future.initialized) {
        ESP_LOGE(TAG, "swap_future system not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!swap) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for scheduling");
        return ESP_ERR_TIMEOUT;
    }
    
    // Copy the swap_future
    memcpy(&s_swap_future.pending, swap, sizeof(swap_future_t));
    s_swap_future.pending.valid = true;
    
    ESP_LOGI(TAG, "Scheduled swap_future: target=%llu ms, frame=%u, live=%d, auto=%d",
             swap->target_time_ms, swap->start_frame, swap->is_live_mode_swap, swap->is_automated);
    
    xSemaphoreGive(s_swap_future.mutex);
    
    return ESP_OK;
}

void swap_future_cancel(void)
{
    if (!s_swap_future.initialized) {
        return;
    }
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for cancellation");
        return;
    }
    
    if (s_swap_future.pending.valid) {
        ESP_LOGI(TAG, "Cancelled pending swap_future");
        s_swap_future.pending.valid = false;
    }
    
    xSemaphoreGive(s_swap_future.mutex);
}

bool swap_future_is_ready(uint64_t current_time_ms, swap_future_t *out_swap)
{
    if (!s_swap_future.initialized) {
        return false;
    }
    
    bool ready = false;
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_swap_future.pending.valid && 
            current_time_ms >= s_swap_future.pending.target_time_ms) {
            ready = true;
            
            if (out_swap) {
                memcpy(out_swap, &s_swap_future.pending, sizeof(swap_future_t));
            }
        }
        
        xSemaphoreGive(s_swap_future.mutex);
    }
    
    return ready;
}

esp_err_t swap_future_get_pending(swap_future_t *out_swap)
{
    if (!s_swap_future.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!out_swap) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t result = ESP_ERR_NOT_FOUND;
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_swap_future.pending.valid) {
            memcpy(out_swap, &s_swap_future.pending, sizeof(swap_future_t));
            result = ESP_OK;
        }
        
        xSemaphoreGive(s_swap_future.mutex);
    }
    
    return result;
}

bool swap_future_has_pending(void)
{
    if (!s_swap_future.initialized) {
        return false;
    }
    
    bool has_pending = false;
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        has_pending = s_swap_future.pending.valid;
        xSemaphoreGive(s_swap_future.mutex);
    }
    
    return has_pending;
}
