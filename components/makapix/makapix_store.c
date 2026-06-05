// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_store.c
 * @brief NVS persistence for Makapix credentials (player_key, TLS certs, broker config)
 */

#include "makapix_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "makapix_store";
static const char *NVS_NAMESPACE = "makapix";
static const char *KEY_PLAYER_KEY = "player_key";
static const char *KEY_MQTT_HOST = "mqtt_host";
static const char *KEY_MQTT_PORT = "mqtt_port";
static const char *KEY_CA_CERT = "ca_cert";
static const char *KEY_CLIENT_CERT = "client_cert";
static const char *KEY_CLIENT_KEY = "client_key";

esp_err_t makapix_store_init(void)
{
    // NVS is initialized globally in app_main, so we just verify it's available
    return ESP_OK;
}

bool makapix_store_has_player_key(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, KEY_PLAYER_KEY, NULL, &required_size);
    nvs_close(nvs_handle);

    return (err == ESP_OK && required_size > 0);
}

esp_err_t makapix_store_get_player_key(char *out_key, size_t max_len)
{
    if (!out_key || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_str(nvs_handle, KEY_PLAYER_KEY, out_key, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    return err;
}

esp_err_t makapix_store_get_mqtt_host(char *out_host, size_t max_len)
{
    if (!out_host || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_str(nvs_handle, KEY_MQTT_HOST, out_host, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    return err;
}

esp_err_t makapix_store_get_mqtt_port(uint16_t *out_port)
{
    if (!out_port) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t port_size = sizeof(uint16_t);
    err = nvs_get_blob(nvs_handle, KEY_MQTT_PORT, out_port, &port_size);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    return err;
}

esp_err_t makapix_store_save_registration(const char *player_key, const char *host, uint16_t port,
                                          const char *ca_pem, const char *cert_pem, const char *key_pem)
{
    if (!player_key || !host || !ca_pem || !cert_pem || !key_pem) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    size_t ca_len = strlen(ca_pem) + 1;  // Include null terminator
    size_t cert_len = strlen(cert_pem) + 1;
    size_t key_len = strlen(key_pem) + 1;

    // Write the complete registration under one handle with a single commit,
    // overwriting any prior registration in place. This minimizes the window
    // where NVS could hold a partial registration (certs without player_key).
    err = nvs_set_str(nvs_handle, KEY_PLAYER_KEY, player_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save player_key: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, KEY_MQTT_HOST, host);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save mqtt_host: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_blob(nvs_handle, KEY_MQTT_PORT, &port, sizeof(port));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save mqtt_port: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_blob(nvs_handle, KEY_CA_CERT, ca_pem, ca_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save CA cert: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_blob(nvs_handle, KEY_CLIENT_CERT, cert_pem, cert_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save client cert: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_blob(nvs_handle, KEY_CLIENT_KEY, key_pem, key_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save client key: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved Makapix registration: player_key=%s, host=%s, port=%d, certs=%zu/%zu/%zu bytes",
                 player_key, host, port, ca_len, cert_len, key_len);
    }

    nvs_close(nvs_handle);
    return err;
}

bool makapix_store_has_certificates(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required_size = 0;
    bool all_exist = true;

    // Check CA cert
    err = nvs_get_blob(nvs_handle, KEY_CA_CERT, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        all_exist = false;
    }

    // Check client cert
    if (all_exist) {
        required_size = 0;
        err = nvs_get_blob(nvs_handle, KEY_CLIENT_CERT, NULL, &required_size);
        if (err != ESP_OK || required_size == 0) {
            all_exist = false;
        }
    }

    // Check client key
    if (all_exist) {
        required_size = 0;
        err = nvs_get_blob(nvs_handle, KEY_CLIENT_KEY, NULL, &required_size);
        if (err != ESP_OK || required_size == 0) {
            all_exist = false;
        }
    }

    nvs_close(nvs_handle);
    return all_exist;
}

esp_err_t makapix_store_get_ca_cert(char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_blob(nvs_handle, KEY_CA_CERT, buffer, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "CA certificate truncated (certificate larger than %zu bytes)", max_len);
        return ESP_FAIL;
    }

    return err;
}

esp_err_t makapix_store_get_client_cert(char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_blob(nvs_handle, KEY_CLIENT_CERT, buffer, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Client certificate truncated (certificate larger than %zu bytes)", max_len);
        return ESP_FAIL;
    }

    return err;
}

esp_err_t makapix_store_get_client_key(char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_blob(nvs_handle, KEY_CLIENT_KEY, buffer, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Client private key truncated (key larger than %zu bytes)", max_len);
        return ESP_FAIL;
    }

    return err;
}

esp_err_t makapix_store_clear(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    // Erase all keys (ignore errors if keys don't exist)
    nvs_erase_key(nvs_handle, KEY_PLAYER_KEY);
    nvs_erase_key(nvs_handle, KEY_MQTT_HOST);
    nvs_erase_key(nvs_handle, KEY_MQTT_PORT);
    nvs_erase_key(nvs_handle, KEY_CA_CERT);
    nvs_erase_key(nvs_handle, KEY_CLIENT_CERT);
    nvs_erase_key(nvs_handle, KEY_CLIENT_KEY);

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Cleared all Makapix credentials and certificates from NVS");
    }

    nvs_close(nvs_handle);
    return err;
}

