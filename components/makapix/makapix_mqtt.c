#include "makapix_mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "version.h"
#include "sntp_sync.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "makapix_mqtt";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static char s_player_key[37] = {0};
static char s_command_topic[128] = {0};
static char s_status_topic[128] = {0};
static char s_mqtt_uri[256] = {0};        // Static buffer for MQTT URI
static char s_client_id[64] = {0};        // Static buffer for client ID
static char s_lwt_payload[128] = {0};     // Static buffer for LWT payload
static bool s_mqtt_connected = false;     // Track connection state manually
static void (*s_command_callback)(const char *command_type, cJSON *payload) = NULL;
static void (*s_connection_callback)(bool connected) = NULL;

// Static buffers for certificates - ESP-IDF MQTT client stores pointers, doesn't copy
// These must remain valid for the lifetime of the MQTT client
static char s_ca_cert[4096] = {0};
static char s_client_cert[4096] = {0};
static char s_client_key[4096] = {0};

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "=== MQTT CONNECTED ===");
        ESP_LOGI(TAG, "Session present: %d", event->session_present);
        ESP_LOGI(TAG, "URI: %s", s_mqtt_uri);
        ESP_LOGI(TAG, "Client ID: %s", s_client_id);
        ESP_LOGI(TAG, "Username: %s", s_player_key);
        ESP_LOGI(TAG, "Command topic: %s", s_command_topic);
        ESP_LOGI(TAG, "Status topic: %s", s_status_topic);
        s_mqtt_connected = true;
        // Subscribe to command topic
        if (strlen(s_command_topic) > 0) {
            int msg_id = esp_mqtt_client_subscribe(client, s_command_topic, 1);
            ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", s_command_topic, msg_id);
        }
        // Notify connection callback
        if (s_connection_callback) {
            s_connection_callback(true);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "=== MQTT DISCONNECTED ===");
        ESP_LOGI(TAG, "URI: %s", s_mqtt_uri);
        ESP_LOGI(TAG, "Client ID: %s", s_client_id);
        s_mqtt_connected = false;
        // Notify connection callback
        if (s_connection_callback) {
            s_connection_callback(false);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "=== MQTT SUBSCRIBED ===");
        ESP_LOGI(TAG, "Message ID: %d", event->msg_id);
        if (event->data_len > 0) {
            ESP_LOGI(TAG, "Data: %.*s", event->data_len, event->data);
        }
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "=== MQTT UNSUBSCRIBED ===");
        ESP_LOGI(TAG, "Message ID: %d", event->msg_id);
        if (event->data_len > 0) {
            ESP_LOGI(TAG, "Data: %.*s", event->data_len, event->data);
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT data received, topic=%.*s, data=%.*s", 
                 event->topic_len, event->topic, event->data_len, event->data);
        
        // Check if this is a command message
        size_t expected_len = strlen(s_command_topic);
        if (event->topic_len > 0 && event->topic_len == expected_len && 
            strncmp(event->topic, s_command_topic, event->topic_len) == 0) {
            // Parse JSON command
            char *json_str = malloc(event->data_len + 1);
            if (json_str) {
                memcpy(json_str, event->data, event->data_len);
                json_str[event->data_len] = '\0';

                cJSON *json = cJSON_Parse(json_str);
                if (json) {
                    cJSON *command_type = cJSON_GetObjectItem(json, "command_type");
                    cJSON *payload = cJSON_GetObjectItem(json, "payload");

                    if (command_type && cJSON_IsString(command_type) && s_command_callback) {
                        const char *cmd_type = cJSON_GetStringValue(command_type);
                        ESP_LOGI(TAG, "Received command: %s", cmd_type);
                        
                        // If payload is NULL, create empty object (callback must not delete it)
                        cJSON *payload_to_pass = payload;
                        cJSON *empty_payload = NULL;
                        if (!payload_to_pass) {
                            empty_payload = cJSON_CreateObject();
                            payload_to_pass = empty_payload;
                        }
                        
                        s_command_callback(cmd_type, payload_to_pass);
                        
                        // Clean up empty payload if we created it
                        if (empty_payload) {
                            cJSON_Delete(empty_payload);
                        }
                    } else {
                        ESP_LOGW(TAG, "Invalid command message format");
                    }

                    cJSON_Delete(json);
                } else {
                    ESP_LOGE(TAG, "Failed to parse command JSON");
                }

                free(json_str);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "=== MQTT ERROR ===");
        ESP_LOGE(TAG, "URI: %s", s_mqtt_uri);
        ESP_LOGE(TAG, "Client ID: %s", s_client_id);
        ESP_LOGE(TAG, "Connected state: %s", s_mqtt_connected ? "true" : "false");
        if (event->error_handle) {
            ESP_LOGE(TAG, "Error type: %d", event->error_handle->error_type);
            ESP_LOGE(TAG, "Connect return code: %d", event->error_handle->connect_return_code);
            if (event->error_handle->esp_tls_last_esp_err) {
                ESP_LOGE(TAG, "TLS error: 0x%x (%s)", 
                         event->error_handle->esp_tls_last_esp_err,
                         esp_err_to_name(event->error_handle->esp_tls_last_esp_err));
            }
            if (event->error_handle->esp_transport_sock_errno) {
                ESP_LOGE(TAG, "Socket error: %d", event->error_handle->esp_transport_sock_errno);
            }
            if (event->error_handle->esp_tls_cert_verify_flags) {
                ESP_LOGE(TAG, "TLS cert verify flags: 0x%x", event->error_handle->esp_tls_cert_verify_flags);
            }
        } else {
            ESP_LOGE(TAG, "MQTT error: unknown (no error handle)");
        }
        break;

    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "=== MQTT BEFORE_CONNECT ===");
        ESP_LOGI(TAG, "URI: %s", s_mqtt_uri);
        ESP_LOGI(TAG, "Client ID: %s", s_client_id);
        ESP_LOGI(TAG, "Authentication: mTLS (client certificate)");
        break;

    default:
        ESP_LOGI(TAG, "MQTT event: %ld (0x%lx)", event_id, event_id);
        break;
    }
}

esp_err_t makapix_mqtt_init(const char *player_key, const char *host, uint16_t port,
                            const char *ca_cert, const char *client_cert, const char *client_key)
{
    if (!player_key || !host || !ca_cert || !client_cert || !client_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_player_key, player_key, sizeof(s_player_key) - 1);
    s_player_key[sizeof(s_player_key) - 1] = '\0';

    // Copy certificates to static buffers - ESP-IDF MQTT client stores pointers, doesn't copy
    // These must remain valid for the lifetime of the MQTT client
    strncpy(s_ca_cert, ca_cert, sizeof(s_ca_cert) - 1);
    s_ca_cert[sizeof(s_ca_cert) - 1] = '\0';
    strncpy(s_client_cert, client_cert, sizeof(s_client_cert) - 1);
    s_client_cert[sizeof(s_client_cert) - 1] = '\0';
    strncpy(s_client_key, client_key, sizeof(s_client_key) - 1);
    s_client_key[sizeof(s_client_key) - 1] = '\0';

    // Build topic strings
    snprintf(s_command_topic, sizeof(s_command_topic), "makapix/player/%s/command", player_key);
    snprintf(s_status_topic, sizeof(s_status_topic), "makapix/player/%s/status", player_key);

    // Build MQTT URI (using static buffer to persist after function returns)
    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtts://%s:%d", host, port);

    // Build client ID (using static buffer to persist after function returns)
    snprintf(s_client_id, sizeof(s_client_id), "p3a-%s", player_key);

    // Build Last Will Testament payload (using static buffer to persist after function returns)
    snprintf(s_lwt_payload, sizeof(s_lwt_payload), "{\"player_key\":\"%s\",\"status\":\"offline\"}", player_key);

    ESP_LOGI(TAG, "=== MQTT INIT START ===");
    ESP_LOGI(TAG, "Player key: %s", player_key);
    ESP_LOGI(TAG, "Host: %s", host);
    ESP_LOGI(TAG, "Port: %d", port);
    ESP_LOGI(TAG, "URI: %s", s_mqtt_uri);
    ESP_LOGI(TAG, "Client ID: %s", s_client_id);
    ESP_LOGI(TAG, "Authentication: mTLS (mutual TLS)");
    ESP_LOGI(TAG, "CA cert length: %zu bytes", strlen(s_ca_cert));
    ESP_LOGI(TAG, "Client cert length: %zu bytes", strlen(s_client_cert));
    ESP_LOGI(TAG, "Client key length: %zu bytes", strlen(s_client_key));
    ESP_LOGI(TAG, "Command topic: %s", s_command_topic);
    ESP_LOGI(TAG, "Status topic: %s", s_status_topic);
    ESP_LOGI(TAG, "LWT topic: %s", s_status_topic);
    ESP_LOGI(TAG, "LWT payload: %s", s_lwt_payload);
    ESP_LOGI(TAG, "LWT QoS: 1");
    ESP_LOGI(TAG, "LWT retain: false");
    ESP_LOGI(TAG, "Keepalive: 60 seconds");
    ESP_LOGI(TAG, "Reconnect timeout: 10000 ms");
    ESP_LOGI(TAG, "Network timeout: 10000 ms");
    ESP_LOGI(TAG, "Clean session: true");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt_uri,
        .credentials.client_id = s_client_id,
        .session.last_will.topic = s_status_topic,
        .session.last_will.msg = s_lwt_payload,
        .session.last_will.qos = 1,
        .session.last_will.retain = false,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 10000,
        .session.disable_clean_session = false,
    };

    // Set mTLS certificates (mutual TLS authentication) - use static buffers
    mqtt_cfg.broker.verification.certificate = s_ca_cert;  // CA cert for server verification
    mqtt_cfg.credentials.authentication.certificate = s_client_cert;  // Client cert for authentication
    mqtt_cfg.credentials.authentication.key = s_client_key;  // Client private key
    
    ESP_LOGI(TAG, "mTLS configuration: CA cert, client cert, and client key set (copied to static buffers)");

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client (out of memory)");
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_LOGI(TAG, "MQTT client initialized successfully");
    ESP_LOGI(TAG, "=== MQTT INIT END ===");

    return ESP_OK;
}

static void log_wifi_status(void)
{
    wifi_ap_record_t ap_info;
    esp_err_t wifi_err = esp_wifi_sta_get_ap_info(&ap_info);
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi AP SSID: %s", ap_info.ssid);
        ESP_LOGI(TAG, "WiFi AP RSSI: %d dBm", ap_info.rssi);
        ESP_LOGI(TAG, "WiFi AP Channel: %d", ap_info.primary);
    } else {
        ESP_LOGW(TAG, "WiFi AP info not available: %s (%d)", esp_err_to_name(wifi_err), wifi_err);
    }

    wifi_mode_t wifi_mode;
    wifi_err = esp_wifi_get_mode(&wifi_mode);
    if (wifi_err == ESP_OK) {
        const char *mode_str = "UNKNOWN";
        switch (wifi_mode) {
            case WIFI_MODE_NULL: mode_str = "NULL"; break;
            case WIFI_MODE_STA: mode_str = "STA"; break;
            case WIFI_MODE_AP: mode_str = "AP"; break;
            case WIFI_MODE_APSTA: mode_str = "APSTA"; break;
            case WIFI_MODE_NAN: mode_str = "NAN"; break;
            case WIFI_MODE_MAX: mode_str = "MAX"; break;
            default: mode_str = "UNKNOWN"; break;
        }
        ESP_LOGI(TAG, "WiFi mode: %s (%d)", mode_str, wifi_mode);
    }

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_ip_info_t ip_info;
        esp_err_t ip_err = esp_netif_get_ip_info(sta_netif, &ip_info);
        if (ip_err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi IP: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "WiFi Netmask: " IPSTR, IP2STR(&ip_info.netmask));
            ESP_LOGI(TAG, "WiFi Gateway: " IPSTR, IP2STR(&ip_info.gw));
        } else {
            ESP_LOGW(TAG, "WiFi IP info not available: %s (%d)", esp_err_to_name(ip_err), ip_err);
        }
    } else {
        ESP_LOGW(TAG, "WiFi STA netif not found");
    }
}

esp_err_t makapix_mqtt_connect(void)
{
    ESP_LOGI(TAG, "=== MQTT CONNECT START ===");
    
    // Log WiFi status before attempting MQTT connection
    ESP_LOGI(TAG, "--- WiFi Status ---");
    log_wifi_status();
    ESP_LOGI(TAG, "--- End WiFi Status ---");
    
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "MQTT client not initialized (NULL)");
        ESP_LOGE(TAG, "Current connection state: %s", s_mqtt_connected ? "connected" : "disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "MQTT client handle: %p", (void*)s_mqtt_client);
    ESP_LOGI(TAG, "Current connection state: %s", s_mqtt_connected ? "connected" : "disconnected");
    ESP_LOGI(TAG, "URI: %s", s_mqtt_uri);
    ESP_LOGI(TAG, "Client ID: %s", s_client_id);
    ESP_LOGI(TAG, "Calling esp_mqtt_client_start()...");

    esp_err_t err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        ESP_LOGE(TAG, "Error code: %d (%s)", err, esp_err_to_name(err));
        ESP_LOGE(TAG, "Error description: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "esp_mqtt_client_start() returned ESP_OK");
        ESP_LOGI(TAG, "Connection attempt initiated (connection is asynchronous)");
    }

    ESP_LOGI(TAG, "=== MQTT CONNECT END ===");
    return err;
}

void makapix_mqtt_disconnect(void)
{
    ESP_LOGI(TAG, "=== MQTT DISCONNECT START ===");
    ESP_LOGI(TAG, "Client handle: %p", (void*)s_mqtt_client);
    ESP_LOGI(TAG, "Connection state before disconnect: %s", s_mqtt_connected ? "connected" : "disconnected");
    
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        ESP_LOGI(TAG, "esp_mqtt_client_stop() called");
        ESP_LOGI(TAG, "MQTT client stopped");
    } else {
        ESP_LOGW(TAG, "MQTT client handle is NULL, nothing to disconnect");
    }
    
    ESP_LOGI(TAG, "=== MQTT DISCONNECT END ===");
}

bool makapix_mqtt_is_connected(void)
{
    bool connected = false;
    if (!s_mqtt_client) {
        ESP_LOGD(TAG, "makapix_mqtt_is_connected(): client NULL, returning false");
        return false;
    }
    connected = s_mqtt_connected;
    ESP_LOGD(TAG, "makapix_mqtt_is_connected(): returning %s", connected ? "true" : "false");
    return connected;
}

esp_err_t makapix_mqtt_publish_status(int32_t current_post_id)
{
    ESP_LOGD(TAG, "=== MQTT PUBLISH STATUS START ===");
    ESP_LOGD(TAG, "Client handle: %p", (void*)s_mqtt_client);
    ESP_LOGD(TAG, "Connection state: %s", s_mqtt_connected ? "connected" : "disconnected");
    ESP_LOGD(TAG, "Status topic: %s", s_status_topic);
    ESP_LOGD(TAG, "Current post ID: %ld", current_post_id);
    
    if (!s_mqtt_client || !makapix_mqtt_is_connected()) {
        ESP_LOGW(TAG, "MQTT not connected, cannot publish status");
        ESP_LOGW(TAG, "Client handle: %s", s_mqtt_client ? "valid" : "NULL");
        ESP_LOGW(TAG, "Connection state: %s", s_mqtt_connected ? "connected" : "disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    // Build status JSON
    cJSON *status = cJSON_CreateObject();
    if (!status) {
        ESP_LOGE(TAG, "Failed to create status JSON");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(status, "player_key", s_player_key);
    cJSON_AddStringToObject(status, "status", "online");
    
    if (current_post_id > 0) {
        cJSON_AddNumberToObject(status, "current_post_id", current_post_id);
    } else {
        cJSON_AddNullToObject(status, "current_post_id");
    }

    cJSON_AddStringToObject(status, "firmware_version", FW_VERSION);

    // Get current timestamp using centralized function
    char timestamp[32];
    esp_err_t time_err = sntp_sync_get_iso8601(timestamp, sizeof(timestamp));
    if (time_err == ESP_OK) {
        cJSON_AddStringToObject(status, "timestamp", timestamp);
    } else {
        // Fallback if time not synchronized
        cJSON_AddStringToObject(status, "timestamp", "1970-01-01T00:00:00Z");
    }

    char *json_string = cJSON_PrintUnformatted(status);
    cJSON_Delete(status);

    if (!json_string) {
        ESP_LOGE(TAG, "Failed to serialize status JSON");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "Publishing to topic: %s", s_status_topic);
    ESP_LOGD(TAG, "Payload length: %zu bytes", strlen(json_string));
    ESP_LOGD(TAG, "Payload: %s", json_string);
    
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_status_topic, json_string, 0, 1, 0);
    free(json_string);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status");
        ESP_LOGE(TAG, "esp_mqtt_client_publish() returned: %d", msg_id);
        ESP_LOGE(TAG, "Topic: %s", s_status_topic);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published status successfully, msg_id=%d", msg_id);
    ESP_LOGD(TAG, "=== MQTT PUBLISH STATUS END ===");
    return ESP_OK;
}

void makapix_mqtt_set_command_callback(void (*cb)(const char *command_type, cJSON *payload))
{
    s_command_callback = cb;
}

void makapix_mqtt_set_connection_callback(void (*cb)(bool connected))
{
    s_connection_callback = cb;
}

void makapix_mqtt_log_state(void)
{
    ESP_LOGI(TAG, "=== MQTT STATE REPORT ===");
    ESP_LOGI(TAG, "Client handle: %p", (void*)s_mqtt_client);
    ESP_LOGI(TAG, "Connection state: %s", s_mqtt_connected ? "connected" : "disconnected");
    ESP_LOGI(TAG, "URI: %s", strlen(s_mqtt_uri) > 0 ? s_mqtt_uri : "(not set)");
    ESP_LOGI(TAG, "Client ID: %s", strlen(s_client_id) > 0 ? s_client_id : "(not set)");
    ESP_LOGI(TAG, "Username: %s", strlen(s_player_key) > 0 ? s_player_key : "(not set)");
    ESP_LOGI(TAG, "Command topic: %s", strlen(s_command_topic) > 0 ? s_command_topic : "(not set)");
    ESP_LOGI(TAG, "Status topic: %s", strlen(s_status_topic) > 0 ? s_status_topic : "(not set)");
    ESP_LOGI(TAG, "LWT payload: %s", strlen(s_lwt_payload) > 0 ? s_lwt_payload : "(not set)");
    ESP_LOGI(TAG, "=== END MQTT STATE REPORT ===");
}

