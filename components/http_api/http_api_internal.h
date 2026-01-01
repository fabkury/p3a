// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_internal.h
 * @brief Internal shared header for http_api module
 * 
 * Contains shared types, macros, and function declarations used across
 * the http_api source files. Not intended for external use.
 */
#pragma once

#include "http_api.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------- Configuration Constants ----------

#define HTTP_API_TAG "HTTP"
#define MAX_JSON (32 * 1024)
#define RECV_CHUNK 4096
#define QUEUE_LEN 10
#define MAX_FILE_PATH 256

#if CONFIG_P3A_PICO8_ENABLE
#define WS_MAX_FRAME_SIZE (8192 + 48 + 6) // framebuffer + palette + magic+len+flags header
#endif

// LCD dimensions - include app_lcd.h for EXAMPLE_LCD_H_RES/V_RES
#include "app_lcd.h"
#define LCD_MAX_WIDTH   EXAMPLE_LCD_H_RES
#define LCD_MAX_HEIGHT  EXAMPLE_LCD_V_RES

// ---------- Command Types ----------

typedef enum {
    CMD_REBOOT,
    CMD_SWAP_NEXT,
    CMD_SWAP_BACK,
    CMD_PAUSE,
    CMD_RESUME
} command_type_t;

typedef struct {
    command_type_t type;
    uint32_t id;
} command_t;

// ---------- Shared State (extern declarations) ----------

extern QueueHandle_t s_cmdq;
extern httpd_handle_t s_server;

// ---------- HTTP Helper Functions (http_api_utils.c) ----------

/**
 * @brief Get HTTP status string from status code
 */
const char* http_status_str(int status);

/**
 * @brief Send JSON response with given status code
 */
void send_json(httpd_req_t *req, int status, const char *json);

/**
 * @brief Check if request has JSON content type
 */
bool ensure_json_content(httpd_req_t *req);

/**
 * @brief Receive JSON body from request
 * @param req HTTP request
 * @param out_len Output: received length
 * @param out_err_status Output: error status code if failed
 * @return Allocated buffer with JSON body, or NULL on error
 */
char* recv_body_json(httpd_req_t *req, size_t *out_len, int *out_err_status);

/**
 * @brief Register URI handler with error logging
 */
void register_uri_handler_or_log(httpd_handle_t server, httpd_uri_t *uri);

/**
 * @brief Get MIME type from file path extension
 */
const char* get_mime_type(const char* path);

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
esp_err_t serve_file(httpd_req_t *req, const char *filepath);

// ---------- Command Queue Functions (http_api.c) ----------

bool api_enqueue_pause(void);
bool api_enqueue_resume(void);

// ---------- REST API Handlers (http_api_rest.c) ----------

esp_err_t h_get_ui_config(httpd_req_t *req);
esp_err_t h_get_network_status(httpd_req_t *req);
esp_err_t h_get_status(httpd_req_t *req);
esp_err_t h_get_api_state(httpd_req_t *req);
esp_err_t h_get_channels_stats(httpd_req_t *req);
esp_err_t h_get_config(httpd_req_t *req);
esp_err_t h_put_config(httpd_req_t *req);
esp_err_t h_get_channel(httpd_req_t *req);
esp_err_t h_post_channel(httpd_req_t *req);
esp_err_t h_get_dwell_time(httpd_req_t *req);
esp_err_t h_put_dwell_time(httpd_req_t *req);
esp_err_t h_get_global_seed(httpd_req_t *req);
esp_err_t h_put_global_seed(httpd_req_t *req);
esp_err_t h_get_play_order(httpd_req_t *req);
esp_err_t h_put_play_order(httpd_req_t *req);
esp_err_t h_post_reboot(httpd_req_t *req);
esp_err_t h_post_swap_next(httpd_req_t *req);
esp_err_t h_post_swap_back(httpd_req_t *req);
esp_err_t h_post_pause(httpd_req_t *req);
esp_err_t h_post_resume(httpd_req_t *req);
esp_err_t h_get_rotation(httpd_req_t *req);
esp_err_t h_post_rotation(httpd_req_t *req);

#if CONFIG_OTA_DEV_MODE
esp_err_t h_post_debug(httpd_req_t *req);
#endif

// ---------- Page Handlers (http_api_page_*.c) ----------

/**
 * @brief GET / - Main control page (http_api_page_root.c)
 */
esp_err_t h_get_root(httpd_req_t *req);

/**
 * @brief GET /config/network - Network status page (http_api_page_network.c)
 */
esp_err_t h_get_network_config(httpd_req_t *req);

/**
 * @brief POST /erase - Erase WiFi credentials (http_api_page_network.c)
 */
esp_err_t h_post_erase(httpd_req_t *req);

/**
 * @brief GET /settings - Settings page (http_api_page_settings.c)
 */
esp_err_t h_get_settings(httpd_req_t *req);

// ---------- Handler Registration Functions ----------

/**
 * @brief Register page handlers (root, config/network, erase, favicon, static files, pico8, pico_stream)
 */
void http_api_register_page_handlers(httpd_handle_t server);

/**
 * @brief Register OTA handlers (/ota, /ota/status, /ota/check, /ota/install, /ota/rollback)
 */
void http_api_register_ota_handlers(httpd_handle_t server);

/**
 * @brief Register upload handler (/upload)
 */
void http_api_register_upload_handler(httpd_handle_t server);

// ---------- Sub-router Functions ----------

/**
 * @brief Route GET request through pages handlers
 */
esp_err_t http_api_pages_route_get(httpd_req_t *req);

/**
 * @brief Route POST request through pages handlers
 */
esp_err_t http_api_pages_route_post(httpd_req_t *req);

/**
 * @brief Route GET request through OTA handlers
 */
esp_err_t http_api_ota_route_get(httpd_req_t *req);

/**
 * @brief Route POST request through OTA handlers
 */
esp_err_t http_api_ota_route_post(httpd_req_t *req);

// ---------- PICO-8 Handlers (http_api_pico8.c) ----------

#if CONFIG_P3A_PICO8_ENABLE
/**
 * @brief GET /pico8 - Serve PICO-8 monitor page
 */
esp_err_t h_get_pico8(httpd_req_t *req);

/**
 * @brief WebSocket /pico_stream - Handle PICO-8 streaming
 */
esp_err_t h_ws_pico_stream(httpd_req_t *req);
#endif
