// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file frame_trace.c
 * @brief Presentation-lateness frame trace: PSRAM ring, stats, stall reporter.
 *
 * Compiled only when CONFIG_P3A_FRAME_TRACE=y (see CMakeLists.txt). Design
 * notes in docs/jitter/PLAN.md §4. Hot-path cost: one atomic add, ~56 bytes of
 * PSRAM writes and a handful of integer ops per frame or mark. No logging from
 * the writers; the only output is the reporter task on core 0 at priority 2.
 */

#include "frame_trace.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_timer.h"

static const char *TAG = "frame_trace";

#define FT_ENTRIES          ((uint32_t)CONFIG_P3A_FRAME_TRACE_ENTRIES)
#define FT_WARN_US          ((int32_t)CONFIG_P3A_FRAME_TRACE_WARN_MS * 1000)
#define FT_STALL_US         ((int32_t)CONFIG_P3A_FRAME_TRACE_STALL_MS * 1000)
#define FT_REPORT_GAP_US    ((int64_t)CONFIG_P3A_FRAME_TRACE_REPORT_MIN_INTERVAL_S * 1000000LL)
#define FT_REPORT_WINDOW_US ((int64_t)CONFIG_P3A_FRAME_TRACE_REPORT_WINDOW_MS * 1000LL)
#define FT_WORST_BUCKETS    10          // trailing ~10 s, one bucket per second
#define FT_STALL_RECENT_US  2000000LL   // overlay red tick duration
#define FT_LOG_SLOW_US      2000        // a single log call slower than this is marked

// 1 when the diag overlay enables per-task run-time accounting, 0 otherwise
// (a plain trace-only build has neither Kconfig symbol defined).
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
#define FT_HAVE_RUNTIME_STATS 1
#else
#define FT_HAVE_RUNTIME_STATS 0
#endif
#define FT_MAX_TASKS        64

static ft_entry_t *s_ring = NULL;
static uint32_t s_next_seq = 1;          // atomic; seq 0 means "empty slot"
static ft_stats_t s_stats;               // single writer for frame fields (consumer); marks counter atomic
static int32_t s_worst[FT_WORST_BUCKETS];
static int64_t s_worst_sec[FT_WORST_BUCKETS];
static uint32_t s_ema_produce_us = 0;    // running mean of producer time for the current generation
static uint32_t s_ema_gen = UINT32_MAX;
static uint32_t s_split_decode_us = 0;   // producer-task-local hand-off (same task writes and reads)
static uint32_t s_split_upscale_us = 0;
static TaskHandle_t s_reporter = NULL;
static vprintf_like_t s_prev_vprintf = NULL;
static volatile bool s_in_log_hook = false;

static const char *const s_kind_names[FT_MARK_COUNT_] = {
    [FT_MARK_NONE]        = "none",
    [FT_MARK_NVS_COMMIT]  = "nvs_commit",
    [FT_MARK_LOADER_LOAD] = "loader_load",
    [FT_MARK_DOWNLOAD]    = "download",
    [FT_MARK_MQTT_RX]     = "mqtt_rx",
    [FT_MARK_SWAP]        = "swap",
    [FT_MARK_REFRESH]     = "refresh",
    [FT_MARK_HTTP_REQ]    = "http_req",
    [FT_MARK_LOG_SLOW]    = "log_slow",
    [FT_MARK_SNAPSHOT]    = "snapshot",
    [FT_MARK_PROVOKE]     = "provoke",
    [FT_MARK_SD_WRITE]    = "sd_write",
    [FT_MARK_RESYNC]      = "resync",
    [FT_MARK_MODE_SWITCH] = "mode_switch",
    [FT_MARK_SD_READ]     = "sd_read",
    [FT_MARK_FLASH_OP]    = "flash_op",
    [FT_MARK_VERIFY]      = "verify",
    [FT_MARK_USER]        = "user",
};

const char *frame_trace_mark_kind_name(uint8_t kind)
{
    if (kind < FT_MARK_COUNT_ && s_kind_names[kind]) return s_kind_names[kind];
    return "?";
}

// ---------------------------------------------------------------------------
// Ring primitives
// ---------------------------------------------------------------------------

static inline ft_entry_t *ft_reserve(uint32_t *seq_out)
{
    uint32_t seq = __atomic_fetch_add(&s_next_seq, 1, __ATOMIC_ACQ_REL);
    ft_entry_t *e = &s_ring[seq % FT_ENTRIES];
    __atomic_store_n(&e->seq, 0, __ATOMIC_RELAXED);   // invalidate while we fill
    *seq_out = seq;
    return e;
}

static inline void ft_publish(ft_entry_t *e, uint32_t seq)
{
    __atomic_store_n(&e->seq, seq, __ATOMIC_RELEASE);
}

static uint32_t ft_task_tag(void)
{
    if (xPortInIsrContext()) return 0;
    const char *name = pcTaskGetName(NULL);
    if (!name) return 0;
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

static void ft_mark_impl(ft_mark_kind_t kind, ft_phase_t phase, uint32_t arg, int64_t now_us, int32_t duration_us)
{
    if (!s_ring) return;
    uint32_t seq;
    ft_entry_t *e = ft_reserve(&seq);
    e->type = FT_TYPE_MARK;
    e->kind = (uint8_t)kind;
    e->phase = (uint8_t)phase;
    e->flags = 0;
    e->t_us = now_us;
    e->lateness_us = duration_us;
    e->ready_margin_us = 0;
    e->produce_us = 0;
    e->decode_us = 0;
    e->upscale_us = 0;
    e->free_wait_us = 0;
    e->vsync_wait_us = 0;
    e->duration_ms = 0;
    e->queue_depth = 0;
    e->core = (uint8_t)esp_cpu_get_core_id();
    e->arg = arg;
    e->task_tag = ft_task_tag();
    ft_publish(e, seq);
    __atomic_fetch_add(&s_stats.marks, 1, __ATOMIC_RELAXED);
}

void frame_trace_mark(ft_mark_kind_t kind, ft_phase_t phase, uint32_t arg)
{
    ft_mark_impl(kind, phase, arg, esp_timer_get_time(), 0);
}

void frame_trace_mark_span(ft_mark_kind_t kind, int64_t t0_us, uint32_t arg)
{
    int64_t now = esp_timer_get_time();
    int64_t d = now - t0_us;
    ft_mark_impl(kind, FT_PHASE_END, arg, now, (d > INT32_MAX) ? INT32_MAX : (int32_t)(d < 0 ? 0 : d));
}

static void ft_worst_update(int64_t now_us, int32_t lateness_us)
{
    int64_t sec = now_us / 1000000LL;
    int idx = (int)(sec % FT_WORST_BUCKETS);
    if (s_worst_sec[idx] != sec) {
        s_worst_sec[idx] = sec;
        s_worst[idx] = 0;
    }
    if (lateness_us > s_worst[idx]) s_worst[idx] = lateness_us;
}

void frame_trace_frame(const ft_frame_in_t *in)
{
    if (!s_ring || !in) return;

    int64_t lat64 = in->present_us - in->target_us;
    if (in->flags & (FT_FLAG_BASELINED | FT_FLAG_MAX_SPEED)) lat64 = 0;
    int32_t lateness = (lat64 > INT32_MAX) ? INT32_MAX : (lat64 < INT32_MIN ? INT32_MIN : (int32_t)lat64);

    int64_t margin64 = in->target_us - in->produce_end_us;
    if (in->flags & (FT_FLAG_BASELINED | FT_FLAG_MAX_SPEED)) margin64 = 0;
    int32_t margin = (margin64 > INT32_MAX) ? INT32_MAX : (margin64 < INT32_MIN ? INT32_MIN : (int32_t)margin64);

    int64_t produce64 = in->produce_end_us - in->produce_start_us;
    uint32_t produce_us = (produce64 < 0) ? 0 : (produce64 > UINT32_MAX ? UINT32_MAX : (uint32_t)produce64);

    uint32_t seq;
    ft_entry_t *e = ft_reserve(&seq);
    e->type = FT_TYPE_FRAME;
    e->kind = 0;
    e->phase = 0;
    e->flags = in->flags;
    e->t_us = in->present_us;
    e->lateness_us = lateness;
    e->ready_margin_us = margin;
    e->produce_us = produce_us;
    e->decode_us = in->decode_us;
    e->upscale_us = in->upscale_us;
    e->free_wait_us = in->free_wait_us;
    e->vsync_wait_us = in->vsync_wait_us;
    e->duration_ms = (in->duration_ms > UINT16_MAX) ? UINT16_MAX : (uint16_t)in->duration_ms;
    e->queue_depth = in->queue_depth;
    e->core = (uint8_t)esp_cpu_get_core_id();
    e->arg = in->generation;
    e->task_tag = 0;
    ft_publish(e, seq);

    // Stats (consumer is the only writer of these fields)
    s_stats.frames++;
    if (!(in->flags & (FT_FLAG_BASELINED | FT_FLAG_MAX_SPEED))) {
        s_stats.lateness_sum_us += lateness;
        if (lateness > s_stats.lateness_max_us) s_stats.lateness_max_us = lateness;
        if (margin < 0) s_stats.overrun_frames++;
        static const int32_t edges[12] = FT_HIST_EDGES_US;
        int b = 0;
        int32_t l = lateness < 0 ? 0 : lateness;
        while (b < 11 && l >= edges[b]) b++;
        s_stats.hist[b]++;
        ft_worst_update(in->present_us, lateness);
        // Producer-time EMA per generation (artwork epoch). A late frame whose
        // producer was itself late AND whose producer time is in line with this
        // artwork's norm is a decode overrun (out of scope): counted, never
        // reported on UART (a chronically slow artwork would otherwise flood the
        // console with a report every resync). A starved producer (anomalous
        // produce_us) or a consumer-side stall still reports.
        if (in->generation != s_ema_gen) {
            s_ema_gen = in->generation;
            s_ema_produce_us = produce_us;
        } else {
            s_ema_produce_us = s_ema_produce_us - (s_ema_produce_us >> 3) + (produce_us >> 3);
        }
        const bool producer_late = (margin < 0) && ((int64_t)-margin >= (int64_t)lateness - 16667);
        // 2x the running mean: heavy artworks vary +-50% frame to frame (RUN-02:
        // 80-137 ms on a 90 ms mean), a starved producer is far beyond that.
        const bool produce_anomalous = produce_us >= (2u * s_ema_produce_us);
        const bool overrun_explained = producer_late && !produce_anomalous;
        if (lateness >= FT_WARN_US) {
            s_stats.stalls_warn++;
            if (lateness >= FT_STALL_US) {
                s_stats.stalls_hard++;
                if (overrun_explained) {
                    s_stats.stalls_overrun++;
                } else {
                    s_stats.last_stall_seq = seq;
                    s_stats.last_stall_us = in->present_us;
                    if (s_reporter) {
                        (void)xTaskNotify(s_reporter, seq, eSetValueWithOverwrite);
                    }
                }
            }
        }
    }
}

void frame_trace_producer_split(uint32_t decode_us, uint32_t upscale_us)
{
    s_split_decode_us = decode_us;
    s_split_upscale_us = upscale_us;
}

void frame_trace_producer_take_split(uint32_t *decode_us, uint32_t *upscale_us)
{
    if (decode_us) *decode_us = s_split_decode_us;
    if (upscale_us) *upscale_us = s_split_upscale_us;
    s_split_decode_us = 0;
    s_split_upscale_us = 0;
}

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

size_t frame_trace_read(uint32_t since_seq, ft_entry_t *out, size_t max_entries, uint32_t *next_seq)
{
    if (!s_ring || !out || max_entries == 0) {
        if (next_seq) *next_seq = since_seq;
        return 0;
    }
    uint32_t head = __atomic_load_n(&s_next_seq, __ATOMIC_ACQUIRE);   // first unwritten seq
    // Leave a small guard band so a slot being overwritten right now is skipped.
    uint32_t oldest = (head > FT_ENTRIES - 8) ? (head - (FT_ENTRIES - 8)) : 1;
    uint32_t s = since_seq < oldest ? oldest : since_seq;
    if (s < 1) s = 1;
    size_t n = 0;
    while (s < head && n < max_entries) {
        const ft_entry_t *e = &s_ring[s % FT_ENTRIES];
        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) == s) {
            memcpy(&out[n], e, sizeof(ft_entry_t));
            if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) == s) {
                n++;
            }
        }
        s++;
    }
    if (next_seq) *next_seq = s;
    return n;
}

void frame_trace_get_stats(ft_stats_t *out)
{
    if (!out) return;
    memcpy(out, &s_stats, sizeof(*out));
    uint32_t head = __atomic_load_n(&s_next_seq, __ATOMIC_ACQUIRE);
    out->next_seq = head;
    out->oldest_seq = (head > FT_ENTRIES - 8) ? (head - (FT_ENTRIES - 8)) : 1;
    int32_t worst = 0;
    int64_t now_sec = esp_timer_get_time() / 1000000LL;
    for (int i = 0; i < FT_WORST_BUCKETS; i++) {
        if (now_sec - s_worst_sec[i] < FT_WORST_BUCKETS && s_worst[i] > worst) worst = s_worst[i];
    }
    out->worst_10s_us = worst;
}

void frame_trace_reset(void)
{
    // Ring contents are kept (sequence keeps climbing); only the aggregates restart.
    uint32_t marks = s_stats.marks;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.marks = marks;
    s_stats.since_us = esp_timer_get_time();
    memset(s_worst, 0, sizeof(s_worst));
    frame_trace_mark(FT_MARK_USER, FT_PHASE_EVENT, 0xE5E7u);
}

int32_t frame_trace_overlay_worst_us(bool *stall_recent)
{
    ft_stats_t st;
    frame_trace_get_stats(&st);
    if (stall_recent) {
        *stall_recent = (st.last_stall_us != 0) &&
                        (esp_timer_get_time() - st.last_stall_us < FT_STALL_RECENT_US);
    }
    return st.worst_10s_us;
}

// ---------------------------------------------------------------------------
// Log hook: time every log call; mark the slow ones (blocking UART evidence)
// ---------------------------------------------------------------------------

static int ft_log_vprintf(const char *fmt, va_list ap)
{
    int64_t t0 = esp_timer_get_time();
    int r = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > FT_LOG_SLOW_US && !s_in_log_hook) {
        s_in_log_hook = true;
        frame_trace_mark(FT_MARK_LOG_SLOW, FT_PHASE_EVENT, (uint32_t)((dt / 1000) << 16 | (r < 0 ? 0 : (r & 0xFFFF))));
        s_in_log_hook = false;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Reporter task (core 0, low priority): periodic run-time snapshots + JTR| stall reports
// ---------------------------------------------------------------------------

#if FT_HAVE_RUNTIME_STATS
// Per-task CPU accounting WITHOUT periodic uxTaskGetSystemState():
// that call holds the kernel lock in a critical section (interrupts off on the
// calling core) for ~3-4 ms while it walks every task and scans stacks. On the
// diag build that delayed the DSI vsync ISR on core 0 and produced a one-frame
// blue flash every few seconds (Fab, 2026-08-28). Instead: a task table
// (handles, names, affinity) is built by ONE full snapshot at init and rebuilt
// only when uxTaskGetNumberOfTasks() changes or at stall-report time (each
// rebuild is marked FT_MARK_SNAPSHOT, so any glitch it causes is attributable).
// The per-second poll reads each task's run-time counter through vTaskGetInfo()
// with the stack scan disabled: a few microseconds per task.
typedef struct {
    TaskHandle_t handle;
    char name[configMAX_TASK_NAME_LEN + 1];
    int core;
    uint32_t prio;
    uint32_t rt_prev;       // run-time counter at the last periodic poll
} ft_task_slot_t;

static TaskStatus_t *s_snap = NULL;           // scratch for full snapshots
static ft_task_slot_t *s_tasks = NULL;
static UBaseType_t s_task_n = 0;
static UBaseType_t s_task_count_seen = 0;
static int64_t s_poll_prev_us = 0;

static void ft_table_rebuild(void)
{
    int64_t t0 = esp_timer_get_time();
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(s_snap, FT_MAX_TASKS, &total);
    int64_t dt = esp_timer_get_time() - t0;
    frame_trace_mark(FT_MARK_SNAPSHOT, FT_PHASE_EVENT, (uint32_t)dt);
    for (UBaseType_t i = 0; i < n; i++) {
        ft_task_slot_t *t = &s_tasks[i];
        t->handle = s_snap[i].xHandle;
        strlcpy(t->name, s_snap[i].pcTaskName, sizeof(t->name));
        t->core = -1;
#if configTASKLIST_INCLUDE_COREID
        t->core = (int)s_snap[i].xCoreID;
#endif
        t->prio = (uint32_t)s_snap[i].uxCurrentPriority;
        t->rt_prev = s_snap[i].ulRunTimeCounter;
    }
    s_task_n = n;
    s_task_count_seen = uxTaskGetNumberOfTasks();
    s_poll_prev_us = t0;
}

// Periodic (1 s): refresh the baseline counters cheaply; rebuild the table only
// if the task population changed.
static void ft_poll(void)
{
    if (uxTaskGetNumberOfTasks() != s_task_count_seen) {
        ft_table_rebuild();
        return;
    }
    for (UBaseType_t i = 0; i < s_task_n; i++) {
        TaskStatus_t st;
        vTaskGetInfo(s_tasks[i].handle, &st, pdFALSE, eBlocked);   // eBlocked: skip state lookup
        s_tasks[i].rt_prev = st.ulRunTimeCounter;
    }
    s_poll_prev_us = esp_timer_get_time();
}

static void ft_print_runtime_delta(void)
{
    // Current counters vs the last periodic poll (<= 1 s ago): which tasks
    // consumed CPU during the window that contains the stall.
    int64_t now = esp_timer_get_time();
    int64_t window_us = now - s_poll_prev_us;
    printf("JTR|T window_us=%lld tasks=%u\n", (long long)window_us, (unsigned)s_task_n);
    for (UBaseType_t i = 0; i < s_task_n; i++) {
        TaskStatus_t st;
        vTaskGetInfo(s_tasks[i].handle, &st, pdFALSE, eBlocked);
        uint32_t delta = st.ulRunTimeCounter - s_tasks[i].rt_prev;
        if (window_us > 0 && (int64_t)delta * 200 < window_us) continue;   // < 0.5 %
        char core_s[16];
        if (s_tasks[i].core == (int)0x7FFFFFFF) snprintf(core_s, sizeof(core_s), "any");
        else if (s_tasks[i].core < 0) snprintf(core_s, sizeof(core_s), "?");
        else snprintf(core_s, sizeof(core_s), "%d", s_tasks[i].core);
        printf("JTR|T %s core=%s prio=%u run_us=%" PRIu32 "\n",
               s_tasks[i].name, core_s, (unsigned)st.uxCurrentPriority, delta);
    }
    // A stall report is a good moment to pick up tasks created since the last
    // rebuild (one marked full snapshot; it happens after the stall, not before).
    ft_table_rebuild();
}
#endif

static void ft_print_entry(const ft_entry_t *e)
{
    if (e->type == FT_TYPE_FRAME) {
        printf("JTR|F %" PRIu32 " %lld %" PRId32 " %" PRId32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %u %u 0x%02x %" PRIu32 "\n",
               e->seq, (long long)e->t_us, e->lateness_us, e->ready_margin_us, e->produce_us,
               e->decode_us, e->upscale_us, e->free_wait_us, e->vsync_wait_us,
               (unsigned)e->duration_ms, (unsigned)e->queue_depth, (unsigned)e->flags, e->arg);
    } else {
        printf("JTR|M %" PRIu32 " %lld %s %u %" PRIu32 " %u 0x%08" PRIx32 " %" PRId32 "\n",
               e->seq, (long long)e->t_us, frame_trace_mark_kind_name(e->kind), (unsigned)e->phase,
               e->arg, (unsigned)e->core, e->task_tag, e->lateness_us);
    }
}

static void ft_report(uint32_t stall_seq)
{
    // Locate the stall entry to anchor the window.
    ft_entry_t anchor;
    uint32_t next;
    if (frame_trace_read(stall_seq, &anchor, 1, &next) != 1 || anchor.seq != stall_seq) {
        printf("JTR|STALL seq=%" PRIu32 " (entry already overwritten)\nJTR|END\n", stall_seq);
        return;
    }
    frame_trace_mark(FT_MARK_USER, FT_PHASE_BEGIN, 1);   // the report itself, visible in the ring
    printf("JTR|STALL seq=%" PRIu32 " t_us=%lld lateness_ms=%" PRId32 " margin_ms=%" PRId32 " dur_ms=%u flags=0x%02x\n",
           anchor.seq, (long long)anchor.t_us, anchor.lateness_us / 1000, anchor.ready_margin_us / 1000,
           (unsigned)anchor.duration_ms, (unsigned)anchor.flags);
    // Walk backwards from the anchor to find the first entry inside the window, then print forward.
    const int64_t t_from = anchor.t_us - FT_REPORT_WINDOW_US;
    uint32_t start = stall_seq;
    ft_stats_t st;
    frame_trace_get_stats(&st);
    while (start > st.oldest_seq) {
        ft_entry_t e;
        uint32_t nn;
        if (frame_trace_read(start - 1, &e, 1, &nn) != 1 || e.seq != start - 1) break;
        if (e.t_us < t_from) break;
        start--;
    }
    // Print window (+ a few entries after the stall, they are usually already there)
    ft_entry_t buf[16];
    uint32_t s = start;
    int printed = 0;
    uint32_t skipped_short_sd = 0;
    for (;;) {
        size_t n = frame_trace_read(s, buf, 16, &next);
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) {
            if (buf[i].seq > stall_seq + 4) { n = 0; break; }
            // Keep the report small: a 30 KB report at 115200 baud is ~2.6 s of
            // UART output on core 0, which measurably drags the render core
            // (RUN-20260829-13-logexp: +18 % upscale time). Sub-millisecond
            // SD spans are summarized by count instead of printed.
            if (buf[i].type == FT_TYPE_MARK && buf[i].lateness_us > 0 && buf[i].lateness_us < 1000 &&
                (buf[i].kind == FT_MARK_SD_READ || buf[i].kind == FT_MARK_SD_WRITE)) {
                skipped_short_sd++;
                continue;
            }
            ft_print_entry(&buf[i]);
            if (++printed >= 120) { n = 0; break; }
        }
        if (n == 0) break;
        s = next;
    }
    if (skipped_short_sd) {
        printf("JTR|S short_sd_spans_not_printed=%" PRIu32 "\n", skipped_short_sd);
    }
#if FT_HAVE_RUNTIME_STATS
    ft_print_runtime_delta();
#else
    printf("JTR|T unavailable (build without FREERTOS_GENERATE_RUN_TIME_STATS)\n");
#endif
    printf("JTR|END\n");
    frame_trace_mark(FT_MARK_USER, FT_PHASE_END, 1);
}

static void ft_reporter_task(void *arg)
{
    (void)arg;
#if FT_HAVE_RUNTIME_STATS
    vTaskDelay(pdMS_TO_TICKS(8000));   // let boot-time tasks come and go before the first table
    ft_table_rebuild();
#endif
    int64_t last_report_us = -FT_REPORT_GAP_US;
    uint32_t pending_seq = 0;
    for (;;) {
        uint32_t seq = 0;
        BaseType_t got = xTaskNotifyWait(0, UINT32_MAX, &seq, pdMS_TO_TICKS(1000));
        if (got == pdTRUE) pending_seq = seq;
#if FT_HAVE_RUNTIME_STATS
        if (got != pdTRUE) ft_poll();
#endif
        if (pending_seq) {
            int64_t now = esp_timer_get_time();
            if (now - last_report_us >= FT_REPORT_GAP_US) {
                ft_report(pending_seq);
                last_report_us = esp_timer_get_time();
                pending_seq = 0;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void frame_trace_init(void)
{
    if (s_ring) return;
    s_ring = heap_caps_calloc(FT_ENTRIES, sizeof(ft_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "ring alloc failed (%u entries)", (unsigned)FT_ENTRIES);
        return;
    }
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.since_us = esp_timer_get_time();
    memset(s_worst, 0, sizeof(s_worst));
    memset(s_worst_sec, 0, sizeof(s_worst_sec));

#if FT_HAVE_RUNTIME_STATS
    s_snap  = heap_caps_calloc(FT_MAX_TASKS, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tasks = heap_caps_calloc(FT_MAX_TASKS, sizeof(ft_task_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif

    if (xTaskCreatePinnedToCore(ft_reporter_task, "jtr_report", 6144, NULL, 2, &s_reporter, 0) != pdPASS) {
        ESP_LOGE(TAG, "reporter task create failed");
        s_reporter = NULL;
    }

    s_prev_vprintf = esp_log_set_vprintf(ft_log_vprintf);

    ESP_LOGW(TAG, "FRAME TRACE ON: %u entries (%u KB PSRAM), warn %d ms, stall %d ms, runtime-stats=%d",
             (unsigned)FT_ENTRIES, (unsigned)(FT_ENTRIES * sizeof(ft_entry_t) / 1024),
             CONFIG_P3A_FRAME_TRACE_WARN_MS, CONFIG_P3A_FRAME_TRACE_STALL_MS, (int)FT_HAVE_RUNTIME_STATS);
    frame_trace_mark(FT_MARK_USER, FT_PHASE_EVENT, 0xB007u);
}
