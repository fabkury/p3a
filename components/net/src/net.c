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
#include "esp_hosted.h"
#include "esp_check.h"

static const char *TAG = "net";

static bool s_net_initialized = false;
// s_wifi_state is managed by wifi_sta.c
extern net_wifi_state_t s_wifi_state;

esp_err_t net_init(void)
{
    if (s_net_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing networking subsystem...");

    // Initialize ESP-NETIF (brings up LWIP/TCPIP stack)
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");

    // Create default event loop if not already created
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create event loop");

    // Initialize ESP-Hosted transport (SDIO to ESP32-C6)
    ESP_LOGI(TAG, "Initializing ESP-Hosted...");
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "esp_hosted_init failed");

    // Connect to slave (ESP32-C6)
    ESP_LOGI(TAG, "Connecting to ESP-Hosted slave...");
    ESP_RETURN_ON_ERROR(esp_hosted_connect_to_slave(), TAG, "esp_hosted_connect_to_slave failed");

    // Create default STA netif (required before wifi_init)
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "Failed to create default STA netif");
        return ESP_FAIL;
    }

    // Create default AP netif (for provisioning)
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "Failed to create default AP netif");
        return ESP_FAIL;
    }

    // Initialize Wi-Fi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_remote_init(&cfg), TAG, "esp_wifi_remote_init failed");

    ESP_LOGI(TAG, "Networking subsystem initialized successfully");
    s_net_initialized = true;

    return ESP_OK;
}

net_wifi_state_t net_wifi_get_state(void)
{
    extern net_wifi_state_t s_wifi_state;
    return s_wifi_state;
}

bool net_wifi_is_connected(void)
{
    return s_wifi_state == NET_WIFI_STATE_CONNECTED;
}

esp_err_t net_wifi_get_ssid(char *ssid, size_t max_len)
{
    if (!ssid || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};
    esp_err_t ret = esp_wifi_remote_get_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = strlen((const char *)wifi_config.sta.ssid);
    if (len >= max_len) {
        len = max_len - 1;
    }
    memcpy(ssid, wifi_config.sta.ssid, len);
    ssid[len] = '\0';

    return ESP_OK;
}
