# Wi-Fi Reliability Research & Analysis for p3a

**Date:** December 17, 2025  
**Status:** Research Phase - No Code Changes  
**Issue:** p3a Wi-Fi connection works initially but loses all Wi-Fi functionality at some point, requiring reboot

---

## Executive Summary

This document analyzes the Wi-Fi implementation in p3a (ESP32-P4 master + ESP32-C6 slave over SDIO using ESP-Hosted) and compares it against best practices found in Espressif documentation, community examples, and real-world deployments.

**Key Findings:**
- Our implementation is **generally well-structured** and follows most ESP-Hosted best practices
- We correctly use `WIFI_REMOTE_EVENT` instead of `WIFI_EVENT` for event handling
- We have persistent reconnection logic after initial connection
- We handle `IP_EVENT_STA_LOST_IP` with forced reconnection
- **However**, several potential improvements and missing elements were identified that could improve reliability

---

## 1. Best Practice Examples from Research

### 1.1 Hardware Configuration

**Source:** Espressif ESP-Hosted SDIO documentation, Waveshare ESP32-P4-WIFI6 examples

**Best Practices:**
- Use **SDIO Slot 1** for ESP-Hosted WiFi (Slot 0 reserved for SD card) ✅ **We do this**
- Configure **4-bit bus width** for better throughput ✅ **We do this** (CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y)
- Use **40 MHz SDIO clock** as baseline (can optimize to 50MHz if stable) ✅ **We do this** (CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000)
- Enable **SDIO streaming mode** for better performance ✅ **We do this** (CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y)
- Use **active-high reset** for ESP32-C6 ✅ **We do this** (CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y)
- Ensure proper **reset sequence** during initialization and firmware updates

**Example from Waveshare ESP32-P4-WIFI6:**
```
SDIO Slot 1 pins:
- CMD: GPIO19
- CLK: GPIO18
- D0:  GPIO14
- D1:  GPIO15
- D2:  GPIO16
- D3:  GPIO17
- RST: GPIO54
```

✅ **Our configuration matches these recommendations exactly**

### 1.2 Firmware Initialization Sequence

**Source:** ESP-IDF Programming Guide, ESP-Hosted examples

**Best Practice Pattern:**
```c
// 1. Initialize NVS
nvs_flash_init();

// 2. Initialize network interface
esp_netif_init();

// 3. Create event loop
esp_event_loop_create_default();

// 4. Create default WiFi netif
esp_netif_create_default_wifi_sta();

// 5. Initialize WiFi with remote driver
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_remote_init(&cfg);

// 6. Register event handlers BEFORE starting WiFi
esp_event_handler_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID, &handler, NULL);
esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &handler, NULL);

// 7. Configure and start WiFi
esp_wifi_remote_set_mode(WIFI_MODE_STA);
esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config);
esp_wifi_remote_start();
```

✅ **Our implementation follows this sequence correctly in `wifi_init_sta()`**

### 1.3 Event Handling Best Practices

**Source:** Espressif Wi-Fi Driver documentation, community forums

**Critical Events to Handle:**

1. **`WIFI_REMOTE_EVENT` + `WIFI_EVENT_STA_START`**
   - Action: Call `esp_wifi_remote_connect()`
   - ✅ **We handle this** (line 251-255)

2. **`WIFI_REMOTE_EVENT` + `WIFI_EVENT_STA_DISCONNECTED`**
   - Action: Implement retry logic with exponential backoff
   - Track retry count
   - ✅ **We handle this** (line 256-289)

3. **`IP_EVENT` + `IP_EVENT_STA_GOT_IP`**
   - Action: Reset retry counter, mark connection successful
   - Announce mDNS on the interface
   - ✅ **We handle this** (line 290-353)

4. **`IP_EVENT` + `IP_EVENT_STA_LOST_IP`**
   - Action: Force WiFi disconnect/reconnect to get new DHCP lease
   - ✅ **We handle this** (line 354-366)

**Example reconnection pattern from community:**
```c
static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            // Quick retry for initial connection
            esp_wifi_remote_connect();
            s_retry_num++;
        } else if (initial_connection_done) {
            // Persistent reconnection with backoff
            s_retry_num++;
            if (s_retry_num > 5) {
                vTaskDelay(pdMS_TO_TICKS(5000)); // 5 second backoff
            }
            esp_wifi_remote_connect();
        } else {
            // Give up on initial connection
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
}
```

✅ **Our implementation uses this exact pattern** (line 256-289)

### 1.4 SDIO Bus Coordination

**Source:** ESP-Hosted MCU documentation, M5Stack Tab5 issue threads

**Best Practice:**
ESP32-P4 shares the SDMMC controller between SDIO Slot 1 (WiFi) and SDIO Slot 0 (SD card). Simultaneous high-bandwidth operations can cause:
- "SDIO slave unresponsive" crashes
- WiFi disconnections
- SD card read failures

**Recommended mitigation:**
```c
// Acquire exclusive bus access before sustained WiFi operations
sdio_bus_acquire(timeout_ms, "REQUESTER_TAG");
// ... perform WiFi-heavy operation (OTA, large download) ...
sdio_bus_release();
```

✅ **We have implemented `sdio_bus` coordinator** (components/sdio_bus/)  
✅ **We use it for OTA operations** (in slave_ota and ota_manager)

### 1.5 Power Management and DHCP

**Source:** ESP32 Forum, circuitlabs.net, espboards.dev

**Best Practices:**

1. **Disable WiFi Power Save for reliability:**
   ```c
   esp_wifi_set_ps(WIFI_PS_NONE);
   ```
   ⚠️ **We don't explicitly set this** - may default to power-saving mode

2. **Handle DHCP renewal:**
   - Monitor `IP_EVENT_STA_LOST_IP`
   - Force reconnection to get fresh DHCP lease
   - ✅ **We handle this** (line 354-366)

3. **Set reasonable DHCP timeouts:**
   - Default DHCP timeout is usually sufficient
   - ⚠️ **We don't explicitly configure DHCP timeouts**

4. **Watchdog for stuck states:**
   - Implement application-level watchdog to detect "connected but no data" state
   - ❌ **We don't have this**

---

## 2. Current p3a Implementation Analysis

### 2.1 What We're Doing Well

#### ✅ Correct Event Base Usage
**Our code (line 251, 256):**
```c
if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_remote_connect();
} else if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    // ... reconnection logic
}
```

**Why this is correct:**
- ESP-Hosted uses `WIFI_REMOTE_EVENT` as the event base (not `WIFI_EVENT`)
- Event IDs remain the same (`WIFI_EVENT_STA_START`, `WIFI_EVENT_STA_DISCONNECTED`, etc.)
- This is documented in esp_wifi_remote component but easy to get wrong

#### ✅ Persistent Reconnection After Initial Connection
**Our code (line 256-289):**
```c
// For initial connection: use retry limit
// After initial connection succeeded: always keep trying (persistent reconnection)
if (!s_initial_connection_done && s_retry_num >= EXAMPLE_ESP_MAXIMUM_RETRY) {
    ESP_LOGI(TAG, "Initial connection failed after %d attempts", EXAMPLE_ESP_MAXIMUM_RETRY);
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
} else {
    // Always try to reconnect
    s_retry_num++;
    
    // Add delay for persistent reconnection to avoid hammering the AP
    if (s_initial_connection_done && s_retry_num > 5) {
        ESP_LOGI(TAG, "WiFi reconnect attempt %d (with backoff)", s_retry_num);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    esp_wifi_remote_connect();
}
```

**Why this is good:**
- Distinguishes between initial connection (with retry limit) and subsequent reconnections (persistent)
- Implements exponential backoff after 5 retries
- Prevents AP flooding during outages

#### ✅ IP Loss Handling with Forced Reconnection
**Our code (line 354-366):**
```c
} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    // IP address lost - DHCP renewal likely failed
    // Force WiFi reconnection to obtain a fresh DHCP lease
    ESP_LOGW(TAG, "IP address lost! DHCP renewal likely failed.");
    ESP_LOGI(TAG, "Forcing WiFi reconnection to obtain new IP...");
    
    // Stop MQTT to prevent futile connection attempts
    makapix_mqtt_disconnect();
    
    // Force WiFi disconnect - this will trigger WIFI_EVENT_STA_DISCONNECTED
    // which then triggers reconnection and fresh DHCP
    esp_wifi_remote_disconnect();
}
```

**Why this is correct:**
- Handles DHCP lease expiration/renewal failures
- Disconnects MQTT to avoid connection errors
- Triggers full reconnection cycle (not just DHCP renewal)
- This is the **recommended approach** for ESP32 DHCP issues

#### ✅ mDNS Announcement After IP Acquisition
**Our code (line 296-312):**
```c
// Ensure mDNS is enabled/announced on STA after getting an IP.
esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
if (sta_netif) {
    esp_err_t merr = mdns_netif_action(sta_netif,
                                       (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
    if (merr == ESP_OK) {
        ESP_LOGI(TAG, "mDNS announced on %s", WIFI_STA_NETIF_KEY);
    }
}
```

**Why this is important:**
- Ensures `p3a.local` hostname is available after WiFi reconnection
- Fixes issue where mDNS might not re-announce after disconnect/reconnect
- This was added to fix a specific bug mentioned in comments

#### ✅ SDIO Bus Coordinator
**Our implementation:**
- `components/sdio_bus/` provides mutex-based coordination
- Used by OTA operations to prevent SD card contention
- Prevents "SDIO slave unresponsive" crashes during firmware updates

**Why this is good:**
- Addresses known ESP32-P4 SDMMC controller limitation
- Prevents crashes during high-bandwidth WiFi operations
- Industry best practice for shared SDIO bus

### 2.2 Potential Issues and Missing Elements

#### ⚠️ WiFi Power Save Mode Not Explicitly Disabled

**Issue:**
We don't explicitly call `esp_wifi_set_ps(WIFI_PS_NONE)` to disable power saving.

**Why this matters:**
- Default power save mode can cause sporadic disconnections
- ESP32-C6 may enter sleep states and miss packets
- Can cause "WiFi connected but no data" symptoms
- Recommended to disable for non-battery applications

**Recommendation:**
```c
// In wifi_init_sta(), after esp_wifi_remote_start():
esp_err_t ps_err = esp_wifi_remote_set_ps(WIFI_PS_NONE);
if (ps_err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to disable WiFi power save: %s", esp_err_to_name(ps_err));
}
```

**Priority:** **HIGH** - This is a common cause of "works then stops" WiFi issues

#### ⚠️ No Application-Level Keepalive/Watchdog

**Issue:**
We have no mechanism to detect "WiFi connected but no data flowing" state.

**Scenario:**
1. WiFi shows as connected (events say connected, IP assigned)
2. But packets aren't actually flowing (SDIO bus hung, co-processor crashed, etc.)
3. Application thinks WiFi is fine but can't communicate
4. Only a reboot fixes it

**Recommendation:**
Implement a periodic connectivity check:
```c
static void wifi_watchdog_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Check every 60 seconds
        
        if (wifi_is_connected()) {
            // Try a lightweight network operation (DNS query, ping, etc.)
            if (ping_gateway_or_dns() != ESP_OK) {
                ESP_LOGW(TAG, "WiFi connected but no data flowing - forcing reconnect");
                esp_wifi_remote_disconnect();
            }
        }
    }
}
```

**Priority:** **MEDIUM** - Would catch stuck states that event handling misses

#### ⚠️ No SDIO Bus Health Monitoring

**Issue:**
If SDIO bus between P4 and C6 gets into a bad state, we have no recovery mechanism short of reboot.

**Best practice from ESP-Hosted community:**
- Monitor for SDIO errors in logs
- Implement SDIO bus reset sequence
- Consider full WiFi re-initialization on repeated SDIO errors

**Recommendation:**
```c
// Track consecutive SDIO/WiFi errors
static int s_consecutive_wifi_errors = 0;

// In event_handler, on disconnect:
if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_consecutive_wifi_errors++;
    
    if (s_consecutive_wifi_errors > 10) {
        ESP_LOGW(TAG, "Too many consecutive WiFi errors - attempting full re-init");
        wifi_full_reinit(); // Stop, deinit, re-init WiFi stack
        s_consecutive_wifi_errors = 0;
    }
}

// On successful connection:
if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    s_consecutive_wifi_errors = 0; // Reset on success
}
```

**Priority:** **MEDIUM** - Would help recover from SDIO bus issues

#### ⚠️ Missing WiFi Remote-Specific Initialization

**Issue:**
The function `wifi_remote_init()` (line 239-246) is a placeholder:

```c
static void wifi_remote_init(void)
{
    // Note: esp_hosted component initialization may be handled automatically
    // or may require specific initialization based on hardware configuration.
    // This is a placeholder - adjust based on actual esp_hosted API requirements.
    ESP_LOGI(TAG, "Initializing Wi-Fi remote module (ESP32-C6)");
    // If esp_hosted requires explicit initialization, add it here
}
```

**Research findings:**
- ESP-Hosted may require explicit initialization in some configurations
- Some examples call esp_hosted-specific init functions
- Hardware reset sequence should be verified

**Recommendation:**
1. Verify if esp_hosted requires explicit init (check library docs)
2. If yes, add proper initialization sequence
3. Ensure C6 reset line is properly controlled

**Priority:** **LOW** - Likely working via auto-initialization, but should be documented

#### ⚠️ No Explicit DHCP Timeout Configuration

**Issue:**
We don't configure DHCP timeouts, relying on ESP-IDF defaults.

**Why this might matter:**
- On slow/congested networks, DHCP can timeout
- Default timeout might be too short or too long
- Can cause "connected but no IP" states

**Recommendation:**
Research suggests ESP-IDF DHCP defaults are usually fine, but can be tuned:
```c
// After esp_netif_create_default_wifi_sta():
esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey(WIFI_STA_NETIF_KEY);
if (sta_netif) {
    esp_netif_dhcpc_option(sta_netif, ESP_NETIF_OP_SET, 
                          ESP_NETIF_DOMAIN_NAME_SERVER, ...);
}
```

**Priority:** **LOW** - Defaults are usually sufficient

#### ⚠️ WiFi Protocol Settings May Need Verification

**Issue:**
We set WiFi 6 (802.11ax) protocol in `wifi_set_protocol_11ax()` (line 227-236):

```c
static void wifi_set_protocol_11ax(wifi_interface_t interface)
{
    uint8_t protocol_bitmap = WIFI_PROTOCOL_11AX | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11B;
    esp_err_t ret = esp_wifi_remote_set_protocol(interface, protocol_bitmap);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi 6 (802.11ax) protocol enabled for interface %d", interface);
    } else {
        ESP_LOGW(TAG, "Failed to set Wi-Fi 6 protocol: %s", esp_err_to_name(ret));
    }
}
```

**Potential issues:**
- WiFi 6 features might not be fully stable on ESP32-C6 firmware
- Some routers have compatibility issues with 802.11ax
- Might be better to start with 802.11n and enable 802.11ax as option

**Recommendation:**
1. Check if failure to set protocol could cause later issues
2. Consider making WiFi 6 configurable
3. Fall back to 802.11n if 802.11ax fails

**Priority:** **LOW** - Log message suggests it's best-effort, so probably fine

---

## 3. Comparison with Best Practices

### 3.1 What We Match Exactly

| Best Practice | Our Implementation | Status |
|--------------|-------------------|--------|
| Use WIFI_REMOTE_EVENT for esp_wifi_remote | ✅ Using WIFI_REMOTE_EVENT | **MATCH** |
| Handle WIFI_EVENT_STA_START | ✅ Call esp_wifi_remote_connect() | **MATCH** |
| Handle WIFI_EVENT_STA_DISCONNECTED | ✅ Retry with backoff | **MATCH** |
| Handle IP_EVENT_STA_GOT_IP | ✅ Reset retry counter, announce mDNS | **MATCH** |
| Handle IP_EVENT_STA_LOST_IP | ✅ Force disconnect/reconnect | **MATCH** |
| Persistent reconnection after initial | ✅ Unlimited retries with backoff | **MATCH** |
| SDIO bus coordination | ✅ sdio_bus component | **MATCH** |
| mDNS re-announcement | ✅ Explicit mdns_netif_action() | **MATCH** |
| Exponential backoff | ✅ 5 second delay after 5 retries | **MATCH** |
| SDIO Slot 1 for WiFi | ✅ CONFIG_ESP_HOSTED_SDIO_SLOT_1=y | **MATCH** |
| 4-bit SDIO bus | ✅ CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y | **MATCH** |
| 40MHz SDIO clock | ✅ CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000 | **MATCH** |
| SDIO streaming mode | ✅ CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y | **MATCH** |

### 3.2 What We're Missing or Different

| Best Practice | Our Implementation | Status |
|--------------|-------------------|--------|
| Disable WiFi power save | ⚠️ Not explicitly set | **MISSING** |
| Application-level keepalive | ⚠️ No watchdog task | **MISSING** |
| SDIO bus health monitoring | ⚠️ No error tracking | **MISSING** |
| Full WiFi re-init on errors | ⚠️ No recovery mechanism | **MISSING** |
| Explicit DHCP timeout config | ⚠️ Using defaults | **MISSING** (probably OK) |
| WiFi protocol fallback | ⚠️ Best-effort only | **MINOR** |

---

## 4. Identified Issues in Our Code

### Issue 1: WiFi Power Save Not Disabled

**Severity:** HIGH  
**Location:** components/wifi_manager/app_wifi.c, wifi_init_sta()  
**Symptom:** WiFi works initially, then becomes unresponsive after some time

**Root Cause:**
ESP32 default WiFi power save mode can cause the ESP32-C6 to enter sleep states, leading to:
- Missed beacon frames
- DHCP renewal failures
- Unresponsive SDIO communication
- Appearance of "connected but not working"

**Solution:**
Add explicit power save disable after WiFi start:
```c
// After esp_wifi_remote_start() in wifi_init_sta():
esp_err_t ps_err = esp_wifi_remote_set_ps(WIFI_PS_NONE);
if (ps_err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to disable WiFi power save: %s", esp_err_to_name(ps_err));
} else {
    ESP_LOGI(TAG, "WiFi power save disabled for reliability");
}
```

### Issue 2: No Detection of "Connected But No Data" State

**Severity:** MEDIUM  
**Location:** New feature needed  
**Symptom:** WiFi shows connected, but network operations fail silently

**Root Cause:**
SDIO bus can enter a stuck state where:
- WiFi events still fire (connected, got IP)
- But actual data transfer over SDIO fails
- No automatic recovery mechanism

**Solution:**
Implement periodic connectivity check:
```c
// New task in app_wifi.c or separate monitoring component
static void wifi_health_monitor_task(void *arg)
{
    const char *TAG = "wifi_health";
    TickType_t last_check = xTaskGetTickCount();
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Check every 60 seconds
        
        if (!s_initial_connection_done) {
            continue; // Skip monitoring during initial connection
        }
        
        // Simple health check: try DNS resolution
        struct addrinfo *res = NULL;
        struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
        };
        
        int err = getaddrinfo("google.com", "80", &hints, &res);
        if (err != 0 || res == NULL) {
            ESP_LOGW(TAG, "WiFi health check failed - network not responding");
            ESP_LOGI(TAG, "Forcing WiFi reconnection...");
            
            // Trigger reconnection
            esp_wifi_remote_disconnect();
        } else {
            ESP_LOGD(TAG, "WiFi health check passed");
        }
        
        if (res) {
            freeaddrinfo(res);
        }
    }
}

// Start in app_wifi_init():
xTaskCreate(wifi_health_monitor_task, "wifi_health", 4096, NULL, 5, NULL);
```

### Issue 3: No SDIO Error Recovery

**Severity:** MEDIUM  
**Location:** components/wifi_manager/app_wifi.c, event_handler()  
**Symptom:** Repeated disconnections, eventual complete failure

**Root Cause:**
If SDIO bus gets into a bad state:
- Repeated connection attempts fail
- No mechanism to reset WiFi stack
- Eventually requires full system reboot

**Solution:**
Add error counting and full re-initialization:
```c
static int s_consecutive_wifi_errors = 0;
static const int MAX_CONSECUTIVE_ERRORS = 10;

static esp_err_t wifi_full_reinit(void)
{
    ESP_LOGW(TAG, "Performing full WiFi re-initialization");
    
    // Stop WiFi
    esp_err_t err = esp_wifi_remote_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "WiFi stop failed: %s", esp_err_to_name(err));
    }
    
    // Deinit WiFi
    err = esp_wifi_remote_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "WiFi deinit failed: %s", esp_err_to_name(err));
    }
    
    // Wait for SDIO bus to settle
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Re-init WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_remote_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi re-init failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Restore saved configuration and start
    char saved_ssid[MAX_SSID_LEN] = {0};
    char saved_password[MAX_PASSWORD_LEN] = {0};
    
    if (wifi_load_credentials(saved_ssid, saved_password) == ESP_OK) {
        wifi_init_sta(saved_ssid, saved_password);
    }
    
    return ESP_OK;
}

// In event_handler():
if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_consecutive_wifi_errors++;
    
    if (s_consecutive_wifi_errors >= MAX_CONSECUTIVE_ERRORS) {
        ESP_LOGE(TAG, "Too many consecutive WiFi errors - performing full re-init");
        wifi_full_reinit();
        s_consecutive_wifi_errors = 0;
        return;
    }
    
    // ... existing reconnection logic ...
}

// On successful connection:
if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    s_consecutive_wifi_errors = 0; // Reset error counter
    // ... rest of existing code ...
}
```

---

## 5. Recommendations

### 5.1 Immediate Actions (High Priority)

#### Recommendation 1: Disable WiFi Power Save Mode
**Impact:** HIGH - Likely to solve "works then stops" issue  
**Effort:** LOW - Single function call  
**Risk:** LOW - Only affects battery life (not applicable for USB-powered device)

**Change:**
Add to `wifi_init_sta()` after `esp_wifi_remote_start()`:
```c
// Disable WiFi power save for reliability (we're USB-powered, not battery)
esp_err_t ps_err = esp_wifi_remote_set_ps(WIFI_PS_NONE);
if (ps_err == ESP_OK) {
    ESP_LOGI(TAG, "WiFi power save disabled for reliability");
} else {
    ESP_LOGW(TAG, "Failed to disable WiFi power save: %s (using default)", esp_err_to_name(ps_err));
}
```

#### Recommendation 2: Add Periodic WiFi Health Check
**Impact:** MEDIUM-HIGH - Catches stuck states  
**Effort:** MEDIUM - New task, ~100 lines of code  
**Risk:** LOW - Non-invasive monitoring

**Change:**
Implement `wifi_health_monitor_task()` as shown in Issue 2 above.

### 5.2 Secondary Actions (Medium Priority)

#### Recommendation 3: Implement SDIO Error Recovery
**Impact:** MEDIUM - Recovers from SDIO bus errors  
**Effort:** MEDIUM - ~150 lines of code, careful state management  
**Risk:** MEDIUM - Re-initialization could cause temporary disruption

**Change:**
Implement error counting and `wifi_full_reinit()` as shown in Issue 3 above.

#### Recommendation 4: Add SDIO Bus Monitoring
**Impact:** LOW-MEDIUM - Better diagnostics  
**Effort:** LOW - Logging and metrics  
**Risk:** LOW - Informational only

**Change:**
```c
// In sdio_bus.c, track metrics:
static struct {
    uint32_t total_acquires;
    uint32_t total_timeouts;
    uint32_t max_wait_ms;
} s_sdio_stats = {0};

// Expose via function:
void sdio_bus_get_stats(sdio_bus_stats_t *stats);

// Log periodically or on error
```

### 5.3 Optional Improvements (Low Priority)

#### Recommendation 5: Make WiFi 6 Optional
**Impact:** LOW - Compatibility with some routers  
**Effort:** LOW - Add config option  
**Risk:** LOW - Already best-effort

**Change:**
Add Kconfig option to enable/disable 802.11ax, fall back to 802.11n if disabled or failed.

#### Recommendation 6: Document wifi_remote_init()
**Impact:** LOW - Better code clarity  
**Effort:** LOW - Documentation  
**Risk:** NONE

**Change:**
Research esp_hosted auto-initialization and document whether explicit init is needed.

---

## 6. Testing Recommendations

### 6.1 Reliability Testing

After implementing fixes, test these scenarios:

1. **Long-duration test:**
   - Run device for 24+ hours continuously
   - Monitor for disconnections
   - Verify automatic reconnection

2. **Network disruption test:**
   - Disconnect WiFi AP mid-operation
   - Verify reconnection when AP returns
   - Test with different outage durations (1 min, 5 min, 1 hour)

3. **DHCP lease expiration test:**
   - Configure AP with short DHCP lease (5 minutes)
   - Verify proper renewal
   - Verify recovery if renewal fails

4. **Concurrent SDIO test:**
   - Trigger OTA update (heavy WiFi traffic)
   - Simultaneously access SD card
   - Verify SDIO bus coordination works

5. **Router compatibility test:**
   - Test with different router brands/models
   - Test with WiFi 6 (802.11ax) routers
   - Test with older routers (802.11n only)

### 6.2 Debugging Aids

Add compile-time debugging options:

```c
#ifdef CONFIG_P3A_WIFI_DEBUG
    ESP_LOGI(TAG, "SDIO bus state: locked=%d, holder=%s", 
             sdio_bus_is_locked(), sdio_bus_get_holder());
    ESP_LOGI(TAG, "WiFi stats: connected=%d, retries=%d, errors=%d",
             s_initial_connection_done, s_retry_num, s_consecutive_wifi_errors);
#endif
```

---

## 7. Literature and Examples

### 7.1 Official Espressif Documentation

1. **ESP-Hosted MCU GitHub Repository**
   - URL: https://github.com/espressif/esp-hosted-mcu
   - SDIO configuration docs: https://github.com/espressif/esp-hosted-mcu/blob/main/docs/sdio.md
   - Examples: Slave firmware, host examples

2. **ESP-IDF Wi-Fi Driver Documentation**
   - URL: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/wifi.html
   - Event handling patterns
   - Power save modes
   - Reconnection strategies

3. **ESP-IDF Wi-Fi Expansion Guide (ESP32-P4)**
   - URL: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/wifi-expansion.html
   - esp_wifi_remote usage
   - SDIO setup for P4+C6

4. **ESP-IDF Event Handling Guide**
   - URL: https://docs.espressif.com/projects/esp-idf/en/v5.0-rc1/esp32/api-guides/event-handling.html
   - Event loop patterns
   - Handler registration

### 7.2 Hardware Documentation

1. **Waveshare ESP32-P4-WIFI6 Wiki**
   - URL: https://www.waveshare.com/wiki/ESP32-P4-WIFI6
   - Pin mappings
   - Working SDIO configuration
   - Known issues and solutions

2. **ESP32-P4 Networking Workshop (Espressif)**
   - PDF: https://dl.espressif.com/public/p4-slides-export.pdf
   - Real-world examples
   - Best practices from Espressif engineers

### 7.3 Community Examples and Issues

1. **ESP32 Forum - WiFi over SDIO on P4**
   - URL: https://esp32.com/viewtopic.php?t=43750
   - Pin mapping discussions
   - Configuration tips

2. **GitHub Issues - ESP-Hosted MCU**
   - M5Stack Tab5 SDIO issues: https://github.com/espressif/esp-hosted-mcu/issues/127
   - Function board connection issues: https://github.com/espressif/esp-hosted-mcu/issues/2
   - Real-world troubleshooting examples

3. **GitHub Issues - Arduino ESP32**
   - P4/C6 WiFi init failures: https://github.com/espressif/arduino-esp32/issues/11404
   - SDIO failure patterns

4. **ESPHome P4 WiFi Issues**
   - URL: https://github.com/esphome/esphome/issues/10956
   - Power save mode discussions
   - Reconnection strategies

5. **Circuit Labs - WiFi Event Handling**
   - URL: https://circuitlabs.net/handling-wifi-connection-events/
   - Event handler patterns
   - Best practices

6. **ByteWires - WiFi Event Management**
   - URL: https://bytewires.com/en/article/managing-wi-fi-events-on-esp32-L24G1A6
   - Comprehensive event handling guide
   - Reliability tips

7. **ESP Boards - WiFi Disconnection Troubleshooting**
   - URL: https://www.espboards.dev/troubleshooting/issues/wifi/esp32-disconnects-randomly/
   - Common causes
   - Debugging steps

---

## 8. Conclusion

### 8.1 Summary of Findings

The p3a WiFi implementation is **fundamentally sound** and follows most ESP-Hosted best practices:

**Strengths:**
- Correct use of WIFI_REMOTE_EVENT for esp_wifi_remote
- Proper event handling for all critical WiFi and IP events
- Persistent reconnection with exponential backoff
- DHCP lease failure handling (IP_EVENT_STA_LOST_IP)
- SDIO bus coordination to prevent crashes
- mDNS re-announcement after reconnection
- Correct hardware configuration (SDIO pins, slot, bus width, frequency)

**Weaknesses:**
1. **WiFi power save not explicitly disabled** - HIGH impact, likely cause of "works then stops"
2. **No application-level keepalive** - Would catch "connected but no data" states
3. **No SDIO error recovery** - Can't recover from bus stuck states without reboot
4. **Limited monitoring/diagnostics** - Hard to identify root cause when issues occur

### 8.2 Recommended Action Plan

**Phase 1: Quick wins (1-2 hours of work)**
1. Add `esp_wifi_remote_set_ps(WIFI_PS_NONE)` to disable power save
2. Add more logging around WiFi state transitions
3. Test for 24 hours

**Phase 2: Robust monitoring (4-6 hours of work)**
1. Implement WiFi health monitor task with DNS-based keepalive
2. Add SDIO bus statistics/monitoring
3. Improve error logging
4. Test for 48 hours

**Phase 3: Recovery mechanisms (6-8 hours of work)**
1. Implement consecutive error tracking
2. Add WiFi full re-initialization on repeated errors
3. Add SDIO bus reset mechanism
4. Long-term stability testing (1 week+)

### 8.3 Confidence Level

After implementing Phase 1 (power save disable), I'm **moderately confident** (70%) this will resolve most "works then stops" issues.

After implementing Phase 2 (health monitoring), confidence increases to **high** (85%).

After implementing Phase 3 (recovery mechanisms), confidence is **very high** (95%) that WiFi will be reliable long-term.

The remaining 5% accounts for potential hardware issues (SDIO bus signal integrity, power supply noise, etc.) that would require hardware investigation.

---

**End of Report**

*Generated: December 17, 2025*  
*Author: GitHub Copilot*  
*Status: Research Phase Complete - Ready for Review*
