// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "config_store.h"
#include "event_bus.h"
#include "p3a_state.h"
#include "app_wifi.h"
#include "wifi_manager_internal.h"

#define EXAMPLE_ESP_AP_SSID     CONFIG_ESP_AP_SSID
#define EXAMPLE_ESP_AP_PASSWORD CONFIG_ESP_AP_PASSWORD

static const char *TAG = "wifi_captive_portal";

/* URL Decode Function - Decodes all %XX hex sequences and converts + to space */
static void url_decode(char *str)
{
    if (!str) return;

    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            // Decode %XX hex sequence
            char hex[3] = {src[1], src[2], '\0'};
            char *endptr;
            unsigned long value = strtoul(hex, &endptr, 16);
            if (*endptr == '\0' && value <= 255) {
                *dst++ = (char)value;
                src += 3;
            } else {
                // Invalid hex sequence, copy as-is
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* URL Encode Function - Encodes unsafe characters for URL parameters */
static void url_encode(const char *in, char *out, size_t out_len) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 3 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

/* HTML Escape Function - Escapes special characters for safe HTML display */
static void html_escape_ssid(const char *in, char *out, size_t out_len) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 6 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case '&':
                memcpy(out + o, "&amp;", 5);
                o += 5;
                break;
            case '<':
                memcpy(out + o, "&lt;", 4);
                o += 4;
                break;
            case '>':
                memcpy(out + o, "&gt;", 4);
                o += 4;
                break;
            case '"':
                memcpy(out + o, "&quot;", 6);
                o += 6;
                break;
            case '\'':
                memcpy(out + o, "&#39;", 5);
                o += 5;
                break;
            default:
                out[o++] = c;
                break;
        }
    }
    out[o] = '\0';
}

/* Serve success page with SSID injected - serves HTML directly instead of redirect */
static esp_err_t serve_success_page_with_ssid(httpd_req_t *req, const char *ssid) {
    const char *filepath = "/webui/setup/success.html";
    const char *placeholder = "{SSID}";

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", filepath);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req,
            "<html><body style=\"font-family:sans-serif;text-align:center;padding:40px;\">"
            "<h1>p3a Setup</h1>"
            "<p>Credentials saved! Device will reboot now.</p>"
            "</body></html>",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 64 * 1024) {
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Invalid file", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Allocate buffer for file content
    char *html = malloc(size + 1);
    if (!html) {
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Memory error", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    size_t bytes_read = fread(html, 1, size, f);
    fclose(f);
    html[bytes_read] = '\0';

    // HTML-escape the SSID
    char ssid_escaped[128];
    html_escape_ssid(ssid, ssid_escaped, sizeof(ssid_escaped));

    // Find and replace {SSID} placeholder
    char *placeholder_pos = strstr(html, placeholder);
    if (placeholder_pos) {
        size_t placeholder_len = strlen(placeholder);
        size_t ssid_len = strlen(ssid_escaped);
        size_t before_len = placeholder_pos - html;
        size_t after_len = bytes_read - before_len - placeholder_len;

        // Allocate new buffer for modified HTML
        size_t new_size = before_len + ssid_len + after_len + 1;
        char *new_html = malloc(new_size);
        if (new_html) {
            memcpy(new_html, html, before_len);
            memcpy(new_html + before_len, ssid_escaped, ssid_len);
            memcpy(new_html + before_len + ssid_len, placeholder_pos + placeholder_len, after_len);
            new_html[new_size - 1] = '\0';

            free(html);
            html = new_html;
            bytes_read = new_size - 1;
        }
    }

    // Replace all {HOSTNAME} placeholders with effective hostname
    {
        char hn[24];
        config_store_get_hostname(hn, sizeof(hn));
        const char *hn_placeholder = "{HOSTNAME}";
        size_t hn_placeholder_len = strlen(hn_placeholder);
        size_t hn_len = strlen(hn);

        char *pos;
        while ((pos = strstr(html, hn_placeholder)) != NULL) {
            size_t before_len = pos - html;
            size_t after_len = bytes_read - before_len - hn_placeholder_len;
            size_t new_size = before_len + hn_len + after_len + 1;
            char *new_html = malloc(new_size);
            if (!new_html) break;
            memcpy(new_html, html, before_len);
            memcpy(new_html + before_len, hn, hn_len);
            memcpy(new_html + before_len + hn_len, pos + hn_placeholder_len, after_len);
            new_html[new_size - 1] = '\0';
            free(html);
            html = new_html;
            bytes_read = new_size - 1;
        }
    }

    // Send the HTML response
    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, html, bytes_read);

    free(html);
    return ret;
}

/* Simple file server for captive portal - serves HTML from LittleFS */
static esp_err_t serve_file_simple(httpd_req_t *req, const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", filepath);
        // Minimal fallback HTML in case files are missing
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req,
            "<html><body style=\"font-family:sans-serif;text-align:center;padding:40px;\">"
            "<h1>p3a Setup</h1>"
            "<p>UI files not found. Please reflash the device.</p>"
            "</body></html>",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 64 * 1024) { // Max 64KB for setup pages
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Invalid file", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Set content type
    httpd_resp_set_type(req, "text/html");

    // Stream file in chunks
    char chunk[1024];
    long remaining = size;

    while (remaining > 0) {
        size_t to_read = (remaining < sizeof(chunk)) ? remaining : sizeof(chunk);
        size_t bytes_read = fread(chunk, 1, to_read, f);
        if (bytes_read == 0) break;

        esp_err_t ret = httpd_resp_send_chunk(req, chunk, bytes_read);
        if (ret != ESP_OK) {
            fclose(f);
            return ret;
        }
        remaining -= bytes_read;
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // End response
    return ESP_OK;
}

/* HTTP Server Handlers */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    return serve_file_simple(req, "/webui/setup/index.html");
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    #define MAX_DEVICE_NAME_LEN 17  // 16 + null
    char content[280];
    size_t recv_size = sizeof(content) - 1;

    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Parse SSID, password, and device_name from form data
    char ssid[MAX_SSID_LEN] = {0};
    char password[MAX_PASSWORD_LEN] = {0};
    char device_name[MAX_DEVICE_NAME_LEN] = {0};

    // Simple form parsing
    char *ssid_start = strstr(content, "ssid=");
    char *password_start = strstr(content, "password=");
    char *device_name_start = strstr(content, "device_name=");

    if (ssid_start) {
        ssid_start += 5; // Skip "ssid="
        char *ssid_end = strchr(ssid_start, '&');
        if (ssid_end) {
            int len = ssid_end - ssid_start;
            if (len > MAX_SSID_LEN - 1) len = MAX_SSID_LEN - 1;
            strncpy(ssid, ssid_start, len);
            ssid[len] = '\0';
        } else {
            strncpy(ssid, ssid_start, MAX_SSID_LEN - 1);
        }
        // Properly decode URL-encoded SSID
        url_decode(ssid);
    }

    if (password_start) {
        password_start += 9; // Skip "password="
        char *password_end = strchr(password_start, '&');
        if (password_end) {
            int len = password_end - password_start;
            if (len > MAX_PASSWORD_LEN - 1) len = MAX_PASSWORD_LEN - 1;
            strncpy(password, password_start, len);
            password[len] = '\0';
        } else {
            strncpy(password, password_start, MAX_PASSWORD_LEN - 1);
        }
        // Properly decode URL-encoded password
        url_decode(password);
    }

    if (device_name_start) {
        device_name_start += 12; // Skip "device_name="
        char *device_name_end = strchr(device_name_start, '&');
        if (device_name_end) {
            int len = device_name_end - device_name_start;
            if (len > MAX_DEVICE_NAME_LEN - 2) len = MAX_DEVICE_NAME_LEN - 2;
            strncpy(device_name, device_name_start, len);
            device_name[len] = '\0';
        } else {
            strncpy(device_name, device_name_start, MAX_DEVICE_NAME_LEN - 2);
        }
        url_decode(device_name);
        // Save device name (config_store validates format)
        esp_err_t dn_err = config_store_set_device_name(device_name);
        if (dn_err != ESP_OK) {
            ESP_LOGW(TAG, "Invalid device_name '%s', ignoring", device_name);
        }
    }

    if (strlen(ssid) > 0) {
        wifi_save_credentials(ssid, password);
        ESP_LOGD(TAG, "Saved credentials, serving success page and rebooting...");

        // Serve success page directly with SSID injected
        esp_err_t ret2 = serve_success_page_with_ssid(req, ssid);

        // Short delay to ensure response is transmitted before reboot
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        return ret2;
    } else {
        // SSID required - serve error page
        return serve_file_simple(req, "/webui/setup/error.html");
    }

    return ESP_OK;
}

static esp_err_t erase_post_handler(httpd_req_t *req)
{
    app_wifi_erase_credentials();
    ESP_LOGD(TAG, "Erased credentials, rebooting...");

    // Serve the erased confirmation page
    esp_err_t ret = serve_file_simple(req, "/webui/setup/erased.html");

    // Delay before reboot to allow the response to be sent and rendered
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ret;
}

// Handler for /setup/ wildcard routes - serves setup pages from LittleFS
static esp_err_t setup_get_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    // Security: prevent path traversal and limit URI length
    if (strstr(uri, "..") != NULL || strlen(uri) > 64) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid path", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Map /setup/X to /webui/setup/X
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "/webui%.64s", uri);

    return serve_file_simple(req, filepath);
}

// Captive portal detection handlers for various OS platforms
// Android connectivity check
static esp_err_t generate_204_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// iOS/macOS captive portal check
static esp_err_t hotspot_detect_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Windows connectivity check
static esp_err_t connecttest_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Microsoft Connect Test", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Firefox captive portal check
static esp_err_t canonical_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, "success\n", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_uri_t root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static httpd_uri_t save = {
    .uri       = "/save",
    .method    = HTTP_POST,
    .handler   = save_post_handler,
    .user_ctx  = NULL
};

static httpd_uri_t erase = {
    .uri       = "/erase",
    .method    = HTTP_POST,
    .handler   = erase_post_handler,
    .user_ctx  = NULL
};

static httpd_uri_t setup_pages = {
    .uri       = "/setup/*",
    .method    = HTTP_GET,
    .handler   = setup_get_handler,
    .user_ctx  = NULL
};

// Captive portal detection URIs
static httpd_uri_t generate_204 = {
    .uri       = "/generate_204",
    .method    = HTTP_GET,
    .handler   = generate_204_handler,
    .user_ctx  = NULL
};

static httpd_uri_t hotspot_detect = {
    .uri       = "/hotspot-detect.html",
    .method    = HTTP_GET,
    .handler   = hotspot_detect_handler,
    .user_ctx  = NULL
};

static httpd_uri_t connecttest = {
    .uri       = "/connecttest.txt",
    .method    = HTTP_GET,
    .handler   = connecttest_handler,
    .user_ctx  = NULL
};

static httpd_uri_t canonical = {
    .uri       = "/canonical.html",
    .method    = HTTP_GET,
    .handler   = canonical_handler,
    .user_ctx  = NULL
};

/* DNS Server for Captive Portal - responds with AP IP to all queries */
static void dns_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char tx_buffer[128];

    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(53)
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket unable to bind");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGD(TAG, "DNS server started - responding with 192.168.4.1 to all queries");

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                          (struct sockaddr *)&source_addr, &socklen);

        if (len < 12) continue;  // DNS header is 12 bytes minimum

        // Build DNS response - copy request and modify flags
        memcpy(tx_buffer, rx_buffer, len);

        // Set response flags: QR=1 (response), AA=1 (authoritative), RCODE=0 (no error)
        tx_buffer[2] = 0x84;  // QR=1, Opcode=0, AA=1, TC=0, RD=0
        tx_buffer[3] = 0x00;  // RA=0, Z=0, RCODE=0

        // Set answer count to 1
        tx_buffer[6] = 0x00;
        tx_buffer[7] = 0x01;

        // Append answer: pointer to name (0xC00C), type A, class IN, TTL, rdlength, IP
        int answer_offset = len;
        tx_buffer[answer_offset++] = 0xC0;  // Pointer to question name
        tx_buffer[answer_offset++] = 0x0C;
        tx_buffer[answer_offset++] = 0x00;  // Type A
        tx_buffer[answer_offset++] = 0x01;
        tx_buffer[answer_offset++] = 0x00;  // Class IN
        tx_buffer[answer_offset++] = 0x01;
        tx_buffer[answer_offset++] = 0x00;  // TTL (60 seconds)
        tx_buffer[answer_offset++] = 0x00;
        tx_buffer[answer_offset++] = 0x00;
        tx_buffer[answer_offset++] = 0x3C;
        tx_buffer[answer_offset++] = 0x00;  // RDLENGTH (4 bytes for IPv4)
        tx_buffer[answer_offset++] = 0x04;
        // IP: 192.168.4.1
        tx_buffer[answer_offset++] = 192;
        tx_buffer[answer_offset++] = 168;
        tx_buffer[answer_offset++] = 4;
        tx_buffer[answer_offset++] = 1;

        sendto(sock, tx_buffer, answer_offset, 0,
               (struct sockaddr *)&source_addr, sizeof(source_addr));
    }

    close(sock);
    vTaskDelete(NULL);
}

/* Start Captive Portal */
static void start_captive_portal(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;  // Enable wildcard URI matching

    if (httpd_start(&s_captive_portal_server, &config) == ESP_OK) {
        // Register captive portal detection handlers first (various OS platforms)
        httpd_register_uri_handler(s_captive_portal_server, &generate_204);     // Android
        httpd_register_uri_handler(s_captive_portal_server, &hotspot_detect);   // iOS/macOS
        httpd_register_uri_handler(s_captive_portal_server, &connecttest);      // Windows
        httpd_register_uri_handler(s_captive_portal_server, &canonical);        // Firefox

        // Register setup page handlers
        httpd_register_uri_handler(s_captive_portal_server, &root);
        httpd_register_uri_handler(s_captive_portal_server, &save);
        httpd_register_uri_handler(s_captive_portal_server, &erase);
        httpd_register_uri_handler(s_captive_portal_server, &setup_pages);
        ESP_LOGD(TAG, "HTTP server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }

    // Start DNS server task
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, NULL);
}

/* Start mDNS in AP mode so p3a.local works during WiFi setup */
static esp_err_t start_mdns_ap(void)
{
    char hostname[24];
    config_store_get_hostname(hostname, sizeof(hostname));

    esp_err_t err = mdns_init();
    bool mdns_was_already_initialized = (err == ESP_ERR_INVALID_STATE);

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (mdns_was_already_initialized) {
        ESP_LOGW(TAG, "mDNS already initialized; reconfiguring for AP");
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return err;
    }

    char instance_name[48];
    snprintf(instance_name, sizeof(instance_name), "%s WiFi Setup", hostname);
    err = mdns_instance_name_set(instance_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
        return err;
    }

    // Only add service if not already added (using global flag for idempotency)
    if (!s_mdns_service_added) {
        err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        if (err == ESP_OK) {
            s_mdns_service_added = true;
        } else if (err == ESP_ERR_INVALID_ARG) {
            // Service might already exist - not fatal
            ESP_LOGW(TAG, "mDNS service may already exist, continuing");
            s_mdns_service_added = true;
        } else {
            ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGD(TAG, "mDNS started in AP mode: http://%s.local/", hostname);
    return ESP_OK;
}

/* Soft AP Initialization with Wi-Fi 6 */
void wifi_init_softap(void)
{
    esp_err_t err;

    // Check if WiFi is already initialized (from previous STA attempt)
    wifi_mode_t current_mode;
    bool wifi_already_initialized = (esp_wifi_remote_get_mode(&current_mode) == ESP_OK);

    if (wifi_already_initialized) {
        // WiFi is already initialized from STA mode - just switch modes
        // This is the standard ESP-IDF approach: stop -> set_mode -> set_config -> start
        ESP_LOGD(TAG, "WiFi already initialized, switching from STA to AP mode");

        err = esp_wifi_remote_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "esp_wifi_remote_stop failed: %s", esp_err_to_name(err));
        }

        // Create AP netif (STA netif can coexist, no need to destroy it)
        ap_netif = esp_netif_create_default_wifi_ap();

        wifi_config_t wifi_config = {
            .ap = {
                .ssid = EXAMPLE_ESP_AP_SSID,
                .ssid_len = strlen(EXAMPLE_ESP_AP_SSID),
                .channel = 1,
                .password = EXAMPLE_ESP_AP_PASSWORD,
                .max_connection = 4,
                .authmode = strlen(EXAMPLE_ESP_AP_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            },
        };

        if (strlen(EXAMPLE_ESP_AP_PASSWORD) == 0) {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_remote_start());
    } else {
        // WiFi not initialized yet - do fresh initialization for AP mode
        ESP_LOGD(TAG, "Fresh WiFi initialization for AP mode");

        ap_netif = esp_netif_create_default_wifi_ap();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_remote_init(&cfg));

        wifi_config_t wifi_config = {
            .ap = {
                .ssid = EXAMPLE_ESP_AP_SSID,
                .ssid_len = strlen(EXAMPLE_ESP_AP_SSID),
                .channel = 1,
                .password = EXAMPLE_ESP_AP_PASSWORD,
                .max_connection = 4,
                .authmode = strlen(EXAMPLE_ESP_AP_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            },
        };

        if (strlen(EXAMPLE_ESP_AP_PASSWORD) == 0) {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_remote_start());
    }

    // Enable Wi-Fi 6 protocol for AP (best-effort)
    wifi_set_protocol_11ax(WIFI_IF_AP);

    ESP_LOGD(TAG, "Soft AP initialized. SSID:%s password:%s", EXAMPLE_ESP_AP_SSID,
             strlen(EXAMPLE_ESP_AP_PASSWORD) > 0 ? EXAMPLE_ESP_AP_PASSWORD : "none");

    // Configure AP IP address
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    ESP_LOGD(TAG, "AP IP address: " IPSTR, IP2STR(&ip_info.ip));

    // Start captive portal
    start_captive_portal();

    // Start mDNS so p3a.local works in AP mode
    esp_err_t mdns_err = start_mdns_ap();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS start failed (captive portal still works via IP): %s", esp_err_to_name(mdns_err));
    }

    // Notify the system that softAP mode has started
    event_bus_emit_simple(P3A_EVENT_SOFTAP_STARTED);
}
