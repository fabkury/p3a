# Unified State Hierarchy

> **Extends**: v1/high-level/state-machine-single-source-of-truth.md  
> **Phase**: 2 (Core Architecture)

## Goal

Merge the existing multiple state machines (`p3a_state`, `connectivity_state`, `makapix_state`, `app_state`) into a single hierarchical state machine that is the sole authority for "what is the device doing right now."

## Status

Completed.

## Progress Checklist

- [x] Consolidate state definitions into unified header/API
- [x] Merge connectivity state into unified context
- [x] Replace Makapix internal state with event-driven transitions
- [x] Deprecate/remove `app_state` and `connectivity_state` usage
- [x] Route UI/rendering decisions through unified state

## Current State (v2 Assessment)

The codebase has **4+ independent state machines**:

| State Machine | Location | Purpose |
|--------------|----------|---------|
| `p3a_state` | `p3a_core/p3a_state.c` | Global states: PLAYBACK, PROVISIONING, OTA, PICO8 |
| `connectivity_state` | `p3a_core/connectivity_state.c` | Network hierarchy: NO_WIFI → ONLINE |
| `makapix_state_t` | `makapix/makapix.h` | Cloud states: IDLE, PROVISIONING, CONNECTED |
| `app_state` | `app_state/app_state.c` | Legacy application state |

These overlap. For example, "provisioning" exists in both `p3a_state` and `makapix_state`.

## v1 Alignment

v1's "State Machine Single Source of Truth" establishes the principle. v2 provides the concrete hierarchy.

## Proposed Hierarchy

```
P3A_STATE (top-level)
│
├── BOOT
│   ├── INITIALIZING      # Hardware init, NVS, filesystem
│   ├── CONNECTING        # WiFi attempting connection
│   └── STABILIZING       # Post-update reboot check
│
├── PLAYBACK
│   ├── PLAYING           # Normal animation display
│   ├── CHANNEL_MESSAGE   # "Loading...", "Empty channel", etc.
│   ├── PREFETCHING       # Background content loading
│   └── PAUSED            # User-initiated pause
│
├── PROVISIONING
│   ├── WIFI_SETUP        # Captive portal active
│   ├── REGISTERING       # HTTP request to Makapix
│   ├── SHOW_CODE         # Displaying registration code
│   └── WAITING_CONFIRM   # Polling for confirmation
│
├── OTA
│   ├── CHECKING          # Querying GitHub Releases
│   ├── DOWNLOADING       # Fetching firmware
│   ├── VERIFYING         # SHA256 check
│   ├── FLASHING          # Writing to flash
│   └── PENDING_REBOOT    # Waiting for restart
│
├── PICO8_STREAMING       # Live PICO-8 frame input
│
└── ERROR
    ├── SD_MISSING        # No SD card detected
    ├── WIFI_FAILED       # Cannot connect after retries
    └── BOOT_FAILED       # Critical init failure
```

## Connectivity as Orthogonal Dimension

Network state becomes an **attribute** of the current state, not a separate state machine:

```c
typedef struct {
    p3a_state_t current_state;
    p3a_substate_t substate;
    
    // Orthogonal dimensions (not states)
    connectivity_level_t connectivity;  // NO_WIFI, NO_INTERNET, NO_MQTT, ONLINE
    bool sd_mounted;
    bool mqtt_connected;
} p3a_state_context_t;
```

This simplifies queries: "What is the device doing?" has one answer.

## Migration Path

### Step 1: Consolidate State Definitions
```c
// New unified header: p3a_state_v2.h
typedef enum {
    P3A_STATE_BOOT,
    P3A_STATE_PLAYBACK,
    P3A_STATE_PROVISIONING,
    P3A_STATE_OTA,
    P3A_STATE_PICO8,
    P3A_STATE_ERROR,
} p3a_state_t;

typedef enum {
    // Boot substates
    P3A_BOOT_INITIALIZING,
    P3A_BOOT_CONNECTING,
    P3A_BOOT_STABILIZING,
    
    // Playback substates
    P3A_PLAYBACK_PLAYING,
    P3A_PLAYBACK_CHANNEL_MESSAGE,
    // ... etc
} p3a_substate_t;
```

### Step 2: Merge `connectivity_state` into Context

```c
// Before: separate state machine
connectivity_state_t conn = connectivity_state_get();

// After: attribute query
connectivity_level_t conn = p3a_state_get_connectivity();
```

### Step 3: Remove `makapix_state_t` Internal State

Makapix module should emit events, not maintain parallel state:

```c
// Before: makapix has its own state
makapix_state_t state = makapix_get_state();

// After: events update unified state
event_bus_emit(EVENT_MAKAPIX_REGISTERING, NULL);
// ... p3a_state handles transition
```

### Step 4: Deprecate `app_state`

`app_state` component can be removed once all callers migrate.

## API Design

```c
// State queries
p3a_state_t p3a_state_get(void);
p3a_substate_t p3a_state_get_substate(void);
const char* p3a_state_get_name(void);  // Human-readable

// Orthogonal queries
connectivity_level_t p3a_state_get_connectivity(void);
bool p3a_state_is_sd_available(void);
bool p3a_state_is_mqtt_connected(void);

// Transitions (internal use or via events)
esp_err_t p3a_state_transition(p3a_state_t new_state, p3a_substate_t substate);

// Callbacks for UI/rendering
typedef void (*p3a_state_observer_t)(p3a_state_t old, p3a_state_t new, void* ctx);
void p3a_state_add_observer(p3a_state_observer_t observer, void* ctx);
```

## Success Criteria

- [ ] One `p3a_state_get()` call answers "what is the device doing"
- [ ] `connectivity_state.c` and `app_state.c` are deleted
- [ ] `makapix_state_t` is internal implementation detail, not public
- [ ] All UI/rendering decisions flow through `p3a_state`
- [ ] Touch routing uses unified state exclusively

## Risks

| Risk | Mitigation |
|------|------------|
| Regressions during merge | Keep old APIs as wrappers initially |
| Missing edge cases | Comprehensive logging during transition |
| Over-complicated hierarchy | Start simple, add substates as needed |
