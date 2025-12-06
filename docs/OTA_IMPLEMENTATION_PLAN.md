# p3a OTA Implementation Plan

> **Project**: p3a Pixel Art Animation Player  
> **Feature**: Over-The-Air Updates via GitHub Releases  
> **Status**: ✅ Phase 3 - Implementation Complete (Pending Testing)  
> **Last Updated**: December 6, 2025 (Phase 3 Complete)

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Requirements](#2-requirements)
3. [Architecture Overview](#3-architecture-overview)
4. [Partition Table Redesign](#4-partition-table-redesign)
5. [Version Management](#5-version-management)
6. [GitHub Releases API Integration](#6-github-releases-api-integration)
7. [OTA State Machine](#7-ota-state-machine)
8. [Download & Flash Flow](#8-download--flash-flow)
9. [Rollback Mechanism](#9-rollback-mechanism)
10. [Web UI Integration](#10-web-ui-integration)
11. [Progress Display (LCD)](#11-progress-display-lcd)
12. [Safety Constraints](#12-safety-constraints)
13. [Security Considerations](#13-security-considerations)
14. [Component Design](#14-component-design)
15. [Implementation Phases](#15-implementation-phases)
16. [Testing Strategy](#16-testing-strategy)
17. [Progress Tracking](#17-progress-tracking)

---

## 1. Executive Summary

This document details the implementation of Over-The-Air (OTA) firmware updates for p3a, leveraging GitHub Releases as the distribution infrastructure and ESP-IDF's native OTA APIs.

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Update Source | GitHub Releases API | Native integration, version metadata, release notes |
| Update Trigger | Periodic (2h) + Manual | Balance between freshness and resource usage |
| Auto-Install | No | User must explicitly approve installation |
| Partition Scheme | Dual OTA (ota_0 + ota_1) | Required for safe atomic updates |
| Rollback | Automatic (3 failures) + Manual | Maximum reliability |
| Signing | HTTPS + SHA256 | Sufficient security without Secure Boot complexity |
| Progress UI | LCD display during OTA | Clear feedback, no animation playback during update |

---

## 2. Requirements

### 2.1 Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| FR-01 | Check for updates every 2 hours | Must |
| FR-02 | Manual update check via Web UI | Must |
| FR-03 | Display available version in Web UI | Must |
| FR-04 | User-initiated download and install only | Must |
| FR-05 | Show download progress on LCD | Must |
| FR-06 | Automatic rollback after 3 boot failures | Must |
| FR-07 | Manual rollback via Web UI | Must |
| FR-08 | Block OTA during PICO-8 streaming | Must |
| FR-09 | Block OTA during USB mass storage mode | Must |
| FR-10 | Stop all animations during OTA process | Must |

### 2.2 Non-Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| NFR-01 | OTA must be atomic (power-loss safe) | Must |
| NFR-02 | Download must verify SHA256 checksum | Must |
| NFR-03 | Use HTTPS for all GitHub API calls | Must |
| NFR-04 | Maximum download timeout: 10 minutes | Should |
| NFR-05 | Minimal memory footprint during OTA | Should |

---

## 3. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                          p3a Firmware                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐   │
│  │  ota_manager │◄───│  http_api    │◄───│  Web UI (Browser)    │   │
│  │  (component) │    │  (endpoints) │    │  /ota, /ota/check... │   │
│  └──────┬───────┘    └──────────────┘    └──────────────────────┘   │
│         │                                                            │
│         ▼                                                            │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐   │
│  │ github_ota   │───►│ esp_https_ota│───►│  esp_ota_ops         │   │
│  │ (API client) │    │ (download)   │    │  (flash write)       │   │
│  └──────┬───────┘    └──────────────┘    └──────────────────────┘   │
│         │                                                            │
│         ▼                                                            │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    GitHub Releases API                        │   │
│  │         https://api.github.com/repos/fabkury/p3a/releases     │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Component Responsibilities

| Component | Responsibility |
|-----------|----------------|
| `ota_manager` | Orchestrates OTA flow, state machine, periodic checks |
| `github_ota` | GitHub Releases API client, version parsing, asset URL extraction |
| `esp_https_ota` | ESP-IDF native HTTPS OTA download with streaming flash |
| `http_api` | Web UI endpoints for OTA status, check, install, rollback |
| `ugfx_ui` | LCD progress display during OTA |

---

## 4. Partition Table Redesign

### 4.1 Current Partition Table

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, ,        8M,
storage,  data, spiffs,  ,        1M,
```

**Problem**: Single `factory` partition cannot support OTA updates.

### 4.2 New Partition Table (OTA-enabled)

```csv
# Name,    Type, SubType,  Offset,   Size,    Flags
# NVS and PHY partitions (unchanged)
nvs,       data, nvs,      0x9000,   0x6000,
phy_init,  data, phy,      0xf000,   0x1000,

# OTA data partition (stores boot selection)
otadata,   data, ota,      0x10000,  0x2000,

# Dual OTA app partitions (8MB each = 16MB total for apps)
ota_0,     app,  ota_0,    0x20000,  0x800000,
ota_1,     app,  ota_1,    0x820000, 0x800000,

# SPIFFS for web UI assets (unchanged size)
storage,   data, spiffs,   0x1020000, 0x100000,
```

### 4.3 Partition Layout Visualization

```
Flash Address Map (32MB Flash):
┌────────────────┬──────────────────────────────────────────────┐
│ 0x0000_0000    │ Bootloader (fixed, ~32KB)                    │
├────────────────┼──────────────────────────────────────────────┤
│ 0x0000_8000    │ Partition Table (4KB)                        │
├────────────────┼──────────────────────────────────────────────┤
│ 0x0000_9000    │ NVS (24KB)                                   │
├────────────────┼──────────────────────────────────────────────┤
│ 0x0000_F000    │ PHY Init (4KB)                               │
├────────────────┼──────────────────────────────────────────────┤
│ 0x0001_0000    │ OTA Data (8KB)                               │
├────────────────┼──────────────────────────────────────────────┤
│ 0x0002_0000    │ ota_0 (8MB) - Primary App Slot               │
├────────────────┼──────────────────────────────────────────────┤
│ 0x0082_0000    │ ota_1 (8MB) - Secondary App Slot             │
├────────────────┼──────────────────────────────────────────────┤
│ 0x0102_0000    │ SPIFFS Storage (1MB)                         │
├────────────────┼──────────────────────────────────────────────┤
│ 0x0112_0000    │ Unused (~15MB available for future use)      │
└────────────────┴──────────────────────────────────────────────┘
```

### 4.4 Migration Considerations

> ⚠️ **IMPORTANT**: Changing from `factory` to `ota_0/ota_1` partition scheme requires a **full re-flash** of existing devices. This is a one-time migration.

After the partition table change:
1. First boot will be into `ota_0`
2. `otadata` will be initialized by bootloader
3. Future OTA updates will alternate between `ota_0` and `ota_1`

---

## 5. Version Management

### 5.1 Version String Format

Use **Semantic Versioning 2.0** with optional build metadata:

```
MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]

Examples:
  1.0.0           - Initial stable release
  1.0.1           - Patch release
  1.1.0           - Minor feature release
  2.0.0           - Major breaking change
  1.2.0-beta.1    - Pre-release (future use)
  1.0.0+abc1234   - Build metadata (git SHA)
```

### 5.2 Version Storage

**Compile-time** (in `version.h`):
```c
#pragma once

#define FW_VERSION_MAJOR  1
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  0

#define FW_VERSION        "1.0.0"
#define FW_DEVICE_MODEL   "p3a"

// For OTA comparison (packed as single integer)
#define FW_VERSION_CODE   ((FW_VERSION_MAJOR << 16) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)
```

**Runtime** (ESP-IDF app description):
```c
// Automatically embedded in firmware binary
const esp_app_desc_t *app_desc = esp_app_get_description();
// app_desc->version contains FW_VERSION string
```

### 5.3 Version Comparison Algorithm

```c
/**
 * Parse semantic version string into comparable integer
 * Returns 0 on parse failure
 * 
 * "1.2.3" -> 0x010203 (66051)
 * "2.0.0" -> 0x020000 (131072)
 */
uint32_t ota_parse_version(const char *version_str);

/**
 * Compare two version strings
 * Returns:
 *   > 0 if v1 > v2 (v1 is newer)
 *   < 0 if v1 < v2 (v2 is newer)
 *   = 0 if v1 == v2 (same version)
 */
int ota_compare_versions(const char *v1, const char *v2);

/**
 * Check if remote version is newer than current
 */
bool ota_is_update_available(const char *remote_version) {
    const esp_app_desc_t *app = esp_app_get_description();
    return ota_compare_versions(remote_version, app->version) > 0;
}
```

**Pseudocode for version parsing:**
```
function parse_version(str):
    // Strip leading 'v' if present ("v1.0.0" -> "1.0.0")
    if str starts with 'v' or 'V':
        str = str[1:]
    
    // Extract major.minor.patch
    parts = split(str, '.')
    if len(parts) < 3:
        return 0  // Invalid
    
    major = parse_int(parts[0])
    minor = parse_int(parts[1])
    
    // Patch may have suffix like "0-beta" or "0+build"
    patch_str = parts[2]
    patch = parse_int(patch_str)  // Stops at first non-digit
    
    // Validate ranges (0-255 each)
    if major > 255 or minor > 255 or patch > 255:
        return 0
    
    return (major << 16) | (minor << 8) | patch
```

---

## 6. GitHub Releases API Integration

### 6.1 API Endpoint

```
GET https://api.github.com/repos/fabkury/p3a/releases/latest
Accept: application/vnd.github+json
```

### 6.2 Response Structure (Relevant Fields)

```json
{
  "tag_name": "v1.1.0",
  "name": "Release 1.1.0",
  "body": "## What's New\n- Feature A\n- Bug fix B",
  "prerelease": false,
  "published_at": "2025-12-06T10:00:00Z",
  "assets": [
    {
      "name": "p3a.bin",
      "browser_download_url": "https://github.com/fabkury/p3a/releases/download/v1.1.0/p3a.bin",
      "size": 3145728,
      "content_type": "application/octet-stream"
    },
    {
      "name": "p3a.bin.sha256",
      "browser_download_url": "https://github.com/fabkury/p3a/releases/download/v1.1.0/p3a.bin.sha256",
      "size": 64,
      "content_type": "text/plain"
    }
  ]
}
```

### 6.3 Release Asset Naming Convention

| Asset | Description | Required |
|-------|-------------|----------|
| `p3a.bin` | Compiled firmware binary | Yes |
| `p3a.bin.sha256` | SHA256 hash (hex string) | Yes |

**SHA256 file format**: Plain text, 64 hex characters, no newlines or extra text.
```
a1b2c3d4e5f6...  (64 hex chars)
```

### 6.4 GitHub API Client Pseudocode

```c
typedef struct {
    char version[32];           // e.g., "1.1.0" (without 'v' prefix)
    char download_url[256];     // Direct download URL for p3a.bin
    char sha256_url[256];       // URL for p3a.bin.sha256
    uint32_t firmware_size;     // Size in bytes
    bool is_prerelease;         // Should be false for stable
    char release_notes[512];    // Truncated release notes for UI
} github_release_info_t;

/**
 * Fetch latest release info from GitHub
 * 
 * @param[out] info  Populated on success
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t github_ota_get_latest_release(github_release_info_t *info);
```

**Implementation pseudocode:**

```
function github_ota_get_latest_release(info):
    // 1. Perform HTTPS GET request
    url = "https://api.github.com/repos/fabkury/p3a/releases/latest"
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "p3a-ota/1.0"  // GitHub requires User-Agent
    }
    
    response = https_get(url, headers)
    if response.status != 200:
        return ESP_ERR_HTTP_FETCH_HEADER
    
    // 2. Parse JSON response
    json = cJSON_Parse(response.body)
    if json == NULL:
        return ESP_ERR_INVALID_RESPONSE
    
    // 3. Check if prerelease (skip if true)
    prerelease = cJSON_GetObjectItem(json, "prerelease")
    if cJSON_IsTrue(prerelease):
        info->is_prerelease = true
        // Could return error or just flag it
    
    // 4. Extract version from tag_name
    tag_name = cJSON_GetStringValue(cJSON_GetObjectItem(json, "tag_name"))
    // Strip 'v' prefix: "v1.1.0" -> "1.1.0"
    if tag_name[0] == 'v':
        strcpy(info->version, tag_name + 1)
    else:
        strcpy(info->version, tag_name)
    
    // 5. Find p3a.bin and p3a.bin.sha256 in assets
    assets = cJSON_GetObjectItem(json, "assets")
    for each asset in assets:
        name = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "name"))
        url = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "browser_download_url"))
        
        if strcmp(name, "p3a.bin") == 0:
            strcpy(info->download_url, url)
            info->firmware_size = cJSON_GetNumberValue(cJSON_GetObjectItem(asset, "size"))
        
        if strcmp(name, "p3a.bin.sha256") == 0:
            strcpy(info->sha256_url, url)
    
    // 6. Validate required fields
    if strlen(info->download_url) == 0:
        return ESP_ERR_NOT_FOUND  // No p3a.bin asset
    
    // 7. Extract release notes (truncated)
    body = cJSON_GetStringValue(cJSON_GetObjectItem(json, "body"))
    if body:
        strncpy(info->release_notes, body, sizeof(info->release_notes) - 1)
    
    cJSON_Delete(json)
    return ESP_OK
```

### 6.5 Rate Limiting Considerations

GitHub API rate limits:
- **Unauthenticated**: 60 requests/hour per IP
- **Authenticated**: 5,000 requests/hour

With 2-hour check interval, we use ~12 requests/day, well under limits.

**Handling 403 rate limit responses:**
```c
// If GitHub returns 403, check for rate limit
if (status_code == 403) {
    // Retry-After header or X-RateLimit-Reset
    // Back off and retry later
    return ESP_ERR_HTTP_CONNECT;  // Signal transient failure
}
```

---

## 7. OTA State Machine

### 7.1 State Definitions

```c
typedef enum {
    OTA_STATE_IDLE,              // No update activity
    OTA_STATE_CHECKING,          // Querying GitHub API
    OTA_STATE_UPDATE_AVAILABLE,  // New version found, awaiting user action
    OTA_STATE_DOWNLOADING,       // Downloading firmware
    OTA_STATE_VERIFYING,         // Verifying SHA256
    OTA_STATE_FLASHING,          // Writing to flash (implicit in esp_https_ota)
    OTA_STATE_PENDING_REBOOT,    // Flash complete, reboot required
    OTA_STATE_ERROR,             // Error occurred
} ota_state_t;
```

### 7.2 State Transition Diagram

```
                            ┌─────────────────┐
                            │   OTA_IDLE      │◄──────────────────────┐
                            └────────┬────────┘                       │
                                     │                                │
                        [Timer tick or manual check]                  │
                                     │                                │
                                     ▼                                │
                            ┌─────────────────┐                       │
                            │  OTA_CHECKING   │                       │
                            └────────┬────────┘                       │
                                     │                                │
                    ┌────────────────┼────────────────┐               │
                    │                │                │               │
              [No update]    [Update found]     [Error]               │
                    │                │                │               │
                    ▼                ▼                ▼               │
              ┌─────────┐   ┌───────────────┐   ┌─────────┐           │
              │  IDLE   │   │ UPDATE_AVAIL  │   │  ERROR  │───────────┤
              └─────────┘   └───────┬───────┘   └─────────┘           │
                                    │                                 │
                          [User clicks "Install"]                     │
                                    │                                 │
                                    ▼                                 │
                            ┌───────────────┐                         │
                            │ DOWNLOADING   │                         │
                            │ (+ progress)  │                         │
                            └───────┬───────┘                         │
                                    │                                 │
                    ┌───────────────┴───────────────┐                 │
                    │                               │                 │
              [Download OK]                   [Download fail]         │
                    │                               │                 │
                    ▼                               ▼                 │
            ┌───────────────┐               ┌─────────────┐           │
            │  VERIFYING    │               │   ERROR     │───────────┤
            └───────┬───────┘               └─────────────┘           │
                    │                                                 │
            ┌───────┴───────┐                                         │
            │               │                                         │
      [SHA256 OK]    [SHA256 fail]                                    │
            │               │                                         │
            ▼               ▼                                         │
    ┌───────────────┐ ┌─────────────┐                                 │
    │   FLASHING    │ │   ERROR     │─────────────────────────────────┤
    │ (implicit)    │ └─────────────┘                                 │
    └───────┬───────┘                                                 │
            │                                                         │
      [Flash complete]                                                │
            │                                                         │
            ▼                                                         │
    ┌───────────────┐                                                 │
    │PENDING_REBOOT │                                                 │
    └───────┬───────┘                                                 │
            │                                                         │
      [Auto reboot after 3s]                                          │
            │                                                         │
            ▼                                                         │
    ┌───────────────┐                                                 │
    │   (Reboot)    │─────────[New firmware boots]────────────────────┘
    └───────────────┘
```

### 7.3 State Persistence

OTA state is **volatile** (RAM only) — lost on reboot. This is intentional:
- After reboot, OTA starts fresh in `IDLE`
- No complex recovery from partial states
- `otadata` partition handles boot slot selection (ESP-IDF managed)

---

## 8. Download & Flash Flow

### 8.1 High-Level Flow

```
1. Pre-flight checks
   ├── Check device state (not in PICO-8, not in USB MSC)
   ├── Check Wi-Fi connectivity
   └── Verify sufficient conditions for OTA

2. Enter OTA mode
   ├── Stop animation playback
   ├── Stop auto-swap task
   ├── Show "Preparing update..." on LCD
   └── Disable HTTP API file upload endpoint

3. Download SHA256 checksum
   ├── HTTPS GET p3a.bin.sha256
   ├── Parse 64-character hex string
   └── Store expected hash

4. Stream firmware with verification
   ├── Use esp_https_ota_begin() with config
   ├── Loop: esp_https_ota_perform() with progress callback
   │   ├── Update LCD progress bar
   │   └── Feed watchdog
   ├── Compute SHA256 incrementally during download
   └── esp_https_ota_finish()

5. Verify checksum
   ├── Compare computed SHA256 with expected
   └── If mismatch: abort, mark as error

6. Set boot partition
   ├── esp_ota_set_boot_partition() to new slot
   └── Mark as "pending verification" (for rollback)

7. Reboot
   ├── Show "Rebooting..." for 3 seconds
   └── esp_restart()

8. Post-boot validation (automatic, in app startup)
   ├── Check if running from OTA partition
   ├── If yes and not yet validated:
   │   ├── Run self-tests
   │   └── esp_ota_mark_app_valid_cancel_rollback()
   └── If self-test fails 3 times: rollback occurs automatically
```

### 8.2 esp_https_ota Integration

ESP-IDF provides `esp_https_ota` component for streaming OTA:

```c
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

esp_err_t perform_ota_update(const char *firmware_url, const uint8_t *expected_sha256) {
    esp_err_t err;
    
    // 1. Configure HTTPS OTA
    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .cert_pem = NULL,  // Use certificate bundle
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .partial_http_download = false,
        .max_http_request_size = 0,  // No limit
    };
    
    // 2. Begin OTA
    esp_https_ota_handle_t ota_handle = NULL;
    err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    // 3. Get image info
    esp_app_desc_t new_app_info;
    err = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (err != ESP_OK) {
        esp_https_ota_abort(ota_handle);
        return err;
    }
    
    // 4. Optional: verify version in image matches expected
    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
    
    // 5. Stream download with progress
    int total_size = esp_https_ota_get_image_size(ota_handle);
    int downloaded = 0;
    
    // Initialize SHA256 context for verification
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);  // 0 = SHA-256
    
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        downloaded = esp_https_ota_get_image_len_read(ota_handle);
        int progress_percent = (downloaded * 100) / total_size;
        
        // Update LCD progress
        ota_ui_update_progress(progress_percent);
        
        // Feed watchdog
        esp_task_wdt_reset();
    }
    
    if (err != ESP_OK) {
        esp_https_ota_abort(ota_handle);
        return err;
    }
    
    // 6. Finish OTA (validates image and sets boot partition)
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    // 7. Verify SHA256 (if we had access to raw bytes, which esp_https_ota doesn't expose)
    // Note: esp_https_ota handles flash internally, so SHA256 verification needs
    // to be done by reading back from flash or using a custom approach.
    // Alternative: Trust HTTPS + rely on esp_https_ota's internal image validation
    
    return ESP_OK;
}
```

### 8.3 SHA256 Verification Strategy

Since `esp_https_ota` streams directly to flash without exposing raw bytes:

**Read-back Verification**
- After `esp_https_ota_finish()`, read firmware from flash
- Compute SHA256 of the written partition
- Compare with downloaded `.sha256` file

```c
esp_err_t verify_ota_partition_sha256(const esp_partition_t *partition, 
                                       size_t size,
                                       const uint8_t expected_sha256[32]) {
    mbedtls_sha256_context ctx;
    uint8_t computed_sha256[32];
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_DMA);
    
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = MIN(4096, size - offset);
        esp_partition_read(partition, offset, buf, chunk);
        mbedtls_sha256_update(&ctx, buf, chunk);
        offset += chunk;
    }
    
    mbedtls_sha256_finish(&ctx, computed_sha256);
    mbedtls_sha256_free(&ctx);
    free(buf);
    
    if (memcmp(computed_sha256, expected_sha256, 32) != 0) {
        ESP_LOGE(TAG, "SHA256 verification FAILED!");
        return ESP_ERR_INVALID_CRC;
    }
    
    ESP_LOGI(TAG, "SHA256 verification passed");
    return ESP_OK;
}
```

---

## 9. Rollback Mechanism

### 9.1 ESP-IDF Rollback Configuration

Add to `sdkconfig.defaults`:

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n
CONFIG_APP_ROLLBACK_ENABLE=y
```

### 9.2 Boot Validation Flow

```c
void app_main(void) {
    // Early in startup, check if we need to validate this boot
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Running from new OTA partition, validating...");
            
            // Run self-tests
            bool self_test_passed = run_self_tests();
            
            if (self_test_passed) {
                esp_ota_mark_app_valid_cancel_rollback();
                ESP_LOGI(TAG, "OTA firmware validated successfully");
            } else {
                ESP_LOGE(TAG, "Self-test failed, will rollback on next boot");
                esp_restart();  // This counts as a boot failure
            }
        }
    }
    
    // Continue normal startup...
}

bool run_self_tests(void) {
    // Basic sanity checks
    
    // 1. Check NVS is accessible
    nvs_handle_t h;
    if (nvs_open("test", NVS_READONLY, &h) == ESP_OK) {
        nvs_close(h);
    }
    
    // 2. Check LCD is initialized
    // 3. Check Wi-Fi module responds
    // 4. Check filesystem mounts
    
    // All passed
    return true;
}
```

### 9.3 Boot Failure Counter

ESP-IDF's bootloader automatically tracks boot failures:
- Increments counter on each boot
- Resets counter when `esp_ota_mark_app_valid_cancel_rollback()` is called
- If counter reaches threshold (3), rolls back to previous partition

**Configuration in `sdkconfig`:**
```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

### 9.4 Manual Rollback via API

```c
esp_err_t ota_manual_rollback(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(running);
    
    // Check if there's a valid image in the other slot
    esp_app_desc_t other_app_info;
    if (esp_ota_get_partition_description(next, &other_app_info) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;  // No valid image to rollback to
    }
    
    // Set next boot to the other partition
    esp_err_t err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        return err;
    }
    
    ESP_LOGI(TAG, "Rollback scheduled to version %s", other_app_info.version);
    return ESP_OK;  // Caller should reboot
}
```

---

## 10. Web UI Integration

### 10.1 New REST API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/ota/status` | Get current OTA state and version info |
| POST | `/ota/check` | Trigger immediate update check |
| POST | `/ota/install` | Start firmware download and installation |
| POST | `/ota/rollback` | Schedule rollback and reboot |

### 10.2 API Response Schemas

**GET /ota/status**
```json
{
  "ok": true,
  "data": {
    "state": "idle",
    "current_version": "1.0.0",
    "available_version": null,
    "available_size": null,
    "last_check": "2025-12-06T10:00:00Z",
    "last_check_result": "no_update",
    "can_rollback": true,
    "rollback_version": "0.9.5",
    "download_progress": null,
    "error_message": null
  }
}
```

**When update is available:**
```json
{
  "ok": true,
  "data": {
    "state": "update_available",
    "current_version": "1.0.0",
    "available_version": "1.1.0",
    "available_size": 3145728,
    "release_notes": "## What's New\n- Feature A\n- Bug fix B",
    "last_check": "2025-12-06T12:00:00Z",
    "last_check_result": "update_found",
    "can_rollback": true,
    "rollback_version": "0.9.5",
    "download_progress": null,
    "error_message": null
  }
}
```

**During download:**
```json
{
  "ok": true,
  "data": {
    "state": "downloading",
    "current_version": "1.0.0",
    "available_version": "1.1.0",
    "download_progress": 45,
    ...
  }
}
```

**POST /ota/check response:**
```json
{
  "ok": true,
  "data": {
    "checking": true,
    "message": "Update check started"
  }
}
```

**POST /ota/install response:**
```json
{
  "ok": true,
  "data": {
    "installing": true,
    "message": "Firmware update started. Device will reboot when complete."
  }
}
```

Error responses:
```json
{
  "ok": false,
  "error": "OTA not allowed during PICO-8 streaming",
  "code": "OTA_BLOCKED"
}
```

### 10.3 Web UI HTML Page

New page at `/ota` served from SPIFFS or embedded:

```html
<!-- Simplified structure - actual implementation will have full styling -->
<div class="ota-container">
  <h1>Firmware Update</h1>
  
  <div class="version-info">
    <p>Current Version: <span id="current-version">1.0.0</span></p>
    <p>Available Version: <span id="available-version">-</span></p>
  </div>
  
  <div class="actions">
    <button id="check-btn" onclick="checkForUpdate()">Check for Updates</button>
    <button id="install-btn" onclick="installUpdate()" disabled>Install Update</button>
    <button id="rollback-btn" onclick="rollback()">Rollback to Previous Version</button>
  </div>
  
  <div class="progress" id="progress-section" style="display:none">
    <div class="progress-bar">
      <div class="progress-fill" id="progress-fill" style="width: 0%"></div>
    </div>
    <p id="progress-text">Downloading... 0%</p>
  </div>
  
  <div class="release-notes" id="release-notes" style="display:none">
    <h3>Release Notes</h3>
    <div id="release-notes-content"></div>
  </div>
  
  <div class="status" id="status-message"></div>
</div>

<script>
async function checkForUpdate() {
  const response = await fetch('/ota/check', { method: 'POST' });
  const data = await response.json();
  if (data.ok) {
    pollStatus();
  }
}

async function installUpdate() {
  if (!confirm('Install firmware update? The device will reboot.')) return;
  
  const response = await fetch('/ota/install', { method: 'POST' });
  const data = await response.json();
  if (data.ok) {
    pollStatus();
  }
}

async function rollback() {
  if (!confirm('Rollback to previous firmware version? The device will reboot.')) return;
  
  const response = await fetch('/ota/rollback', { method: 'POST' });
  // Device will reboot
}

async function pollStatus() {
  const response = await fetch('/ota/status');
  const data = await response.json();
  
  // Update UI based on state
  updateUI(data.data);
  
  // Continue polling if in progress
  if (['checking', 'downloading', 'verifying', 'flashing'].includes(data.data.state)) {
    setTimeout(pollStatus, 500);
  }
}

// Poll on page load
pollStatus();
setInterval(pollStatus, 5000);  // Refresh every 5s when idle
</script>
```

---

## 11. Progress Display (LCD)

### 11.1 OTA UI States

The LCD should display different screens during OTA:

```
┌─────────────────────────────────────┐
│                                     │
│         ╔═══════════════╗           │
│         ║   UPDATING    ║           │
│         ╚═══════════════╝           │
│                                     │
│    Downloading firmware...          │
│                                     │
│    ┌─────────────────────────┐      │
│    │████████████░░░░░░░░░░░░░│ 45%  │
│    └─────────────────────────┘      │
│                                     │
│    Version: 1.0.0 → 1.1.0           │
│                                     │
│    Do not power off.                │
│                                     │
└─────────────────────────────────────┘
```

### 11.2 Implementation Using µGFX

Leverage existing `ugfx_ui` component:

```c
// In components/ugfx/include/ugfx_ui.h (add)
esp_err_t ugfx_ui_show_ota_progress(int percent, const char *status_text);
esp_err_t ugfx_ui_show_ota_complete(const char *message);
esp_err_t ugfx_ui_hide_ota(void);

// Implementation sketch
esp_err_t ugfx_ui_show_ota_progress(int percent, const char *status_text) {
    // Enter UI mode (stops animation)
    app_lcd_enter_ui_mode();
    
    // Clear screen with dark background
    gdispFillArea(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, HTML2COLOR(0x1a1a2e));
    
    // Draw title
    gdispFillStringBox(0, 100, SCREEN_WIDTH, 50, "UPDATING", 
                       font_large, HTML2COLOR(0x00ff88), HTML2COLOR(0x1a1a2e), 
                       justifyCenter);
    
    // Draw status text
    gdispFillStringBox(0, 180, SCREEN_WIDTH, 30, status_text,
                       font_medium, White, HTML2COLOR(0x1a1a2e),
                       justifyCenter);
    
    // Draw progress bar background
    int bar_x = 60;
    int bar_y = 300;
    int bar_w = SCREEN_WIDTH - 120;
    int bar_h = 40;
    gdispFillArea(bar_x, bar_y, bar_w, bar_h, HTML2COLOR(0x333355));
    
    // Draw progress bar fill
    int fill_w = (bar_w * percent) / 100;
    gdispFillArea(bar_x, bar_y, fill_w, bar_h, HTML2COLOR(0x00ff88));
    
    // Draw percentage text
    char pct_text[16];
    snprintf(pct_text, sizeof(pct_text), "%d%%", percent);
    gdispFillStringBox(0, 360, SCREEN_WIDTH, 30, pct_text,
                       font_medium, White, HTML2COLOR(0x1a1a2e),
                       justifyCenter);
    
    // Draw warning
    gdispFillStringBox(0, 500, SCREEN_WIDTH, 30, "DO NOT POWER OFF",
                       font_small, HTML2COLOR(0xff6666), HTML2COLOR(0x1a1a2e),
                       justifyCenter);
    
    return ESP_OK;
}
```

---

## 12. Safety Constraints

### 12.1 OTA Blockers

OTA installation should be **rejected** when:

| Condition | Check Function | Error Code |
|-----------|---------------|------------|
| PICO-8 streaming active | `pico8_stream_is_active()` | `OTA_BLOCKED_PICO8` |
| USB MSC connected (SD exported) | `animation_player_is_sd_export_locked()` | `OTA_BLOCKED_USB_MSC` |
| Already updating | `ota_manager_get_state() != IDLE` | `OTA_ALREADY_IN_PROGRESS` |
| No Wi-Fi connection | Check via `esp_netif` | `OTA_NO_WIFI` |

```c
esp_err_t ota_check_preconditions(void) {
    #if CONFIG_P3A_PICO8_ENABLE
    if (pico8_stream_is_active()) {
        ESP_LOGW(TAG, "OTA blocked: PICO-8 streaming active");
        return ESP_ERR_INVALID_STATE;  // OTA_BLOCKED_PICO8
    }
    #endif
    
    // Check if SD card is exported via USB MSC
    if (animation_player_is_sd_export_locked()) {
        ESP_LOGW(TAG, "OTA blocked: USB MSC active");
        return ESP_ERR_INVALID_STATE;  // OTA_BLOCKED_USB_MSC
    }
    
    ota_state_t current_state = ota_manager_get_state();
    if (current_state != OTA_STATE_IDLE && 
        current_state != OTA_STATE_UPDATE_AVAILABLE) {
        ESP_LOGW(TAG, "OTA blocked: already in progress (state=%d)", current_state);
        return ESP_ERR_INVALID_STATE;  // OTA_ALREADY_IN_PROGRESS
    }
    
    // Check Wi-Fi connectivity - try both default and remote STA interface keys
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_RMT");
    }
    if (!netif) {
        ESP_LOGW(TAG, "OTA blocked: no Wi-Fi interface");
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "OTA blocked: no IP address");
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    
    return ESP_OK;
}
```

### 12.2 Resource Cleanup Before OTA

```c
void ota_prepare_system(void) {
    ESP_LOGI(TAG, "Preparing system for OTA...");
    
    // 1. Stop animation playback AND free animation buffers
    //    animation_player_enter_ui_mode() automatically unloads both
    //    front_buffer and back_buffer to free memory for SSL/HTTP
    app_lcd_enter_ui_mode();
    
    // 2. Stop auto-swap task (automatic - task checks animation_player_is_ui_mode())
    
    // 3. Stop MQTT client to free resources
    makapix_mqtt_disconnect();
    
    // 4. Stop SNTP to free resources (need to add sntp_sync_stop())
    sntp_sync_stop();
    
    // Note: animation buffers already freed by app_lcd_enter_ui_mode()
    // which calls animation_player_enter_ui_mode() -> unload_animation_buffer()
    
    ESP_LOGI(TAG, "System ready for OTA, free heap: %lu", esp_get_free_heap_size());
}
```

---

## 13. Security Considerations

### 13.1 HTTPS Enforcement

All GitHub API and download requests use HTTPS:
- Certificate bundle includes GitHub's CA certificates
- No fallback to HTTP
- Certificate validation enabled by default

```c
esp_http_client_config_t config = {
    .url = "https://api.github.com/...",
    .crt_bundle_attach = esp_crt_bundle_attach,  // Use bundle
    .transport_type = HTTP_TRANSPORT_OVER_SSL,
    // No skip_cert_common_name_check
    // No use_global_ca_store (we use bundle)
};
```

### 13.2 SHA256 Integrity Verification

1. Download `p3a.bin.sha256` from GitHub Release
2. After writing firmware to flash, read it back
3. Compute SHA256 of written data
4. Compare with expected hash
5. If mismatch: abort and don't set boot partition

### 13.3 Version Downgrade Prevention (Future)

Currently not implemented (per requirements). For future consideration:
- `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y`
- Security version in eFuse

---

## 14. Component Design

### 14.1 New Component: `ota_manager`

**Location**: `components/ota_manager/`

**Files**:
```
components/ota_manager/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── ota_manager.h
├── ota_manager.c
├── github_ota.c
└── github_ota.h
```

**Public API** (`ota_manager.h`):

```c
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_CHECKING,
    OTA_STATE_UPDATE_AVAILABLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_FLASHING,
    OTA_STATE_PENDING_REBOOT,
    OTA_STATE_ERROR,
} ota_state_t;

typedef struct {
    ota_state_t state;
    char current_version[32];
    char available_version[32];
    uint32_t available_size;
    char release_notes[512];
    int64_t last_check_time;       // Unix timestamp
    int download_progress;          // 0-100
    char error_message[128];
    bool can_rollback;
    char rollback_version[32];
} ota_status_t;

/**
 * Initialize OTA manager
 * Starts periodic check timer (2 hours)
 */
esp_err_t ota_manager_init(void);

/**
 * Get current OTA status
 */
esp_err_t ota_manager_get_status(ota_status_t *status);

/**
 * Trigger immediate update check
 * Non-blocking, check runs in background task
 */
esp_err_t ota_manager_check_for_update(void);

/**
 * Start firmware installation
 * Blocks until complete or error
 * Device will reboot on success
 */
esp_err_t ota_manager_install_update(void);

/**
 * Schedule rollback and reboot
 */
esp_err_t ota_manager_rollback(void);

/**
 * Validate current firmware (call early in app_main)
 * Marks app valid if self-tests pass
 */
esp_err_t ota_manager_validate_boot(void);
```

### 14.2 Kconfig Options

```kconfig
menu "OTA Manager Configuration"

config OTA_CHECK_INTERVAL_HOURS
    int "Update check interval (hours)"
    default 2
    range 1 24
    help
        How often to automatically check for firmware updates.

config OTA_GITHUB_REPO
    string "GitHub repository (owner/repo)"
    default "fabkury/p3a"
    help
        GitHub repository for firmware releases.

config OTA_FIRMWARE_ASSET_NAME
    string "Firmware asset filename"
    default "p3a.bin"
    help
        Name of the firmware binary in GitHub releases.

config OTA_DOWNLOAD_TIMEOUT_SEC
    int "Download timeout (seconds)"
    default 600
    range 60 1800
    help
        Maximum time for firmware download.

endmenu
```

---

## 15. Implementation Phases

### Phase 1: Planning ✅
- [x] Requirements gathering
- [x] Architecture design
- [x] Partition table design
- [x] API design
- [x] Create this document

### Phase 2: Review & Refinement ✅
- [x] Review codebase for potential conflicts
- [x] Validate partition sizes with current binary
- [x] Review ESP-IDF OTA examples
- [x] Finalize pseudocode

#### Phase 2 Findings (December 6, 2025)

**Firmware Size Analysis:**
- Current binary: **1.54 MB** (1,613,600 bytes)
- Proposed OTA partition: **8 MB** (8,388,608 bytes)
- Utilization: **19.2%** - ample headroom for future growth

**Codebase Compatibility:**

| Component | Status | Notes |
|-----------|--------|-------|
| Certificate Bundle | ✅ Ready | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` already enabled |
| PICO-8 Detection | ✅ Ready | `pico8_stream_is_active()` exists in `pico8_stream.h` |
| USB MSC Detection | ✅ Ready | Use `animation_player_is_sd_export_locked()` for MSC check |
| Animation Buffer Cleanup | ✅ Ready | `animation_player_enter_ui_mode()` already unloads buffers |
| MQTT Disconnect | ✅ Ready | `makapix_mqtt_disconnect()` in `makapix_mqtt.h` |
| SNTP Stop | ⚠️ Add | Need to add `sntp_sync_stop()` wrapper |
| µGFX UI | ✅ Ready | Add OTA progress functions to `ugfx_ui.h` |
| HTTP API | ✅ Ready | Add endpoints to `http_api.c` following existing pattern |

**Plan Refinements:**

1. **USB MSC Check**: Use `animation_player_is_sd_export_locked()` instead of creating new `app_usb_is_msc_active()`. This function returns `true` when SD card is exported via USB.

2. **Buffer Cleanup**: Remove `animation_player_free_buffers()` from plan - `animation_player_enter_ui_mode()` already calls `unload_animation_buffer()` on both front and back buffers.

3. **SNTP Stop**: Add simple `sntp_sync_stop()` function that calls `esp_netif_sntp_deinit()`.

4. **Component Location**: Create `ota_manager` as a component under `components/` (not `main/`) for clean separation.

**No Blocking Issues Found** - Ready for Phase 3 implementation.

### Phase 3: Implementation
- [ ] **Step 1**: Update partition table (`partitions.csv`)
- [ ] **Step 2**: Enable OTA sdkconfig options
- [ ] **Step 3**: Create `ota_manager` component skeleton
- [ ] **Step 4**: Implement GitHub API client (`github_ota.c`)
- [ ] **Step 5**: Implement version comparison
- [ ] **Step 6**: Implement periodic check timer
- [ ] **Step 7**: Implement download with progress
- [ ] **Step 8**: Implement SHA256 verification
- [ ] **Step 9**: Implement rollback logic
- [ ] **Step 10**: Add boot validation in `app_main`
- [ ] **Step 11**: Add HTTP API endpoints
- [ ] **Step 12**: Add Web UI page
- [ ] **Step 13**: Add LCD progress display
- [ ] **Step 14**: Build and fix errors
- [ ] **Step 15**: Manual testing
- [ ] **Step 16**: Create first GitHub Release for testing

---

## 16. Testing Strategy

### 16.1 Unit Tests

| Test | Description |
|------|-------------|
| Version parsing | `"1.0.0"`, `"v2.1.3"`, `"1.0.0-beta"`, invalid strings |
| Version comparison | Verify ordering of version numbers |
| JSON parsing | Mock GitHub API responses |

### 16.2 Integration Tests

| Test | Description |
|------|-------------|
| API connectivity | Can reach `api.github.com` |
| Release fetch | Parse real release metadata |
| Download | Download small test file |
| Flash write | Write to OTA partition |
| Reboot cycle | Boot from new partition |

### 16.3 System Tests

| Test | Description |
|------|-------------|
| Full update cycle | Check → Download → Verify → Flash → Reboot → Validate |
| Rollback on failure | Simulate boot failure, verify rollback |
| Power loss recovery | Interrupt during download, verify clean recovery |
| Blocker enforcement | Try OTA during PICO-8/USB, verify rejection |
| UI feedback | Verify progress bar updates, final status |

### 16.4 First Release Checklist

Before publishing first OTA-enabled release:

1. [ ] Build firmware with OTA partition table
2. [ ] Flash device with new partition layout
3. [ ] Verify device boots and runs normally
4. [ ] Create test GitHub Release with `p3a.bin` and `p3a.bin.sha256`
5. [ ] Trigger update check
6. [ ] Verify update is detected
7. [ ] Install update
8. [ ] Verify successful reboot
9. [ ] Verify rollback works
10. [ ] Test from older version to confirm upgrade path

---

## 17. Progress Tracking

### Current Phase: ✅ Phase 3 - Implementation Complete

| Task | Status | Notes |
|------|--------|-------|
| Requirements | ✅ Complete | User requirements gathered |
| Architecture | ✅ Complete | Component design finalized |
| Partition design | ✅ Complete | Dual OTA layout defined |
| API design | ✅ Complete | REST endpoints defined |
| Pseudocode | ✅ Complete | Core algorithms documented |
| Codebase review | ✅ Complete | No blocking issues found |
| Binary size check | ✅ Complete | 1.54 MB fits in 8MB partition |
| Plan refinements | ✅ Complete | Updated API function names |
| **Implementation** | ✅ Complete | All code written |
| **Build verification** | ✅ Complete | Clean build successful |

### Phase 3 Implementation Summary

**Files Created:**
- `components/ota_manager/CMakeLists.txt` - Component build configuration
- `components/ota_manager/Kconfig` - OTA configuration options
- `components/ota_manager/include/ota_manager.h` - Public API header
- `components/ota_manager/ota_manager.c` - State machine and OTA logic
- `components/ota_manager/github_ota.h` - GitHub API client header
- `components/ota_manager/github_ota.c` - GitHub API implementation

**Files Modified:**
- `partitions.csv` - Dual OTA partition layout (ota_0, ota_1, otadata)
- `def/sdkconfig.defaults` - Added OTA rollback configuration
- `main/include/version.h` - Enhanced version format
- `main/include/sntp_sync.h` - Added `sntp_sync_stop()` declaration
- `main/sntp_sync.c` - Added `sntp_sync_stop()` implementation
- `main/include/ugfx_ui.h` - Added OTA progress display functions
- `main/ugfx_ui.c` - Added OTA progress UI mode and rendering
- `main/p3a_main.c` - Added OTA boot validation and initialization
- `main/CMakeLists.txt` - Added ota_manager dependency
- `components/http_api/CMakeLists.txt` - Added ota_manager dependency
- `components/http_api/http_api.c` - Added OTA endpoints and Web UI

**Key Features Implemented:**
- ✅ Periodic update check (configurable, default 2 hours)
- ✅ Manual update check via `/ota/check` API
- ✅ Full Web UI at `/ota` for managing updates
- ✅ OTA state machine with proper transitions
- ✅ GitHub Releases API integration
- ✅ SHA256 verification of downloaded firmware
- ✅ LCD progress display during updates
- ✅ Automatic rollback on boot failure (via ESP-IDF bootloader)
- ✅ Manual rollback via Web UI
- ✅ Safety checks (blocks during PICO-8/USB MSC modes)
- ✅ UI mode handling (stops animations during OTA)

**Build Output:**
- Binary size: 1.66 MB (fits in 8MB partition)
- 80% free space remaining in OTA partition

### Development Mode Testing Workflow

The OTA implementation includes a **development mode** (`CONFIG_OTA_DEV_MODE`) that allows testing with pre-release versions:

**How it works:**
- Production devices (dev_mode=OFF): Only see and install **regular releases**
- Development devices (dev_mode=ON): Also see and install **pre-releases**

**Testing Workflow:**

1. **Build dev firmware** (with `CONFIG_OTA_DEV_MODE=y` in sdkconfig.defaults)
2. **Flash to test device** with new partition table
3. **Create a pre-release** on GitHub:
   - Tag: `v1.0.1-test1` (or similar)
   - Mark as "Pre-release" ✓
   - Attach `p3a.bin` and `p3a.bin.sha256`
4. **Test the OTA flow** on your dev device
5. **Iterate**: Create more pre-releases as needed (`v1.0.1-test2`, etc.)
6. **When ready for production**:
   - Set `CONFIG_OTA_DEV_MODE=n`
   - Rebuild
   - Create a **regular release** (not pre-release)
   - All devices will see and can install it

**Creating SHA256 checksum file:**
```bash
# On Linux/Mac:
sha256sum p3a.bin | cut -d' ' -f1 > p3a.bin.sha256

# On Windows PowerShell:
(Get-FileHash p3a.bin -Algorithm SHA256).Hash.ToLower() | Out-File -Encoding ASCII p3a.bin.sha256
```

### Next Steps

1. **Testing Phase**: 
   - Flash the new firmware to device (requires full reflash for new partitions)
   - Create pre-release on GitHub with `p3a.bin` and `p3a.bin.sha256`
   - Navigate to `http://p3a.local/ota` (will show "DEV MODE" badge)
   - Test full OTA flow: Check → Install → Reboot → Validate
   - Test rollback functionality
   - Test safety blocks (PICO-8 streaming, USB MSC)

2. **Production Deployment**:
   - Set `CONFIG_OTA_DEV_MODE=n` in sdkconfig.defaults
   - Rebuild firmware
   - Create regular release on GitHub
   - All devices will receive the update

---

## Appendix A: References

- [ESP-IDF OTA Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html)
- [ESP-IDF App Rollback](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/app_rollback.html)
- [GitHub Releases API](https://docs.github.com/en/rest/releases/releases)
- [Semantic Versioning 2.0](https://semver.org/)

---

## Appendix B: Glossary

| Term | Definition |
|------|------------|
| OTA | Over-The-Air update |
| otadata | Partition storing boot slot selection |
| Rollback | Reverting to previous firmware version |
| Slot | One of the two OTA app partitions (ota_0, ota_1) |

---

*Document Version: 1.2*  
*Created: December 6, 2025*  
*Phase 2 Review Completed: December 6, 2025*  
*Phase 3 Implementation Completed: December 6, 2025*  
*Author: Claude (AI Assistant) with Fab Kury*

