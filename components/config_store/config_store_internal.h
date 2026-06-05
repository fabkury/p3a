// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file config_store_internal.h
 * @brief Config store internal shared header: NVS helpers, cache callbacks
 */

#pragma once

#include "config_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define NAMESPACE "appcfg"
#define KEY_CUR   "cfg"
#define KEY_NEW   "cfg_new"
// Max serialized config size. Sized against the 64 KB NVS partition: saves
// are double-buffered (KEY_NEW + KEY_CUR), so peak NVS demand is 2x this,
// alongside the makapix cert blobs (up to 3x MAKAPIX_PEM_MAX_LEN, 2x during
// re-registration) and Wi-Fi credentials. Today's config is ~1-2 KB; 8 KB is
// generous headroom. Keep these budgets in sync if either side grows.
#define MAX_JSON  (8 * 1024)

// Internal helpers (defined in config_store.c)
esp_err_t cfg_ensure_nvs(nvs_handle_t *h);
uint8_t cfg_clamp_u8_num(const cJSON *n, uint8_t def);
esp_err_t cfg_set_string(const char *key, const char *value);
esp_err_t cfg_get_string(const char *key, const char *default_val,
                         char *out, size_t max_len);

// Cache-apply callbacks (defined in config_store_settings.c)
void cfg_bg_apply_from_cfg(const cJSON *cfg);
void cfg_show_fps_apply_from_cfg(const cJSON *cfg);
void cfg_max_speed_playback_apply_from_cfg(const cJSON *cfg);
