// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file github_ota.c
 * @brief GitHub Releases API client implementation
 */

#include "github_ota.h"
#include "http_fetch.h"
#include "esp_http_client.h"   // ESP_ERR_HTTP_* error codes
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

// GitHub REST API requires an Accept header (and a User-Agent, set per request)
static const http_fetch_header_t GH_API_HEADERS[] = {
    { .name = "Accept", .value = "application/vnd.github+json" },
};

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
    
    // ota_manager retries this call itself, so a single attempt here keeps
    // the overall attempt count unchanged.
    http_fetch_request_t fr = {
        .url = GITHUB_API_URL,
        .timeout_ms = 30000,
        .rx_buffer_size = CONFIG_OTA_HTTP_BUFFER_SIZE,
        .user_agent = GITHUB_USER_AGENT,
        .headers = GH_API_HEADERS,
        .header_count = 1,
        .max_attempts = 1,
    };

    ESP_LOGI(TAG, "Fetching releases from GitHub: %s", GITHUB_API_URL);

    // Yield to allow WiFi/SDIO driver to settle before starting transfer
    vTaskDelay(pdMS_TO_TICKS(100));

    size_t received = 0;
    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, response_buffer, MAX_API_RESPONSE_SIZE,
                                         &received, &res);

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No releases found (404)");
        free(response_buffer);
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        ESP_LOGW(TAG, "Rate limited or forbidden (403)");
        free(response_buffer);
        return ESP_ERR_HTTP_CONNECT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s (status %d)",
                 esp_err_to_name(err), res.http_status);
        free(response_buffer);
        return ESP_ERR_HTTP_CONNECT;
    }

    ESP_LOGI(TAG, "Received %zu bytes from GitHub API (buffer max: %d)", received, MAX_API_RESPONSE_SIZE);

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
    
    // GitHub asset URLs redirect to the CDN; follow them manually with a
    // larger tx buffer (the signed CDN URLs are long).
    http_fetch_request_t fr = {
        .url = sha256_url,
        .timeout_ms = 30000,
        .tx_buffer_size = 1024,
        .user_agent = GITHUB_USER_AGENT,
        .redirect_mode = HTTP_FETCH_REDIRECT_MANUAL,
        .max_redirects = 5,  // GitHub -> CDN
    };

    ESP_LOGI(TAG, "Downloading SHA256 checksum from: %s", sha256_url);

    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, response_buffer, MAX_SHA256_RESPONSE_SIZE,
                                         NULL, &res);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to download SHA256: err=%s, status=%d",
                 esp_err_to_name(err), res.http_status);
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

uint16_t github_ota_parse_webui_version(const char *version_str)
{
    if (!version_str || strlen(version_str) == 0) {
        return 0;
    }

    unsigned int major = 0, minor = 0;
    int parsed = sscanf(version_str, "%u.%u", &major, &minor);

    if (parsed < 1) {
        ESP_LOGW(TAG, "Failed to parse web UI version: %s", version_str);
        return 0;
    }

    // Validate ranges (0-255 each)
    if (major > 255 || minor > 255) {
        ESP_LOGW(TAG, "Web UI version component out of range: %s", version_str);
        return 0;
    }

    return (major << 8) | minor;
}

int github_ota_compare_webui_versions(const char *v1, const char *v2)
{
    uint16_t ver1 = github_ota_parse_webui_version(v1);
    uint16_t ver2 = github_ota_parse_webui_version(v2);

    if (ver1 == 0 || ver2 == 0) {
        return 0;  // Parse error, treat as equal
    }

    if (ver1 > ver2) return 1;
    if (ver1 < ver2) return -1;
    return 0;
}

/**
 * @brief Helper to find and parse manifest.json from release assets
 */
static esp_err_t parse_manifest_from_release(cJSON *release, github_release_manifest_t *manifest)
{
    // Extract basic release info
    cJSON *tag_name = cJSON_GetObjectItem(release, "tag_name");
    if (tag_name && cJSON_IsString(tag_name)) {
        strncpy(manifest->tag_name, cJSON_GetStringValue(tag_name), sizeof(manifest->tag_name) - 1);
    }

    cJSON *prerelease = cJSON_GetObjectItem(release, "prerelease");
    manifest->is_prerelease = prerelease && cJSON_IsTrue(prerelease);

    cJSON *body = cJSON_GetObjectItem(release, "body");
    if (body && cJSON_IsString(body)) {
        const char *notes = cJSON_GetStringValue(body);
        if (notes) {
            strncpy(manifest->release_notes, notes, sizeof(manifest->release_notes) - 1);
        }
    }

    // Find manifest.json and asset download URLs
    cJSON *assets = cJSON_GetObjectItem(release, "assets");
    if (!assets || !cJSON_IsArray(assets)) {
        ESP_LOGW(TAG, "No assets in release");
        return ESP_ERR_NOT_FOUND;
    }

    char manifest_url[256] = {0};
    char firmware_url[256] = {0};
    char webui_url[256] = {0};

    cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        cJSON *download_url = cJSON_GetObjectItem(asset, "browser_download_url");

        if (!name || !cJSON_IsString(name) || !download_url || !cJSON_IsString(download_url)) {
            continue;
        }

        const char *asset_name = cJSON_GetStringValue(name);
        const char *asset_url = cJSON_GetStringValue(download_url);

        if (strcmp(asset_name, "manifest.json") == 0) {
            strncpy(manifest_url, asset_url, sizeof(manifest_url) - 1);
        } else if (strcmp(asset_name, CONFIG_OTA_FIRMWARE_ASSET_NAME) == 0) {
            strncpy(firmware_url, asset_url, sizeof(firmware_url) - 1);
        } else if (strcmp(asset_name, "storage.bin") == 0) {
            strncpy(webui_url, asset_url, sizeof(webui_url) - 1);
        }
    }

    // Copy download URLs to manifest struct
    snprintf(manifest->firmware.download_url, sizeof(manifest->firmware.download_url), "%s", firmware_url);
    snprintf(manifest->webui.download_url, sizeof(manifest->webui.download_url), "%s", webui_url);

    // If no manifest.json found, return error
    if (strlen(manifest_url) == 0) {
        ESP_LOGW(TAG, "manifest.json not found in release assets");
        return ESP_ERR_NOT_FOUND;
    }

    // Download and parse manifest.json
    char *manifest_buffer = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!manifest_buffer) {
        manifest_buffer = (char *)malloc(4096);
    }
    if (!manifest_buffer) {
        return ESP_ERR_NO_MEM;
    }

    http_fetch_request_t fr = {
        .url = manifest_url,
        .timeout_ms = 30000,
        .rx_buffer_size = 8192,
        .tx_buffer_size = 1024,
        .user_agent = GITHUB_USER_AGENT,
        .redirect_mode = HTTP_FETCH_REDIRECT_MANUAL,
        .max_redirects = 5,  // GitHub -> CDN
    };

    ESP_LOGI(TAG, "Downloading manifest.json from: %s", manifest_url);

    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, manifest_buffer, 4096, NULL, &res);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to download manifest.json: err=%s, status=%d",
                 esp_err_to_name(err), res.http_status);
        free(manifest_buffer);
        return ESP_FAIL;
    }

    // Parse manifest.json
    cJSON *manifest_json = cJSON_Parse(manifest_buffer);
    free(manifest_buffer);

    if (!manifest_json) {
        ESP_LOGE(TAG, "Failed to parse manifest.json");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Extract firmware info
    cJSON *firmware = cJSON_GetObjectItem(manifest_json, "firmware");
    if (firmware && cJSON_IsObject(firmware)) {
        cJSON *ver = cJSON_GetObjectItem(firmware, "version");
        cJSON *file = cJSON_GetObjectItem(firmware, "file");
        cJSON *sha = cJSON_GetObjectItem(firmware, "sha256");

        if (ver && cJSON_IsString(ver)) {
            strncpy(manifest->firmware.version, cJSON_GetStringValue(ver),
                    sizeof(manifest->firmware.version) - 1);
        }
        if (file && cJSON_IsString(file)) {
            strncpy(manifest->firmware.file, cJSON_GetStringValue(file),
                    sizeof(manifest->firmware.file) - 1);
        }
        if (sha && cJSON_IsString(sha)) {
            strncpy(manifest->firmware.sha256, cJSON_GetStringValue(sha),
                    sizeof(manifest->firmware.sha256) - 1);
        }
    }

    // Extract webui info
    cJSON *webui = cJSON_GetObjectItem(manifest_json, "webui");
    if (webui && cJSON_IsObject(webui)) {
        cJSON *ver = cJSON_GetObjectItem(webui, "version");
        cJSON *file = cJSON_GetObjectItem(webui, "file");
        cJSON *sha = cJSON_GetObjectItem(webui, "sha256");

        if (ver && cJSON_IsString(ver)) {
            strncpy(manifest->webui.version, cJSON_GetStringValue(ver),
                    sizeof(manifest->webui.version) - 1);
        }
        if (file && cJSON_IsString(file)) {
            strncpy(manifest->webui.file, cJSON_GetStringValue(file),
                    sizeof(manifest->webui.file) - 1);
        }
        if (sha && cJSON_IsString(sha)) {
            strncpy(manifest->webui.sha256, cJSON_GetStringValue(sha),
                    sizeof(manifest->webui.sha256) - 1);
        }
    }

    cJSON_Delete(manifest_json);

    ESP_LOGI(TAG, "Manifest parsed: firmware=%s, webui=%s",
             manifest->firmware.version, manifest->webui.version);

    return ESP_OK;
}

esp_err_t github_ota_get_release_manifest(github_release_manifest_t *manifest)
{
    if (!manifest) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(manifest, 0, sizeof(github_release_manifest_t));

    // Allocate response buffer
    char *response_buffer = (char *)heap_caps_malloc(MAX_API_RESPONSE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response_buffer) {
        response_buffer = (char *)malloc(MAX_API_RESPONSE_SIZE);
    }
    if (!response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return ESP_ERR_NO_MEM;
    }

    http_fetch_request_t fr = {
        .url = GITHUB_API_URL,
        .timeout_ms = 30000,
        .rx_buffer_size = CONFIG_OTA_HTTP_BUFFER_SIZE,
        .user_agent = GITHUB_USER_AGENT,
        .headers = GH_API_HEADERS,
        .header_count = 1,
    };

    ESP_LOGI(TAG, "Fetching releases from GitHub for manifest...");

    vTaskDelay(pdMS_TO_TICKS(100));

    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, response_buffer, MAX_API_RESPONSE_SIZE,
                                         NULL, &res);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: err=%s, status=%d",
                 esp_err_to_name(err), res.http_status);
        free(response_buffer);
        return ESP_ERR_HTTP_CONNECT;
    }

    // Parse JSON response
    cJSON *releases_array = cJSON_Parse(response_buffer);
    free(response_buffer);

    if (!releases_array || !cJSON_IsArray(releases_array)) {
        ESP_LOGE(TAG, "Failed to parse releases JSON");
        if (releases_array) cJSON_Delete(releases_array);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int array_size = cJSON_GetArraySize(releases_array);
    if (array_size == 0) {
        ESP_LOGW(TAG, "No releases found");
        cJSON_Delete(releases_array);
        return ESP_ERR_NOT_FOUND;
    }

    // Determine what type of release we're looking for
#if CONFIG_OTA_DEV_MODE
    bool want_prerelease = true;
#else
    bool want_prerelease = false;
#endif

    cJSON *selected_release = NULL;
    cJSON *fallback_release = NULL;

    cJSON *release;
    cJSON_ArrayForEach(release, releases_array) {
        cJSON *prerelease_flag = cJSON_GetObjectItem(release, "prerelease");
        cJSON *draft_flag = cJSON_GetObjectItem(release, "draft");

        if (draft_flag && cJSON_IsTrue(draft_flag)) {
            continue;
        }

        bool is_prerelease = prerelease_flag && cJSON_IsTrue(prerelease_flag);

        if (want_prerelease) {
            if (is_prerelease && !selected_release) {
                selected_release = release;
            } else if (!is_prerelease && !fallback_release) {
                fallback_release = release;
            }
        } else {
            if (!is_prerelease) {
                selected_release = release;
                break;
            }
        }
    }

    if (want_prerelease && !selected_release && fallback_release) {
        selected_release = fallback_release;
    }

    if (!selected_release) {
        ESP_LOGW(TAG, "No suitable release found");
        cJSON_Delete(releases_array);
        return ESP_ERR_NOT_FOUND;
    }

    // Parse manifest from the selected release
    err = parse_manifest_from_release(selected_release, manifest);
    cJSON_Delete(releases_array);

    return err;
}

