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

#ifdef __cplusplus
extern "C" {
#endif

/** Publish the post currently on screen. `source` is a post_source_t value. */
void p3a_current_post_set(int32_t post_id, int source);

/** Reset to "nothing on screen" (post_id=0, source=POST_SOURCE_NONE). */
void p3a_current_post_clear(void);

/** Returns the most recently published post_id (0 if none). */
int32_t p3a_current_post_get_id(void);

/** Returns the most recently published source (0 / POST_SOURCE_NONE if none). */
int p3a_current_post_get_source(void);

#ifdef __cplusplus
}
#endif

#endif // P3A_CURRENT_POST_H
