#include "makapix_mqtt.h"
#include "makapix_certs.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "version.h"
#include "sntp_sync.h"
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

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_connected = true;
        // Subscribe to command topic
        if (strlen(s_command_topic) > 0) {
            int msg_id = esp_mqtt_client_subscribe(client, s_command_topic, 1);
            ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", s_command_topic, msg_id);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT unsubscribed, msg_id=%d", event->msg_id);
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
        if (event->error_handle) {
            ESP_LOGE(TAG, "MQTT error type: %d", event->error_handle->error_type);
            if (event->error_handle->esp_tls_last_esp_err) {
                ESP_LOGE(TAG, "TLS error: 0x%x", event->error_handle->esp_tls_last_esp_err);
            }
            if (event->error_handle->esp_transport_sock_errno) {
                ESP_LOGE(TAG, "Socket error: %d", event->error_handle->esp_transport_sock_errno);
            }
        } else {
            ESP_LOGE(TAG, "MQTT error: unknown");
        }
        break;

    default:
        ESP_LOGD(TAG, "MQTT event: %ld", event_id);
        break;
    }
}

esp_err_t makapix_mqtt_init(const char *player_key, const char *host, uint16_t port)
{
    if (!player_key || !host) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_player_key, player_key, sizeof(s_player_key) - 1);
    s_player_key[sizeof(s_player_key) - 1] = '\0';

    // Build topic strings
    snprintf(s_command_topic, sizeof(s_command_topic), "makapix/player/%s/command", player_key);
    snprintf(s_status_topic, sizeof(s_status_topic), "makapix/player/%s/status", player_key);

    // Build MQTT URI (using static buffer to persist after function returns)
    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtts://%s:%d", host, port);

    // Build client ID (using static buffer to persist after function returns)
    snprintf(s_client_id, sizeof(s_client_id), "p3a-%s", player_key);

    // Build Last Will Testament payload (using static buffer to persist after function returns)
    snprintf(s_lwt_payload, sizeof(s_lwt_payload), "{\"player_key\":\"%s\",\"status\":\"offline\"}", player_key);

    ESP_LOGI(TAG, "Initializing MQTT client: uri=%s, client_id=%s", s_mqtt_uri, s_client_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt_uri,
        .credentials.username = s_player_key,
        .credentials.authentication.password = "",  // Empty password
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

    // Set TLS certificate
    mqtt_cfg.broker.verification.certificate = makapix_get_mqtt_ca_cert();

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    return ESP_OK;
}

esp_err_t makapix_mqtt_connect(void)
{
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "MQTT client started");
    }

    return err;
}

void makapix_mqtt_disconnect(void)
{
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        ESP_LOGI(TAG, "MQTT client stopped");
    }
}

bool makapix_mqtt_is_connected(void)
{
    if (!s_mqtt_client) {
        return false;
    }
    return s_mqtt_connected;
}

esp_err_t makapix_mqtt_publish_status(int32_t current_post_id)
{
    if (!s_mqtt_client || !makapix_mqtt_is_connected()) {
        ESP_LOGW(TAG, "MQTT not connected, cannot publish status");
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

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_status_topic, json_string, 0, 1, 0);
    free(json_string);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published status, msg_id=%d", msg_id);
    return ESP_OK;
}

void makapix_mqtt_set_command_callback(void (*cb)(const char *command_type, cJSON *payload))
{
    s_command_callback = cb;
}

