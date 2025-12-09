#include "playback_controller.h"
#include "animation_metadata.h"
#include "pico8_stream.h"
#include "p3a_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "playback_ctrl";

// Internal state
static struct {
    playback_source_t current_source;
    animation_metadata_t current_metadata;
    SemaphoreHandle_t mutex;
    bool initialized;
} s_controller = {0};

esp_err_t playback_controller_init(void)
{
    if (s_controller.initialized) {
        ESP_LOGW(TAG, "Playback controller already initialized");
        return ESP_OK;
    }
    
    // Initialize state
    s_controller.current_source = PLAYBACK_SOURCE_NONE;
    animation_metadata_init(&s_controller.current_metadata);
    
    // Create mutex
    s_controller.mutex = xSemaphoreCreateMutex();
    if (!s_controller.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    s_controller.initialized = true;
    ESP_LOGI(TAG, "Playback controller initialized");
    
    return ESP_OK;
}

void playback_controller_deinit(void)
{
    if (!s_controller.initialized) {
        return;
    }
    
    // Free metadata
    animation_metadata_free(&s_controller.current_metadata);
    
    // Delete mutex
    if (s_controller.mutex) {
        vSemaphoreDelete(s_controller.mutex);
        s_controller.mutex = NULL;
    }
    
    s_controller.current_source = PLAYBACK_SOURCE_NONE;
    s_controller.initialized = false;
    
    ESP_LOGI(TAG, "Playback controller deinitialized");
}

playback_source_t playback_controller_get_source(void)
{
    playback_source_t source = PLAYBACK_SOURCE_NONE;
    
    if (s_controller.initialized && s_controller.mutex) {
        if (xSemaphoreTake(s_controller.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            source = s_controller.current_source;
            xSemaphoreGive(s_controller.mutex);
        } else {
            // If we can't get mutex, read directly (may be slightly stale)
            source = s_controller.current_source;
        }
    }
    
    return source;
}

esp_err_t playback_controller_enter_pico8_mode(void)
{
    if (!s_controller.initialized) {
        ESP_LOGE(TAG, "Controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_controller.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    if (s_controller.current_source == PLAYBACK_SOURCE_PICO8_STREAM) {
        // Already in PICO-8 mode
        xSemaphoreGive(s_controller.mutex);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Entering PICO-8 mode (was: %d)", s_controller.current_source);
    
    // Note: We don't clear animation metadata here - it's preserved for resumption
    s_controller.current_source = PLAYBACK_SOURCE_PICO8_STREAM;
    
    xSemaphoreGive(s_controller.mutex);
    
    // Enter unified p3a PICO-8 streaming state
    esp_err_t state_err = p3a_state_enter_pico8_streaming();
    if (state_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enter p3a PICO-8 state: %s (continuing anyway)", esp_err_to_name(state_err));
    }
    
    // Enter PICO-8 streaming mode
    pico8_stream_enter_mode();
    
    return ESP_OK;
}

void playback_controller_exit_pico8_mode(void)
{
    if (!s_controller.initialized) {
        return;
    }
    
    // Exit PICO-8 streaming mode first
    pico8_stream_exit_mode();
    
    if (xSemaphoreTake(s_controller.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    
    if (s_controller.current_source != PLAYBACK_SOURCE_PICO8_STREAM) {
        xSemaphoreGive(s_controller.mutex);
        return;
    }
    
    ESP_LOGI(TAG, "Exiting PICO-8 mode");
    
    // Exit unified p3a PICO-8 streaming state and return to playback
    p3a_state_exit_to_playback();
    
    // If we had animation metadata, resume local animation mode
    if (animation_metadata_has_filepath(&s_controller.current_metadata)) {
        s_controller.current_source = PLAYBACK_SOURCE_LOCAL_ANIMATION;
        ESP_LOGI(TAG, "Resuming local animation: %s", s_controller.current_metadata.filepath);
    } else {
        s_controller.current_source = PLAYBACK_SOURCE_NONE;
    }
    
    xSemaphoreGive(s_controller.mutex);
}

bool playback_controller_is_pico8_active(void)
{
    return playback_controller_get_source() == PLAYBACK_SOURCE_PICO8_STREAM;
}

esp_err_t playback_controller_get_current_metadata(const animation_metadata_t **out_meta)
{
    if (!out_meta) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_controller.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_controller.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Only return metadata if we're playing local animation or have preserved metadata
    if (!animation_metadata_has_filepath(&s_controller.current_metadata)) {
        xSemaphoreGive(s_controller.mutex);
        return ESP_ERR_NOT_FOUND;
    }
    
    *out_meta = &s_controller.current_metadata;
    xSemaphoreGive(s_controller.mutex);
    
    return ESP_OK;
}

esp_err_t playback_controller_set_animation_metadata(const char *filepath, bool try_load_sidecar)
{
    if (!filepath) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_controller.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_controller.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Clear existing metadata
    animation_metadata_free(&s_controller.current_metadata);
    animation_metadata_init(&s_controller.current_metadata);
    
    // Set the filepath
    esp_err_t err = animation_metadata_set_filepath(&s_controller.current_metadata, filepath);
    if (err != ESP_OK) {
        xSemaphoreGive(s_controller.mutex);
        return err;
    }
    
    // Try to load sidecar metadata if requested
    if (try_load_sidecar) {
        err = animation_metadata_load_sidecar(&s_controller.current_metadata);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded metadata sidecar for: %s", filepath);
        } else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGD(TAG, "No metadata sidecar for: %s", filepath);
        } else {
            ESP_LOGW(TAG, "Failed to parse metadata sidecar for: %s", filepath);
        }
        // Note: Even if sidecar loading fails, we still have valid filepath
    }
    
    // Update source to local animation (unless in PICO-8 mode)
    if (s_controller.current_source != PLAYBACK_SOURCE_PICO8_STREAM) {
        s_controller.current_source = PLAYBACK_SOURCE_LOCAL_ANIMATION;
    }
    
    xSemaphoreGive(s_controller.mutex);
    
    return ESP_OK;
}

void playback_controller_clear_metadata(void)
{
    if (!s_controller.initialized) {
        return;
    }
    
    if (xSemaphoreTake(s_controller.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    
    animation_metadata_free(&s_controller.current_metadata);
    animation_metadata_init(&s_controller.current_metadata);
    
    // Don't change source if in PICO-8 mode
    if (s_controller.current_source == PLAYBACK_SOURCE_LOCAL_ANIMATION) {
        s_controller.current_source = PLAYBACK_SOURCE_NONE;
    }
    
    xSemaphoreGive(s_controller.mutex);
}

bool playback_controller_has_animation_metadata(void)
{
    bool has_meta = false;
    
    if (s_controller.initialized && s_controller.mutex) {
        if (xSemaphoreTake(s_controller.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            has_meta = s_controller.current_metadata.has_metadata;
            xSemaphoreGive(s_controller.mutex);
        }
    }
    
    return has_meta;
}

const char *playback_controller_get_metadata_field1(void)
{
    const char *value = NULL;
    
    if (s_controller.initialized && s_controller.mutex) {
        if (xSemaphoreTake(s_controller.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_controller.current_metadata.has_metadata) {
                value = s_controller.current_metadata.field1;
            }
            xSemaphoreGive(s_controller.mutex);
        }
    }
    
    return value;
}

int32_t playback_controller_get_metadata_field2(void)
{
    int32_t value = 0;
    
    if (s_controller.initialized && s_controller.mutex) {
        if (xSemaphoreTake(s_controller.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_controller.current_metadata.has_metadata) {
                value = s_controller.current_metadata.field2;
            }
            xSemaphoreGive(s_controller.mutex);
        }
    }
    
    return value;
}

bool playback_controller_get_metadata_field3(void)
{
    bool value = false;
    
    if (s_controller.initialized && s_controller.mutex) {
        if (xSemaphoreTake(s_controller.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_controller.current_metadata.has_metadata) {
                value = s_controller.current_metadata.field3;
            }
            xSemaphoreGive(s_controller.mutex);
        }
    }
    
    return value;
}

