// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file debug_http_log.h
 * @brief Debug performance logging with pre-aggregation (compile-time optional)
 * 
 * Set CONFIG_P3A_PERF_DEBUG to 1 to enable performance instrumentation.
 * When disabled (default), all logging functions become no-ops with zero overhead.
 */

#pragma once

#include "esp_timer.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration - Set to 1 to enable performance debugging
// ============================================================================
#ifndef CONFIG_P3A_PERF_DEBUG
#define CONFIG_P3A_PERF_DEBUG 0  // OFF by default
#endif

#define DEBUG_PERF_REPORT_INTERVAL 1000  // Report every N frames (when enabled)

// ============================================================================
// Performance Stats Collection API
// ============================================================================

#if CONFIG_P3A_PERF_DEBUG

/**
 * @brief Record a frame's timing data
 */
void debug_perf_record_frame(bool is_target,
                             int64_t decode_us,
                             int64_t upscale_us,
                             int64_t total_us,
                             int64_t target_delay_ms);

/**
 * @brief Record WebP decoder details
 */
void debug_perf_record_decode_detail(bool is_target,
                                     int64_t webp_lib_decode_us,
                                     int64_t convert_or_blend_us,
                                     bool has_alpha,
                                     int64_t pixel_count);

/**
 * @brief Force output of current stats
 */
void debug_perf_flush_stats(void);

/**
 * @brief Get current time in microseconds
 */
static inline int64_t debug_timer_now_us(void) {
    return esp_timer_get_time();
}

#else  // CONFIG_P3A_PERF_DEBUG disabled - all functions become no-ops

#define debug_perf_record_frame(is_target, decode_us, upscale_us, total_us, target_delay_ms) ((void)0)
#define debug_perf_record_decode_detail(is_target, webp_lib_decode_us, convert_or_blend_us, has_alpha, pixel_count) ((void)0)
#define debug_perf_flush_stats() ((void)0)
#define debug_timer_now_us() (0)

#endif  // CONFIG_P3A_PERF_DEBUG

#ifdef __cplusplus
}
#endif
