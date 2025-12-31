/**
 * @file http_api_pages.c
 * @brief HTTP API page routing and static file serving
 *
 * Contains:
 * - serve_file(): Generic file server with gzip support
 * - GET /favicon.ico handler
 * - GET /static/... static file serving
 * - Page routing (routes to handlers in http_api_page_*.c)
 * - Handler registration
 *
 * Page handlers are in separate files:
 * - http_api_page_root.c: GET / (main control page)
 * - http_api_page_network.c: GET /config/network, POST /erase
 * - http_api_page_settings.c: GET /settings
 * - http_api_pico8.c: GET /pico8, WS /pico_stream
 */

#include "http_api_internal.h"
#include <sys/stat.h>

// ---------- Generic File Server with Gzip Support ----------

/**
 * @brief Serve a file from LittleFS with automatic gzip support
 *
 * Tries to serve the gzipped version (filepath.gz) first. If found,
 * sets Content-Encoding: gzip header and browser decompresses.
 * Falls back to uncompressed version if .gz not found.
 *
 * @param req HTTP request
 * @param filepath Full path to file (e.g., "/spiffs/index.html")
 * @return ESP_OK on success, error code on failure
 */
esp_err_t serve_file(httpd_req_t *req, const char *filepath) {
    char gz_path[MAX_FILE_PATH];
    bool is_gzipped = false;
    FILE *f = NULL;

    // Try gzipped version first
    if (strlen(filepath) + 3 < sizeof(gz_path)) {
        snprintf(gz_path, sizeof(gz_path), "%s.gz", filepath);
        f = fopen(gz_path, "r");
        if (f) {
            is_gzipped = true;
            ESP_LOGD(HTTP_API_TAG, "Serving gzipped: %s", gz_path);
        }
    }

    // Fall back to uncompressed
    if (!f) {
        f = fopen(filepath, "r");
        if (!f) {
            ESP_LOGE(HTTP_API_TAG, "Failed to open %s", filepath);
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_send(req, "File not found", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 10 * 1024 * 1024) { // Max 10MB
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Invalid file size", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Set MIME type based on original filepath (not .gz path)
    httpd_resp_set_type(req, get_mime_type(filepath));

    // Set gzip encoding header if serving compressed file
    if (is_gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    // Set cache headers for static assets
    if (strstr(filepath, ".js") || strstr(filepath, ".wasm") ||
        strstr(filepath, ".css") || strstr(filepath, ".png") ||
        strstr(filepath, ".ico")) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    }

    // Stream file in chunks
    char chunk[RECV_CHUNK];
    long remaining = size;

    while (remaining > 0) {
        size_t to_read = (remaining < RECV_CHUNK) ? remaining : RECV_CHUNK;
        size_t bytes_read = fread(chunk, 1, to_read, f);

        if (bytes_read == 0) {
            break;
        }

        esp_err_t ret = httpd_resp_send_chunk(req, chunk, bytes_read);
        if (ret != ESP_OK) {
            fclose(f);
            return ret;
        }

        remaining -= bytes_read;
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // End response

    return ESP_OK;
}

// ---------- Favicon Handler ----------

/**
 * GET /favicon.ico
 * Serves favicon from LittleFS
 */
static esp_err_t h_get_favicon(httpd_req_t *req) {
    return serve_file(req, "/spiffs/favicon.ico");
}

// ---------- Static File Handler ----------

/**
 * GET /static/<path>
 * Serves static files from LittleFS with gzip support
 */
static esp_err_t h_get_static(httpd_req_t *req) {
    const char* uri = req->uri;

    // Map /static/* to /spiffs/static/*
    char filepath[MAX_FILE_PATH];
    static const char *prefix = "/spiffs";
    size_t prefix_len = strlen(prefix);
    size_t uri_len = strlen(uri);
    if (prefix_len + uri_len >= sizeof(filepath)) {
        ESP_LOGW(HTTP_API_TAG, "Static path too long: %s", uri);
        httpd_resp_set_status(req, "414 Request-URI Too Long");
        httpd_resp_send(req, "Path too long", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    snprintf(filepath, sizeof(filepath), "%s%s", prefix, uri);

    return serve_file(req, filepath);
}

// ---------- Sub-router Entrypoints ----------

esp_err_t http_api_pages_route_get(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(uri, "/favicon.ico") == 0) {
        return h_get_favicon(req);
    }
    if (strcmp(uri, "/") == 0) {
        return h_get_root(req);
    }
    if (strcmp(uri, "/config/network") == 0) {
        return h_get_network_config(req);
    }
    if (strcmp(uri, "/settings") == 0) {
        return h_get_settings(req);
    }

#if CONFIG_P3A_PICO8_ENABLE
    if (strcmp(uri, "/pico8") == 0) {
        return h_get_pico8(req);
    }
#endif

    return ESP_ERR_NOT_FOUND;
}

esp_err_t http_api_pages_route_post(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(uri, "/erase") == 0) {
        return h_post_erase(req);
    }

    return ESP_ERR_NOT_FOUND;
}

// ---------- Registration Function ----------

void http_api_register_page_handlers(httpd_handle_t server) {
    httpd_uri_t u = {0};

    u.uri = "/static/*";
    u.method = HTTP_GET;
    u.handler = h_get_static;
    u.user_ctx = NULL;
    register_uri_handler_or_log(server, &u);

#if CONFIG_P3A_PICO8_ENABLE
    // WebSocket endpoint for PICO-8 streaming
    httpd_uri_t ws_uri = {
        .uri = "/pico_stream",
        .method = HTTP_GET,
        .handler = h_ws_pico_stream,
        .user_ctx = NULL,
        .is_websocket = true
    };
    register_uri_handler_or_log(server, &ws_uri);
#endif
}
