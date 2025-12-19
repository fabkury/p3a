/**
 * @file http_api.c
 * @brief HTTP API core - Server infrastructure and REST API handlers
 * 
 * This is the main entry point for the HTTP API component.
 * Contains:
 * - Command queue and worker task
 * - HTTP helper functions
 * - mDNS setup
 * - Server start/stop
 * - REST API handlers (/status, /config, /action/..., /rotation)
 * 
 * Additional handlers are in separate files:
 * - http_api_pages.c: HTML pages and static file serving
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
#include "channel_player.h"
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
    ESP_LOGI(HTTP_API_TAG, "Worker task started");
    for(;;) {
        command_t cmd;
        if (xQueueReceive(s_cmdq, &cmd, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(HTTP_API_TAG, "Processing command %lu (type=%d)", cmd.id, cmd.type);
            app_state_enter_processing();

            switch(cmd.type) {
                case CMD_REBOOT:
                    do_reboot();
                    // No return - device restarts
                    break;

                case CMD_SWAP_NEXT:
                    if (s_swap_next_callback) {
                        ESP_LOGI(HTTP_API_TAG, "Executing swap_next");
                        s_swap_next_callback();
                        app_state_enter_ready();
                    } else {
                        ESP_LOGW(HTTP_API_TAG, "swap_next callback not set");
                        app_state_enter_error();
                    }
                    break;

                case CMD_SWAP_BACK:
                    if (s_swap_back_callback) {
                        ESP_LOGI(HTTP_API_TAG, "Executing swap_back");
                        s_swap_back_callback();
                        app_state_enter_ready();
                    } else {
                        ESP_LOGW(HTTP_API_TAG, "swap_back callback not set");
                        app_state_enter_error();
                    }
                    break;

                case CMD_PAUSE:
                    ESP_LOGI(HTTP_API_TAG, "Executing pause");
                    app_lcd_set_animation_paused(true);
                    app_state_enter_ready();
                    break;

                case CMD_RESUME:
                    ESP_LOGI(HTTP_API_TAG, "Executing resume");
                    app_lcd_set_animation_paused(false);
                    app_state_enter_ready();
                    break;

                default:
                    ESP_LOGE(HTTP_API_TAG, "Unknown command type: %d", cmd.type);
                    app_state_enter_error();
                    break;
            }
        }
    }
}

#if CONFIG_OTA_DEV_MODE
#include "swap_future.h"
#include "playlist_manager.h"
#include <sys/time.h>
// External: wakes the auto_swap_task in main/p3a_main.c.
extern void auto_swap_reset_timer(void);

static uint64_t wall_clock_ms_http(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

/**
 * POST /debug  (CONFIG_OTA_DEV_MODE only)
 *
 * JSON request:
 * {
 *   "op": "swap_future_test" | "swap_future_cancel" | "live_mode_enter" | "live_mode_exit",
 *   "data": { ... }
 * }
 */
static esp_err_t h_post_debug(httpd_req_t *req)
{
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *op = cJSON_GetObjectItem(root, "op");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    const char *op_s = (op && cJSON_IsString(op)) ? cJSON_GetStringValue(op) : NULL;

    if (!op_s || !*op_s) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'op'\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "op", op_s);

    if (strcmp(op_s, "swap_future_cancel") == 0) {
        swap_future_cancel();
        auto_swap_reset_timer();
        cJSON_AddStringToObject(resp, "result", "cancelled");
    } else if (strcmp(op_s, "live_mode_enter") == 0 || strcmp(op_s, "live_mode_exit") == 0) {
        void *nav = channel_player_get_navigator();
        if (!nav) {
            cJSON_Delete(root);
            cJSON_Delete(resp);
            send_json(req, 409, "{\"ok\":false,\"error\":\"No navigator\",\"code\":\"NO_NAV\"}");
            return ESP_OK;
        }
        esp_err_t e = (strcmp(op_s, "live_mode_enter") == 0) ? live_mode_enter(nav) : (live_mode_exit(nav), ESP_OK);
        cJSON_AddNumberToObject(resp, "esp_err", (double)e);
    } else if (strcmp(op_s, "swap_future_test") == 0) {
        // Build swap_future targeting the current file.
        const sdcard_post_t *post = NULL;
        if (channel_player_get_current_post(&post) != ESP_OK || !post || !post->filepath) {
            cJSON_Delete(root);
            cJSON_Delete(resp);
            send_json(req, 409, "{\"ok\":false,\"error\":\"No current post\",\"code\":\"NO_CURRENT\"}");
            return ESP_OK;
        }

        uint32_t delay_ms = 1000;
        uint32_t start_offset_ms = 0;
        uint32_t start_frame = 0;

        if (data && cJSON_IsObject(data)) {
            cJSON *d = cJSON_GetObjectItem(data, "delay_ms");
            if (d && cJSON_IsNumber(d) && cJSON_GetNumberValue(d) >= 0) {
                delay_ms = (uint32_t)cJSON_GetNumberValue(d);
            }
            cJSON *o = cJSON_GetObjectItem(data, "start_offset_ms");
            if (o && cJSON_IsNumber(o) && cJSON_GetNumberValue(o) >= 0) {
                start_offset_ms = (uint32_t)cJSON_GetNumberValue(o);
            }
            cJSON *sf = cJSON_GetObjectItem(data, "start_frame");
            if (sf && cJSON_IsNumber(sf) && cJSON_GetNumberValue(sf) >= 0) {
                start_frame = (uint32_t)cJSON_GetNumberValue(sf);
            }
        }

        uint64_t now_ms = wall_clock_ms_http();
        uint64_t target_ms = now_ms + (uint64_t)delay_ms;
        uint64_t start_ms = (start_offset_ms <= delay_ms) ? (target_ms - (uint64_t)start_offset_ms) : target_ms;

        artwork_ref_t art = {0};
        strlcpy(art.filepath, post->filepath, sizeof(art.filepath));
        art.type = post->type;
        art.dwell_time_ms = post->dwell_time_ms;
        art.downloaded = true;

        swap_future_t sf = {0};
        sf.valid = true;
        sf.target_time_ms = target_ms;
        sf.start_time_ms = start_ms;
        sf.start_frame = start_frame;
        sf.artwork = art;
        sf.is_live_mode_swap = false;
        sf.is_automated = true;

        swap_future_cancel();
        esp_err_t e = swap_future_schedule(&sf);
        auto_swap_reset_timer();

        cJSON_AddNumberToObject(resp, "esp_err", (double)e);
        cJSON_AddNumberToObject(resp, "now_ms", (double)now_ms);
        cJSON_AddNumberToObject(resp, "target_time_ms", (double)target_ms);
        cJSON_AddNumberToObject(resp, "start_time_ms", (double)start_ms);
        cJSON_AddNumberToObject(resp, "start_frame", (double)start_frame);
        cJSON_AddStringToObject(resp, "filepath", post->filepath);
    } else {
        cJSON_Delete(root);
        cJSON_Delete(resp);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Unknown op\",\"code\":\"UNKNOWN_OP\"}");
        return ESP_OK;
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(root);
    cJSON_Delete(resp);
    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }
    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}
#endif // CONFIG_OTA_DEV_MODE

static bool enqueue_cmd(command_type_t t) {
    if (!s_cmdq) {
        ESP_LOGE(HTTP_API_TAG, "Command queue not initialized");
        return false;
    }

    command_t c = { .type = t, .id = ++s_cmd_id };
    BaseType_t result = xQueueSend(s_cmdq, &c, pdMS_TO_TICKS(10));
    if (result != pdTRUE) {
        ESP_LOGW(HTTP_API_TAG, "Failed to enqueue command (queue full)");
        return false;
    }
    ESP_LOGI(HTTP_API_TAG, "Command %lu enqueued", c.id);
    return true;
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
 * 
 * Called by makapix_mqtt when a command is received via MQTT.
 */
static void makapix_command_handler(const char *command_type, cJSON *payload)
{
    ESP_LOGI(HTTP_API_TAG, "MQTT command received: %s", command_type);
    
    if (strcmp(command_type, "swap_next") == 0) {
        api_enqueue_swap_next();
    } else if (strcmp(command_type, "swap_back") == 0) {
        api_enqueue_swap_back();
    } else if (strcmp(command_type, "set_background_color") == 0) {
        // Payload: {"r":0,"g":0,"b":0}
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
        // Extract channel information from payload (new format)
        // Payload contains exactly ONE of: channel_name, hashtag, or user_sqid
        cJSON *channel_name = cJSON_GetObjectItem(payload, "channel_name");
        cJSON *hashtag = cJSON_GetObjectItem(payload, "hashtag");
        cJSON *user_sqid = cJSON_GetObjectItem(payload, "user_sqid");
        
        const char *channel = NULL;
        const char *identifier = NULL;
        
        if (channel_name && cJSON_IsString(channel_name)) {
            // System channel: "all" or "promoted"
            channel = cJSON_GetStringValue(channel_name);
            ESP_LOGI(HTTP_API_TAG, "Requesting channel switch to: %s", channel);
        } else if (hashtag && cJSON_IsString(hashtag)) {
            // Hashtag channel
            channel = "hashtag";
            identifier = cJSON_GetStringValue(hashtag);
            ESP_LOGI(HTTP_API_TAG, "Requesting channel switch to hashtag: #%s", identifier);
        } else if (user_sqid && cJSON_IsString(user_sqid)) {
            // User profile channel
            channel = "by_user";
            identifier = cJSON_GetStringValue(user_sqid);
            ESP_LOGI(HTTP_API_TAG, "Requesting channel switch to user: %s", identifier);
        } else {
            ESP_LOGE(HTTP_API_TAG, "Invalid play_channel payload: missing channel_name, hashtag, or user_sqid");
            return;
        }
        
        makapix_request_channel_switch(channel, identifier);
    } else if (strcmp(command_type, "show_artwork") == 0) {
        // Extract artwork information and show it directly
        // This creates a single-artwork channel and switches to it
        cJSON *art_url = cJSON_GetObjectItem(payload, "art_url");
        cJSON *storage_key = cJSON_GetObjectItem(payload, "storage_key");
        cJSON *post_id = cJSON_GetObjectItem(payload, "post_id");
        
        if (art_url && cJSON_IsString(art_url) && 
            storage_key && cJSON_IsString(storage_key)) {
            
            const char *url = cJSON_GetStringValue(art_url);
            const char *key = cJSON_GetStringValue(storage_key);
            int32_t pid = post_id && cJSON_IsNumber(post_id) ? (int32_t)cJSON_GetNumberValue(post_id) : 0;
            
            ESP_LOGI(HTTP_API_TAG, "Showing artwork: %s", key);
            esp_err_t err = makapix_show_artwork(pid, key, url);
            if (err != ESP_OK) {
                ESP_LOGE(HTTP_API_TAG, "Failed to show artwork: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(HTTP_API_TAG, "Invalid show_artwork payload");
        }
    } else {
        ESP_LOGW(HTTP_API_TAG, "Unknown command type: %s", command_type);
    }
}

// ---------- Callback Registration ----------

void http_api_set_action_handlers(action_callback_t swap_next, action_callback_t swap_back) {
    s_swap_next_callback = swap_next;
    s_swap_back_callback = swap_back;
    ESP_LOGI(HTTP_API_TAG, "Action handlers registered");
    
    // Register MQTT command callback (only if makapix is available)
    makapix_mqtt_set_command_callback(makapix_command_handler);
}

// ---------- HTTP Helper Functions ----------

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

// ---------- Method routers (reduce URI handler usage) ----------

// Forward declarations for handlers referenced by routers (defined later in this file)
static esp_err_t h_get_api_state(httpd_req_t *req);
static esp_err_t h_get_config(httpd_req_t *req);
static esp_err_t h_put_config(httpd_req_t *req);
static esp_err_t h_post_channel(httpd_req_t *req);
static esp_err_t h_get_dwell_time(httpd_req_t *req);
static esp_err_t h_put_dwell_time(httpd_req_t *req);
static esp_err_t h_get_global_seed(httpd_req_t *req);
static esp_err_t h_put_global_seed(httpd_req_t *req);
static esp_err_t h_post_reboot(httpd_req_t *req);
static esp_err_t h_post_swap_next(httpd_req_t *req);
static esp_err_t h_post_swap_back(httpd_req_t *req);
static esp_err_t h_post_pause(httpd_req_t *req);
static esp_err_t h_post_resume(httpd_req_t *req);
static esp_err_t h_get_rotation(httpd_req_t *req);
static esp_err_t h_post_rotation(httpd_req_t *req);
static esp_err_t h_get_channels_stats(httpd_req_t *req);
#if CONFIG_OTA_DEV_MODE
static esp_err_t h_post_debug(httpd_req_t *req);
#endif

static esp_err_t h_get_router(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bad request");
        return ESP_OK;
    }

    // Core JSON endpoints (hot paths kept here to avoid extra module hops)
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
    if (strcmp(uri, "/channels/stats") == 0) {
        return h_get_channels_stats(req);
    }

    // UI/pages module (/, /favicon.ico, /config/network, /seed, /pico8)
    esp_err_t pr = http_api_pages_route_get(req);
    if (pr == ESP_OK) {
        return ESP_OK;
    }

    // OTA module (/ota, /ota/status)
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

    // UI/pages module (POST /erase)
    esp_err_t pr = http_api_pages_route_post(req);
    if (pr == ESP_OK) {
        return ESP_OK;
    }

    // OTA module (POST /ota/check|install|rollback)
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

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

// ---------- REST API Handlers ----------

/**
 * GET /status
 * Returns device status including state, uptime, heap, RSSI, firmware info, and queue depth
 */
static esp_err_t h_get_status(httpd_req_t *req) {
    wifi_ap_record_t ap = {0};
    int rssi_ok = (esp_wifi_remote_sta_get_ap_info(&ap) == ESP_OK);

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddStringToObject(data, "state", app_state_str(app_state_get()));
    cJSON_AddNumberToObject(data, "uptime_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(data, "heap_free", (double)esp_get_free_heap_size());
    
    if (rssi_ok) {
        cJSON_AddNumberToObject(data, "rssi", ap.rssi);
    } else {
        cJSON_AddNullToObject(data, "rssi");
    }

    cJSON *fw = cJSON_CreateObject();
    if (fw) {
        cJSON_AddStringToObject(fw, "version", FW_VERSION);
        cJSON_AddStringToObject(fw, "idf", IDF_VER);
        cJSON_AddItemToObject(data, "fw", fw);
    }

    uint32_t queue_depth = s_cmdq ? uxQueueMessagesWaiting(s_cmdq) : 0;
    cJSON_AddNumberToObject(data, "queue_depth", (double)queue_depth);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(data);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

/**
 * GET /api/state
 * Lightweight state snapshot for UI/automation.
 */
static esp_err_t h_get_api_state(httpd_req_t *req)
{
    wifi_ap_record_t ap = {0};
    int rssi_ok = (esp_wifi_remote_sta_get_ap_info(&ap) == ESP_OK);

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddStringToObject(data, "state", app_state_str(app_state_get()));
    cJSON_AddNumberToObject(data, "uptime_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(data, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddBoolToObject(data, "live_mode", channel_player_is_live_mode_active());

    if (rssi_ok) {
        cJSON_AddNumberToObject(data, "rssi", ap.rssi);
    } else {
        cJSON_AddNullToObject(data, "rssi");
    }

    // Current Makapix post_id if available; NULL for SD card or unknown.
    int32_t post_id = makapix_get_current_post_id();
    if (post_id > 0) {
        cJSON_AddNumberToObject(data, "current_post_id", (double)post_id);
    } else {
        cJSON_AddNullToObject(data, "current_post_id");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(data);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

/**
 * GET /config
 * Returns current configuration as JSON object
 */
static esp_err_t h_get_config(httpd_req_t *req) {
    char *json;
    size_t len;
    esp_err_t err = config_store_get_serialized(&json, &len);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"CONFIG_READ_FAIL\",\"code\":\"CONFIG_READ_FAIL\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(json);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *data = cJSON_ParseWithLength(json, len);
    if (!data) {
        data = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(root, "data", data);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(json);

    if (!out) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

/**
 * PUT /config
 * Accepts JSON config object (max 32 KB), validates, and saves to NVS
 */
static esp_err_t h_put_config(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        if (err_status == 413) {
            send_json(req, 413, "{\"ok\":false,\"error\":\"Payload too large\",\"code\":\"PAYLOAD_TOO_LARGE\"}");
        } else {
            send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        }
        return ESP_OK;
    }

    cJSON *o = cJSON_ParseWithLength(body, len);
    free(body);

    if (!o || !cJSON_IsObject(o)) {
        if (o) cJSON_Delete(o);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    esp_err_t e = config_store_save(o);
    cJSON_Delete(o);

    if (e != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"CONFIG_SAVE_FAIL\",\"code\":\"CONFIG_SAVE_FAIL\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/**
 * POST /channel
 * Switch to a channel
 * Body: {"channel": "all"|"promoted"|"user"|"by_user"|"sdcard", "user_handle": "..." (optional)}
 */
static esp_err_t h_post_channel(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        if (err_status == 413) {
            send_json(req, 413, "{\"ok\":false,\"error\":\"Payload too large\",\"code\":\"PAYLOAD_TOO_LARGE\"}");
        } else {
            send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        }
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);

    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    // Parse new format: exactly ONE of channel_name, hashtag, or user_sqid
    cJSON *channel_name = cJSON_GetObjectItem(root, "channel_name");
    cJSON *hashtag = cJSON_GetObjectItem(root, "hashtag");
    cJSON *user_sqid = cJSON_GetObjectItem(root, "user_sqid");

    const char *ch_name = NULL;
    const char *identifier = NULL;

    if (channel_name && cJSON_IsString(channel_name)) {
        // System channel: "all", "promoted", or "sdcard"
        ch_name = cJSON_GetStringValue(channel_name);
    } else if (hashtag && cJSON_IsString(hashtag)) {
        // Hashtag channel
        ch_name = "hashtag";
        identifier = cJSON_GetStringValue(hashtag);
    } else if (user_sqid && cJSON_IsString(user_sqid)) {
        // User profile channel
        ch_name = "by_user";
        identifier = cJSON_GetStringValue(user_sqid);
    } else {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing channel_name, hashtag, or user_sqid\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    esp_err_t err;
    
    // Handle sdcard channel separately (this is fast, can be done synchronously)
    if (strcmp(ch_name, "sdcard") == 0) {
        // Abort any ongoing Makapix channel loading
        if (makapix_is_channel_loading(NULL, 0)) {
            ESP_LOGI(HTTP_API_TAG, "Aborting Makapix channel load for SD card switch");
            makapix_abort_channel_load();
        }
        
        err = p3a_state_switch_channel(P3A_CHANNEL_SDCARD, NULL);
        if (err == ESP_OK) {
            // Switch animation player back to sdcard_channel source
            channel_player_switch_to_sdcard_channel();
            // Reload channel to pick up any new files
            channel_player_load_channel();
            // Trigger animation swap to show an item from sdcard
            animation_player_request_swap_current();
        }
        cJSON_Delete(root);
        if (err != ESP_OK) {
            send_json(req, 500, "{\"ok\":false,\"error\":\"Channel switch failed\",\"code\":\"CHANNEL_SWITCH_FAILED\"}");
            return ESP_OK;
        }
        send_json(req, 200, "{\"ok\":true}");
        return ESP_OK;
    }
    
    // Handle Makapix channels asynchronously via the channel switch task
    // This returns immediately - the actual switch happens in background
    err = makapix_request_channel_switch(ch_name, identifier);
    
    cJSON_Delete(root);

    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Channel switch request failed\",\"code\":\"CHANNEL_SWITCH_FAILED\"}");
        return ESP_OK;
    }

    // Request accepted - switch will happen in background
    send_json(req, 202, "{\"ok\":true,\"message\":\"Channel switch initiated\"}");
    return ESP_OK;
}

/**
 * GET /settings/dwell_time
 * Get current dwell time setting
 */
static esp_err_t h_get_dwell_time(httpd_req_t *req) {
    uint32_t dwell_time = animation_player_get_dwell_time();
    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":true,\"data\":{\"dwell_time\":%lu}}", (unsigned long)dwell_time);
    send_json(req, 200, response);
    return ESP_OK;
}

/**
 * GET /channels/stats
 * Get cached artwork counts for each Makapix channel
 */
static esp_err_t h_get_channels_stats(httpd_req_t *req) {
    // Cache + serialize stats computation: counting involves SD card IO (stat per entry).
    // With multiple browser tabs, doing this concurrently can lead to timeouts/failed requests.
    static SemaphoreHandle_t s_stats_mu = NULL;
    static int64_t s_last_us = 0;
    static size_t s_all_total = 0, s_all_cached = 0;
    static size_t s_promoted_total = 0, s_promoted_cached = 0;

    if (!s_stats_mu) {
        s_stats_mu = xSemaphoreCreateMutex();
        // If mutex creation fails, fall back to direct computation (best-effort).
    }

    const int64_t now_us = esp_timer_get_time();
    const bool should_refresh = (s_last_us == 0) || ((now_us - s_last_us) > 2 * 1000 * 1000);

    size_t all_total = s_all_total, all_cached = s_all_cached;
    size_t promoted_total = s_promoted_total, promoted_cached = s_promoted_cached;

    bool have_lock = false;
    if (s_stats_mu) {
        have_lock = (xSemaphoreTake(s_stats_mu, pdMS_TO_TICKS(250)) == pdTRUE);
    }

    if (have_lock || !s_stats_mu) {
        if (should_refresh) {
            // Count cached artworks for each channel
            makapix_channel_count_cached("all", "/sdcard/channel", "/sdcard/vault", &all_total, &all_cached);
            makapix_channel_count_cached("promoted", "/sdcard/channel", "/sdcard/vault", &promoted_total, &promoted_cached);

            s_all_total = all_total;
            s_all_cached = all_cached;
            s_promoted_total = promoted_total;
            s_promoted_cached = promoted_cached;
            s_last_us = now_us;
        }
        if (have_lock) xSemaphoreGive(s_stats_mu);
    } else {
        // Couldn't lock quickly (another request computing). Return last cached values.
        all_total = s_all_total;
        all_cached = s_all_cached;
        promoted_total = s_promoted_total;
        promoted_cached = s_promoted_cached;
    }

    char response[256];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"data\":{"
             "\"all\":{\"total\":%zu,\"cached\":%zu},"
             "\"promoted\":{\"total\":%zu,\"cached\":%zu}"
             "}}",
             all_total, all_cached, promoted_total, promoted_cached);
    send_json(req, 200, response);
    return ESP_OK;
}

/**
 * PUT /settings/dwell_time
 * Set dwell time (1-100000 seconds)
 * Body: {"dwell_time": 30}
 */
static esp_err_t h_put_dwell_time(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        if (err_status == 413) {
            send_json(req, 413, "{\"ok\":false,\"error\":\"Payload too large\",\"code\":\"PAYLOAD_TOO_LARGE\"}");
        } else {
            send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        }
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);

    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *dwell_item = cJSON_GetObjectItem(root, "dwell_time");
    if (!dwell_item || !cJSON_IsNumber(dwell_item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'dwell_time' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    uint32_t dwell_time = (uint32_t)cJSON_GetNumberValue(dwell_item);
    cJSON_Delete(root);

    // Validate range: 0-100000 seconds (0 disables global override)
    if (dwell_time > 100000) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid dwell_time (must be 0-100000 seconds)\",\"code\":\"INVALID_DWELL_TIME\"}");
        return ESP_OK;
    }

    esp_err_t err = animation_player_set_dwell_time(dwell_time);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to set dwell_time\",\"code\":\"SET_DWELL_TIME_FAILED\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/**
 * GET /settings/global_seed
 */
static esp_err_t h_get_global_seed(httpd_req_t *req)
{
    uint32_t seed = config_store_get_global_seed();
    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":true,\"data\":{\"global_seed\":%lu}}", (unsigned long)seed);
    send_json(req, 200, response);
    return ESP_OK;
}

/**
 * PUT /settings/global_seed
 * Body: {"global_seed": 4011}
 */
static esp_err_t h_put_global_seed(httpd_req_t *req)
{
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        if (err_status == 413) {
            send_json(req, 413, "{\"ok\":false,\"error\":\"Payload too large\",\"code\":\"PAYLOAD_TOO_LARGE\"}");
        } else {
            send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"error\":\"READ_BODY\",\"code\":\"READ_BODY\"}");
        }
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"INVALID_JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *seed_item = cJSON_GetObjectItem(root, "global_seed");
    if (!seed_item || !cJSON_IsNumber(seed_item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'global_seed' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    uint32_t seed = (uint32_t)cJSON_GetNumberValue(seed_item);
    cJSON_Delete(root);

    esp_err_t err = config_store_set_global_seed(seed);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to set global_seed\",\"code\":\"SET_GLOBAL_SEED_FAILED\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/**
 * POST /action/reboot
 * Enqueues reboot command, returns 202 Accepted
 */
static esp_err_t h_post_reboot(httpd_req_t *req) {
    // Allow empty body, but if provided and not JSON, enforce 415
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    if (!api_enqueue_reboot()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"reboot\"}}");
    return ESP_OK;
}

/**
 * POST /action/swap_next
 * Enqueues swap_next command, returns 202 Accepted
 * Returns 409 Conflict if state is ERROR
 */
static esp_err_t h_post_swap_next(httpd_req_t *req) {
    if (app_state_get() == STATE_ERROR) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Bad state\",\"code\":\"BAD_STATE\"}");
        return ESP_OK;
    }

    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    if (!api_enqueue_swap_next()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"swap_next\"}}");
    return ESP_OK;
}

/**
 * POST /action/swap_back
 * Enqueues swap_back command, returns 202 Accepted
 * Returns 409 Conflict if state is ERROR
 */
static esp_err_t h_post_swap_back(httpd_req_t *req) {
    if (app_state_get() == STATE_ERROR) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Bad state\",\"code\":\"BAD_STATE\"}");
        return ESP_OK;
    }

    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    if (!api_enqueue_swap_back()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"swap_back\"}}");
    return ESP_OK;
}

/**
 * POST /action/pause
 * Enqueues pause command, returns 202 Accepted
 */
static esp_err_t h_post_pause(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    if (!api_enqueue_pause()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"pause\"}}");
    return ESP_OK;
}

/**
 * POST /action/resume
 * Enqueues resume command, returns 202 Accepted
 */
static esp_err_t h_post_resume(httpd_req_t *req) {
    if (req->content_len > 0 && !ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    if (!api_enqueue_resume()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"Queue full\",\"code\":\"QUEUE_FULL\"}");
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"queued\":true,\"action\":\"resume\"}}");
    return ESP_OK;
}

/**
 * GET /rotation
 * Returns current screen rotation angle
 */
static esp_err_t h_get_rotation(httpd_req_t *req) {
    screen_rotation_t rotation = app_get_screen_rotation();
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }
    
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "rotation", (double)rotation);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }
    
    send_json(req, 200, json_str);
    free(json_str);
    return ESP_OK;
}

/**
 * POST /rotation
 * Sets screen rotation angle
 * Body: {"rotation": 90}
 */
static esp_err_t h_post_rotation(httpd_req_t *req) {
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }
    
    // Read request body
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }
    
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        free(buf);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    
    if (!root) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid JSON\",\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }
    
    cJSON *rotation_item = cJSON_GetObjectItem(root, "rotation");
    if (!rotation_item || !cJSON_IsNumber(rotation_item)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'rotation' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }
    
    int rotation_value = (int)cJSON_GetNumberValue(rotation_item);
    cJSON_Delete(root);
    
    // Validate rotation value
    if (rotation_value != 0 && rotation_value != 90 && rotation_value != 180 && rotation_value != 270) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid rotation angle (must be 0, 90, 180, or 270)\",\"code\":\"INVALID_ROTATION\"}");
        return ESP_OK;
    }
    
    // Apply rotation
    esp_err_t err = app_set_screen_rotation((screen_rotation_t)rotation_value);
    if (err == ESP_ERR_INVALID_STATE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Rotation operation already in progress\",\"code\":\"ROTATION_IN_PROGRESS\"}");
        return ESP_OK;
    } else if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to set rotation\",\"code\":\"ROTATION_FAILED\"}");
        return ESP_OK;
    }
    
    send_json(req, 200, "{\"ok\":true,\"data\":{\"rotation\":null}}");
    return ESP_OK;
}

// ---------- Network Diagnostics ----------

static void log_all_netifs(void) {
    ESP_LOGI(HTTP_API_TAG, "=== Network Interface Diagnostics ===");
    
    // Iterate all network interfaces
    esp_netif_t *netif = NULL;
    int count = 0;
    while ((netif = esp_netif_next(netif)) != NULL) {
        count++;
        const char *desc = esp_netif_get_desc(netif);
        const char *key = esp_netif_get_ifkey(netif);
        bool is_up = esp_netif_is_netif_up(netif);
        
        esp_netif_ip_info_t ip_info;
        esp_err_t err = esp_netif_get_ip_info(netif, &ip_info);
        
        if (err == ESP_OK && ip_info.ip.addr != 0) {
            ESP_LOGI(HTTP_API_TAG, "  [%d] %s (%s): UP=%d IP=" IPSTR " GW=" IPSTR,
                     count, desc ? desc : "?", key ? key : "?", is_up,
                     IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));
        } else {
            ESP_LOGI(HTTP_API_TAG, "  [%d] %s (%s): UP=%d (no IP)", 
                     count, desc ? desc : "?", key ? key : "?", is_up);
        }
    }
    if (count == 0) {
        ESP_LOGW(HTTP_API_TAG, "  No network interfaces found!");
    }
    ESP_LOGI(HTTP_API_TAG, "=== End Network Diagnostics ===");
}

static void verify_server_listening(void) {
    // Try to connect to our own HTTP server to verify it's actually listening.
    // Test both loopback and the device's own STA IP. If "own IP" fails locally, the server
    // may be bound to loopback-only, which would explain why LAN clients can't connect.

    const struct {
        const char *name;
        bool use_loopback;
    } targets[] = {
        { "loopback", true },
        { "sta-ip",   false },
    };

    esp_netif_ip_info_t sta_ip = {0};
    bool have_sta_ip = false;
    {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta && esp_netif_get_ip_info(sta, &sta_ip) == ESP_OK && sta_ip.ip.addr != 0) {
            have_sta_ip = true;
        }
    }

    for (size_t i = 0; i < sizeof(targets)/sizeof(targets[0]); i++) {
        if (!targets[i].use_loopback && !have_sta_ip) {
            ESP_LOGW(HTTP_API_TAG, "HTTP self-connect %s skipped (no STA IP yet)", targets[i].name);
            continue;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            ESP_LOGW(HTTP_API_TAG, "HTTP self-connect %s: socket() failed (errno=%d)", targets[i].name, errno);
            continue;
        }

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(80);
        addr.sin_addr.s_addr = targets[i].use_loopback ? htonl(INADDR_LOOPBACK) : sta_ip.ip.addr;

        // Set non-blocking to avoid long timeout
        int flags = fcntl(sock, F_GETFL, 0);
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        int result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
        if (result == 0 || errno == EINPROGRESS) {
            char ipbuf[16] = {0};
            inet_ntoa_r(addr.sin_addr, ipbuf, sizeof(ipbuf));
            ESP_LOGI(HTTP_API_TAG, "HTTP self-connect %s OK (dst=%s:80)", targets[i].name, ipbuf);
        } else {
            ESP_LOGW(HTTP_API_TAG, "HTTP self-connect %s failed: errno=%d (%s)", targets[i].name, errno, strerror(errno));
        }

        close(sock);
    }
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
    struct sockaddr_storage peer = {0};
    struct sockaddr_storage local = {0};
    socklen_t peer_len = sizeof(peer);
    socklen_t local_len = sizeof(local);
    char peer_s[64];
    char local_s[64];

    if (getpeername(sockfd, (struct sockaddr *)&peer, &peer_len) == 0) {
        format_sockaddr((struct sockaddr *)&peer, peer_s, sizeof(peer_s));
    } else {
        snprintf(peer_s, sizeof(peer_s), "(peer? errno=%d)", errno);
    }

    if (getsockname(sockfd, (struct sockaddr *)&local, &local_len) == 0) {
        format_sockaddr((struct sockaddr *)&local, local_s, sizeof(local_s));
    } else {
        snprintf(local_s, sizeof(local_s), "(local? errno=%d)", errno);
    }

    ESP_LOGI(HTTP_API_TAG, "HTTP open: fd=%d peer=%s local=%s", sockfd, peer_s, local_s);
    return ESP_OK;
}

static void http_close_fn(httpd_handle_t hd, int sockfd) {
    (void)hd;
    ESP_LOGI(HTTP_API_TAG, "HTTP close: fd=%d", sockfd);
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
        ESP_LOGI(HTTP_API_TAG, "Command queue created (length=%d)", QUEUE_LEN);
    }

    // Create worker task if not exists
    if (!s_worker) {
        // This task executes user actions (swap/OTA/etc). These paths can be stack-hungry due to
        // nested calls + logging/formatting, so keep a healthy margin to avoid stack protection faults.
        BaseType_t ret = xTaskCreate(api_worker_task, "api_worker", 8192, NULL, 5, &s_worker);
        if (ret != pdPASS) {
            ESP_LOGE(HTTP_API_TAG, "Failed to create worker task");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(HTTP_API_TAG, "Worker task created");
    }

#if CONFIG_P3A_PICO8_ENABLE
    // Initialize PICO-8 stream parser (always, not just for USB)
    esp_err_t stream_init_ret = pico8_stream_init();
    if (stream_init_ret != ESP_OK) {
        ESP_LOGW(HTTP_API_TAG, "PICO-8 stream init failed: %s (continuing anyway)", esp_err_to_name(stream_init_ret));
    }
#endif

    // Start HTTP server
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;  // Increased to prevent stack overflow in WebSocket handlers
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 6;   // Limit connections to leave sockets for downloads (MQTT, HTTP client)
    cfg.max_uri_handlers = 12;  // Reduced: most endpoints are multiplexed via method routers
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
    ESP_LOGI(HTTP_API_TAG, "HTTP server started on port %d", cfg.server_port);

    // Register dedicated handlers first (more specific URIs must be registered before catch-all routers)
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

    ESP_LOGI(HTTP_API_TAG, "HTTP API server started on port 80");
    
    // Diagnostic: log network interfaces and verify server is listening
    log_all_netifs();
    verify_server_listening();
    
    return ESP_OK;
}

esp_err_t http_api_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(HTTP_API_TAG, "HTTP API server stopped");
    }
    // Worker task and queue remain active for simplicity
    return ESP_OK;
}
