// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file http_fetch.c
 * @brief Shared HTTP fetch/download helper — see http_fetch.h.
 *
 * Both public entry points are thin wrappers over do_fetch(), which owns the
 * one copy of: client config, optional POST open()+write(), status
 * classification, the standardized retry/backoff/truncation/Retry-After loop,
 * optional manual 3xx-redirect following, and the optional SDIO poll-wait.
 * The only per-sink difference is how the body is consumed (buffer vs file).
 *
 * The retry/truncation behavior intentionally mirrors the pre-refactor Giphy
 * path (components/giphy/giphy_download.c, components/giphy/giphy_api.c) so the
 * migration is behavior-preserving for the sites that already retried, and a
 * net improvement (the same robustness) for the sites that did not.
 *
 * do_fetch() also owns a process-wide TLS concurrency gate (a counting
 * semaphore) so overlapping HTTPS transfers can't starve each other on the
 * single Wi-Fi link — see the gate section below and
 * docs/concurrent-tls-eagain-tabled.md (Option 4).
 */

#include "http_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <unistd.h>    // ftruncate, fsync, unlink

#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "sdio_bus.h"

static const char *TAG = "http_fetch";

// Belt-and-suspenders: the Kconfig default is 2, but keep the helper buildable
// if the option is ever stripped from a generated sdkconfig.
#ifndef CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS
#define CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS 2
#endif

#define HTTP_FETCH_DEFAULT_TIMEOUT_MS   15000
#define HTTP_FETCH_DEFAULT_RX_BUFFER    4096
#define HTTP_FETCH_DEFAULT_ATTEMPTS     3
#define HTTP_FETCH_DEFAULT_CHUNK        (32 * 1024)
#define HTTP_FETCH_DEFAULT_REDIRECTS    5
#define HTTP_FETCH_DEFAULT_SDIO_WAIT_S  120
#define HTTP_FETCH_URL_MAX              1024   // working URL + captured Location (manual redirect)

static const uint32_t s_default_backoff_ms[HTTP_FETCH_DEFAULT_ATTEMPTS] = {0, 1000, 3000};

// ---------------------------------------------------------------------------
// TLS concurrency gate (Option 4, docs/concurrent-tls-eagain-tabled.md)
//
// Every transfer issued through this helper passes through a process-wide
// counting semaphore, so at most CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS fetches
// hold a live TLS session at once. Overlapping HTTPS streams on the single
// Wi-Fi link were starving large downloads: a socket read stalls past the
// timeout, lwIP returns EAGAIN ("esp_tls_conn_read error, errno=No more
// processes"), esp_http_client maps that to end-of-stream, and the body is
// retried as a truncated read — which big GIFs could lose on every attempt.
// Gating concurrency caps that contention structurally; raising the lwIP recv
// mailboxes only raised the ceiling.
//
// Lazy init: do_fetch() runs from several tasks (download_mgr, giphy refresh,
// museum refresh, show_url) with no shared init point. xSemaphoreCreateCounting
// allocates, so it runs OUTSIDE the critical section; a compare-and-set under a
// spinlock publishes the winner and any racing loser deletes its spare. If the
// allocation fails we degrade to ungated rather than wedging every fetcher.
// ---------------------------------------------------------------------------

static SemaphoreHandle_t s_tls_gate = NULL;
static portMUX_TYPE      s_tls_gate_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t tls_gate(void)
{
    SemaphoreHandle_t gate = s_tls_gate;
    if (gate) return gate;

    SemaphoreHandle_t created =
        xSemaphoreCreateCounting(CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS,
                                 CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS);
    if (!created) {
        ESP_LOGE(TAG, "TLS gate alloc failed; proceeding ungated");
        return NULL;
    }

    portENTER_CRITICAL(&s_tls_gate_lock);
    if (s_tls_gate == NULL) {
        s_tls_gate = created;   // publish ours
        created = NULL;
    }
    gate = s_tls_gate;
    portEXIT_CRITICAL(&s_tls_gate_lock);

    if (created) vSemaphoreDelete(created);  // lost the race; drop the spare
    return gate;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

typedef struct {
    char location[HTTP_FETCH_URL_MAX];
} redir_ctx_t;

// Capture the Location header on a redirect (manual-redirect mode only).
static esp_err_t capture_location_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data &&
        evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Location") == 0) {
        redir_ctx_t *ctx = (redir_ctx_t *)evt->user_data;
        strlcpy(ctx->location, evt->header_value, sizeof(ctx->location));
    }
    return ESP_OK;
}

// Parse a numeric Retry-After (seconds). Mirrors the old ai_parse_retry_after:
// leading spaces skipped, non-positive/non-numeric -> 0, capped at 1 hour.
static uint32_t parse_retry_after(const char *value)
{
    if (!value) return 0;
    while (*value == ' ') value++;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || v <= 0) return 0;
    if (v > 3600) v = 3600;
    return (uint32_t)v;
}

static uint32_t backoff_for(const http_fetch_request_t *req, int attempt)
{
    if (req->backoff_ms) return req->backoff_ms[attempt];
    int idx = (attempt < HTTP_FETCH_DEFAULT_ATTEMPTS) ? attempt : HTTP_FETCH_DEFAULT_ATTEMPTS - 1;
    return s_default_backoff_ms[idx];
}

static void apply_headers(esp_http_client_handle_t client, const http_fetch_request_t *req)
{
    if (req->user_agent) {
        esp_http_client_set_header(client, "User-Agent", req->user_agent);
    }
    for (size_t i = 0; i < req->header_count; i++) {
        if (req->headers[i].name && req->headers[i].value) {
            esp_http_client_set_header(client, req->headers[i].name, req->headers[i].value);
        }
    }
}

// Block (poll, never acquire) while the SDIO bus is held by a WiFi-heavy op.
static esp_err_t sdio_wait(const http_fetch_request_t *req)
{
    if (!req->wait_on_sdio || !sdio_bus_is_locked()) return ESP_OK;

    const char *who = req->sdio_requester ? req->sdio_requester : "http_fetch";
    const char *holder = sdio_bus_get_holder();
    ESP_LOGI(TAG, "[%s] SDIO bus locked by %s, waiting...", who, holder ? holder : "unknown");

    int max_wait = req->sdio_wait_max_s ? req->sdio_wait_max_s : HTTP_FETCH_DEFAULT_SDIO_WAIT_S;
    int waited = 0;
    while (sdio_bus_is_locked() && waited < max_wait) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited++;
    }
    if (waited >= max_wait) {
        ESP_LOGE(TAG, "[%s] SDIO bus still locked after %ds, aborting", who, max_wait);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Sink: where the body goes (caller buffer vs file)
// ---------------------------------------------------------------------------

typedef enum { SINK_BUFFER, SINK_FILE } sink_kind_t;

typedef struct {
    sink_kind_t kind;
    // buffer sink
    char   *buf;
    size_t  buf_size;
    // file sink
    FILE   *f;
    uint8_t *chunk;
    size_t  chunk_size;
    // output
    size_t  bytes;   // bytes delivered in the current attempt
} fetch_sink_t;

typedef enum {
    BODY_OK = 0,
    BODY_READ_ERR,
    BODY_WRITE_ERR,
    BODY_TOO_LARGE,
    BODY_BUFFER_FULL,
    BODY_ABORTED,
} body_status_t;

// Reset the sink before an attempt/hop. Returns false only on a fatal file error.
static bool sink_reset(fetch_sink_t *s)
{
    s->bytes = 0;
    if (s->kind == SINK_FILE && s->f) {
        rewind(s->f);
        if (ftruncate(fileno(s->f), 0) != 0) {
            ESP_LOGE(TAG, "Failed to truncate temp file");
            return false;
        }
    }
    return true;
}

static body_status_t read_into_buffer(fetch_sink_t *s, esp_http_client_handle_t client,
                                       int64_t content_length,
                                       const http_fetch_request_t *req)
{
    bool read_err = false;
    while (s->bytes < s->buf_size - 1) {
        int rl = esp_http_client_read(client, s->buf + s->bytes,
                                      s->buf_size - 1 - s->bytes);
        if (rl < 0) { read_err = true; break; }
        if (rl == 0) break;
        s->bytes += (size_t)rl;
        if (req->should_abort && req->should_abort(req->user_ctx)) return BODY_ABORTED;
        if (req->progress) {
            req->progress(s->bytes, content_length > 0 ? (size_t)content_length : 0, req->user_ctx);
        }
    }
    if (read_err) return BODY_READ_ERR;
    if (s->bytes >= s->buf_size - 1) {
        // Buffer filled to capacity. That alone doesn't prove truncation: the
        // body may end exactly at capacity (storage.bin is exactly the 4 MB
        // LittleFS partition size). Probe one more byte to tell EOF apart from
        // a genuinely larger body.
        char probe;
        int rl = esp_http_client_read(client, &probe, 1);
        if (rl < 0) return BODY_READ_ERR;
        if (rl > 0) return BODY_BUFFER_FULL;
    }
    return BODY_OK;
}

static body_status_t read_into_file(fetch_sink_t *s, esp_http_client_handle_t client,
                                     int64_t content_length, int64_t max_size,
                                     const http_fetch_request_t *req)
{
    bool read_ok = true;
    while (true) {
        size_t chunk_received = 0;
        while (chunk_received < s->chunk_size) {
            int rl = esp_http_client_read(client, (char *)s->chunk + chunk_received,
                                          s->chunk_size - chunk_received);
            if (rl < 0) { read_ok = false; break; }
            if (rl == 0) break;  // end of data
            chunk_received += (size_t)rl;
        }
        if (!read_ok || chunk_received == 0) break;

        if (req->should_abort && req->should_abort(req->user_ctx)) return BODY_ABORTED;

        size_t written = fwrite(s->chunk, 1, chunk_received, s->f);
        if (written != chunk_received) {
            ESP_LOGE(TAG, "Write error: wrote %zu/%zu bytes", written, chunk_received);
            return BODY_WRITE_ERR;
        }
        s->bytes += written;

        if (max_size > 0 && (int64_t)s->bytes > max_size) return BODY_TOO_LARGE;

        if (req->progress) {
            req->progress(s->bytes, content_length > 0 ? (size_t)content_length : 0, req->user_ctx);
        }

        if (chunk_received < s->chunk_size) break;  // last chunk
    }
    return read_ok ? BODY_OK : BODY_READ_ERR;
}

// ---------------------------------------------------------------------------
// Core: one fetch, with retries + optional manual redirects, into a sink.
// ---------------------------------------------------------------------------

static esp_err_t do_fetch(const http_fetch_request_t *req, fetch_sink_t *sink,
                          http_fetch_result_t *result)
{
    const int     timeout_ms   = req->timeout_ms     ? req->timeout_ms     : HTTP_FETCH_DEFAULT_TIMEOUT_MS;
    const int     rx_buf       = req->rx_buffer_size ? req->rx_buffer_size : HTTP_FETCH_DEFAULT_RX_BUFFER;
    const int     max_attempts = req->max_attempts   ? req->max_attempts   : HTTP_FETCH_DEFAULT_ATTEMPTS;
    const int64_t max_size     = req->max_size;  // 0 => no cap
    const bool    manual_redirect = (req->redirect_mode == HTTP_FETCH_REDIRECT_MANUAL);
    const int     max_redirects   = manual_redirect
                                        ? (req->max_redirects ? req->max_redirects : HTTP_FETCH_DEFAULT_REDIRECTS)
                                        : 0;

    esp_err_t sw = sdio_wait(req);
    if (sw != ESP_OK) return sw;

    // Manual-redirect scratch (working URL + captured Location) is heap-backed
    // and allocated only when needed, so do_fetch keeps a tiny stack footprint.
    // Museum refresh runs on an ~8 KB task stack and these 2 KB of buffers, when
    // placed on the stack, overflowed it during the TLS read. Non-redirect
    // fetches (the common case) allocate nothing here.
    char *work_url = NULL;
    redir_ctx_t *redir = NULL;
    const char *cur_url = req->url;
    if (manual_redirect) {
        work_url = heap_caps_malloc(HTTP_FETCH_URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!work_url) work_url = heap_caps_malloc(HTTP_FETCH_URL_MAX, MALLOC_CAP_8BIT);
        redir = heap_caps_malloc(sizeof(redir_ctx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!redir) redir = heap_caps_malloc(sizeof(redir_ctx_t), MALLOC_CAP_8BIT);
        if (!work_url || !redir) {
            free(work_url);
            free(redir);
            return ESP_ERR_NO_MEM;
        }
        strlcpy(work_url, req->url, HTTP_FETCH_URL_MAX);
        cur_url = work_url;
    }

    esp_err_t fatal = ESP_OK;
    bool      success = false;
    int       last_status = 0;
    int64_t   content_length = -1;
    uint32_t  retry_after = 0;
    int       attempts_used = 0;
    bool      buffer_full = false;
    bool      aborted = false;

    // Hold a TLS-concurrency slot for the duration of the live network work.
    // Acquired here (after the SDIO wait and the redirect-scratch alloc, which
    // do no network and must not hold a slot) and released once below the hop
    // loop. The slot is briefly handed back during inter-attempt backoff sleeps
    // so a retrying transfer doesn't pin a slot while idle. No early returns
    // exist between here and the matching give, so the slot can't leak.
    SemaphoreHandle_t gate = tls_gate();
    if (gate) xSemaphoreTake(gate, portMAX_DELAY);

    for (int hop = 0; hop <= max_redirects && !success && fatal == ESP_OK; hop++) {
        bool got_redirect = false;

        for (int attempt = 0;
             attempt < max_attempts && !success && fatal == ESP_OK && !got_redirect;
             attempt++) {

            if (attempt > 0) {
                uint32_t delay = backoff_for(req, attempt);
                if (delay) {
                    ESP_LOGW(TAG, "Retrying %s in %lums (attempt %d/%d)",
                             cur_url, (unsigned long)delay, attempt + 1, max_attempts);
                    // Don't pin a concurrency slot while sleeping on the backoff.
                    if (gate) xSemaphoreGive(gate);
                    vTaskDelay(pdMS_TO_TICKS(delay));
                    if (gate) xSemaphoreTake(gate, portMAX_DELAY);
                }
            }
            if (!sink_reset(sink)) { fatal = ESP_FAIL; break; }
            attempts_used++;

            esp_http_client_config_t cfg = {
                .url = cur_url,
                .timeout_ms = timeout_ms,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .buffer_size = rx_buf,
                .buffer_size_tx = req->tx_buffer_size,  // 0 -> IDF default
            };
            if (req->method == HTTP_FETCH_POST) {
                cfg.method = HTTP_METHOD_POST;
            }
            if (manual_redirect) {
                cfg.disable_auto_redirect = true;
                cfg.event_handler = capture_location_cb;
                cfg.user_data = redir;
                redir->location[0] = '\0';
            }

            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            if (!client) {
                ESP_LOGE(TAG, "Failed to init HTTP client");
                continue;
            }

            apply_headers(client, req);

            size_t open_len = (req->method == HTTP_FETCH_POST) ? req->body_len : 0;
            esp_err_t err = esp_http_client_open(client, open_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
                esp_http_client_cleanup(client);
                continue;
            }
            if (req->method == HTTP_FETCH_POST && req->body_len > 0) {
                int w = esp_http_client_write(client, (const char *)req->body, req->body_len);
                if (w != (int)req->body_len) {
                    ESP_LOGE(TAG, "HTTP write %d/%zu failed", w, req->body_len);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    continue;
                }
            }

            esp_http_client_fetch_headers(client);
            content_length = esp_http_client_get_content_length(client);
            last_status = esp_http_client_get_status_code(client);

            if (last_status == 404) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal = ESP_ERR_NOT_FOUND;
                break;
            }
            if (last_status == 429) {
                char *v = NULL;
                if (esp_http_client_get_header(client, "Retry-After", &v) == ESP_OK && v) {
                    retry_after = parse_retry_after(v);
                }
                if (req->on_rate_limited) req->on_rate_limited(retry_after, req->user_ctx);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (last_status == 401 || last_status == 403) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal = ESP_ERR_NOT_ALLOWED;
                break;
            }
            if (manual_redirect && last_status >= 300 && last_status < 400) {
                if (redir->location[0] != '\0') {
                    strlcpy(work_url, redir->location, HTTP_FETCH_URL_MAX);
                    got_redirect = true;
                } else {
                    ESP_LOGE(TAG, "Redirect %d with no Location header", last_status);
                    fatal = ESP_FAIL;
                }
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                break;  // leave attempt loop; hop loop advances iff got_redirect
            }
            bool status_ok = (last_status == 200) ||
                             (req->accept_2xx && last_status >= 200 && last_status < 300);
            if (!status_ok) {
                ESP_LOGW(TAG, "HTTP status %d for %s", last_status, cur_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                continue;  // retry
            }

            // --- 200 OK (or accepted 2xx) ---
            if (content_length == 0 && req->treat_empty_as_not_found) {
                ESP_LOGW(TAG, "Empty body (200, Content-Length: 0): %s", cur_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal = ESP_ERR_NOT_FOUND;
                break;
            }
            if (max_size > 0 && content_length > max_size) {
                ESP_LOGW(TAG, "Response too large: %lld > %lld bytes",
                         (long long)content_length, (long long)max_size);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal = ESP_ERR_INVALID_SIZE;
                break;
            }

            body_status_t br = (sink->kind == SINK_BUFFER)
                                   ? read_into_buffer(sink, client, content_length, req)
                                   : read_into_file(sink, client, content_length, max_size, req);

            // Chunked responses carry no Content-Length, so the byte-count
            // truncation check below can't see a premature end-of-stream (the
            // EAGAIN-under-load path surfaces as read==0, indistinguishable
            // from EOF — see docs/concurrent-tls-eagain-tabled.md). The parser
            // does know whether the terminating zero-length chunk arrived;
            // capture that before close/cleanup destroy the parser state.
            // Gated on is_chunked: for close-delimited responses (no
            // Content-Length and not chunked) is_complete_data_received()
            // reports false even on success, which would fail every transfer.
            bool chunked_incomplete = esp_http_client_is_chunked_response(client) &&
                                      !esp_http_client_is_complete_data_received(client);

            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            if (br == BODY_ABORTED) {
                ESP_LOGI(TAG, "Transfer aborted by caller: %s", cur_url);
                aborted = true;
                fatal = ESP_ERR_INVALID_STATE;
                break;
            }
            if (br == BODY_WRITE_ERR) { fatal = ESP_FAIL; break; }
            if (br == BODY_TOO_LARGE) {
                ESP_LOGW(TAG, "Download exceeded %lld bytes, aborting: %s",
                         (long long)max_size, cur_url);
                fatal = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (br == BODY_BUFFER_FULL) {
                ESP_LOGE(TAG, "Response truncated at %zu bytes (buffer full): %s",
                         sink->bytes, cur_url);
                buffer_full = true;
                fatal = ESP_ERR_INVALID_SIZE;
                break;
            }

            // BODY_OK or BODY_READ_ERR -> truncation check, then final validations.
            bool truncated = (br == BODY_READ_ERR) ||
                             (sink->bytes == 0 && !req->allow_empty_body) ||
                             (content_length > 0 && sink->bytes < (size_t)content_length) ||
                             chunked_incomplete;
            if (truncated) {
                if (chunked_incomplete) {
                    ESP_LOGW(TAG, "Truncated chunked response: got %zu bytes, terminal chunk never arrived: %s",
                             sink->bytes, cur_url);
                } else {
                    ESP_LOGW(TAG, "Truncated response: got %zu/%lld bytes",
                             sink->bytes, (long long)content_length);
                }
                continue;  // retry
            }
            if (req->min_size > 0 && sink->bytes < req->min_size) {
                ESP_LOGW(TAG, "Response too small: %zu < %zu bytes", sink->bytes, req->min_size);
                fatal = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (req->require_exact_length && content_length > 0 &&
                sink->bytes != (size_t)content_length) {
                ESP_LOGW(TAG, "Length mismatch: got %zu, expected %lld",
                         sink->bytes, (long long)content_length);
                fatal = ESP_ERR_INVALID_SIZE;
                break;
            }
            success = true;
        }  // attempt loop

        if (!got_redirect) break;  // no redirect this hop -> done (success / fatal / exhausted)
    }  // hop loop

    if (gate) xSemaphoreGive(gate);

    free(work_url);
    free(redir);

    if (!success && fatal == ESP_OK && manual_redirect) {
        ESP_LOGE(TAG, "Exhausted %d redirects without success: %s", max_redirects, req->url);
    }

    if (result) {
        result->http_status     = last_status;
        result->bytes           = sink->bytes;
        result->content_length  = content_length;
        result->attempts        = attempts_used;
        result->buffer_full     = buffer_full;
        result->aborted         = aborted;
        result->retry_after_sec = retry_after;
    }

    if (fatal != ESP_OK) return fatal;
    if (!success) return ESP_FAIL;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

esp_err_t http_fetch_to_buffer(const http_fetch_request_t *req,
                               char *buf, size_t buf_size,
                               size_t *out_len,
                               http_fetch_result_t *result)
{
    if (out_len) *out_len = 0;
    if (result) memset(result, 0, sizeof(*result));
    if (!req || !req->url || !buf || buf_size == 0) return ESP_ERR_INVALID_ARG;

    fetch_sink_t sink = {
        .kind = SINK_BUFFER,
        .buf = buf,
        .buf_size = buf_size,
    };
    esp_err_t err = do_fetch(req, &sink, result);

    // Always NUL-terminate whatever was received (mirrors the old giphy_api path).
    buf[sink.bytes] = '\0';
    if (out_len) *out_len = sink.bytes;
    return err;
}

esp_err_t http_fetch_to_file(const http_fetch_request_t *req,
                             const char *out_path,
                             http_fetch_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));
    if (!req || !req->url || !out_path || out_path[0] == '\0') return ESP_ERR_INVALID_ARG;

    const size_t chunk_size = req->chunk_size ? req->chunk_size : HTTP_FETCH_DEFAULT_CHUNK;

    uint8_t *chunk = heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) chunk = heap_caps_malloc(chunk_size, MALLOC_CAP_8BIT);
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to allocate %zu-byte chunk buffer", chunk_size);
        return ESP_ERR_NO_MEM;
    }

    // leave_temp: write straight to out_path (caller finalizes). Otherwise write
    // to "{out_path}.tmp" and atomically rename on success.
    char temp_path[512];
    const char *write_path;
    if (req->leave_temp) {
        write_path = out_path;
    } else {
        int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", out_path);
        if (n < 0 || n >= (int)sizeof(temp_path)) {
            ESP_LOGE(TAG, "Path too long: %s", out_path);
            free(chunk);
            return ESP_FAIL;
        }
        write_path = temp_path;
    }

    FILE *f = fopen(write_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", write_path);
        free(chunk);
        return ESP_FAIL;
    }

    fetch_sink_t sink = {
        .kind = SINK_FILE,
        .f = f,
        .chunk = chunk,
        .chunk_size = chunk_size,
    };
    esp_err_t err = do_fetch(req, &sink, result);

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(chunk);

    if (err != ESP_OK) {
        unlink(write_path);
        return err;
    }
    if (req->leave_temp) {
        return ESP_OK;  // file is at out_path; caller takes it from here
    }

    unlink(out_path);  // remove any stale file
    if (rename(temp_path, out_path) != 0) {
        ESP_LOGE(TAG, "Rename failed: %s -> %s", temp_path, out_path);
        unlink(temp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}
