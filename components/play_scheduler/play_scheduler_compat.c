// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_compat.c
 * @brief Compatibility shims for legacy symbols removed during refactors.
 */

#include "play_scheduler.h"

/**
 * @brief Legacy hook used by live_mode/swap_future and dev HTTP debug endpoints.
 *
 * The old auto-swap task/timer lived in main; it was moved into play_scheduler.
 */
void auto_swap_reset_timer(void)
{
    play_scheduler_reset_timer();
}





