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
#include "bsp/esp-bsp.h"
#include "animation_player.h"
#include "app_lcd.h"
#include "favicon_data.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "HTTP";

#define MAX_JSON (32 * 1024)
#define RECV_CHUNK 4096
#define QUEUE_LEN 10

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

// ---------- Callback Registration ----------

void http_api_set_action_handlers(action_callback_t swap_next, action_callback_t swap_back) {
    s_swap_next_callback = swap_next;
    s_swap_back_callback = swap_back;
    ESP_LOGI(TAG, "Action handlers registered");
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
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">"
        "<title>p3a Status</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }"
        ".container { max-width: 600px; margin: 0 auto; background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
        "h1 { color: #333; text-align: center; margin-bottom: 30px; }"
        ".info-section { margin: 20px 0; padding: 15px; background-color: #f9f9f9; border-radius: 5px; }"
        ".info-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #eee; }"
        ".info-row:last-child { border-bottom: none; }"
        ".info-label { font-weight: bold; color: #555; }"
        ".info-value { color: #333; }"
        ".status-badge { display: inline-block; padding: 4px 12px; border-radius: 12px; font-size: 0.85em; font-weight: bold; }"
        ".status-connected { background-color: #4CAF50; color: white; }"
        ".status-disconnected { background-color: #f44336; color: white; }"
        ".erase-section { margin-top: 30px; padding-top: 20px; border-top: 2px solid #eee; }"
        ".erase-btn { background-color: #f44336; color: white; padding: 12px 24px; border: none; border-radius: 5px; cursor: pointer; width: 100%; font-size: 16px; font-weight: bold; }"
        ".erase-btn:hover { background-color: #da190b; }"
        ".erase-btn:active { background-color: #c1170a; }"
        ".warning { color: #f44336; font-size: 0.9em; margin-top: 10px; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"container\">"
        "<h1>p3a Pixel Art Player</h1>"
        "<div class=\"info-section\">"
        "<h2>Connection Status</h2>"
        "<div class=\"info-row\">"
        "<span class=\"info-label\">Status:</span>"
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
        "<div class=\"erase-section\">"
        "<form action=\"/erase\" method=\"POST\" onsubmit=\"return confirm('Are you sure you want to erase the Wi-Fi credentials? The device will reboot and enter configuration mode.');\">"
        "<button type=\"submit\" class=\"erase-btn\">Erase Wi-Fi Credentials & Reboot</button>"
        "</form>"
        "<p class=\"warning\">Warning: This will erase the saved Wi-Fi network credentials. The device will reboot and start a configuration access point.</p>"
        "</div>"
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
        "    padding: 0;"
        "    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "    min-height: 100vh;"
        "    display: flex;"
        "    flex-direction: column;"
        "    overflow-x: hidden;"
        "}"
        ".header {"
        "    text-align: center;"
        "    padding: 16px 12px 12px;"
        "    color: white;"
        "}"
        ".header h1 {"
        "    margin: 0;"
        "    font-size: 2.5rem;"
        "    font-weight: 300;"
        "    letter-spacing: 0.1em;"
        "    text-transform: lowercase;"
        "}"
        ".controls {"
        "    flex: 1;"
        "    display: flex;"
        "    flex-direction: column;"
        "    align-items: center;"
        "    justify-content: center;"
        "    padding: 12px;"
        "    gap: 20px;"
        "}"
        ".arrow-row {"
        "    display: flex;"
        "    gap: 24px;"
        "    align-items: center;"
        "}"
        ".arrow-btn {"
        "    background: rgba(255,255,255,0.95);"
        "    border: none;"
        "    border-radius: 50%;"
        "    width: 80px;"
        "    height: 80px;"
        "    display: flex;"
        "    align-items: center;"
        "    justify-content: center;"
        "    cursor: pointer;"
        "    font-size: 2rem;"
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
        "    padding: 12px 32px;"
        "    font-size: 1rem;"
        "    font-weight: 500;"
        "    color: #667eea;"
        "    cursor: pointer;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "    transition: all 0.2s ease;"
        "    -webkit-tap-highlight-color: transparent;"
        "    min-width: 120px;"
        "}"
        ".pause-btn:active {"
        "    transform: scale(0.95);"
        "}"
        ".footer {"
        "    padding: 12px;"
        "    display: flex;"
        "    justify-content: center;"
        "    gap: 12px;"
        "    flex-wrap: wrap;"
        "}"
        ".footer-btn {"
        "    background: rgba(255,255,255,0.2);"
        "    border: 1px solid rgba(255,255,255,0.3);"
        "    border-radius: 8px;"
        "    padding: 8px 16px;"
        "    font-size: 0.875rem;"
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
        "    top: 80px;"
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
        "    background: rgba(255,255,255,0.95);"
        "    border-radius: 12px;"
        "    padding: 16px;"
        "    margin: 0 12px 12px;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "}"
        ".upload-section h3 {"
        "    margin: 0 0 12px;"
        "    font-size: 0.875rem;"
        "    font-weight: 500;"
        "    color: #333;"
        "    text-transform: uppercase;"
        "    letter-spacing: 0.05em;"
        "}"
        ".upload-form {"
        "    display: flex;"
        "    flex-direction: column;"
        "    gap: 10px;"
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
        "    padding: 10px;"
        "    background: #667eea;"
        "    color: white;"
        "    border-radius: 8px;"
        "    text-align: center;"
        "    font-size: 0.875rem;"
        "    cursor: pointer;"
        "    transition: background 0.2s;"
        "}"
        ".file-input-label:active {"
        "    background: #5568d3;"
        "}"
        ".file-name {"
        "    font-size: 0.75rem;"
        "    color: #666;"
        "    word-break: break-all;"
        "    padding: 0 4px;"
        "}"
        ".upload-btn {"
        "    background: #4CAF50;"
        "    color: white;"
        "    border: none;"
        "    padding: 10px;"
        "    border-radius: 8px;"
        "    font-size: 0.875rem;"
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
        "    margin-top: 8px;"
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
        "    .header h1 { font-size: 2rem; }"
        "    .arrow-btn { width: 70px; height: 70px; font-size: 1.75rem; }"
        "    .arrow-row { gap: 20px; }"
        "    .pause-btn { padding: 10px 24px; font-size: 0.9rem; }"
        "}"
        "@media (min-width: 481px) {"
        "    .arrow-btn:hover { transform: scale(1.05); }"
        "    .pause-btn:hover { transform: scale(1.02); }"
        "    .footer-btn:hover { background: rgba(255,255,255,0.3); }"
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
        "<div class=\"footer\">"
        "    <button class=\"footer-btn\" onclick=\"window.location.href='/config/network'\">Network</button>"
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
        "fileInput.addEventListener('change', function(e) {"
        "    var file = e.target.files[0];"
        "    if (file) {"
        "        if (file.size > 5 * 1024 * 1024) {"
        "            fileName.textContent = 'File too large! Maximum size is 5MB.';"
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
        "    if (file.size > 5 * 1024 * 1024) {"
        "        statusDiv.textContent = 'File too large! Maximum size is 5MB.';"
        "        statusDiv.className = 'status error';"
        "        statusDiv.style.display = 'block';"
        "        setTimeout(function() { statusDiv.style.display = 'none'; }, 3000);"
        "        return;"
        "    }"
        "    var formData = new FormData();"
        "    formData.append('file', file);"
        "    uploadBtn.disabled = true;"
        "    uploadProgress.classList.add('active');"
        "    progressFill.style.width = '0%';"
        "    var xhr = new XMLHttpRequest();"
        "    xhr.open('POST', '/upload', true);"
        "    xhr.upload.onprogress = function(e) {"
        "        if (e.lengthComputable) {"
        "            var percentComplete = (e.loaded / e.total) * 100;"
        "            progressFill.style.width = percentComplete + '%';"
        "        }"
        "    };"
        "    xhr.onreadystatechange = function() {"
        "        if (xhr.readyState === 4) {"
        "            uploadBtn.disabled = false;"
        "            uploadProgress.classList.remove('active');"
        "            progressFill.style.width = '0%';"
        "            try {"
        "                var result = JSON.parse(xhr.responseText);"
        "                if (xhr.status >= 200 && xhr.status < 300 && result.ok) {"
        "                    statusDiv.textContent = 'Upload successful!';"
        "                    statusDiv.className = 'status success';"
        "                    fileInput.value = '';"
        "                    fileName.textContent = '';"
        "                } else {"
        "                    statusDiv.textContent = 'Upload failed: ' + (result.error || 'HTTP ' + xhr.status);"
        "                    statusDiv.className = 'status error';"
        "                }"
        "            } catch (e) {"
        "                statusDiv.textContent = 'Upload failed: ' + xhr.statusText;"
        "                statusDiv.className = 'status error';"
        "            }"
        "            statusDiv.style.display = 'block';"
        "            setTimeout(function() { statusDiv.style.display = 'none'; }, 5000);"
        "        }"
        "    };"
        "    xhr.send(formData);"
        "});"
        "</script>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
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
        cJSON_AddStringToObject(fw, "version", "1.0.0");
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
    
    // Ensure downloads directory exists
    struct stat st;
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
    
    FILE *fp = fopen(temp_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open temp file for writing: %s", strerror(errno));
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to open file\",\"code\":\"FILE_OPEN_FAIL\"}");
        return ESP_OK;
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
        fclose(fp);
        unlink(temp_path);
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
        unlink(temp_path);
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
        unlink(temp_path);
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

    // Start HTTP server
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 15;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Register URI handlers
    httpd_uri_t u;

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

    u.uri = "/upload";
    u.method = HTTP_POST;
    u.handler = h_post_upload;
    u.user_ctx = NULL;
    register_uri_handler_or_log(s_server, &u);

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

