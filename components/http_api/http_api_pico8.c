// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_pico8.c
 * @brief HTTP API PICO-8 handlers
 * 
 * Contains handlers for PICO-8 monitor page and WebSocket streaming
 */

#include "http_api_internal.h"

#if CONFIG_P3A_PICO8_ENABLE
#include "pico8_stream.h"
#include <sys/stat.h>

static bool s_ws_client_connected = false;

/**
 * GET /pico8
 * Serves the PICO-8 monitor page from SPIFFS
 */
esp_err_t h_get_pico8(httpd_req_t *req) {
    const char* filepath = "/spiffs/pico8/index.html";
    
    FILE* f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(HTTP_API_TAG, "Failed to open %s", filepath);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "PICO-8 page not found", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 1024 * 1024) { // Max 1MB
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Invalid file size", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    
    char* buf = malloc(size);
    if (!buf) {
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Out of memory", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    
    size_t read = fread(buf, 1, size, f);
    fclose(f);
    
    if (read != (size_t)size) {
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Read error", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    
    // Enter PICO-8 mode when page is visited
    pico8_stream_enter_mode();
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, size);
    free(buf);
    
    return ESP_OK;
}

/**
 * WebSocket handler for /pico_stream
 */
esp_err_t h_ws_pico_stream(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(HTTP_API_TAG, "WebSocket connection request");
        pico8_stream_enter_mode();
        s_ws_client_connected = true;
        return ESP_OK;
    }

    uint8_t stack_buf[WS_MAX_FRAME_SIZE] = {0};

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = NULL,
        .len = 0
    };

    // Step 1: read frame metadata
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(HTTP_API_TAG, "Failed to read WebSocket header: %s", esp_err_to_name(ret));
            if (s_ws_client_connected) {
                pico8_stream_exit_mode();
                s_ws_client_connected = false;
            }
        }
        return ret;
    }

    size_t payload_len = frame.len;
    uint8_t *payload_buf = NULL;
    bool payload_allocated = false;

    if (payload_len > 0) {
        if (payload_len <= sizeof(stack_buf)) {
            payload_buf = stack_buf;
        } else if (payload_len <= WS_MAX_FRAME_SIZE) {
            payload_buf = (uint8_t *)malloc(payload_len);
            if (!payload_buf) {
                ESP_LOGE(HTTP_API_TAG, "Unable to allocate %zu bytes for WS payload", payload_len);
                return ESP_ERR_NO_MEM;
            }
            payload_allocated = true;
        } else {
            ESP_LOGW(HTTP_API_TAG, "WebSocket frame too large (%zu bytes)", payload_len);
            return ESP_ERR_INVALID_SIZE;
        }

        frame.payload = payload_buf;
        ret = httpd_ws_recv_frame(req, &frame, payload_len);
        if (ret != ESP_OK) {
            ESP_LOGE(HTTP_API_TAG, "Failed to read WebSocket payload: %s", esp_err_to_name(ret));
            if (payload_allocated) {
                free(payload_buf);
            }
            if (s_ws_client_connected) {
                pico8_stream_exit_mode();
                s_ws_client_connected = false;
            }
            return ret;
        }
    } else {
        frame.payload = NULL;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(HTTP_API_TAG, "WebSocket close frame");
        s_ws_client_connected = false;
        pico8_stream_exit_mode();
        if (payload_allocated) {
            free(payload_buf);
        }
        return ESP_OK;
    }

    if (frame.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong = {
            .type = HTTPD_WS_TYPE_PONG,
            .payload = frame.payload,
            .len = frame.len
        };
        httpd_ws_send_frame(req, &pong);
        if (payload_allocated) {
            free(payload_buf);
        }
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_BINARY) {
        ESP_LOGW(HTTP_API_TAG, "Ignoring non-binary WebSocket frame (type=%d, len=%zu)", frame.type, frame.len);
        if (payload_allocated) {
            free(payload_buf);
        }
        return ESP_OK;
    }

    if (!frame.payload || frame.len < 6) {
        if (payload_allocated) {
            free(payload_buf);
        }
        return ESP_OK;
    }

    if (frame.payload[0] != 0x70 || frame.payload[1] != 0x38 || frame.payload[2] != 0x46) {
        if (payload_allocated) {
            free(payload_buf);
        }
        return ESP_OK;
    }

    s_ws_client_connected = true;

    esp_err_t feed_ret = pico8_stream_feed_packet(frame.payload, frame.len);
    if (feed_ret != ESP_OK) {
        ESP_LOGW(HTTP_API_TAG, "pico8_stream_feed_packet failed: %s (len=%zu)",
                 esp_err_to_name(feed_ret), frame.len);
    }

    if (payload_allocated) {
        free(payload_buf);
    }

    return ESP_OK;
}

#endif // CONFIG_P3A_PICO8_ENABLE

