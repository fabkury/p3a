// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_current_post.h
 * @brief Tracks the post currently committed to the display.
 *
 * Updated by animation_player after each successful buffer swap. Queried by:
 *   - the touch router, to decide whether swipe-up/down should submit or
 *     revoke a reaction (only valid on Makapix posts)
 *   - the /status REST endpoint, to report the current post_id
 *   - makapix internal code (status publishing, connection bookkeeping)
 *
 * The `source` values match `post_source_t` from play_scheduler_types.h:
 *   0 = NONE, 1 = MAKAPIX, 2 = GIPHY, 3 = SDCARD.
 * The type is represented as `int` here so this header does not drag in
 * play_scheduler_types.h (which would create a circular component dep).
 */

#ifndef P3A_CURRENT_POST_H
#define P3A_CURRENT_POST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Publish the post currently on screen. `source` is a post_source_t value.
 *
 * @param giphy_id Original Giphy string ID (null-terminated, max 23 chars).
 *                 Pass NULL or "" for non-Giphy sources; the field is then
 *                 cleared so a stale value cannot leak into a later swipe.
 * @param filepath On-disk path to the currently displayed artwork
 *                 (e.g., /sdcard/p3a/vault/.../{uuid}.webp). NULL or ""
 *                 clears the field. Consumed by the pin dispatcher.
 */
void p3a_current_post_set(int32_t post_id, int source, const char *giphy_id,
                          const char *filepath);

/** Reset to "nothing on screen" (post_id=0, source=POST_SOURCE_NONE). */
void p3a_current_post_clear(void);

/** Returns the most recently published post_id (0 if none). */
int32_t p3a_current_post_get_id(void);

/** Returns the most recently published source (0 / POST_SOURCE_NONE if none). */
int p3a_current_post_get_source(void);

/**
 * Copy the most recently published Giphy ID into out (null-terminated).
 * Empty string when the current source is not GIPHY or no ID was published.
 *
 * @param out     Output buffer (always written; empty on no-ID).
 * @param max_len Size of out (must be >= 1).
 */
void p3a_current_post_get_giphy_id(char *out, size_t max_len);

/**
 * Copy the most recently published on-disk filepath into out (null-terminated).
 * Empty string when no filepath was published. Consumed by the pin dispatcher.
 *
 * @param out     Output buffer (always written; empty when no filepath).
 * @param max_len Size of out (must be >= 1).
 */
void p3a_current_post_get_filepath(char *out, size_t max_len);

/**
 * Set whether the user has submitted a thumbs-up reaction to the currently
 * displayed Makapix post. Resets to false automatically whenever
 * p3a_current_post_set() is called with a new post_id (or p3a_current_post_clear()
 * is called). Used by the touch router and the web UI reaction button so both
 * UIs converge on the same state.
 */
void p3a_current_post_set_reaction_submitted(bool submitted);

/** Returns the reaction-submitted flag for the currently displayed post. */
bool p3a_current_post_get_reaction_submitted(void);

#ifdef __cplusplus
}
#endif

#endif // P3A_CURRENT_POST_H
