# Security Audit

Vulnerabilities found across HTTP API, WebSocket, OTA, WiFi provisioning, and
MQTT components.

---

## Critical

### 1. No Authentication on HTTP API

**File:** `components/http_api/http_api.c:424-603`

The entire HTTP API has zero authentication — no password, no API key, no token.
Any device on the same network can:

- Reboot the device (`POST /action/reboot`)
- Erase WiFi credentials (`POST /erase`)
- Upload arbitrary files (`POST /upload`)
- Modify all settings (`PUT /config`, `PUT /settings/*`)
- Trigger OTA updates (`POST /ota/install`)
- Control playback

**Recommendation:** Implement API token authentication or HTTP Basic Auth with a
password stored in NVS.

---

### 2. No WebSocket Authentication

**File:** `components/http_api/http_api_pico8.c:78-220`

The WebSocket endpoint `/pico_stream` accepts connections without any
authentication:

```c
esp_err_t h_ws_pico_stream(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(HTTP_API_TAG, "WebSocket connection request");
        playback_controller_enter_pico8_mode();
        s_ws_client_connected = true;
        return ESP_OK;  // NO AUTHENTICATION CHECK
    }
```

**Impact:** Unauthenticated attackers can stream arbitrary PICO-8 commands and
trigger mode changes.

**Recommendation:** Add WebSocket authentication via initial handshake token.

---

## High

### 3. Path Traversal in Static File Serving

**File:** `components/http_api/http_api_pages.c:224-241`

The static file handler constructs paths using user-controlled URI without
sanitization:

```c
static esp_err_t h_get_static(httpd_req_t *req) {
    const char* uri = req->uri;
    char filepath[MAX_FILE_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", prefix, uri);
    return serve_file(req, filepath);
}
```

**Attack vector:**
```
GET /static/../../../sdcard/p3a/vault/<hash> HTTP/1.1
```

Could access vault files or other filesystem areas.

**Recommendation:** Validate that the resolved path stays within `/webui/` using
`realpath()` or explicit `..` detection.

---

### 4. Filename Injection in File Upload

**File:** `components/http_api/http_api_upload.c:236-243, 400`

Uploaded filename from multipart form data is used directly in file operations
without sanitization:

```c
char *fn_start = strstr(cd, "filename=\"");
fn_start += 10;
char *fn_end = strchr(fn_start, '"');
size_t fn_len = fn_end - fn_start;
memcpy(filename, fn_start, fn_len);  // No validation
```

Later used as:
```c
snprintf(final_path, sizeof(final_path), "%s/%s", ANIMATIONS_DIR, filename);
```

**Attack vectors:**
- Filename `../../system_file.txt` writes outside animations directory
- Filename `../vault/malicious.webp` overwrites vault files

**Recommendation:** Reject filenames containing `/`, `\`, `..`. Whitelist
allowed characters (alphanumeric, `.`, `-`, `_`).

---

### 5. No OTA Downgrade Protection

**File:** `components/ota_manager/ota_manager_install.c:86-336`

The OTA update process validates SHA256 integrity but does NOT prevent downgrade
attacks. Version comparison happens at check time but is not enforced during
install. An attacker serving a MitM GitHub API response could force installation
of an older, vulnerable firmware version.

**Recommendation:**
1. Store current version in NVS.
2. Reject firmware with version <= current version.
3. Add a rollback counter to prevent replay attacks.

---

### 6. WiFi Provisioning Credential Injection

**File:** `components/wifi_manager/app_wifi.c:998-1029`

The captive portal form parser uses unsafe string operations:

```c
char *ssid_start = strstr(content, "ssid=");
ssid_start += 5;
char *ssid_end = strchr(ssid_start, '&');
int len = ssid_end - ssid_start;
strncpy(ssid, ssid_start, len);
```

If `strchr` returns NULL (no `&` found), the code copies unbounded data.

**Recommendation:** Always validate parsed length against `MAX_SSID_LEN` before
copy. Handle NULL `ssid_end` explicitly.

---

## Medium

### 7. No MQTT Message Size Enforcement

**File:** `components/makapix/makapix_mqtt.c:146-200`

MQTT reassembly buffer is 128 KB. `total_data_len` is checked AFTER allocation
attempt. Oversized messages are dropped but fragments continue to be processed,
wasting CPU.

**Recommendation:** Close MQTT connection on oversized message. Add rate
limiting.

---

### 8. Integer Overflow in Image Dimension Calculations

**File:** `main/animation_player_loader.c:621, 628`

Upscale lookup table allocation multiplies user-controlled dimensions without
overflow check. A malicious image with extreme dimensions could cause undersized
allocation.

**Recommendation:** Add explicit overflow checks before `malloc`:
```c
if (max_len > SIZE_MAX / sizeof(uint16_t)) return ESP_ERR_NO_MEM;
```

---

### 9. Missing WebSocket Origin Validation

**File:** `components/http_api/http_api_pico8.c`

WebSocket accepts connections without verifying the Origin header. Enables CSRF
attacks from malicious websites if the device is on the local network.

**Recommendation:** Validate Origin header matches expected domain or localhost.

---

### 10. MQTT Command URL Not Validated

**File:** `components/http_api/http_api.c:289-298`

MQTT commands like `play_channel` and `swap_to` accept JSON with URL fields that
are not validated for scheme or host:

```c
const char *url = cJSON_GetStringValue(art_url);
makapix_show_artwork(pid, key, url);  // url not validated
```

**Recommendation:** Whitelist allowed URL schemes (`https://` only) and validate
hosts.

---

### 11. No Rate Limiting on HTTP Endpoints

**Files:** All HTTP handlers

No rate limiting on any endpoint. Expensive operations like `/upload`,
`/ota/check`, `/action/swap_next` are vulnerable to denial-of-service via rapid
requests.

**Recommendation:** Implement per-client rate limiting using IP-based token
bucket.

---

### 12. Generic CA Bundle for OTA

**Files:** `components/ota_manager/github_ota.c:248`,
`ota_manager_install.c:207`

OTA uses the generic ESP-IDF CA bundle (`esp_crt_bundle_attach`) instead of
pinning GitHub's certificate. Vulnerable to a compromised CA scenario.

**Recommendation:** Consider pinning GitHub's CA certificate for OTA paths.

---

## Low / Informational

### 13. Giphy API Key Stored in Plain NVS

**File:** `components/config_store/config_store.c:1249`

User's Giphy API key stored in NVS without encryption. Extractable if device is
physically compromised.

### 14. Verbose Error Messages

Multiple endpoints return detailed internal error messages (e.g., "Failed to
allocate decoder") which could aid attackers in understanding device internals.

### 15. Stack Buffers Without Canaries

WebSocket handler uses stack buffers (`uint8_t stack_buf[512]` at
`http_api_pico8.c:86`). Ensure compiler stack protection is enabled.

---

## Positive Practices Observed

- Uses TLS for OTA, MQTT, and HTTPS (with `esp_crt_bundle`)
- SHA256 verification for firmware downloads
- Bounded buffer sizes (`MAX_JSON=32KB`, `WS_MAX_FRAME_SIZE`)
- No hardcoded secrets found (Giphy API key is user-configured)
- Uses `snprintf` instead of `sprintf` throughout
- No use of `system()` / `exec()` / `popen()`
