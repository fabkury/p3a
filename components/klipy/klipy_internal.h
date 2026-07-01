// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file klipy_internal.h
 * @brief Private helpers shared between klipy_api.c and klipy_download.c.
 */

#pragma once

#include "cJSON.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Pick the best rendition URL from a Klipy `file` object
 *
 * Klipy `file` is `{hd|md|sm|xs} -> {gif|webp|mp4} -> {url,width,height,size}`.
 * The tiers are NOT a strict resolution ladder (md can exceed hd), so selection
 * is by explicit width/height: the largest-area rendition that fits within
 * 1.5x the screen on each axis, else the largest available. If the preferred
 * format is absent at every tier, falls back to the other format.
 *
 * @param file        cJSON `file` object
 * @param fmt_pref    "gif" or "webp" (preferred format)
 * @param screen_w    display width
 * @param screen_h    display height
 * @param out_url     receives a pointer INTO the cJSON tree (do not free)
 * @param out_w       receives chosen width
 * @param out_h       receives chosen height
 * @param out_used_gif true if the chosen rendition is the gif variant
 * @return true if a usable rendition was found
 */
bool klipy_pick_rendition(const cJSON *file, const char *fmt_pref,
                          uint16_t screen_w, uint16_t screen_h,
                          const char **out_url, uint16_t *out_w, uint16_t *out_h,
                          bool *out_used_gif);
