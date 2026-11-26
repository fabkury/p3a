// SPDX-License-Identifier: Apache-2.0
// The HTTP server: dispatch through the route tables, and the one place where
// answers are written. See p3a_http.h.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "http_envelope.h"
#include "http_static.h"
#include "lwip/sockets.h"
#include "net_internal.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "psram_alloc.h"

static const char *TAG = "http";

#define MAX_TABLES     16
#define MAX_JSON_BODY  (32 * 1024)  // a playset of 64 channels with long identifiers is about 20 KB
#define MAX_PATH_LEN   256

struct p3a_http_request {
    httpd_req_t *raw;
    p3a_http_method_t method;
    const char *query;  // after the '?', or NULL
    p3a_http_param_t params[P3A_HTTP_MAX_PARAMS];
    cJSON *body;
    bool answered;
    bool no_store;
};

typedef struct {
    const p3a_http_route_t *routes;
    size_t count;
} table_t;

static httpd_handle_t s_server;
static table_t s_tables[MAX_TABLES];
static size_t s_table_count;

// --- reading --------------------------------------------------------------------

bool p3a_http_param(const p3a_http_request_t *req, int index, char *buf, size_t buf_len)
{
    if (index < 0 || index >= P3A_HTTP_MAX_PARAMS || req->params[index].start == NULL) {
        return false;
    }
    return p3a_http_url_decode(req->params[index].start, req->params[index].len, buf, buf_len) >= 0;
}

bool p3a_http_query(const p3a_http_request_t *req, const char *key, char *buf, size_t buf_len)
{
    if (req->query == NULL || key == NULL) {
        return false;
    }
    size_t key_len = strlen(key);
    const char *p = req->query;
    while (*p != '\0') {
        const char *end = strchr(p, '&');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        if (len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            return p3a_http_url_decode(p + key_len + 1, len - key_len - 1, buf, buf_len) >= 0;
        }
        if (len == key_len && strncmp(p, key, key_len) == 0) {
            return p3a_http_url_decode("", 0, buf, buf_len) >= 0;  // "?flag" with no value
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return false;
}

const cJSON *p3a_http_body(const p3a_http_request_t *req)
{
    return req->body;
}

httpd_req_t *p3a_http_raw(p3a_http_request_t *req)
{
    return req->raw;
}

void p3a_http_mark_answered(p3a_http_request_t *req)
{
    req->answered = true;
}

void p3a_http_no_store(p3a_http_request_t *req)
{
    req->no_store = true;
}

// --- answering ------------------------------------------------------------------

static void send_json(p3a_http_request_t *req, int status, const char *text, size_t len)
{
    httpd_resp_set_status(req->raw, p3a_http_status_line(status));
    httpd_resp_set_type(req->raw, "application/json");
    // The API is live state: a cache may keep an answer only to revalidate
    // it, and an answer that carries a secret not at all.
    httpd_resp_set_hdr(req->raw, "Cache-Control", req->no_store ? "no-store" : "no-cache");
    httpd_resp_send(req->raw, text, (ssize_t)len);
    req->answered = true;
}

static void send_error(p3a_http_request_t *req, int status, const char *code, const char *message)
{
    char buf[256];
    size_t len = p3a_http_envelope_error(buf, sizeof(buf), code, message);
    send_json(req, status, buf, len);
}

void p3a_http_ok(p3a_http_request_t *req, cJSON *data)
{
    if (req->answered) {
        cJSON_Delete(data);
        ESP_LOGE(TAG, "%s answered twice", req->raw->uri);
        return;
    }
    char *text = p3a_http_envelope_ok(data);
    if (text == NULL) {
        send_error(req, 503, "NO_MEMORY", "out of memory");
        return;
    }
    size_t len = strlen(text);
    if (req->method == P3A_HTTP_GET && !req->no_store) {
        // The header strings must outlive httpd_resp_send(), which is below.
        char etag[P3A_HTTP_ETAG_LEN];
        char asked[64];
        p3a_http_etag(text, len, etag);
        httpd_resp_set_hdr(req->raw, "ETag", etag);
        if (httpd_req_get_hdr_value_str(req->raw, "If-None-Match", asked, sizeof(asked)) == ESP_OK &&
            p3a_http_etag_matches(asked, etag)) {
            httpd_resp_set_status(req->raw, p3a_http_status_line(304));
            httpd_resp_set_hdr(req->raw, "Cache-Control", "no-cache");
            httpd_resp_send(req->raw, NULL, 0);
            req->answered = true;
        } else {
            send_json(req, 200, text, len);
        }
    } else {
        send_json(req, 200, text, len);
    }
    cJSON_free(text);
}

void p3a_http_accepted(p3a_http_request_t *req, p3a_job_id_t job)
{
    char text[64];
    int len = snprintf(text, sizeof(text), "{\"ok\":true,\"data\":{\"job\":%u}}", (unsigned)job);
    send_json(req, 202, text, (size_t)len);
}

void p3a_http_fail(p3a_http_request_t *req, int status, const char *code, const char *format, ...)
{
    char message[160];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (req->answered) {
        ESP_LOGE(TAG, "%s answered twice", req->raw->uri);
        return;
    }
    send_error(req, status, code, message);
}

void p3a_http_fail_err(p3a_http_request_t *req, esp_err_t err, const char *what)
{
    int status;
    const char *code;
    p3a_http_error_for(err, &status, &code);
    p3a_http_fail(req, status, code, "%s: %s", what, p3a_err_to_name(err));
}

// --- dispatch -------------------------------------------------------------------

static bool method_of(int http_method, p3a_http_method_t *out)
{
    switch (http_method) {
    case HTTP_GET:
        *out = P3A_HTTP_GET;
        return true;
    case HTTP_POST:
        *out = P3A_HTTP_POST;
        return true;
    case HTTP_PUT:
        *out = P3A_HTTP_PUT;
        return true;
    case HTTP_PATCH:
        *out = P3A_HTTP_PATCH;
        return true;
    case HTTP_DELETE:
        *out = P3A_HTTP_DELETE;
        return true;
    default:
        return false;
    }
}

// Reads the whole body and parses it as a JSON object. On failure it has
// answered and returns false.
static bool read_json_body(p3a_http_request_t *req)
{
    size_t len = req->raw->content_len;
    if (len == 0) {
        send_error(req, 400, "INVALID_JSON", "a JSON object is required in the body");
        return false;
    }
    if (len > MAX_JSON_BODY) {
        send_error(req, 413, "TOO_LARGE", "the request body is too large");
        return false;
    }
    char *text = psram_malloc(len + 1);
    if (text == NULL) {
        send_error(req, 503, "NO_MEMORY", "out of memory");
        return false;
    }
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req->raw, text + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(text);
            send_error(req, 400, "READ_BODY", "the request body did not arrive");
            return false;
        }
        got += (size_t)n;
    }
    req->body = cJSON_ParseWithLength(text, len);
    free(text);
    if (!cJSON_IsObject(req->body)) {
        cJSON_Delete(req->body);
        req->body = NULL;
        send_error(req, 400, "INVALID_JSON", "the body is not a JSON object");
        return false;
    }
    return true;
}

static bool serve_unrouted_get(httpd_req_t *raw, const char *path, size_t path_len);

static esp_err_t dispatch(httpd_req_t *raw)
{
    p3a_http_request_t req = {.raw = raw};
    if (!method_of(raw->method, &req.method)) {
        send_error(&req, 405, "METHOD_NOT_ALLOWED", "method not supported");
        return ESP_OK;
    }
    const char *question = strchr(raw->uri, '?');
    size_t path_len = question != NULL ? (size_t)(question - raw->uri) : strlen(raw->uri);
    req.query = question != NULL ? question + 1 : NULL;
    if (path_len > MAX_PATH_LEN) {
        send_error(&req, 414, "URI_TOO_LONG", "the path is too long");
        return ESP_OK;
    }

    const p3a_http_route_t *route = NULL;
    unsigned allowed = 0;
    for (size_t t = 0; t < s_table_count && route == NULL; t++) {
        unsigned these = 0;
        p3a_http_match(s_tables[t].routes, s_tables[t].count, req.method, raw->uri, path_len, &route, req.params,
                       &these);
        allowed |= these;
    }
    if (route == NULL) {
        if (allowed == 0 && req.method == P3A_HTTP_GET && serve_unrouted_get(raw, raw->uri, path_len)) {
            return ESP_OK;
        }
        if (allowed != 0) {
            char allow[40] = "";
            for (int m = 0; m < P3A_HTTP_METHOD_COUNT; m++) {
                if (allowed & (1u << m)) {
                    strlcat(allow, allow[0] != '\0' ? ", " : "", sizeof(allow));
                    strlcat(allow, p3a_http_method_name((p3a_http_method_t)m), sizeof(allow));
                }
            }
            httpd_resp_set_hdr(raw, "Allow", allow);  // `allow` lives until the send below
            send_error(&req, 405, "METHOD_NOT_ALLOWED", "this path does not take that method");
        } else {
            send_error(&req, 404, "NOT_FOUND", "no such path");
        }
        return ESP_OK;
    }

    if ((route->flags & P3A_HTTP_BODY_JSON) && !read_json_body(&req)) {
        return ESP_OK;
    }
    route->handler(&req);
    cJSON_Delete(req.body);
    if (!req.answered) {
        ESP_LOGE(TAG, "%s %s: the handler did not answer", p3a_http_method_name(req.method), route->pattern);
        send_error(&req, 500, "INTERNAL", "the handler did not answer");
    }
    return ESP_OK;
}

// --- pages ----------------------------------------------------------------------

#define SETUP_URL "http://192.168.4.1/"

static void send_static(httpd_req_t *raw, const p3a_static_file_t *file)
{
    char asked[64];
    httpd_resp_set_hdr(raw, "ETag", file->etag);
    // Kept by the browser but checked each time: a 304 is a few bytes, and a
    // firmware update is seen at once.
    httpd_resp_set_hdr(raw, "Cache-Control", "no-cache");
    if (httpd_req_get_hdr_value_str(raw, "If-None-Match", asked, sizeof(asked)) == ESP_OK &&
        p3a_http_etag_matches(asked, file->etag)) {
        httpd_resp_set_status(raw, p3a_http_status_line(304));
        httpd_resp_send(raw, NULL, 0);
        return;
    }
    httpd_resp_set_type(raw, file->content_type);
    if (file->gzipped) {
        httpd_resp_set_hdr(raw, "Content-Encoding", "gzip");
    }
    httpd_resp_send(raw, (const char *)file->data, (ssize_t)file->len);
}

// A GET that no route took: a built-in page, or, while the device is its own
// access point, the way to the setup page. Returns false for "answer 404".
//
// In setup every unknown page redirects. A phone that has just joined asks
// its operating system's probe URL (/generate_204, /hotspot-detect.html,
// /connecttest.txt and others); getting a redirect instead of the expected
// answer is what makes it open its sign-in sheet on the target.
static bool serve_unrouted_get(httpd_req_t *raw, const char *path, size_t path_len)
{
    const p3a_static_file_t *file = p3a_static_find(path, path_len);
    if (net_setup_active() && path_len == 1) {
        file = p3a_static_find("/setup/", 7);  // not the home page: there is no network for it to be of use on
    } else if (file == NULL && net_setup_active()) {
        if (strncmp(path, "/api/", 5) != 0) {
            httpd_resp_set_status(raw, "302 Found");
            httpd_resp_set_hdr(raw, "Location", SETUP_URL);
            httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
            httpd_resp_set_type(raw, "text/html");
            httpd_resp_sendstr(raw, "<a href=\"" SETUP_URL "\">p3a setup</a>");
            return true;
        }
    }
    if (file == NULL) {
        return false;
    }
    send_static(raw, file);
    return true;
}

// --- the server -----------------------------------------------------------------

// Close with a reset instead of lingering in TIME_WAIT. lwIP has 32 sockets
// for everything (HTTP, MQTT, downloads, updates), and a browser opening and
// closing connections can otherwise exhaust them.
static esp_err_t on_open(httpd_handle_t server, int fd)
{
    (void)server;
    struct linger hard = {.l_onoff = 1, .l_linger = 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));
    return ESP_OK;
}

static void on_close(httpd_handle_t server, int fd)
{
    (void)server;
    close(fd);
}

esp_err_t p3a_http_add_routes(const p3a_http_route_t *routes, size_t count)
{
    if (routes == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_table_count >= MAX_TABLES) {
        // Nobody checks this call's answer: without this line the routes are
        // simply not there (found when the ninth table was added).
        ESP_LOGE(TAG, "no room for another route table (%d): %s and its neighbours are not served", MAX_TABLES,
                 routes[0].pattern);
        return ESP_ERR_NO_MEM;
    }
    // Written before the count, so that the server task never sees a slot
    // that is counted but not yet filled.
    s_tables[s_table_count].routes = routes;
    s_tables[s_table_count].count = count;
    s_table_count++;
    return ESP_OK;
}

bool p3a_http_is_up(void)
{
    return s_server != NULL;
}

esp_err_t p3a_http_start(void)
{
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    net_api_register();

    // A client that resets a kept-alive connection instead of closing it is
    // ordinary on a LAN and not worth a warning each time.
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0;  // off the render core (docs/jitter, fix 7b)
    config.stack_size = 8192;
    config.server_port = 80;
    config.max_open_sockets = 12;
    config.lru_purge_enable = true;  // a new client may displace the idlest one
    config.max_uri_handlers = P3A_HTTP_METHOD_COUNT;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.open_fn = on_open;
    config.close_fn = on_close;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the HTTP server did not start: %s", esp_err_to_name(err));
        return err;
    }
    static const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_PATCH, HTTP_DELETE};
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        const httpd_uri_t all = {.uri = "/*", .method = methods[i], .handler = dispatch};
        err = httpd_register_uri_handler(s_server, &all);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not register a method: %s", esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "listening on port 80");
    return ESP_OK;
}
