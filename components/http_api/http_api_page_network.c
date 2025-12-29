/**
 * @file http_api_page_network.c
 * @brief Network configuration page and credential erase handler
 * 
 * Contains handlers for:
 * - GET /config/network - Network status page
 * - POST /erase - Erase WiFi credentials and reboot
 */

#include "http_api_internal.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_netif.h"
#include "app_wifi.h"
#include "freertos/task.h"

/**
 * GET /config/network
 * Returns HTML status page with connection information and erase button
 */
esp_err_t h_get_network_config(httpd_req_t *req) {
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
    
    // Build HTML response
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
    
    char html[4096];
    int len = 0;
    int ret;
    
    ret = snprintf(html, sizeof(html), "%s", html_header);
    if (ret < 0 || ret >= sizeof(html)) {
        ESP_LOGE(HTTP_API_TAG, "HTML buffer overflow in header");
        return ESP_FAIL;
    }
    len = ret;
    
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
esp_err_t h_post_erase(httpd_req_t *req) {
    ESP_LOGI(HTTP_API_TAG, "Erase credentials requested via web interface");
    app_wifi_erase_credentials();
    
    const char *response = 
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">"
        "<title>p3a - Reset Wi-Fi</title>"
        "<style>"
        "* { box-sizing: border-box; }"
        "body {"
        "  margin: 0;"
        "  padding: 12px 10px 16px;"
        "  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
        "  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "  min-height: 100vh;"
        "  display: flex;"
        "  flex-direction: column;"
        "  align-items: center;"
        "  gap: 12px;"
        "  color: #fff;"
        "}"
        "@supports (min-height: 100svh) { body { min-height: 100svh; } }"
        "@supports (min-height: 100dvh) { body { min-height: 100dvh; } }"
        ".header { text-align: center; padding: 8px 0 4px; }"
        ".header h1 {"
        "  margin: 0;"
        "  font-size: clamp(2rem, 4vw, 2.4rem);"
        "  font-weight: 300;"
        "  letter-spacing: 0.1em;"
        "  text-transform: lowercase;"
        "}"
        ".subtitle { margin: 0; opacity: 0.9; font-size: 0.95rem; }"
        ".card {"
        "  width: min(520px, 100%);"
        "  background: rgba(255,255,255,0.95);"
        "  border-radius: 16px;"
        "  padding: 14px;"
        "  box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "  color: #111;"
        "}"
        ".card h2 {"
        "  margin: 0 0 10px;"
        "  font-size: 0.85rem;"
        "  font-weight: 600;"
        "  color: #333;"
        "  text-transform: uppercase;"
        "  letter-spacing: 0.05em;"
        "}"
        ".help { margin: 10px 0 0; font-size: 0.85rem; color: #555; line-height: 1.35; }"
        ".help code { background: rgba(102,126,234,0.12); padding: 2px 6px; border-radius: 8px; }"
        ".divider { height: 1px; background: #eee; margin: 12px 0; }"
        ".pill { display: inline-block; padding: 6px 10px; border-radius: 999px; background: rgba(102,126,234,0.12); color: #334; font-weight: 600; }"
        ".list { margin: 10px 0 0; padding-left: 18px; color: #444; }"
        ".list li { margin: 6px 0; }"
        ".btn {"
        "  width: 100%;"
        "  background: #667eea;"
        "  color: white;"
        "  padding: 13px 14px;"
        "  border: none;"
        "  border-radius: 12px;"
        "  font-size: 0.98rem;"
        "  font-weight: 600;"
        "  cursor: pointer;"
        "  transition: transform 0.2s;"
        "  -webkit-tap-highlight-color: transparent;"
        "}"
        ".btn:active { transform: scale(0.98); }"
        ".btn-ghost {"
        "  background: rgba(102,126,234,0.12);"
        "  color: #3b4cca;"
        "  border: 1px solid rgba(102,126,234,0.25);"
        "}"
        "</style>"
        "</head>"
        "<body>"
        "  <div class=\"header\">"
        "    <h1>p3a</h1>"
        "    <p class=\"subtitle\">resetting wi-fi</p>"
        "  </div>"
        "  <div class=\"card\">"
        "    <h2>Wi-Fi erased. Rebooting now</h2>"
        "    <p class=\"help\">Saved Wi-Fi credentials have been erased. p3a will reboot into setup mode and create the <code>p3a-setup</code> Wi-Fi network again.</p>"
        "    <ul class=\"list\">"
        "      <li>After reboot, connect to <code>p3a-setup</code>.</li>"
        "      <li>Then open <code>http://p3a.local/</code> to set up Wi-Fi.</li>"
        "    </ul>"
        "    <div class=\"divider\"></div>"
        "    <p class=\"help\">This page will try to reload in <span class=\"pill\"><span id=\"sec\">10</span>s</span>.</p>"
        "    <button class=\"btn btn-ghost\" type=\"button\" onclick=\"window.location.href='/';\">Go to Home now</button>"
        "  </div>"
        "  <script>"
        "  (function(){"
        "    var remaining = 10;"
        "    var el = document.getElementById('sec');"
        "    function tick(){"
        "      remaining--; if (el) el.textContent = String(remaining);"
        "      if (remaining <= 0){ clearInterval(timer); window.location.href='/'; }"
        "    }"
        "    var timer = setInterval(tick, 1000);"
        "  })();"
        "  </script>"
        "</body>"
        "</html>";
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    
    // Delay before reboot to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    
    return ESP_OK;
}

