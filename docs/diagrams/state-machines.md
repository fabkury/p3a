# p3a State Machines

This document describes the state machines that govern p3a's behavior, including the core application states, connectivity tracking, display rendering pipeline, and animation playback.

## 1. Core Application State Machine (`p3a_state_t`)

The top-level state machine lives in `components/p3a_core/p3a_state.c`. It is a **hierarchical state machine**: most states contain sub-states, and there is an orthogonal connectivity dimension tracked in parallel.

```
                         ┌──────────────────────────────────────────────────┐
                         │                  P3A_STATE_BOOT                  │
                         │        (System initialization in progress)       │
                         └──────────────────────┬───────────────────────────┘
                                                │ p3a_state_init()
                                                │ sets initial state to
                                                │ ANIMATION_PLAYBACK +
                                                │ substate CHANNEL_MESSAGE("Starting...")
                                                ▼
 ┌────────────────────────────────────────────────────────────────────────────────────┐
 │                         P3A_STATE_ANIMATION_PLAYBACK                               │
 │                                                                                    │
 │  Sub-states (p3a_playback_substate_t):                                             │
 │  ┌─────────────────────┐         ┌──────────────────────────────────┐              │
 │  │ P3A_PLAYBACK_PLAYING│ ◄─────► │ P3A_PLAYBACK_CHANNEL_MESSAGE     │              │
 │  │ (animation frames)  │         │ (FETCHING / DOWNLOADING /        │              │
 │  └─────────────────────┘         │  DOWNLOAD_FAILED / EMPTY /       │              │
 │                                  │  LOADING / ERROR / NONE)         │              │
 │                                  └──────────────────────────────────┘              │
 └──────┬──────────────────────┬──────────────────────┬───────────────────────────────┘
        │                      │                      │
        │ enter_provisioning() │ enter_ota()          │ enter_pico8_streaming()
        │ (only from here)     │ (only from here)     │ (only from here)
        ▼                      ▼                      ▼
 ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────────┐
 │ P3A_STATE_       │   │ P3A_STATE_OTA    │   │ P3A_STATE_           │
 │ PROVISIONING     │   │                  │   │ PICO8_STREAMING      │
 │                  │   │ Sub-states:       │   │                      │
 │ Sub-states:      │   │ ┌──────────────┐ │   │ (No sub-states.      │
 │ ┌──────────────┐ │   │ │OTA_CHECKING  │ │   │  External render     │
 │ │PROV_STATUS   │ │   │ │OTA_DOWNLOADING│ │   │  source drives       │
 │ │PROV_SHOW_CODE│ │   │ │OTA_VERIFYING │ │   │  display directly.)  │
 │ │PROV_CAPTIVE_ │ │   │ │OTA_FLASHING  │ │   │                      │
 │ │  AP_INFO     │ │   │ │OTA_PENDING_  │ │   └──────────┬───────────┘
 │ └──────────────┘ │   │ │  REBOOT      │ │              │
 └────────┬─────────┘   │ └──────────────┘ │              │
          │              └────────┬─────────┘              │
          │                       │                        │
          │ exit_to_playback()    │ exit_to_playback()     │ exit_to_playback()
          │                       │ (or reboot)            │
          └───────────┬───────────┴────────────────────────┘
                      │
                      ▼
               ┌──────────────┐
               │ ANIMATION_   │  (return to normal playback)
               │ PLAYBACK     │
               └──────────────┘

 ┌──────────────────────────────────────────────────────────────────────┐
 │                        P3A_STATE_ERROR                               │
 │                                                                      │
 │  Can be entered from ANY state. Represents a critical system error.  │
 └──────────────────────────────────────────────────────────────────────┘
```

### Transition Rules

| Target State            | Allowed Source State(s)  | Entry Function                          |
|-------------------------|--------------------------|-----------------------------------------|
| `ANIMATION_PLAYBACK`    | Any                      | `p3a_state_enter_animation_playback()`  |
| `PROVISIONING`          | `ANIMATION_PLAYBACK`     | `p3a_state_enter_provisioning()`        |
| `OTA`                   | `ANIMATION_PLAYBACK`     | `p3a_state_enter_ota()`                 |
| `PICO8_STREAMING`       | `ANIMATION_PLAYBACK`     | `p3a_state_enter_pico8_streaming()`     |
| `ERROR`                 | Any                      | `p3a_state_enter_error()`               |

All transitions are **mutex-protected** and follow this sequence:
1. Acquire state mutex
2. Validate via `can_enter_state()` (enforces rules above)
3. Save previous state for history
4. Update current state and relevant sub-states
5. Release mutex
6. Notify all registered callbacks

### Entry Actions

| State                | Initial Sub-state               | Entry Action                         |
|----------------------|---------------------------------|--------------------------------------|
| `ANIMATION_PLAYBACK` | `P3A_PLAYBACK_PLAYING`         | Resumes animation render pipeline    |
| `PROVISIONING`       | `P3A_PROV_STATUS` ("Starting...") | Enters UI render mode (µGFX)     |
| `OTA`                | `P3A_OTA_CHECKING`, progress=0 | Enters UI render mode (µGFX)        |
| `PICO8_STREAMING`    | *(none)*                        | External source drives display       |
| `ERROR`              | *(none)*                        | Displays error message               |

---

## 2. Connectivity State Machine (Orthogonal)

Connectivity is tracked **independently** of the main state. It represents the network status and is updated by external events (Wi-Fi driver, MQTT client, registration status).

```
 ┌──────────────────┐  wifi_connected    ┌──────────────────┐
 │ NO_WIFI          │ ──────────────────►│ NO_INTERNET      │
 │ (WiFi off/       │                    │ (WiFi connected, │
 │  disconnected)   │ ◄────────────────  │  checking access)│
 └──────────────────┘  wifi_disconnected └────────┬─────────┘
                                                  │ internet confirmed
                                                  ▼
                                         ┌──────────────────┐
                                         │ NO_REGISTRATION  │
               registration_changed ───► │ (Internet OK, no │
               (registration removed)    │  Makapix device  │
                                         │  registration)   │
                                         └────────┬─────────┘
                                                  │ registration found
                                                  ▼
 ┌──────────────────┐  mqtt_disconnected ┌──────────────────┐
 │ ONLINE           │ ◄────────────────  │ NO_MQTT          │
 │ (Fully connected │  mqtt_connected    │ (Registered, but │
 │  to Makapix      │ ──────────────────►│  MQTT broker not │
 │  Cloud)          │                    │  connected)      │
 └──────────────────┘                    └──────────────────┘
```

**MQTT reconnection** uses exponential backoff with jitter, starting at a minimum interval and increasing on each disconnect.

### External Events → Connectivity Transitions

| Event                              | Handler Function                        | Transition                                     |
|------------------------------------|-----------------------------------------|------------------------------------------------|
| Wi-Fi STA connected                | `p3a_state_on_wifi_connected()`         | → `NO_INTERNET` (starts internet check timer)  |
| Wi-Fi STA disconnected             | `p3a_state_on_wifi_disconnected()`      | → `NO_WIFI` (stops timer, resets MQTT backoff) |
| MQTT broker connected              | `p3a_state_on_mqtt_connected()`         | → `ONLINE` (resets backoff to minimum)         |
| MQTT broker disconnected           | `p3a_state_on_mqtt_disconnected()`      | → `NO_MQTT` or `NO_REGISTRATION`              |
| Device registration added/removed  | `p3a_state_on_registration_changed()`   | Toggles between `NO_REGISTRATION` ↔ `NO_MQTT` |

---

## 3. Application Status (Orthogonal)

A lightweight status dimension tracks whether the system is idle, processing a command, or in an error condition.

```
 ┌──────────────┐   command received   ┌──────────────┐
 │ APP_STATUS_  │ ────────────────────►│ APP_STATUS_  │
 │ READY        │                      │ PROCESSING   │
 │              │ ◄────────────────────│              │
 └──────────────┘   command complete   └──────┬───────┘
                                              │ unrecoverable error
                                              ▼
                                       ┌──────────────┐
                                       │ APP_STATUS_  │
                                       │ ERROR        │
                                       └──────────────┘
```

---

## 4. Boot Sequence

The boot sequence in `main/p3a_main.c` (`app_main()`) progresses through initialization phases before the state machine takes over:

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │                         BOOT SEQUENCE                               │
 │                                                                     │
 │  Phase 1: System Init                                               │
 │  ├── NVS flash init (with erase recovery)                           │
 │  ├── Timezone setup (UTC for Live Mode)                             │
 │  └── Random seed init                                               │
 │                                                                     │
 │  Phase 2: Storage & State                                           │
 │  ├── SDIO bus init (WiFi/SD mutual exclusion)                       │
 │  ├── p3a_state_init() ─► state = ANIMATION_PLAYBACK                │
 │  │                        substate = CHANNEL_MESSAGE("Starting...")  │
 │  └── Event bus init                                                 │
 │                                                                     │
 │  Phase 3: Services                                                  │
 │  ├── Content service init (channel cache)                           │
 │  ├── Playback service init (play scheduler)                         │
 │  └── OTA boot validation                                            │
 │                                                                     │
 │  Phase 4: Network & Hardware                                        │
 │  ├── Network interface init                                         │
 │  ├── ESP event loop                                                 │
 │  ├── LittleFS mount (WebUI assets)                                  │
 │  ├── Makapix API init                                               │
 │  ├── LCD display init (shows boot logo)                             │
 │  └── Touch screen init                                              │
 │                                                                     │
 │  Phase 5: Rendering                                                 │
 │  ├── p3a_render_init() ─► links state machine to rendering          │
 │  └── USB init (PICO-8 streaming, serial)                            │
 │                                                                     │
 │  Phase 6: Connectivity                                              │
 │  ├── WiFi init (captive portal or stored credentials)               │
 │  ├── ESP32-C6 co-processor firmware check                           │
 │  └── OTA manager init (periodic update checks)                      │
 │                                                                     │
 │  ► System Ready ─ "p3a ready: tap the display..."                   │
 └─────────────────────────────────────────────────────────────────────┘
```
