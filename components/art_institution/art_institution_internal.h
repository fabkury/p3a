// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file art_institution_internal.h
 * @brief Shared internals between art_institution.c, the per-museum adapters
 *        under museums/, and the rate-limit / refresh / download units.
 */

#pragma once

#include "art_institution.h"
#include "art_institution_types.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default cooldown seconds per museum when a 429 lands without Retry-After.
// 60 s matches AIC's 1-minute rate-limit window and is a safe default for
// Rijks too (its listing endpoint is light and rarely 429s).
#define ART_INSTITUTION_DEFAULT_COOLDOWN_SEC 60

/**
 * @brief Reset all per-museum cooldown state to "not throttled"
 *
 * Called from art_institution_init(). Cooldowns are RAM-only by design
 * (reboot clears them, matching giphy_set_rate_limited).
 */
void art_institution_rate_limit_reset(void);

/**
 * @brief Internal: set cooldown by enum (avoids a string lookup)
 *
 * Used by museum adapters that already know their enum value.
 */
void art_institution_rate_limit_set_enum(museum_id_t id, uint32_t cooldown_sec);

/**
 * @brief Internal: check cooldown by enum
 */
bool art_institution_rate_limit_is_active_enum(museum_id_t id);

/**
 * @brief Internal: remaining cooldown seconds by enum
 */
uint32_t art_institution_rate_limit_remaining_enum(museum_id_t id);

// ============================================================================
// AIC adapter entry points (defined in museums/artic.c)
// ============================================================================

esp_err_t art_institution_artic_refresh_channel(const char *axis,
                                                const char *term_id,
                                                struct channel_cache_s *cache);

esp_err_t art_institution_artic_build_iiif_url(const institution_channel_entry_t *entry,
                                               int longest_side,
                                               char *out, size_t len);

#ifdef __cplusplus
}
#endif
