// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file lai_verify.h
 * @brief LAi verification sweep: paced reconciliation of LAi against disk
 *
 * The LAi (Locally Available index) is a persisted belief about which
 * artwork files exist on the SD card. When files vanish behind its back
 * (user deletion, storage eviction, MSC-host edits, layout changes), the
 * pick path only heals one entry per missing-file hit — far too slow for
 * mass staleness. This module audits a channel's entire LAi in paced
 * batches, evicting entries whose files are confirmed missing, so the
 * download manager can re-discover and re-fetch them.
 *
 * Sweeps are requested by the play scheduler's pick path (windowed
 * miss-rate threshold and retry-loop give-up) via lai_verify_request()
 * and executed exclusively inside the download manager task, one
 * time-budgeted slice of paced batches per task-loop iteration.
 */

#ifndef LAI_VERIFY_H
#define LAI_VERIFY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Outcome of one lai_verify_run_slice() call (last batch's result)
 */
typedef enum {
    LAI_VERIFY_IDLE = 0,  // No sweep pending or active
    LAI_VERIFY_RAN,       // Processed batch(es) (includes their pacing delays)
    LAI_VERIFY_GATED,     // Work pending but blocked (SD exported, SDIO locked, PICO-8)
} lai_verify_result_t;

/**
 * @brief Initialize the sweep engine (creates its mutex)
 *
 * Called once from download_manager_init(). Requests arriving before
 * initialization are silently dropped.
 */
void lai_verify_init(void);

/**
 * @brief Request a verification sweep of one channel's LAi
 *
 * Deduplicates per channel and applies a per-channel cooldown after a
 * completed sweep. Wakes the download task. Cheap and non-blocking:
 * safe to call while holding the play scheduler's state mutex (takes
 * only the engine's leaf mutex with a bounded timeout).
 *
 * @param channel_id Channel identifier (hex hash, as in ps_channel_state_t)
 */
void lai_verify_request(const char *channel_id);

/**
 * @brief Whether a sweep is pending or in progress
 */
bool lai_verify_has_work(void);

/**
 * @brief Run a time-budgeted slice of paced verification batches
 *
 * MUST only be called from the download manager task (shares its
 * single-task path-building buffers). Runs verification batches (16
 * stats + 50 ms pacing gap each) back-to-back until the ~500 ms slice
 * budget elapses, the work runs out, or a gate closes — so a deep
 * re-download queue doesn't throttle the sweep to one batch per
 * multi-second download. Each batch re-checks the SD/MSC/SDIO/PICO-8
 * gates, verifies LAi entries against disk, and evicts
 * confirmed-missing ones. On sweep completion: schedules a cache save,
 * notifies the play scheduler, and triggers a download rescan if any
 * entries were evicted.
 *
 * @return Last batch's outcome: LAI_VERIFY_IDLE / RAN / GATED
 */
lai_verify_result_t lai_verify_run_slice(void);

#ifdef __cplusplus
}
#endif

#endif // LAI_VERIFY_H
