/* WiFi station Example with Captive Portal

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "app_state.h"
#include "http_api.h"
#include "app_wifi.h"

#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY
#define EXAMPLE_ESP_AP_SSID        CONFIG_ESP_AP_SSID
#define EXAMPLE_ESP_AP_PASSWORD    CONFIG_ESP_AP_PASSWORD
#define NVS_NAMESPACE              "wifi_config"
#define NVS_KEY_SSID               "ssid"
#define NVS_KEY_PASSWORD           "password"
#define MAX_SSID_LEN               32
#define MAX_PASSWORD_LEN           64

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "app_wifi";

static int s_retry_num = 0;
static httpd_handle_t s_captive_portal_server = NULL;
static esp_netif_t *ap_netif = NULL;

// Callback for when REST API should be started (to register action handlers)
static app_wifi_rest_callback_t s_rest_start_callback = NULL;

/* NVS Credential Storage Functions */
static esp_err_t wifi_load_credentials(char *ssid, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t required_size;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved credentials found");
        return err;
    }

    // Read SSID
    required_size = MAX_SSID_LEN;
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &required_size);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Failed to read SSID from NVS");
        nvs_close(nvs_handle);
        return err;
    }

    // Read password
    required_size = MAX_PASSWORD_LEN;
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, password, &required_size);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Failed to read password from NVS");
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded credentials: SSID=%s", ssid);
    return ESP_OK;
}

static esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID");
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password");
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS");
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Saved credentials: SSID=%s", ssid);
    return err;
}

esp_err_t app_wifi_erase_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return err;
    }

    err = nvs_erase_key(nvs_handle, NVS_KEY_SSID);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase SSID");
    }

    err = nvs_erase_key(nvs_handle, NVS_KEY_PASSWORD);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase password");
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS");
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Erased credentials");
    return ESP_OK;
}

esp_err_t app_wifi_get_saved_ssid(char *ssid, size_t max_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t required_size = max_len;

    if (!ssid || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &required_size);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ssid[0] = '\0';
    }

    return err;
}

/* Wi-Fi 6 Protocol Configuration */
static void wifi_set_protocol_11ax(wifi_interface_t interface)
{
    uint8_t protocol_bitmap = WIFI_PROTOCOL_11AX | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11B;
    esp_err_t ret = esp_wifi_remote_set_protocol(interface, protocol_bitmap);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi 6 (802.11ax) protocol enabled for interface %d", interface);
    } else {
        ESP_LOGW(TAG, "Failed to set Wi-Fi 6 protocol: %s", esp_err_to_name(ret));
    }
}

/* Wi-Fi Remote Initialization (ESP32-C6 via SDIO) */
static void wifi_remote_init(void)
{
    // Note: esp_hosted component initialization may be handled automatically
    // or may require specific initialization based on hardware configuration.
    // This is a placeholder - adjust based on actual esp_hosted API requirements.
    ESP_LOGI(TAG, "Initializing Wi-Fi remote module (ESP32-C6)");
    // If esp_hosted requires explicit initialization, add it here
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_remote_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_remote_connect failed: %s", esp_err_to_name(err));
        }
    } else if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_err_t err = esp_wifi_remote_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_remote_connect failed: %s", esp_err_to_name(err));
            }
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (attempt %d/%d)", s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY);
        } else {
            ESP_LOGI(TAG, "connect to the AP failed after %d attempts", EXAMPLE_ESP_MAXIMUM_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Stop captive portal server if running (to avoid port 80 conflict)
        if (s_captive_portal_server != NULL) {
            ESP_LOGI(TAG, "Stopping captive portal server");
            httpd_stop(s_captive_portal_server);
            s_captive_portal_server = NULL;
        }
        
        // Initialize app state and start REST API after STA gets IP
        ESP_LOGI(TAG, "STA connected, initializing app services");
        app_state_init();
        esp_err_t api_err = http_api_start();
        if (api_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP API: %s", esp_err_to_name(api_err));
            app_state_enter_error();
        } else {
            // Call callback to register action handlers
            if (s_rest_start_callback) {
                s_rest_start_callback();
            }
            app_state_enter_ready();
            ESP_LOGI(TAG, "REST API started at http://p3a.local/");
        }
    }
}

static bool wifi_init_sta(const char *ssid, const char *password)
{
    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_remote_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_REMOTE_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };
    // Copy SSID and password with guaranteed null termination
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    if (ssid_len >= sizeof(wifi_config.sta.ssid)) {
        ssid_len = sizeof(wifi_config.sta.ssid) - 1;
    }
    if (password_len >= sizeof(wifi_config.sta.password)) {
        password_len = sizeof(wifi_config.sta.password) - 1;
    }
    memcpy((char*)wifi_config.sta.ssid, ssid, ssid_len);
    wifi_config.sta.ssid[ssid_len] = '\0';
    memcpy((char*)wifi_config.sta.password, password, password_len);
    wifi_config.sta.password[password_len] = '\0';

    ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_remote_start());
    
    // Enable Wi-Fi 6 protocol (best-effort)
    wifi_set_protocol_11ax(WIFI_IF_STA);

    ESP_LOGI(TAG, "wifi_init_sta finished. Connecting to SSID:%s", ssid);

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000)); // 30 second timeout

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", ssid);
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s after %d attempts", ssid, EXAMPLE_ESP_MAXIMUM_RETRY);
        return false;
    } else {
        ESP_LOGW(TAG, "Connection timeout");
        return false;
    }
}

/* Captive Portal HTML */
static const char* captive_portal_html = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>ESP32 WiFi Configuration</title>"
"<style>"
"body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }"
".container { max-width: 400px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
"h1 { color: #333; text-align: center; }"
"input[type=text], input[type=password] { width: 100%; padding: 12px; margin: 8px 0; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }"
"button { background-color: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; width: 100%; margin: 5px 0; }"
"button:hover { background-color: #45a049; }"
".erase-btn { background-color: #f44336; }"
".erase-btn:hover { background-color: #da190b; }"
"</style>"
"</head>"
"<body>"
"<div class=\"container\">"
"<h1>WiFi Configuration</h1>"
"<form action=\"/save\" method=\"POST\">"
"<label for=\"ssid\">SSID:</label>"
"<input type=\"text\" id=\"ssid\" name=\"ssid\" required>"
"<label for=\"password\">Password:</label>"
"<input type=\"password\" id=\"password\" name=\"password\">"
"<button type=\"submit\">Save & Connect</button>"
"</form>"
"<form action=\"/erase\" method=\"POST\">"
"<button type=\"submit\" class=\"erase-btn\">Erase Saved Credentials</button>"
"</form>"
"</div>"
"</body>"
"</html>";

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

/* HTTP Server Handlers */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, captive_portal_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char content[200];
    size_t recv_size = sizeof(content) - 1;
    
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Parse SSID and password from form data
    char ssid[MAX_SSID_LEN] = {0};
    char password[MAX_PASSWORD_LEN] = {0};
    
    // Simple form parsing
    char *ssid_start = strstr(content, "ssid=");
    char *password_start = strstr(content, "password=");
    
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

    if (strlen(ssid) > 0) {
        wifi_save_credentials(ssid, password);
        ESP_LOGI(TAG, "Saved credentials, rebooting...");
        httpd_resp_send(req, "<html><body><h1>Credentials saved! Rebooting...</h1></body></html>", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        httpd_resp_send(req, "<html><body><h1>Error: SSID required</h1></body></html>", HTTPD_RESP_USE_STRLEN);
    }
    
    return ESP_OK;
}

static esp_err_t erase_post_handler(httpd_req_t *req)
{
    app_wifi_erase_credentials();
    ESP_LOGI(TAG, "Erased credentials, rebooting...");
    httpd_resp_send(req, "<html><body><h1>Credentials erased! Rebooting...</h1></body></html>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
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

/* DNS Server for Captive Portal */
static void dns_server_task(void *pvParameters)
{
    char rx_buffer[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_UDP;

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "DNS socket unable to bind");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started");

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            // Continue on error
            continue;
        }

        // For captive portal, we acknowledge DNS queries
        // The actual redirection happens at the HTTP level when devices try to access the internet
        // This is a minimal implementation - proper DNS response would parse and respond correctly
    }

    if (sock != -1) {
        close(sock);
    }
    vTaskDelete(NULL);
}

/* Start Captive Portal */
static void start_captive_portal(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    if (httpd_start(&s_captive_portal_server, &config) == ESP_OK) {
        httpd_register_uri_handler(s_captive_portal_server, &root);
        httpd_register_uri_handler(s_captive_portal_server, &save);
        httpd_register_uri_handler(s_captive_portal_server, &erase);
        ESP_LOGI(TAG, "HTTP server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }

    // Start DNS server task
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}

/* Soft AP Initialization with Wi-Fi 6 */
static void wifi_init_softap(void)
{
    // Stop WiFi if it was already started (ignore errors if not initialized)
    esp_err_t err = esp_wifi_remote_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_remote_stop failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_remote_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_remote_deinit failed: %s", esp_err_to_name(err));
    }
    
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
    
    // Enable Wi-Fi 6 protocol for AP (best-effort)
    wifi_set_protocol_11ax(WIFI_IF_AP);

    ESP_LOGI(TAG, "Soft AP initialized. SSID:%s password:%s", EXAMPLE_ESP_AP_SSID, 
             strlen(EXAMPLE_ESP_AP_PASSWORD) > 0 ? EXAMPLE_ESP_AP_PASSWORD : "none");

    // Configure AP IP address
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    ESP_LOGI(TAG, "AP IP address: " IPSTR, IP2STR(&ip_info.ip));

    // Start captive portal
    start_captive_portal();
}

esp_err_t app_wifi_init(app_wifi_rest_callback_t rest_callback)
{
    s_rest_start_callback = rest_callback;

    // Initialize Wi-Fi remote module (ESP32-C6 via SDIO)
    wifi_remote_init();

    // Try to load saved credentials
    char saved_ssid[MAX_SSID_LEN] = {0};
    char saved_password[MAX_PASSWORD_LEN] = {0};
    
    bool has_credentials = (wifi_load_credentials(saved_ssid, saved_password) == ESP_OK);
    
    if (has_credentials && strlen(saved_ssid) > 0) {
        ESP_LOGI(TAG, "Found saved credentials, attempting to connect...");
        bool connected = wifi_init_sta(saved_ssid, saved_password);
        
        if (connected) {
            ESP_LOGI(TAG, "Successfully connected to WiFi network");
            return ESP_OK;
        } else {
            ESP_LOGI(TAG, "Failed to connect with saved credentials, starting captive portal");
        }
    } else {
        ESP_LOGI(TAG, "No saved credentials found, starting captive portal");
    }

    // Start Soft AP with captive portal
    wifi_init_softap();
    
    ESP_LOGI(TAG, "Captive portal is running. Connect to SSID: %s", EXAMPLE_ESP_AP_SSID);
    ESP_LOGI(TAG, "Then open http://192.168.4.1 in your browser");
    return ESP_OK;
}

