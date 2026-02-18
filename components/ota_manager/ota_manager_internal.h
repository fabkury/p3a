// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file ota_manager_internal.h
 * @brief Internal shared header for ota_manager module
 *
 * Contains shared types, state, and function declarations used across
 * the ota_manager source files. Not intended for external use.
 */
#pragma once

#include "ota_manager.h"
#include "github_ota.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>

// --- Shared firmware OTA state ---

typedef struct {
    ota_state_t state;
    github_release_info_t release_info;
    int64_t last_check_time;
    int download_progress;
    char error_message[128];
    SemaphoreHandle_t mutex;
    esp_timer_handle_t check_timer;
    TaskHandle_t check_task;
    ota_progress_cb_t progress_callback;
    ota_ui_cb_t ui_callback;
    bool initialized;
    bool ui_active;
} ota_internal_state_t;

extern ota_internal_state_t s_ota;

// --- State helpers (defined in ota_manager.c) ---

void set_state(ota_state_t new_state);
void set_error(const char *message);
void set_progress(int percent, const char *status);
void ota_exit_ui_mode(void);
esp_err_t ota_check_wifi_connected(void);

// --- Extern animation player functions ---

extern bool animation_player_is_sd_export_locked(void);
extern bool animation_player_is_loader_busy(void);
extern bool animation_player_is_ui_mode(void);
extern void animation_player_pause_sd_access(void);
extern void animation_player_resume_sd_access(void);

// --- WebUI OTA internal init/deinit (defined in ota_manager_webui.c) ---

esp_err_t webui_ota_init(void);
void webui_ota_deinit(void);
