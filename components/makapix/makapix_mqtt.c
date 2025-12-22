#include "makapix_mqtt.h"
#include "mqtt_client.h"
#include "esp_transport.h"
#include "esp_log.h"
#include "cJSON.h"
#include "version.h"
#include "sntp_sync.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "lwip/inet.h"
#include "channel_player.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "makapix_mqtt";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static char s_player_key[37] = {0};
static char s_command_topic[128] = {0};
static char s_status_topic[128] = {0};
static char s_response_topic[128] = {0};
static char s_response_prefix[128] = {0};
static char s_mqtt_uri[256] = {0};        // Static buffer for MQTT URI
static char s_client_id[64] = {0};        // Static buffer for client ID
static char s_lwt_payload[128] = {0};     // Static buffer for LWT payload
static bool s_mqtt_connected = false;     // Track connection state manually
static bool s_response_subscribed = false;  // Track if response topic subscription confirmed
static int s_pending_response_sub_msg_id = -1;  // Message ID of pending response subscription
static void (*s_command_callback)(const char *command_type, cJSON *payload) = NULL;
static void (*s_connection_callback)(bool connected) = NULL;
static void (*s_response_callback)(const char *topic, char *data, int data_len) = NULL;

// Static buffers for certificates - ESP-IDF MQTT client stores pointers, doesn't copy
// These must remain valid for the lifetime of the MQTT client
static char s_ca_cert[4096] = {0};
static char s_client_cert[4096] = {0};
static char s_client_key[4096] = {0};

// Message reassembly for fragmented MQTT messages
// ESP-IDF MQTT client splits large messages across multiple MQTT_EVENT_DATA events
#define MQTT_REASSEMBLY_BUFFER_SIZE (128 * 1024)
static char *s_reassembly_buffer = NULL;
static size_t s_reassembly_len = 0;
static size_t s_reassembly_total_len = 0;
static char s_reassembly_topic[256] = {0};
static bool s_reassembly_in_progress = false;
static bool s_reassembly_drop = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to %s", s_mqtt_uri);
        s_mqtt_connected = true;
        s_response_subscribed = false;
        s_pending_response_sub_msg_id = -1;
        // Subscribe to command and response topics
        if (strlen(s_command_topic) > 0) {
            esp_mqtt_client_subscribe(client, s_command_topic, 1);
        }
        if (strlen(s_response_topic) > 0) {
            s_pending_response_sub_msg_id = esp_mqtt_client_subscribe(client, s_response_topic, 1);
        }
        if (s_connection_callback) {
            s_connection_callback(true);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected");
        s_mqtt_connected = false;
        s_response_subscribed = false;
        s_pending_response_sub_msg_id = -1;
        if (s_connection_callback) {
            s_connection_callback(false);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        if (event->msg_id == s_pending_response_sub_msg_id) {
            s_response_subscribed = true;
            s_pending_response_sub_msg_id = -1;
            ESP_LOGD(TAG, "Response subscription confirmed");
        }
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGD(TAG, "Unsubscribed msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "Published msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        // Handle message fragmentation - ESP-IDF MQTT client splits large messages
        // First fragment has topic_len > 0, subsequent fragments have topic_len == 0
        
        if (event->topic_len > 0) {
            // This is a new message (first fragment or complete message)
            // If we had an incomplete message in progress, discard it
            if (s_reassembly_in_progress) {
                ESP_LOGW(TAG, "Discarding incomplete reassembly buffer (%zu bytes)", s_reassembly_len);
                s_reassembly_in_progress = false;
                s_reassembly_len = 0;
                s_reassembly_total_len = 0;
                s_reassembly_drop = false;
            }
            
            // Store the topic for this message
            size_t topic_copy_len = event->topic_len < sizeof(s_reassembly_topic) - 1 
                                    ? event->topic_len : sizeof(s_reassembly_topic) - 1;
            memcpy(s_reassembly_topic, event->topic, topic_copy_len);
            s_reassembly_topic[topic_copy_len] = '\0';
            
            ESP_LOGD(TAG, "New message, topic: %s, data_len: %d", s_reassembly_topic, event->data_len);
            
            s_reassembly_total_len = (event->total_data_len > 0) ? (size_t)event->total_data_len : 0;

            // Allocate reassembly buffer if needed (prefer PSRAM)
            if (!s_reassembly_buffer) {
                s_reassembly_buffer = (char *)heap_caps_malloc(MQTT_REASSEMBLY_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!s_reassembly_buffer) {
                    s_reassembly_buffer = (char *)malloc(MQTT_REASSEMBLY_BUFFER_SIZE);
                }
                if (!s_reassembly_buffer) {
                    ESP_LOGE(TAG, "Failed to allocate reassembly buffer");
                    break;
                }
            }
            
            s_reassembly_in_progress = true;
            s_reassembly_len = 0;
            s_reassembly_drop = false;

            if (s_reassembly_total_len > 0 && s_reassembly_total_len > (MQTT_REASSEMBLY_BUFFER_SIZE - 1)) {
                ESP_LOGE(TAG, "Inbound MQTT message too large (%zu bytes > %d). Dropping.", s_reassembly_total_len, MQTT_REASSEMBLY_BUFFER_SIZE - 1);
                s_reassembly_drop = true;
            }
        } else if (s_reassembly_in_progress) {
            // This is a continuation fragment (topic_len == 0)
            ESP_LOGD(TAG, "Continuation fragment: %d bytes (buffer has %zu)", 
                     event->data_len, s_reassembly_len);
        } else {
            // Continuation fragment but no reassembly in progress - discard
            ESP_LOGW(TAG, "Received continuation fragment without start - discarding");
            break;
        }

        // Copy fragment into reassembly buffer using offset/total_len metadata.
        if (s_reassembly_in_progress && event->data_len > 0 && event->data && s_reassembly_buffer && !s_reassembly_drop) {
            size_t offset = (event->current_data_offset > 0) ? (size_t)event->current_data_offset : 0;
            if (offset >= (MQTT_REASSEMBLY_BUFFER_SIZE - 1)) {
                ESP_LOGE(TAG, "MQTT fragment offset out of range (%zu). Dropping message.", offset);
                s_reassembly_drop = true;
            } else {
                size_t space_left = (MQTT_REASSEMBLY_BUFFER_SIZE - 1) - offset;
                size_t copy_len = (size_t)event->data_len;
                if (copy_len > space_left) {
                    copy_len = space_left;
                    ESP_LOGW(TAG, "MQTT fragment truncated to %zu bytes (buffer full)", copy_len);
                }
                memcpy(s_reassembly_buffer + offset, event->data, copy_len);
                size_t end = offset + copy_len;
                if (end > s_reassembly_len) {
                    s_reassembly_len = end;
                }
            }
        }

        // Completion: rely on ESP-IDF metadata instead of JSON parsing.
        bool is_complete = false;
        if (s_reassembly_in_progress) {
            if (event->total_data_len > 0) {
                size_t total = (size_t)event->total_data_len;
                size_t offset = (event->current_data_offset > 0) ? (size_t)event->current_data_offset : 0;
                size_t frag = (event->data_len > 0) ? (size_t)event->data_len : 0;
                is_complete = (offset + frag) >= total;
                s_reassembly_total_len = total;
            } else if (event->topic_len > 0) {
                // No fragmentation metadata; treat as complete single-frame payload.
                is_complete = true;
                s_reassembly_total_len = s_reassembly_len;
            }
        }

        if (s_reassembly_in_progress && is_complete) {
            if (s_reassembly_drop) {
                ESP_LOGW(TAG, "Dropped MQTT message on topic %s (too large/invalid fragments)", s_reassembly_topic);
            } else if (!s_reassembly_buffer || s_reassembly_len == 0) {
                ESP_LOGW(TAG, "Complete MQTT message but empty payload on topic %s", s_reassembly_topic);
            } else {
                // Null terminate for any consumers that treat it as C-string.
                if (s_reassembly_len < MQTT_REASSEMBLY_BUFFER_SIZE) {
                    s_reassembly_buffer[s_reassembly_len] = '\0';
                } else {
                    s_reassembly_buffer[MQTT_REASSEMBLY_BUFFER_SIZE - 1] = '\0';
                }

                ESP_LOGD(TAG, "Received: %s (%zu bytes)", s_reassembly_topic, s_reassembly_len);

                // Command topic: parse exactly once here (small payloads), pass payload object to callback.
                if (strcmp(s_reassembly_topic, s_command_topic) == 0) {
                    cJSON *json = cJSON_Parse(s_reassembly_buffer);
                    if (json) {
                        cJSON *command_type = cJSON_GetObjectItem(json, "command_type");
                        cJSON *payload = cJSON_GetObjectItem(json, "payload");

                        if (command_type && cJSON_IsString(command_type) && s_command_callback) {
                            const char *cmd_type = cJSON_GetStringValue(command_type);
                            ESP_LOGD(TAG, "Command: %s", cmd_type);

                            cJSON *payload_to_pass = payload;
                            cJSON *empty_payload = NULL;
                            if (!payload_to_pass) {
                                empty_payload = cJSON_CreateObject();
                                payload_to_pass = empty_payload;
                            }

                            s_command_callback(cmd_type, payload_to_pass);

                            if (empty_payload) {
                                cJSON_Delete(empty_payload);
                            }
                        }
                        cJSON_Delete(json);
                    } else {
                        ESP_LOGW(TAG, "Failed to parse command JSON on topic %s", s_reassembly_topic);
                    }
                }
                // Response topic: do NOT parse here. Hand the owned string to makapix_api (single parse there).
                else if (strncmp(s_reassembly_topic, s_response_prefix, strlen(s_response_prefix)) == 0) {
                    ESP_LOGD(TAG, "Routing response to callback");
                    if (s_response_callback) {
                        // Transfer ownership of a right-sized buffer to the callback.
                        char *owned = (char *)heap_caps_malloc(s_reassembly_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (!owned) {
                            owned = (char *)malloc(s_reassembly_len + 1);
                            if (owned) {
                                ESP_LOGW(TAG, "PSRAM alloc failed; using internal heap for MQTT response payload (%zu bytes)", s_reassembly_len + 1);
                            }
                        }
                        if (!owned) {
                            ESP_LOGE(TAG, "Failed to allocate MQTT response payload (%zu bytes); dropping message", s_reassembly_len + 1);
                        } else {
                            memcpy(owned, s_reassembly_buffer, s_reassembly_len);
                            owned[s_reassembly_len] = '\0';
                            s_response_callback(s_reassembly_topic, owned, (int)s_reassembly_len);
                        }
                    } else {
                        ESP_LOGW(TAG, "Response callback is NULL!");
                    }
                } else {
                    ESP_LOGD(TAG, "Topic does not match command or response prefix");
                }
            }

            // Reset reassembly state for next message
            s_reassembly_in_progress = false;
            s_reassembly_len = 0;
            s_reassembly_total_len = 0;
            s_reassembly_drop = false;
            s_reassembly_topic[0] = '\0';
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
        ESP_LOGD(TAG, "Connecting to %s", s_mqtt_uri);
        break;

    default:
        ESP_LOGD(TAG, "Event: %ld", event_id);
        break;
    }
}

esp_err_t makapix_mqtt_init(const char *player_key, const char *host, uint16_t port,
                            const char *ca_cert, const char *client_cert, const char *client_key)
{
    if (!player_key || !host || !ca_cert || !client_cert || !client_key) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clean up existing client if any
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        s_mqtt_connected = false;
    }

    strncpy(s_player_key, player_key, sizeof(s_player_key) - 1);
    s_player_key[sizeof(s_player_key) - 1] = '\0';

    // Copy certificates to static buffers
    strncpy(s_ca_cert, ca_cert, sizeof(s_ca_cert) - 1);
    s_ca_cert[sizeof(s_ca_cert) - 1] = '\0';
    strncpy(s_client_cert, client_cert, sizeof(s_client_cert) - 1);
    s_client_cert[sizeof(s_client_cert) - 1] = '\0';
    strncpy(s_client_key, client_key, sizeof(s_client_key) - 1);
    s_client_key[sizeof(s_client_key) - 1] = '\0';

    // Build topic strings
    snprintf(s_command_topic, sizeof(s_command_topic), "makapix/player/%s/command", player_key);
    snprintf(s_status_topic, sizeof(s_status_topic), "makapix/player/%s/status", player_key);
    snprintf(s_response_topic, sizeof(s_response_topic), "makapix/player/%s/response/#", player_key);
    snprintf(s_response_prefix, sizeof(s_response_prefix), "makapix/player/%s/response/", player_key);

    // Build MQTT URI and client ID
    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtts://%s:%d", host, port);
    snprintf(s_client_id, sizeof(s_client_id), "p3a-%s", player_key);
    snprintf(s_lwt_payload, sizeof(s_lwt_payload), "{\"player_key\":\"%s\",\"status\":\"offline\"}", player_key);

    ESP_LOGI(TAG, "Initializing MQTT client for %s:%d", host, port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt_uri,
        .credentials.client_id = s_client_id,
        .credentials.username = s_player_key,  // Server requires player_key as username
        .credentials.authentication.password = "",  // Empty password (server uses mTLS + username)
        .session.last_will.topic = s_status_topic,
        .session.last_will.msg = s_lwt_payload,
        .session.last_will.qos = 1,
        .session.last_will.retain = false,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 10000,
        .network.disable_auto_reconnect = true,
        .network.tcp_keep_alive_cfg = {
            .keep_alive_enable = true,
            .keep_alive_idle = 60,      // Start probes after 60s idle
            .keep_alive_interval = 10,  // Probe every 10s
            .keep_alive_count = 5,      // 5 failed probes = connection dead
        },
        .session.disable_clean_session = false,
    };

    // Set mTLS certificates
    mqtt_cfg.broker.verification.certificate = s_ca_cert;
    mqtt_cfg.credentials.authentication.certificate = s_client_cert;
    mqtt_cfg.credentials.authentication.key = s_client_key;

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
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
        ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(err));
    }
    return err;
}

void makapix_mqtt_disconnect(void)
{
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
    }
}

void makapix_mqtt_deinit(void)
{
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        s_mqtt_connected = false;
    }
    
    if (s_reassembly_buffer) {
        free(s_reassembly_buffer);
        s_reassembly_buffer = NULL;
    }
    s_reassembly_len = 0;
    s_reassembly_total_len = 0;
    s_reassembly_in_progress = false;
    s_reassembly_drop = false;
    s_reassembly_topic[0] = '\0';
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

bool makapix_mqtt_is_ready(void)
{
    // Ready means connected AND response topic subscription confirmed
    if (!s_mqtt_client) {
        return false;
    }
    bool ready = s_mqtt_connected && s_response_subscribed;
    ESP_LOGD(TAG, "makapix_mqtt_is_ready(): connected=%d, subscribed=%d, ready=%d", 
             s_mqtt_connected, s_response_subscribed, ready);
    return ready;
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
    cJSON_AddBoolToObject(status, "live_mode", channel_player_is_live_mode_active());
    
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

void makapix_mqtt_set_response_callback(void (*cb)(const char *topic, char *data, int data_len))
{
    s_response_callback = cb;
}

esp_err_t makapix_mqtt_publish_raw(const char *topic, const char *payload, int qos)
{
    if (!s_mqtt_client || !topic || !payload) {
        ESP_LOGE(TAG, "publish_raw: invalid args (client=%p, topic=%p, payload=%p)", 
                 (void*)s_mqtt_client, (void*)topic, (void*)payload);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "publish_raw: MQTT not connected, cannot publish to %s", topic);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGD(TAG, "Publishing to %s (qos=%d, len=%zu)", topic, qos, strlen(payload));
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, qos, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish_raw: esp_mqtt_client_publish returned %d", msg_id);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "publish_raw: msg_id=%d", msg_id);
    return ESP_OK;
}

esp_err_t makapix_mqtt_subscribe(const char *topic, int qos)
{
    if (!s_mqtt_client || !topic) {
        return ESP_ERR_INVALID_ARG;
    }
    int msg_id = esp_mqtt_client_subscribe(s_mqtt_client, topic, qos);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

void makapix_mqtt_log_state(void)
{
    ESP_LOGI(TAG, "State: %s, URI: %s", 
             s_mqtt_connected ? "connected" : "disconnected",
             strlen(s_mqtt_uri) > 0 ? s_mqtt_uri : "(not set)");
}

