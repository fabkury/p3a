// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_pin_dispatcher.c
 * @brief Pin / unpin dispatcher (gesture + HTTP entry point)
 *
 * Mirrors p3a_reaction_dispatcher: shows the LCD overlay optimistically,
 * spawns a fire-and-forget FreeRTOS task to perform the SD-card copy and
 * index mutation, and reverts to the error overlay on failure.
 *
 * Phase 3 supports Makapix only. Giphy and museum sources return
 * ESP_ERR_NOT_SUPPORTED with the error overlay until phase 4.
 */

#include "p3a_pin_dispatcher.h"
#include "p3a_current_post.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "p3a_pin_dispatcher";

// Mirrors post_source_t values from play_scheduler_types.h. Re-declared
// locally so this file doesn't drag in play_scheduler headers.
#define PIN_DISP_SOURCE_NONE     0
#define PIN_DISP_SOURCE_MAKAPIX  1
#define PIN_DISP_SOURCE_GIPHY    2
#define PIN_DISP_SOURCE_SDCARD   3

// LCD overlay (defined in main/display_pin_overlay.c)
extern void pin_overlay_show_submit(void) __attribute__((weak));
extern void pin_overlay_show_error(void)  __attribute__((weak));

// Pin-lists API (defined in pin_lists component) — declared as weak externs
// so p3a_core doesn't need to REQUIRE pin_lists (which would create a circular
// dep, since pin_lists already REQUIRES p3a_core for sd_path / current_post).
extern esp_err_t pin_lists_pin_makapix(const char *slug,
                                       int32_t original_post_id,
                                       const char *uuid_36chars,
                                       uint8_t extension,
                                       const char *title,
                                       const char *creator,
                                       uint32_t original_created_at,
                                       const char *src_artwork_path) __attribute__((weak));
extern esp_err_t pin_lists_unpin_makapix(const char *slug, const char *uuid_36chars) __attribute__((weak));

// Active artwork title (defined in components/p3a_core/p3a_state.c).
extern const char *p3a_state_get_active_artwork_title(void) __attribute__((weak));

/* ------------------------------------------------------------------------- */
/*  Filename parsing                                                         */
/* ------------------------------------------------------------------------- */

/**
 * Extract the basename (last path component) without extension.
 * Returns the length, or -1 on failure.
 */
static int extract_basename_stem(const char *filepath, char *out, size_t out_len)
{
    if (!filepath || !out || out_len == 0) return -1;
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n >= out_len) return -1;
    memcpy(out, base, n);
    out[n] = '\0';
    return (int)n;
}

/**
 * Get the extension index from a filepath. webp=0, gif=1, png=2, jpg=3.
 * Returns -1 if unrecognized.
 */
static int extension_index_from_path(const char *filepath)
{
    if (!filepath) return -1;
    const char *dot = strrchr(filepath, '.');
    if (!dot) return -1;
    if (strcasecmp(dot, ".webp") == 0) return 0;
    if (strcasecmp(dot, ".gif") == 0)  return 1;
    if (strcasecmp(dot, ".png") == 0)  return 2;
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return 3;
    return -1;
}

/* ------------------------------------------------------------------------- */
/*  Async pin task (Makapix)                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    char     slug[16];
    int32_t  original_post_id;
    char     uuid[40];
    uint8_t  extension;
    char     title[256];
    char     creator[128];
    uint32_t original_created_at;
    char     src_artwork_path[256];
    bool     is_unpin;
} pin_task_params_t;

static void pin_task(void *arg)
{
    pin_task_params_t *p = (pin_task_params_t *)arg;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (p->is_unpin) {
        if (pin_lists_unpin_makapix) {
            err = pin_lists_unpin_makapix(p->slug[0] ? p->slug : NULL, p->uuid);
        }
    } else {
        if (pin_lists_pin_makapix) {
            err = pin_lists_pin_makapix(p->slug[0] ? p->slug : NULL,
                                        p->original_post_id, p->uuid, p->extension,
                                        p->title, p->creator,
                                        p->original_created_at, p->src_artwork_path);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed for uuid=%s err=%s",
                 p->is_unpin ? "unpin" : "pin", p->uuid, esp_err_to_name(err));
        if (pin_overlay_show_error) pin_overlay_show_error();
    } else {
        ESP_LOGI(TAG, "%s ok uuid=%s slug=%s",
                 p->is_unpin ? "unpin" : "pin", p->uuid,
                 p->slug[0] ? p->slug : "(active)");
    }
    free(p);
    vTaskDelete(NULL);
}

static esp_err_t spawn_pin_task(pin_task_params_t *p)
{
    BaseType_t ret = xTaskCreate(pin_task, "pin_io", 4096, p, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pin_io task");
        free(p);
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Resolve current-post info (Makapix path only for phase 3)                */
/* ------------------------------------------------------------------------- */

static esp_err_t build_makapix_params_from_current(pin_task_params_t *p)
{
    char filepath[256];
    p3a_current_post_get_filepath(filepath, sizeof(filepath));
    if (filepath[0] == '\0') {
        ESP_LOGW(TAG, "Current post has no filepath");
        return ESP_ERR_INVALID_STATE;
    }

    int ext = extension_index_from_path(filepath);
    if (ext < 0) {
        ESP_LOGW(TAG, "Unrecognized extension in %s", filepath);
        return ESP_ERR_INVALID_ARG;
    }
    p->extension = (uint8_t)ext;

    char stem[40];
    if (extract_basename_stem(filepath, stem, sizeof(stem)) < 0) {
        ESP_LOGW(TAG, "Cannot extract basename from %s", filepath);
        return ESP_ERR_INVALID_ARG;
    }
    /* Expect a 36-char UUID for Makapix. */
    if (strlen(stem) != 36) {
        ESP_LOGW(TAG, "Basename '%s' is not a 36-char UUID", stem);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(p->uuid, stem, sizeof(p->uuid));
    strlcpy(p->src_artwork_path, filepath, sizeof(p->src_artwork_path));
    p->original_post_id = p3a_current_post_get_id();
    p->original_created_at = 0;

    /* Best-effort title; creator left empty for v1. */
    if (p3a_state_get_active_artwork_title) {
        const char *t = p3a_state_get_active_artwork_title();
        if (t) strlcpy(p->title, t, sizeof(p->title));
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Public entry points                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t p3a_pin_dispatch_from_current(const char *slug)
{
    int source = p3a_current_post_get_source();
    if (source == PIN_DISP_SOURCE_NONE) {
        ESP_LOGW(TAG, "Pin requested with no current post");
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_INVALID_STATE;
    }
    if (source != PIN_DISP_SOURCE_MAKAPIX) {
        /* Phase 3 supports Makapix only. Silent no-op for SD card (matches
           the pin-not-allowed policy). For Giphy/museum, surface error icon. */
        if (source == PIN_DISP_SOURCE_SDCARD) {
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Pin not yet supported for source=%d (phase 4)", source);
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_NOT_SUPPORTED;
    }

    pin_task_params_t *p = calloc(1, sizeof(*p));
    if (!p) {
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = build_makapix_params_from_current(p);
    if (err != ESP_OK) {
        free(p);
        if (pin_overlay_show_error) pin_overlay_show_error();
        return err;
    }
    if (slug && slug[0]) strlcpy(p->slug, slug, sizeof(p->slug));
    p->is_unpin = false;

    /* Optimistic overlay before spawning. */
    if (pin_overlay_show_submit) pin_overlay_show_submit();
    return spawn_pin_task(p);
}

esp_err_t p3a_pin_dispatch_unpin_from_current(const char *slug)
{
    int source = p3a_current_post_get_source();
    if (source == PIN_DISP_SOURCE_NONE) return ESP_ERR_INVALID_STATE;
    if (source == PIN_DISP_SOURCE_SDCARD) return ESP_OK;  /* silent no-op */
    if (source != PIN_DISP_SOURCE_MAKAPIX) {
        ESP_LOGI(TAG, "Unpin not yet supported for source=%d (phase 4)", source);
        return ESP_ERR_NOT_SUPPORTED;
    }

    pin_task_params_t *p = calloc(1, sizeof(*p));
    if (!p) return ESP_ERR_NO_MEM;
    char filepath[256];
    p3a_current_post_get_filepath(filepath, sizeof(filepath));
    if (filepath[0] == '\0') { free(p); return ESP_ERR_INVALID_STATE; }
    char stem[40];
    if (extract_basename_stem(filepath, stem, sizeof(stem)) < 0 || strlen(stem) != 36) {
        free(p);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(p->uuid, stem, sizeof(p->uuid));
    if (slug && slug[0]) strlcpy(p->slug, slug, sizeof(p->slug));
    p->is_unpin = true;

    /* No optimistic overlay for unpin in v1 — gesture-driven unpin is
       commonly paired with reaction-revoke which already shows feedback. */
    return spawn_pin_task(p);
}

esp_err_t p3a_pin_dispatch_makapix_pin(int32_t post_id, const char *slug)
{
    if (p3a_current_post_get_source() != PIN_DISP_SOURCE_MAKAPIX) {
        return ESP_ERR_INVALID_STATE;
    }
    if (p3a_current_post_get_id() != post_id) {
        return ESP_ERR_INVALID_STATE;
    }
    return p3a_pin_dispatch_from_current(slug);
}

esp_err_t p3a_pin_dispatch_makapix_unpin(int32_t post_id, const char *slug)
{
    if (p3a_current_post_get_source() != PIN_DISP_SOURCE_MAKAPIX) {
        return ESP_ERR_INVALID_STATE;
    }
    if (p3a_current_post_get_id() != post_id) {
        return ESP_ERR_INVALID_STATE;
    }
    return p3a_pin_dispatch_unpin_from_current(slug);
}
