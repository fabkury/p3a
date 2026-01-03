// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api.c
 * @brief HTTP API core - Server infrastructure and command processing
 * 
 * This is the main entry point for the HTTP API component.
 * Contains:
 * - Command queue and worker task
 * - mDNS setup
 * - Server start/stop
 * - Method routers (GET/POST/PUT)
 * - Network diagnostics
 * 
 * REST API handlers are in: http_api_rest.c
 * Page handlers are in: http_api_page_*.c
 * Additional handlers are in:
 * - http_api_ota.c: OTA update functionality
 * - http_api_upload.c: File upload handler
 */

#include "http_api_internal.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_netif.h"
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "app_state.h"
#include "config_store.h"
#include "app_wifi.h"
#include "makapix.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "animation_player.h"
#include "app_lcd.h"
#include "version.h"
#include "makapix.h"
#include "makapix_mqtt.h"
#include "makapix_artwork.h"
#include "makapix_channel_impl.h"
#include "ota_manager.h"
#include "p3a_state.h"
#include "esp_heap_caps.h"
#if CONFIG_P3A_PICO8_ENABLE
#include "pico8_stream.h"
#endif

// ---------- Shared State ----------

// Action callback function pointers
static action_callback_t s_swap_next_callback = NULL;
static action_callback_t s_swap_back_callback = NULL;

QueueHandle_t s_cmdq = NULL;
httpd_handle_t s_server = NULL;
static TaskHandle_t s_worker = NULL;
static uint32_t s_cmd_id = 0;

// ---------- Worker Task ----------

static void do_reboot(void) {
    ESP_LOGI(HTTP_API_TAG, "Reboot command executing, delaying 250ms...");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static void api_worker_task(void *arg) {
    for(;;) {
        command_t cmd;
        if (xQueueReceive(s_cmdq, &cmd, portMAX_DELAY) == pdTRUE) {
            app_state_enter_processing();

            switch(cmd.type) {
                case CMD_REBOOT:
                    do_reboot();
                    break;

                case CMD_SWAP_NEXT:
                    if (s_swap_next_callback) {
                        s_swap_next_callback();
                        app_state_enter_ready();
                    } else {
                        app_state_enter_error();
                    }
                    break;

                case CMD_SWAP_BACK:
                    if (s_swap_back_callback) {
                        s_swap_back_callback();
                        app_state_enter_ready();
                    } else {
                        app_state_enter_error();
                    }
                    break;

                case CMD_PAUSE:
                    app_lcd_set_animation_paused(true);
                    app_state_enter_ready();
                    break;

                case CMD_RESUME:
                    app_lcd_set_animation_paused(false);
                    app_state_enter_ready();
                    break;

                default:
                    app_state_enter_error();
                    break;
            }
        }
    }
}

// ---------- Command Queue Functions ----------

static bool enqueue_cmd(command_type_t t) {
    if (!s_cmdq) {
        return false;
    }

    command_t c = { .type = t, .id = ++s_cmd_id };
    return xQueueSend(s_cmdq, &c, pdMS_TO_TICKS(10)) == pdTRUE;
}

bool api_enqueue_reboot(void) {
    return enqueue_cmd(CMD_REBOOT);
}

bool api_enqueue_swap_next(void) {
    return enqueue_cmd(CMD_SWAP_NEXT);
}

bool api_enqueue_swap_back(void) {
    return enqueue_cmd(CMD_SWAP_BACK);
}

bool api_enqueue_pause(void) {
    return enqueue_cmd(CMD_PAUSE);
}

bool api_enqueue_resume(void) {
    return enqueue_cmd(CMD_RESUME);
}

// ---------- MQTT Command Handler ----------

/**
 * @brief Handle MQTT commands
 */
static void makapix_command_handler(const char *command_type, cJSON *payload)
{
    if (strcmp(command_type, "swap_next") == 0) {
        api_enqueue_swap_next();
    } else if (strcmp(command_type, "swap_back") == 0) {
        api_enqueue_swap_back();
    } else if (strcmp(command_type, "set_background_color") == 0) {
        if (!payload || !cJSON_IsObject(payload)) {
            ESP_LOGE(HTTP_API_TAG, "Invalid set_background_color payload (expected object)");
            return;
        }

        cJSON *r = cJSON_GetObjectItem(payload, "r");
        cJSON *g = cJSON_GetObjectItem(payload, "g");
        cJSON *b = cJSON_GetObjectItem(payload, "b");

        if (!r || !g || !b || !cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b)) {
            ESP_LOGE(HTTP_API_TAG, "Invalid set_background_color payload fields");
            return;
        }

        int rv = (int)cJSON_GetNumberValue(r);
        int gv = (int)cJSON_GetNumberValue(g);
        int bv = (int)cJSON_GetNumberValue(b);
        if (rv < 0) rv = 0;
        if (rv > 255) rv = 255;
        if (gv < 0) gv = 0;
        if (gv > 255) gv = 255;
        if (bv < 0) bv = 0;
        if (bv > 255) bv = 255;

        esp_err_t err = config_store_set_background_color((uint8_t)rv, (uint8_t)gv, (uint8_t)bv);
        if (err != ESP_OK) {
            ESP_LOGE(HTTP_API_TAG, "Failed to set background color: %s", esp_err_to_name(err));
        }
    } else if (strcmp(command_type, "play_channel") == 0) {
        cJSON *channel_name = cJSON_GetObjectItem(payload, "channel_name");
        cJSON *hashtag = cJSON_GetObjectItem(payload, "hashtag");
        cJSON *user_sqid = cJSON_GetObjectItem(payload, "user_sqid");
        
        const char *channel = NULL;
        const char *identifier = NULL;
        
        if (channel_name && cJSON_IsString(channel_name)) {
            channel = cJSON_GetStringValue(channel_name);
        } else if (hashtag && cJSON_IsString(hashtag)) {
            channel = "hashtag";
            identifier = cJSON_GetStringValue(hashtag);
        } else if (user_sqid && cJSON_IsString(user_sqid)) {
            channel = "by_user";
            identifier = cJSON_GetStringValue(user_sqid);
        } else {
            return;
        }
        
        makapix_request_channel_switch(channel, identifier);
    } else if (strcmp(command_type, "show_artwork") == 0) {
        cJSON *art_url = cJSON_GetObjectItem(payload, "art_url");
        cJSON *storage_key = cJSON_GetObjectItem(payload, "storage_key");
        cJSON *post_id = cJSON_GetObjectItem(payload, "post_id");
        
        if (art_url && cJSON_IsString(art_url) && 
            storage_key && cJSON_IsString(storage_key)) {
            
            const char *url = cJSON_GetStringValue(art_url);
            const char *key = cJSON_GetStringValue(storage_key);
            int32_t pid = post_id && cJSON_IsNumber(post_id) ? (int32_t)cJSON_GetNumberValue(post_id) : 0;
            
            makapix_show_artwork(pid, key, url);
        }
    }
}

// ---------- Callback Registration ----------

void http_api_set_action_handlers(action_callback_t swap_next, action_callback_t swap_back) {
    s_swap_next_callback = swap_next;
    s_swap_back_callback = swap_back;
    makapix_mqtt_set_command_callback(makapix_command_handler);
}

// ---------- Method Routers ----------

static esp_err_t h_get_router(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bad request");
        return ESP_OK;
    }

    // Core JSON endpoints
    if (strcmp(uri, "/api/ui-config") == 0) {
        return h_get_ui_config(req);
    }
    if (strcmp(uri, "/api/network-status") == 0) {
        return h_get_network_status(req);
    }
    if (strcmp(uri, "/api/state") == 0) {
        return h_get_api_state(req);
    }
    if (strcmp(uri, "/config") == 0) {
        return h_get_config(req);
    }
    if (strcmp(uri, "/rotation") == 0) {
        return h_get_rotation(req);
    }
    if (strcmp(uri, "/settings/dwell_time") == 0) {
        return h_get_dwell_time(req);
    }
    if (strcmp(uri, "/settings/global_seed") == 0) {
        return h_get_global_seed(req);
    }
    if (strcmp(uri, "/settings/play_order") == 0) {
        return h_get_play_order(req);
    }
    if (strcmp(uri, "/channels/stats") == 0) {
        return h_get_channels_stats(req);
    }
    if (strcmp(uri, "/channel") == 0) {
        return h_get_channel(req);
    }

    // UI/pages module
    esp_err_t pr = http_api_pages_route_get(req);
    if (pr == ESP_OK) {
        return ESP_OK;
    }

    // OTA module
    esp_err_t orr = http_api_ota_route_get(req);
    if (orr == ESP_OK) {
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

static esp_err_t h_post_router(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bad request");
        return ESP_OK;
    }

    // Core action endpoints
    if (strcmp(uri, "/action/reboot") == 0) {
        return h_post_reboot(req);
    }
    if (strcmp(uri, "/action/swap_next") == 0) {
        return h_post_swap_next(req);
    }
    if (strcmp(uri, "/action/swap_back") == 0) {
        return h_post_swap_back(req);
    }
    if (strcmp(uri, "/action/pause") == 0) {
        return h_post_pause(req);
    }
    if (strcmp(uri, "/action/resume") == 0) {
        return h_post_resume(req);
    }
    if (strcmp(uri, "/rotation") == 0) {
        return h_post_rotation(req);
    }
    if (strcmp(uri, "/channel") == 0) {
        return h_post_channel(req);
    }

    // UI/pages module
    esp_err_t pr = http_api_pages_route_post(req);
    if (pr == ESP_OK) {
        return ESP_OK;
    }

    // OTA module
    esp_err_t orr = http_api_ota_route_post(req);
    if (orr == ESP_OK) {
        return ESP_OK;
    }

#if CONFIG_OTA_DEV_MODE
    if (strcmp(uri, "/debug") == 0) {
        return h_post_debug(req);
    }
#endif

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

static esp_err_t h_put_router(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bad request");
        return ESP_OK;
    }

    if (strcmp(uri, "/config") == 0) {
        return h_put_config(req);
    }
    if (strcmp(uri, "/settings/dwell_time") == 0) {
        return h_put_dwell_time(req);
    }
    if (strcmp(uri, "/settings/global_seed") == 0) {
        return h_put_global_seed(req);
    }
    if (strcmp(uri, "/settings/play_order") == 0) {
        return h_put_play_order(req);
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

// ---------- Network Diagnostics ----------

static void log_all_netifs(void) {
    // Network diagnostics disabled for cleaner boot - use http_api_log_netifs() for debug
}

static void verify_server_listening(void) {
    // Server verification disabled for cleaner boot
}

static void format_sockaddr(const struct sockaddr *sa, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    if (!sa) {
        snprintf(out, out_len, "(null)");
        return;
    }

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        char ipbuf[16] = {0};
        inet_ntoa_r(in->sin_addr, ipbuf, sizeof(ipbuf));
        snprintf(out, out_len, "%s:%u", ipbuf, (unsigned)ntohs(in->sin_port));
        return;
    }

    snprintf(out, out_len, "(af=%d)", (int)sa->sa_family);
}

static esp_err_t http_open_fn(httpd_handle_t hd, int sockfd) {
    (void)hd;
    (void)sockfd;
    return ESP_OK;
}

static void http_close_fn(httpd_handle_t hd, int sockfd) {
    (void)hd;
    (void)sockfd;
}

// ---------- Start/Stop ----------

esp_err_t http_api_start(void) {
    // Create command queue if not exists
    if (!s_cmdq) {
        s_cmdq = xQueueCreate(QUEUE_LEN, sizeof(command_t));
        if (!s_cmdq) {
            ESP_LOGE(HTTP_API_TAG, "Failed to create command queue");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create worker task if not exists
    if (!s_worker) {
        BaseType_t ret = xTaskCreate(api_worker_task, "api_worker", 8192, NULL, 5, &s_worker);
        if (ret != pdPASS) {
            ESP_LOGE(HTTP_API_TAG, "Failed to create worker task");
            return ESP_ERR_NO_MEM;
        }
    }

#if CONFIG_P3A_PICO8_ENABLE
    esp_err_t stream_init_ret = pico8_stream_init();
    if (stream_init_ret != ESP_OK) {
        ESP_LOGW(HTTP_API_TAG, "PICO-8 stream init failed: %s (continuing anyway)", esp_err_to_name(stream_init_ret));
    }
#endif

    // Start HTTP server
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 6;
    cfg.max_uri_handlers = 12;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.open_fn = http_open_fn;
    cfg.close_fn = http_close_fn;

    esp_err_t httpd_err = httpd_start(&s_server, &cfg);
    if (httpd_err != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "Failed to start HTTP server: %s", esp_err_to_name(httpd_err));
        ESP_LOGE(HTTP_API_TAG, "Heap (default): free=%u, min_free=%u", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
        ESP_LOGE(HTTP_API_TAG, "Heap (internal): free=%u, largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        ESP_LOGE(HTTP_API_TAG, "Heap (spiram): free=%u, largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return httpd_err;
    }
    ESP_LOGI(HTTP_API_TAG, "HTTP server started on port 80");

    // Register dedicated handlers first
    httpd_uri_t u = {0};

    u.uri = "/status";
    u.method = HTTP_GET;
    u.handler = h_get_status;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    // Register handlers from other modules
    http_api_register_page_handlers(s_server);
    http_api_register_ota_handlers(s_server);
    http_api_register_upload_handler(s_server);

    // Register method routers last (catch-all)
    u.uri = "/*";
    u.method = HTTP_GET;
    u.handler = h_get_router;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/*";
    u.method = HTTP_POST;
    u.handler = h_post_router;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/*";
    u.method = HTTP_PUT;
    u.handler = h_put_router;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    return ESP_OK;
}

esp_err_t http_api_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(HTTP_API_TAG, "HTTP API server stopped");
    }
    return ESP_OK;
}
