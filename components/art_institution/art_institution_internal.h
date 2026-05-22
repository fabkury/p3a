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
#include "uthash.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
// Shared refresh helpers (defined in art_institution_refresh.c)
// ============================================================================

/**
 * @brief Si hash node — tracks post_ids seen during a refresh cycle
 *
 * Same shape as giphy's si_node_t and the makapix channel_cache_si_node_t,
 * but with its own typedef so the helpers can live in this component
 * without depending on giphy/channel_manager internals.
 */
typedef struct ai_si_node_s {
    int32_t post_id;
    UT_hash_handle hh;
} ai_si_node_t;

/**
 * @brief Merge a page of new institution entries into a channel cache
 *
 * Dedups by post_id (updates in-place on match). Allows the cache to
 * temporarily exceed max_entries; the orphan-eviction pass after the
 * page loop compacts it back. Rebuilds the post_id hash table. Schedules
 * a debounced cache save. Thread-safe (takes cache mutex).
 */
esp_err_t art_institution_merge_entries(struct channel_cache_s *cache,
                                        const institution_channel_entry_t *new_entries,
                                        size_t new_count,
                                        size_t max_entries);

/**
 * @brief Evict cache entries whose post_id isn't in the Si hash
 *
 * Drops entries from Ci and deletes the corresponding vault files (under
 * /sdcard/p3a/museum/{museum_id}/...). Rebuilds Ci hash. Removes evicted
 * post_ids from LAi. Schedules a debounced save. Thread-safe.
 *
 * Mirrors giphy_evict_orphans / channel_cache_evict_orphans_makapix.
 */
void art_institution_evict_orphans(struct channel_cache_s *cache,
                                   ai_si_node_t *si_hash,
                                   const char *museum_id);

// ============================================================================
// Shared museum HTTP helpers (defined in museums/common.c)
// ============================================================================

/**
 * @brief Percent-encode an input string per RFC 3986 unreserved set
 *
 * Output is null-terminated. Truncated silently if out_len too small —
 * callers should size buffers generously (worst-case 3x input length).
 */
void ai_url_encode(const char *in, char *out, size_t out_len);

/**
 * @brief Drain an open esp_http_client response into a caller-provided buffer
 *
 * Reads until EOF or until one byte short of buf_size (to reserve room for a
 * caller-applied null terminator). Returns total bytes read, or -1 on any
 * read error. Caller is responsible for null-terminating the buffer.
 */
int ai_drain_body(esp_http_client_handle_t client, char *buf, size_t buf_size);

/**
 * @brief Parse a Retry-After header value (seconds form only)
 *
 * Returns the cooldown seconds requested by the server, capped at 3600.
 * Returns 0 for missing, malformed, or HTTP-date-formatted values (caller
 * should fall back to a museum-specific default cooldown in that case).
 */
uint32_t ai_parse_retry_after(const char *value);

// ============================================================================
// AIC adapter entry points (defined in museums/artic.c)
// ============================================================================

esp_err_t art_institution_artic_refresh_channel(const char *channel_id,
                                                const char *axis,
                                                const char *term_id,
                                                uint32_t channel_offset);

esp_err_t art_institution_artic_build_iiif_url(const institution_channel_entry_t *entry,
                                               int longest_side,
                                               char *out, size_t len);

// ============================================================================
// Rijksmuseum adapter entry points (defined in museums/rijksmuseum.c)
// ============================================================================

esp_err_t art_institution_rijks_refresh_channel(const char *channel_id,
                                                const char *axis,
                                                const char *term_id,
                                                uint32_t channel_offset);

esp_err_t art_institution_rijks_build_iiif_url(const institution_channel_entry_t *entry,
                                               int longest_side,
                                               char *out, size_t len);

esp_err_t art_institution_rijks_resolve_entry(institution_channel_entry_t *entry);

// ============================================================================
// V&A adapter entry points (defined in museums/vam.c)
// ============================================================================

esp_err_t art_institution_vam_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id,
                                              uint32_t channel_offset);

esp_err_t art_institution_vam_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len);

// ============================================================================
// Wellcome adapter entry points (defined in museums/wellcome.c)
// ============================================================================

esp_err_t art_institution_wellcome_refresh_channel(const char *channel_id,
                                                   const char *axis,
                                                   const char *term_id,
                                                   uint32_t channel_offset);

esp_err_t art_institution_wellcome_build_iiif_url(const institution_channel_entry_t *entry,
                                                  int longest_side,
                                                  char *out, size_t len);

// ============================================================================
// SMK adapter entry points (defined in museums/smk.c)
// ============================================================================

esp_err_t art_institution_smk_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id,
                                              uint32_t channel_offset);

esp_err_t art_institution_smk_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len);

// ============================================================================
// Harvard Art Museums adapter entry points (defined in museums/ham.c)
// ============================================================================

esp_err_t art_institution_ham_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id,
                                              uint32_t channel_offset);

esp_err_t art_institution_ham_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len);

// ============================================================================
// Smithsonian Open Access adapter entry points (defined in museums/smithsonian.c)
// ============================================================================

esp_err_t art_institution_si_refresh_channel(const char *channel_id,
                                             const char *axis,
                                             const char *term_id,
                                             uint32_t channel_offset);

esp_err_t art_institution_si_build_iiif_url(const institution_channel_entry_t *entry,
                                            int longest_side,
                                            char *out, size_t len);

#ifdef __cplusplus
}
#endif
