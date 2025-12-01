#include "makapix_provision.h"
#include "makapix_certs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "version.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "makapix_provision";

#define MAX_RESPONSE_SIZE 2048

// Response buffer for HTTP event handler
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t data_len;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp && resp->buffer && evt->data_len > 0) {
            // Check if we have room
            size_t remaining = resp->buffer_size - resp->data_len - 1; // -1 for null terminator
            size_t copy_len = (evt->data_len < remaining) ? evt->data_len : remaining;
            if (copy_len > 0) {
                memcpy(resp->buffer + resp->data_len, evt->data, copy_len);
                resp->data_len += copy_len;
                resp->buffer[resp->data_len] = '\0';
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t makapix_provision_request(makapix_provision_result_t *result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(makapix_provision_result_t));

    // Build JSON request body
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(json, "device_model", FW_DEVICE_MODEL);
    cJSON_AddStringToObject(json, "firmware_version", FW_VERSION);

    char *json_string = cJSON_PrintUnformatted(json);
    if (!json_string) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    // Build provisioning URL from CONFIG
    char provision_url[256];
    snprintf(provision_url, sizeof(provision_url), "https://%s/api/player/provision", CONFIG_MAKAPIX_CLUB_HOST);
    
    ESP_LOGI(TAG, "Requesting provisioning from %s", provision_url);
    ESP_LOGD(TAG, "Request body: %s", json_string);

    // Allocate response buffer
    http_response_t response = {
        .buffer = malloc(MAX_RESPONSE_SIZE),
        .buffer_size = MAX_RESPONSE_SIZE,
        .data_len = 0
    };
    
    if (!response.buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        free(json_string);
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    response.buffer[0] = '\0';

    // Configure HTTP client
    // Try using ESP-IDF certificate bundle first (for Let's Encrypt and other public CAs)
    // If using self-signed dev CA, set USE_CUSTOM_CA to 1
    #define USE_CUSTOM_CA 0
    
    esp_http_client_config_t config = {
        .url = provision_url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 30000,
    };

#if USE_CUSTOM_CA
    const char *ca_cert = makapix_get_provision_ca_cert();
    ESP_LOGI(TAG, "Using custom CA cert");
    config.cert_pem = ca_cert;
    config.skip_cert_common_name_check = true;
#else
    ESP_LOGI(TAG, "Using ESP-IDF certificate bundle");
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(response.buffer);
        free(json_string);
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    // Set headers
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_string, strlen(json_string));

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "HTTP Status = %d, response_len = %zu", status_code, response.data_len);

        if (status_code == 201) {
            // Parse response body captured by event handler
            if (response.data_len > 0) {
                ESP_LOGD(TAG, "Response: %s", response.buffer);

                // Parse JSON response
                cJSON *response_json = cJSON_Parse(response.buffer);
                if (response_json) {
                    cJSON *player_key = cJSON_GetObjectItem(response_json, "player_key");
                    cJSON *reg_code = cJSON_GetObjectItem(response_json, "registration_code");
                    cJSON *expires = cJSON_GetObjectItem(response_json, "registration_code_expires_at");
                    cJSON *mqtt_obj = cJSON_GetObjectItem(response_json, "mqtt_broker");
                    
                    if (player_key && cJSON_IsString(player_key)) {
                        strncpy(result->player_key, cJSON_GetStringValue(player_key), sizeof(result->player_key) - 1);
                    }
                    if (reg_code && cJSON_IsString(reg_code)) {
                        strncpy(result->registration_code, cJSON_GetStringValue(reg_code), sizeof(result->registration_code) - 1);
                    }
                    if (expires && cJSON_IsString(expires)) {
                        strncpy(result->expires_at, cJSON_GetStringValue(expires), sizeof(result->expires_at) - 1);
                    }
                    if (mqtt_obj && cJSON_IsObject(mqtt_obj)) {
                        cJSON *host = cJSON_GetObjectItem(mqtt_obj, "host");
                        cJSON *port = cJSON_GetObjectItem(mqtt_obj, "port");
                        if (host && cJSON_IsString(host)) {
                            strncpy(result->mqtt_host, cJSON_GetStringValue(host), sizeof(result->mqtt_host) - 1);
                        }
                        if (port && cJSON_IsNumber(port)) {
                            result->mqtt_port = (uint16_t)cJSON_GetNumberValue(port);
                        }
                    }

                    cJSON_Delete(response_json);

                    // Validate required fields
                    if (strlen(result->player_key) > 0 && strlen(result->registration_code) > 0 && 
                        strlen(result->mqtt_host) > 0 && result->mqtt_port > 0) {
                        ESP_LOGI(TAG, "Provisioning successful: player_key=%s, code=%s", 
                                result->player_key, result->registration_code);
                        err = ESP_OK;
                    } else {
                        ESP_LOGE(TAG, "Missing required fields in response");
                        err = ESP_ERR_INVALID_RESPONSE;
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON response");
                    err = ESP_ERR_INVALID_RESPONSE;
                }
            } else {
                ESP_LOGE(TAG, "Empty response body");
                err = ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(response.buffer);
    free(json_string);
    cJSON_Delete(json);

    return err;
}

esp_err_t makapix_poll_credentials(const char *player_key, makapix_credentials_result_t *result)
{
    if (!player_key || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(makapix_credentials_result_t));

    // Build URL: GET /api/player/{player_key}/credentials
    char url[256];
    snprintf(url, sizeof(url), "https://%s/api/player/%s/credentials", CONFIG_MAKAPIX_CLUB_HOST, player_key);

    ESP_LOGI(TAG, "Polling credentials from %s", url);

    // Allocate response buffer (certificates can be large)
    // Use SPIRAM to leave internal RAM for mbedTLS SSL buffers
    #define CREDENTIALS_MAX_RESPONSE_SIZE 16384  // 16KB should be enough for 3 PEM certificates
    http_response_t response = {
        .buffer = heap_caps_malloc(CREDENTIALS_MAX_RESPONSE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .buffer_size = CREDENTIALS_MAX_RESPONSE_SIZE,
        .data_len = 0
    };
    
    if (!response.buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer from SPIRAM");
        return ESP_ERR_NO_MEM;
    }
    response.buffer[0] = '\0';

    // Configure HTTP client
    #define USE_CUSTOM_CA 0
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 5000,  // Reduced from 10000 for faster cancellation response
    };

#if USE_CUSTOM_CA
    const char *ca_cert = makapix_get_provision_ca_cert();
    ESP_LOGI(TAG, "Using custom CA cert");
    config.cert_pem = ca_cert;
    config.skip_cert_common_name_check = true;
#else
    ESP_LOGD(TAG, "Using ESP-IDF certificate bundle");
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(response.buffer);
        return ESP_ERR_NO_MEM;
    }

    // Set method to GET
    esp_http_client_set_method(client, HTTP_METHOD_GET);

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "Credentials poll HTTP Status = %d, response_len = %zu", status_code, response.data_len);

        if (status_code == 200) {
            // Credentials are available - parse response
            if (response.data_len > 0) {
                ESP_LOGD(TAG, "Credentials response received");

                // Parse JSON response
                cJSON *response_json = cJSON_Parse(response.buffer);
                if (response_json) {
                    cJSON *ca_pem = cJSON_GetObjectItem(response_json, "ca_pem");
                    cJSON *cert_pem = cJSON_GetObjectItem(response_json, "cert_pem");
                    cJSON *key_pem = cJSON_GetObjectItem(response_json, "key_pem");
                    cJSON *broker_obj = cJSON_GetObjectItem(response_json, "broker");
                    
                    bool all_certs_present = true;
                    
                    if (ca_pem && cJSON_IsString(ca_pem)) {
                        const char *ca_str = cJSON_GetStringValue(ca_pem);
                        size_t ca_len = strlen(ca_str);
                        if (ca_len < sizeof(result->ca_pem)) {
                            strncpy(result->ca_pem, ca_str, sizeof(result->ca_pem) - 1);
                            result->ca_pem[sizeof(result->ca_pem) - 1] = '\0';
                        } else {
                            ESP_LOGE(TAG, "CA certificate too large");
                            all_certs_present = false;
                        }
                    } else {
                        ESP_LOGE(TAG, "Missing or invalid ca_pem in response");
                        all_certs_present = false;
                    }

                    if (cert_pem && cJSON_IsString(cert_pem)) {
                        const char *cert_str = cJSON_GetStringValue(cert_pem);
                        size_t cert_len = strlen(cert_str);
                        if (cert_len < sizeof(result->cert_pem)) {
                            strncpy(result->cert_pem, cert_str, sizeof(result->cert_pem) - 1);
                            result->cert_pem[sizeof(result->cert_pem) - 1] = '\0';
                        } else {
                            ESP_LOGE(TAG, "Client certificate too large");
                            all_certs_present = false;
                        }
                    } else {
                        ESP_LOGE(TAG, "Missing or invalid cert_pem in response");
                        all_certs_present = false;
                    }

                    if (key_pem && cJSON_IsString(key_pem)) {
                        const char *key_str = cJSON_GetStringValue(key_pem);
                        size_t key_len = strlen(key_str);
                        if (key_len < sizeof(result->key_pem)) {
                            strncpy(result->key_pem, key_str, sizeof(result->key_pem) - 1);
                            result->key_pem[sizeof(result->key_pem) - 1] = '\0';
                        } else {
                            ESP_LOGE(TAG, "Client private key too large");
                            all_certs_present = false;
                        }
                    } else {
                        ESP_LOGE(TAG, "Missing or invalid key_pem in response");
                        all_certs_present = false;
                    }

                    if (broker_obj && cJSON_IsObject(broker_obj)) {
                        cJSON *host = cJSON_GetObjectItem(broker_obj, "host");
                        cJSON *port = cJSON_GetObjectItem(broker_obj, "port");
                        if (host && cJSON_IsString(host)) {
                            strncpy(result->mqtt_host, cJSON_GetStringValue(host), sizeof(result->mqtt_host) - 1);
                        }
                        if (port && cJSON_IsNumber(port)) {
                            result->mqtt_port = (uint16_t)cJSON_GetNumberValue(port);
                        }
                    }

                    cJSON_Delete(response_json);

                    if (all_certs_present) {
                        ESP_LOGI(TAG, "Credentials received successfully: host=%s, port=%d", 
                                result->mqtt_host, result->mqtt_port);
                        err = ESP_OK;
                    } else {
                        ESP_LOGE(TAG, "Missing required certificate fields in response");
                        err = ESP_ERR_INVALID_RESPONSE;
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON response");
                    err = ESP_ERR_INVALID_RESPONSE;
                }
            } else {
                ESP_LOGE(TAG, "Empty response body");
                err = ESP_ERR_INVALID_RESPONSE;
            }
        } else if (status_code == 404) {
            // Registration not complete yet - this is expected while polling
            ESP_LOGD(TAG, "Credentials not ready yet (404)");
            err = ESP_ERR_NOT_FOUND;
        } else if (status_code == 500) {
            // Server error - retry with exponential backoff
            ESP_LOGW(TAG, "Server error (500) - will retry");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(response.buffer);

    return err;
}

