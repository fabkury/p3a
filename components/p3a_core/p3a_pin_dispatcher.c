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
#define PIN_DISP_SOURCE_NONE        0
#define PIN_DISP_SOURCE_MAKAPIX     1
#define PIN_DISP_SOURCE_GIPHY       2
#define PIN_DISP_SOURCE_SDCARD      3
#define PIN_DISP_SOURCE_INSTITUTION 4

// LCD overlay (defined in main/display_pin_overlay.c)
extern void pin_overlay_show_submit(void) __attribute__((weak));
extern void pin_overlay_show_unpin(void)  __attribute__((weak));
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

extern esp_err_t pin_lists_pin_giphy(const char *slug,
                                     int32_t original_post_id,
                                     const char *giphy_id,
                                     uint8_t extension,
                                     const char *title,
                                     const char *creator,
                                     uint32_t original_created_at,
                                     const char *src_artwork_path) __attribute__((weak));
extern esp_err_t pin_lists_unpin_giphy(const char *slug, const char *giphy_id) __attribute__((weak));

extern esp_err_t pin_lists_pin_institution(const char *slug,
                                           int32_t original_post_id,
                                           uint16_t museum_id,
                                           const char *iiif_key_safe,
                                           uint8_t extension,
                                           const char *title,
                                           const char *creator,
                                           uint32_t original_created_at,
                                           const char *src_artwork_path) __attribute__((weak));
extern esp_err_t pin_lists_unpin_institution(const char *slug,
                                             uint16_t museum_id,
                                             const char *iiif_key_safe) __attribute__((weak));

// Auto-swap dwell timer (defined in play_scheduler component). Weak to avoid
// a circular REQUIRES — play_scheduler already depends on p3a_core.
extern void play_scheduler_reset_timer(void) __attribute__((weak));

// Map a museum vault-path identifier ("artic", "rijks", ...) to its enum
// ordinal. Declared as weak extern so this file doesn't need to REQUIRE
// art_institution — which would create a cycle, since art_institution
// already REQUIREs p3a_core. The canonical mapping (and a build-time guard
// that catches missing entries) lives in components/art_institution/.
extern int art_institution_enum_from_id(const char *museum_id) __attribute__((weak));

/* Active single-artwork title (defined in components/play_scheduler/play_scheduler.c).
   Weak symbol so this file doesn't drag in play_scheduler headers. Reads the
   title field from the active playset's ARTWORK channel — empty if not a
   single-artwork playset. */
extern void play_scheduler_get_active_artwork_title(char *out_title, size_t len)
    __attribute__((weak));

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
    int       source;                     // PIN_DISP_SOURCE_* (1, 2, or 4)
    char      slug[16];
    int32_t   original_post_id;
    uint8_t   extension;
    uint16_t  museum_id;                  // institution only
    char      title[256];
    char      creator[128];
    uint32_t  original_created_at;
    char      src_artwork_path[256];
    /* Source-specific identifier, populated by the resolver. */
    char      uuid[40];                   // makapix
    char      giphy_id[40];               // giphy
    char      iiif_key[48];               // institution (safe form)
    bool      is_unpin;
} pin_task_params_t;

static void pin_task(void *arg)
{
    pin_task_params_t *p = (pin_task_params_t *)arg;
    const char *slug = p->slug[0] ? p->slug : NULL;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    const char *id_log = "";

    switch (p->source) {
        case PIN_DISP_SOURCE_MAKAPIX:
            id_log = p->uuid;
            if (p->is_unpin) {
                if (pin_lists_unpin_makapix) err = pin_lists_unpin_makapix(slug, p->uuid);
            } else {
                if (pin_lists_pin_makapix) {
                    err = pin_lists_pin_makapix(slug, p->original_post_id, p->uuid,
                                                p->extension, p->title, p->creator,
                                                p->original_created_at, p->src_artwork_path);
                }
            }
            break;
        case PIN_DISP_SOURCE_GIPHY:
            id_log = p->giphy_id;
            if (p->is_unpin) {
                if (pin_lists_unpin_giphy) err = pin_lists_unpin_giphy(slug, p->giphy_id);
            } else {
                if (pin_lists_pin_giphy) {
                    err = pin_lists_pin_giphy(slug, p->original_post_id, p->giphy_id,
                                              p->extension, p->title, p->creator,
                                              p->original_created_at, p->src_artwork_path);
                }
            }
            break;
        case PIN_DISP_SOURCE_INSTITUTION:
            id_log = p->iiif_key;
            if (p->is_unpin) {
                if (pin_lists_unpin_institution) {
                    err = pin_lists_unpin_institution(slug, p->museum_id, p->iiif_key);
                }
            } else {
                if (pin_lists_pin_institution) {
                    err = pin_lists_pin_institution(slug, p->original_post_id,
                                                    p->museum_id, p->iiif_key,
                                                    p->extension, p->title, p->creator,
                                                    p->original_created_at, p->src_artwork_path);
                }
            }
            break;
        default:
            break;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed src=%d id=%s err=%s",
                 p->is_unpin ? "unpin" : "pin", p->source, id_log, esp_err_to_name(err));
        if (pin_overlay_show_error) pin_overlay_show_error();
    } else {
        ESP_LOGI(TAG, "%s ok src=%d id=%s slug=%s",
                 p->is_unpin ? "unpin" : "pin", p->source, id_log,
                 p->slug[0] ? p->slug : "(active)");
    }
    free(p);
    vTaskDelete(NULL);
}

static esp_err_t spawn_pin_task(pin_task_params_t *p)
{
    /* 8 KB: matches giphy_click_task — the deepest call chain
     * (pin_list_pin -> pl_manifest_save -> cJSON_PrintUnformatted -> fwrite ->
     *  vsnprintf) plus pin_list_pin's own 512 B pinned_entry_file_t copy and
     * several 256-byte path buffers across nested frames easily blows past
     * 4 KB; observed overflow ~116 B below the lower bound on first
     * makapix pin via /action/pin. */
    BaseType_t ret = xTaskCreate(pin_task, "pin_io", 8192, p, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pin_io task");
        free(p);
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Source-specific resolvers (parse current_post into pin_task_params_t)    */
/* ------------------------------------------------------------------------- */

/* Find the path component immediately after `marker` (e.g., "/museum/").
 * Copies up to the next '/' into out. Returns true on success. */
static bool extract_path_component_after(const char *path, const char *marker,
                                         char *out, size_t out_len)
{
    const char *p = strstr(path, marker);
    if (!p) return false;
    p += strlen(marker);
    const char *slash = strchr(p, '/');
    if (!slash) return false;
    size_t n = (size_t)(slash - p);
    if (n == 0 || n >= out_len) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static esp_err_t resolve_makapix_from_current(pin_task_params_t *p)
{
    char filepath[256];
    p3a_current_post_get_filepath(filepath, sizeof(filepath));
    if (filepath[0] == '\0') return ESP_ERR_INVALID_STATE;

    int ext = extension_index_from_path(filepath);
    if (ext < 0) return ESP_ERR_INVALID_ARG;
    p->extension = (uint8_t)ext;

    char stem[40];
    if (extract_basename_stem(filepath, stem, sizeof(stem)) < 0) return ESP_ERR_INVALID_ARG;
    if (strlen(stem) != 36) {
        ESP_LOGW(TAG, "Makapix basename '%s' is not a 36-char UUID", stem);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(p->uuid, stem, sizeof(p->uuid));
    strlcpy(p->src_artwork_path, filepath, sizeof(p->src_artwork_path));
    p->original_post_id = p3a_current_post_get_id();
    p->original_created_at = 0;
    if (play_scheduler_get_active_artwork_title) {
        play_scheduler_get_active_artwork_title(p->title, sizeof(p->title));
    }
    return ESP_OK;
}

static esp_err_t resolve_giphy_from_current(pin_task_params_t *p)
{
    char filepath[256];
    p3a_current_post_get_filepath(filepath, sizeof(filepath));
    if (filepath[0] == '\0') return ESP_ERR_INVALID_STATE;

    int ext = extension_index_from_path(filepath);
    if (ext < 0) return ESP_ERR_INVALID_ARG;
    p->extension = (uint8_t)ext;

    /* Prefer the giphy_id explicitly tracked on current_post; fall back to
       the filepath basename. */
    p3a_current_post_get_giphy_id(p->giphy_id, sizeof(p->giphy_id));
    if (p->giphy_id[0] == '\0') {
        if (extract_basename_stem(filepath, p->giphy_id, sizeof(p->giphy_id)) < 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    strlcpy(p->src_artwork_path, filepath, sizeof(p->src_artwork_path));
    p->original_post_id = p3a_current_post_get_id();
    p->original_created_at = 0;
    if (play_scheduler_get_active_artwork_title) {
        play_scheduler_get_active_artwork_title(p->title, sizeof(p->title));
    }
    return ESP_OK;
}

static esp_err_t resolve_institution_from_current(pin_task_params_t *p)
{
    char filepath[256];
    p3a_current_post_get_filepath(filepath, sizeof(filepath));
    if (filepath[0] == '\0') return ESP_ERR_INVALID_STATE;

    int ext = extension_index_from_path(filepath);
    if (ext < 0) return ESP_ERR_INVALID_ARG;
    p->extension = (uint8_t)ext;

    /* filepath: {museum-root}/{museum_str}/{d0}/{d1}/{safe_iiif_key}.{ext}
       (v1.0 shard layout, see sd_path_build_sharded()) */
    char museum_str[16];
    if (!extract_path_component_after(filepath, "/museum/", museum_str, sizeof(museum_str))) {
        ESP_LOGW(TAG, "Museum component not found in %s", filepath);
        return ESP_ERR_INVALID_ARG;
    }
    if (!art_institution_enum_from_id) {
        ESP_LOGE(TAG, "art_institution_enum_from_id unavailable (link error)");
        return ESP_ERR_INVALID_STATE;
    }
    int museum_enum = art_institution_enum_from_id(museum_str);
    if (museum_enum < 0) {
        ESP_LOGW(TAG, "Unknown museum string '%s'", museum_str);
        return ESP_ERR_INVALID_ARG;
    }
    p->museum_id = (uint16_t)museum_enum;

    char stem[48];
    if (extract_basename_stem(filepath, stem, sizeof(stem)) < 0) return ESP_ERR_INVALID_ARG;

    /* HAM's canonical iiif_key contains colons (`urn-3:HUAM:{id}_dynmc`),
       but FAT filenames cannot carry them — art_institution_build_vault_path
       sanitizes the two colons to underscores before composing the on-disk
       name. Reverse that here so the pin entry stores the canonical form,
       matching the channel cache and round-tripping cleanly through
       art_institution_build_iiif_url at playback time. The strncmp guard
       makes this a no-op for any future HAM URN that doesn't start with the
       documented prefix, and for the other museums (whose identifiers use
       only FAT-safe characters today). */
    if (strcmp(museum_str, "ham") == 0 && strncmp(stem, "urn-3_HUAM_", 11) == 0) {
        stem[5]  = ':';
        stem[10] = ':';
    }

    strlcpy(p->iiif_key, stem, sizeof(p->iiif_key));

    strlcpy(p->src_artwork_path, filepath, sizeof(p->src_artwork_path));
    p->original_post_id = p3a_current_post_get_id();
    p->original_created_at = 0;
    if (play_scheduler_get_active_artwork_title) {
        play_scheduler_get_active_artwork_title(p->title, sizeof(p->title));
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Public entry points                                                      */
/* ------------------------------------------------------------------------- */

static esp_err_t resolve_from_current(int source, pin_task_params_t *p)
{
    p->source = source;
    switch (source) {
        case PIN_DISP_SOURCE_MAKAPIX:     return resolve_makapix_from_current(p);
        case PIN_DISP_SOURCE_GIPHY:       return resolve_giphy_from_current(p);
        case PIN_DISP_SOURCE_INSTITUTION: return resolve_institution_from_current(p);
        default:                          return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t p3a_pin_dispatch_from_current(const char *slug)
{
    int source = p3a_current_post_get_source();
    if (source == PIN_DISP_SOURCE_NONE) {
        ESP_LOGW(TAG, "Pin requested with no current post");
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_INVALID_STATE;
    }
    if (source == PIN_DISP_SOURCE_SDCARD) {
        /* SD-card artworks cannot be pinned per spec; silent no-op. */
        return ESP_OK;
    }

    /* User engagement with the current artwork extends dwell. */
    if (play_scheduler_reset_timer) play_scheduler_reset_timer();

    pin_task_params_t *p = calloc(1, sizeof(*p));
    if (!p) {
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = resolve_from_current(source, p);
    if (err != ESP_OK) {
        free(p);
        if (pin_overlay_show_error) pin_overlay_show_error();
        return err;
    }
    if (slug && slug[0]) strlcpy(p->slug, slug, sizeof(p->slug));
    p->is_unpin = false;

    if (pin_overlay_show_submit) pin_overlay_show_submit();
    return spawn_pin_task(p);
}

esp_err_t p3a_pin_dispatch_unpin_from_current(const char *slug)
{
    int source = p3a_current_post_get_source();
    if (source == PIN_DISP_SOURCE_NONE) {
        ESP_LOGW(TAG, "Unpin requested with no current post");
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_INVALID_STATE;
    }
    if (source == PIN_DISP_SOURCE_SDCARD) return ESP_OK;  /* silent no-op */

    pin_task_params_t *p = calloc(1, sizeof(*p));
    if (!p) {
        if (pin_overlay_show_error) pin_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = resolve_from_current(source, p);
    if (err != ESP_OK) {
        free(p);
        if (pin_overlay_show_error) pin_overlay_show_error();
        return err;
    }
    if (slug && slug[0]) strlcpy(p->slug, slug, sizeof(p->slug));
    p->is_unpin = true;

    if (pin_overlay_show_unpin) pin_overlay_show_unpin();
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
