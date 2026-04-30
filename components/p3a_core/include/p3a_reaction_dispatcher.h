// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_reaction_dispatcher.h
 * @brief Single entry point for sending reactions, regardless of trigger source.
 *
 * Both the touch router (swipe-up / swipe-down) and the HTTP API
 * (POST /action/reaction) call into this module. Each call:
 *
 *  1. Validates preconditions (e.g. MQTT connected, Giphy API key set);
 *  2. Shows the appropriate on-screen overlay (submit / revoke / click);
 *  3. For Makapix, optimistically flips the reaction-submitted flag on
 *     p3a_current_post so any web UI poll picks it up immediately;
 *  4. Spawns a fire-and-forget FreeRTOS task to perform the network I/O;
 *  5. Returns ESP_OK without waiting for the network call.
 *
 * If the network call fails, the spawned task shows the error overlay and
 * (for Makapix) reverts the reaction-submitted flag.
 */

#ifndef P3A_REACTION_DISPATCHER_H
#define P3A_REACTION_DISPATCHER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Submit a thumbs-up reaction to a Makapix post. Caller is expected to have
 * already verified that post_id matches the currently displayed post.
 *
 * @return ESP_OK if dispatched (the network call may still fail asynchronously);
 *         ESP_ERR_INVALID_ARG if post_id <= 0;
 *         ESP_ERR_INVALID_STATE if MQTT is not connected;
 *         ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t p3a_reaction_dispatch_makapix_submit(int32_t post_id);

/**
 * Revoke a previously-submitted thumbs-up reaction from a Makapix post.
 * Same return contract as p3a_reaction_dispatch_makapix_submit.
 */
esp_err_t p3a_reaction_dispatch_makapix_revoke(int32_t post_id);

/**
 * Register an "onclick" analytics event with Giphy for the given giphy_id.
 *
 * @return ESP_OK if dispatched;
 *         ESP_ERR_INVALID_ARG if giphy_id is empty;
 *         ESP_ERR_INVALID_STATE if Giphy API key or random_id is missing;
 *         ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t p3a_reaction_dispatch_giphy_click(const char *giphy_id);

#ifdef __cplusplus
}
#endif

#endif // P3A_REACTION_DISPATCHER_H
