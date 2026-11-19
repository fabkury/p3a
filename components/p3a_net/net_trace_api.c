// SPDX-License-Identifier: Apache-2.0
// The frame trace over HTTP, for the jitter work (docs/jitter). Compiled only
// into builds with CONFIG_P3A_FRAME_TRACE (lab.py build trace, trace-test).
//
//   GET  /api/v2/debug/frames?since=<seq>   the ring's entries from <seq> on, as
//                                           CSV; the last line is "#next,<seq>"
//   GET  /api/v2/debug/frames/stats         aggregates, the lateness histogram,
//                                           mark names, the trace's configuration
//   POST /api/v2/debug/frames/reset         restart the aggregates (the ring is kept)
//   POST /api/v2/debug/mark?arg=<n>         a marker of the caller's own
//
// The CSV columns are those of the 1.x endpoint, which host/jitter-lab reads.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "frame_trace.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_internal.h"
#include "p3a_http.h"
#include "psram_alloc.h"

#define CSV_BATCH     128
#define CSV_LINE_ROOM 4096

static void get_frames(p3a_http_request_t *req)
{
    char text[16];
    uint32_t since = p3a_http_query(req, "since", text, sizeof(text)) ? (uint32_t)strtoul(text, NULL, 10) : 0;
    ft_entry_t *batch = psram_malloc(CSV_BATCH * sizeof(ft_entry_t));
    char *line = psram_malloc(CSV_LINE_ROOM);
    if (batch == NULL || line == NULL) {
        free(batch);
        free(line);
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading the frame trace");
        return;
    }
    httpd_req_t *raw = p3a_http_raw(req);
    httpd_resp_set_type(raw, "text/csv");
    httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(raw, "type,seq,t_us,kind,phase,arg,core,task_tag,lateness_us,ready_margin_us,"
                                  "produce_us,decode_us,upscale_us,free_wait_us,vsync_wait_us,duration_ms,"
                                  "queue_depth,flags\n");

    // The read-out is itself something that happens to the device: marked, so
    // that a late frame during a pull can be told from one that was not.
    frame_trace_mark(FT_MARK_HTTP_REQ, FT_PHASE_BEGIN, 0xC5);
    uint32_t next = since;
    esp_err_t err = ESP_OK;
    for (uint32_t cursor = since; err == ESP_OK; cursor = next) {
        const size_t n = frame_trace_read(cursor, batch, CSV_BATCH, &next);
        if (n == 0) {
            break;
        }
        size_t used = 0;
        for (size_t i = 0; i < n && err == ESP_OK; i++) {
            const ft_entry_t *e = &batch[i];
            const size_t room = CSV_LINE_ROOM - used;
            int w;
            if (e->type == FT_TYPE_FRAME) {
                w = snprintf(line + used, room,
                             "F,%" PRIu32 ",%lld,,,%" PRIu32 ",%u,,%" PRId32 ",%" PRId32 ",%" PRIu32 ",%" PRIu32
                             ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%u,%u\n",
                             e->seq, (long long)e->t_us, e->arg, (unsigned)e->core, e->lateness_us,
                             e->ready_margin_us, e->produce_us, e->decode_us, e->upscale_us, e->free_wait_us,
                             e->vsync_wait_us, (unsigned)e->duration_ms, (unsigned)e->queue_depth,
                             (unsigned)e->flags);
            } else {
                w = snprintf(line + used, room,
                             "M,%" PRIu32 ",%lld,%s,%u,%" PRIu32 ",%u,%08" PRIx32 ",%" PRId32 ",,,,,,,,,\n", e->seq,
                             (long long)e->t_us, frame_trace_mark_kind_name(e->kind), (unsigned)e->phase, e->arg,
                             (unsigned)e->core, e->task_tag, e->lateness_us);
            }
            if (w > 0 && (size_t)w < room) {
                used += (size_t)w;
            }
            // A line is at most about 200 characters: send before one might not fit.
            if (used > CSV_LINE_ROOM - 512 || i == n - 1) {
                err = httpd_resp_send_chunk(raw, line, (ssize_t)used);
                used = 0;
            }
        }
        taskYIELD();  // a long read-out must not starve the other connections
    }
    if (err == ESP_OK) {
        const int w = snprintf(line, CSV_LINE_ROOM, "#next,%" PRIu32 "\n", next);
        httpd_resp_send_chunk(raw, line, w);
    }
    httpd_resp_send_chunk(raw, NULL, 0);
    frame_trace_mark(FT_MARK_HTTP_REQ, FT_PHASE_END, 0xC5);
    free(batch);
    free(line);
    p3a_http_mark_answered(req);
}

static void get_stats(p3a_http_request_t *req)
{
    ft_stats_t st;
    frame_trace_get_stats(&st);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "now_us", (double)esp_timer_get_time());
    cJSON_AddNumberToObject(data, "since_us", (double)st.since_us);
    cJSON_AddNumberToObject(data, "frames", st.frames);
    cJSON_AddNumberToObject(data, "marks", st.marks);
    cJSON_AddNumberToObject(data, "stalls_warn", st.stalls_warn);
    cJSON_AddNumberToObject(data, "stalls_hard", st.stalls_hard);
    cJSON_AddNumberToObject(data, "overrun_frames", st.overrun_frames);
    cJSON_AddNumberToObject(data, "stalls_overrun", st.stalls_overrun);
    cJSON_AddNumberToObject(data, "lateness_max_us", st.lateness_max_us);
    cJSON_AddNumberToObject(data, "lateness_mean_us", st.frames ? (double)st.lateness_sum_us / (double)st.frames : 0.0);
    cJSON_AddNumberToObject(data, "worst_10s_us", st.worst_10s_us);
    cJSON_AddNumberToObject(data, "last_stall_seq", st.last_stall_seq);
    cJSON_AddNumberToObject(data, "last_stall_us", (double)st.last_stall_us);
    cJSON_AddNumberToObject(data, "next_seq", st.next_seq);
    cJSON_AddNumberToObject(data, "oldest_seq", st.oldest_seq);

    cJSON *hist = cJSON_AddArrayToObject(data, "hist");
    static const int32_t edges[12] = FT_HIST_EDGES_US;
    for (int i = 0; i < 12; i++) {
        cJSON *bucket = cJSON_CreateObject();
        cJSON_AddNumberToObject(bucket, "lt_us", edges[i]);
        cJSON_AddNumberToObject(bucket, "n", st.hist[i]);
        cJSON_AddItemToArray(hist, bucket);
    }
    cJSON *kinds = cJSON_AddObjectToObject(data, "kinds");
    for (int k = 0; k < FT_MARK_COUNT_; k++) {
        const char *name = frame_trace_mark_kind_name((uint8_t)k);
        if (name && name[0] && name[0] != '?') {
            char key[8];
            snprintf(key, sizeof(key), "%d", k);
            cJSON_AddStringToObject(kinds, key, name);
        }
    }
    cJSON *cfg = cJSON_AddObjectToObject(data, "config");
    cJSON_AddNumberToObject(cfg, "entries", CONFIG_P3A_FRAME_TRACE_ENTRIES);
    cJSON_AddNumberToObject(cfg, "warn_ms", CONFIG_P3A_FRAME_TRACE_WARN_MS);
    cJSON_AddNumberToObject(cfg, "stall_ms", CONFIG_P3A_FRAME_TRACE_STALL_MS);
    cJSON_AddNumberToObject(cfg, "report_min_interval_s", CONFIG_P3A_FRAME_TRACE_REPORT_MIN_INTERVAL_S);
    cJSON_AddNumberToObject(cfg, "report_window_ms", CONFIG_P3A_FRAME_TRACE_REPORT_WINDOW_MS);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY
    cJSON_AddBoolToObject(cfg, "runtime_stats", true);
#else
    cJSON_AddBoolToObject(cfg, "runtime_stats", false);
#endif
    p3a_http_ok(req, data);
}

static void post_reset(p3a_http_request_t *req)
{
    frame_trace_reset();
    p3a_http_ok(req, NULL);
}

static void post_mark(p3a_http_request_t *req)
{
    char text[16];
    const uint32_t arg = p3a_http_query(req, "arg", text, sizeof(text)) ? (uint32_t)strtoul(text, NULL, 10) : 0;
    frame_trace_mark(FT_MARK_USER, FT_PHASE_EVENT, arg);
    p3a_http_ok(req, NULL);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, P3A_HTTP_RAW, "/api/v2/debug/frames", get_frames},
    {P3A_HTTP_GET, 0, "/api/v2/debug/frames/stats", get_stats},
    {P3A_HTTP_POST, 0, "/api/v2/debug/frames/reset", post_reset},
    {P3A_HTTP_POST, 0, "/api/v2/debug/mark", post_mark},
};

void net_trace_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
