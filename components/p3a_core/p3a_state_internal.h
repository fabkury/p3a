// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file p3a_state_internal.h
 * @brief Private internal header for p3a state machine implementation files
 *
 * This header is NOT in include/ and therefore not visible to other components.
 * It exposes the shared s_state struct across the split .c files within p3a_core.
 */

#ifndef P3A_STATE_INTERNAL_H
#define P3A_STATE_INTERNAL_H

#include "p3a_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CALLBACKS 8

typedef struct {
    // Global state
    p3a_state_t current_state;
    p3a_state_t previous_state;

    // App-level status (legacy app_state)
    p3a_app_status_t app_status;

    // Sub-states
    p3a_playback_substate_t playback_substate;
    p3a_provisioning_substate_t provisioning_substate;
    p3a_ota_substate_t ota_substate;

    // Active playset name (persisted to NVS)
    char active_playset[P3A_PLAYSET_MAX_NAME_LEN + 1];

    // Channel info
    p3a_channel_info_t current_channel;

    // Channel message (for CHANNEL_MESSAGE sub-state)
    p3a_channel_message_t channel_message;

    // OTA progress
    int ota_progress_percent;
    char ota_status_text[64];
    char ota_version_from[32];
    char ota_version_to[32];

    // Provisioning info
    char provisioning_status[128];
    char provisioning_code[16];
    char provisioning_expires[32];

    // Connectivity (orthogonal)
    p3a_connectivity_level_t connectivity;
    EventGroupHandle_t connectivity_event_group;
    TimerHandle_t internet_check_timer;
    time_t last_internet_check;
    bool internet_check_in_progress;
    uint32_t mqtt_backoff_ms;
    bool has_registration;

    // Callbacks
    struct {
        p3a_state_change_cb_t callback;
        void *user_data;
    } callbacks[MAX_CALLBACKS];
    int callback_count;

    // Synchronization
    SemaphoreHandle_t mutex;
    bool initialized;
} p3a_state_internal_t;

extern p3a_state_internal_t s_state;

void p3a_state_notify_callbacks(p3a_state_t old_state, p3a_state_t new_state);
void p3a_state_update_channel_display_name(p3a_channel_info_t *info);

#ifdef __cplusplus
}
#endif

#endif // P3A_STATE_INTERNAL_H
