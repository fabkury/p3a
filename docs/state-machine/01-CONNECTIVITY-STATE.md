# p3a State Machine — Connectivity State

**Document:** 01-CONNECTIVITY-STATE.md  
**Status:** Final Specification

---

## 1. Problem Statement

Currently, p3a tracks connectivity states as independent event bits:
- `MAKAPIX_EVENT_WIFI_CONNECTED` / `WIFI_DISCONNECTED`
- `MAKAPIX_EVENT_MQTT_CONNECTED` / `MQTT_DISCONNECTED`

This flat model has problems:

1. **Confusing user messages**: If WiFi is down, the system might say "No MQTT connection" instead of "No WiFi"
2. **No internet check**: Connected to WiFi doesn't mean internet is available
3. **No registration check**: Internet available doesn't mean device is registered
4. **State explosion**: Each independent state doubles the number of valid combinations

---

## 2. Hierarchical State Machine

### 2.1 State Hierarchy

```
NO_WIFI
   │
   ▼ (WiFi connected)
NO_INTERNET
   │
   ▼ (Internet reachable)
NO_REGISTRATION
   │
   ▼ (Makapix registration exists)
NO_MQTT
   │
   ▼ (MQTT connected)
ONLINE
```

Each state implies all states above it are satisfied:
- `NO_MQTT` implies WiFi ✓, Internet ✓, Registration ✓
- `NO_REGISTRATION` implies WiFi ✓, Internet ✓
- etc.

### 2.2 State Definitions

```c
typedef enum {
    CONN_STATE_NO_WIFI,        // WiFi not connected
    CONN_STATE_NO_INTERNET,    // WiFi connected, but no internet
    CONN_STATE_NO_REGISTRATION,// Internet available, but no Makapix registration
    CONN_STATE_NO_MQTT,        // Registered, but MQTT not connected
    CONN_STATE_ONLINE,         // Fully connected
} connectivity_state_t;
```

### 2.3 State Transitions

| Current State | Event | New State |
|---------------|-------|-----------|
| NO_WIFI | WiFi connects | NO_INTERNET (pending internet check) |
| NO_INTERNET | Internet reachable | NO_REGISTRATION (if no creds) or NO_MQTT |
| NO_REGISTRATION | Registration completes | NO_MQTT |
| NO_MQTT | MQTT connects | ONLINE |
| ONLINE | MQTT disconnects | NO_MQTT |
| ONLINE | Internet lost | NO_INTERNET |
| * | WiFi disconnects | NO_WIFI |

---

## 3. Implementation Details

### 3.1 Internet Reachability Check

When WiFi connects, perform a lightweight internet check using DNS lookup for `example.com`:

```c
static bool check_internet_reachable(void) {
    // DNS lookup for example.com (no Makapix dependency)
    struct addrinfo *res = NULL;
    int err = getaddrinfo("example.com", "80", NULL, &res);
    if (res) freeaddrinfo(res);
    return (err == 0);
}
```

**Rationale**: Using `example.com` avoids any fundamental dependency on Makapix Club infrastructure for basic internet detection.

**Frequency**: Check on WiFi connect, then every 60 seconds while in NO_INTERNET state.

### 3.2 Registration Check

```c
static bool has_valid_registration(void) {
    return makapix_store_has_player_key();
}
```

This is a simple NVS check, essentially free.

### 3.3 MQTT Connection

Already implemented in `makapix_mqtt_is_connected()` and `makapix_mqtt_is_ready()`.

### 3.4 MQTT Reconnection Strategy

Use exponential backoff with jitter for MQTT reconnection attempts:

```c
static uint32_t calculate_mqtt_backoff(uint32_t attempt) {
    // Base: 5 seconds, max: 5 minutes
    uint32_t base_ms = 5000;
    uint32_t max_ms = 300000;
    
    uint32_t backoff = base_ms * (1 << min(attempt, 6));  // 5s, 10s, 20s, 40s, 80s, 160s, 320s capped
    backoff = min(backoff, max_ms);
    
    // Add jitter: ±25%
    uint32_t jitter = (prng_next() % (backoff / 2)) - (backoff / 4);
    return backoff + jitter;
}
```

### 3.5 State Machine API

```c
// Initialize connectivity state tracking
void connectivity_state_init(void);

// Get current connectivity state
connectivity_state_t connectivity_state_get(void);

// Get human-readable status message for current state
const char* connectivity_state_get_message(void);

// Get detailed status for Web UI
void connectivity_state_get_details(connectivity_details_t *out);

// Register callback for state changes
void connectivity_state_register_callback(void (*cb)(connectivity_state_t old, connectivity_state_t new));

// Wait for online state (for download manager)
void connectivity_state_wait_for_online(TickType_t timeout);
```

### 3.6 User-Facing Messages

| State | Short Message | Detail Message |
|-------|---------------|----------------|
| NO_WIFI | "No Wi-Fi" | "Connect to Wi-Fi network" |
| NO_INTERNET | "No Internet" | "Wi-Fi connected but no internet access" |
| NO_REGISTRATION | "Not Registered" | "Long-press to register with Makapix Club" |
| NO_MQTT | "Connecting..." | "Connecting to Makapix Cloud" |
| ONLINE | "Online" | "Connected to Makapix Club" |

**Note**: Captive portals are treated as "No Internet" — no special detection needed.

---

## 4. Integration Points

### 4.1 With p3a_state

The `p3a_state` module should use connectivity state for user messages:

```c
void p3a_state_update_connectivity_message(void) {
    if (p3a_state_get_playback_substate() == P3A_PLAYBACK_CHANNEL_MESSAGE) {
        connectivity_state_t conn = connectivity_state_get();
        if (conn != CONN_STATE_ONLINE) {
            // Update channel message to reflect connectivity issue
            p3a_channel_message_t msg = {0};
            msg.type = P3A_CHANNEL_MSG_LOADING;
            strlcpy(msg.detail, connectivity_state_get_message(), sizeof(msg.detail));
            p3a_state_set_channel_message(&msg);
        }
    }
}
```

### 4.2 With Play Scheduler

The Play Scheduler should check connectivity before triggering background refresh:

```c
if (connectivity_state_get() == CONN_STATE_ONLINE) {
    // Safe to trigger MQTT queries for channel refresh
    ps_refresh_signal_work();
}
```

### 4.3 With Download Manager

Downloads only proceed when online:

```c
// In download_task()
if (connectivity_state_get() != CONN_STATE_ONLINE) {
    // Wait for online state
    connectivity_state_wait_for_online(portMAX_DELAY);
}
```

---

## 5. File Location

New file: `components/p3a_core/connectivity_state.c` and `connectivity_state.h`

This keeps connectivity tracking close to the core state machine.

---

## 6. Backward Compatibility

The existing `makapix_channel_events` FreeRTOS event group should remain for low-level signaling. The new connectivity state machine is a higher-level abstraction built on top of it:

```c
// In connectivity_state.c

static void on_wifi_connected(void) {
    // Existing event still fires
    makapix_channel_signal_wifi_connected();
    
    // New state machine updates
    connectivity_state_transition_to(CONN_STATE_NO_INTERNET);
    trigger_internet_check();
}
```

---

*Previous: [00-OVERVIEW.md](00-OVERVIEW.md)*  
*Next: [02-LOCALLY-AVAILABLE-INDEX.md](02-LOCALLY-AVAILABLE-INDEX.md)*
