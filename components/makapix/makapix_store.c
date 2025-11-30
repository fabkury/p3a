#include "makapix_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "fs_init.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "makapix_store";
static const char *NVS_NAMESPACE = "makapix";
static const char *KEY_PLAYER_KEY = "player_key";
static const char *KEY_MQTT_HOST = "mqtt_host";
static const char *KEY_MQTT_PORT = "mqtt_port";

// Certificate file paths in SPIFFS (SPIFFS doesn't support directories, so flat paths)
#define CA_CERT_PATH "/spiffs/makapix_ca.pem"
#define CLIENT_CERT_PATH "/spiffs/makapix_cert.pem"
#define CLIENT_KEY_PATH "/spiffs/makapix_key.pem"

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

esp_err_t makapix_store_save_credentials(const char *player_key, const char *host, uint16_t port)
{
    if (!player_key || !host) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    // Save player_key
    err = nvs_set_str(nvs_handle, KEY_PLAYER_KEY, player_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save player_key: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Save MQTT host
    err = nvs_set_str(nvs_handle, KEY_MQTT_HOST, host);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save mqtt_host: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Save MQTT port
    err = nvs_set_blob(nvs_handle, KEY_MQTT_PORT, &port, sizeof(port));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save mqtt_port: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved Makapix credentials: player_key=%s, host=%s, port=%d", player_key, host, port);
    }

    nvs_close(nvs_handle);
    return err;
}

bool makapix_store_has_certificates(void)
{
    if (!fs_is_mounted()) {
        ESP_LOGW(TAG, "SPIFFS not mounted");
        return false;
    }

    FILE *fp;
    bool all_exist = true;

    fp = fopen(CA_CERT_PATH, "r");
    if (!fp) {
        all_exist = false;
    } else {
        fclose(fp);
    }

    fp = fopen(CLIENT_CERT_PATH, "r");
    if (!fp) {
        all_exist = false;
    } else {
        fclose(fp);
    }

    fp = fopen(CLIENT_KEY_PATH, "r");
    if (!fp) {
        all_exist = false;
    } else {
        fclose(fp);
    }

    return all_exist;
}

esp_err_t makapix_store_save_certificates(const char *ca_pem, const char *cert_pem, const char *key_pem)
{
    if (!ca_pem || !cert_pem || !key_pem) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fs_is_mounted()) {
        ESP_LOGE(TAG, "SPIFFS not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    // SPIFFS doesn't support directories, so files are written directly
    // Note: If any write fails, we return error but don't clean up partial writes.
    // This is acceptable because makapix_store_has_certificates() checks for all files,
    // and retries will overwrite the files.

    FILE *fp;
    size_t written;

    // Save CA certificate
    fp = fopen(CA_CERT_PATH, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open CA cert file for writing");
        return ESP_FAIL;
    }
    written = fwrite(ca_pem, 1, strlen(ca_pem), fp);
    fclose(fp);
    if (written != strlen(ca_pem)) {
        ESP_LOGE(TAG, "Failed to write CA cert (wrote %zu of %zu bytes)", written, strlen(ca_pem));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Saved CA certificate to %s", CA_CERT_PATH);

    // Save client certificate
    fp = fopen(CLIENT_CERT_PATH, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open client cert file for writing");
        // Note: CA cert already written, but we'll return error and retry will overwrite
        return ESP_FAIL;
    }
    written = fwrite(cert_pem, 1, strlen(cert_pem), fp);
    fclose(fp);
    if (written != strlen(cert_pem)) {
        ESP_LOGE(TAG, "Failed to write client cert (wrote %zu of %zu bytes)", written, strlen(cert_pem));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Saved client certificate to %s", CLIENT_CERT_PATH);

    // Save client private key
    fp = fopen(CLIENT_KEY_PATH, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open client key file for writing");
        // Note: Previous certs already written, but we'll return error and retry will overwrite
        return ESP_FAIL;
    }
    written = fwrite(key_pem, 1, strlen(key_pem), fp);
    fclose(fp);
    if (written != strlen(key_pem)) {
        ESP_LOGE(TAG, "Failed to write client key (wrote %zu of %zu bytes)", written, strlen(key_pem));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Saved client private key to %s", CLIENT_KEY_PATH);

    ESP_LOGI(TAG, "All certificates saved successfully");
    return ESP_OK;
}

esp_err_t makapix_store_get_ca_cert(char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fs_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(CA_CERT_PATH, "r");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t read_len = fread(buffer, 1, max_len - 1, fp);
    buffer[read_len] = '\0';
    
    // Check if file was fully read (if we read max_len-1, file might be larger)
    int c = fgetc(fp);
    fclose(fp);
    
    if (c != EOF) {
        ESP_LOGW(TAG, "CA certificate file truncated (file larger than %zu bytes)", max_len - 1);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t makapix_store_get_client_cert(char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fs_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(CLIENT_CERT_PATH, "r");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t read_len = fread(buffer, 1, max_len - 1, fp);
    buffer[read_len] = '\0';
    
    // Check if file was fully read (if we read max_len-1, file might be larger)
    int c = fgetc(fp);
    fclose(fp);
    
    if (c != EOF) {
        ESP_LOGW(TAG, "Client certificate file truncated (file larger than %zu bytes)", max_len - 1);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t makapix_store_get_client_key(char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fs_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(CLIENT_KEY_PATH, "r");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t read_len = fread(buffer, 1, max_len - 1, fp);
    buffer[read_len] = '\0';
    
    // Check if file was fully read (if we read max_len-1, file might be larger)
    int c = fgetc(fp);
    fclose(fp);
    
    if (c != EOF) {
        ESP_LOGW(TAG, "Client private key file truncated (file larger than %zu bytes)", max_len - 1);
        return ESP_FAIL;
    }

    return ESP_OK;
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

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Cleared Makapix NVS credentials");
    }

    nvs_close(nvs_handle);

    // Delete certificate files from SPIFFS
    if (fs_is_mounted()) {
        int ret;
        ret = remove(CA_CERT_PATH);
        if (ret == 0) {
            ESP_LOGI(TAG, "Deleted CA certificate file");
        }
        ret = remove(CLIENT_CERT_PATH);
        if (ret == 0) {
            ESP_LOGI(TAG, "Deleted client certificate file");
        }
        ret = remove(CLIENT_KEY_PATH);
        if (ret == 0) {
            ESP_LOGI(TAG, "Deleted client key file");
        }
        ESP_LOGI(TAG, "Cleared all Makapix credentials and certificates");
    }

    return err;
}

