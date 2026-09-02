// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_idle_wait.h
 * @brief Identity of the SD busy-wait variant linked into this build.
 *
 * Two independent facts, both decided at build/link time:
 *  - whether p3a's yielding wrapper replaced IDF's sdmmc_wait_for_idle()
 *    (CONFIG_P3A_SD_IDLE_WAIT_WRAP, jitter fix 8);
 *  - whether the IDF tree itself carries the CMD13 back-off fix proposed in
 *    esp-idf issue #19034 (detected by the presence of its new symbol
 *    sdmmc_poll_delay_and_backoff()).
 *
 * Reported by GET /api/debug/frames/stats (frame-trace builds) so that an
 * A/B run can prove which variant the device is running.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True when the yielding wrapper is linked over IDF's sdmmc_wait_for_idle(). */
bool sd_idle_wait_wrap_enabled(void);

/** True when the linked IDF carries the esp-idf #19034 back-off patch. */
bool sd_idle_wait_idf_patched(void);

#ifdef __cplusplus
}
#endif
