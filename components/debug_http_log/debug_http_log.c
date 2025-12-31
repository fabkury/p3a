// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file debug_http_log.c
 * @brief Pre-aggregated performance statistics collection (compile-time optional)
 * 
 * This file is only compiled when CONFIG_P3A_PERF_DEBUG is enabled.
 */

#include "debug_http_log.h"

#if CONFIG_P3A_PERF_DEBUG

#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define TAG "PERF"

// ============================================================================
// Statistics structures
// ============================================================================

typedef struct {
    int64_t sum;
    int64_t min;
    int64_t max;
    uint32_t count;
} stat_t;

typedef struct {
    stat_t decode_us;
    stat_t upscale_us;
    stat_t total_us;
    stat_t target_delay_ms;
    stat_t webp_lib_us;
    stat_t convert_blend_us;
    stat_t pixel_count;
    uint32_t frames_with_alpha;
    uint32_t frames_without_alpha;
    uint32_t late_frames;  // frames where total_us > target_delay_ms * 1000
} perf_stats_t;

static perf_stats_t s_target_stats;
static perf_stats_t s_other_stats;
static uint32_t s_total_frame_count = 0;

// ============================================================================
// Helper functions
// ============================================================================

static void stat_init(stat_t *s) {
    s->sum = 0;
    s->min = INT64_MAX;
    s->max = 0;
    s->count = 0;
}

static void stat_add(stat_t *s, int64_t value) {
    s->sum += value;
    if (value < s->min) s->min = value;
    if (value > s->max) s->max = value;
    s->count++;
}

static int64_t stat_avg(const stat_t *s) {
    return (s->count > 0) ? (s->sum / (int64_t)s->count) : 0;
}

static void stats_init(perf_stats_t *p) {
    stat_init(&p->decode_us);
    stat_init(&p->upscale_us);
    stat_init(&p->total_us);
    stat_init(&p->target_delay_ms);
    stat_init(&p->webp_lib_us);
    stat_init(&p->convert_blend_us);
    stat_init(&p->pixel_count);
    p->frames_with_alpha = 0;
    p->frames_without_alpha = 0;
    p->late_frames = 0;
}

static void print_stats(const char *label, const perf_stats_t *p) {
    if (p->total_us.count == 0) return;
    
    int64_t avg_total = stat_avg(&p->total_us);
    int64_t avg_decode = stat_avg(&p->decode_us);
    int64_t avg_upscale = stat_avg(&p->upscale_us);
    int64_t avg_target = stat_avg(&p->target_delay_ms);
    int64_t avg_webp = stat_avg(&p->webp_lib_us);
    int64_t avg_blend = stat_avg(&p->convert_blend_us);
    
    // Calculate percentage of time budget used
    int64_t target_us = avg_target * 1000;
    int pct_used = (target_us > 0) ? (int)((avg_total * 100) / target_us) : 0;
    
    printf("PERF_STATS:%s frames=%lu late=%lu(%d%%) "
           "total_us[avg=%lld,min=%lld,max=%lld] "
           "decode_us[avg=%lld] upscale_us[avg=%lld] "
           "webp_lib_us[avg=%lld] blend_us[avg=%lld] "
           "target_ms[avg=%lld] budget_used=%d%% "
           "alpha=%lu noalpha=%lu\n",
           label,
           (unsigned long)p->total_us.count,
           (unsigned long)p->late_frames,
           (p->total_us.count > 0) ? (int)((p->late_frames * 100) / p->total_us.count) : 0,
           avg_total, p->total_us.min, p->total_us.max,
           avg_decode,
           avg_upscale,
           avg_webp,
           avg_blend,
           avg_target,
           pct_used,
           (unsigned long)p->frames_with_alpha,
           (unsigned long)p->frames_without_alpha);
}

// ============================================================================
// Public API
// ============================================================================

void debug_perf_record_frame(bool is_target,
                             int64_t decode_us,
                             int64_t upscale_us,
                             int64_t total_us,
                             int64_t target_delay_ms)
{
    perf_stats_t *p = is_target ? &s_target_stats : &s_other_stats;
    
    stat_add(&p->decode_us, decode_us);
    stat_add(&p->upscale_us, upscale_us);
    stat_add(&p->total_us, total_us);
    stat_add(&p->target_delay_ms, target_delay_ms);
    
    // Check if frame was late
    if (total_us > target_delay_ms * 1000) {
        p->late_frames++;
    }
    
    s_total_frame_count++;
    
    // Report every N frames
    if (s_total_frame_count % DEBUG_PERF_REPORT_INTERVAL == 0) {
        debug_perf_flush_stats();
    }
}

void debug_perf_record_decode_detail(bool is_target,
                                     int64_t webp_lib_decode_us,
                                     int64_t convert_or_blend_us,
                                     bool has_alpha,
                                     int64_t pixel_count)
{
    perf_stats_t *p = is_target ? &s_target_stats : &s_other_stats;
    
    stat_add(&p->webp_lib_us, webp_lib_decode_us);
    stat_add(&p->convert_blend_us, convert_or_blend_us);
    stat_add(&p->pixel_count, pixel_count);
    
    if (has_alpha) {
        p->frames_with_alpha++;
    } else {
        p->frames_without_alpha++;
    }
}

void debug_perf_flush_stats(void)
{
    printf("\n=== PERF REPORT (total frames: %lu) ===\n", (unsigned long)s_total_frame_count);
    print_stats("TARGET", &s_target_stats);
    print_stats("OTHER", &s_other_stats);
    printf("=== END PERF REPORT ===\n\n");
    
    // Reset stats for next interval
    stats_init(&s_target_stats);
    stats_init(&s_other_stats);
}

#endif  // CONFIG_P3A_PERF_DEBUG
