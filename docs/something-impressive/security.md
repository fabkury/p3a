# Security Audit

Vulnerabilities found across HTTP API, WebSocket, OTA, WiFi provisioning, and
MQTT components.

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

### 12. Generic CA Bundle for OTA

**Files:** `components/ota_manager/github_ota.c:248`,
`ota_manager_install.c:207`

OTA uses the generic ESP-IDF CA bundle (`esp_crt_bundle_attach`) instead of
pinning GitHub's certificate. Vulnerable to a compromised CA scenario.

**Recommendation:** Consider pinning GitHub's CA certificate for OTA paths.

---

## Low / Informational


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
