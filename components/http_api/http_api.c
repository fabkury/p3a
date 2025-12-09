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
#include "mdns.h"
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
#include "ota_manager.h"
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
    } else if (strcmp(command_type, "play_channel") == 0) {
        // Extract channel information from payload
        cJSON *channel = cJSON_GetObjectItem(payload, "channel");
        cJSON *user_handle = cJSON_GetObjectItem(payload, "user_handle");
        
        if (channel && cJSON_IsString(channel)) {
            const char *ch_name = cJSON_GetStringValue(channel);
            const char *user = user_handle && cJSON_IsString(user_handle) ? cJSON_GetStringValue(user_handle) : NULL;
            
            ESP_LOGI(HTTP_API_TAG, "Switching to channel: %s", ch_name);
            esp_err_t err = makapix_switch_to_channel(ch_name, user);
            if (err != ESP_OK) {
                ESP_LOGE(HTTP_API_TAG, "Failed to switch channel: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(HTTP_API_TAG, "Invalid play_channel payload");
        }
    } else if (strcmp(command_type, "show_artwork") == 0) {
        // Extract artwork information from payload
        cJSON *art_url = cJSON_GetObjectItem(payload, "art_url");
        cJSON *storage_key = cJSON_GetObjectItem(payload, "storage_key");
        cJSON *post_id = cJSON_GetObjectItem(payload, "post_id");
        
        if (art_url && cJSON_IsString(art_url) && 
            storage_key && cJSON_IsString(storage_key)) {
            
            const char *url = cJSON_GetStringValue(art_url);
            const char *key = cJSON_GetStringValue(storage_key);
            int32_t pid = post_id && cJSON_IsNumber(post_id) ? (int32_t)cJSON_GetNumberValue(post_id) : 0;
            
            ESP_LOGI(HTTP_API_TAG, "Showing artwork: %s", url);
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
 * Switch to a Makapix channel
 * Body: {"channel": "all"|"promoted"|"user"|"by_user", "user_handle": "..." (optional)}
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

    cJSON *channel = cJSON_GetObjectItem(root, "channel");
    cJSON *user_handle = cJSON_GetObjectItem(root, "user_handle");

    if (!channel || !cJSON_IsString(channel)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing or invalid 'channel' field\",\"code\":\"INVALID_REQUEST\"}");
        return ESP_OK;
    }

    const char *ch_name = cJSON_GetStringValue(channel);
    const char *user = user_handle && cJSON_IsString(user_handle) ? cJSON_GetStringValue(user_handle) : NULL;

    esp_err_t err = makapix_switch_to_channel(ch_name, user);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Channel switch failed\",\"code\":\"CHANNEL_SWITCH_FAILED\"}");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
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

    // Validate range: 1-100000 seconds
    if (dwell_time < 1 || dwell_time > 100000) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Invalid dwell_time (must be 1-100000 seconds)\",\"code\":\"INVALID_DWELL_TIME\"}");
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

// ---------- mDNS Setup ----------

static esp_err_t start_mdns(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set("p3a");
    if (err != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set("p3a");
    if (err != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "mDNS service add failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(HTTP_API_TAG, "mDNS started: p3a.local");
    return ESP_OK;
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
        BaseType_t ret = xTaskCreate(api_worker_task, "api_worker", 4096, NULL, 5, &s_worker);
        if (ret != pdPASS) {
            ESP_LOGE(HTTP_API_TAG, "Failed to create worker task");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(HTTP_API_TAG, "Worker task created");
    }

    // Start mDNS
    esp_err_t e = start_mdns();
    if (e != ESP_OK) {
        ESP_LOGW(HTTP_API_TAG, "mDNS start failed (continuing anyway): %s", esp_err_to_name(e));
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
    cfg.max_uri_handlers = 28;  // Increased from 20 to accommodate OTA endpoints
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Register core REST API handlers
    httpd_uri_t u = {0};

    u.uri = "/status";
    u.method = HTTP_GET;
    u.handler = h_get_status;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/config";
    u.method = HTTP_GET;
    u.handler = h_get_config;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/config";
    u.method = HTTP_PUT;
    u.handler = h_put_config;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/action/reboot";
    u.method = HTTP_POST;
    u.handler = h_post_reboot;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/action/swap_next";
    u.method = HTTP_POST;
    u.handler = h_post_swap_next;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/action/swap_back";
    u.method = HTTP_POST;
    u.handler = h_post_swap_back;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/action/pause";
    u.method = HTTP_POST;
    u.handler = h_post_pause;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/action/resume";
    u.method = HTTP_POST;
    u.handler = h_post_resume;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/rotation";
    u.method = HTTP_GET;
    u.handler = h_get_rotation;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/rotation";
    u.method = HTTP_POST;
    u.handler = h_post_rotation;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/channel";
    u.method = HTTP_POST;
    u.handler = h_post_channel;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/settings/dwell_time";
    u.method = HTTP_GET;
    u.handler = h_get_dwell_time;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/settings/dwell_time";
    u.method = HTTP_PUT;
    u.handler = h_put_dwell_time;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    // Register handlers from other modules
    http_api_register_page_handlers(s_server);
    http_api_register_ota_handlers(s_server);
    http_api_register_upload_handler(s_server);

    ESP_LOGI(HTTP_API_TAG, "HTTP API server started on port 80");
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
