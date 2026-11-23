// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_intro_anim_list.h
 * @brief Public read-only view of the intro-animation registry.
 *
 * Exposes just enough for the HTTP API and web UI to enumerate registered
 * animation names. The full registry struct lives in the private
 * intro_anims/intro_anim.h to keep the animation interface internal.
 */

#ifndef P3A_INTRO_ANIM_LIST_H
#define P3A_INTRO_ANIM_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Number of registered intro animations.
 */
int p3a_intro_anim_get_count(void);

/**
 * @brief Get the kebab-case name of the animation at index `idx`.
 *
 * @param idx 0..p3a_intro_anim_get_count()-1
 * @return Stable string pointer, or NULL if idx is out of range.
 */
const char *p3a_intro_anim_get_name(int idx);

#ifdef __cplusplus
}
#endif

#endif /* P3A_INTRO_ANIM_LIST_H */
