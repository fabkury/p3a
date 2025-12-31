/**
 * @file http_api_page_root.c
 * @brief Main control page handler
 *
 * Serves the main remote control page from LittleFS at GET /
 */

#include "http_api_internal.h"

/**
 * GET /
 * Serves the main control page from /spiffs/index.html
 */
esp_err_t h_get_root(httpd_req_t *req) {
    return serve_file(req, "/spiffs/index.html");
}
