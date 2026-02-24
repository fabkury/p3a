# Architecture

## System Overview

```
┌──────────────────────────────────────────────────────────────────┐
│                          p3a_main.c                              │
│                     (Application Entry Point)                    │
└──────┬──────────┬──────────────┬──────────────┬─────────────────┘
       │          │              │              │
┌──────▼───┐ ┌───▼─────────┐ ┌─▼───────────┐ ┌▼───────────────┐
│ p3a_core │ │Service Layer│ │Hardware Init│ │   event_bus    │
│  (State  │ │             │ │             │ │  (Pub/Sub)     │
│  Machine)│ │connectivity │ │ app_lcd_p4  │ └────────────────┘
│          │ │  _service   │ │ app_touch   │
│p3a_state │ │content      │ │ app_usb     │
│p3a_render│ │  _service   │ └─────────────┘
│p3a_touch │ │playback     │
│  _router │ │  _service   │
│sd_path   │ │render_engine│
└──────────┘ └──────┬──────┘
                    │
     ┌──────────────┼────────────────────┐
     │              │                    │
┌────▼──────┐ ┌────▼──────────┐ ┌───────▼────────┐
│play       │ │content_cache  │ │wifi_manager    │
│_scheduler │ │loader_service │ │ota_manager     │
│           │ │content_source │ │makapix         │
│playback   │ └───────────────┘ └────────────────┘
│  _queue   │
└────┬──────┘
     │
┌────▼──────────────────────────────────────────┐
│              channel_manager                   │
│  sdcard_channel · makapix_channel · vault      │
└────┬──────────────────────────────────────────┘
     │
┌────▼──────────────────────────────────────────┐
│            animation_player                    │
│  animation_player_render · animation_player    │
│  _loader · display_renderer · decoders         │
└───────────────────────────────────────────────┘
```

## Initialization Sequence

The `app_main()` function in `main/p3a_main.c` executes the following boot sequence:

1. **NVS flash init** — initialize non-volatile storage; erase and retry on corruption
2. **Timezone and seed** — set UTC timezone, generate hardware random seed, store in config
3. **`sdio_bus_init()`** — SDIO bus mutex coordinator (shared between WiFi and SD card)
4. **`p3a_state_init()`** — unified state machine; loads remembered channel, sets initial state
5. **`event_bus_init()`** — async event pub/sub bus; subscribes to playback, system, and Makapix events
6. **`content_service_init()`** — channel cache subsystem (LAi persistence, debounced saves)
7. **`playback_service_init()`** — deterministic playback engine (play scheduler)
8. **`ota_manager_validate_boot()`** — marks new OTA firmware as valid to prevent rollback
9. **`esp_netif_init()` + `esp_event_loop_create_default()`** — network interface and event loop
10. **`p3a_board_littlefs_mount()`** — mount LittleFS partition; check web UI health
11. **`makapix_init()`** — Makapix MQTT/TLS API layer (before channel refresh tasks start)
12. **`app_lcd_init()` + `app_touch_init()`** — LCD display and touch controller
13. **`p3a_board_button_init()`** — BOOT button for pause/resume (conditional, if `P3A_HAS_BUTTONS`)
14. **`p3a_render_init()`** — state-aware rendering dispatch
15. **`app_usb_init()`** — USB composite device (CDC-ACM + Mass Storage)
16. **`connectivity_service_init()`** — Wi-Fi (captive portal or saved network)
17. **`show_url_init()`** — URL artwork download task
18. **`slave_ota_check_and_update()`** — ESP32-C6 co-processor firmware check/update
19. **`ota_manager_init()`** — OTA manager with periodic update checks

## Runtime Operation

- **p3a_core** manages global state transitions (animation playback, provisioning, OTA, PICO-8)
- **play_scheduler** executes the active playset to select artwork across channels
- **event_bus** delivers events between decoupled components (playback, connectivity, UI)
- **animation_player** task renders decoded frames to the LCD via the display renderer
- **p3a_touch_router** routes touch gestures based on current state (navigation, brightness)
- **connectivity_service** manages Wi-Fi connection and triggers OTA checks
- **HTTP server** handles REST API requests, WebSocket connections, and static file serving
- **makapix** MQTT client handles cloud commands and artwork receiving
- **USB tasks** handle CDC-ACM console and Mass Storage access

## Service Layer

The service layer provides thin, high-level wrappers over internal components, decoupling `p3a_main.c` from implementation details:

| Service | File | Purpose |
|---------|------|---------|
| `connectivity_service` | `main/connectivity_service.c` | Wraps Wi-Fi init, OTA check, OTA install |
| `content_service` | `main/content_service.c` | Initializes channel cache subsystem |
| `playback_service` | `main/playback_service.c` | Play scheduler wrapper: channel switching, navigation, pause/resume, rotation |
| `render_engine` | `main/render_engine.c` | Display rotation and background color configuration |

### playback_service API

The playback service is the richest service, exposing:
- `playback_service_play_channel()` / `play_user_channel()` / `play_hashtag_channel()` — channel switching
- `playback_service_next()` / `prev()` — artwork navigation
- `playback_service_pause()` / `resume()` / `is_paused()` — pause with brightness save/restore
- `playback_service_set_rotation()` — screen rotation (0/90/180/270)
