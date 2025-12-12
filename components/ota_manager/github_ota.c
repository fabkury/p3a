/**
 * @file github_ota.c
 * @brief GitHub Releases API client implementation
 */

#include "github_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "github_ota";

// GitHub API URL template - fetch multiple releases to find prereleases
#define GITHUB_API_URL "https://api.github.com/repos/" CONFIG_OTA_GITHUB_REPO "/releases?per_page=3"
#define GITHUB_USER_AGENT "p3a-ota/1.0"

// Maximum response size for API calls (JSON can be large with release notes and assets)
#define MAX_API_RESPONSE_SIZE (128 * 1024)
// Maximum response size for SHA256 file (64 hex chars + some padding)
#define MAX_SHA256_RESPONSE_SIZE 256

/**
 * @brief HTTP event handler for buffering response
 */
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t received;
} http_response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_buffer_t *resp = (http_response_buffer_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (resp && resp->buffer && evt->data_len > 0) {
                size_t remaining = resp->buffer_size - resp->received - 1;
                size_t to_copy = (evt->data_len < remaining) ? evt->data_len : remaining;
                if (to_copy > 0) {
                    memcpy(resp->buffer + resp->received, evt->data, to_copy);
                    resp->received += to_copy;
                    resp->buffer[resp->received] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

uint32_t github_ota_parse_version(const char *version_str)
{
    if (!version_str || strlen(version_str) == 0) {
        return 0;
    }
    
    const char *p = version_str;
    
    // Skip leading 'v' or 'V' if present
    if (*p == 'v' || *p == 'V') {
        p++;
    }
    
    // Parse major.minor.patch
    unsigned int major = 0, minor = 0, patch = 0;
    int parsed = sscanf(p, "%u.%u.%u", &major, &minor, &patch);
    
    if (parsed < 2) {
        ESP_LOGW(TAG, "Failed to parse version: %s", version_str);
        return 0;
    }
    
    // Validate ranges (0-255 each)
    if (major > 255 || minor > 255 || patch > 255) {
        ESP_LOGW(TAG, "Version component out of range: %s", version_str);
        return 0;
    }
    
    return (major << 16) | (minor << 8) | patch;
}

int github_ota_compare_versions(const char *v1, const char *v2)
{
    uint32_t ver1 = github_ota_parse_version(v1);
    uint32_t ver2 = github_ota_parse_version(v2);
    
    if (ver1 == 0 || ver2 == 0) {
        return 0;  // Parse error, treat as equal
    }
    
    if (ver1 > ver2) return 1;
    if (ver1 < ver2) return -1;
    return 0;
}

esp_err_t github_ota_hex_to_bin(const char *hex, uint8_t *bin)
{
    if (!hex || !bin || strlen(hex) != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    
    for (int i = 0; i < 32; i++) {
        char byte_str[3] = {hex[i*2], hex[i*2+1], '\0'};
        char *endptr;
        unsigned long val = strtoul(byte_str, &endptr, 16);
        if (*endptr != '\0') {
            ESP_LOGE(TAG, "Invalid hex character at position %d", i*2);
            return ESP_ERR_INVALID_ARG;
        }
        bin[i] = (uint8_t)val;
    }
    
    return ESP_OK;
}

/**
 * @brief Helper to extract release info from a cJSON release object
 */
static esp_err_t parse_release_object(cJSON *release, github_release_info_t *info)
{
    // Extract tag_name (version)
    cJSON *tag_name = cJSON_GetObjectItem(release, "tag_name");
    if (!tag_name || !cJSON_IsString(tag_name)) {
        return ESP_ERR_NOT_FOUND;
    }
    
    const char *tag = cJSON_GetStringValue(tag_name);
    strncpy(info->tag_name, tag, sizeof(info->tag_name) - 1);
    
    // Strip 'v' prefix for version
    if (tag[0] == 'v' || tag[0] == 'V') {
        strncpy(info->version, tag + 1, sizeof(info->version) - 1);
    } else {
        strncpy(info->version, tag, sizeof(info->version) - 1);
    }
    
    // Check if prerelease
    cJSON *prerelease = cJSON_GetObjectItem(release, "prerelease");
    info->is_prerelease = prerelease && cJSON_IsTrue(prerelease);
    
    // Extract release notes (body)
    cJSON *body = cJSON_GetObjectItem(release, "body");
    if (body && cJSON_IsString(body)) {
        const char *notes = cJSON_GetStringValue(body);
        if (notes) {
            strncpy(info->release_notes, notes, sizeof(info->release_notes) - 1);
        }
    }
    
    // Find firmware assets
    cJSON *assets = cJSON_GetObjectItem(release, "assets");
    if (!assets || !cJSON_IsArray(assets)) {
        ESP_LOGW(TAG, "No assets in release %s", info->tag_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    bool found_bin = false;
    bool found_sha256 = false;
    
    cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        cJSON *download_url = cJSON_GetObjectItem(asset, "browser_download_url");
        cJSON *size = cJSON_GetObjectItem(asset, "size");
        
        if (!name || !cJSON_IsString(name) || !download_url || !cJSON_IsString(download_url)) {
            continue;
        }
        
        const char *asset_name = cJSON_GetStringValue(name);
        const char *asset_url = cJSON_GetStringValue(download_url);
        
        // Check for firmware binary
        if (strcmp(asset_name, CONFIG_OTA_FIRMWARE_ASSET_NAME) == 0) {
            strncpy(info->download_url, asset_url, sizeof(info->download_url) - 1);
            if (size && cJSON_IsNumber(size)) {
                info->firmware_size = (uint32_t)cJSON_GetNumberValue(size);
            }
            found_bin = true;
        }
        
        // Check for SHA256 checksum file
        char sha256_name[64];
        snprintf(sha256_name, sizeof(sha256_name), "%s.sha256", CONFIG_OTA_FIRMWARE_ASSET_NAME);
        if (strcmp(asset_name, sha256_name) == 0) {
            strncpy(info->sha256_url, asset_url, sizeof(info->sha256_url) - 1);
            found_sha256 = true;
        }
    }
    
    if (!found_bin) {
        ESP_LOGW(TAG, "Firmware asset '%s' not found in release %s", 
                 CONFIG_OTA_FIRMWARE_ASSET_NAME, info->tag_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    if (!found_sha256) {
        ESP_LOGW(TAG, "SHA256 checksum file not found in release %s", info->tag_name);
    }
    
    return ESP_OK;
}

esp_err_t github_ota_get_latest_release(github_release_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(info, 0, sizeof(github_release_info_t));
    
    // Allocate response buffer (prefer PSRAM to avoid starving TLS/internal heap)
    char *response_buffer = (char *)heap_caps_malloc(MAX_API_RESPONSE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response_buffer) {
        ESP_LOGI(TAG, "Allocated GitHub API response buffer in PSRAM (%d bytes)", MAX_API_RESPONSE_SIZE);
    } else {
        response_buffer = (char *)malloc(MAX_API_RESPONSE_SIZE);
        if (response_buffer) {
            ESP_LOGW(TAG, "PSRAM alloc failed; using internal heap for GitHub API response buffer (%d bytes)", MAX_API_RESPONSE_SIZE);
        }
    }
    if (!response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return ESP_ERR_NO_MEM;
    }
    
    http_response_buffer_t resp = {
        .buffer = response_buffer,
        .buffer_size = MAX_API_RESPONSE_SIZE,
        .received = 0
    };
    
    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .buffer_size = CONFIG_OTA_HTTP_BUFFER_SIZE,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(response_buffer);
        return ESP_ERR_NO_MEM;
    }
    
    // Set required headers
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(client, "User-Agent", GITHUB_USER_AGENT);
    
    ESP_LOGI(TAG, "Fetching releases from GitHub: %s", GITHUB_API_URL);
    
    // Yield to allow WiFi/SDIO driver to settle before starting transfer
    vTaskDelay(pdMS_TO_TICKS(100));
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(response_buffer);
        return ESP_ERR_HTTP_CONNECT;
    }
    
    if (status_code == 404) {
        ESP_LOGW(TAG, "No releases found (404)");
        free(response_buffer);
        return ESP_ERR_NOT_FOUND;
    }
    
    if (status_code == 403) {
        ESP_LOGW(TAG, "Rate limited or forbidden (403)");
        free(response_buffer);
        return ESP_ERR_HTTP_CONNECT;
    }
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
        free(response_buffer);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }
    
    ESP_LOGI(TAG, "Received %zu bytes from GitHub API (buffer max: %d)", resp.received, MAX_API_RESPONSE_SIZE);
    
    // Check if response was truncated
    if (resp.received >= MAX_API_RESPONSE_SIZE - 1) {
        ESP_LOGW(TAG, "Response may have been truncated!");
    }
    
    // Parse JSON response - expect an array of releases
    cJSON *releases_array = cJSON_Parse(response_buffer);
    
    if (!releases_array) {
        const char *error_ptr = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "Failed to parse JSON response. Error near: %.50s", error_ptr ? error_ptr : "(null)");
        ESP_LOGE(TAG, "First 200 chars: %.200s", response_buffer);
        free(response_buffer);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    free(response_buffer);
    
    if (!cJSON_IsArray(releases_array)) {
        ESP_LOGE(TAG, "Expected JSON array of releases");
        cJSON_Delete(releases_array);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    int array_size = cJSON_GetArraySize(releases_array);
    if (array_size == 0) {
        ESP_LOGW(TAG, "No releases in repository");
        cJSON_Delete(releases_array);
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Found %d releases, searching for appropriate version...", array_size);
    
    // Determine what type of release we're looking for
#if CONFIG_OTA_DEV_MODE
    bool want_prerelease = true;
    ESP_LOGI(TAG, "DEV MODE: Looking for pre-release first, then regular release");
#else
    bool want_prerelease = false;
    ESP_LOGI(TAG, "PRODUCTION MODE: Looking for regular release only");
#endif
    
    cJSON *selected_release = NULL;
    cJSON *fallback_release = NULL;  // In dev mode, fall back to regular release if no prerelease
    
    // Iterate through releases to find the appropriate one
    cJSON *release;
    cJSON_ArrayForEach(release, releases_array) {
        cJSON *prerelease_flag = cJSON_GetObjectItem(release, "prerelease");
        cJSON *draft_flag = cJSON_GetObjectItem(release, "draft");
        
        // Skip drafts
        if (draft_flag && cJSON_IsTrue(draft_flag)) {
            continue;
        }
        
        bool is_prerelease = prerelease_flag && cJSON_IsTrue(prerelease_flag);
        
        if (want_prerelease) {
            // Dev mode: prefer prerelease, but remember first regular as fallback
            if (is_prerelease && !selected_release) {
                selected_release = release;
                ESP_LOGI(TAG, "Found pre-release candidate");
            } else if (!is_prerelease && !fallback_release) {
                fallback_release = release;
            }
        } else {
            // Production mode: only consider regular releases
            if (!is_prerelease) {
                selected_release = release;
                break;  // First non-prerelease is the latest stable
            }
        }
    }
    
    // In dev mode, use fallback if no prerelease found
    if (want_prerelease && !selected_release && fallback_release) {
        ESP_LOGI(TAG, "No pre-release found, using latest regular release");
        selected_release = fallback_release;
    }
    
    if (!selected_release) {
        ESP_LOGW(TAG, "No suitable release found");
        cJSON_Delete(releases_array);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Parse the selected release
    err = parse_release_object(selected_release, info);
    cJSON_Delete(releases_array);
    
    if (err != ESP_OK) {
        return err;
    }
    
    ESP_LOGI(TAG, "Selected release: %s (prerelease=%d, size=%u)", 
             info->version, info->is_prerelease, info->firmware_size);
    
    return ESP_OK;
}

esp_err_t github_ota_download_sha256(const char *sha256_url, char *sha256_hex, size_t hex_buf_size)
{
    if (!sha256_url || !sha256_hex || hex_buf_size < 65) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(sha256_hex, 0, hex_buf_size);
    
    // Allocate buffer for SHA256 response (prefer PSRAM)
    char *response_buffer = (char *)heap_caps_malloc(MAX_SHA256_RESPONSE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response_buffer) {
        response_buffer = (char *)malloc(MAX_SHA256_RESPONSE_SIZE);
        if (response_buffer) {
            ESP_LOGW(TAG, "PSRAM alloc failed; using internal heap for SHA256 response buffer (%d bytes)", MAX_SHA256_RESPONSE_SIZE);
        }
    }
    if (!response_buffer) {
        return ESP_ERR_NO_MEM;
    }
    
    http_response_buffer_t resp = {
        .buffer = response_buffer,
        .buffer_size = MAX_SHA256_RESPONSE_SIZE,
        .received = 0
    };
    
    // GitHub raw URLs redirect to CDN, so we must follow redirects
    // The buffer_size must be large enough to hold redirect response headers
    esp_http_client_config_t config = {
        .url = sha256_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .max_redirection_count = 5,  // Follow up to 5 redirects (GitHub -> CDN)
        .buffer_size = 4096,         // Large buffer for redirect header handling
        .buffer_size_tx = 1024,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(response_buffer);
        return ESP_ERR_NO_MEM;
    }
    
    esp_http_client_set_header(client, "User-Agent", GITHUB_USER_AGENT);
    
    ESP_LOGI(TAG, "Downloading SHA256 checksum from: %s", sha256_url);
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "Failed to download SHA256: err=%s, status=%d", 
                 esp_err_to_name(err), status_code);
        free(response_buffer);
        return ESP_FAIL;
    }
    
    // Parse SHA256 - expect 64 hex characters
    // Skip any whitespace and extract hex string
    char *p = response_buffer;
    while (*p && isspace((unsigned char)*p)) p++;
    
    size_t hex_len = 0;
    while (p[hex_len] && isxdigit((unsigned char)p[hex_len]) && hex_len < 64) {
        sha256_hex[hex_len] = tolower((unsigned char)p[hex_len]);
        hex_len++;
    }
    sha256_hex[hex_len] = '\0';
    
    free(response_buffer);
    
    if (hex_len != 64) {
        ESP_LOGE(TAG, "Invalid SHA256 format (got %zu hex chars, expected 64)", hex_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    ESP_LOGI(TAG, "SHA256: %.16s...", sha256_hex);
    return ESP_OK;
}

