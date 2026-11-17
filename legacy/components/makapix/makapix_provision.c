// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_provision.c
 * @brief Makapix HTTP provisioning: device registration and credential polling
 */

#include "makapix_provision.h"
#include "http_fetch.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "version.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "makapix_provision";

#define MAX_RESPONSE_SIZE 2048

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
    char *response_buf = malloc(MAX_RESPONSE_SIZE);
    if (!response_buf) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        free(json_string);
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    // POST the provision request. Provisioning creates server-side state, so
    // never auto-retry (max_attempts = 1); success is 201 Created, which the
    // accept_2xx flag lets through for the explicit status check below.
    const http_fetch_header_t headers[] = {
        { .name = "Content-Type", .value = "application/json" },
    };
    http_fetch_request_t fr = {
        .url = provision_url,
        .method = HTTP_FETCH_POST,
        .body = json_string,
        .body_len = strlen(json_string),
        .headers = headers,
        .header_count = 1,
        .timeout_ms = 30000,
        .max_attempts = 1,
        .accept_2xx = true,
    };
    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, response_buf, MAX_RESPONSE_SIZE, NULL, &res);

    ESP_LOGI(TAG, "HTTP Status = %d, response_len = %zu", res.http_status, res.bytes);

    if (err != ESP_OK) {
        if (res.buffer_full) {
            ESP_LOGE(TAG, "Provisioning response truncated: response exceeded %d-byte buffer",
                     MAX_RESPONSE_SIZE);
            err = ESP_ERR_INVALID_SIZE;
        } else {
            ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        }
    } else if (res.http_status != 201) {
        ESP_LOGE(TAG, "HTTP request failed with status %d", res.http_status);
        err = ESP_ERR_INVALID_RESPONSE;
    } else {
        ESP_LOGD(TAG, "Response: %s", response_buf);

        // Parse JSON response
        cJSON *response_json = cJSON_Parse(response_buf);
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
    }

    free(response_buf);
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
    #define CREDENTIALS_MAX_RESPONSE_SIZE 32768  // 32KB: 3 JSON-escaped PEMs typically ~6-9KB, generous headroom for larger certs/chains
    char *response_buf = heap_caps_malloc(CREDENTIALS_MAX_RESPONSE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response_buf) {
        ESP_LOGE(TAG, "Failed to allocate response buffer from SPIRAM");
        return ESP_ERR_NO_MEM;
    }

    // One attempt per call: the caller polls every 3 s and owns the retry
    // cadence, and the short timeout keeps cancellation responsive.
    http_fetch_request_t fr = {
        .url = url,
        .timeout_ms = 5000,  // Reduced from 10000 for faster cancellation response
        .max_attempts = 1,
    };
    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, response_buf, CREDENTIALS_MAX_RESPONSE_SIZE, NULL, &res);

    ESP_LOGI(TAG, "Credentials poll HTTP Status = %d, response_len = %zu", res.http_status, res.bytes);

    if (err == ESP_ERR_NOT_FOUND) {
        // Registration not complete yet (404) - this is expected while polling
        ESP_LOGD(TAG, "Credentials not ready yet (404)");
    } else if (err != ESP_OK) {
        if (res.buffer_full) {
            ESP_LOGE(TAG, "Credentials response truncated: response exceeded %d-byte buffer — server response too large",
                     CREDENTIALS_MAX_RESPONSE_SIZE);
            err = ESP_ERR_INVALID_SIZE;
        } else if (res.http_status == 500) {
            // Server error - caller retries on the next poll
            ESP_LOGW(TAG, "Server error (500) - will retry");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            ESP_LOGE(TAG, "HTTP request failed: %s (status %d)",
                     esp_err_to_name(err), res.http_status);
        }
    } else {
        // Credentials are available - parse response
        ESP_LOGD(TAG, "Credentials response received");

        cJSON *response_json = cJSON_Parse(response_buf);
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

            // Optional: HTTPS bearer token, minted by the server only on the
            // very first credentials fetch (null on re-fetches). Needed later
            // for device-initiated certificate renewal; devices that miss it
            // bootstrap via POST /player/{player_key}/token/rotate instead.
            cJSON *api_token = cJSON_GetObjectItem(response_json, "api_token");
            if (api_token && cJSON_IsString(api_token)) {
                const char *token_str = cJSON_GetStringValue(api_token);
                if (strlen(token_str) < sizeof(result->api_token)) {
                    strncpy(result->api_token, token_str, sizeof(result->api_token) - 1);
                    result->api_token[sizeof(result->api_token) - 1] = '\0';
                } else {
                    // Token too large for the buffer: not fatal (renewal can
                    // bootstrap via token/rotate), but say so loudly.
                    ESP_LOGW(TAG, "api_token too large (%zu bytes), discarding", strlen(token_str));
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
    }

    free(response_buf);

    return err;
}

