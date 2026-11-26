// SPDX-License-Identifier: Apache-2.0
// Shared between the files of p3a_update. Not for other components.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_update.h"
#include "update_release.h"

#define UPDATE_LIST_URL   "https://api.github.com/repos/fabkury/p3a/releases?per_page=8"
#define UPDATE_URL_PREFIX "https://github.com/fabkury/p3a/releases/download/"
#define UPDATE_LIST_MAX   (512u * 1024u)  // eight releases with every asset GitHub lists: about 300 KB

typedef struct {
    SemaphoreHandle_t lock;

    // What p3a_update_boot() found.
    char running[32];
    bool on_trial;
    char other[32];
    char undone[32];
    const esp_partition_t *other_slot;  // where an install goes; what a roll back boots
    bool other_bootable;

    p3a_update_state_t state;
    uint8_t progress;

    bool checked;
    bool check_ok;
    int64_t checked_us;
    esp_err_t check_error;
    bool has_latest;
    update_release_t latest;

    esp_err_t install_error;
    char install_error_text[96];

    // The clock's facts.
    bool auto_check;
    bool prereleases;
    bool boot_done;
    bool net_seen;
    int64_t net_up_us;
    bool trial_decided;

    char list_url[160];
    char url_prefix[128];
} update_state_t;

extern update_state_t g_update;

static inline void update_lock(void)
{
    xSemaphoreTake(g_update.lock, portMAX_DELAY);
}

static inline void update_unlock(void)
{
    xSemaphoreGive(g_update.lock);
}

// update_install.c: the job. The state is INSTALLING when it starts; it
// leaves RESTARTING (and the device restarts) or IDLE with install_error.
esp_err_t update_install_job(void *arg);

// update_api.c
esp_err_t update_api_register(void);
