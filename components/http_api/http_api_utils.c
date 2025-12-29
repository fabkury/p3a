// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_utils.c
 * @brief HTTP API utility functions
 * 
 * Contains helper functions for HTTP handling, JSON processing, and MIME types
 */

#include "http_api_internal.h"
#include <strings.h>

const char* http_status_str(int status) {
    switch(status) {
        case 200: return "200 OK";
        case 202: return "202 Accepted";
        case 400: return "400 Bad Request";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 415: return "415 Unsupported Media Type";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default: return "500 Internal Server Error";
    }
}

void send_json(httpd_req_t *req, int status, const char *json) {
    httpd_resp_set_status(req, http_status_str(status));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
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

    char *buf = malloc(total + 1);
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
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    
    return "application/octet-stream";
}

