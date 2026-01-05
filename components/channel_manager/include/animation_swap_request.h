// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdcard_channel.h"  // asset_type_t

#ifdef __cplusplus
extern "C" {
#endif

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
    int32_t post_id;           // For view tracking (0 if not applicable)
    uint32_t dwell_time_ms;    // Effective dwell time for this artwork
    uint64_t start_time_ms;    // For Live Mode alignment (0 = ignore)
    uint32_t start_frame;      // For Live Mode alignment (0 = start from beginning)
    bool is_live_mode;         // Whether this maintains Live Mode synchronization
} swap_request_t;

#ifdef __cplusplus
}
#endif





