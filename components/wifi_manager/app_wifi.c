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
#include "sntp_sync.h"
#include "makapix.h"
#include "makapix_mqtt.h"
#include "mdns.h"

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

// ESP-IDF default interface key for WiFi STA (used by esp_netif_create_default_wifi_sta)
#define WIFI_STA_NETIF_KEY         "WIFI_STA_DEF"

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
static bool s_initial_connection_done = false;  // Track if we've ever successfully connected
static bool s_services_initialized = false;     // Track if app services have been initialized
static int s_consecutive_wifi_errors = 0;
static const int s_max_consecutive_wifi_errors = 10;

// Register WiFi/IP event handlers once; repeated registration causes duplicate callbacks.
static bool s_event_handlers_registered = false;
static esp_event_handler_instance_t s_instance_wifi_any_id;
static esp_event_handler_instance_t s_instance_ip_any_id;

// WiFi health monitor + recovery worker
static TaskHandle_t s_wifi_health_task = NULL;
static TaskHandle_t s_wifi_recovery_task = NULL;
static bool s_reinit_in_progress = false;
static const uint32_t s_wifi_health_interval_ms = 120000; // 120 seconds

// Callback for when REST API should be started (to register action handlers)
static app_wifi_rest_callback_t s_rest_start_callback = NULL;

// Forward declaration (event_handler referenced before definition)
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data);

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

static void wifi_register_event_handlers_once(void)
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

static bool wifi_sta_has_ip(void)
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

static void wifi_disable_power_save_best_effort(void)
{
    // esp_wifi_set_ps() exists in ESP-IDF. With esp_wifi_remote_* it should still apply to the
    // underlying Wi-Fi driver; if not supported it will return an error which we log.
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi power save disabled for reliability (WIFI_PS_NONE)");
    } else {
        ESP_LOGW(TAG, "Failed to disable WiFi power save: %s (continuing with default)", esp_err_to_name(ps_err));
    }
}

static void wifi_recovery_task(void *arg)
{
    (void)arg;
    char saved_ssid[MAX_SSID_LEN] = {0};
    char saved_password[MAX_PASSWORD_LEN] = {0};

    while (true) {
        // Wait until we are notified to perform a full reinit
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGW(TAG, "WiFi recovery: performing full WiFi re-initialization");

        // Best-effort stop/deinit
        esp_err_t err = esp_wifi_remote_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "esp_wifi_remote_stop failed: %s", esp_err_to_name(err));
        }

        err = esp_wifi_remote_deinit();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "esp_wifi_remote_deinit failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));

        // Re-init WiFi remote driver
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_remote_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_remote_init failed during recovery: %s", esp_err_to_name(err));
            s_reinit_in_progress = false;
            continue;
        }

        // Ensure handlers stay registered (registering twice is prevented)
        wifi_register_event_handlers_once();

        // Reload credentials from NVS (user preference)
        bool has_credentials = (wifi_load_credentials(saved_ssid, saved_password) == ESP_OK) && (strlen(saved_ssid) > 0);
        if (!has_credentials) {
            ESP_LOGE(TAG, "WiFi recovery: no saved credentials; cannot restart STA");
            s_reinit_in_progress = false;
            continue;
        }

        // Reconfigure STA and restart (non-blocking; event flow drives reconnect)
        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
                .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
                .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
            },
        };

        size_t ssid_len = strlen(saved_ssid);
        size_t password_len = strlen(saved_password);
        if (ssid_len >= sizeof(wifi_config.sta.ssid)) {
            ssid_len = sizeof(wifi_config.sta.ssid) - 1;
        }
        if (password_len >= sizeof(wifi_config.sta.password)) {
            password_len = sizeof(wifi_config.sta.password) - 1;
        }
        memcpy((char*)wifi_config.sta.ssid, saved_ssid, ssid_len);
        wifi_config.sta.ssid[ssid_len] = '\0';
        memcpy((char*)wifi_config.sta.password, saved_password, password_len);
        wifi_config.sta.password[password_len] = '\0';

        // Clear connection bits before restarting
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        }

        ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_remote_start());

        wifi_disable_power_save_best_effort();
        wifi_set_protocol_11ax(WIFI_IF_STA);

        // Kick connection attempt
        err = esp_wifi_remote_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_remote_connect failed during recovery: %s", esp_err_to_name(err));
        }

        ESP_LOGW(TAG, "WiFi recovery: reinit complete; reconnect will proceed via events");
        s_reinit_in_progress = false;
    }
}

static void wifi_schedule_full_reinit(void)
{
    if (s_reinit_in_progress) {
        ESP_LOGW(TAG, "WiFi recovery: reinit already in progress; ignoring request");
        return;
    }
    if (!s_wifi_recovery_task) {
        ESP_LOGE(TAG, "WiFi recovery: recovery task not running; cannot reinit");
        return;
    }

    s_reinit_in_progress = true;
    xTaskNotifyGive(s_wifi_recovery_task);
}

static void wifi_health_monitor_task(void *arg)
{
    (void)arg;
    const char *HTAG = "wifi_health";

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(s_wifi_health_interval_ms));

        // Only monitor after we have been successfully connected at least once.
        if (!s_initial_connection_done) {
            continue;
        }

        // Skip monitoring while captive portal is active (AP mode)
        if (s_captive_portal_server != NULL) {
            continue;
        }

        // Skip monitoring if we're already performing a recovery reinit
        if (s_reinit_in_progress) {
            continue;
        }

        // Only check when we appear to have an IP
        if (!wifi_sta_has_ip()) {
            continue;
        }

        // DNS-based reachability check (requires internet; chosen by user)
        struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
        };
        struct addrinfo *res = NULL;

        int err = getaddrinfo("google.com", "80", &hints, &res);
        if (err != 0 || res == NULL) {
            ESP_LOGW(HTAG, "Health check failed (getaddrinfo): err=%d res=%p; forcing WiFi reconnect", err, res);
            esp_wifi_remote_disconnect(); // triggers WIFI_EVENT_STA_DISCONNECTED -> reconnect logic
        } else {
            ESP_LOGD(HTAG, "Health check OK");
        }

        if (res) {
            freeaddrinfo(res);
        }
    }
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
        ESP_LOGW(TAG, "WiFi disconnected (initial_connection_done=%d, retry=%d)", 
                 s_initial_connection_done, s_retry_num);

        // Track consecutive errors and attempt a full reinit if we get stuck cycling.
        // Reset occurs on GOT_IP.
        s_consecutive_wifi_errors++;
        if (s_consecutive_wifi_errors >= s_max_consecutive_wifi_errors) {
            ESP_LOGE(TAG, "Too many consecutive WiFi errors (%d) - scheduling full re-init", s_consecutive_wifi_errors);
            s_consecutive_wifi_errors = 0;
            wifi_schedule_full_reinit();
            return;
        }
        
        // Stop MQTT client when WiFi disconnects to prevent futile reconnection attempts
        if (s_initial_connection_done) {
            ESP_LOGI(TAG, "Stopping MQTT client due to WiFi disconnect");
            makapix_mqtt_disconnect();
        }
        
        // For initial connection: use retry limit
        // After initial connection succeeded: always keep trying (persistent reconnection)
        if (!s_initial_connection_done && s_retry_num >= EXAMPLE_ESP_MAXIMUM_RETRY) {
            ESP_LOGI(TAG, "Initial connection failed after %d attempts", EXAMPLE_ESP_MAXIMUM_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else {
            // Always try to reconnect
            s_retry_num++;
            
            // Add delay for persistent reconnection to avoid hammering the AP
            if (s_initial_connection_done && s_retry_num > 5) {
                // After 5 quick retries, slow down to every 5 seconds
                ESP_LOGI(TAG, "WiFi reconnect attempt %d (with backoff)", s_retry_num);
                vTaskDelay(pdMS_TO_TICKS(5000));
            } else {
                ESP_LOGI(TAG, "WiFi reconnect attempt %d/%d", s_retry_num, 
                         s_initial_connection_done ? -1 : EXAMPLE_ESP_MAXIMUM_RETRY);
            }
            
            esp_err_t err = esp_wifi_remote_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_remote_connect failed: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_consecutive_wifi_errors = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Ensure mDNS is enabled/announced on STA after getting an IP.
        // Important: depending on init ordering, mdns_init() might have happened after GOT_IP in the past,
        // which means mDNS would never get enabled on the STA interface and p3a.local would not resolve.
        {
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
            if (sta_netif) {
                esp_err_t merr = mdns_netif_action(sta_netif,
                                                  (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
                if (merr == ESP_OK) {
                    ESP_LOGI(TAG, "mDNS announced on %s", WIFI_STA_NETIF_KEY);
                } else {
                    ESP_LOGW(TAG, "mDNS announce failed on %s: %s", WIFI_STA_NETIF_KEY, esp_err_to_name(merr));
                }
            } else {
                ESP_LOGW(TAG, "STA netif not found for mDNS announce (ifkey=%s)", WIFI_STA_NETIF_KEY);
            }
        }
        
        // Stop captive portal server if running (to avoid port 80 conflict)
        if (s_captive_portal_server != NULL) {
            ESP_LOGI(TAG, "Stopping captive portal server");
            httpd_stop(s_captive_portal_server);
            s_captive_portal_server = NULL;
        }
        
        // Initialize app services only once (first successful start)
        if (!s_services_initialized) {
            ESP_LOGI(TAG, "STA connected, initializing app services");
            
            // Initialize SNTP for time synchronization
            sntp_sync_init();
            
            app_state_init();
            esp_err_t api_err = http_api_start();
            if (api_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start HTTP API: %s", esp_err_to_name(api_err));
                app_state_enter_error();
                // IMPORTANT: do not mark services initialized on failure.
                // We want to retry on next reconnect (or next IP event) because failures can be transient
                // (e.g., port 80 still held by captive portal, low memory, etc.).
            } else {
                // Call callback to register action handlers
                if (s_rest_start_callback) {
                    s_rest_start_callback();
                }
                app_state_enter_ready();
                ESP_LOGI(TAG, "REST API started at http://p3a.local/");
                s_services_initialized = true;
            }
        } else {
            ESP_LOGI(TAG, "WiFi reconnected after disconnect");
        }
        
        // Mark initial connection as done
        s_initial_connection_done = true;
        
        // Connect/reconnect to MQTT if registered
        makapix_connect_if_registered();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        // IP address lost - DHCP renewal likely failed
        // Force WiFi reconnection to obtain a fresh DHCP lease
        ESP_LOGW(TAG, "IP address lost! DHCP renewal likely failed.");
        ESP_LOGI(TAG, "Forcing WiFi reconnection to obtain new IP...");
        
        // Stop MQTT to prevent futile connection attempts
        makapix_mqtt_disconnect();
        
        // Force WiFi disconnect - this will trigger WIFI_EVENT_STA_DISCONNECTED
        // which then triggers reconnection and fresh DHCP
        esp_wifi_remote_disconnect();
    }
}

/* Start mDNS in STA mode so p3a.local works on the LAN */
static esp_err_t start_mdns_sta(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        // mDNS already initialized (e.g., we previously started AP mode captive portal).
        ESP_LOGW(TAG, "mDNS already initialized; reconfiguring for STA");
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

    // Advertise HTTP service. Treat "already exists" as OK.
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    // Some mdns versions return ESP_ERR_INVALID_ARG when the service already exists.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS configured in STA mode: http://p3a.local/");
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
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
        if (sta_netif) {
            esp_err_t herr = esp_netif_set_hostname(sta_netif, "p3a");
            if (herr != ESP_OK) {
                ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s", esp_err_to_name(herr));
            }
        }
    }

    // Start mDNS early (before GOT_IP) so it can reliably hook IP events and respond to queries.
    // If it fails, the UI still works via IP address.
    esp_err_t mdns_err = start_mdns_sta();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS start failed in STA mode (UI still works via IP): %s", esp_err_to_name(mdns_err));
    }

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

    ESP_LOGI(TAG, "DNS server started - responding with 192.168.4.1 to all queries");

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

/* Start mDNS in AP mode so p3a.local works during WiFi setup */
static esp_err_t start_mdns_ap(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mDNS already initialized; reconfiguring for AP");
    }

    err = mdns_hostname_set("p3a");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set("p3a WiFi Setup");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    // Some mdns versions return ESP_ERR_INVALID_ARG when the service already exists.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS started in AP mode: http://p3a.local/");
    return ESP_OK;
}

/* Soft AP Initialization with Wi-Fi 6 */
static void wifi_init_softap(void)
{
    esp_err_t err;
    
    // Check if WiFi is already initialized (from previous STA attempt)
    wifi_mode_t current_mode;
    bool wifi_already_initialized = (esp_wifi_remote_get_mode(&current_mode) == ESP_OK);
    
    if (wifi_already_initialized) {
        // WiFi is already initialized from STA mode - just switch modes
        // This is the standard ESP-IDF approach: stop -> set_mode -> set_config -> start
        ESP_LOGI(TAG, "WiFi already initialized, switching from STA to AP mode");
        
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
        ESP_LOGI(TAG, "Fresh WiFi initialization for AP mode");
        
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

    // Start mDNS so p3a.local works in AP mode
    esp_err_t mdns_err = start_mdns_ap();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS start failed (captive portal still works via IP): %s", esp_err_to_name(mdns_err));
    }
}

esp_err_t app_wifi_init(app_wifi_rest_callback_t rest_callback)
{
    s_rest_start_callback = rest_callback;

    // Initialize Wi-Fi remote module (ESP32-C6 via SDIO)
    wifi_remote_init();

    // Start recovery worker once (used by Recommendation 3)
    if (!s_wifi_recovery_task) {
        xTaskCreate(wifi_recovery_task, "wifi_recovery", 4096, NULL, 6, &s_wifi_recovery_task);
    }

    // Start WiFi health monitor once (Recommendation 2)
    if (!s_wifi_health_task) {
        xTaskCreate(wifi_health_monitor_task, "wifi_health", 4096, NULL, 5, &s_wifi_health_task);
    }

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
    ESP_LOGI(TAG, "Then open http://p3a.local/ or http://192.168.4.1 in your browser");
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

