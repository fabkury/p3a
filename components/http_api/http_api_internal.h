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

// ---------- HTTP Helper Functions ----------

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

// ---------- Command Queue Functions ----------

bool api_enqueue_pause(void);
bool api_enqueue_resume(void);

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

// ---------- Sub-router entrypoints (used by method routers in http_api.c) ----------

/**
 * @brief Route GET requests for page/UI endpoints (/, /favicon.ico, /config/network, /seed, /pico8).
 *        Note: static file wildcard and WebSocket endpoints are registered separately.
 * @return ESP_OK if handled, ESP_ERR_NOT_FOUND if URI not owned by this module.
 */
esp_err_t http_api_pages_route_get(httpd_req_t *req);

/**
 * @brief Route POST requests for page/UI endpoints.
 *        Currently: /erase
 * @return ESP_OK if handled, ESP_ERR_NOT_FOUND if URI not owned by this module.
 */
esp_err_t http_api_pages_route_post(httpd_req_t *req);

/**
 * @brief Route GET requests for OTA endpoints (/ota, /ota/status).
 * @return ESP_OK if handled, ESP_ERR_NOT_FOUND if URI not owned by this module.
 */
esp_err_t http_api_ota_route_get(httpd_req_t *req);

/**
 * @brief Route POST requests for OTA endpoints (/ota/check, /ota/install, /ota/rollback).
 * @return ESP_OK if handled, ESP_ERR_NOT_FOUND if URI not owned by this module.
 */
esp_err_t http_api_ota_route_post(httpd_req_t *req);

