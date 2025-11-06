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
#include "esp_netif.h"
#include "storage/kv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "net_sta";

#define MAX_RETRY 3
#define CONNECTED_BIT BIT0
#define FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;
net_wifi_state_t s_wifi_state = NET_WIFI_STATE_DISCONNECTED;  // Exported for net.c

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_REMOTE_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            s_wifi_state = NET_WIFI_STATE_CONNECTING;
            esp_wifi_remote_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected to AP");
            s_retry_count = 0;
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "STA disconnected");
            s_wifi_state = NET_WIFI_STATE_DISCONNECTED;
            
            if (s_retry_count < MAX_RETRY) {
                esp_wifi_remote_connect();
                s_retry_count++;
                ESP_LOGI(TAG, "Retrying connection (%d/%d)", s_retry_count, MAX_RETRY);
            } else {
                ESP_LOGW(TAG, "Max retries (%d) reached, connection failed. Starting provisioning mode...", MAX_RETRY);
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, FAIL_BIT);
                }
                // Stop STA mode and start provisioning
                esp_wifi_remote_stop();
                esp_err_t prov_ret = net_wifi_start_provisioning();
                if (prov_ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start provisioning after connection failure: %s", esp_err_to_name(prov_ret));
                } else {
                    ESP_LOGI(TAG, "Provisioning mode started successfully");
                }
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            {
                ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
                s_wifi_state = NET_WIFI_STATE_CONNECTED;
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
                }
            }
            break;

        default:
            break;
        }
    }
}

static bool wifi_load_credentials(wifi_config_t *wifi_config)
{
    void *kv_handle = storage_kv_open_namespace("wifi", "readonly");
    if (!kv_handle) {
        ESP_LOGI(TAG, "No Wi-Fi credentials found in NVS");
        return false;
    }

    char ssid[64] = {0};
    char password[64] = {0};

    esp_err_t ret = storage_kv_get_str(kv_handle, "ssid", ssid, sizeof(ssid));
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No SSID found in NVS");
        storage_kv_close_namespace(kv_handle);
        return false;
    }

    storage_kv_get_str(kv_handle, "password", password, sizeof(password));
    storage_kv_close_namespace(kv_handle);

    memset(wifi_config, 0, sizeof(wifi_config_t));
    strncpy((char *)wifi_config->sta.ssid, ssid, sizeof(wifi_config->sta.ssid) - 1);
    strncpy((char *)wifi_config->sta.password, password, sizeof(wifi_config->sta.password) - 1);

    // Configure for WPA3 and Wi-Fi 6
    // Set threshold to prefer WPA3 (will connect to WPA2_WPA3_PSK or WPA3_PSK)
    wifi_config->sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    // Configure SAE (WPA3) PWE method to support both Hunt and Peck and H2E
    wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    // Enable PMF (Protected Management Frames) required for WPA3
    wifi_config->sta.pmf_cfg.capable = true;
    wifi_config->sta.pmf_cfg.required = false; // Allow fallback to WPA2 if needed

    ESP_LOGI(TAG, "Loaded credentials: SSID='%s' (WPA3/Wi-Fi 6 enabled)", ssid);
    return true;
}

esp_err_t net_wifi_connect(void)
{
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT | FAIL_BIT);
    s_retry_count = 0;

    // Register event handlers if not already registered
    static bool handlers_registered = false;
    if (!handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &wifi_event_handler, NULL));
        handlers_registered = true;
    }

    // Load credentials from NVS
    wifi_config_t wifi_config = {0};
    bool has_creds = wifi_load_credentials(&wifi_config);

    if (!has_creds) {
        ESP_LOGI(TAG, "No credentials found, starting provisioning...");
        // Start provisioning instead
        esp_err_t ret = net_wifi_start_provisioning();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start provisioning");
            return ret;
        }
        return ESP_OK;
    }

    // Configure and start STA
    ESP_LOGI(TAG, "Configuring STA mode with WPA3 and Wi-Fi 6...");
    ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config));
    
    // Enable Wi-Fi 6 (802.11ax) protocol
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    esp_err_t protocol_ret = esp_wifi_remote_set_protocols(WIFI_IF_STA, &protocols);
    if (protocol_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set Wi-Fi 6 protocol (may not be supported): %s", esp_err_to_name(protocol_ret));
    } else {
        ESP_LOGI(TAG, "Wi-Fi 6 (802.11ax) protocol enabled");
    }
    
    ESP_ERROR_CHECK(esp_wifi_remote_start());

    s_wifi_state = NET_WIFI_STATE_CONNECTING;

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          CONNECTED_BIT | FAIL_BIT,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(30000)); // 30 second timeout

    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected successfully");
        return ESP_OK;
    } else if (bits & FAIL_BIT) {
        ESP_LOGW(TAG, "Wi-Fi connection failed after retries");
        return ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "Wi-Fi connection timeout");
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t net_wifi_disconnect(void)
{
    s_wifi_state = NET_WIFI_STATE_DISCONNECTED;
    return esp_wifi_remote_disconnect();
}

