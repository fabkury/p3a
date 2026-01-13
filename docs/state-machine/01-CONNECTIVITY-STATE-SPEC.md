# Connectivity State Specification

## Overview

p3a needs clear, source-of-truth tracking of its connectivity state at all times. This state determines what operations are allowed (downloading, refreshing, view tracking) and what the user should see.

## State Hierarchy (Cascading)

The connectivity states form a dependency hierarchy:

```
Level 1: WI-FI
    ├─ CONNECTED
    │   └─ Level 2: INTERNET
    │       ├─ AVAILABLE
    │       │   └─ Level 3: MPX REGISTRATION
    │       │       ├─ REGISTERED
    │       │       │   └─ Level 4: MQTT
    │       │       │       ├─ CONNECTED ✅ (Full Online Mode)
    │       │       │       └─ DISCONNECTED ⚠️
    │       │       └─ NOT REGISTERED ⚠️
    │       └─ UNAVAILABLE ⚠️
    └─ DISCONNECTED ⚠️ (Offline Mode)
```

**Cascading Rule**: If a parent level fails, all child levels are implicitly unavailable.

### Example States

| Wi-Fi | Internet | MPX Reg | MQTT | User Message | Behavior |
|-------|----------|---------|------|--------------|----------|
| ❌ | - | - | - | "No Wi-Fi" | Offline: play local only |
| ✅ | ❌ | - | - | "No Internet" | Offline: play local only |
| ✅ | ✅ | ❌ | - | "Not registered. Long-press to register." | Offline: play local only |
| ✅ | ✅ | ✅ | ❌ | "Connecting to Makapix..." | Offline: play local only |
| ✅ | ✅ | ✅ | ✅ | (none - normal operation) | Online: refresh + download in background |

## State Structure

```c
typedef struct {
    bool wifi_connected;
    bool internet_available;
    bool mpx_registered;
    bool mqtt_connected;
    
    // Derived state
    bool online_mode;  // = mqtt_connected (and all ancestors)
    
    // Timestamps for retry logic
    uint32_t wifi_last_disconnect_ms;
    uint32_t internet_last_check_ms;
    uint32_t mqtt_last_disconnect_ms;
} p3a_connectivity_state_t;
```

### Derived State: Online Mode

```c
online_mode = wifi_connected && 
              internet_available && 
              mpx_registered && 
              mqtt_connected;
```

This single boolean determines:
- ✅ Can refresh channel indices
- ✅ Can download artworks
- ✅ Can send view tracking
- ✅ Can process MQTT commands

## State Tracking Location

**Current situation:**
- Wi-Fi state: tracked in `wifi_manager` component
- Internet: not explicitly tracked
- MPX registration: tracked in `makapix` component (makapix_state)
- MQTT: tracked in `makapix_mqtt.c`

**Proposed:**
- Keep existing component-level tracking
- **Add centralized aggregator**: `p3a_connectivity_state` in `p3a_state.c`
- Components report state changes to aggregator via callbacks
- Aggregator computes `online_mode` and notifies subscribers

## State Transitions

### Wi-Fi Events
```c
// From wifi_manager
void on_wifi_connected(void) {
    p3a_connectivity_set_wifi(true);
    // Trigger internet check
    check_internet_connectivity();
}

void on_wifi_disconnected(void) {
    p3a_connectivity_set_wifi(false);
    // Cascading: internet, mpx, mqtt all become unavailable
}
```

### Internet Checks
```c
// Periodic or triggered after Wi-Fi connect
void check_internet_connectivity(void) {
    // HTTP HEAD request to reliable endpoint (e.g., http://clients3.google.com/generate_204)
    bool available = test_internet_connection();
    p3a_connectivity_set_internet(available);
}
```

**Strategy**: 
- Check on Wi-Fi connect
- Periodic check every 60 seconds if Wi-Fi connected
- Don't over-check to avoid network spam

### MPX Registration
```c
// From makapix component
void on_registration_changed(bool registered) {
    p3a_connectivity_set_mpx_registered(registered);
    if (registered) {
        // Trigger MQTT connect attempt
        makapix_connect_if_registered();
    }
}
```

### MQTT Connection
```c
// From makapix_mqtt
void on_mqtt_connected(void) {
    p3a_connectivity_set_mqtt(true);
    // Now in online mode - can trigger background refresh
    if (play_scheduler_is_initialized()) {
        ps_refresh_signal_work();
    }
}

void on_mqtt_disconnected(void) {
    p3a_connectivity_set_mqtt(false);
    // Drop to offline mode
}
```

## User Messaging Priority

When displaying connectivity issues to the user, show the **most fundamental problem**:

```c
const char* get_connectivity_message(void) {
    if (!state.wifi_connected) {
        return "No Wi-Fi connection";
    }
    if (!state.internet_available) {
        return "No Internet connection";
    }
    if (!state.mpx_registered) {
        return "Not registered\nLong-press to register with Makapix";
    }
    if (!state.mqtt_connected) {
        return "Connecting to Makapix...";
    }
    return NULL;  // All good
}
```

## Integration Points

### Play Scheduler
```c
// Before triggering refresh
if (!p3a_connectivity_is_online()) {
    ESP_LOGD(TAG, "Skipping channel refresh - offline mode");
    return ESP_ERR_INVALID_STATE;
}
```

### Download Manager
```c
// Before attempting download
if (!p3a_connectivity_is_online()) {
    ESP_LOGD(TAG, "Skipping download - offline mode");
    return ESP_ERR_INVALID_STATE;
}
```

### View Tracker
```c
// Before sending view event
if (!p3a_connectivity_is_online()) {
    ESP_LOGD(TAG, "Skipping view tracking - offline mode");
    return;
}
```

## Implementation Additions to p3a_state.h

```c
// New connectivity APIs
bool p3a_connectivity_is_online(void);
bool p3a_connectivity_has_wifi(void);
bool p3a_connectivity_has_internet(void);
bool p3a_connectivity_is_registered(void);
bool p3a_connectivity_has_mqtt(void);

// State setters (called by component callbacks)
void p3a_connectivity_set_wifi(bool connected);
void p3a_connectivity_set_internet(bool available);
void p3a_connectivity_set_mpx_registered(bool registered);
void p3a_connectivity_set_mqtt(bool connected);

// User-facing status
const char* p3a_connectivity_get_status_message(void);

// Callback registration for online/offline transitions
typedef void (*p3a_online_mode_cb_t)(bool online, void *user_data);
esp_err_t p3a_connectivity_register_callback(p3a_online_mode_cb_t cb, void *user_data);
```

## Open Questions

### Q1: Internet Connectivity Check Endpoint
**Question**: What endpoint should we use for internet checks?
**Options**:
- A) `http://clients3.google.com/generate_204` (Google's captive portal check)
- B) `http://detectportal.firefox.com/success.txt` (Mozilla)
- C) `http://api.makapix.club/ping` (Makapix backend)

**Recommendation**: Option A or B (public infrastructure, unlikely to change). Option C adds dependency on Makapix availability.

### Q2: Retry Backoff
**Question**: How aggressively should we retry MQTT connections after disconnect?
**Options**:
- A) Exponential backoff: 1s, 2s, 4s, 8s, 16s, max 60s
- B) Fixed interval: 10s
- C) Progressive: 5s, 10s, 30s, 60s

**Recommendation**: Option A with max 60s - balances responsiveness with network courtesy.

### Q3: Show Transient States?
**Question**: Should we show "Connecting to Makapix..." or only show problems?
**Options**:
- A) Show transient connecting states (more verbose)
- B) Only show failures/problems (cleaner UX)

**Recommendation**: Option A initially (helps debugging), with config option to hide transient states later.

## Migration Pain Points

1. **Callback Wiring**: Need to wire up callbacks from wifi_manager, makapix_mqtt to p3a_state
2. **Internet Check**: Need to add periodic internet connectivity check (new HTTP client code)
3. **Message Display**: Need to integrate connectivity messages into display_renderer
4. **State Persistence**: Should we persist last-known connectivity state to NVS?

## Testing Strategy

1. **Unit Tests**: Mock connectivity state transitions
2. **Integration Tests**: 
   - Start with no Wi-Fi, verify offline mode
   - Connect Wi-Fi, verify internet check
   - Provision Makapix, verify MQTT connect
   - Disconnect Wi-Fi, verify graceful degradation
3. **Field Test**: Run for 24h on real device with network instability
