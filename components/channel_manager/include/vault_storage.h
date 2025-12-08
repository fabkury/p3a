#ifndef VAULT_STORAGE_H
#define VAULT_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vault Storage - manages artwork files with SHA256-based deduplication
 * 
 * Files are stored in a sharded directory structure:
 *   /vault/ab/cd/<full_sha256>.<ext>
 * 
 * Where:
 *   - "ab" is the first byte of SHA256 as hex
 *   - "cd" is the second byte of SHA256 as hex
 *   - Full SHA256 hash is used as filename
 *   - Extension indicates file format (.webp, .gif, .png, .jpg)
 * 
 * Each artwork file can have an optional JSON sidecar:
 *   /vault/ab/cd/<full_sha256>.json
 * 
 * All file operations use atomic writes (.tmp + fsync + rename).
 */

/**
 * @brief Vault file types (extension)
 */
typedef enum {
    VAULT_FILE_WEBP = 0,
    VAULT_FILE_GIF  = 1,
    VAULT_FILE_PNG  = 2,
    VAULT_FILE_JPEG = 3,
} vault_file_type_t;

/**
 * @brief Vault storage handle
 */
typedef struct vault_storage_s *vault_handle_t;

/**
 * @brief Initialize vault storage
 * 
 * @param base_path Base directory for vault (e.g., "/sdcard/vault")
 * @param out_handle Pointer to receive vault handle
 * @return ESP_OK on success
 */
esp_err_t vault_init(const char *base_path, vault_handle_t *out_handle);

/**
 * @brief Deinitialize vault and free resources
 * 
 * @param handle Vault handle
 */
void vault_deinit(vault_handle_t handle);

/**
 * @brief Check if a file exists in the vault
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes)
 * @param type File type
 * @return true if file exists
 */
bool vault_file_exists(vault_handle_t handle, const uint8_t *sha256, vault_file_type_t type);

/**
 * @brief Get the full path for a vault file
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes)
 * @param type File type
 * @param out_path Buffer to receive path
 * @param out_path_len Length of buffer
 * @return ESP_OK on success
 */
esp_err_t vault_get_file_path(vault_handle_t handle, 
                               const uint8_t *sha256, 
                               vault_file_type_t type,
                               char *out_path, 
                               size_t out_path_len);

/**
 * @brief Store a file in the vault atomically
 * 
 * Creates necessary subdirectories and writes file atomically.
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes) - should match content hash
 * @param type File type
 * @param data File data
 * @param data_len Data length
 * @return ESP_OK on success
 */
esp_err_t vault_store_file(vault_handle_t handle,
                           const uint8_t *sha256,
                           vault_file_type_t type,
                           const void *data,
                           size_t data_len);

/**
 * @brief Delete a file from the vault
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes)
 * @param type File type
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not exists
 */
esp_err_t vault_delete_file(vault_handle_t handle,
                            const uint8_t *sha256,
                            vault_file_type_t type);

/**
 * @brief Check if sidecar JSON exists for a file
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes)
 * @return true if sidecar exists
 */
bool vault_sidecar_exists(vault_handle_t handle, const uint8_t *sha256);

/**
 * @brief Get the sidecar JSON path
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes)
 * @param out_path Buffer to receive path
 * @param out_path_len Length of buffer
 * @return ESP_OK on success
 */
esp_err_t vault_get_sidecar_path(vault_handle_t handle,
                                  const uint8_t *sha256,
                                  char *out_path,
                                  size_t out_path_len);

/**
 * @brief Store sidecar JSON atomically
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes)
 * @param json_str JSON content (null-terminated)
 * @return ESP_OK on success
 */
esp_err_t vault_store_sidecar(vault_handle_t handle,
                               const uint8_t *sha256,
                               const char *json_str);

/**
 * @brief Read sidecar JSON content
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes)
 * @param out_json Buffer to receive JSON (will be null-terminated)
 * @param out_json_len Length of buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not exists
 */
esp_err_t vault_read_sidecar(vault_handle_t handle,
                              const uint8_t *sha256,
                              char *out_json,
                              size_t out_json_len);

/**
 * @brief Delete sidecar JSON
 * 
 * @param handle Vault handle
 * @param sha256 SHA256 hash (32 bytes)
 * @return ESP_OK on success
 */
esp_err_t vault_delete_sidecar(vault_handle_t handle, const uint8_t *sha256);

/**
 * @brief Get vault statistics
 */
typedef struct {
    size_t total_files;      // Number of artwork files
    size_t total_sidecars;   // Number of sidecar JSONs
    size_t total_bytes;      // Approximate total storage used
} vault_stats_t;

/**
 * @brief Get vault statistics (may be slow - scans directories)
 * 
 * @param handle Vault handle
 * @param out_stats Pointer to receive statistics
 * @return ESP_OK on success
 */
esp_err_t vault_get_stats(vault_handle_t handle, vault_stats_t *out_stats);

/**
 * @brief Parse SHA256 hex string to bytes
 * 
 * @param hex_str 64-character hex string
 * @param out_sha256 Buffer for 32 bytes
 * @return ESP_OK on success
 */
esp_err_t vault_parse_sha256(const char *hex_str, uint8_t *out_sha256);

/**
 * @brief Format SHA256 bytes as hex string
 * 
 * @param sha256 32-byte hash
 * @param out_hex Buffer for 65 characters (64 + null)
 * @param out_len Buffer length
 * @return ESP_OK on success
 */
esp_err_t vault_format_sha256(const uint8_t *sha256, char *out_hex, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif // VAULT_STORAGE_H

