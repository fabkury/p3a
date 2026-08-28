// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file frame_trace.h
 * @brief Presentation-lateness frame trace for the jitter work stream.
 *
 * Everything in this header compiles to nothing unless CONFIG_P3A_FRAME_TRACE
 * is set: release builds carry no code and no data. See docs/jitter/PLAN.md §4.
 *
 * Two record types share one PSRAM ring buffer:
 *  - FRAME: written by the display consumer at esp_lcd_panel_draw_bitmap time.
 *    lateness_us = present - target is THE jitter metric. ready_margin_us =
 *    target - produce_end; negative means the producer was late (decode
 *    overrun, out of scope for the work stream but recorded and flagged).
 *  - MARK: written by any task via frame_trace_mark() to timestamp events that
 *    might stall the pipeline (NVS commit, loader load, download, MQTT rx, ...).
 *
 * The ring is lock-free multi-writer: a slot is reserved with an atomic
 * fetch-add on the sequence counter, filled, then published by storing the
 * sequence into the entry last (release). Readers validate entry.seq.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FT_TYPE_FRAME = 0,
    FT_TYPE_MARK  = 1,
} ft_type_t;

typedef enum {
    FT_PHASE_EVENT = 0,
    FT_PHASE_BEGIN = 1,
    FT_PHASE_END   = 2,
} ft_phase_t;

// Keep in sync with ft_mark_kind_name() in frame_trace.c and host/jitter-lab/analyze.py.
typedef enum {
    FT_MARK_NONE = 0,
    FT_MARK_NVS_COMMIT,     // arg: site id
    FT_MARK_LOADER_LOAD,    // arg: asset type
    FT_MARK_DOWNLOAD,       // arg: bytes (END) / 0
    FT_MARK_MQTT_RX,        // arg: payload bytes
    FT_MARK_SWAP,           // arg: new generation (content discontinuity)
    FT_MARK_REFRESH,        // arg: channel type
    FT_MARK_HTTP_REQ,       // arg: method (1 GET, 2 POST, 3 PUT, 4 DELETE)
    FT_MARK_LOG_SLOW,       // arg: bytes; a single log call took > 2 ms
    FT_MARK_SNAPSHOT,       // arg: us spent in uxTaskGetSystemState (reporter)
    FT_MARK_PROVOKE,        // arg: provoke kind (dev endpoints)
    FT_MARK_SD_WRITE,       // arg: bytes
    FT_MARK_RESYNC,         // arg: drift us (consumer forfeited time)
    FT_MARK_MODE_SWITCH,    // arg: new display mode
    FT_MARK_SD_READ,        // arg: bytes (link-time wrap of sdmmc_read_sectors, diag builds)
    FT_MARK_FLASH_OP,       // arg: (op<<28)|len, op 1 read 2 write 3 erase (wrap of esp_flash_*)
    FT_MARK_VERIFY,         // lai_verify_run_slice (download_mgr sweep)
    FT_MARK_USER = 32,      // free-form, arg-defined
    FT_MARK_COUNT_ = 48,
} ft_mark_kind_t;

// Frame flags
#define FT_FLAG_MAX_SPEED   (1u << 0)   // max-speed playback, no playhead
#define FT_FLAG_BASELINED   (1u << 1)   // first frame after init/discontinuity (lateness forced 0)
#define FT_FLAG_RESYNCED    (1u << 2)   // consumer resynced the playhead on this frame
#define FT_FLAG_UI_MODE     (1u << 3)   // frame rendered by the UI path, not the animation callback
#define FT_FLAG_BLACK       (1u << 4)   // paused / brightness-zero black frame

typedef struct __attribute__((packed)) {
    uint32_t seq;               // publication sequence (0 = empty slot)
    uint8_t  type;              // ft_type_t
    uint8_t  kind;              // ft_mark_kind_t (MARK) / 0
    uint8_t  phase;             // ft_phase_t (MARK) / 0
    uint8_t  flags;             // FT_FLAG_* (FRAME) / 0
    int64_t  t_us;              // present time (FRAME) / event time (MARK), esp_timer clock
    int32_t  lateness_us;       // FRAME: present - target
    int32_t  ready_margin_us;   // FRAME: target - produce_end (negative = producer late)
    uint32_t produce_us;        // FRAME: frame callback wall time (decode + upscale + overlays)
    uint32_t decode_us;         // FRAME: decoder time inside the callback (0 if unknown)
    uint32_t upscale_us;        // FRAME: upscale time inside the callback (0 if unknown)
    uint32_t free_wait_us;      // FRAME: producer blocked waiting for a free buffer
    uint32_t vsync_wait_us;     // FRAME: consumer blocked in the vsync alignment loop
    uint16_t duration_ms;       // FRAME: intended on-screen duration
    uint8_t  queue_depth;       // FRAME: ready-queue occupancy after dequeue
    uint8_t  core;              // MARK: core the writer ran on; FRAME: 1
    uint32_t arg;               // MARK: kind-specific; FRAME: generation
    uint32_t task_tag;          // MARK: FNV-1a of the writer task name (0 in ISR)
} ft_entry_t;                   // 56 bytes

_Static_assert(sizeof(ft_entry_t) == 56, "ft_entry_t must stay 56 bytes (host parser)");

// Consumer-side frame record input. All times in esp_timer microseconds.
typedef struct {
    int64_t  target_us;
    int64_t  present_us;
    int64_t  produce_start_us;
    int64_t  produce_end_us;
    uint32_t decode_us;
    uint32_t upscale_us;
    uint32_t free_wait_us;
    uint32_t vsync_wait_us;
    uint32_t duration_ms;
    uint32_t generation;
    uint8_t  queue_depth;
    uint8_t  flags;
} ft_frame_in_t;

typedef struct {
    uint32_t frames;
    uint32_t marks;
    uint32_t stalls_warn;       // lateness >= WARN_MS (includes hard)
    uint32_t stalls_hard;       // lateness >= STALL_MS
    uint32_t overrun_frames;    // ready_margin < 0
    uint32_t stalls_overrun;    // lateness >= STALL_MS explained by a normal-for-this-artwork producer time (no UART report)
    int32_t  lateness_max_us;
    int64_t  lateness_sum_us;   // of non-baselined frames, for the mean
    int32_t  worst_10s_us;      // worst lateness in the trailing ~10 s window
    uint32_t last_stall_seq;
    int64_t  last_stall_us;
    int64_t  since_us;          // stats epoch (reset time)
    uint32_t next_seq;          // next sequence to be written
    uint32_t oldest_seq;        // oldest sequence still in the ring
    uint32_t hist[12];          // lateness histogram, see FT_HIST_EDGES_US
} ft_stats_t;

// Histogram bucket upper edges (us): [0,1ms) [1,5) [5,17) [17,34) [34,50) [50,100) [100,250) [250,500) [500,1000) [1s,2s) [2s,5s) [5s,inf)
#define FT_HIST_EDGES_US { 1000, 5000, 17000, 34000, 50000, 100000, 250000, 500000, 1000000, 2000000, 5000000, INT32_MAX }

#if CONFIG_P3A_FRAME_TRACE

#include "esp_timer.h"

/** Allocate the ring in PSRAM, install the log hook, start the reporter task. Call once, early. */
void frame_trace_init(void);

/** Record a presented frame. Called from the display consumer only. */
void frame_trace_frame(const ft_frame_in_t *in);

/** Record an event marker from any task (not ISR-safe). */
void frame_trace_mark(ft_mark_kind_t kind, ft_phase_t phase, uint32_t arg);

/** Producer-side helper: render_next_frame reports its decode/upscale split for the frame in flight. */
void frame_trace_producer_split(uint32_t decode_us, uint32_t upscale_us);
void frame_trace_producer_take_split(uint32_t *decode_us, uint32_t *upscale_us);

/** Copy entries with seq >= since_seq (oldest first). Returns count; *next_seq = seq to pass next time. */
size_t frame_trace_read(uint32_t since_seq, ft_entry_t *out, size_t max_entries, uint32_t *next_seq);

void frame_trace_get_stats(ft_stats_t *out);
void frame_trace_reset(void);

/** Worst lateness in the trailing ~10 s window and whether a stall fired within the last 2 s (overlay). */
int32_t frame_trace_overlay_worst_us(bool *stall_recent);

const char *frame_trace_mark_kind_name(uint8_t kind);

static inline int64_t frame_trace_now_us(void) { return esp_timer_get_time(); }

#else  // !CONFIG_P3A_FRAME_TRACE — every call vanishes

#define frame_trace_init()                              ((void)0)
#define frame_trace_frame(in)                           ((void)0)
#define frame_trace_mark(kind, phase, arg)              ((void)0)
#define frame_trace_producer_split(d, u)                ((void)0)
#define frame_trace_producer_take_split(d, u)           ((void)0)
#define frame_trace_read(since, out, max, next)         ((size_t)0)
#define frame_trace_get_stats(out)                      ((void)0)
#define frame_trace_reset()                             ((void)0)
#define frame_trace_overlay_worst_us(p)                 ((int32_t)0)
#define frame_trace_mark_kind_name(k)                   ("")
#define frame_trace_now_us()                            ((int64_t)0)

#endif

#ifdef __cplusplus
}
#endif
