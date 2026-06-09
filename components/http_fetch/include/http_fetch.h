// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file http_fetch.h
 * @brief Shared HTTP fetch/download helper.
 *
 * Collapses the esp_http_client boilerplate that was hand-rolled across the
 * Giphy, museum (art_institution), Makapix and show_url fetchers into one
 * place: client config + cert bundle, optional POST body, status
 * classification, a unified retry / truncated-read / Retry-After policy
 * (modeled on the Giphy path), optional manual 3xx-redirect following, an
 * optional SDIO-bus poll-wait, and (for the file variant) an atomic
 * temp-file-then-rename write.
 *
 * Two sinks, one core:
 *   - http_fetch_to_buffer() — body into a caller-owned buffer (JSON/text APIs)
 *   - http_fetch_to_file()   — body into a file on the SD card (binary artwork)
 *
 * Domain-specific behavior stays in the caller and is injected via callbacks:
 *   - on_rate_limited: caller records the 429 cooldown (e.g. per-museum table)
 *   - should_abort:    caller decides whether to cancel mid-transfer
 *   - progress:        caller reports/render download progress
 * This keeps http_fetch free of any dependency on the fetchers (no cycle).
 *
 * Lifetime: @p req and everything it points to (url, headers, body,
 * user_agent) must remain valid for the duration of the call.
 */

#ifndef HTTP_FETCH_H
#define HTTP_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HTTP_FETCH_GET = 0,
    HTTP_FETCH_POST,
} http_fetch_method_t;

typedef enum {
    HTTP_FETCH_REDIRECT_AUTO = 0,    ///< esp_http_client follows redirects itself
    HTTP_FETCH_REDIRECT_MANUAL,      ///< follow 3xx manually, capturing Location (resets sink per hop)
} http_fetch_redirect_mode_t;

typedef struct {
    const char *name;
    const char *value;
} http_fetch_header_t;

/** Optional per-call outcome detail. May be NULL. */
typedef struct {
    int      http_status;       ///< last HTTP status seen
    size_t   bytes;             ///< bytes delivered to the sink
    int64_t  content_length;    ///< server Content-Length, or -1 if unknown
    int      attempts;          ///< HTTP attempts consumed (across retries+hops)
    bool     buffer_full;       ///< fetch_to_buffer hit buf_size-1 (response too big)
    bool     aborted;           ///< should_abort fired
    uint32_t retry_after_sec;   ///< parsed Retry-After on 429 (0 if absent/non-numeric)
} http_fetch_result_t;

/** Return true to abort the transfer. Checked once per chunk. */
typedef bool (*http_fetch_should_abort_cb)(void *ctx);

/** Called after each received/written chunk, for both the buffer and file
 *  sinks (caller does its own throttling). content_length is 0 if unknown. */
typedef void (*http_fetch_progress_cb)(size_t total, size_t content_length, void *ctx);

/** Called once on HTTP 429 before returning ESP_ERR_INVALID_RESPONSE. */
typedef void (*http_fetch_rate_limited_cb)(uint32_t retry_after_sec, void *ctx);

/**
 * Request descriptor. Zero-initialize and set what you need; zeroed fields take
 * sane defaults (GET, 15 s timeout, 4096 rx buffer, 3 attempts, {0,1000,3000}ms
 * backoff, 32 KiB chunks, auto-redirect, no size cap, no SDIO wait).
 */
typedef struct {
    // --- target ---
    const char               *url;             ///< required
    http_fetch_method_t       method;          ///< default GET
    const void               *body;            ///< POST body (streamed via open(len)+write)
    size_t                    body_len;         ///< POST body length

    // --- request shaping ---
    const http_fetch_header_t *headers;        ///< extra request headers
    size_t                    header_count;
    int                       timeout_ms;      ///< 0 -> 15000
    int                       rx_buffer_size;  ///< esp_http_client buffer_size; 0 -> 4096
    int                       tx_buffer_size;  ///< esp_http_client buffer_size_tx; 0 -> IDF default
    const char               *user_agent;      ///< convenience; added as "User-Agent" if non-NULL
    bool                      accept_2xx;      ///< any 2xx counts as success (default: 200 only);
                                               ///< caller inspects result->http_status for the exact code

    // --- redirects ---
    http_fetch_redirect_mode_t redirect_mode;  ///< default AUTO
    int                        max_redirects;  ///< MANUAL only; 0 -> 5

    // --- standardized retry policy ---
    int                        max_attempts;   ///< 0 -> 3
    const uint32_t            *backoff_ms;      ///< NULL -> {0,1000,3000}; else length == max_attempts

    // --- SDIO coordination (poll sdio_bus_is_locked; never acquires) ---
    bool                       wait_on_sdio;
    int                        sdio_wait_max_s; ///< 0 -> 120
    const char                *sdio_requester;  ///< log tag

    // --- size / truncation policy ---
    int64_t                    max_size;             ///< hard cap; 0 -> no cap
    size_t                     min_size;             ///< reject success if bytes < min_size (0 -> none)
    bool                       require_exact_length; ///< reject success if bytes != Content-Length
    bool                       treat_empty_as_not_found; ///< 200 + Content-Length 0 -> ESP_ERR_NOT_FOUND
    bool                       allow_empty_body;     ///< a 0-byte body is success (default: treated
                                                     ///< as a truncated read and retried)

    // --- file sink only ---
    size_t                     chunk_size;      ///< 0 -> 32768
    bool                       leave_temp;      ///< write to out_path directly, fsync+close, NO rename

    // --- callbacks (all receive user_ctx) ---
    http_fetch_should_abort_cb should_abort;
    http_fetch_progress_cb     progress;
    http_fetch_rate_limited_cb on_rate_limited;
    void                      *user_ctx;
} http_fetch_request_t;

/**
 * @brief Fetch an HTTP response body into a caller-owned buffer.
 *
 * The buffer is always NUL-terminated; at most buf_size-1 payload bytes are
 * stored. If the body would exceed buf_size-1, returns ESP_ERR_INVALID_SIZE
 * with result->buffer_full = true and NO retry.
 *
 * @param req       request descriptor (must outlive the call)
 * @param buf       caller-owned destination buffer
 * @param buf_size  size of @p buf in bytes (must be > 0)
 * @param out_len   optional; receives payload byte count (excludes NUL)
 * @param result    optional; receives outcome detail
 * @return ESP_OK, or ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_RESPONSE (429) /
 *         ESP_ERR_NOT_ALLOWED (401/403) / ESP_ERR_INVALID_SIZE /
 *         ESP_ERR_INVALID_STATE (aborted) / ESP_ERR_TIMEOUT (sdio) /
 *         ESP_ERR_INVALID_ARG / ESP_FAIL.
 */
esp_err_t http_fetch_to_buffer(const http_fetch_request_t *req,
                               char *buf, size_t buf_size,
                               size_t *out_len,
                               http_fetch_result_t *result);

/**
 * @brief Download an HTTP response body to a file on the SD card.
 *
 * Writes to "{out_path}.tmp", fsyncs, then atomically renames to out_path on
 * success (unlinks the temp on failure). With req->leave_temp = true, writes
 * directly to out_path with no rename — the caller owns finalization (used by
 * show_url, which sniffs the file then renames to a unique name itself).
 *
 * The caller owns parent-directory creation and the final path/name.
 *
 * @param req       request descriptor (must outlive the call)
 * @param out_path  destination path (or, with leave_temp, the path to write)
 * @param result    optional; receives outcome detail
 * @return ESP_OK or one of the error codes listed for http_fetch_to_buffer
 *         (plus ESP_ERR_NO_MEM if the chunk buffer can't be allocated).
 */
esp_err_t http_fetch_to_file(const http_fetch_request_t *req,
                             const char *out_path,
                             http_fetch_result_t *result);

#ifdef __cplusplus
}
#endif

#endif // HTTP_FETCH_H
