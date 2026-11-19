// SPDX-License-Identifier: Apache-2.0
// Test hooks reached over HTTP, for what the UART is too slow or too narrow
// for. They exist only in the test build, under /api/v2/test/.
//
//   GET  /api/v2/test/screenshot[?div=N]  what the test hooks last put on the
//                                         panel, as a BMP; N keeps every Nth pixel
//        ...&crc=1                        the CRC-32 of those pixels instead (blue,
//                                         green, red; top row first), so that a test
//                                         downloads the picture only on a mismatch
//   GET  /api/v2/test/log[?since=N]       log lines captured since line N
//   GET  /api/v2/test/clock               the clock
//   POST /api/v2/test/clock               {"unix": seconds}: set the clock

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_board.h"
#include "p3a_clock.h"
#include "p3a_crc32.h"
#include "p3a_http.h"
#include "p3a_test_internal.h"
#include "psram_alloc.h"

// --- screenshot -----------------------------------------------------------------

static void put_u32(uint8_t *at, uint32_t v)
{
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
    at[2] = (uint8_t)(v >> 16);
    at[3] = (uint8_t)(v >> 24);
}

static void get_screenshot(p3a_http_request_t *req)
{
    char text[8];
    int div = p3a_http_query(req, "div", text, sizeof(text)) ? atoi(text) : 1;
    const uint8_t *buffer = p3a_test_shown_buffer();
    if (buffer == NULL) {
        p3a_http_fail(req, 409, "NOT_NOW", "the test hooks have not put anything on the panel yet");
        return;
    }
    if (div < 1 || div > 90 || P3A_DISPLAY_WIDTH % div != 0) {
        p3a_http_fail(req, 400, "INVALID", "div must divide %d (1..90)", P3A_DISPLAY_WIDTH);
        return;
    }
    const int w = P3A_DISPLAY_WIDTH / div, h = P3A_DISPLAY_HEIGHT / div;
    if (p3a_http_query(req, "crc", text, sizeof(text))) {
        const size_t line = p3a_board_get_row_stride();
        uint32_t crc = 0;
        for (int y = 0; y < h; y++) {
            const uint8_t *src = buffer + (size_t)(y * div + div / 2) * line;
            if (div == 1) {
                crc = p3a_crc32(crc, src, (size_t)w * 3);
            } else {
                for (int x = 0; x < w; x++) {
                    crc = p3a_crc32(crc, src + (size_t)(x * div + div / 2) * 3, 3);
                }
            }
        }
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "crc", crc);
        cJSON_AddNumberToObject(data, "w", w);
        cJSON_AddNumberToObject(data, "h", h);
        p3a_http_ok(req, data);
        return;
    }
    const size_t row_bytes = ((size_t)w * 3 + 3) & ~(size_t)3;  // BMP rows are padded to 4 bytes
    // Rows are sent a batch at a time: one small send per row made the full
    // frame take over ten seconds.
    const int batch = 32;
    uint8_t *rows = psram_calloc((size_t)batch, row_bytes);
    if (rows == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "taking the screenshot");
        return;
    }
    // A 24-bit BMP, top row first (negative height). The framebuffer is
    // already in BMP's byte order: blue, green, red.
    uint8_t header[54] = {'B', 'M'};
    put_u32(header + 2, (uint32_t)(sizeof(header) + row_bytes * (size_t)h));
    put_u32(header + 10, sizeof(header));
    put_u32(header + 14, 40);
    put_u32(header + 18, (uint32_t)w);
    put_u32(header + 22, (uint32_t)-h);
    header[26] = 1;
    header[28] = 24;
    put_u32(header + 34, (uint32_t)(row_bytes * (size_t)h));

    httpd_req_t *raw = p3a_http_raw(req);
    httpd_resp_set_type(raw, "image/bmp");
    httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(raw, (const char *)header, sizeof(header));
    const size_t stride = p3a_board_get_row_stride();
    int held = 0;
    for (int y = 0; y < h && err == ESP_OK; y++) {
        const uint8_t *src = buffer + (size_t)(y * div + div / 2) * stride;
        uint8_t *row = rows + (size_t)held * row_bytes;
        if (div == 1) {
            memcpy(row, src, (size_t)w * 3);
        } else {
            for (int x = 0; x < w; x++) {
                memcpy(row + (size_t)x * 3, src + (size_t)(x * div + div / 2) * 3, 3);
            }
        }
        if (++held == batch || y == h - 1) {
            err = httpd_resp_send_chunk(raw, (const char *)rows, (ssize_t)(row_bytes * (size_t)held));
            held = 0;
        }
    }
    free(rows);
    httpd_resp_send_chunk(raw, NULL, 0);
    p3a_http_mark_answered(req);
}

// --- log capture ----------------------------------------------------------------
//
// The last LOG_LINES log lines, numbered from 1 since the hook was installed.

#define LOG_LINES    256
#define LOG_LINE_MAX 160

static char (*s_log)[LOG_LINE_MAX];
static uint32_t s_log_count;  // lines captured so far; line n lives in slot (n - 1) % LOG_LINES
static SemaphoreHandle_t s_log_lock;
static vprintf_like_t s_log_previous;

static int capture_vprintf(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    // Never wait: a log call that cannot be captured at once is only printed.
    if (s_log != NULL && xSemaphoreTake(s_log_lock, 0) == pdTRUE) {
        char *slot = s_log[s_log_count % LOG_LINES];
        int n = vsnprintf(slot, LOG_LINE_MAX, format, copy);
        if (n > 0) {
            size_t len = strlen(slot);
            while (len > 0 && (slot[len - 1] == '\n' || slot[len - 1] == '\r')) {
                slot[--len] = '\0';
            }
            if (len > 0) {
                s_log_count++;
            }
        }
        xSemaphoreGive(s_log_lock);
    }
    va_end(copy);
    return s_log_previous != NULL ? s_log_previous(format, args) : vprintf(format, args);
}

static void get_log(p3a_http_request_t *req)
{
    char text[12];
    uint32_t since = p3a_http_query(req, "since", text, sizeof(text)) ? (uint32_t)strtoul(text, NULL, 10) : 0;
    cJSON *data = cJSON_CreateObject();
    cJSON *lines = cJSON_AddArrayToObject(data, "lines");
    char line[LOG_LINE_MAX];
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    const uint32_t count = s_log_count;
    xSemaphoreGive(s_log_lock);
    uint32_t first = count > LOG_LINES ? count - LOG_LINES : 0;  // lines 1..first have been overwritten
    if (since > first) {
        first = since;
    }
    for (uint32_t n = first; n < count; n++) {
        xSemaphoreTake(s_log_lock, portMAX_DELAY);
        memcpy(line, s_log[n % LOG_LINES], sizeof(line));
        xSemaphoreGive(s_log_lock);
        // A log line is bytes from anywhere; keep what JSON can carry.
        for (char *c = line; *c != '\0'; c++) {
            if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e) {
                *c = '?';
            }
        }
        cJSON_AddItemToArray(lines, cJSON_CreateString(line));
    }
    cJSON_AddNumberToObject(data, "first", first + 1);
    cJSON_AddNumberToObject(data, "next", count);  // pass this as `since` to read on
    p3a_http_no_store(req);
    p3a_http_ok(req, data);
}

// --- clock ----------------------------------------------------------------------

static void reply_clock(p3a_http_request_t *req)
{
    char now[P3A_CLOCK_ISO8601_LEN] = "";
    p3a_clock_now_iso8601(now, sizeof(now));
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "set", p3a_clock_is_set());
    cJSON_AddStringToObject(data, "now", now);
    cJSON_AddNumberToObject(data, "unix", (double)time(NULL));
    p3a_http_no_store(req);
    p3a_http_ok(req, data);
}

static void get_clock(p3a_http_request_t *req)
{
    reply_clock(req);
}

static void post_clock(p3a_http_request_t *req)
{
    const cJSON *unix_time = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "unix");
    if (!cJSON_IsNumber(unix_time) || unix_time->valuedouble < 0 || unix_time->valuedouble > 4102444800.0) {
        p3a_http_fail(req, 400, "INVALID", "send {\"unix\": seconds since 1970}");
        return;
    }
    const struct timeval tv = {.tv_sec = (time_t)unix_time->valuedouble};
    settimeofday(&tv, NULL);
    p3a_clock_mark_set();
    reply_clock(req);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, P3A_HTTP_RAW, "/api/v2/test/screenshot", get_screenshot},
    {P3A_HTTP_GET, 0, "/api/v2/test/log", get_log},
    {P3A_HTTP_GET, 0, "/api/v2/test/clock", get_clock},
    {P3A_HTTP_POST, P3A_HTTP_BODY_JSON, "/api/v2/test/clock", post_clock},
};

void p3a_test_http_start(void)
{
    s_log_lock = xSemaphoreCreateMutex();
    s_log = psram_calloc(LOG_LINES, LOG_LINE_MAX);
    if (s_log_lock != NULL && s_log != NULL) {
        s_log_previous = esp_log_set_vprintf(capture_vprintf);
    }
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
