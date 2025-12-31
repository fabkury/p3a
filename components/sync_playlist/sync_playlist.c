// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/*
 * Synchronized Playlist Engine (ms-accurate)
 *
 * This component maps wall-clock time to a deterministic position in a
 * fixed-duration cycle. It is used by Live Mode to ensure all devices
 * pick the same item at the same wall-clock time.
 *
 * Notes:
 * - The input `animations[]` order is assumed to already represent the
 *   intended playback order (server/created/random already resolved upstream).
 * - Timing uses milliseconds since Unix epoch (UTC).
 */

#include "sync_playlist.h"
#include <string.h>
#include <stdlib.h>

static struct {
    uint64_t start_ms;
    const animation_t *animations;
    animation_t *owned_animations;
    uint32_t count;
    sync_playlist_mode_t mode;
    bool live_enabled;

    uint64_t total_cycle_ms;

    // For change detection
    uint32_t last_index;

    // Manual index when live_enabled == false
    uint32_t manual_index;
} S = {0};

static uint64_t compute_total_cycle_ms(void)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < S.count; i++) {
        uint32_t d = S.animations ? S.animations[i].duration_ms : 0;
        if (d == 0) d = 1; // hard minimum
        total += d;
    }
    if (total == 0) total = 1;
    return total;
}

static void sp_init(uint64_t master_seed,
                    uint64_t playlist_start_ms,
                    const animation_t *animations,
                    uint32_t count,
                    sync_playlist_mode_t mode)
{
    (void)master_seed; // master_seed currently unused (order is pre-resolved upstream)
    // Replace any previously-owned schedule.
    if (S.owned_animations) {
        free(S.owned_animations);
        S.owned_animations = NULL;
    }
    memset(&S, 0, sizeof(S));
    S.start_ms = playlist_start_ms;
    if (animations && count > 0) {
        S.owned_animations = (animation_t *)malloc((size_t)count * sizeof(animation_t));
        if (S.owned_animations) {
            memcpy(S.owned_animations, animations, (size_t)count * sizeof(animation_t));
            S.animations = S.owned_animations;
        } else {
            // Fall back to borrowed pointer (caller must ensure lifetime).
            S.animations = animations;
        }
    } else {
        S.animations = NULL;
    }
    S.count = count;
    S.mode = mode;
    S.live_enabled = true;
    S.manual_index = 0;
    S.last_index = 0;
    S.total_cycle_ms = compute_total_cycle_ms();
}

static bool sp_update(uint64_t current_time_ms,
                      uint32_t *out_index,
                      uint32_t *out_elapsed_in_anim_ms)
{
    uint32_t prev = S.last_index;

    if (!S.animations || S.count == 0) {
        if (out_index) *out_index = 0;
        if (out_elapsed_in_anim_ms) *out_elapsed_in_anim_ms = 0;
        S.last_index = 0;
        return (prev != 0);
    }

    if (!S.live_enabled) {
        uint32_t idx = (S.manual_index < S.count) ? S.manual_index : 0;
        S.last_index = idx;
        if (out_index) *out_index = idx;
        if (out_elapsed_in_anim_ms) *out_elapsed_in_anim_ms = 0;
        return (prev != idx);
    }

    uint64_t elapsed_ms = 0;
    if (current_time_ms > S.start_ms) {
        elapsed_ms = current_time_ms - S.start_ms;
    }

    if (S.mode == SYNC_MODE_FORGIVING) {
        // FORGIVING: advance at a coarse interval ~average dwell.
        uint32_t avg_ms = (uint32_t)(S.total_cycle_ms / (uint64_t)S.count);
        if (avg_ms < 1) avg_ms = 1;
        uint64_t step = elapsed_ms / (uint64_t)avg_ms;
        uint32_t idx = (uint32_t)(step % (uint64_t)S.count);
        S.last_index = idx;
        if (out_index) *out_index = idx;
        if (out_elapsed_in_anim_ms) *out_elapsed_in_anim_ms = (uint32_t)(elapsed_ms % (uint64_t)avg_ms);
        return (prev != idx);
    }

    // PRECISE: exact position within the cycle.
    const uint64_t pos = elapsed_ms % S.total_cycle_ms;
    uint64_t spent = 0;
    uint32_t idx = 0;
    uint32_t elapsed_in = 0;

    for (uint32_t i = 0; i < S.count; i++) {
        uint32_t d = S.animations[i].duration_ms;
        if (d == 0) d = 1;
        uint64_t next = spent + (uint64_t)d;
        if (next > pos) {
            idx = i;
            elapsed_in = (uint32_t)(pos - spent);
            break;
        }
        spent = next;
        // If we somehow didn't break (shouldn't), default to last index.
        if (i == S.count - 1) {
            idx = S.count - 1;
            elapsed_in = 0;
        }
    }

    S.last_index = idx;
    if (out_index) *out_index = idx;
    if (out_elapsed_in_anim_ms) *out_elapsed_in_anim_ms = elapsed_in;
    return (prev != idx);
}

static void sp_next(void)
{
    if (S.count == 0) return;
    if (S.live_enabled) return; // Live Mode swaps are time-driven; manual next is handled upstream.
    S.manual_index = (S.manual_index + 1) % S.count;
}

static void sp_prev(void)
{
    if (S.count == 0) return;
    if (S.live_enabled) return;
    S.manual_index = (S.manual_index == 0) ? (S.count - 1) : (S.manual_index - 1);
}

static void sp_jump_steps(int64_t steps)
{
    if (S.count == 0) return;
    if (S.live_enabled) return;
    int64_t m = (int64_t)S.manual_index + steps;
    m %= (int64_t)S.count;
    if (m < 0) m += (int64_t)S.count;
    S.manual_index = (uint32_t)m;
}

static void sp_enable_live(bool enable)
{
    S.live_enabled = enable;
    if (!enable) {
        // Freeze at last computed index for manual stepping.
        S.manual_index = (S.last_index < S.count) ? S.last_index : 0;
    }
}

const sync_playlist_t SyncPlaylist = {
    .init        = sp_init,
    .update      = sp_update,
    .next        = sp_next,
    .prev        = sp_prev,
    .jump_steps  = sp_jump_steps,
    .enable_live = sp_enable_live,
};

uint32_t sync_playlist_get_count(void)
{
    return S.count;
}

esp_err_t sync_playlist_get_duration_ms(uint32_t index, uint32_t *out_duration_ms)
{
    if (!out_duration_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!S.animations || S.count == 0 || index >= S.count) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t d = S.animations[index].duration_ms;
    if (d == 0) d = 1;
    *out_duration_ms = d;
    return ESP_OK;
}