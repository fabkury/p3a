// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/artic.c
 * @brief Art Institute of Chicago (AIC) adapter
 *
 * Stage 1 ships only the dispatch slots; the listing and IIIF URL
 * implementations are added in Stage 3. Returning ESP_ERR_NOT_SUPPORTED
 * keeps the build green while the rest of the scaffolding lands.
 *
 * Spec: docs/art-institutions/finalized-design.md §9.1.
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "ai_artic";

esp_err_t art_institution_artic_refresh_channel(const char *axis,
                                                const char *term_id,
                                                struct channel_cache_s *cache)
{
    (void)cache;
    ESP_LOGW(TAG, "refresh_channel not yet implemented (axis=%s term=%s)",
             axis ? axis : "?", term_id ? term_id : "?");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t art_institution_artic_build_iiif_url(const institution_channel_entry_t *entry,
                                               int longest_side,
                                               char *out, size_t len)
{
    (void)entry;
    (void)longest_side;
    if (out && len > 0) out[0] = '\0';
    return ESP_ERR_NOT_SUPPORTED;
}
