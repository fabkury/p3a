// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file http_api_utils.c
 * @brief HTTP API utility functions
 * 
 * Contains helper functions for HTTP handling, JSON processing, and MIME types
 */

#include "http_api_internal.h"
#include "psram_alloc.h"
#include "play_scheduler.h"
#include <stdarg.h>
#include <strings.h>

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void url_decode_in_place(char *str)
{
    if (!str) return;
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = hex_digit(src[1]);
            int lo = hex_digit(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

const char* http_status_str(int status) {
    switch(status) {
        case 200: return "200 OK";
        case 202: return "202 Accepted";
        case 400: return "400 Bad Request";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 415: return "415 Unsupported Media Type";
        case 500: return "500 Internal Server Error";
        case 404: return "404 Not Found";
        case 503: return "503 Service Unavailable";
        case 504: return "504 Gateway Timeout";
        default: return "500 Internal Server Error";
    }
}

void send_json(httpd_req_t *req, int status, const char *json) {
    httpd_resp_set_status(req, http_status_str(status));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

void send_json_error(httpd_req_t *req, int status, const char *code, const char *msg) {
    if (!code) code = "ERROR";
    if (!msg) msg = code;
    // code/msg are plain-ASCII literals from call sites (no JSON escaping done);
    // a fixed buffer keeps this path allocation-free so it works under OOM.
    char buf[224];
    int n = snprintf(buf, sizeof(buf),
                     "{\"ok\":false,\"error\":\"%s\",\"code\":\"%s\"}", msg, code);
    if (n < 0 || n >= (int)sizeof(buf)) {
        send_json(req, status, "{\"ok\":false,\"error\":\"Internal error\",\"code\":\"INTERNAL\"}");
        return;
    }
    send_json(req, status, buf);
}

void send_json_errorf(httpd_req_t *req, int status, const char *code, const char *fmt, ...) {
    char msg[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    send_json_error(req, status, code, msg);
}

void send_json_oom(httpd_req_t *req) {
    send_json_error(req, 500, "OOM", "Out of memory");
}

void send_json_root(httpd_req_t *req, int status, cJSON *root) {
    if (!root) {
        send_json_oom(req);
        return;
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        send_json_oom(req);
        return;
    }
    send_json(req, status, out);
    free(out);
}

cJSON *recv_json_object(httpd_req_t *req) {
    int err_status = 0;
    size_t len = 0;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        if (err_status == 413) {
            send_json_error(req, 413, "PAYLOAD_TOO_LARGE", "Request body too large");
        } else {
            send_json_error(req, err_status ? err_status : 500, "READ_BODY",
                            "Failed to read request body");
        }
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json_error(req, 400, "INVALID_JSON", "Invalid JSON");
        return NULL;
    }
    return root;
}

const char *json_get_string(const cJSON *obj, const char *key, const char *def) {
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : def;
}

int json_get_int(const cJSON *obj, const char *key, int def) {
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(it) ? it->valueint : def;
}

bool json_get_bool(const cJSON *obj, const char *key, bool def) {
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsBool(it) ? cJSON_IsTrue(it) : def;
}

const char *pick_mode_str(int mode) {
    switch ((ps_pick_mode_t)mode) {
        case PS_PICK_RECENCY: return "recency";
        case PS_PICK_RANDOM:  return "random";
        default:              return "unknown";
    }
}

bool ensure_json_content(httpd_req_t *req) {
    char content_type[64] = {0};
    esp_err_t ret = httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    if (ret != ESP_OK) {
        return false;
    }
    // Check if starts with "application/json"
    return (strncasecmp(content_type, "application/json", 16) == 0);
}

char* recv_body_json(httpd_req_t *req, size_t *out_len, int *out_err_status) {
    size_t total = req->content_len;
    
    if (total > MAX_JSON) {
        *out_err_status = 413;
        return NULL;
    }

    char *buf = psram_malloc(total + 1);
    if (!buf) {
        *out_err_status = 500;
        return NULL;
    }

    size_t recvd = 0;
    while(recvd < total) {
        size_t want = total - recvd;
        if (want > RECV_CHUNK) {
            want = RECV_CHUNK;
        }

        int r = httpd_req_recv(req, buf + recvd, want);
        if (r <= 0) {
            free(buf);
            *out_err_status = 500;
            return NULL;
        }
        recvd += r;
    }

    buf[recvd] = '\0';
    *out_len = recvd;
    *out_err_status = 0;
    return buf;
}

void register_uri_handler_or_log(httpd_handle_t server, httpd_uri_t *uri) {
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "Failed to register URI %s: %s", uri->uri, esp_err_to_name(err));
    }
}

const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    ext++; // Skip the dot
    
    if (strcasecmp(ext, "html") == 0) return "text/html";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "wasm") == 0) return "application/wasm";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "webmanifest") == 0) return "application/manifest+json";

    return "application/octet-stream";
}

