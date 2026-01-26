// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef LOAD_TRACKER_H
#define LOAD_TRACKER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file load_tracker.h
 * @brief Load Tracker File (LTF) system - prevents infinite re-download loops
 *
 * When an artwork file fails to load (corrupted, unsupported format, etc.),
 * the LTF system tracks failure attempts to prevent infinite re-download loops.
 *
 * ## 3-Strike Rule
 *
 * 1. **First failure**: Create LTF with attempts=1, delete file, allow re-download
 * 2. **Second failure**: Update LTF to attempts=2, delete file, allow re-download
 * 3. **Third failure**: Set LTF terminal=true, delete file, block future downloads
 *
 * ## LTF File Format
 *
 * LTF files are small JSON files stored alongside the artwork in the vault:
 *   /vault/{sha[0]:02x}/{sha[1]:02x}/{sha[2]:02x}/{storage_key}.ltf
 *
 * Example:
 * ```json
 * {
 *   "attempts": 2,
 *   "terminal": false,
 *   "last_failure": 1704067200,
 *   "reason": "decode_error"
 * }
 * ```
 *
 * ## Usage Flow
 *
 * **Download Manager (before download):**
 * ```c
 * if (!ltf_can_download(storage_key, vault_path)) {
 *     // Skip this entry - terminal failure
 *     continue;
 * }
 * ```
 *
 * **Play Scheduler (on load failure):**
 * ```c
 * ltf_record_failure(storage_key, vault_path, "decode_error");
 * unlink(filepath);  // Delete corrupted file
 * lai_remove_entry(cache, ci_index);
 * ```
 *
 * **Download Manager (on success):**
 * ```c
 * ltf_clear(storage_key, vault_path);  // Clear any previous failures
 * ```
 */

// Maximum failure attempts before terminal state
#define LTF_MAX_ATTEMPTS 3

// Maximum length of failure reason string
#define LTF_REASON_MAX_LEN 32

// Download failure backoff parameters
#define LTF_BACKOFF_INITIAL_SEC     1
#define LTF_BACKOFF_MAX_SEC         30
#define LTF_BACKOFF_MULTIPLIER      2
#define LTF_MAX_DOWNLOAD_ATTEMPTS   5
#define LTF_COOLDOWN_SEC            300  // 5 minutes after max attempts

/**
 * @brief Error classification for download failures
 */
typedef enum {
    LTF_ERROR_CLASS_NONE = 0,       // No error / unknown
    LTF_ERROR_CLASS_TRANSIENT,      // Temporary error - retry with backoff
    LTF_ERROR_CLASS_PERMANENT,      // Permanent error - don't retry
} ltf_error_class_t;

/**
 * @brief Load Tracker state for an artwork
 *
 * Extended to support download failure tracking with exponential backoff.
 * New fields are backward compatible - missing fields in JSON default to 0.
 */
typedef struct {
    uint8_t attempts;               // Number of load attempts (0-3)
    uint8_t download_attempts;      // Number of download failures
    bool terminal;                  // If true, no more re-downloads allowed
    time_t last_failure;            // Unix timestamp of last failure
    time_t retry_after;             // Earliest time to retry download (0 = now)
    ltf_error_class_t error_class;  // Classification of last error
    char reason[LTF_REASON_MAX_LEN];  // Failure reason (e.g., "decode_error")
} load_tracker_t;

/**
 * @brief Check if an artwork can be downloaded
 *
 * Returns false if a terminal LTF exists for this storage_key.
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @return true if download is allowed, false if blocked by terminal LTF
 */
bool ltf_can_download(const char *storage_key, const char *vault_path);

/**
 * @brief Record a load failure
 *
 * Increments the failure counter. If this is the 3rd failure, marks as terminal.
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @param reason Failure reason (e.g., "decode_error", "file_missing", "timeout")
 * @return ESP_OK on success
 */
esp_err_t ltf_record_failure(const char *storage_key, const char *vault_path, const char *reason);

/**
 * @brief Clear LTF for an artwork
 *
 * Called on successful load to clear any previous failure tracking.
 * Deletes the LTF file if it exists.
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @return ESP_OK on success (even if LTF didn't exist)
 */
esp_err_t ltf_clear(const char *storage_key, const char *vault_path);

/**
 * @brief Load LTF state for an artwork
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @param out Pointer to receive LTF state
 * @return ESP_OK if LTF exists and was loaded
 *         ESP_ERR_NOT_FOUND if no LTF exists
 */
esp_err_t ltf_load(const char *storage_key, const char *vault_path, load_tracker_t *out);

/**
 * @brief Check if LTF is terminal (no more retries allowed)
 *
 * Convenience function combining ltf_load and terminal check.
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @return true if LTF exists and is terminal
 */
bool ltf_is_terminal(const char *storage_key, const char *vault_path);

/**
 * @brief Get the number of remaining download attempts
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @return Number of attempts remaining (0 if terminal, LTF_MAX_ATTEMPTS if no LTF)
 */
int ltf_get_remaining_attempts(const char *storage_key, const char *vault_path);

/**
 * @brief Build LTF file path for a storage key
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @param out Output buffer for path
 * @param out_len Length of output buffer
 */
void ltf_build_path(const char *storage_key, const char *vault_path,
                    char *out, size_t out_len);

// ============================================================================
// Download Failure Tracking API (Exponential Backoff)
// ============================================================================

/**
 * @brief Classify an error as transient or permanent
 *
 * Permanent errors (404, 403, 410, ESP_ERR_NO_MEM) should not be retried.
 * Transient errors (timeouts, connection failures, 5xx) use exponential backoff.
 *
 * @param err ESP error code from download attempt
 * @param http_status HTTP status code (0 if not applicable)
 * @return Error classification
 */
ltf_error_class_t ltf_classify_error(esp_err_t err, int http_status);

/**
 * @brief Record a download failure with automatic backoff calculation
 *
 * For transient errors: increments download_attempts, calculates backoff.
 * For permanent errors: marks as terminal immediately.
 *
 * Backoff progression: 1s → 2s → 4s → 8s → 16s → 30s (capped)
 * After 5 transient failures: 5-minute cooldown before retry
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @param err ESP error code from download attempt
 * @param http_status HTTP status code (0 if not applicable)
 * @return ESP_OK on success
 */
esp_err_t ltf_record_download_failure(const char *storage_key, const char *vault_path,
                                       esp_err_t err, int http_status);

/**
 * @brief Check if a file can be downloaded now
 *
 * Returns false if:
 * - LTF exists and is terminal (permanent failure)
 * - LTF exists with retry_after > current time (still in backoff)
 *
 * This replaces ltf_can_download() for download manager use.
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @return true if download is allowed now, false if blocked
 */
bool ltf_can_download_now(const char *storage_key, const char *vault_path);

/**
 * @brief Get the number of seconds until retry is allowed
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @return Seconds until retry allowed (0 = can retry now or no LTF)
 */
uint32_t ltf_get_retry_delay(const char *storage_key, const char *vault_path);

/**
 * @brief Reset download failure tracking for a file
 *
 * Called on successful download to clear download_attempts and retry_after.
 * Does NOT clear load failure tracking (attempts, terminal).
 *
 * @param storage_key Artwork storage key (UUID string)
 * @param vault_path Base path for vault files
 * @return ESP_OK on success
 */
esp_err_t ltf_clear_download_failures(const char *storage_key, const char *vault_path);

#ifdef __cplusplus
}
#endif

#endif // LOAD_TRACKER_H
