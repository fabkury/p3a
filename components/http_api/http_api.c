#include "http_api.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_netif.h"
#include "mdns.h"
#include "cJSON.h"
#include "app_state.h"
#include "config_store.h"
#include "app_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "animation_player.h"
#include "app_lcd.h"
#include "favicon_data.h"
#include "fs_init.h"
#include "version.h"
#include "makapix.h"
#include "makapix_mqtt.h"
#include "makapix_artwork.h"
#include "ota_manager.h"
#include "animation_player.h"
#include "ugfx_ui.h"
#if CONFIG_P3A_PICO8_ENABLE
#include "pico8_stream.h"
#endif
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// LCD dimensions from project configuration
#define LCD_MAX_WIDTH   EXAMPLE_LCD_H_RES
#define LCD_MAX_HEIGHT  EXAMPLE_LCD_V_RES
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#ifndef CONFIG_HTTPD_WS_SUPPORT
#error "CONFIG_HTTPD_WS_SUPPORT must be enabled (Component config -> HTTP Server -> Enable WebSocket support)"
#endif

static const char *TAG = "HTTP";

#define MAX_JSON (32 * 1024)
#define RECV_CHUNK 4096
#define QUEUE_LEN 10
#define MAX_FILE_PATH 256
#if CONFIG_P3A_PICO8_ENABLE
#define WS_MAX_FRAME_SIZE (8192 + 48 + 6) // framebuffer + palette + magic+len+flags header
#endif

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

// Action callback function pointers
typedef void (*action_callback_t)(void);
static action_callback_t s_swap_next_callback = NULL;
static action_callback_t s_swap_back_callback = NULL;

static QueueHandle_t s_cmdq = NULL;
static httpd_handle_t s_server = NULL;
static TaskHandle_t s_worker = NULL;
static uint32_t s_cmd_id = 0;
#if CONFIG_P3A_PICO8_ENABLE
static bool s_ws_client_connected = false;
#endif

// ---------- Worker Task ----------

static void do_reboot(void) {
    ESP_LOGI(TAG, "Reboot command executing, delaying 250ms...");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static void api_worker_task(void *arg) {
    ESP_LOGI(TAG, "Worker task started");
    for(;;) {
        command_t cmd;
        if (xQueueReceive(s_cmdq, &cmd, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Processing command %lu (type=%d)", cmd.id, cmd.type);
            app_state_enter_processing();

            switch(cmd.type) {
                case CMD_REBOOT:
                    do_reboot();
                    // No return - device restarts
                    break;

                case CMD_SWAP_NEXT:
                    if (s_swap_next_callback) {
                        ESP_LOGI(TAG, "Executing swap_next");
                        s_swap_next_callback();
                        app_state_enter_ready();
                    } else {
                        ESP_LOGW(TAG, "swap_next callback not set");
                        app_state_enter_error();
                    }
                    break;

                case CMD_SWAP_BACK:
                    if (s_swap_back_callback) {
                        ESP_LOGI(TAG, "Executing swap_back");
                        s_swap_back_callback();
                        app_state_enter_ready();
                    } else {
                        ESP_LOGW(TAG, "swap_back callback not set");
                        app_state_enter_error();
                    }
                    break;

                case CMD_PAUSE:
                    ESP_LOGI(TAG, "Executing pause");
                    app_lcd_set_animation_paused(true);
                    app_state_enter_ready();
                    break;

                case CMD_RESUME:
                    ESP_LOGI(TAG, "Executing resume");
                    app_lcd_set_animation_paused(false);
                    app_state_enter_ready();
                    break;

                default:
                    ESP_LOGE(TAG, "Unknown command type: %d", cmd.type);
                    app_state_enter_error();
                    break;
            }
        }
    }
}

static bool enqueue_cmd(command_type_t t) {
    if (!s_cmdq) {
        ESP_LOGE(TAG, "Command queue not initialized");
        return false;
    }

    command_t c = { .type = t, .id = ++s_cmd_id };
    BaseType_t result = xQueueSend(s_cmdq, &c, pdMS_TO_TICKS(10));
    if (result != pdTRUE) {
        ESP_LOGW(TAG, "Failed to enqueue command (queue full)");
        return false;
    }
    ESP_LOGI(TAG, "Command %lu enqueued", c.id);
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
    ESP_LOGI(TAG, "MQTT command received: %s", command_type);
    
    if (strcmp(command_type, "swap_next") == 0) {
        api_enqueue_swap_next();
    } else if (strcmp(command_type, "swap_back") == 0) {
        api_enqueue_swap_back();
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
            
            ESP_LOGI(TAG, "Downloading artwork: %s", url);
            
            // Download artwork to vault
            char file_path[256];
            esp_err_t err = makapix_artwork_download(url, key, file_path, sizeof(file_path));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Artwork downloaded to: %s", file_path);
                
                // Ensure cache limit
                makapix_artwork_ensure_cache_limit(250);
                
                // Update post ID tracking
                makapix_set_current_post_id(pid);
                
                // Trigger swap_next to display the new artwork
                // Note: Full playback implementation deferred per plan
                api_enqueue_swap_next();
            } else {
                ESP_LOGE(TAG, "Failed to download artwork: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "Invalid show_artwork payload");
        }
    } else {
        ESP_LOGW(TAG, "Unknown command type: %s", command_type);
    }
}

// ---------- Callback Registration ----------

void http_api_set_action_handlers(action_callback_t swap_next, action_callback_t swap_back) {
    s_swap_next_callback = swap_next;
    s_swap_back_callback = swap_back;
    ESP_LOGI(TAG, "Action handlers registered");
    
    // Register MQTT command callback (only if makapix is available)
    makapix_mqtt_set_command_callback(makapix_command_handler);
}

// ---------- HTTP Helper Functions ----------

static const char* http_status_str(int status) {
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

static void send_json(httpd_req_t *req, int status, const char *json) {
    httpd_resp_set_status(req, http_status_str(status));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool ensure_json_content(httpd_req_t *req) {
    char content_type[64] = {0};
    esp_err_t ret = httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    if (ret != ESP_OK) {
        return false;
    }
    // Check if starts with "application/json"
    return (strncasecmp(content_type, "application/json", 16) == 0);
}

static char* recv_body_json(httpd_req_t *req, size_t *out_len, int *out_err_status) {
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

static void register_uri_handler_or_log(httpd_handle_t server, httpd_uri_t *uri) {
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI %s: %s", uri->uri, esp_err_to_name(err));
    }
}

// ---------- HTTP Handlers ----------

/**
 * GET /config/network
 * Returns HTML status page with connection information and erase button
 */
static esp_err_t h_get_network_config(httpd_req_t *req) {
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_RMT");
    }
    
    esp_netif_ip_info_t ip_info;
    bool has_ip = false;
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
        has_ip = true;
    }
    
    wifi_ap_record_t ap = {0};
    bool has_rssi = (esp_wifi_remote_sta_get_ap_info(&ap) == ESP_OK);
    
    char saved_ssid[33] = {0};
    bool has_ssid = (app_wifi_get_saved_ssid(saved_ssid, sizeof(saved_ssid)) == ESP_OK);
    
    // Build HTML response - use static HTML template to avoid format string issues
    static const char html_header[] =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">"
        "<title>p3a - Network</title>"
        "<style>"
        "* { box-sizing: border-box; }"
        "body {"
        "    margin: 0;"
        "    padding: 16px;"
        "    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "    min-height: 100vh;"
        "    display: flex;"
        "    flex-direction: column;"
        "    align-items: center;"
        "}"
        ".container { width: min(420px, 100%); }"
        "h1 {"
        "    text-align: center;"
        "    color: white;"
        "    font-size: 1.5rem;"
        "    font-weight: 300;"
        "    margin: 0 0 16px;"
        "    text-shadow: 0 2px 4px rgba(0,0,0,0.2);"
        "}"
        ".card {"
        "    background: rgba(255,255,255,0.95);"
        "    border-radius: 16px;"
        "    padding: 16px;"
        "    margin-bottom: 12px;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "}"
        ".card h2 {"
        "    margin: 0 0 12px;"
        "    font-size: 0.85rem;"
        "    font-weight: 600;"
        "    color: #333;"
        "    text-transform: uppercase;"
        "    letter-spacing: 0.05em;"
        "}"
        ".info-row {"
        "    display: flex;"
        "    justify-content: space-between;"
        "    padding: 10px 0;"
        "    border-bottom: 1px solid #eee;"
        "}"
        ".info-row:last-child { border-bottom: none; }"
        ".info-label { color: #666; font-size: 0.9rem; }"
        ".info-value { color: #333; font-weight: 500; font-size: 0.9rem; text-align: right; }"
        ".status-badge {"
        "    display: inline-block;"
        "    padding: 4px 10px;"
        "    border-radius: 12px;"
        "    font-size: 0.8rem;"
        "    font-weight: 600;"
        "}"
        ".status-connected { background: #e8f5e9; color: #2e7d32; }"
        ".status-disconnected { background: #ffebee; color: #c62828; }"
        ".erase-btn {"
        "    width: 100%;"
        "    background: #ff6b6b;"
        "    color: white;"
        "    padding: 14px;"
        "    border: none;"
        "    border-radius: 12px;"
        "    font-size: 0.95rem;"
        "    font-weight: 500;"
        "    cursor: pointer;"
        "    box-shadow: 0 4px 12px rgba(255,107,107,0.3);"
        "    transition: transform 0.2s;"
        "}"
        ".erase-btn:active { transform: scale(0.98); }"
        ".warning {"
        "    color: #666;"
        "    font-size: 0.8rem;"
        "    margin-top: 10px;"
        "    text-align: center;"
        "}"
        ".back-link {"
        "    display: block;"
        "    text-align: center;"
        "    color: rgba(255,255,255,0.8);"
        "    text-decoration: none;"
        "    margin-top: 16px;"
        "    font-size: 0.9rem;"
        "}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"container\">"
        "<h1>Network Status</h1>"
        "<div class=\"card\">"
        "<h2>Connection</h2>"
        "<div class=\"info-row\">"
        "<span class=\"info-label\">Status</span>"
        "<span class=\"info-value\">"
        "<span class=\"status-badge ";
    
    static const char html_status_connected[] = "status-connected\">Connected</span>";
    static const char html_status_disconnected[] = "status-disconnected\">Disconnected</span>";
    static const char html_status_end[] = "</span></div>";
    
    char html[4096];  // Increased buffer size
    int len = 0;
    int ret;
    
    // Copy header
    ret = snprintf(html, sizeof(html), "%s", html_header);
    if (ret < 0 || ret >= sizeof(html)) {
        ESP_LOGE(TAG, "HTML buffer overflow in header");
        return ESP_FAIL;
    }
    len = ret;
    
    // Add status badge
    ret = snprintf(html + len, sizeof(html) - len, "%s%s",
        has_ip ? html_status_connected : html_status_disconnected,
        html_status_end);
    if (ret < 0 || len + ret >= sizeof(html)) {
        ESP_LOGE(TAG, "HTML buffer overflow in status badge");
        return ESP_FAIL;
    }
    len += ret;
    
    if (has_ssid && strlen(saved_ssid) > 0) {
        ret = snprintf(html + len, sizeof(html) - len,
            "<div class=\"info-row\">"
            "<span class=\"info-label\">Network (SSID):</span>"
            "<span class=\"info-value\">%s</span>"
            "</div>",
            saved_ssid
        );
        if (ret < 0 || len + ret >= sizeof(html)) {
            ESP_LOGE(TAG, "HTML buffer overflow in SSID");
            return ESP_FAIL;
        }
        len += ret;
    }
    
    if (has_ip) {
        ret = snprintf(html + len, sizeof(html) - len,
            "<div class=\"info-row\">"
            "<span class=\"info-label\">IP Address:</span>"
            "<span class=\"info-value\">" IPSTR "</span>"
            "</div>"
            "<div class=\"info-row\">"
            "<span class=\"info-label\">Gateway:</span>"
            "<span class=\"info-value\">" IPSTR "</span>"
            "</div>"
            "<div class=\"info-row\">"
            "<span class=\"info-label\">Netmask:</span>"
            "<span class=\"info-value\">" IPSTR "</span>"
            "</div>",
            IP2STR(&ip_info.ip),
            IP2STR(&ip_info.gw),
            IP2STR(&ip_info.netmask)
        );
        if (ret < 0 || len + ret >= sizeof(html)) {
            ESP_LOGE(TAG, "HTML buffer overflow in IP info");
            return ESP_FAIL;
        }
        len += ret;
    }
    
    if (has_rssi) {
        ret = snprintf(html + len, sizeof(html) - len,
            "<div class=\"info-row\">"
            "<span class=\"info-label\">Signal Strength (RSSI):</span>"
            "<span class=\"info-value\">%d dBm</span>"
            "</div>",
            ap.rssi
        );
        if (ret < 0 || len + ret >= sizeof(html)) {
            ESP_LOGE(TAG, "HTML buffer overflow in RSSI");
            return ESP_FAIL;
        }
        len += ret;
    }
    
    static const char html_footer[] =
        "</div>"
        "<div class=\"card\">"
        "<form action=\"/erase\" method=\"POST\" onsubmit=\"return confirm('Erase Wi-Fi credentials? Device will reboot into setup mode.');\">"
        "<button type=\"submit\" class=\"erase-btn\">Erase Wi-Fi &amp; Reboot</button>"
        "</form>"
        "<p class=\"warning\">Device will restart in configuration mode</p>"
        "</div>"
        "<a href=\"/\" class=\"back-link\">&#8592; Back to Home</a>"
        "</div>"
        "</body>"
        "</html>";
    
    ret = snprintf(html + len, sizeof(html) - len, "%s", html_footer);
    if (ret < 0 || len + ret >= sizeof(html)) {
        ESP_LOGE(TAG, "HTML buffer overflow in footer");
        return ESP_FAIL;
    }
    len += ret;
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, len);
    ESP_LOGI(TAG, "Status page sent, length=%d", len);
    return ESP_OK;
}

/**
 * POST /erase
 * Erases Wi-Fi credentials and reboots the device
 */
static esp_err_t h_post_erase(httpd_req_t *req) {
    ESP_LOGI(TAG, "Erase credentials requested via web interface");
    app_wifi_erase_credentials();
    
    const char *response = 
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Credentials Erased</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; text-align: center; }"
        ".container { max-width: 500px; margin: 50px auto; background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
        "h1 { color: #333; }"
        "p { color: #666; margin: 20px 0; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"container\">"
        "<h1>Credentials Erased</h1>"
        "<p>Wi-Fi credentials have been erased. The device will reboot in a moment...</p>"
        "<p>After reboot, connect to the configuration access point to set up Wi-Fi again.</p>"
        "</div>"
        "</body>"
        "</html>";
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    
    // Delay before reboot to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

/**
 * GET /favicon.ico
 * Returns the favicon PNG image
 */
static esp_err_t h_get_favicon(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/png");
    httpd_resp_send(req, (const char *)favicon_data, FAVICON_SIZE);
    return ESP_OK;
}

/**
 * Get MIME type from file extension
 */
static const char* get_mime_type(const char* path) {
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

#if CONFIG_P3A_PICO8_ENABLE
/**
 * GET /pico8
 * Serves the PICO-8 monitor HTML page
 */
static esp_err_t h_get_pico8(httpd_req_t *req) {
    const char* filepath = "/spiffs/pico8/index.html";
    
    FILE* f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", filepath);
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
#endif // CONFIG_P3A_PICO8_ENABLE

/**
 * GET /static/<path>
 * Serves static files from SPIFFS
 */
static esp_err_t h_get_static(httpd_req_t *req) {
    const char* uri = req->uri;
    
    // Map /static/* to /spiffs/static/*
    char filepath[MAX_FILE_PATH];
    static const char *prefix = "/spiffs";
    size_t prefix_len = strlen(prefix);
    size_t uri_len = strlen(uri);
    if (prefix_len + uri_len >= sizeof(filepath)) {
        ESP_LOGW(TAG, "Static path too long: %s", uri);
        httpd_resp_set_status(req, "414 Request-URI Too Long");
        httpd_resp_send(req, "Path too long", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    snprintf(filepath, sizeof(filepath), "%s%s", prefix, uri);
    
    FILE* f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", filepath);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "File not found", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
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
    
    // Set MIME type
    httpd_resp_set_type(req, get_mime_type(filepath));
    
    // Set cache headers for static assets
    if (strstr(filepath, ".js") || strstr(filepath, ".wasm") || strstr(filepath, ".css")) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    }
    
    // Stream file in chunks
    char chunk[RECV_CHUNK];
    long remaining = size;
    
    while (remaining > 0) {
        size_t to_read = (remaining < RECV_CHUNK) ? remaining : RECV_CHUNK;
        size_t read = fread(chunk, 1, to_read, f);
        
        if (read == 0) {
            break;
        }
        
        esp_err_t ret = httpd_resp_send_chunk(req, chunk, read);
        if (ret != ESP_OK) {
            fclose(f);
            return ret;
        }
        
        remaining -= read;
    }
    
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // End response
    
    return ESP_OK;
}

#if CONFIG_P3A_PICO8_ENABLE
/**
 * WebSocket handler for /pico_stream
 */
static esp_err_t h_ws_pico_stream(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket connection request");
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
            ESP_LOGE(TAG, "Failed to read WebSocket header: %s", esp_err_to_name(ret));
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
                ESP_LOGE(TAG, "Unable to allocate %zu bytes for WS payload", payload_len);
                return ESP_ERR_NO_MEM;
            }
            payload_allocated = true;
        } else {
            ESP_LOGW(TAG, "WebSocket frame too large (%zu bytes)", payload_len);
            return ESP_ERR_INVALID_SIZE;
        }

        frame.payload = payload_buf;
        ret = httpd_ws_recv_frame(req, &frame, payload_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read WebSocket payload: %s", esp_err_to_name(ret));
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
        ESP_LOGI(TAG, "WebSocket close frame");
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
        ESP_LOGW(TAG, "Ignoring non-binary WebSocket frame (type=%d, len=%zu)", frame.type, frame.len);
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
        ESP_LOGW(TAG, "pico8_stream_feed_packet failed: %s (len=%zu)",
                 esp_err_to_name(feed_ret), frame.len);
    }

    if (payload_allocated) {
        free(payload_buf);
    }

    return ESP_OK;
}
#endif // CONFIG_P3A_PICO8_ENABLE

/**
 * GET /
 * Returns Remote Control HTML page with swap buttons and navigation to network config
 */
static esp_err_t h_get_root(httpd_req_t *req) {
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">"
        "<title>p3a</title>"
        "<style>"
        "* { box-sizing: border-box; }"
        "body {"
        "    margin: 0;"
        "    padding: 12px 10px 16px;"
        "    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "    min-height: 100vh;"
        "    display: flex;"
        "    flex-direction: column;"
        "    align-items: center;"
        "    justify-content: flex-start;"
        "    gap: 12px;"
        "    overflow-x: hidden;"
        "}"
        "@supports (min-height: 100svh) {"
        "    body {"
        "        min-height: 100svh;"
        "    }"
        "}"
        "@supports (min-height: 100dvh) {"
        "    body {"
        "        min-height: 100dvh;"
        "    }"
        "}"
        ".header {"
        "    text-align: center;"
        "    padding: 8px 0 4px;"
        "    color: white;"
        "}"
        ".header h1 {"
        "    margin: 0;"
        "    font-size: clamp(2rem, 4vw, 2.5rem);"
        "    font-weight: 300;"
        "    letter-spacing: 0.1em;"
        "    text-transform: lowercase;"
        "}"
        ".controls {"
        "    flex: 0 0 auto;"
        "    width: min(420px, 100%);"
        "    display: flex;"
        "    flex-direction: column;"
        "    align-items: center;"
        "    justify-content: center;"
        "    padding: 16px;"
        "    gap: 16px;"
        "    background: rgba(255,255,255,0.12);"
        "    border-radius: 18px;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "}"
        ".arrow-row {"
        "    display: flex;"
        "    gap: 18px;"
        "    align-items: center;"
        "    justify-content: space-between;"
        "    width: 100%;"
        "}"
        ".arrow-btn {"
        "    background: rgba(255,255,255,0.95);"
        "    border: none;"
        "    border-radius: 50%;"
        "    width: 72px;"
        "    height: 72px;"
        "    display: flex;"
        "    align-items: center;"
        "    justify-content: center;"
        "    cursor: pointer;"
        "    font-size: 1.8rem;"
        "    color: #667eea;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "    transition: all 0.2s ease;"
        "    -webkit-tap-highlight-color: transparent;"
        "}"
        ".arrow-btn:active {"
        "    transform: scale(0.92);"
        "    box-shadow: 0 2px 6px rgba(0,0,0,0.2);"
        "}"
        ".arrow-btn:disabled {"
        "    opacity: 0.5;"
        "    cursor: not-allowed;"
        "}"
        ".pause-btn {"
        "    background: rgba(255,255,255,0.95);"
        "    border: none;"
        "    border-radius: 12px;"
        "    padding: 10px 28px;"
        "    font-size: 0.95rem;"
        "    font-weight: 500;"
        "    color: #667eea;"
        "    cursor: pointer;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "    transition: all 0.2s ease;"
        "    -webkit-tap-highlight-color: transparent;"
        "    min-width: 110px;"
        "}"
        ".pause-btn:active {"
        "    transform: scale(0.95);"
        "}"
        ".footer {"
        "    width: min(420px, 100%);"
        "    padding: 8px 0 4px;"
        "    display: flex;"
        "    justify-content: center;"
        "    gap: 10px;"
        "    flex-wrap: wrap;"
        "}"
        ".footer-btn {"
        "    background: rgba(255,255,255,0.2);"
        "    border: 1px solid rgba(255,255,255,0.3);"
        "    border-radius: 8px;"
        "    padding: 7px 14px;"
        "    font-size: 0.85rem;"
        "    color: white;"
        "    cursor: pointer;"
        "    transition: all 0.2s ease;"
        "    -webkit-tap-highlight-color: transparent;"
        "}"
        ".footer-btn:active {"
        "    background: rgba(255,255,255,0.3);"
        "}"
        ".status {"
        "    position: fixed;"
        "    top: clamp(48px, 12vh, 80px);"
        "    left: 50%;"
        "    transform: translateX(-50%);"
        "    padding: 10px 20px;"
        "    border-radius: 8px;"
        "    font-size: 0.875rem;"
        "    font-weight: 500;"
        "    display: none;"
        "    z-index: 1000;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.2);"
        "}"
        ".status.success {"
        "    background: #4CAF50;"
        "    color: white;"
        "}"
        ".status.error {"
        "    background: #f44336;"
        "    color: white;"
        "}"
        ".upload-section {"
        "    width: min(420px, 100%);"
        "    background: rgba(255,255,255,0.95);"
        "    border-radius: 16px;"
        "    padding: 14px;"
        "    margin: 0;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "}"
        ".upload-section h3 {"
        "    margin: 0 0 8px;"
        "    font-size: 0.85rem;"
        "    font-weight: 500;"
        "    color: #333;"
        "    text-transform: uppercase;"
        "    letter-spacing: 0.05em;"
        "}"
        ".upload-form {"
        "    display: flex;"
        "    flex-direction: column;"
        "    gap: 8px;"
        "}"
        ".file-input-wrapper {"
        "    position: relative;"
        "    overflow: hidden;"
        "}"
        ".file-input-wrapper input[type=file] {"
        "    position: absolute;"
        "    left: -9999px;"
        "}"
        ".file-input-label {"
        "    display: block;"
        "    padding: 9px;"
        "    background: #667eea;"
        "    color: white;"
        "    border-radius: 8px;"
        "    text-align: center;"
        "    font-size: 0.85rem;"
        "    cursor: pointer;"
        "    transition: background 0.2s;"
        "}"
        ".file-input-label:active {"
        "    background: #5568d3;"
        "}"
        ".file-name {"
        "    font-size: 0.75rem;"
        "    color: #555;"
        "    word-break: break-word;"
        "    min-height: 1.2em;"
        "}"
        ".upload-btn {"
        "    background: #4CAF50;"
        "    color: white;"
        "    border: none;"
        "    padding: 9px;"
        "    border-radius: 8px;"
        "    font-size: 0.85rem;"
        "    font-weight: 500;"
        "    cursor: pointer;"
        "    transition: background 0.2s;"
        "}"
        ".upload-btn:active:not(:disabled) {"
        "    background: #45a049;"
        "}"
        ".upload-btn:disabled {"
        "    background: #ccc;"
        "    cursor: not-allowed;"
        "}"
        ".upload-progress {"
        "    display: none;"
        "    margin-top: 6px;"
        "}"
        ".upload-progress.active {"
        "    display: block;"
        "}"
        ".progress-bar {"
        "    width: 100%;"
        "    height: 6px;"
        "    background: #e0e0e0;"
        "    border-radius: 3px;"
        "    overflow: hidden;"
        "}"
        ".progress-fill {"
        "    height: 100%;"
        "    background: #4CAF50;"
        "    transition: width 0.3s;"
        "    width: 0%;"
        "}"
        "@media (max-width: 480px) {"
        "    .header h1 { font-size: 1.9rem; }"
        "    .controls { padding: 14px; }"
        "    .arrow-btn { width: 64px; height: 64px; font-size: 1.6rem; }"
        "    .arrow-row { gap: 16px; }"
        "    .pause-btn { padding: 9px 20px; font-size: 0.85rem; }"
        "}"
        "@media (max-height: 640px) {"
        "    .header h1 { font-size: 1.75rem; letter-spacing: 0.08em; }"
        "    .controls { padding: 12px; gap: 12px; }"
        "    .arrow-btn { width: 60px; height: 60px; font-size: 1.5rem; }"
        "    .pause-btn { padding: 8px 18px; font-size: 0.82rem; }"
        "    .upload-section { padding: 12px; }"
        "    .footer { padding-top: 4px; }"
        "}"
        "@media (min-width: 481px) {"
        "    .arrow-btn:hover { transform: scale(1.05); }"
        "    .pause-btn:hover { transform: scale(1.02); }"
        "    .footer-btn:hover { background: rgba(255,255,255,0.3); }"
        "}"
        ".update-banner {"
        "    display: none;"
        "    width: min(420px, 100%);"
        "    background: linear-gradient(135deg, #00c853 0%, #00e676 100%);"
        "    border-radius: 12px;"
        "    padding: 12px 16px;"
        "    color: white;"
        "    cursor: pointer;"
        "    transition: transform 0.2s, box-shadow 0.2s;"
        "    box-shadow: 0 4px 12px rgba(0,200,83,0.3);"
        "}"
        ".update-banner:active {"
        "    transform: scale(0.98);"
        "}"
        ".update-banner h4 {"
        "    margin: 0 0 4px;"
        "    font-size: 0.95rem;"
        "    font-weight: 600;"
        "}"
        ".update-banner p {"
        "    margin: 0;"
        "    font-size: 0.8rem;"
        "    opacity: 0.9;"
        "}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"header\">"
        "    <h1>p3a</h1>"
        "</div>"
        "<div class=\"controls\">"
        "    <div class=\"arrow-row\">"
        "        <button class=\"arrow-btn\" id=\"back-btn\" onclick=\"sendCommand('swap_back')\">◄</button>"
        "        <button class=\"arrow-btn\" id=\"next-btn\" onclick=\"sendCommand('swap_next')\">►</button>"
        "    </div>"
        "    <button class=\"pause-btn\" id=\"pause-btn\" onclick=\"togglePause()\">Pause</button>"
        "</div>"
        "<div class=\"upload-section\">"
        "    <h3>Upload</h3>"
        "    <form class=\"upload-form\" id=\"upload-form\" enctype=\"multipart/form-data\">"
        "        <div class=\"file-input-wrapper\">"
        "            <label for=\"file-input\" class=\"file-input-label\">Choose File</label>"
        "            <input type=\"file\" id=\"file-input\" name=\"file\" accept=\".webp,.gif,.jpg,.jpeg,.png,image/webp,image/gif,image/jpeg,image/png\" required>"
        "        </div>"
        "        <div class=\"file-name\" id=\"file-name\"></div>"
        "        <button type=\"submit\" class=\"upload-btn\" id=\"upload-btn\">Upload</button>"
        "        <div class=\"upload-progress\" id=\"upload-progress\">"
        "            <div class=\"progress-bar\">"
        "                <div class=\"progress-fill\" id=\"progress-fill\"></div>"
        "            </div>"
        "        </div>"
        "    </form>"
        "</div>"
        "<div class=\"update-banner\" id=\"update-banner\" onclick=\"window.location.href='/ota'\">"
        "    <h4>&#x2B06; Update Available</h4>"
        "    <p id=\"update-version\">A new firmware version is ready to install</p>"
        "</div>"
        "<div class=\"footer\">"
        "    <button class=\"footer-btn\" onclick=\"window.location.href='/config/network'\">Network</button>"
#if CONFIG_P3A_PICO8_ENABLE
        "    <button class=\"footer-btn\" onclick=\"window.location.href='/pico8'\">PICO-8</button>"
#endif
        "    <button class=\"footer-btn\" onclick=\"window.location.href='/ota'\">Update</button>"
        "</div>"
        "<div class=\"status\" id=\"status\"></div>"
        "<script>"
        "var isPaused = false;"
        "function togglePause() {"
        "    var action = isPaused ? 'resume' : 'pause';"
        "    var status = document.getElementById('status');"
        "    var pauseBtn = document.getElementById('pause-btn');"
        "    pauseBtn.disabled = true;"
        "    var xhr = new XMLHttpRequest();"
        "    xhr.open('POST', '/action/' + action, true);"
        "    xhr.setRequestHeader('Content-Type', 'application/json');"
        "    xhr.onreadystatechange = function() {"
        "        if (xhr.readyState === 4) {"
        "            var pauseBtn = document.getElementById('pause-btn');"
        "            try {"
        "                var result = JSON.parse(xhr.responseText);"
        "                if (xhr.status >= 200 && xhr.status < 300 && result.ok) {"
        "                    isPaused = !isPaused;"
        "                    pauseBtn.textContent = isPaused ? 'Resume' : 'Pause';"
        "                    status.textContent = isPaused ? 'Paused' : 'Resumed';"
        "                    status.className = 'status success';"
        "                } else {"
        "                    status.textContent = 'Command failed: ' + (result.error || 'HTTP ' + xhr.status);"
        "                    status.className = 'status error';"
        "                }"
        "            } catch (e) {"
        "                status.textContent = 'Parse error: ' + e.message;"
        "                status.className = 'status error';"
        "            }"
        "            status.style.display = 'block';"
        "            setTimeout(function() { status.style.display = 'none'; }, 2000);"
        "            pauseBtn.disabled = false;"
        "        }"
        "    };"
        "    xhr.send('{}');"
        "}"
        "function sendCommand(action) {"
        "    console.log('Sending command:', action);"
        "    var status = document.getElementById('status');"
        "    var backBtn = document.getElementById('back-btn');"
        "    var nextBtn = document.getElementById('next-btn');"
        "    backBtn.disabled = true;"
        "    nextBtn.disabled = true;"
        "    var xhr = new XMLHttpRequest();"
        "    xhr.open('POST', '/action/' + action, true);"
        "    xhr.setRequestHeader('Content-Type', 'application/json');"
        "    xhr.onreadystatechange = function() {"
        "        if (xhr.readyState === 4) {"
        "            console.log('XHR status:', xhr.status);"
        "            console.log('XHR response:', xhr.responseText);"
        "            try {"
        "                var result = JSON.parse(xhr.responseText);"
        "                if (xhr.status >= 200 && xhr.status < 300 && result.ok) {"
        "                    status.textContent = 'Command sent successfully';"
        "                    status.className = 'status success';"
        "                } else {"
        "                    status.textContent = 'Command failed: ' + (result.error || 'HTTP ' + xhr.status);"
        "                    status.className = 'status error';"
        "                }"
        "            } catch (e) {"
        "                status.textContent = 'Parse error: ' + e.message;"
        "                status.className = 'status error';"
        "            }"
        "            status.style.display = 'block';"
        "            setTimeout(function() { status.style.display = 'none'; }, 2000);"
        "            backBtn.disabled = false;"
        "            nextBtn.disabled = false;"
        "        }"
        "    };"
        "    xhr.send('{}');"
        "}"
        "var fileInput = document.getElementById('file-input');"
        "var fileName = document.getElementById('file-name');"
        "var uploadForm = document.getElementById('upload-form');"
        "var uploadBtn = document.getElementById('upload-btn');"
        "var uploadProgress = document.getElementById('upload-progress');"
        "var progressFill = document.getElementById('progress-fill');"
        "var statusDiv = document.getElementById('status');"
        "function isJpegFile(file) {"
        "    var name = file.name.toLowerCase();"
        "    return name.endsWith('.jpg') || name.endsWith('.jpeg') || file.type === 'image/jpeg';"
        "}"
        "function getImageDimensions(file) {"
        "    return new Promise(function(resolve, reject) {"
        "        var reader = new FileReader();"
        "        reader.onerror = function() { reject(new Error('Failed to read file.')); };"
        "        reader.onload = function() {"
        "            var img = new Image();"
        "            img.onload = function() {"
        "                resolve({ width: img.width, height: img.height });"
        "            };"
        "            img.onerror = function() { reject(new Error('Failed to load image.')); };"
        "            img.src = reader.result;"
        "        };"
        "        reader.readAsDataURL(file);"
        "    });"
        "}"
        "function resizeAndConvertToPng(file, maxW, maxH) {"
        "    return new Promise(function(resolve, reject) {"
        "        if (!file.type.startsWith('image/')) {"
        "            reject(new Error('Selected file is not an image.'));"
        "            return;"
        "        }"
        "        var reader = new FileReader();"
        "        reader.onerror = function() { reject(new Error('Failed to read file.')); };"
        "        reader.onload = function() {"
        "            var img = new Image();"
        "            img.onload = function() {"
        "                try {"
        "                    var width = img.width;"
        "                    var height = img.height;"
        "                    var scale = Math.min(maxW / width, maxH / height, 1);"
        "                    var newW = Math.round(width * scale);"
        "                    var newH = Math.round(height * scale);"
        "                    var canvas = document.createElement('canvas');"
        "                    canvas.width = newW;"
        "                    canvas.height = newH;"
        "                    var ctx = canvas.getContext('2d');"
        "                    ctx.drawImage(img, 0, 0, newW, newH);"
        "                    canvas.toBlob(function(blob) {"
        "                        if (!blob) {"
        "                            reject(new Error('Canvas conversion to PNG failed.'));"
        "                        } else {"
        "                            resolve(blob);"
        "                        }"
        "                    }, 'image/png', 1.0);"
        "                } catch (e) {"
        "                    reject(e);"
        "                }"
        "            };"
        "            img.onerror = function() { reject(new Error('Failed to load image.')); };"
        "            img.src = reader.result;"
        "        };"
        "        reader.readAsDataURL(file);"
        "    });"
        "}"
        "fileInput.addEventListener('change', function(e) {"
        "    var file = e.target.files[0];"
        "    if (file) {"
        "        var maxSize = isJpegFile(file) ? 25 * 1024 * 1024 : 5 * 1024 * 1024;"
        "        var maxSizeMB = isJpegFile(file) ? '25MB' : '5MB';"
        "        if (file.size > maxSize) {"
        "            fileName.textContent = 'File too large! Maximum size is ' + maxSizeMB + '.';"
        "            fileName.style.color = '#f44336';"
        "            uploadBtn.disabled = true;"
        "            fileInput.value = '';"
        "        } else {"
        "            fileName.textContent = 'Selected: ' + file.name + ' (' + (file.size / 1024).toFixed(1) + ' KB)';"
        "            fileName.style.color = '#666';"
        "            uploadBtn.disabled = false;"
        "        }"
        "    } else {"
        "        fileName.textContent = '';"
        "        uploadBtn.disabled = false;"
        "    }"
        "});"
        "uploadForm.addEventListener('submit', function(e) {"
        "    e.preventDefault();"
        "    var file = fileInput.files[0];"
        "    if (!file) {"
        "        statusDiv.textContent = 'Please select a file';"
        "        statusDiv.className = 'status error';"
        "        statusDiv.style.display = 'block';"
        "        setTimeout(function() { statusDiv.style.display = 'none'; }, 3000);"
        "        return;"
        "    }"
        "    var maxSize = isJpegFile(file) ? 25 * 1024 * 1024 : 5 * 1024 * 1024;"
        "    var maxSizeMB = isJpegFile(file) ? '25MB' : '5MB';"
        "    if (file.size > maxSize) {"
        "        statusDiv.textContent = 'File too large! Maximum size is ' + maxSizeMB + '.';"
        "        statusDiv.className = 'status error';"
        "        statusDiv.style.display = 'block';"
        "        setTimeout(function() { statusDiv.style.display = 'none'; }, 3000);"
        "        return;"
        "    }"
        "    uploadBtn.disabled = true;"
        "    statusDiv.textContent = 'Processing...';"
        "    statusDiv.className = 'status';"
        "    statusDiv.style.display = 'block';"
        "    var processAndUpload = function(fileToUpload, filename) {"
        "        var formData = new FormData();"
        "        formData.append('file', fileToUpload, filename);"
        "        uploadProgress.classList.add('active');"
        "        progressFill.style.width = '0%';"
        "        statusDiv.textContent = 'Uploading...';"
        "        var xhr = new XMLHttpRequest();"
        "        xhr.open('POST', '/upload', true);"
        "        xhr.upload.onprogress = function(e) {"
        "            if (e.lengthComputable) {"
        "                var percentComplete = (e.loaded / e.total) * 100;"
        "                progressFill.style.width = percentComplete + '%';"
        "            }"
        "        };"
        "        xhr.onreadystatechange = function() {"
        "            if (xhr.readyState === 4) {"
        "                uploadBtn.disabled = false;"
        "                uploadProgress.classList.remove('active');"
        "                progressFill.style.width = '0%';"
        "                try {"
        "                    var result = JSON.parse(xhr.responseText);"
        "                    if (xhr.status >= 200 && xhr.status < 300 && result.ok) {"
        "                        statusDiv.textContent = 'Upload successful!';"
        "                        statusDiv.className = 'status success';"
        "                        fileInput.value = '';"
        "                        fileName.textContent = '';"
        "                    } else {"
        "                        statusDiv.textContent = 'Upload failed: ' + (result.error || 'HTTP ' + xhr.status);"
        "                        statusDiv.className = 'status error';"
        "                    }"
        "                } catch (e) {"
        "                    statusDiv.textContent = 'Upload failed: ' + xhr.statusText;"
        "                    statusDiv.className = 'status error';"
        "                }"
        "                statusDiv.style.display = 'block';"
        "                setTimeout(function() { statusDiv.style.display = 'none'; }, 5000);"
        "            }"
        "        };"
        "        xhr.send(formData);"
        "    };"
        "    if (isJpegFile(file)) {"
        "        getImageDimensions(file).then(function(dims) {"
        "            if (dims.width > LCD_MAX_WIDTH || dims.height > LCD_MAX_HEIGHT) {"
        "                return resizeAndConvertToPng(file, LCD_MAX_WIDTH, LCD_MAX_HEIGHT).then(function(pngBlob) {"
        "                    var pngFileName = file.name.replace(/\\.[^/.]+$/, '.png');"
        "                    processAndUpload(pngBlob, pngFileName);"
        "                });"
        "            } else {"
        "                processAndUpload(file, file.name);"
        "            }"
        "        }).catch(function(err) {"
        "            uploadBtn.disabled = false;"
        "            statusDiv.textContent = 'Error processing image: ' + err.message;"
        "            statusDiv.className = 'status error';"
        "            statusDiv.style.display = 'block';"
        "            setTimeout(function() { statusDiv.style.display = 'none'; }, 5000);"
        "        });"
        "    } else {"
        "        processAndUpload(file, file.name);"
        "    }"
        "});"
        "function checkForUpdates() {"
        "    fetch('/ota/status').then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok && d.data.state === 'update_available') {"
        "            var banner = document.getElementById('update-banner');"
        "            var verText = document.getElementById('update-version');"
        "            verText.textContent = 'v' + d.data.current_version + ' → v' + d.data.available_version;"
        "            banner.style.display = 'block';"
        "        }"
        "    }).catch(function(e) { console.log('Update check failed:', e); });"
        "}"
        "checkForUpdates();"
        "</script>"
        "</body>"
        "</html>";

    // Build HTML with LCD dimensions injected
    // First, calculate the exact size needed for the injection
    int injection_len = snprintf(NULL, 0,
                                 "var LCD_MAX_WIDTH = %d;\n"
                                 "        var LCD_MAX_HEIGHT = %d;\n"
                                 "        ",
                                 LCD_MAX_WIDTH, LCD_MAX_HEIGHT);
    if (injection_len < 0) {
        ESP_LOGE(TAG, "Failed to calculate injection size");
        return ESP_FAIL;
    }
    
    // Calculate total buffer size needed
    size_t html_len = strlen(html);
    size_t html_buf_size = html_len + (size_t)injection_len + 1; // +1 for null terminator
    char *html_buf = malloc(html_buf_size);
    if (!html_buf) {
        ESP_LOGE(TAG, "Failed to allocate HTML buffer");
        return ESP_ERR_NO_MEM;
    }

    // Find the position where we need to inject LCD dimensions
    // Look for the script tag and inject variables right after it
    const char *script_start = strstr(html, "<script>");
    if (!script_start) {
        ESP_LOGE(TAG, "Could not find script tag in HTML");
        free(html_buf);
        return ESP_FAIL;
    }

    size_t before_script = script_start - html;
    size_t after_script = strlen("<script>");
    
    // Copy everything before script tag
    memcpy(html_buf, html, before_script);
    
    // Copy script tag
    memcpy(html_buf + before_script, script_start, after_script);
    
    // Inject LCD dimension variables right after <script>
    // Inject actual JavaScript code (not string literal format)
    size_t inject_pos = before_script + after_script;
    size_t available = html_buf_size - inject_pos;
    int injected = snprintf(html_buf + inject_pos, 
                           available,
                           "var LCD_MAX_WIDTH = %d;\n"
                           "        var LCD_MAX_HEIGHT = %d;\n"
                           "        ",
                           LCD_MAX_WIDTH, LCD_MAX_HEIGHT);
    
    if (injected < 0 || (size_t)injected >= available) {
        ESP_LOGE(TAG, "Failed to inject LCD dimensions (injected=%d, available=%zu)", 
                 injected, available);
        free(html_buf);
        return ESP_FAIL;
    }
    
    // Copy the rest of the HTML (skip the original script tag since we already copied it)
    const char *rest_start = script_start + after_script;
    size_t rest_len = strlen(rest_start);
    size_t current_pos = inject_pos + injected;
    size_t total_needed = current_pos + rest_len + 1; // +1 for null terminator
    
    if (total_needed > html_buf_size) {
        ESP_LOGE(TAG, "HTML buffer too small (needed=%zu, available=%zu)", 
                 total_needed, html_buf_size);
        free(html_buf);
        return ESP_FAIL;
    }
    
    memcpy(html_buf + current_pos, rest_start, rest_len);
    html_buf[current_pos + rest_len] = '\0';

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_buf, strlen(html_buf));
    free(html_buf);
    ESP_LOGI(TAG, "Remote control page sent");
    return ESP_OK;
}

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

/**
 * POST /upload
 * Handles multipart/form-data file upload, saves to /sdcard/downloads, then moves to /sdcard/animations
 * Maximum file size: 5 MB
 * Supported formats: WebP, GIF, JPG, JPEG, PNG
 */
static esp_err_t h_post_upload(httpd_req_t *req) {
    const size_t MAX_FILE_SIZE = 5 * 1024 * 1024; // 5 MB
    const char *DOWNLOADS_DIR = "/sdcard/downloads";
    const char *ANIMATIONS_DIR = "/sdcard/animations";
    
    // Check Content-Type
    char content_type[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing Content-Type\",\"code\":\"MISSING_CONTENT_TYPE\"}");
        return ESP_OK;
    }
    
    // Check if multipart/form-data
    if (strstr(content_type, "multipart/form-data") == NULL) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"Unsupported Content-Type\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    bool sd_locked = animation_player_is_sd_export_locked();
    if (sd_locked) {
        // Drain request body to keep the HTTP connection consistent, then report busy
        size_t remaining = req->content_len;
        char drain_buf[128];
        while (remaining > 0) {
            size_t chunk = (remaining > sizeof(drain_buf)) ? sizeof(drain_buf) : remaining;
            int ret = httpd_req_recv(req, drain_buf, chunk);
            if (ret <= 0) {
                break;
            }
            remaining -= ret;
        }
        send_json(req, 423, "{\"ok\":false,\"error\":\"SD card shared over USB\",\"code\":\"SD_LOCKED\"}");
        return ESP_OK;
    }
    // Extract boundary from Content-Type
    const char *boundary_str = strstr(content_type, "boundary=");
    if (!boundary_str) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing boundary\",\"code\":\"MISSING_BOUNDARY\"}");
        return ESP_OK;
    }
    boundary_str += 9; // Skip "boundary="
    
    char boundary[128];
    size_t boundary_len = 0;
    while (boundary_str[boundary_len] != '\0' && boundary_str[boundary_len] != ';' && boundary_str[boundary_len] != ' ' && boundary_len < sizeof(boundary) - 1) {
        boundary[boundary_len] = boundary_str[boundary_len];
        boundary_len++;
    }
    boundary[boundary_len] = '\0';
    
    // Check Content-Length
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > MAX_FILE_SIZE) {
        send_json(req, 413, "{\"ok\":false,\"error\":\"File size exceeds 5MB limit\",\"code\":\"FILE_TOO_LARGE\"}");
        return ESP_OK;
    }
    
    struct stat st;
    // Ensure downloads directory exists
    if (stat(DOWNLOADS_DIR, &st) != 0) {
        ESP_LOGI(TAG, "Creating downloads directory: %s", DOWNLOADS_DIR);
        if (mkdir(DOWNLOADS_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create downloads directory: %s", strerror(errno));
            send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to create downloads directory\",\"code\":\"DIR_CREATE_FAIL\"}");
            return ESP_OK;
        }
    }

    // Ensure animations directory exists
    if (stat(ANIMATIONS_DIR, &st) != 0) {
        ESP_LOGI(TAG, "Creating animations directory: %s", ANIMATIONS_DIR);
        if (mkdir(ANIMATIONS_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create animations directory: %s", strerror(errno));
            send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to create animations directory\",\"code\":\"DIR_CREATE_FAIL\"}");
            return ESP_OK;
        }
    }
    
    // Temporary file path in downloads directory
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/upload_%llu.tmp", DOWNLOADS_DIR, (unsigned long long)(esp_timer_get_time() / 1000));
    
FILE *fp = NULL;
if (!sd_locked) {
    fp = fopen(temp_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open temp file for writing: %s", strerror(errno));
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to open file\",\"code\":\"FILE_OPEN_FAIL\"}");
        return ESP_OK;
    }
}

// Build boundary strings (without leading \r\n for initial boundary detection)
// boundary is max 128 bytes, so we need 2 + 128 = 130 bytes for "--" + boundary
char boundary_marker[130];
snprintf(boundary_marker, sizeof(boundary_marker), "--%s", boundary);
size_t boundary_marker_len = strlen(boundary_marker);

// boundary_line needs 4 + 128 = 132 bytes for "\r\n--" + boundary
char boundary_line[132];
snprintf(boundary_line, sizeof(boundary_line), "\r\n--%s", boundary);
size_t boundary_line_len = strlen(boundary_line);

// boundary_end needs 6 + 128 = 134 bytes for "\r\n--" + boundary + "--"
char boundary_end[134];
snprintf(boundary_end, sizeof(boundary_end), "\r\n--%s--", boundary);
size_t boundary_end_len = strlen(boundary_end);
    
    // Buffer for reading - need extra space for boundary matching across chunks
    const size_t BUF_SIZE = RECV_CHUNK + boundary_line_len + 16; // Extra space for overlap
    char *recv_buf = malloc(BUF_SIZE);
    if (!recv_buf) {
        if (fp) {
            fclose(fp);
            unlink(temp_path);
        }
        send_json(req, 500, "{\"ok\":false,\"error\":\"Out of memory\",\"code\":\"OOM\"}");
        return ESP_OK;
    }
    
    size_t total_received = 0;
    bool found_filename = false;
    char filename[256] = {0};
    
    // State machine for multipart parsing
    enum {
        STATE_FIND_INITIAL_BOUNDARY,
        STATE_READ_HEADERS,
        STATE_STREAM_FILE_DATA,
        STATE_DONE
    } state = STATE_FIND_INITIAL_BOUNDARY;
    
    size_t buf_len = 0;  // Total valid data in recv_buf
    bool boundary_found = false;
    
    while ((total_received < content_len || buf_len > 0) && state != STATE_DONE) {
        // Read more data if buffer has space and we haven't received all content
        if (buf_len < BUF_SIZE - 1 && total_received < content_len) {
            int recv_len = httpd_req_recv(req, recv_buf + buf_len, BUF_SIZE - buf_len - 1);
            if (recv_len <= 0) {
                if (recv_len < 0) {
                    ESP_LOGE(TAG, "Error receiving data: %d", recv_len);
                    break;
                }
                // recv_len == 0 means connection closed, but continue processing buffer
            } else {
                total_received += recv_len;
                buf_len += recv_len;
            }
        }
        
        if (state == STATE_FIND_INITIAL_BOUNDARY) {
            // Look for initial boundary: "--boundary\r\n" (no leading \r\n)
            // Must be at the start of the buffer
            if (buf_len >= boundary_marker_len + 2) {
                if (memcmp(recv_buf, boundary_marker, boundary_marker_len) == 0) {
                    // Found boundary marker, check for \r\n after it
                    if (recv_buf[boundary_marker_len] == '\r' && 
                        recv_buf[boundary_marker_len + 1] == '\n') {
                        // Skip boundary line: "--boundary\r\n"
                        size_t skip = boundary_marker_len + 2;
                        memmove(recv_buf, recv_buf + skip, buf_len - skip);
                        buf_len -= skip;
                        state = STATE_READ_HEADERS;
                        ESP_LOGD(TAG, "Found initial boundary");
                    }
                } else {
                    // Not a valid boundary, skip one byte and try again
                    memmove(recv_buf, recv_buf + 1, buf_len - 1);
                    buf_len--;
                }
            } else {
                // Not enough data yet, wait for more
                if (buf_len >= BUF_SIZE - 1) {
                    ESP_LOGE(TAG, "Boundary not found, buffer full");
                    break;
                }
            }
        } else if (state == STATE_READ_HEADERS) {
            // Look for end of headers: \r\n\r\n
            // Headers are text, so we can use strstr safely here
            char *header_end = NULL;
            for (size_t i = 0; i + 3 < buf_len; i++) {
                if (recv_buf[i] == '\r' && recv_buf[i+1] == '\n' && 
                    recv_buf[i+2] == '\r' && recv_buf[i+3] == '\n') {
                    header_end = recv_buf + i;
                    break;
                }
            }
            
            if (header_end) {
                size_t header_end_pos = header_end - recv_buf;
                
                // Extract filename from headers (headers are text, safe to null-terminate temporarily)
                char save_char = recv_buf[header_end_pos];
                recv_buf[header_end_pos] = '\0';
                
                char *cd = strstr(recv_buf, "Content-Disposition:");
                if (cd) {
                    char *fn_start = strstr(cd, "filename=\"");
                    if (fn_start) {
                        fn_start += 10; // Skip "filename=\""
                        char *fn_end = strchr(fn_start, '"');
                        if (fn_end) {
                            size_t fn_len = fn_end - fn_start;
                            if (fn_len < sizeof(filename)) {
                                memcpy(filename, fn_start, fn_len);
                                filename[fn_len] = '\0';
                                found_filename = true;
                            }
                        }
                    }
                }
                
                recv_buf[header_end_pos] = save_char; // Restore
                
                // Skip headers: header_end points to \r\n\r\n, skip all 4 bytes
                size_t skip = header_end_pos + 4;
                memmove(recv_buf, recv_buf + skip, buf_len - skip);
                buf_len -= skip;
                state = STATE_STREAM_FILE_DATA;
                ESP_LOGD(TAG, "Headers parsed, starting file data");
            } else {
                // Headers not complete yet
                if (buf_len >= 2048) {
                    ESP_LOGE(TAG, "Headers too long or malformed");
                    break;
                }
            }
        } else if (state == STATE_STREAM_FILE_DATA) {
            // Stream file data until we find a boundary
            // Boundaries are: \r\n--boundary or \r\n--boundary--
            // We need to check for boundaries that might be split across chunks
            // The overlap buffer ensures boundaries split across recv() calls are detected
            
            size_t write_end = buf_len;
            bool found_boundary = false;
            
            // Look for boundary_end first (more specific)
            if (buf_len >= boundary_end_len) {
                for (size_t i = 0; i <= buf_len - boundary_end_len; i++) {
                    if (memcmp(recv_buf + i, boundary_end, boundary_end_len) == 0) {
                        // Found end boundary: \r\n--boundary--
                        // File data ends before the \r\n
                        write_end = i;
                        found_boundary = true;
                        boundary_found = true;
                        ESP_LOGD(TAG, "Found end boundary at position %zu", i);
                        break;
                    }
                }
            }
            
            // If not found, look for regular boundary
            if (!found_boundary && buf_len >= boundary_line_len) {
                for (size_t i = 0; i <= buf_len - boundary_line_len; i++) {
                    if (memcmp(recv_buf + i, boundary_line, boundary_line_len) == 0) {
                        // Found boundary: \r\n--boundary
                        // File data ends before the \r\n
                        write_end = i;
                        found_boundary = true;
                        boundary_found = true;
                        ESP_LOGD(TAG, "Found boundary at position %zu", i);
                        break;
                    }
                }
            }
            
            if (found_boundary) {
                // Write file data up to (but not including) the boundary
                if (write_end > 0) {
                    size_t written = fwrite(recv_buf, 1, write_end, fp);
                    if (written != write_end) {
                        ESP_LOGE(TAG, "Failed to write file data");
                        break;
                    }
                }
                state = STATE_DONE;
            } else {
                // No boundary found in current buffer
                // Check if we've received all content - if so, boundary must be here or missing
                if (total_received >= content_len) {
                    // We've read all content but haven't found boundary
                    // This shouldn't happen, but try to write remaining data as file content
                    ESP_LOGW(TAG, "End of content reached but boundary not found, buf_len=%zu", buf_len);
                    if (buf_len > 0) {
                        // Write remaining data - might be incomplete
                        size_t written = fwrite(recv_buf, 1, buf_len, fp);
                        if (written != buf_len) {
                            ESP_LOGE(TAG, "Failed to write file data");
                        }
                        buf_len = 0;
                    }
                    // Mark as found to avoid error, but file might be incomplete
                    boundary_found = true;
                    state = STATE_DONE;
                } else {
                    // Write data but keep enough for boundary detection overlap
                    size_t safe_write_len = 0;
                    if (buf_len > boundary_line_len) {
                        safe_write_len = buf_len - boundary_line_len;
                        size_t written = fwrite(recv_buf, 1, safe_write_len, fp);
                        if (written != safe_write_len) {
                            ESP_LOGE(TAG, "Failed to write file data");
                            break;
                        }
                        // Move remaining data to start of buffer
                        memmove(recv_buf, recv_buf + safe_write_len, buf_len - safe_write_len);
                        buf_len -= safe_write_len;
                    } else if (buf_len == BUF_SIZE - 1 && total_received < content_len) {
                        // Buffer is full but we haven't read all content - this is an error
                        ESP_LOGE(TAG, "Buffer full but boundary not found, cannot continue");
                        break;
                    }
                    // If buf_len <= boundary_line_len and we haven't read all content, 
                    // loop will continue to read more data
                }
            }
        }
    }
    
    fclose(fp);
    free(recv_buf);
    
    // Validate that we found the boundary and filename
    if (!boundary_found) {
        unlink(temp_path);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Boundary not found or incomplete upload\",\"code\":\"MALFORMED_DATA\"}");
        return ESP_OK;
    }
    
    if (!found_filename || strlen(filename) == 0) {
        if (fp) {
            unlink(temp_path);
        }
        send_json(req, 400, "{\"ok\":false,\"error\":\"No filename in upload\",\"code\":\"NO_FILENAME\"}");
        return ESP_OK;
    }
    
    // Validate file extension
    const char *ext = strrchr(filename, '.');
    if (!ext) {
        unlink(temp_path);
        send_json(req, 400, "{\"ok\":false,\"error\":\"File must have an extension\",\"code\":\"INVALID_EXTENSION\"}");
        return ESP_OK;
    }
    ext++; // Skip the dot
    
    bool valid_ext = false;
    if (strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0) {
        valid_ext = true;
    }
    
    if (!valid_ext) {
        if (fp) {
            unlink(temp_path);
        }
        send_json(req, 400, "{\"ok\":false,\"error\":\"Unsupported file type. Use WebP, GIF, JPG, JPEG, or PNG\",\"code\":\"UNSUPPORTED_TYPE\"}");
        return ESP_OK;
    }
    
    // Final destination path in animations directory
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", ANIMATIONS_DIR, filename);
    
    // Check if file already exists, delete it if it does
    if (stat(final_path, &st) == 0) {
        ESP_LOGI(TAG, "File %s already exists, deleting old version", filename);
        if (unlink(final_path) != 0) {
            ESP_LOGW(TAG, "Failed to delete existing file %s: %s", final_path, strerror(errno));
            // Continue anyway - try to overwrite with rename
        }
    }
    
    // Move file from temp location to final location
    if (rename(temp_path, final_path) != 0) {
        ESP_LOGE(TAG, "Failed to move file: %s", strerror(errno));
        unlink(temp_path);
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to save file\",\"code\":\"FILE_SAVE_FAIL\"}");
        return ESP_OK;
    }
    
    // Use the original filename (no suffix needed)
    const char *final_filename = filename;
    
    ESP_LOGI(TAG, "File uploaded successfully: %s", final_filename);
    
    // Get current playing index to insert after it
    // Returns SIZE_MAX if nothing is playing, which will cause insertion at index 0
    size_t current_index = animation_player_get_current_index();
    
    // Add file to animation list immediately after currently playing artwork
    // If nothing is playing (current_index == SIZE_MAX), insert at index 0
    size_t new_index = 0;
    esp_err_t add_err = animation_player_add_file(final_filename, ANIMATIONS_DIR, current_index, &new_index);
    if (add_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add file to animation list: %s", esp_err_to_name(add_err));
        // File is saved, but couldn't add to list - still return success
        char json_resp[512];
        snprintf(json_resp, sizeof(json_resp), "{\"ok\":true,\"data\":{\"filename\":\"%s\",\"warning\":\"File saved but not added to list\"}}", final_filename);
        send_json(req, 200, json_resp);
        return ESP_OK;
    }
    
    // Swap to the newly added file
    esp_err_t swap_err = animation_player_swap_to_index(new_index);
    if (swap_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to swap to new file (index %zu): %s", new_index, esp_err_to_name(swap_err));
        // File is added but couldn't swap - still return success
        char json_resp[512];
        snprintf(json_resp, sizeof(json_resp), "{\"ok\":true,\"data\":{\"filename\":\"%s\",\"index\":%zu,\"message\":\"File uploaded and added to animation list\"}}", final_filename, new_index);
        send_json(req, 200, json_resp);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Successfully uploaded, added, and swapped to file %s at index %zu", final_filename, new_index);
    char json_resp[512];
    snprintf(json_resp, sizeof(json_resp), "{\"ok\":true,\"data\":{\"filename\":\"%s\",\"index\":%zu,\"message\":\"File uploaded, added to list, and displayed\"}}", final_filename, new_index);
    send_json(req, 200, json_resp);
    return ESP_OK;
}

// ---------- OTA Handlers ----------

/**
 * OTA UI callback - controls animation player and LCD during OTA
 */
static void ota_ui_callback(bool enter, const char *version_from, const char *version_to) {
    if (enter) {
        animation_player_enter_ui_mode();
        ugfx_ui_show_ota_progress(version_from, version_to);
    } else {
        ugfx_ui_hide_ota_progress();
        animation_player_exit_ui_mode();
    }
}

/**
 * OTA progress callback - updates LCD progress display
 */
static void ota_progress_callback(int percent, const char *status_text) {
    ugfx_ui_update_ota_progress(percent, status_text);
}

/**
 * GET /ota/status
 * Returns current OTA status including version info and update availability
 */
static esp_err_t h_get_ota_status(httpd_req_t *req) {
    ota_status_t status;
    esp_err_t err = ota_manager_get_status(&status);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to get OTA status\",\"code\":\"OTA_STATUS_FAIL\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        if (root) cJSON_Delete(root);
        if (data) cJSON_Delete(data);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    
    cJSON_AddStringToObject(data, "state", ota_state_to_string(status.state));
    cJSON_AddStringToObject(data, "current_version", status.current_version);
    
    if (strlen(status.available_version) > 0) {
        cJSON_AddStringToObject(data, "available_version", status.available_version);
        cJSON_AddNumberToObject(data, "available_size", status.available_size);
        if (strlen(status.release_notes) > 0) {
            cJSON_AddStringToObject(data, "release_notes", status.release_notes);
        }
    } else {
        cJSON_AddNullToObject(data, "available_version");
        cJSON_AddNullToObject(data, "available_size");
    }
    
    if (status.last_check_time > 0) {
        cJSON_AddNumberToObject(data, "last_check", (double)status.last_check_time);
    } else {
        cJSON_AddNullToObject(data, "last_check");
    }
    
    if (status.state == OTA_STATE_DOWNLOADING) {
        cJSON_AddNumberToObject(data, "download_progress", status.download_progress);
    } else {
        cJSON_AddNullToObject(data, "download_progress");
    }
    
    if (status.state == OTA_STATE_ERROR && strlen(status.error_message) > 0) {
        cJSON_AddStringToObject(data, "error_message", status.error_message);
    } else {
        cJSON_AddNullToObject(data, "error_message");
    }
    
    cJSON_AddBoolToObject(data, "can_rollback", status.can_rollback);
    if (status.can_rollback && strlen(status.rollback_version) > 0) {
        cJSON_AddStringToObject(data, "rollback_version", status.rollback_version);
    } else {
        cJSON_AddNullToObject(data, "rollback_version");
    }
    
    // Dev mode info
    cJSON_AddBoolToObject(data, "dev_mode", status.dev_mode);
    cJSON_AddBoolToObject(data, "is_prerelease", status.is_prerelease);
    
    cJSON_AddItemToObject(root, "data", data);
    
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
 * POST /ota/check
 * Triggers an immediate update check
 */
static esp_err_t h_post_ota_check(httpd_req_t *req) {
    esp_err_t err = ota_manager_check_for_update();
    
    if (err == ESP_ERR_INVALID_STATE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Check already in progress\",\"code\":\"CHECK_IN_PROGRESS\"}");
        return ESP_OK;
    }
    
    if (err != ESP_OK) {
        char response[128];
        snprintf(response, sizeof(response), 
                 "{\"ok\":false,\"error\":\"Failed to start check: %s\",\"code\":\"CHECK_FAIL\"}", 
                 esp_err_to_name(err));
        send_json(req, 500, response);
        return ESP_OK;
    }
    
    send_json(req, 202, "{\"ok\":true,\"data\":{\"checking\":true,\"message\":\"Update check started\"}}");
    return ESP_OK;
}

/**
 * POST /ota/install
 * Starts firmware installation (device will reboot on success)
 */
static esp_err_t h_post_ota_install(httpd_req_t *req) {
    // Check if update is available
    ota_state_t state = ota_manager_get_state();
    if (state != OTA_STATE_UPDATE_AVAILABLE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"No update available\",\"code\":\"NO_UPDATE\"}");
        return ESP_OK;
    }
    
    // Check blockers
    const char *block_reason;
    if (ota_manager_is_blocked(&block_reason)) {
        char response[256];
        snprintf(response, sizeof(response), 
                 "{\"ok\":false,\"error\":\"%s\",\"code\":\"OTA_BLOCKED\"}", 
                 block_reason);
        send_json(req, 423, response);
        return ESP_OK;
    }
    
    // Send response before starting (we'll reboot after)
    send_json(req, 202, "{\"ok\":true,\"data\":{\"installing\":true,\"message\":\"Firmware update started. Device will reboot when complete.\"}}");
    
    // Small delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Start installation (this will reboot on success)
    // Pass callbacks for progress and UI control
    esp_err_t err = ota_manager_install_update(ota_progress_callback, ota_ui_callback);
    
    // If we get here, installation failed
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA install failed: %s", esp_err_to_name(err));
        // Can't send response - connection likely closed
    }
    
    return ESP_OK;
}

/**
 * POST /ota/rollback
 * Schedules rollback to previous firmware and reboots
 */
static esp_err_t h_post_ota_rollback(httpd_req_t *req) {
    ota_status_t status;
    ota_manager_get_status(&status);
    
    if (!status.can_rollback) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"No rollback available\",\"code\":\"NO_ROLLBACK\"}");
        return ESP_OK;
    }
    
    // Send response before rebooting
    char response[256];
    snprintf(response, sizeof(response), 
             "{\"ok\":true,\"data\":{\"rolling_back\":true,\"target_version\":\"%s\",\"message\":\"Rolling back. Device will reboot.\"}}",
             status.rollback_version);
    send_json(req, 202, response);
    
    // Small delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Perform rollback (this will reboot)
    esp_err_t err = ota_manager_rollback();
    
    // If we get here, rollback failed
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(err));
    }
    
    return ESP_OK;
}

/**
 * GET /ota
 * Returns the OTA update web UI page
 */
static esp_err_t h_get_ota_page(httpd_req_t *req) {
    static const char ota_html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">"
        "<title>p3a - Firmware Update</title>"
        "<style>"
        "* { box-sizing: border-box; }"
        "body {"
        "    margin: 0;"
        "    padding: 16px;"
        "    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "    min-height: 100vh;"
        "    display: flex;"
        "    flex-direction: column;"
        "    align-items: center;"
        "}"
        ".container { width: min(420px, 100%); }"
        "h1 {"
        "    text-align: center;"
        "    color: white;"
        "    font-size: 1.5rem;"
        "    font-weight: 300;"
        "    margin: 0 0 16px;"
        "    text-shadow: 0 2px 4px rgba(0,0,0,0.2);"
        "}"
        ".dev-badge {"
        "    display: inline-block;"
        "    background: #ff9800;"
        "    color: #000;"
        "    padding: 3px 8px;"
        "    border-radius: 4px;"
        "    font-size: 0.65rem;"
        "    font-weight: bold;"
        "    margin-left: 8px;"
        "    vertical-align: middle;"
        "}"
        ".card {"
        "    background: rgba(255,255,255,0.95);"
        "    border-radius: 16px;"
        "    padding: 16px;"
        "    margin-bottom: 12px;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "}"
        ".version-row {"
        "    display: flex;"
        "    justify-content: space-between;"
        "    padding: 10px 0;"
        "    border-bottom: 1px solid #eee;"
        "}"
        ".version-row:last-child { border-bottom: none; }"
        ".version-label { color: #666; font-size: 0.9rem; }"
        ".version-value { color: #333; font-weight: 500; font-size: 0.9rem; }"
        ".update-available { color: #4CAF50; font-weight: 600; }"
        "button {"
        "    width: 100%;"
        "    padding: 14px;"
        "    border: none;"
        "    border-radius: 12px;"
        "    font-size: 0.95rem;"
        "    font-weight: 500;"
        "    cursor: pointer;"
        "    transition: all 0.2s;"
        "    margin-bottom: 10px;"
        "}"
        ".btn-primary {"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "    color: white;"
        "    box-shadow: 0 4px 12px rgba(102,126,234,0.4);"
        "}"
        ".btn-primary:active { transform: scale(0.98); }"
        ".btn-primary:disabled {"
        "    background: #ccc;"
        "    color: #888;"
        "    cursor: not-allowed;"
        "    box-shadow: none;"
        "}"
        ".btn-danger {"
        "    background: #ff6b6b;"
        "    color: white;"
        "    box-shadow: 0 4px 12px rgba(255,107,107,0.3);"
        "}"
        ".btn-danger:active { transform: scale(0.98); }"
        ".progress-container {"
        "    display: none;"
        "    background: rgba(255,255,255,0.95);"
        "    border-radius: 12px;"
        "    padding: 16px;"
        "    margin-bottom: 12px;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "}"
        ".progress-bar {"
        "    height: 10px;"
        "    background: #e0e0e0;"
        "    border-radius: 5px;"
        "    overflow: hidden;"
        "}"
        ".progress-fill {"
        "    height: 100%;"
        "    background: linear-gradient(90deg, #667eea, #764ba2);"
        "    transition: width 0.3s;"
        "    width: 0%;"
        "}"
        ".progress-text {"
        "    text-align: center;"
        "    margin-top: 8px;"
        "    color: #666;"
        "    font-size: 0.85rem;"
        "}"
        ".status-message {"
        "    text-align: center;"
        "    padding: 12px;"
        "    border-radius: 8px;"
        "    margin-bottom: 12px;"
        "    display: none;"
        "    font-size: 0.9rem;"
        "}"
        ".status-success { background: #e8f5e9; color: #2e7d32; }"
        ".status-error { background: #ffebee; color: #c62828; }"
        ".status-info { background: #e3f2fd; color: #1565c0; }"
        ".release-notes {"
        "    max-height: 150px;"
        "    overflow-y: auto;"
        "    font-size: 0.8rem;"
        "    white-space: pre-wrap;"
        "    background: #f5f5f5;"
        "    padding: 12px;"
        "    border-radius: 8px;"
        "    margin-top: 12px;"
        "    display: none;"
        "    color: #333;"
        "}"
        ".back-link {"
        "    display: block;"
        "    text-align: center;"
        "    color: rgba(255,255,255,0.8);"
        "    text-decoration: none;"
        "    margin-top: 16px;"
        "    font-size: 0.9rem;"
        "}"
        ".back-link:active { color: white; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"container\">"
        "<h1>Firmware Update<span class=\"dev-badge\" id=\"dev-badge\" style=\"display:none\">DEV</span></h1>"
        "<div class=\"status-message\" id=\"status-msg\"></div>"
        "<div class=\"card\">"
        "<div class=\"version-row\"><span class=\"version-label\">Current Version</span><span class=\"version-value\" id=\"current-ver\">-</span></div>"
        "<div class=\"version-row\"><span class=\"version-label\">Available</span><span class=\"version-value\" id=\"available-ver\">-</span></div>"
        "<div class=\"version-row\"><span class=\"version-label\">Status</span><span class=\"version-value\" id=\"state\">-</span></div>"
        "</div>"
        "<div class=\"progress-container\" id=\"progress\">"
        "<div class=\"progress-bar\"><div class=\"progress-fill\" id=\"progress-fill\"></div></div>"
        "<div class=\"progress-text\" id=\"progress-text\">0%</div>"
        "</div>"
        "<button class=\"btn-primary\" id=\"check-btn\" onclick=\"checkUpdate()\">Check for Updates</button>"
        "<button class=\"btn-primary\" id=\"install-btn\" onclick=\"installUpdate()\" disabled>Install Update</button>"
        "<button class=\"btn-danger\" id=\"rollback-btn\" onclick=\"rollback()\" style=\"display:none\">Rollback to Previous</button>"
        "<div class=\"release-notes\" id=\"release-notes\"></div>"
        "<a href=\"/\" class=\"back-link\">&#8592; Back to Home</a>"
        "</div>"
        "<script>"
        "var pollInterval = null;"
        "function showStatus(msg, type) {"
        "    var el = document.getElementById('status-msg');"
        "    el.textContent = msg;"
        "    el.className = 'status-message status-' + type;"
        "    el.style.display = 'block';"
        "    if (type !== 'info') setTimeout(function() { el.style.display = 'none'; }, 5000);"
        "}"
        "function updateUI(data) {"
        "    document.getElementById('current-ver').textContent = data.current_version || '-';"
        "    var availVerEl = document.getElementById('available-ver');"
        "    var verText = data.available_version || 'Up to date';"
        "    if (data.available_version && data.is_prerelease) verText += ' (pre-release)';"
        "    availVerEl.textContent = verText;"
        "    availVerEl.className = 'version-value' + (data.available_version ? ' update-available' : '');"
        "    document.getElementById('state').textContent = data.state.replace('_', ' ');"
        "    document.getElementById('dev-badge').style.display = data.dev_mode ? 'inline-block' : 'none';"
        "    var checkBtn = document.getElementById('check-btn');"
        "    var installBtn = document.getElementById('install-btn');"
        "    var rollbackBtn = document.getElementById('rollback-btn');"
        "    var progressEl = document.getElementById('progress');"
        "    checkBtn.disabled = (data.state === 'checking' || data.state === 'downloading');"
        "    installBtn.disabled = (data.state !== 'update_available');"
        "    installBtn.style.display = (data.state === 'downloading' || data.state === 'verifying') ? 'none' : 'block';"
        "    rollbackBtn.style.display = data.can_rollback ? 'block' : 'none';"
        "    if (data.can_rollback) rollbackBtn.textContent = 'Rollback to ' + (data.rollback_version || 'Previous');"
        "    if (data.state === 'downloading' || data.state === 'verifying') {"
        "        progressEl.style.display = 'block';"
        "        var pct = data.download_progress || 0;"
        "        document.getElementById('progress-fill').style.width = pct + '%';"
        "        document.getElementById('progress-text').textContent = data.state === 'verifying' ? 'Verifying...' : pct + '%';"
        "    } else {"
        "        progressEl.style.display = 'none';"
        "    }"
        "    var notesEl = document.getElementById('release-notes');"
        "    if (data.release_notes && data.available_version) {"
        "        notesEl.textContent = data.release_notes;"
        "        notesEl.style.display = 'block';"
        "    } else {"
        "        notesEl.style.display = 'none';"
        "    }"
        "    if (data.error_message) showStatus(data.error_message, 'error');"
        "}"
        "function fetchStatus() {"
        "    fetch('/ota/status').then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok) updateUI(d.data);"
        "    }).catch(function(e) { console.error('Status fetch error:', e); });"
        "}"
        "function checkUpdate() {"
        "    showStatus('Checking for updates...', 'info');"
        "    fetch('/ota/check', { method: 'POST' }).then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok) {"
        "            startPolling();"
        "        } else {"
        "            showStatus(d.error || 'Check failed', 'error');"
        "        }"
        "    }).catch(function(e) { showStatus('Network error', 'error'); });"
        "}"
        "function installUpdate() {"
        "    if (!confirm('Install firmware update? The device will reboot.')) return;"
        "    showStatus('Starting update...', 'info');"
        "    fetch('/ota/install', { method: 'POST' }).then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok) {"
        "            showStatus('Update in progress. Device will reboot...', 'info');"
        "            startPolling();"
        "        } else {"
        "            showStatus(d.error || 'Install failed', 'error');"
        "        }"
        "    }).catch(function(e) { showStatus('Network error', 'error'); });"
        "}"
        "function rollback() {"
        "    if (!confirm('Roll back to previous firmware? The device will reboot.')) return;"
        "    fetch('/ota/rollback', { method: 'POST' }).then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok) showStatus('Rolling back...', 'info');"
        "        else showStatus(d.error || 'Rollback failed', 'error');"
        "    }).catch(function(e) { showStatus('Network error', 'error'); });"
        "}"
        "function startPolling() {"
        "    if (pollInterval) clearInterval(pollInterval);"
        "    pollInterval = setInterval(fetchStatus, 1000);"
        "    setTimeout(function() { if (pollInterval) { clearInterval(pollInterval); pollInterval = null; } }, 120000);"
        "}"
        "fetchStatus();"
        "setInterval(fetchStatus, 5000);"
        "</script>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ota_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------- mDNS Setup ----------

static esp_err_t start_mdns(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set("p3a");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set("p3a");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS started: p3a.local");
    return ESP_OK;
}

// ---------- Start/Stop ----------

esp_err_t http_api_start(void) {
    // Create command queue if not exists
    if (!s_cmdq) {
        s_cmdq = xQueueCreate(QUEUE_LEN, sizeof(command_t));
        if (!s_cmdq) {
            ESP_LOGE(TAG, "Failed to create command queue");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Command queue created (length=%d)", QUEUE_LEN);
    }

    // Create worker task if not exists
    if (!s_worker) {
        BaseType_t ret = xTaskCreate(api_worker_task, "api_worker", 4096, NULL, 5, &s_worker);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create worker task");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Worker task created");
    }

    // Start mDNS
    esp_err_t e = start_mdns();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "mDNS start failed (continuing anyway): %s", esp_err_to_name(e));
    }

#if CONFIG_P3A_PICO8_ENABLE
    // Initialize PICO-8 stream parser (always, not just for USB)
    esp_err_t stream_init_ret = pico8_stream_init();
    if (stream_init_ret != ESP_OK) {
        ESP_LOGW(TAG, "PICO-8 stream init failed: %s (continuing anyway)", esp_err_to_name(stream_init_ret));
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
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Register URI handlers
    httpd_uri_t u = {0};

    u.uri = "/favicon.ico";
    u.method = HTTP_GET;
    u.handler = h_get_favicon;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/";
    u.method = HTTP_GET;
    u.handler = h_get_root;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/config/network";
    u.method = HTTP_GET;
    u.handler = h_get_network_config;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/erase";
    u.method = HTTP_POST;
    u.handler = h_post_erase;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

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

    u.uri = "/upload";
    u.method = HTTP_POST;
    u.handler = h_post_upload;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

#if CONFIG_P3A_PICO8_ENABLE
    u.uri = "/pico8";
    u.method = HTTP_GET;
    u.handler = h_get_pico8;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);
#endif

    u.uri = "/static/*";
    u.method = HTTP_GET;
    u.handler = h_get_static;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    // OTA update endpoints
    u.uri = "/ota";
    u.method = HTTP_GET;
    u.handler = h_get_ota_page;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/ota/status";
    u.method = HTTP_GET;
    u.handler = h_get_ota_status;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/ota/check";
    u.method = HTTP_POST;
    u.handler = h_post_ota_check;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/ota/install";
    u.method = HTTP_POST;
    u.handler = h_post_ota_install;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

    u.uri = "/ota/rollback";
    u.method = HTTP_POST;
    u.handler = h_post_ota_rollback;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

#if CONFIG_P3A_PICO8_ENABLE
    // WebSocket endpoint for PICO-8 streaming
    httpd_uri_t ws_uri = {
        .uri = "/pico_stream",
        .method = HTTP_GET,
        .handler = h_ws_pico_stream,
        .user_ctx = NULL,
        .is_websocket = true
    };
    register_uri_handler_or_log(s_server, &ws_uri);
#endif

    ESP_LOGI(TAG, "HTTP API server started on port 80");
    return ESP_OK;
}

esp_err_t http_api_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP API server stopped");
    }
    // Worker task and queue remain active for simplicity
    return ESP_OK;
}

