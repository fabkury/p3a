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
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HTTP";

#define MAX_JSON (32 * 1024)
#define RECV_CHUNK 4096
#define QUEUE_LEN 10

typedef enum {
    CMD_REBOOT,
    CMD_SWAP_NEXT,
    CMD_SWAP_BACK
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
                        app_state_enter_playing();
                    } else {
                        ESP_LOGW(TAG, "swap_next callback not set");
                        app_state_enter_error();
                    }
                    break;

                case CMD_SWAP_BACK:
                    if (s_swap_back_callback) {
                        ESP_LOGI(TAG, "Executing swap_back");
                        s_swap_back_callback();
                        app_state_enter_playing();
                    } else {
                        ESP_LOGW(TAG, "swap_back callback not set");
                        app_state_enter_error();
                    }
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
        "<title>P3A Status</title>"
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
        "<h1>P3A Pixel Art Player</h1>"
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
 * GET /
 * Returns Remote Control HTML page with swap buttons and navigation to network config
 */
static esp_err_t h_get_root(httpd_req_t *req) {
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "<title>P3A Remote Control</title>"
        "<style>"
        "body {"
        "    margin: 0;"
        "    padding: 0;"
        "    font-family: Arial, sans-serif;"
        "    background-color: #f0f0f0;"
        "    min-height: 100vh;"
        "    display: flex;"
        "    flex-direction: column;"
        "}"
        ".banner {"
        "    background-color: #333;"
        "    color: white;"
        "    text-align: center;"
        "    padding: 20px;"
        "    font-size: 2em;"
        "    font-weight: bold;"
        "}"
        ".control-area {"
        "    flex: 1;"
        "    display: flex;"
        "    align-items: center;"
        "    justify-content: center;"
        "    padding: 20px;"
        "}"
        ".arrow-container {"
        "    display: flex;"
        "    align-items: center;"
        "    gap: 40px;"
        "}"
        ".arrow-btn {"
        "    background-color: #4CAF50;"
        "    border: none;"
        "    border-radius: 50%;"
        "    width: 120px;"
        "    height: 120px;"
        "    display: flex;"
        "    align-items: center;"
        "    justify-content: center;"
        "    cursor: pointer;"
        "    font-size: 3em;"
        "    color: white;"
        "    box-shadow: 0 4px 8px rgba(0,0,0,0.3);"
        "    transition: all 0.2s;"
        "}"
        ".arrow-btn:hover {"
        "    background-color: #45a049;"
        "    transform: scale(1.05);"
        "}"
        ".arrow-btn:active {"
        "    transform: scale(0.95);"
        "}"
        ".arrow-btn:disabled {"
        "    background-color: #cccccc;"
        "    cursor: not-allowed;"
        "}"
        ".arrow-label {"
        "    text-align: center;"
        "    margin-top: 10px;"
        "    font-size: 1.2em;"
        "    color: #333;"
        "}"
        ".config-btn {"
        "    position: fixed;"
        "    bottom: 20px;"
        "    right: 20px;"
        "    background-color: #2196F3;"
        "    color: white;"
        "    border: none;"
        "    border-radius: 50%;"
        "    width: 60px;"
        "    height: 60px;"
        "    font-size: 1.5em;"
        "    cursor: pointer;"
        "    box-shadow: 0 2px 4px rgba(0,0,0,0.3);"
        "}"
        ".config-btn:hover {"
        "    background-color: #1976D2;"
        "}"
        ".status {"
        "    position: fixed;"
        "    bottom: 20px;"
        "    left: 50%;"
        "    transform: translateX(-50%);"
        "    text-align: center;"
        "    max-width: 400px;"
        "    padding: 10px;"
        "    border-radius: 5px;"
        "    font-weight: bold;"
        "    display: none;"
        "    z-index: 1000;"
        "}"
        ".status.success {"
        "    background-color: #4CAF50;"
        "    color: white;"
        "}"
        ".status.error {"
        "    background-color: #f44336;"
        "    color: white;"
        "}"
        "@media (max-width: 768px) {"
        "    .arrow-container {"
        "        gap: 20px;"
        "    }"
        "    .arrow-btn {"
        "        width: 100px;"
        "        height: 100px;"
        "        font-size: 2.5em;"
        "    }"
        "    .banner {"
        "        font-size: 1.5em;"
        "        padding: 15px;"
        "    }"
        "}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"banner\">P3A</div>"
        "<div class=\"control-area\">"
        "    <div class=\"arrow-container\">"
        "        <div>"
        "            <button class=\"arrow-btn\" id=\"back-btn\" onclick=\"sendCommand('swap_back')\">◄</button>"
        "            <div class=\"arrow-label\">Back</div>"
        "        </div>"
        "        <div>"
        "            <button class=\"arrow-btn\" id=\"next-btn\" onclick=\"sendCommand('swap_next')\">►</button>"
        "            <div class=\"arrow-label\">Next</div>"
        "        </div>"
        "    </div>"
        "</div>"
        "<div class=\"status\" id=\"status\"></div>"
        "<button class=\"config-btn\" onclick=\"window.location.href='/config/network'\">⚙</button>"
        "<script>"
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
        "                        try {"
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
        "            setTimeout(function() { status.style.display = 'none'; }, 3000);"
        "            backBtn.disabled = false;"
        "            nextBtn.disabled = false;"
        "        }"
        "    };"
        "    xhr.send('{}');"
        "}"
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
 * POST /upload/image
 * Uploads an image file to SD card and displays it immediately
 * Accepts multipart/form-data or raw binary (image/png, image/jpeg, image/webp)
 */
static esp_err_t h_post_upload_image(httpd_req_t *req) {
    #define MAX_UPLOAD_SIZE (5 * 1024 * 1024)  // 5MB max
    #define UPLOAD_BUFFER_SIZE (4096)

    ESP_LOGI(TAG, "Image upload request received, content_length=%d", (int)req->content_len);

    // Check content length
    if (req->content_len == 0) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Empty request\",\"code\":\"EMPTY_REQUEST\"}");
        return ESP_OK;
    }

    if (req->content_len > MAX_UPLOAD_SIZE) {
        send_json(req, 413, "{\"ok\":false,\"error\":\"File too large (max 5MB)\",\"code\":\"FILE_TOO_LARGE\"}");
        return ESP_OK;
    }

    // Get content type
    char content_type[128] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    if (err != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing Content-Type header\",\"code\":\"MISSING_CONTENT_TYPE\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Content-Type: %s", content_type);

    // Determine file extension from content type
    const char *file_ext = NULL;
    if (strstr(content_type, "image/png") || strstr(content_type, "png")) {
        file_ext = "png";
    } else if (strstr(content_type, "image/jpeg") || strstr(content_type, "jpeg") || strstr(content_type, "image/jpg")) {
        file_ext = "jpg";
    } else if (strstr(content_type, "image/webp") || strstr(content_type, "webp")) {
        file_ext = "webp";
    } else if (strstr(content_type, "image/gif") || strstr(content_type, "gif")) {
        file_ext = "gif";
    } else if (strstr(content_type, "multipart/form-data")) {
        // Default to PNG for multipart (we'll detect from data if needed)
        file_ext = "png";
    } else {
        ESP_LOGW(TAG, "Unknown content type: %s, defaulting to png", content_type);
        file_ext = "png";
    }

    // Build save path
    char save_path[128];
    snprintf(save_path, sizeof(save_path), "/sdcard/animations/td_live.%s", file_ext);

    ESP_LOGI(TAG, "Saving upload to: %s", save_path);

    // Open temp file for atomic write
    char temp_path[128];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", save_path);

    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open temp file: %s", temp_path);
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to open file for writing\",\"code\":\"FILE_WRITE_ERROR\"}");
        return ESP_OK;
    }

    // Allocate receive buffer
    char *buffer = malloc(UPLOAD_BUFFER_SIZE);
    if (!buffer) {
        fclose(f);
        remove(temp_path);
        send_json(req, 500, "{\"ok\":false,\"error\":\"Out of memory\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    // Receive and write data in chunks
    size_t total_received = 0;
    size_t remaining = req->content_len;
    bool write_error = false;

    while (remaining > 0 && !write_error) {
        size_t chunk_size = (remaining > UPLOAD_BUFFER_SIZE) ? UPLOAD_BUFFER_SIZE : remaining;

        int received = httpd_req_recv(req, buffer, chunk_size);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Socket timeout during upload");
            }
            ESP_LOGE(TAG, "Failed to receive data: %d", received);
            write_error = true;
            break;
        }

        // Write chunk to file
        size_t written = fwrite(buffer, 1, received, f);
        if (written != (size_t)received) {
            ESP_LOGE(TAG, "Failed to write data to file (wrote %zu of %d bytes)", written, received);
            write_error = true;
            break;
        }

        total_received += received;
        remaining -= received;

        // Log progress for large files
        if (total_received % (100 * 1024) == 0 || remaining == 0) {
            ESP_LOGI(TAG, "Upload progress: %zu / %d bytes (%.1f%%)",
                     total_received, (int)req->content_len,
                     (100.0 * total_received) / req->content_len);
        }
    }

    // Clean up
    free(buffer);
    fclose(f);

    if (write_error || total_received != req->content_len) {
        ESP_LOGE(TAG, "Upload failed: received %zu of %d bytes", total_received, (int)req->content_len);
        remove(temp_path);
        send_json(req, 500, "{\"ok\":false,\"error\":\"Upload incomplete\",\"code\":\"UPLOAD_INCOMPLETE\"}");
        return ESP_OK;
    }

    // Atomic rename: replace old file with new one
    remove(save_path);  // Remove old file if exists
    if (rename(temp_path, save_path) != 0) {
        ESP_LOGE(TAG, "Failed to rename temp file to %s", save_path);
        remove(temp_path);
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to save file\",\"code\":\"FILE_RENAME_ERROR\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Upload successful: %zu bytes saved to %s", total_received, save_path);

    // Load and display the uploaded image
    // Note: We call this directly rather than enqueuing to provide immediate feedback
    // animation_player_load_asset() is called from main context, should be safe
    extern esp_err_t animation_player_load_asset(const char *filepath);
    esp_err_t load_err = animation_player_load_asset(save_path);

    if (load_err != ESP_OK) {
        ESP_LOGW(TAG, "Uploaded file saved but failed to load as animation: %s", esp_err_to_name(load_err));
        // Still report success since file was saved
    }

    // Build success response JSON
    char response[512];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"data\":{\"saved_path\":\"%s\",\"file_size\":%zu,\"format\":\"%s\",\"display_updated\":%s}}",
             save_path, total_received, file_ext, (load_err == ESP_OK) ? "true" : "false");

    send_json(req, 200, response);
    return ESP_OK;

    #undef MAX_UPLOAD_SIZE
    #undef UPLOAD_BUFFER_SIZE
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
    cfg.max_uri_handlers = 12;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Register URI handlers
    httpd_uri_t u;

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

    u.uri = "/upload/image";
    u.method = HTTP_POST;
    u.handler = h_post_upload_image;
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

