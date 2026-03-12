// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

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
#include "http_api.h"
#include "app_wifi.h"
#include "sntp_sync.h"
#include "makapix.h"
#include "makapix_mqtt.h"
#include "makapix_channel_events.h"
#include "mdns.h"
#include "p3a_state.h"
#include "event_bus.h"
#include "esp_heap_caps.h"
#include "config_store.h"
#include "wifi_manager_internal.h"

#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY
#define EXAMPLE_ESP_AP_SSID        CONFIG_ESP_AP_SSID
#define EXAMPLE_ESP_AP_PASSWORD    CONFIG_ESP_AP_PASSWORD

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
EventGroupHandle_t s_wifi_event_group;

static const char *TAG = "app_wifi";

int s_retry_num = 0;
httpd_handle_t s_captive_portal_server = NULL;
esp_netif_t *ap_netif = NULL;
bool s_initial_connection_done = false;  // Track if we've ever successfully connected
bool s_services_initialized = false;     // Track if app services have been initialized
int s_consecutive_wifi_errors = 0;
const int s_max_consecutive_wifi_errors = 10;
bool s_mdns_service_added = false;       // Track if mDNS HTTP service has been registered
int s_no_ip_health_cycles = 0;           // Health monitor cycles with no IP after initial connection

// Register WiFi/IP event handlers once; repeated registration causes duplicate callbacks.
static bool s_event_handlers_registered = false;
static esp_event_handler_instance_t s_instance_wifi_any_id;
static esp_event_handler_instance_t s_instance_ip_any_id;

// WiFi health monitor + recovery worker
static TaskHandle_t s_wifi_health_task = NULL;
TaskHandle_t s_wifi_recovery_task = NULL;
bool s_reinit_in_progress = false;

// PSRAM-backed stacks for WiFi tasks
static StackType_t *s_wifi_recovery_stack = NULL;
static StaticTask_t s_wifi_recovery_task_buffer;
static StackType_t *s_wifi_health_stack = NULL;
static StaticTask_t s_wifi_health_task_buffer;

// Hard recovery escalation (C6 WiFi stack permanently broken)
int s_total_recovery_failures = 0;

// Forward declaration (event_handler referenced before definition)
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data);

/* NVS Credential Storage Functions */
esp_err_t wifi_load_credentials(char *ssid, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t required_size;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No saved credentials found");
        return err;
    }

    // Read SSID
    required_size = MAX_SSID_LEN;
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &required_size);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Failed to read SSID from NVS");
        nvs_close(nvs_handle);
        return err;
    }

    // Read password
    required_size = MAX_PASSWORD_LEN;
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, password, &required_size);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Failed to read password from NVS");
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);
    ESP_LOGD(TAG, "Loaded credentials: SSID=%s", ssid);
    return ESP_OK;
}

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
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
    ESP_LOGD(TAG, "Saved credentials: SSID=%s", ssid);
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
    ESP_LOGD(TAG, "Erased credentials");
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
void wifi_set_protocol_11ax(wifi_interface_t interface)
{
    uint8_t protocol_bitmap = WIFI_PROTOCOL_11AX | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11B;
    esp_wifi_remote_set_protocol(interface, protocol_bitmap);
}

/* Wi-Fi Remote Initialization (ESP32-C6 via SDIO) */
static void wifi_remote_init(void)
{
    // esp_hosted component initialization handled by component
}

void wifi_register_event_handlers_once(void)
{
    if (s_event_handlers_registered) {
        return;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_REMOTE_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &s_instance_wifi_any_id));

    // Register for all IP events to handle both IP_EVENT_STA_GOT_IP and IP_EVENT_STA_LOST_IP
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &s_instance_ip_any_id));

    s_event_handlers_registered = true;
}

bool wifi_sta_has_ip(void)
{
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
    if (!sta_netif) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta_netif, &ip_info) != ESP_OK) {
        return false;
    }
    return ip_info.ip.addr != 0;
}

void wifi_disable_power_save_best_effort(void)
{
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_reinit_in_progress) {
            ESP_LOGD(TAG, "STA_START during recovery - skipping connect (recovery task will handle)");
        } else {
            esp_err_t err = esp_wifi_remote_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_remote_connect failed: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected (initial_connection_done=%d, retry=%d)",
                 s_initial_connection_done, s_retry_num);

        // Signal WiFi disconnection (always, before possible reinit)
        makapix_channel_signal_wifi_disconnected();
        event_bus_emit_simple(P3A_EVENT_WIFI_DISCONNECTED);
        if (s_initial_connection_done) {
            ESP_LOGD(TAG, "Stopping MQTT client due to WiFi disconnect");
            makapix_mqtt_disconnect();
        }

        // Track consecutive errors and attempt a full reinit if we get stuck cycling.
        // Reset occurs on GOT_IP.
        s_consecutive_wifi_errors++;
        if (s_consecutive_wifi_errors >= s_max_consecutive_wifi_errors) {
            ESP_LOGE(TAG, "Too many consecutive WiFi errors (%d) - scheduling full re-init", s_consecutive_wifi_errors);
            s_consecutive_wifi_errors = 0;
            wifi_schedule_full_reinit();
            return;
        }

        // For initial connection: use retry limit
        // After initial connection succeeded: always keep trying (persistent reconnection)
        if (!s_initial_connection_done && s_retry_num >= EXAMPLE_ESP_MAXIMUM_RETRY) {
            ESP_LOGD(TAG, "Initial connection failed after %d attempts", EXAMPLE_ESP_MAXIMUM_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else {
            // Always try to reconnect
            s_retry_num++;

            // Add delay for persistent reconnection to avoid hammering the AP
            if (s_initial_connection_done && s_retry_num > 5) {
                // After 5 quick retries, slow down to every 5 seconds
                ESP_LOGD(TAG, "WiFi reconnect attempt %d (with backoff)", s_retry_num);
                vTaskDelay(pdMS_TO_TICKS(5000));
            } else {
                ESP_LOGD(TAG, "WiFi reconnect attempt %d/%d", s_retry_num,
                         s_initial_connection_done ? -1 : EXAMPLE_ESP_MAXIMUM_RETRY);
            }

            esp_err_t err = esp_wifi_remote_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_remote_connect failed: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_consecutive_wifi_errors = 0;
        s_total_recovery_failures = 0;
        config_store_reset_wifi_reboot_streak();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        makapix_channel_signal_wifi_connected();

        // Emit WiFi connected event (will trigger connectivity tracking)
        event_bus_emit_simple(P3A_EVENT_WIFI_CONNECTED);

        // Ensure mDNS is announced
        {
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
            if (sta_netif) {
                mdns_netif_action(sta_netif,
                                  (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
            }
        }

        // Stop captive portal if running
        if (s_captive_portal_server != NULL) {
            httpd_stop(s_captive_portal_server);
            s_captive_portal_server = NULL;
            // Allow socket cleanup time before starting new server on same port
            // See: https://github.com/espressif/esp-idf/issues/3381
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Initialize app services once
        if (!s_services_initialized) {
            sntp_sync_init();
            // Start HTTP API with retry logic for transient port binding failures
            esp_err_t api_err = ESP_FAIL;
            const int max_retries = 3;
            for (int attempt = 0; attempt < max_retries && api_err != ESP_OK; attempt++) {
                if (attempt > 0) {
                    ESP_LOGW(TAG, "HTTP API start retry %d/%d", attempt + 1, max_retries);
                    vTaskDelay(pdMS_TO_TICKS(200 * attempt));
                }
                api_err = http_api_start();
            }

            if (api_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start HTTP API after %d attempts: %s",
                         max_retries, esp_err_to_name(api_err));
                p3a_state_enter_app_error();
            } else {
                p3a_state_enter_ready();
                ESP_LOGD(TAG, "HTTP ready at http://p3a.local/");
            }
            s_services_initialized = true;
        }

        s_initial_connection_done = true;
        makapix_connect_if_registered();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "IP lost - reconnecting");
        makapix_mqtt_disconnect();
        if (!s_reinit_in_progress) {
            esp_wifi_remote_disconnect();
        } else {
            ESP_LOGW(TAG, "IP lost during reinit - skipping disconnect (recovery in progress)");
        }
    }
}

/* Start mDNS in STA mode so p3a.local works on the LAN */
static esp_err_t start_mdns_sta(void)
{
    char hostname[24];
    config_store_get_hostname(hostname, sizeof(hostname));

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    mdns_hostname_set(hostname);
    mdns_instance_name_set(hostname);

    // Only add service if not already added
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

    return ESP_OK;
}

static bool wifi_init_sta(const char *ssid, const char *password)
{
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    } else {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    s_retry_num = 0;

    esp_netif_create_default_wifi_sta();

    // Set a hostname on the STA netif as a secondary discovery mechanism (DHCP/DNS on some networks).
    // This does NOT replace mDNS (.local), but helps on LANs that publish DHCP hostnames.
    {
        char hostname[24];
        config_store_get_hostname(hostname, sizeof(hostname));
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
        if (sta_netif) {
            esp_err_t herr = esp_netif_set_hostname(sta_netif, hostname);
            if (herr != ESP_OK) {
                ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s", esp_err_to_name(herr));
            }
        }
    }

    start_mdns_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_remote_init(&cfg));
    wifi_register_event_handlers_once();

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

    // Recommendation 1: disable WiFi power save for reliability (USB-powered device).
    wifi_disable_power_save_best_effort();

    // Enable Wi-Fi 6 protocol (best-effort)
    wifi_set_protocol_11ax(WIFI_IF_STA);

    ESP_LOGD(TAG, "Connecting to: %s", ssid);

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000)); // 30 second timeout

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGD(TAG, "Connected to: %s", ssid);
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Failed to connect after %d attempts", EXAMPLE_ESP_MAXIMUM_RETRY);
        return false;
    } else {
        ESP_LOGW(TAG, "Connection timeout");
        return false;
    }
}

esp_err_t app_wifi_init(void)
{
    // Initialize Wi-Fi remote module (ESP32-C6 via SDIO)
    wifi_remote_init();

    // Start recovery worker once (used by Recommendation 3) with SPIRAM-backed stack
    if (!s_wifi_recovery_task) {
        const size_t wifi_recovery_stack_size = 4096;
        if (!s_wifi_recovery_stack) {
            s_wifi_recovery_stack = heap_caps_malloc(wifi_recovery_stack_size * sizeof(StackType_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }

        bool task_created = false;
        if (s_wifi_recovery_stack) {
            s_wifi_recovery_task = xTaskCreateStatic(wifi_recovery_task, "wifi_recovery",
                                                      wifi_recovery_stack_size, NULL, CONFIG_P3A_APP_TASK_PRIORITY,
                                                      s_wifi_recovery_stack, &s_wifi_recovery_task_buffer);
            task_created = (s_wifi_recovery_task != NULL);
        }

        if (!task_created) {
            xTaskCreate(wifi_recovery_task, "wifi_recovery",
                        wifi_recovery_stack_size, NULL, CONFIG_P3A_APP_TASK_PRIORITY, &s_wifi_recovery_task);
        }
    }

    // Start WiFi health monitor once (Recommendation 2) with SPIRAM-backed stack
    if (!s_wifi_health_task) {
        const size_t wifi_health_stack_size = 4096;
        if (!s_wifi_health_stack) {
            s_wifi_health_stack = heap_caps_malloc(wifi_health_stack_size * sizeof(StackType_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }

        bool task_created = false;
        if (s_wifi_health_stack) {
            s_wifi_health_task = xTaskCreateStatic(wifi_health_monitor_task, "wifi_health",
                                                    wifi_health_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY,
                                                    s_wifi_health_stack, &s_wifi_health_task_buffer);
            task_created = (s_wifi_health_task != NULL);
        }

        if (!task_created) {
            xTaskCreate(wifi_health_monitor_task, "wifi_health",
                        wifi_health_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_wifi_health_task);
        }
    }

    // Try to load saved credentials
    char saved_ssid[MAX_SSID_LEN] = {0};
    char saved_password[MAX_PASSWORD_LEN] = {0};

    bool has_credentials = (wifi_load_credentials(saved_ssid, saved_password) == ESP_OK);

    if (has_credentials && strlen(saved_ssid) > 0) {
        bool connected = wifi_init_sta(saved_ssid, saved_password);
        if (connected) {
            return ESP_OK;
        }
    }

    // Start captive portal
    ESP_LOGD(TAG, "Starting captive portal: %s", EXAMPLE_ESP_AP_SSID);
    wifi_init_softap();
    return ESP_OK;
}

bool app_wifi_is_captive_portal_active(void)
{
    return s_captive_portal_server != NULL;
}

esp_err_t app_wifi_get_local_ip(char *ip_str, size_t max_len)
{
    if (!ip_str || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ip_str[0] = '\0';

    // Check if in captive portal (AP) mode
    if (s_captive_portal_server != NULL && ap_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, max_len, IPSTR, IP2STR(&ip_info.ip));
            return ESP_OK;
        }
    }

    // Check if in STA mode with connection
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
    if (sta_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {  // Check if IP is assigned
                snprintf(ip_str, max_len, IPSTR, IP2STR(&ip_info.ip));
                return ESP_OK;
            }
        }
    }

    return ESP_ERR_NOT_FOUND;
}
