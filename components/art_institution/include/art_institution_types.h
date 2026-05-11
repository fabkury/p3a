// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file art_institution_types.h
 * @brief Institution channel entry layout and museum id enum
 *
 * The 64-byte packed entry sits alongside makapix_channel_entry_t and
 * giphy_channel_entry_t in channel_cache_t->entries. The per-channel-type
 * dispatch (see play_scheduler_is_institution_channel) tells callers which
 * layout to reinterpret the bytes as.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Internal stable identifier for each museum
 *
 * The enum is for type-safe internal indexing (rate-limit table, dispatch
 * lookup). The user-facing identifier on the wire is the string id stored
 * in art_institution_museum_t.id ("artic", "rijks", ...).
 *
 * Append-only: ordinals are used as rate-limit table indices, so reordering
 * would corrupt cooldown state across upgrades. Adding a museum is appending
 * a new value before ART_INSTITUTION_NUM_MUSEUMS.
 */
typedef enum {
    ART_INSTITUTION_MUSEUM_ARTIC = 0,
    ART_INSTITUTION_MUSEUM_RIJKS = 1,
    ART_INSTITUTION_NUM_MUSEUMS  // sentinel; keep last. Distinct from the
                                 // extern const ART_INSTITUTION_MUSEUM_COUNT
                                 // (dispatch-table size) — the two must stay
                                 // numerically equal but live in different
                                 // namespaces (enum vs. object).
} museum_id_t;

/**
 * @brief Institution channel cache entry (64 bytes, packed)
 *
 * Same size as makapix_channel_entry_t / giphy_channel_entry_t so it
 * occupies one channel_cache slot without changing channel_cache_t.
 *
 * Discriminated at runtime by ps_channel_state_t.type ==
 * PS_CHANNEL_TYPE_INSTITUTION; not persisted in the cache file (the per-type
 * dispatch is sufficient).
 *
 * Field order is chosen to keep all multi-byte members naturally aligned
 * under __attribute__((packed)): post_id at 0, width at 6, created_at at 8,
 * height at 12. iiif_key starts at 14 and runs to 61; reserved fills 62-63.
 * This mirrors the alignment trick in giphy_channel_entry_t. The extension
 * sentinels 0xFF/0xFE are reserved for M2's Rijks Linked-Art walk per
 * docs/art-institutions/finalized-design.md §9.2.
 */
typedef struct __attribute__((packed)) {
    int32_t  post_id;        ///< offset  0 — salted DJB2 hash of "{museum}:{iiif_key}"
    uint8_t  kind;           ///< offset  4 — 0 = artwork (only value used today)
    uint8_t  extension;      ///< offset  5 — 0=webp, 1=gif, 2=png, 3=jpg (matches
                             ///<              makapix_channel_entry_t / giphy_channel_entry_t
                             ///<              so the picker's get_asset_type_from_extension
                             ///<              works without a special case). AIC uses 3 (jpg).
                             ///<              0xFF=unresolved (Rijks: iiif_key holds the HMO
                             ///<              id, awaiting Linked-Art walk).
                             ///<              0xFE=tombstone (Rijks: walk failed 3 times;
                             ///<              skip forever until next refresh re-adds the HMO
                             ///<              with a fresh resolve_fails=0 budget).
    uint16_t width;          ///< offset  6 — pixels at requested rendition (0 = unknown)
    uint32_t created_at;     ///< offset  8 — Unix timestamp from museum metadata (0 = unknown)
    uint16_t height;         ///< offset 12 — pixels at requested rendition (0 = unknown)
    char     iiif_key[48];   ///< offset 14 — null-terminated; museum-specific identifier
                             ///<              (AIC image_id, or Rijks HMO id while unresolved,
                             ///<              or Rijks micrio short id once resolved)
    uint8_t  resolve_fails;  ///< offset 62 — Rijks: consecutive failed Linked-Art walks for
                             ///<              this entry. Promotes to extension=0xFE at 3.
                             ///<              Unused (0) for museums that don't need resolution.
    uint8_t  _reserved;      ///< offset 63 — future use (zeroed on write)
} institution_channel_entry_t;

_Static_assert(sizeof(institution_channel_entry_t) == 64, "institution entry must be 64 bytes");

/**
 * @brief DJB2 salt for institution post_id generation
 *
 * Distinct from GIPHY_DJB2_SALT and the Makapix hash space so the three
 * post_id pools don't collide.
 * 0x41494e53 = "AINS" in ASCII (Art INStitution).
 */
#define ART_INSTITUTION_DJB2_SALT 0x41494e53u

#ifdef __cplusplus
}
#endif
