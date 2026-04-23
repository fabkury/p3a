// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file playback_controller.c
 * @brief Playback source arbitration between animation player and PICO-8 streams
 */

#include "playback_controller.h"
#include "pico8_stream.h"
#include "view_tracker.h"
#include "p3a_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "playback_ctrl";

// Internal state.
// `has_pending_animation` records whether a local animation was playing before
// a PICO-8 stream started, so exit_pico8_mode can decide whether to resume
// LOCAL_ANIMATION or drop back to NONE.
static struct {
    playback_source_t current_source;
    bool has_pending_animation;
    SemaphoreHandle_t mutex;
    bool initialized;
} s_controller = {0};

esp_err_t playback_controller_init(void)
{
    if (s_controller.initialized) {
        ESP_LOGW(TAG, "Playback controller already initialized");
        return ESP_OK;
    }

    s_controller.current_source = PLAYBACK_SOURCE_NONE;
    s_controller.has_pending_animation = false;

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

    if (s_controller.mutex) {
        vSemaphoreDelete(s_controller.mutex);
        s_controller.mutex = NULL;
    }

    s_controller.current_source = PLAYBACK_SOURCE_NONE;
    s_controller.has_pending_animation = false;
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
        xSemaphoreGive(s_controller.mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Entering PICO-8 mode (was: %d)", s_controller.current_source);

    // Stop sending view events - user is gaming, not viewing artwork
    view_tracker_stop();

    // has_pending_animation is preserved across PICO-8 so exit can resume.
    s_controller.current_source = PLAYBACK_SOURCE_PICO8_STREAM;

    xSemaphoreGive(s_controller.mutex);

    esp_err_t state_err = p3a_state_enter_pico8_streaming();
    if (state_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enter p3a PICO-8 state: %s (continuing anyway)", esp_err_to_name(state_err));
    }

    pico8_stream_enter_mode();
    return ESP_OK;
}

void playback_controller_exit_pico8_mode(void)
{
    if (!s_controller.initialized) {
        return;
    }

    pico8_stream_exit_mode();

    if (xSemaphoreTake(s_controller.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_controller.current_source != PLAYBACK_SOURCE_PICO8_STREAM) {
        xSemaphoreGive(s_controller.mutex);
        return;
    }

    ESP_LOGI(TAG, "Exiting PICO-8 mode");

    p3a_state_exit_to_playback();

    if (s_controller.has_pending_animation) {
        s_controller.current_source = PLAYBACK_SOURCE_LOCAL_ANIMATION;
        ESP_LOGI(TAG, "Resuming local animation");
    } else {
        s_controller.current_source = PLAYBACK_SOURCE_NONE;
    }

    xSemaphoreGive(s_controller.mutex);
}

bool playback_controller_is_pico8_active(void)
{
    return playback_controller_get_source() == PLAYBACK_SOURCE_PICO8_STREAM;
}

void playback_controller_notify_animation_active(void)
{
    if (!s_controller.initialized) {
        return;
    }
    if (xSemaphoreTake(s_controller.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    s_controller.has_pending_animation = true;
    if (s_controller.current_source != PLAYBACK_SOURCE_PICO8_STREAM) {
        s_controller.current_source = PLAYBACK_SOURCE_LOCAL_ANIMATION;
    }

    xSemaphoreGive(s_controller.mutex);
}

void playback_controller_notify_animation_stopped(void)
{
    if (!s_controller.initialized) {
        return;
    }
    if (xSemaphoreTake(s_controller.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    s_controller.has_pending_animation = false;
    if (s_controller.current_source == PLAYBACK_SOURCE_LOCAL_ANIMATION) {
        s_controller.current_source = PLAYBACK_SOURCE_NONE;
    }

    xSemaphoreGive(s_controller.mutex);
}
