// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file art_institution.h
 * @brief Public API for the art-institution channel source (Museums)
 *
 * The component owns: refresh of saved museum channels, IIIF download path
 * construction, and per-museum rate-limit cooldown shared between device
 * and browser. Adapters per museum live in museums/<id>.c and register
 * through the dispatch table declared here.
 *
 * Lifecycle:
 *   - art_institution_init() once at boot (after config_store_init).
 *   - play_scheduler_refresh.c calls art_institution_refresh_by_spec()
 *     for any channel with type=PS_CHANNEL_TYPE_INSTITUTION.
 *   - download_manager.c calls art_institution_build_vault_path() and
 *     art_institution_download_entry() for institution-channel entries.
 */

#pragma once

#include "esp_err.h"
#include "art_institution_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward decl to avoid pulling channel_manager headers into every caller.
struct channel_cache_s;

/**
 * @brief Per-museum dispatch entry (one per museum)
 *
 * Populated at compile time in art_institution.c. The string `id` is what
 * appears on the wire as the prefix of the playset channel `name` field
 * (e.g. `"artic:departments"`); it must never change once shipped.
 */
typedef struct {
    const char *id;            ///< Stable wire id ("artic", "rijks")
    const char *display;       ///< Human-readable label ("Art Institute of Chicago")
    museum_id_t museum_enum;   ///< Enum value used to index the rate-limit table

    /**
     * Refresh a single saved channel. Implementation walks the museum's
     * listing API for (axis, term_id), pages through it, and merges into
     * the channel cache. The adapter re-resolves the cache from the
     * registry between pages under channel_cache_lifecycle_lock() — same
     * pattern Giphy uses — so a concurrent playset switch can't free the
     * cache out from under us mid-refresh. Intra-channel orphan eviction
     * runs at the end. Returns ESP_OK on full success,
     * ESP_ERR_INVALID_RESPONSE on HTTP 429 (caller surfaces cooldown UI),
     * other codes for network / parse failures.
     *
     * `channel_offset` is the per-playset starting offset into the museum's
     * listing. Museums that support random-access pagination apply it
     * directly; museums that only support cursor walks discard the first
     * channel_offset entries during the walk. Modulo-wrap against the
     * facet's total record count happens inside the adapter once it
     * knows the count.
     */
    esp_err_t (*refresh_channel)(const char *channel_id,
                                 const char *axis,
                                 const char *term_id,
                                 uint32_t channel_offset);

    /**
     * Build the IIIF JPEG URL for one entry at the requested longest-side
     * pixel cap. Out buffer must hold a full URL (suggest >= 256 bytes).
     * For museums that need on-demand resolution (e.g. Rijks Linked Art),
     * returns ESP_ERR_INVALID_STATE if the entry is still unresolved.
     */
    esp_err_t (*build_iiif_url)(const institution_channel_entry_t *entry,
                                int longest_side,
                                char *out, size_t len);

    /**
     * Optional. Resolve one cache entry that arrived in an "unresolved"
     * state (extension == 0xFF). Called from the resolver loop in the
     * download task. On ESP_OK the implementation MUST have mutated
     * `entry` in place: iiif_key updated to the resolved key (e.g. Rijks
     * micrio short id), extension set to a valid image format (e.g. 3),
     * and resolve_fails reset to 0. On failure, the caller increments
     * resolve_fails and tombstones at 3.
     *
     * NULL for museums that don't need resolution (AIC and any future
     * museum that already returns the IIIF id directly).
     */
    esp_err_t (*resolve_entry)(institution_channel_entry_t *entry);

    /**
     * Optional. True when this museum is BYOK (the user must supply their
     * own API key) and that key is currently unset, so refresh can never
     * run. NULL for museums that ship a built-in / public key — those are
     * never "missing". Mirrors the resolve_entry optional-callback idiom.
     *
     * Used purely for UI signalling (home-page banner/badge, Settings
     * badge, on-device "needs a key" message); it does NOT gate refresh,
     * which already no-ops on an empty key inside refresh_channel.
     */
    bool (*api_key_missing)(void);
} art_institution_museum_t;

extern const art_institution_museum_t ART_INSTITUTION_MUSEUMS[];
extern const size_t ART_INSTITUTION_MUSEUM_COUNT;

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Initialize the art-institution subsystem
 *
 * Resets the rate-limit table. Safe to call multiple times. No I/O.
 *
 * @return ESP_OK on success.
 */
esp_err_t art_institution_init(void);

// ============================================================================
// Dispatch helpers (used by play_scheduler and download_manager)
// ============================================================================

/**
 * @brief Look up a museum dispatch entry by wire id
 *
 * @param museum_id Stable wire id ("artic", ...)
 * @return Pointer to dispatch entry in ART_INSTITUTION_MUSEUMS, or NULL.
 */
const art_institution_museum_t *art_institution_find(const char *museum_id);

/**
 * @brief Map a wire id ("artic", "si", ...) to its museum_id_t ordinal
 *
 * Thin wrappers around art_institution_find() that exist so callers can
 * avoid pulling in the full dispatch-entry struct. Used by p3a_core (which
 * cannot REQUIRE art_institution without creating a cycle) via weak extern
 * declarations.
 *
 * @return enum ordinal on hit, -1 on miss / NULL input.
 */
int art_institution_enum_from_id(const char *museum_id);

/**
 * @brief Map a museum_id_t ordinal back to its wire id
 *
 * @return Pointer into the static dispatch table on hit, NULL on out-of-range.
 */
const char *art_institution_id_from_enum(uint16_t museum_enum);

/**
 * @brief True iff a museum is BYOK and its API key is currently unset
 *
 * Thin dispatch over the per-museum api_key_missing callback. Returns false
 * for unknown ids and for museums that ship a built-in/public key (those
 * have no callback and can never be "missing"). UI-only signal — does not
 * affect refresh, which independently no-ops on an empty key.
 *
 * @param museum_id Stable wire id ("ham", "si", ...)
 */
bool art_institution_api_key_missing(const char *museum_id);

/**
 * @brief Parse a channel-spec `name` of the form "{museum_id}:{axis}"
 *
 * The two output buffers receive null-terminated strings. The colon is
 * required; if absent, returns ESP_ERR_INVALID_ARG and leaves outputs
 * empty.
 *
 * @param spec_name   The full name string from ps_channel_spec_t.name
 * @param out_museum  Buffer for the museum id (suggest >= 16 bytes)
 * @param museum_len  Size of out_museum
 * @param out_axis    Buffer for the axis (suggest >= 32 bytes)
 * @param axis_len    Size of out_axis
 * @return ESP_OK on a well-formed spec.
 */
esp_err_t art_institution_parse_spec(const char *spec_name,
                                     char *out_museum, size_t museum_len,
                                     char *out_axis, size_t axis_len);

/**
 * @brief Refresh a single institution channel
 *
 * Parses the spec, looks up the museum, paginates the listing API, and
 * merges entries into the cache (resolving the cache from the registry on
 * each page under channel_cache_lifecycle_lock() — same pattern Giphy
 * uses, so a concurrent playset switch can't free the cache mid-refresh).
 *
 * @param channel_id     Channel id (used to re-resolve cache between pages)
 * @param spec_name      Channel spec name ("artic:departments")
 * @param identifier     Term id ("PC-4")
 * @param channel_offset Per-playset starting offset into the listing
 * @return ESP_OK on success, ESP_ERR_INVALID_RESPONSE on HTTP 429,
 *         ESP_ERR_NOT_FOUND for unknown museum, other codes on error.
 */
esp_err_t art_institution_refresh_by_spec(const char *channel_id,
                                          const char *spec_name,
                                          const char *identifier,
                                          uint32_t channel_offset);

/**
 * @brief Build the SD-card vault path for one entry
 *
 * Layout: /sdcard/p3a/museum/{museum_id}/{d0}/{d1}/{iiif_key}.{ext}
 *
 * The d_i are the 6-bit decimal shard dirs derived from the sanitized
 * iiif_key, matching the vault and giphy conventions; built via the shared
 * sd_path_build_sharded() helper (depth = SD_SHARD_DEPTH), which also
 * FAT-sanitizes the leaf filename.
 *
 * @param museum_id  Stable wire id
 * @param entry      Cache entry; iiif_key and extension are read
 * @param out_path   Output buffer (suggest >= 256 bytes)
 * @param out_len    Size of out_path
 */
esp_err_t art_institution_build_vault_path(const char *museum_id,
                                           const institution_channel_entry_t *entry,
                                           char *out_path, size_t out_len);

/**
 * @brief Build vault path from a channel spec name + entry
 *
 * Convenience wrapper: parses the museum prefix out of spec_name (the
 * portion before the colon — e.g. "artic" from "artic:departments") and
 * forwards to art_institution_build_vault_path(). Used by the picker and
 * download manager so neither needs to know the spec parsing rules.
 */
esp_err_t art_institution_build_vault_path_from_spec(const char *spec_name,
                                                     const institution_channel_entry_t *entry,
                                                     char *out_path, size_t out_len);

/**
 * @brief Map a museum's iiif_key string to a salted DJB2 post_id
 *
 * The salt is namespaced with the museum id so the same iiif_key in two
 * museums (extremely unlikely but theoretically possible) yields distinct
 * post_ids. The returned int32 is non-zero and non-negative.
 */
int32_t art_institution_compute_post_id(const char *museum_id, const char *iiif_key);

/**
 * @brief Build the IIIF image URL for one entry (per-museum dispatch)
 *
 * Public wrapper around the dispatch table's build_iiif_url. The download
 * manager uses this to display the source URL in download_request_t for
 * logs / future inspection, even though art_institution_download_entry()
 * internally re-derives the URL.
 *
 * @param museum_id    Stable wire id ("artic", ...)
 * @param entry        Cache entry
 * @param longest_side Pixel cap (M1 hardcodes 720)
 * @param out          Output URL buffer (suggest >= 256 bytes)
 * @param len          Size of out
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND for unknown museum.
 */
esp_err_t art_institution_build_iiif_url(const char *museum_id,
                                         const institution_channel_entry_t *entry,
                                         int longest_side,
                                         char *out, size_t len);

/**
 * @brief Stream an IIIF image URL to a vault path
 *
 * Called by the download manager once per missing entry. Uses the same
 * chunked-retry pattern as Giphy's downloader. Handles parent-directory
 * creation, atomic temp-file rename, SDIO bus contention, and USB-MSC
 * export aborts.
 *
 * The museum_id parameter is consulted for two things:
 *   - 429 cooldown engagement (art_institution_set_rate_limited).
 *   - (Future) per-museum download-time headers — none today.
 *
 * The URL and out_path are pre-built by the download manager via
 * art_institution_build_iiif_url() and
 * art_institution_build_vault_path_from_spec(); this function does no
 * URL/path construction itself.
 *
 * @param museum_id Stable wire id ("artic", ...)
 * @param url       Full IIIF image URL (already built)
 * @param out_path  Target vault path (must be writable; parent dirs may be missing)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND for HTTP 404,
 *         ESP_ERR_INVALID_RESPONSE for HTTP 429,
 *         ESP_ERR_INVALID_SIZE if the body exceeds P3A_MAX_ARTWORK_SIZE,
 *         ESP_ERR_INVALID_STATE if SD card was exported to USB mid-stream,
 *         other codes for I/O errors.
 */
esp_err_t art_institution_download_to_path(const char *museum_id,
                                           const char *url,
                                           const char *out_path);

/**
 * @brief Resolve one pending unresolved entry across all active channels
 *
 * Walks the active institution channels looking for an entry with
 * extension == 0xFF (and resolve_fails < 3), invokes the museum's
 * resolve_entry function, and mutates the cache entry in place. On
 * three consecutive failures the entry is tombstoned (extension =
 * 0xFE) so the download manager stops considering it.
 *
 * Designed to be called once per download_task loop iteration: doing
 * a single 3-hop Linked-Art walk costs a few hundred ms of HTTP +
 * JSON-LD parsing, which we interleave with regular downloads instead
 * of bunching into a long blocking pass.
 *
 * Cheap no-op when no museum exposes resolve_entry, or when there are
 * no unresolved entries.
 *
 * @return ESP_OK if an entry was resolved (or tombstoned),
 *         ESP_ERR_NOT_FOUND if no unresolved entry was found,
 *         other codes propagated from the resolver.
 */
esp_err_t art_institution_resolve_pending(void);

// ============================================================================
// Rate-limit cooldown (shared between refresh + download + browser reports)
// ============================================================================

/**
 * @brief Engage the cooldown for a museum
 *
 * Pass 0 for cooldown_sec to honor a museum-specific default (60 s for
 * AIC and Rijks today). If the cooldown is already further out, the
 * existing deadline is preserved.
 *
 * @param museum_id Stable wire id; unknown ids are ignored.
 */
void art_institution_set_rate_limited(const char *museum_id,
                                      uint32_t cooldown_sec);

/**
 * @brief Check whether a museum is currently in cooldown
 */
bool art_institution_is_rate_limited(const char *museum_id);

/**
 * @brief Seconds remaining in the cooldown for a museum (0 if not active)
 */
uint32_t art_institution_rate_limit_remaining(const char *museum_id);

#ifdef __cplusplus
}
#endif
