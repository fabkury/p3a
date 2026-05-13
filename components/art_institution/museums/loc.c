// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/loc.c
 * @brief Library of Congress adapter — refresh + IIIF URL build.
 *
 * Single axis (`format`). LoC's `/search/` API surfaces a IIIF URL inline
 * on a fraction of results (~8% for photo/print/drawing, ~28% for
 * manuscript/mixed material). Results without an `image-services/iiif`
 * URL are silently dropped at parse time; results whose IIIF id is 48
 * chars or longer are also dropped (see docs/deferred/loc-iiif-key-48-char.md).
 *
 * Reference: docs/art-institutions/loc-channel-design.md and
 * docs/art-institutions/loc-investigation/REPORT.md.
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "art_institution_types.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "ai_loc";

esp_err_t art_institution_loc_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    (void)entry;
    (void)longest_side;
    if (out && len > 0) out[0] = '\0';
    ESP_LOGE(TAG, "build_iiif_url stub — implement in Task 5");
    return ESP_FAIL;
}

esp_err_t art_institution_loc_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id)
{
    (void)channel_id;
    (void)axis;
    (void)term_id;
    ESP_LOGE(TAG, "refresh_channel stub — implement in Task 6");
    return ESP_FAIL;
}
