// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file http_api_rest_debug_frames.c
 * @brief Jitter work stream (docs/jitter): frame-trace REST endpoints.
 *
 * Compiled only when CONFIG_P3A_FRAME_TRACE=y. Routes (see http_api.c routers):
 *   GET  /api/debug/frames?since=<seq>     CSV of ring entries with seq >= since
 *   GET  /api/debug/frames/stats           JSON aggregates + kind names + config
 *   POST /api/debug/frames/reset           restart aggregates (ring kept)
 *   POST /api/debug/mark?arg=<n>           user marker (run boundaries etc.)
 *   POST /api/debug/provoke?kind=..&n=..   dev-only stall provocation (CONFIG_P3A_FRAME_TRACE_DEV_ENDPOINTS)
 */

#include "sdkconfig.h"
#if CONFIG_P3A_FRAME_TRACE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "frame_trace.h"
#include "http_api_internal.h"

#if CONFIG_P3A_FRAME_TRACE_DEV_ENDPOINTS
#include "nvs_flash.h"
#include "nvs.h"
#include "sd_path.h"
#endif

#define DBG_TAG "http_dbg"
#define CSV_BATCH 128

static bool query_u32(httpd_req_t *req, const char *key, uint32_t *out)
{
    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    char v[24];
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return false;
    *out = (uint32_t)strtoul(v, NULL, 10);
    return true;
}

static bool query_str(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    return httpd_query_key_value(q, key, out, out_len) == ESP_OK;
}

// ---------------------------------------------------------------- GET /api/debug/frames

static esp_err_t h_get_frames_csv(httpd_req_t *req)
{
    uint32_t since = 0;
    (void)query_u32(req, "since", &since);

    ft_entry_t *batch = malloc(CSV_BATCH * sizeof(ft_entry_t));
    char *line = malloc(4096);
    if (!batch || !line) {
        free(batch);
        free(line);
        send_json_oom(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char *hdr = "type,seq,t_us,kind,phase,arg,core,task_tag,lateness_us,ready_margin_us,"
                      "produce_us,decode_us,upscale_us,free_wait_us,vsync_wait_us,duration_ms,queue_depth,flags\n";
    httpd_resp_send_chunk(req, hdr, HTTPD_RESP_USE_STRLEN);

    frame_trace_mark(FT_MARK_HTTP_REQ, FT_PHASE_BEGIN, 0xC5);
    uint32_t next = since;
    uint32_t cursor = since;
    for (;;) {
        size_t n = frame_trace_read(cursor, batch, CSV_BATCH, &next);
        if (n == 0) break;
        size_t used = 0;
        for (size_t i = 0; i < n; i++) {
            const ft_entry_t *e = &batch[i];
            int w;
            if (e->type == FT_TYPE_FRAME) {
                w = snprintf(line + used, 4096 - used,
                             "F,%" PRIu32 ",%lld,,,%" PRIu32 ",%u,,%" PRId32 ",%" PRId32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%u,%u\n",
                             e->seq, (long long)e->t_us, e->arg, (unsigned)e->core,
                             e->lateness_us, e->ready_margin_us, e->produce_us, e->decode_us, e->upscale_us,
                             e->free_wait_us, e->vsync_wait_us, (unsigned)e->duration_ms, (unsigned)e->queue_depth,
                             (unsigned)e->flags);
            } else {
                w = snprintf(line + used, 4096 - used,
                             "M,%" PRIu32 ",%lld,%s,%u,%" PRIu32 ",%u,%08" PRIx32 ",%" PRId32 ",,,,,,,,,\n",
                             e->seq, (long long)e->t_us, frame_trace_mark_kind_name(e->kind), (unsigned)e->phase,
                             e->arg, (unsigned)e->core, e->task_tag, e->lateness_us);
            }
            if (w < 0 || (size_t)w >= 4096 - used) {
                // flush and retry this entry in a fresh buffer
                if (used) httpd_resp_send_chunk(req, line, used);
                used = 0;
                i--;
                continue;
            }
            used += (size_t)w;
            if (used > 3600) {
                httpd_resp_send_chunk(req, line, used);
                used = 0;
            }
        }
        if (used) httpd_resp_send_chunk(req, line, used);
        cursor = next;
        // Small yield so a long dump doesn't starve the httpd peer.
        taskYIELD();
    }
    int w = snprintf(line, 4096, "#next,%" PRIu32 "\n", next);
    httpd_resp_send_chunk(req, line, (size_t)w);
    httpd_resp_send_chunk(req, NULL, 0);
    frame_trace_mark(FT_MARK_HTTP_REQ, FT_PHASE_END, 0xC5);
    free(batch);
    free(line);
    return ESP_OK;
}

static esp_err_t h_get_frames_stats(httpd_req_t *req)
{
    ft_stats_t st;
    frame_trace_get_stats(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        send_json_oom(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data);

    cJSON_AddNumberToObject(data, "now_us", (double)esp_timer_get_time());
    cJSON_AddNumberToObject(data, "since_us", (double)st.since_us);
    cJSON_AddNumberToObject(data, "frames", st.frames);
    cJSON_AddNumberToObject(data, "marks", st.marks);
    cJSON_AddNumberToObject(data, "stalls_warn", st.stalls_warn);
    cJSON_AddNumberToObject(data, "stalls_hard", st.stalls_hard);
    cJSON_AddNumberToObject(data, "overrun_frames", st.overrun_frames);
    cJSON_AddNumberToObject(data, "stalls_overrun", st.stalls_overrun);
    cJSON_AddNumberToObject(data, "lateness_max_us", st.lateness_max_us);
    cJSON_AddNumberToObject(data, "lateness_mean_us",
                            st.frames ? (double)st.lateness_sum_us / (double)st.frames : 0.0);
    cJSON_AddNumberToObject(data, "worst_10s_us", st.worst_10s_us);
    cJSON_AddNumberToObject(data, "last_stall_seq", st.last_stall_seq);
    cJSON_AddNumberToObject(data, "last_stall_us", (double)st.last_stall_us);
    cJSON_AddNumberToObject(data, "next_seq", st.next_seq);
    cJSON_AddNumberToObject(data, "oldest_seq", st.oldest_seq);

    cJSON *hist = cJSON_CreateArray();
    static const int32_t edges[12] = FT_HIST_EDGES_US;
    for (int i = 0; i < 12; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "lt_us", edges[i]);
        cJSON_AddNumberToObject(b, "n", st.hist[i]);
        cJSON_AddItemToArray(hist, b);
    }
    cJSON_AddItemToObject(data, "hist", hist);

    cJSON *kinds = cJSON_CreateObject();
    for (int k = 0; k < FT_MARK_COUNT_; k++) {
        const char *name = frame_trace_mark_kind_name((uint8_t)k);
        if (name && name[0] && name[0] != '?') {
            char key[8];
            snprintf(key, sizeof(key), "%d", k);
            cJSON_AddStringToObject(kinds, key, name);
        }
    }
    cJSON_AddItemToObject(data, "kinds", kinds);

    cJSON *cfg = cJSON_CreateObject();
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
#if CONFIG_P3A_FRAME_TRACE_DEV_ENDPOINTS
    cJSON_AddBoolToObject(cfg, "dev_endpoints", true);
#else
    cJSON_AddBoolToObject(cfg, "dev_endpoints", false);
#endif
    cJSON_AddItemToObject(data, "config", cfg);

    send_json_root(req, 200, root);
    return ESP_OK;
}

esp_err_t h_get_debug_frames_route(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strncmp(uri, "/api/debug/frames/stats", 23) == 0) {
        return h_get_frames_stats(req);
    }
    if (strncmp(uri, "/api/debug/frames", 17) == 0 && (uri[17] == '\0' || uri[17] == '?')) {
        return h_get_frames_csv(req);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

// ---------------------------------------------------------------- POST /api/debug/*

#if CONFIG_P3A_FRAME_TRACE_DEV_ENDPOINTS

static void provoke_nvs(uint32_t n)
{
    nvs_handle_t h;
    if (nvs_open("jtr", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t *blob = malloc(1024);
    if (!blob) { nvs_close(h); return; }
    for (uint32_t i = 0; i < n; i++) {
        for (int j = 0; j < 1024; j++) blob[j] = (uint8_t)(j + i + (esp_timer_get_time() & 0xFF));
        frame_trace_mark(FT_MARK_NVS_COMMIT, FT_PHASE_BEGIN, 99);
        nvs_set_blob(h, "blob", blob, 1024);
        nvs_commit(h);
        frame_trace_mark(FT_MARK_NVS_COMMIT, FT_PHASE_END, 99);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    nvs_erase_key(h, "blob");
    nvs_commit(h);
    nvs_close(h);
    free(blob);
}

static void provoke_log(uint32_t n)
{
    static const char pad[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_BEGIN, 0x10u << 24 | n);
    for (uint32_t i = 0; i < n; i++) {
        ESP_LOGI(DBG_TAG, "provoke log %" PRIu32 "/%" PRIu32 " %s", i + 1, n, pad);
    }
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_END, 0x10u << 24 | n);
}

// SD write provocation with controlled buffer placement (jitter work stream, H3b/H4):
//   mode 0: PSRAM, deliberately misaligned (+64 B)   -> driver bounces 512 B per SD command
//   mode 1: PSRAM, 128-byte aligned                  -> direct DMA from PSRAM
//   mode 2: internal DMA-capable RAM                 -> direct DMA, never touches PSRAM
// chunk = bytes per fwrite (512..65536), n = number of fwrites. Marks: PROVOKE
// begin/end with arg = (mode << 24) | chunk; each fwrite shows up as an sd_write span.
static void provoke_sd(uint32_t n, uint32_t chunk, uint32_t mode)
{
    char dir[128];
    if (sd_path_get_temporary(dir, sizeof(dir)) != ESP_OK) return;
    if (chunk < 512) chunk = 512;
    if (chunk > 65536) chunk = 65536;
    char path[160];
    snprintf(path, sizeof(path), "%s/jtr_provoke.bin", dir);
    uint8_t *raw = NULL, *buf = NULL;
    if (mode == 0) {
        raw = heap_caps_aligned_alloc(128, chunk + 128, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        buf = raw ? raw + 64 : NULL;
    } else if (mode == 1) {
        raw = heap_caps_aligned_alloc(128, chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        buf = raw;
    } else {
        raw = heap_caps_aligned_alloc(128, chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        buf = raw;
    }
    if (!buf) {
        ESP_LOGW(DBG_TAG, "provoke sd: alloc failed (mode %u chunk %u)", (unsigned)mode, (unsigned)chunk);
        return;
    }
    memset(buf, 0xA5, chunk);
    const uint32_t code = (mode << 24) | chunk;
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_BEGIN, code);
    FILE *f = fopen(path, "wb");
    if (f) {
        for (uint32_t i = 0; i < n; i++) {
            fwrite(buf, 1, chunk, f);
        }
        fclose(f);
        unlink(path);
    }
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_END, code);
    free(raw);
}

// Memory-traffic provocation (H4 variant): a task on core `core` at priority
// `prio` streams through a PSRAM buffer of `kb` KB (memcpy + CRC-like read) for
// `ms` milliseconds. Control: kind=spin (same task, no memory traffic).
typedef struct { uint32_t kb, ms, prio, core; bool spin; } mem_prov_t;
static void provoke_mem_task(void *arg)
{
    mem_prov_t *pv = (mem_prov_t *)arg;
    const uint32_t code = (pv->spin ? 0x5Fu : 0x3Eu) << 24 | (pv->kb & 0xFFFFFF);
    uint8_t *a = NULL, *b = NULL;
    const size_t bytes = (size_t)pv->kb * 1024;
    if (!pv->spin) {
        a = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        b = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a || !b) { free(a); free(b); free(pv); vTaskDelete(NULL); return; }
        memset(a, 0x5A, bytes);
    }
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_BEGIN, code);
    const int64_t end = esp_timer_get_time() + (int64_t)pv->ms * 1000;
    volatile uint32_t sink = 0;
    while (esp_timer_get_time() < end) {
        if (pv->spin) {
            sink++;
        } else {
            memcpy(b, a, bytes);
            uint32_t h = 2166136261u;
            for (size_t i = 0; i < bytes; i += 4) { h ^= b[i]; h *= 16777619u; }
            sink += h;
        }
    }
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_END, code);
    free(a); free(b); free(pv);
    vTaskDelete(NULL);
}

// Cache-sync provocation (H4c): loop esp_cache_msync() C2M+M2C on a buffer of
// `kb` KB (PSRAM by default, internal if buf=2) for `ms` ms on `core` at `prio`,
// with no SD traffic at all. Every SD transaction performs such syncs on its
// buffer and DMA descriptors; if syncs alone slow the upscale, the L2 cache
// sync is the toxic primitive behind SD command storms.
typedef struct { uint32_t kb, ms, prio, core, internal; } msync_prov_t;
static void provoke_msync_task(void *arg)
{
    msync_prov_t *pv = (msync_prov_t *)arg;
    const size_t bytes = (size_t)pv->kb * 1024;
    uint8_t *buf = heap_caps_aligned_alloc(128, bytes, (pv->internal ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT);
    if (!buf) { free(pv); vTaskDelete(NULL); return; }
    memset(buf, 0x3C, bytes);
    const uint32_t code = 0x7Au << 24 | (pv->internal ? 0x800000u : 0) | (pv->kb & 0xFFFF);
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_BEGIN, code);
    const int64_t end = esp_timer_get_time() + (int64_t)pv->ms * 1000;
    uint32_t ops = 0;
    while (esp_timer_get_time() < end) {
        esp_cache_msync(buf, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_cache_msync(buf, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        buf[(ops * 64) % bytes] ^= 1;   // keep a line dirty
        ops += 2;
    }
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_END, ops);
    free(buf); free(pv);
    vTaskDelete(NULL);
}

static void provoke_cpu1_task(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_BEGIN, 0x0C1);
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    volatile uint32_t sink = 0;
    while (esp_timer_get_time() < end) { sink++; }
    frame_trace_mark(FT_MARK_PROVOKE, FT_PHASE_END, 0x0C1);
    vTaskDelete(NULL);
}

static esp_err_t h_post_provoke(httpd_req_t *req)
{
    char kind[16] = {0};
    uint32_t n = 1;
    (void)query_str(req, "kind", kind, sizeof(kind));
    (void)query_u32(req, "n", &n);
    if (n > 10000) n = 10000;

    if (strcmp(kind, "nvs") == 0) {
        provoke_nvs(n);
    } else if (strcmp(kind, "log") == 0) {
        provoke_log(n);
    } else if (strcmp(kind, "sd") == 0) {
        uint32_t chunk = 32768, mode = 0;
        (void)query_u32(req, "chunk", &chunk);
        (void)query_u32(req, "buf", &mode);   // 0 psram-misaligned, 1 psram-aligned, 2 internal
        provoke_sd(n, chunk, mode);
    } else if (strcmp(kind, "mem") == 0 || strcmp(kind, "spin") == 0) {
        mem_prov_t *pv = calloc(1, sizeof(*pv));
        if (!pv) { send_json_oom(req); return ESP_OK; }
        pv->kb = 256; pv->ms = n; pv->prio = 4; pv->core = 0; pv->spin = (kind[0] == 's');
        (void)query_u32(req, "kb", &pv->kb);
        (void)query_u32(req, "prio", &pv->prio);
        (void)query_u32(req, "core", &pv->core);
        if (pv->kb > 4096) pv->kb = 4096;
        if (pv->prio > 20) pv->prio = 20;
        xTaskCreatePinnedToCore(provoke_mem_task, "jtr_mem", 4096, pv, (UBaseType_t)pv->prio, NULL, (BaseType_t)(pv->core ? 1 : 0));
    } else if (strcmp(kind, "msync") == 0) {
        msync_prov_t *pv = calloc(1, sizeof(*pv));
        if (!pv) { send_json_oom(req); return ESP_OK; }
        pv->kb = 1; pv->ms = n; pv->prio = 4; pv->core = 0; pv->internal = 0;
        (void)query_u32(req, "kb", &pv->kb);
        (void)query_u32(req, "prio", &pv->prio);
        (void)query_u32(req, "core", &pv->core);
        (void)query_u32(req, "buf", &pv->internal);   // 2 = internal RAM (L1), else PSRAM (L2)
        pv->internal = (pv->internal == 2);
        if (pv->kb > 1024) pv->kb = 1024;
        if (pv->prio > 20) pv->prio = 20;
        xTaskCreatePinnedToCore(provoke_msync_task, "jtr_msync", 4096, pv, (UBaseType_t)pv->prio, NULL, (BaseType_t)(pv->core ? 1 : 0));
    } else if (strcmp(kind, "cpu1") == 0) {
        // Busy-spin on core 1 above the consumer's priority for n ms: validates
        // detection + run-time-stats attribution.
        xTaskCreatePinnedToCore(provoke_cpu1_task, "jtr_hog", 2048, (void *)(uintptr_t)n,
                                CONFIG_P3A_RENDER_TASK_PRIORITY + 2, NULL, 1);
    } else {
        send_json_error(req, 400, "BAD_KIND", "kind must be nvs|log|sd|cpu1|mem|spin");
        return ESP_OK;
    }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}
#endif  // CONFIG_P3A_FRAME_TRACE_DEV_ENDPOINTS

esp_err_t h_post_debug_frames_route(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strncmp(uri, "/api/debug/frames/reset", 23) == 0) {
        frame_trace_reset();
        send_json(req, 200, "{\"ok\":true}");
        return ESP_OK;
    }
    if (strncmp(uri, "/api/debug/mark", 15) == 0) {
        uint32_t arg = 0;
        (void)query_u32(req, "arg", &arg);
        frame_trace_mark(FT_MARK_USER, FT_PHASE_EVENT, arg);
        send_json(req, 200, "{\"ok\":true}");
        return ESP_OK;
    }
#if CONFIG_P3A_FRAME_TRACE_DEV_ENDPOINTS
    if (strncmp(uri, "/api/debug/provoke", 18) == 0) {
        return h_post_provoke(req);
    }
#endif
    return ESP_ERR_NOT_FOUND;
}

#endif  // CONFIG_P3A_FRAME_TRACE
