// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_timer.c
 * @brief Auto-swap software timer
 *
 * Uses a FreeRTOS software timer to trigger auto-swap when dwell expires.
 */

#include "play_scheduler_internal.h"
#include "play_scheduler.h"
#include "p3a_state.h"
#include "event_bus.h"
#include "esp_log.h"

static const char *TAG = "ps_timer";

// ============================================================================
// Timer Task
// ============================================================================

static void dwell_timer_callback(TimerHandle_t timer)
{
    ps_state_t *state = (ps_state_t *)pvTimerGetTimerID(timer);
    if (!state) {
        return;
    }

    if (p3a_state_get() != P3A_STATE_ANIMATION_PLAYBACK) {
        return;
    }

    if (state->dwell_time_seconds == 0) {
        return;
    }

    // Skip auto-swap if only one artwork available (nothing to swap to)
    if (play_scheduler_get_total_available() <= 1) {
        ESP_LOGD(TAG, "Auto-swap skipped: only %zu artwork(s) available",
                 play_scheduler_get_total_available());
        return;
    }

    event_bus_emit_simple(P3A_EVENT_SWAP_NEXT);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t ps_timer_start(ps_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state->dwell_timer) {
        ESP_LOGW(TAG, "Dwell timer already running");
        return ESP_OK;
    }

    uint32_t dwell_ms = state->dwell_time_seconds * 1000;
    if (dwell_ms == 0) {
        dwell_ms = 1000;
    }

    state->dwell_timer = xTimerCreate("ps_dwell", pdMS_TO_TICKS(dwell_ms), pdTRUE,
                                      state, dwell_timer_callback);
    if (!state->dwell_timer) {
        ESP_LOGE(TAG, "Failed to create dwell timer");
        return ESP_ERR_NO_MEM;
    }

    if (state->dwell_time_seconds > 0) {
        xTimerStart(state->dwell_timer, 0);
    }

    ESP_LOGI(TAG, "Dwell timer created");
    return ESP_OK;
}

void ps_timer_stop(ps_state_t *state)
{
    if (!state) return;

    if (state->dwell_timer) {
        ESP_LOGI(TAG, "Stopping dwell timer");
        xTimerStop(state->dwell_timer, portMAX_DELAY);
        xTimerDelete(state->dwell_timer, portMAX_DELAY);
        state->dwell_timer = NULL;
    }
}

void ps_timer_reset(ps_state_t *state)
{
    if (!state || !state->dwell_timer) return;

    uint32_t dwell_ms = state->dwell_time_seconds * 1000;
    if (dwell_ms == 0) {
        xTimerStop(state->dwell_timer, 0);
        return;
    }

    xTimerChangePeriod(state->dwell_timer, pdMS_TO_TICKS(dwell_ms), 0);
    xTimerStart(state->dwell_timer, 0);
}

void play_scheduler_pause_auto_swap(void)
{
    ps_state_t *state = ps_get_state();
    if (!state || !state->dwell_timer) {
        return;
    }

    ESP_LOGI(TAG, "Pausing auto-swap timer (PICO-8 mode)");
    xTimerStop(state->dwell_timer, 0);
}

void play_scheduler_resume_auto_swap(void)
{
    ps_state_t *state = ps_get_state();
    if (!state || !state->dwell_timer) {
        return;
    }

    ESP_LOGI(TAG, "Resuming auto-swap timer");
    ps_timer_reset(state);
}
