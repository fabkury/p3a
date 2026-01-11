// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_timer.c
 * @brief Auto-swap timer task
 *
 * Monitors dwell time and triggers auto-swap when timeout expires.
 * Also handles touch event flags for responsive navigation.
 */

#include "play_scheduler_internal.h"
#include "play_scheduler.h"
#include "p3a_state.h"
#include "esp_log.h"

static const char *TAG = "ps_timer";

// ============================================================================
// Timer Task
// ============================================================================

static void timer_task(void *arg)
{
    ps_state_t *state = (ps_state_t *)arg;

    ESP_LOGI(TAG, "Timer task started");

    // Initial delay
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (true) {
        // Skip all swap operations if not in animation playback state
        // This prevents the scheduler from interfering with provisioning, OTA, or PICO-8 modes
        p3a_state_t current_state = p3a_state_get();
        if (current_state != P3A_STATE_ANIMATION_PLAYBACK) {
            // Not in animation playback - just wait and check again
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }

        // Check for touch events
        if (state->touch_next) {
            state->touch_next = false;
            ESP_LOGD(TAG, "Touch next triggered");
            play_scheduler_next(NULL);
        }
        if (state->touch_back) {
            state->touch_back = false;
            ESP_LOGD(TAG, "Touch back triggered");
            play_scheduler_prev(NULL);
        }

        // Get current dwell time
        uint32_t dwell_ms = state->dwell_time_seconds * 1000;

        // If dwell_time is 0, auto-swap is disabled
        if (dwell_ms == 0) {
            // Just wait for touch events or reset notification
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        // Wait for dwell timeout or reset notification
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(dwell_ms));
        if (notified > 0) {
            // Timer was reset (manual navigation or dwell change)
            ESP_LOGD(TAG, "Timer reset");
            continue;
        }

        // Timeout: perform auto-swap
        ESP_LOGD(TAG, "Auto-swap timer elapsed, advancing");
        play_scheduler_next(NULL);
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t ps_timer_start(ps_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state->timer_task) {
        ESP_LOGW(TAG, "Timer task already running");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(
        timer_task,
        "ps_timer",
        4096,
        state,
        5,  // Priority
        &state->timer_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create timer task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Timer task created");
    return ESP_OK;
}

void ps_timer_stop(ps_state_t *state)
{
    if (!state) return;

    if (state->timer_task) {
        ESP_LOGI(TAG, "Stopping timer task");
        vTaskDelete(state->timer_task);
        state->timer_task = NULL;
    }
}

void ps_timer_reset(ps_state_t *state)
{
    if (!state || !state->timer_task) return;

    xTaskNotifyGive(state->timer_task);
}
