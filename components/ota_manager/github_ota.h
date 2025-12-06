/**
 * @file github_ota.h
 * @brief GitHub Releases API client for OTA updates
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GitHub release information
 */
typedef struct {
    char version[32];           ///< Version string (without 'v' prefix)
    char download_url[256];     ///< Direct download URL for firmware binary
    char sha256_url[256];       ///< URL for SHA256 checksum file
    uint32_t firmware_size;     ///< Firmware size in bytes
    bool is_prerelease;         ///< Whether this is a pre-release
    char release_notes[512];    ///< Truncated release notes
    char tag_name[32];          ///< Git tag name (e.g., "v1.0.0")
} github_release_info_t;

/**
 * @brief Fetch latest release info from GitHub
 * 
 * Queries the GitHub Releases API for the latest release.
 * 
 * @param[out] info Pointer to structure to fill with release info
 * @return ESP_OK on success
 *         ESP_ERR_NOT_FOUND if no release or no firmware asset found
 *         ESP_ERR_HTTP_CONNECT on network error
 *         ESP_ERR_NO_MEM on memory allocation failure
 */
esp_err_t github_ota_get_latest_release(github_release_info_t *info);

/**
 * @brief Download SHA256 checksum from GitHub release
 * 
 * @param sha256_url URL to the .sha256 file
 * @param[out] sha256_hex Buffer to store 64-character hex string (must be >= 65 bytes)
 * @param hex_buf_size Size of the output buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t github_ota_download_sha256(const char *sha256_url, char *sha256_hex, size_t hex_buf_size);

/**
 * @brief Parse version string to comparable integer
 * 
 * Converts "1.2.3" or "v1.2.3" to a packed integer for comparison.
 * 
 * @param version_str Version string (e.g., "1.2.3" or "v1.2.3")
 * @return Packed version (major<<16 | minor<<8 | patch), or 0 on parse error
 */
uint32_t github_ota_parse_version(const char *version_str);

/**
 * @brief Compare two version strings
 * 
 * @param v1 First version string
 * @param v2 Second version string
 * @return >0 if v1 > v2, <0 if v1 < v2, 0 if equal or parse error
 */
int github_ota_compare_versions(const char *v1, const char *v2);

/**
 * @brief Convert hex string to binary
 * 
 * @param hex 64-character hex string
 * @param[out] bin Output buffer (must be at least 32 bytes)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid input
 */
esp_err_t github_ota_hex_to_bin(const char *hex, uint8_t *bin);

#ifdef __cplusplus
}
#endif

