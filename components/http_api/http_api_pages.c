/**
 * @file http_api_pages.c
 * @brief HTTP API page handlers - HTML pages and static file serving
 * 
 * Contains handlers for:
 * - GET / (main control page)
 * - GET /config/network (network status page)
 * - POST /erase (erase credentials)
 * - GET /favicon.ico
 * - GET /static/... (static file serving)
 * - GET /pico8 (PICO-8 monitor page)
 * - WS /pico_stream (WebSocket for PICO-8)
 */

#include "http_api_internal.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_netif.h"
#include "app_wifi.h"
#include "favicon_data.h"
#if CONFIG_P3A_PICO8_ENABLE
#include "pico8_stream.h"
#endif
#include <sys/stat.h>

#if CONFIG_P3A_PICO8_ENABLE
static bool s_ws_client_connected = false;
#endif

// ---------- Favicon Handler ----------

/**
 * GET /favicon.ico
 * Returns the favicon PNG image
 */
static esp_err_t h_get_favicon(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/png");
    httpd_resp_send(req, (const char *)favicon_data, FAVICON_SIZE);
    return ESP_OK;
}

// ---------- Static File Handler ----------

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
        ESP_LOGW(HTTP_API_TAG, "Static path too long: %s", uri);
        httpd_resp_set_status(req, "414 Request-URI Too Long");
        httpd_resp_send(req, "Path too long", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    snprintf(filepath, sizeof(filepath), "%s%s", prefix, uri);
    
    FILE* f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(HTTP_API_TAG, "Failed to open %s", filepath);
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

// ---------- Network Config Page ----------

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
        ESP_LOGE(HTTP_API_TAG, "HTML buffer overflow in header");
        return ESP_FAIL;
    }
    len = ret;
    
    // Add status badge
    ret = snprintf(html + len, sizeof(html) - len, "%s%s",
        has_ip ? html_status_connected : html_status_disconnected,
        html_status_end);
    if (ret < 0 || len + ret >= sizeof(html)) {
        ESP_LOGE(HTTP_API_TAG, "HTML buffer overflow in status badge");
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
            ESP_LOGE(HTTP_API_TAG, "HTML buffer overflow in SSID");
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
            ESP_LOGE(HTTP_API_TAG, "HTML buffer overflow in IP info");
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
            ESP_LOGE(HTTP_API_TAG, "HTML buffer overflow in RSSI");
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
        ESP_LOGE(HTTP_API_TAG, "HTML buffer overflow in footer");
        return ESP_FAIL;
    }
    len += ret;
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, len);
    ESP_LOGI(HTTP_API_TAG, "Status page sent, length=%d", len);
    return ESP_OK;
}

/**
 * POST /erase
 * Erases Wi-Fi credentials and reboots the device
 */
static esp_err_t h_post_erase(httpd_req_t *req) {
    ESP_LOGI(HTTP_API_TAG, "Erase credentials requested via web interface");
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

// ---------- Main Control Page ----------

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
        ".channel-btn {"
        "    background: rgba(255,255,255,0.95);"
        "    border: none;"
        "    border-radius: 12px;"
        "    padding: 12px 20px;"
        "    font-size: 0.9rem;"
        "    font-weight: 500;"
        "    color: #667eea;"
        "    cursor: pointer;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "    transition: all 0.2s ease;"
        "    -webkit-tap-highlight-color: transparent;"
        "    width: 100%;"
        "}"
        ".channel-btn:active {"
        "    transform: scale(0.95);"
        "}"
        ".channel-btn:disabled {"
        "    opacity: 0.5;"
        "    cursor: not-allowed;"
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
        ".bg-row {"
        "    display: flex;"
        "    gap: 8px;"
        "    align-items: center;"
        "}"
        ".bg-input {"
        "    width: 100%;"
        "    max-width: 84px;"
        "    padding: 8px 10px;"
        "    border-radius: 8px;"
        "    border: 1px solid #ddd;"
        "    font-size: 0.9rem;"
        "}"
        ".bg-save {"
        "    flex: 1 1 auto;"
        "    min-width: 90px;"
        "}"
        ".toggle-row {"
        "    display: flex;"
        "    align-items: center;"
        "    justify-content: space-between;"
        "    padding: 4px 0;"
        "}"
        ".toggle-label {"
        "    font-size: 0.9rem;"
        "    color: #333;"
        "}"
        ".toggle-switch {"
        "    position: relative;"
        "    display: inline-block;"
        "    width: 48px;"
        "    height: 26px;"
        "}"
        ".toggle-switch input {"
        "    opacity: 0;"
        "    width: 0;"
        "    height: 0;"
        "}"
        ".toggle-slider {"
        "    position: absolute;"
        "    cursor: pointer;"
        "    top: 0;"
        "    left: 0;"
        "    right: 0;"
        "    bottom: 0;"
        "    background-color: #ccc;"
        "    transition: 0.3s;"
        "    border-radius: 26px;"
        "}"
        ".toggle-slider:before {"
        "    position: absolute;"
        "    content: '';"
        "    height: 20px;"
        "    width: 20px;"
        "    left: 3px;"
        "    bottom: 3px;"
        "    background-color: white;"
        "    transition: 0.3s;"
        "    border-radius: 50%;"
        "}"
        ".toggle-switch input:checked + .toggle-slider {"
        "    background-color: #667eea;"
        "}"
        ".toggle-switch input:checked + .toggle-slider:before {"
        "    transform: translateX(22px);"
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
        "<div class=\"controls\" style=\"margin-top: 0;\">"
        "    <div style=\"display: flex; flex-direction: column; gap: 10px; width: 100%;\">"
        "        <button class=\"channel-btn\" onclick=\"switchChannel('all')\">Recent Artworks</button>"
        "        <button class=\"channel-btn\" onclick=\"switchChannel('promoted')\">Promoted</button>"
        "        <button class=\"channel-btn\" onclick=\"switchChannel('sdcard')\">SD Card</button>"
        "    </div>"
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
        "<div class=\"upload-section\">"
        "    <h3>Background</h3>"
        "    <div class=\"bg-row\">"
        "        <input class=\"bg-input\" id=\"bg-r\" type=\"number\" min=\"0\" max=\"255\" placeholder=\"R\">"
        "        <input class=\"bg-input\" id=\"bg-g\" type=\"number\" min=\"0\" max=\"255\" placeholder=\"G\">"
        "        <input class=\"bg-input\" id=\"bg-b\" type=\"number\" min=\"0\" max=\"255\" placeholder=\"B\">"
        "        <button class=\"upload-btn bg-save\" id=\"bg-save\" type=\"button\" onclick=\"saveBackgroundColor()\">Save</button>"
        "    </div>"
        "    <div class=\"file-name\" id=\"bg-hint\">Applies to transparent artwork (GIF/WebP/PNG)</div>"
        "</div>"
        "<div class=\"upload-section\">"
        "    <h3>Display</h3>"
        "    <div class=\"toggle-row\">"
        "        <label for=\"fps-toggle\" class=\"toggle-label\">Show FPS</label>"
        "        <label class=\"toggle-switch\">"
        "            <input type=\"checkbox\" id=\"fps-toggle\" onchange=\"toggleShowFps(this.checked)\">"
        "            <span class=\"toggle-slider\"></span>"
        "        </label>"
        "    </div>"
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
        "    <button class=\"footer-btn\" onclick=\"window.location.href='/seed'\">Seed</button>"
        "    <button class=\"footer-btn\" onclick=\"window.location.href='/ota'\">Update</button>"
        "</div>"
        "<div class=\"status\" id=\"status\"></div>"
        "<script>"
        "var isPaused = false;"
        "function showToast(msg, isError) {"
        "    var status = document.getElementById('status');"
        "    status.textContent = msg;"
        "    status.className = isError ? 'status error' : 'status success';"
        "    status.style.display = 'block';"
        "    setTimeout(function() { status.style.display = 'none'; }, 2000);"
        "}"
        "function clamp255(x) {"
        "    x = parseInt(x, 10);"
        "    if (isNaN(x)) x = 0;"
        "    if (x < 0) x = 0;"
        "    if (x > 255) x = 255;"
        "    return x;"
        "}"
        "function loadBackgroundColor() {"
        "    var xhr = new XMLHttpRequest();"
        "    xhr.open('GET', '/config', true);"
        "    xhr.onreadystatechange = function() {"
        "        if (xhr.readyState === 4) {"
        "            try {"
        "                var resp = JSON.parse(xhr.responseText || '{}');"
        "                var cfg = (resp && resp.ok && resp.data) ? resp.data : {};"
        "                var bg = (cfg && cfg.background_color) ? cfg.background_color : null;"
        "                var r = bg && typeof bg.r === 'number' ? bg.r : 0;"
        "                var g = bg && typeof bg.g === 'number' ? bg.g : 0;"
        "                var b = bg && typeof bg.b === 'number' ? bg.b : 0;"
        "                document.getElementById('bg-r').value = clamp255(r);"
        "                document.getElementById('bg-g').value = clamp255(g);"
        "                document.getElementById('bg-b').value = clamp255(b);"
        "            } catch (e) {"
        "                document.getElementById('bg-r').value = 0;"
        "                document.getElementById('bg-g').value = 0;"
        "                document.getElementById('bg-b').value = 0;"
        "            }"
        "        }"
        "    };"
        "    xhr.send();"
        "}"
        "function saveBackgroundColor() {"
        "    var btn = document.getElementById('bg-save');"
        "    btn.disabled = true;"
        "    var r = clamp255(document.getElementById('bg-r').value);"
        "    var g = clamp255(document.getElementById('bg-g').value);"
        "    var b = clamp255(document.getElementById('bg-b').value);"
        "    var xhrGet = new XMLHttpRequest();"
        "    xhrGet.open('GET', '/config', true);"
        "    xhrGet.onreadystatechange = function() {"
        "        if (xhrGet.readyState === 4) {"
        "            var cfg = {};"
        "            try {"
        "                var resp = JSON.parse(xhrGet.responseText || '{}');"
        "                cfg = (resp && resp.ok && resp.data) ? resp.data : {};"
        "            } catch (e) { cfg = {}; }"
        "            cfg.background_color = { r: r, g: g, b: b };"
        "            var xhrPut = new XMLHttpRequest();"
        "            xhrPut.open('PUT', '/config', true);"
        "            xhrPut.setRequestHeader('Content-Type', 'application/json');"
        "            xhrPut.onreadystatechange = function() {"
        "                if (xhrPut.readyState === 4) {"
        "                    btn.disabled = false;"
        "                    if (xhrPut.status >= 200 && xhrPut.status < 300) {"
        "                        showToast('Background saved', false);"
        "                    } else {"
        "                        showToast('Save failed (HTTP ' + xhrPut.status + ')', true);"
        "                    }"
        "                }"
        "            };"
        "            xhrPut.send(JSON.stringify(cfg));"
        "        }"
        "    };"
        "    xhrGet.send();"
        "}"
        "function loadShowFps() {"
        "    var xhr = new XMLHttpRequest();"
        "    xhr.open('GET', '/config', true);"
        "    xhr.onreadystatechange = function() {"
        "        if (xhr.readyState === 4) {"
        "            try {"
        "                var resp = JSON.parse(xhr.responseText || '{}');"
        "                var cfg = (resp && resp.ok && resp.data) ? resp.data : {};"
        "                var showFps = (typeof cfg.show_fps === 'boolean') ? cfg.show_fps : true;"
        "                document.getElementById('fps-toggle').checked = showFps;"
        "            } catch (e) {"
        "                document.getElementById('fps-toggle').checked = true;"
        "            }"
        "        }"
        "    };"
        "    xhr.send();"
        "}"
        "function toggleShowFps(enable) {"
        "    var xhrGet = new XMLHttpRequest();"
        "    xhrGet.open('GET', '/config', true);"
        "    xhrGet.onreadystatechange = function() {"
        "        if (xhrGet.readyState === 4) {"
        "            var cfg = {};"
        "            try {"
        "                var resp = JSON.parse(xhrGet.responseText || '{}');"
        "                cfg = (resp && resp.ok && resp.data) ? resp.data : {};"
        "            } catch (e) { cfg = {}; }"
        "            cfg.show_fps = enable;"
        "            var xhrPut = new XMLHttpRequest();"
        "            xhrPut.open('PUT', '/config', true);"
        "            xhrPut.setRequestHeader('Content-Type', 'application/json');"
        "            xhrPut.onreadystatechange = function() {"
        "                if (xhrPut.readyState === 4) {"
        "                    if (xhrPut.status >= 200 && xhrPut.status < 300) {"
        "                        showToast('FPS display ' + (enable ? 'enabled' : 'disabled'), false);"
        "                    } else {"
        "                        showToast('Save failed (HTTP ' + xhrPut.status + ')', true);"
        "                        document.getElementById('fps-toggle').checked = !enable;"
        "                    }"
        "                }"
        "            };"
        "            xhrPut.send(JSON.stringify(cfg));"
        "        }"
        "    };"
        "    xhrGet.send();"
        "}"
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
        "function switchChannel(channel) {"
        "    console.log('Switching to channel:', channel);"
        "    var status = document.getElementById('status');"
        "    var xhr = new XMLHttpRequest();"
        "    xhr.open('POST', '/channel', true);"
        "    xhr.setRequestHeader('Content-Type', 'application/json');"
        "    xhr.onreadystatechange = function() {"
        "        if (xhr.readyState === 4) {"
        "            try {"
        "                var result = JSON.parse(xhr.responseText);"
        "                if (xhr.status >= 200 && xhr.status < 300 && result.ok) {"
        "                    var channelName = channel === 'all' ? 'Recent Artworks' : (channel === 'promoted' ? 'Promoted' : 'SD Card');"
        "                    status.textContent = 'Switched to ' + channelName;"
        "                    status.className = 'status success';"
        "                } else {"
        "                    status.textContent = 'Channel switch failed: ' + (result.error || 'HTTP ' + xhr.status);"
        "                    status.className = 'status error';"
        "                }"
        "            } catch (e) {"
        "                status.textContent = 'Parse error: ' + e.message;"
        "                status.className = 'status error';"
        "            }"
        "            status.style.display = 'block';"
        "            setTimeout(function() { status.style.display = 'none'; }, 2000);"
        "        }"
        "    };"
        "    xhr.send(JSON.stringify({channel: channel}));"
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
        "loadBackgroundColor();"
        "loadShowFps();"
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
        ESP_LOGE(HTTP_API_TAG, "Failed to calculate injection size");
        return ESP_FAIL;
    }
    
    // Calculate total buffer size needed
    size_t html_len = strlen(html);
    size_t html_buf_size = html_len + (size_t)injection_len + 1; // +1 for null terminator
    char *html_buf = malloc(html_buf_size);
    if (!html_buf) {
        ESP_LOGE(HTTP_API_TAG, "Failed to allocate HTML buffer");
        return ESP_ERR_NO_MEM;
    }

    // Find the position where we need to inject LCD dimensions
    // Look for the script tag and inject variables right after it
    const char *script_start = strstr(html, "<script>");
    if (!script_start) {
        ESP_LOGE(HTTP_API_TAG, "Could not find script tag in HTML");
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
        ESP_LOGE(HTTP_API_TAG, "Failed to inject LCD dimensions (injected=%d, available=%zu)", 
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
        ESP_LOGE(HTTP_API_TAG, "HTML buffer too small (needed=%zu, available=%zu)", 
                 total_needed, html_buf_size);
        free(html_buf);
        return ESP_FAIL;
    }
    
    memcpy(html_buf + current_pos, rest_start, rest_len);
    html_buf[current_pos + rest_len] = '\0';

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_buf, strlen(html_buf));
    free(html_buf);
    ESP_LOGI(HTTP_API_TAG, "Remote control page sent");
    return ESP_OK;
}

// ---------- PICO-8 Handlers ----------

#if CONFIG_P3A_PICO8_ENABLE
/**
 * GET /pico8
 * Serves the PICO-8 monitor HTML page
 */
static esp_err_t h_get_pico8(httpd_req_t *req) {
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
static esp_err_t h_ws_pico_stream(httpd_req_t *req) {
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

// ---------- Global Seed Page ----------

static esp_err_t h_get_seed(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "<title>p3a - Global Seed</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{margin:0;padding:12px 10px 16px;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;flex-direction:column;align-items:center;gap:12px;color:#fff}"
        ".card{width:min(520px,100%);background:rgba(255,255,255,0.12);border-radius:18px;box-shadow:0 4px 12px rgba(0,0,0,0.15);padding:16px}"
        ".row{display:flex;gap:10px;align-items:center;justify-content:space-between;flex-wrap:wrap}"
        "input{width:100%;padding:12px 12px;border-radius:12px;border:none;font-size:1rem}"
        ".btn{background:rgba(255,255,255,0.95);border:none;border-radius:12px;padding:12px 14px;font-size:1rem;color:#667eea;cursor:pointer;box-shadow:0 4px 12px rgba(0,0,0,0.15)}"
        ".btn.secondary{background:rgba(255,255,255,0.25);color:#fff}"
        ".status{display:none;margin-top:10px;padding:10px;border-radius:12px;font-size:0.95rem}"
        ".status.ok{display:block;background:rgba(0,200,0,0.25)}"
        ".status.err{display:block;background:rgba(255,0,0,0.25)}"
        "h1{margin:0 0 8px 0;font-weight:300;letter-spacing:0.08em;text-transform:lowercase}"
        "p{margin:0 0 12px 0;opacity:0.9}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"card\">"
        "  <h1>global seed</h1>"
        "  <p>This seed is persisted and takes effect after reboot.</p>"
        "  <div class=\"row\">"
        "    <div style=\"flex:1;min-width:220px\">"
        "      <label for=\"seed\" style=\"display:block;margin-bottom:6px;opacity:0.9\">Seed (uint32)</label>"
        "      <input id=\"seed\" type=\"number\" min=\"0\" step=\"1\" />"
        "    </div>"
        "    <button class=\"btn\" onclick=\"saveSeed()\">Save</button>"
        "  </div>"
        "  <div class=\"row\" style=\"margin-top:12px\">"
        "    <button class=\"btn secondary\" onclick=\"window.location.href='/'\">Back</button>"
        "    <button class=\"btn\" onclick=\"reboot()\">Reboot</button>"
        "  </div>"
        "  <div id=\"status\" class=\"status\"></div>"
        "</div>"
        "<script>"
        "function setStatus(ok,msg){var s=document.getElementById('status');s.textContent=msg;s.className='status '+(ok?'ok':'err');}"
        "function loadSeed(){var xhr=new XMLHttpRequest();xhr.open('GET','/settings/global_seed',true);xhr.onreadystatechange=function(){"
        " if(xhr.readyState===4){try{var r=JSON.parse(xhr.responseText);if(xhr.status>=200&&xhr.status<300&&r.ok){document.getElementById('seed').value=r.data.global_seed;}"
        " else{setStatus(false,'Failed to load seed');}}catch(e){setStatus(false,'Parse error');}}};xhr.send();}"
        "function saveSeed(){var v=document.getElementById('seed').value;var n=parseInt(v,10);if(isNaN(n)||n<0){setStatus(false,'Invalid seed');return;}"
        " var xhr=new XMLHttpRequest();xhr.open('PUT','/settings/global_seed',true);xhr.setRequestHeader('Content-Type','application/json');"
        " xhr.onreadystatechange=function(){if(xhr.readyState===4){try{var r=JSON.parse(xhr.responseText);if(xhr.status>=200&&xhr.status<300&&r.ok){setStatus(true,'Saved. Reboot to apply.');}"
        " else{setStatus(false,'Save failed');}}catch(e){setStatus(false,'Parse error');}}};"
        " xhr.send(JSON.stringify({global_seed:n}));}"
        "function reboot(){var xhr=new XMLHttpRequest();xhr.open('POST','/action/reboot',true);xhr.setRequestHeader('Content-Type','application/json');xhr.send('{}');setStatus(true,'Reboot queued');}"
        "loadSeed();"
        "</script>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------- Sub-router entrypoints ----------

esp_err_t http_api_pages_route_get(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(uri, "/favicon.ico") == 0) {
        return h_get_favicon(req);
    }
    if (strcmp(uri, "/") == 0) {
        return h_get_root(req);
    }
    if (strcmp(uri, "/config/network") == 0) {
        return h_get_network_config(req);
    }
    if (strcmp(uri, "/seed") == 0) {
        return h_get_seed(req);
    }

#if CONFIG_P3A_PICO8_ENABLE
    if (strcmp(uri, "/pico8") == 0) {
        return h_get_pico8(req);
    }
#endif

    return ESP_ERR_NOT_FOUND;
}

esp_err_t http_api_pages_route_post(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(uri, "/erase") == 0) {
        return h_post_erase(req);
    }

    return ESP_ERR_NOT_FOUND;
}

// ---------- Registration Function ----------

void http_api_register_page_handlers(httpd_handle_t server) {
    httpd_uri_t u = {0};

    u.uri = "/static/*";
    u.method = HTTP_GET;
    u.handler = h_get_static;
    u.user_ctx = NULL;
    register_uri_handler_or_log(server, &u);

#if CONFIG_P3A_PICO8_ENABLE
    // WebSocket endpoint for PICO-8 streaming
    httpd_uri_t ws_uri = {
        .uri = "/pico_stream",
        .method = HTTP_GET,
        .handler = h_ws_pico_stream,
        .user_ctx = NULL,
        .is_websocket = true
    };
    register_uri_handler_or_log(server, &ws_uri);
#endif
}

