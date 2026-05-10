// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file animation_swap_request.h
 * @brief Swap request structure passed from play scheduler to animation player
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdcard_channel.h"  // asset_type_t
#include "play_scheduler_types.h"  // ps_channel_type_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How a swap request should report load failures.
 *
 * Zero-initialized swap requests default to SWAP_FAIL_SILENT, which matches
 * the dominant code path (timer auto-swap, touch navigation, channel
 * switches, boot fallback). Only paths where the user explicitly asked for
 * a specific artwork (HTTP play_artwork / play_local_file) opt in to
 * SWAP_FAIL_LOUD.
 *
 * Add new modes here as the failure-handling policy gains nuance.
 */
typedef enum {
    SWAP_FAIL_SILENT = 0,  // log only, retry the auto-swap up to 3 times, then escalate
    SWAP_FAIL_LOUD,        // surface "Failed to load artwork" overlay immediately, no retry
} swap_fail_mode_t;

/**
 * @brief Swap request structure for animation player
 *
 * This structure contains all information needed for animation_player
 * to perform a seamless transition to a new artwork. All fields are
 * pre-validated by the caller before being passed to animation_player.
 */
typedef struct swap_request_s {
    char filepath[256];        // Validated file path (file exists)
    asset_type_t type;         // File type (WebP, GIF, PNG, JPEG)
    ps_channel_type_t channel_type; // Channel type (for PPA upscale branching and view-event reporting)
    char channel_spec_name[33];     // Channel sub-type name ("all", "promoted", "user", ...) for view-event reporting
    char channel_identifier[33];    // USER/REACTIONS: sqid; HASHTAG: tag; empty otherwise
    int32_t post_id;           // For view tracking (0 if not applicable)
    post_source_t post_source; // Source of the post_id
    uint32_t dwell_time_ms;    // Effective dwell time for this artwork
    swap_fail_mode_t fail_mode;  // How load failures should be reported (default: SILENT)
} swap_request_t;

#ifdef __cplusplus
}
#endif








