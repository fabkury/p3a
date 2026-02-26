# Service Layer Boundaries

> **Extends**: v1/high-level/formalize-layering-and-dependencies.md  
> **Phase**: 2 (Core Architecture)

## Goal

Define explicit service boundaries with clear interfaces, enforcing unidirectional dependencies and making the architecture self-documenting.

## Status

Completed (dependency lint deferred).

## Progress Checklist

- [x] Add service facades (playback/content/connectivity)
- [x] Route app calls through service APIs
- [x] Enforce dependency rules with CMake `PRIV_REQUIRES`
- [x] Thin `main/p3a_main.c` to orchestrator
- [ ] Add dependency linting or compile-time guards

## Final Decisions (Deferred Items)

- **Dependency linting / compile-time guards**: Deferred because this requires new tooling or CI integration, which is outside the current migration scope and would need agreement on the target environment and enforcement level.

## Current State (v2 Assessment)

Dependencies are implicit and often circular:

```
main/animation_player.c includes:
├── animation_player_priv.h
├── sd_path.h
├── play_scheduler.h
├── sdcard_channel_impl.h
├── playlist_manager.h
├── download_manager.h
├── ugfx_ui.h
├── config_store.h
├── ota_manager.h
├── sdio_bus.h
├── pico8_stream.h
├── pico8_render.h
├── p3a_state.h
├── makapix_channel_impl.h
├── makapix.h
├── p3a_render.h
├── display_renderer.h
├── makapix_channel_events.h
├── fresh_boot.h
└── connectivity_state.h
```

**24 includes** for one file indicates unclear layering.

## v1 Alignment

v1's "Formalize Layering and Dependencies" describes the need. v2 provides concrete service definitions and enforcement strategy.

## Proposed Layer Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        APPLICATION LAYER                                │
│                                                                         │
│   main/p3a_main.c - Boot sequence, lifecycle coordination              │
│   main/ugfx_ui.c - µGFX rendering (moves to component eventually)      │
│                                                                         │
├─────────────────────────────────────────────────────────────────────────┤
│                         SERVICE LAYER                                   │
│                                                                         │
│  ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────────────┐  │
│  │ playback_svc    │ │ content_svc     │ │ connectivity_svc        │  │
│  │                 │ │                 │ │                         │  │
│  │ - play_scheduler│ │ - channel_mgr   │ │ - wifi_manager          │  │
│  │ - frame_render  │ │ - content_cache │ │ - makapix               │  │
│  │ - display_ctrl  │ │ - download_mgr  │ │ - ota_manager           │  │
│  └─────────────────┘ └─────────────────┘ └─────────────────────────┘  │
│                                                                         │
├─────────────────────────────────────────────────────────────────────────┤
│                      INFRASTRUCTURE LAYER                               │
│                                                                         │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────────────────┐  │
│  │ event_bus │ │config_store│ │ p3a_state │ │ time_service (SNTP)  │  │
│  └───────────┘ └───────────┘ └───────────┘ └───────────────────────┘  │
│                                                                         │
├─────────────────────────────────────────────────────────────────────────┤
│                           HAL LAYER                                     │
│                                                                         │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌─────────┐ │
│  │ display_  │ │ touch_    │ │ storage_  │ │ network_  │ │ usb_    │ │
│  │ driver    │ │ driver    │ │ driver    │ │ driver    │ │ driver  │ │
│  │           │ │           │ │           │ │           │ │         │ │
│  │ - LCD     │ │ - GT911   │ │ - SD card │ │ - WiFi    │ │ - CDC   │ │
│  │ - MIPI    │ │ - gesture │ │ - NVS     │ │ - MQTT    │ │ - MSC   │ │
│  │ - VSYNC   │ │ - calib   │ │ - LittleFS│ │ - HTTP    │ │         │ │
│  └───────────┘ └───────────┘ └───────────┘ └───────────┘ └─────────┘ │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

## Dependency Rules

### Rule 1: Downward Only

Each layer may only depend on layers below it:
- Application → Service → Infrastructure → HAL
- Never HAL → Application

### Rule 2: No Peer Dependencies

Services don't call each other directly:
- `playback_svc` does NOT include `connectivity_svc` headers
- Communication happens via event bus

### Rule 3: Narrow Interfaces

Each service exposes one header with minimal API:
- `playback_service.h` - not internal headers
- Internal headers stay private to the component

## Service Definitions

### Playback Service

```c
// playback_service.h
// Owns: play_scheduler, frame rendering, display control

esp_err_t playback_service_init(void);
esp_err_t playback_service_play_channel(const char* channel_id);
esp_err_t playback_service_next(void);
esp_err_t playback_service_prev(void);
esp_err_t playback_service_pause(void);
esp_err_t playback_service_resume(void);
esp_err_t playback_service_set_rotation(int degrees);
```

### Content Service

```c
// content_service.h
// Owns: channel management, caching, downloading

esp_err_t content_service_init(void);
esp_err_t content_service_get_channels(channel_list_t* out);
esp_err_t content_service_refresh_channel(const char* channel_id);
esp_err_t content_service_prefetch(const char* storage_key, const char* url);
esp_err_t content_service_get_stats(content_stats_t* out);
```

### Connectivity Service

```c
// connectivity_service.h
// Owns: WiFi, Makapix, OTA

esp_err_t connectivity_service_init(void);
connectivity_level_t connectivity_service_get_level(void);
esp_err_t connectivity_service_start_provisioning(void);
esp_err_t connectivity_service_cancel_provisioning(void);
esp_err_t connectivity_service_check_ota(void);
esp_err_t connectivity_service_install_ota(void);
```

## Enforcement Strategy

### CMake PRIV_REQUIRES

```cmake
# playback_service/CMakeLists.txt
idf_component_register(
    SRCS "playback_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES play_scheduler display_renderer animation_decoder
    REQUIRES event_bus config_store p3a_state   # Infrastructure only
)
```

### Header Guards

```c
// playback_service_internal.h
#ifndef PLAYBACK_SERVICE_INTERNAL
#error "This header is internal to playback_service"
#endif
```

### Dependency Linting

Add a CI step that parses includes and flags violations:

```python
# scripts/check_layering.py
LAYERS = {
    'hal': ['p3a_board_ep44b', 'display_renderer', ...],
    'infra': ['event_bus', 'config_store', 'p3a_state'],
    'service': ['playback_service', 'content_service', 'connectivity_service'],
    'app': ['main'],
}

# Rule: hal cannot include service or app
# Rule: infra cannot include service or app
# Rule: service cannot include app or peer services
```

## Migration Path

### Step 1: Create Service Facades

Add thin wrappers without changing internals:

```c
// playback_service.c
#include "play_scheduler.h"
#include "display_renderer.h"

esp_err_t playback_service_next(void) {
    return play_scheduler_next(NULL);  // Delegate to existing
}
```

### Step 2: Move Internal Logic

Gradually move implementation behind facades:
- `play_scheduler.c` → internal to `playback_service`
- `animation_player.c` → internal to `playback_service`

### Step 3: Enforce Dependencies

Update CMakeLists.txt to use PRIV_REQUIRES and remove cross-service includes.

### Step 4: Main Becomes Thin

After services exist, `p3a_main.c` becomes:

```c
void app_main(void) {
    // Phase 1: HAL
    p3a_hal_init();
    
    // Phase 2: Infrastructure
    event_bus_init();
    config_store_init();
    p3a_state_init();
    
    // Phase 3: Services
    connectivity_service_init();
    content_service_init();
    playback_service_init();
    
    // Phase 4: Application
    event_bus_emit_simple(EVENT_BOOT_COMPLETE);
}
```

## Success Criteria

- [ ] Each service has exactly one public header
- [ ] No service-to-service direct includes
- [ ] `main/CMakeLists.txt` REQUIRES only services, not internal components
- [ ] CI fails on layering violations
- [ ] `main/p3a_main.c` < 100 lines

## Risks

| Risk | Mitigation |
|------|------------|
| Large refactor surface | Facade pattern allows gradual migration |
| Breaking compilation | Feature branch, comprehensive testing |
| Increased binary size | Inline facades, LTO optimization |
