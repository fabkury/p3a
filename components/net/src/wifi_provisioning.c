#include "net.h"

// CRITICAL: Include injected headers BEFORE esp_wifi_remote.h
// This must be done via a wrapper to ensure esp_wifi.h is found
// when esp_wifi_remote.h includes it
#include "injected/esp_wifi.h"
#include "esp_wifi_remote.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "storage/kv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// Forward declaration - state managed by wifi_sta.c
extern net_wifi_state_t s_wifi_state;

static const char *TAG = "net_prov";

#define PROV_SSID_PREFIX "P3A-Prov"
#define PROV_PASSWORD "p3a-setup-2024"
#define PROV_MAX_RETRY 5

static bool s_provisioning_active = false;
static httpd_handle_t s_httpd = NULL;
static EventGroupHandle_t s_prov_event_group;
#define PROV_COMPLETE_BIT BIT0
#define PROV_AP_STARTED_BIT BIT1

static esp_err_t prov_handler_root(httpd_req_t *req)
{
    const char *html = 
        "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>P3A Setup</title></head><body>"
        "<h1>P3A Wi-Fi Setup</h1>"
        "<form method=\"post\" action=\"/connect\" enctype=\"application/x-www-form-urlencoded\">"
        "<p><label>SSID:<br><input type=\"text\" name=\"ssid\" required></label></p>"
        "<p><label>Password:<br><input type=\"password\" name=\"password\"></label></p>"
        "<p><button type=\"submit\">Connect</button></p>"
        "</form></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t prov_handler_connect(httpd_req_t *req)
{
    char content[512];
    size_t recv_size = sizeof(content) - 1;
    char ssid[64] = {0};
    char password[64] = {0};

    // Handle POST request
    if (req->method == HTTP_POST) {
        int ret = httpd_req_recv(req, content, recv_size);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }

        // Parse form data
        char *ptr = strstr(content, "ssid=");
        if (ptr) {
            ptr += 5; // Skip "ssid="
            size_t i = 0;
            while (*ptr && *ptr != '&' && i < sizeof(ssid) - 1) {
                if (*ptr == '+') {
                    ssid[i++] = ' ';
                } else if (*ptr == '%') {
                    // Simple URL decode (basic only)
                    ssid[i++] = ' ';
                } else {
                    ssid[i++] = *ptr;
                }
                ptr++;
            }
            ssid[i] = '\0';
        }

        ptr = strstr(content, "password=");
        if (ptr) {
            ptr += 9; // Skip "password="
            size_t i = 0;
            while (*ptr && *ptr != '&' && i < sizeof(password) - 1) {
                if (*ptr == '+') {
                    password[i++] = ' ';
                } else if (*ptr == '%') {
                    // Simple URL decode (basic only)
                    password[i++] = ' ';
                } else {
                    password[i++] = *ptr;
                }
                ptr++;
            }
            password[i] = '\0';
        }
    } else {
        // Handle GET request (query string)
        if (httpd_req_get_url_query_str(req, content, recv_size) == ESP_OK) {
            httpd_query_key_value(content, "ssid", ssid, sizeof(ssid));
            httpd_query_key_value(content, "password", password, sizeof(password));
        }
    }

    ESP_LOGI(TAG, "Provisioning request: SSID='%s', password len=%zu", ssid, strlen(password));

    if (strlen(ssid) > 0) {
        // Save credentials to NVS
        void *kv_handle = storage_kv_open_namespace("wifi", "rw");
        if (kv_handle) {
            storage_kv_set_str(kv_handle, "ssid", ssid);
            storage_kv_set_str(kv_handle, "password", password);
            storage_kv_close_namespace(kv_handle);
            ESP_LOGI(TAG, "Credentials saved to NVS");
        }

        // Signal provisioning complete
        xEventGroupSetBits(s_prov_event_group, PROV_COMPLETE_BIT);

        const char *response = 
            "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>P3A Setup</title></head><body>"
            "<h1>Provisioning Complete</h1>"
            "<p>Connecting to network...</p>"
            "<p>You can close this page.</p></body></html>";

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const char *response = 
        "<!DOCTYPE html><html><head><title>Error</title></head><body>"
        "<h1>Error</h1><p>Invalid request</p></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t prov_start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    if (httpd_start(&s_httpd, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = prov_handler_root,
        };
        httpd_register_uri_handler(s_httpd, &root_uri);

        httpd_uri_t connect_uri = {
            .uri = "/connect",
            .method = HTTP_GET | HTTP_POST,
            .handler = prov_handler_connect,
        };
        httpd_register_uri_handler(s_httpd, &connect_uri);

        return ESP_OK;
    }

    return ESP_FAIL;
}

static void prov_stop_http_server(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

static void prov_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Provisioning task started");

    // Wait for provisioning complete or timeout
    EventBits_t bits = xEventGroupWaitBits(s_prov_event_group, PROV_COMPLETE_BIT,
                                          pdFALSE, pdFALSE, pdMS_TO_TICKS(300000)); // 5 min timeout

    if (bits & PROV_COMPLETE_BIT) {
        ESP_LOGI(TAG, "Provisioning completed, stopping SoftAP");
    } else {
        ESP_LOGW(TAG, "Provisioning timeout");
    }

    // Stop HTTP server
    prov_stop_http_server();

    // Stop SoftAP
    esp_wifi_remote_set_mode(WIFI_MODE_STA);
    esp_wifi_remote_stop();

    s_provisioning_active = false;
    extern net_wifi_state_t s_wifi_state;
    s_wifi_state = NET_WIFI_STATE_DISCONNECTED;
    
    // If provisioning completed successfully, trigger connection attempt
    if (bits & PROV_COMPLETE_BIT) {
        ESP_LOGI(TAG, "Attempting to connect with new credentials...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Small delay before switching modes
        
        // Trigger connection attempt (will be handled by main or auto-reconnect)
        net_wifi_connect();
    }
    
    vTaskDelete(NULL);
}

static void prov_wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started, signaling HTTP server can start");
        if (s_prov_event_group) {
            xEventGroupSetBits(s_prov_event_group, PROV_AP_STARTED_BIT);
        }
    }
}

esp_err_t net_wifi_start_provisioning(void)
{
    if (s_provisioning_active) {
        ESP_LOGW(TAG, "Provisioning already active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting Wi-Fi provisioning...");

    // Create event group for provisioning status
    if (!s_prov_event_group) {
        s_prov_event_group = xEventGroupCreate();
        if (!s_prov_event_group) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_prov_event_group, PROV_COMPLETE_BIT | PROV_AP_STARTED_BIT);

    // Register handler for AP_START event
    esp_event_handler_instance_t ap_start_handler;
    esp_event_handler_instance_register(WIFI_REMOTE_EVENT, WIFI_EVENT_AP_START,
                                       prov_wifi_event_handler, NULL, &ap_start_handler);

    // Configure SoftAP
    wifi_config_t ap_config = {
        .ap = {
            .ssid = PROV_SSID_PREFIX,
            .ssid_len = strlen(PROV_SSID_PREFIX),
            .password = PROV_PASSWORD,
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_remote_start());

    ESP_LOGI(TAG, "SoftAP started: SSID='%s', password='%s'", PROV_SSID_PREFIX, PROV_PASSWORD);

    // Wait for AP_START event or timeout (5 seconds)
    (void)xEventGroupWaitBits(s_prov_event_group, PROV_AP_STARTED_BIT,
                              pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

    // Unregister handler
    esp_event_handler_instance_unregister(WIFI_REMOTE_EVENT, WIFI_EVENT_AP_START, ap_start_handler);

    // Wait for AP netif to be ready - check if it has an IP assigned
    // For SoftAP, this should happen automatically, but we need to wait for LWIP to initialize
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_RMT");
    }
    
    int retries = 50; // 5 seconds total
    bool netif_ready = false;
    while (retries-- > 0 && !netif_ready) {
        if (ap_netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
                // Check if IP is valid (not 0.0.0.0)
                if (ip_info.ip.addr != 0) {
                    ESP_LOGI(TAG, "AP netif ready, IP: " IPSTR, IP2STR(&ip_info.ip));
                    netif_ready = true;
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (!netif_ready) {
        ESP_LOGW(TAG, "AP netif not ready after timeout, starting HTTP server anyway");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Extra delay for LWIP
    } else {
        // Even if netif is ready, give LWIP TCP/IP task more time to initialize
        // The netif having an IP doesn't guarantee LWIP's TCP/IP task mailbox is ready
        ESP_LOGI(TAG, "Waiting additional 500ms for LWIP TCP/IP task initialization...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Start HTTP server for captive portal
    esp_err_t ret = prov_start_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        esp_wifi_remote_stop();
        return ret;
    }

    s_provisioning_active = true;
    s_wifi_state = NET_WIFI_STATE_PROVISIONING;

    // Start provisioning task
    xTaskCreate(prov_task, "prov_task", 4096, NULL, 5, NULL);

    return ESP_OK;
}

esp_err_t net_wifi_stop_provisioning(void)
{
    if (!s_provisioning_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping Wi-Fi provisioning...");

    prov_stop_http_server();

    esp_wifi_remote_set_mode(WIFI_MODE_STA);
    esp_wifi_remote_stop();

    s_provisioning_active = false;

    return ESP_OK;
}

