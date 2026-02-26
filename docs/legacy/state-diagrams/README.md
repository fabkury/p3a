# p3a State Machine Diagrams

This folder contains Mermaid state diagrams documenting p3a's internal state machines. Each diagram is in a separate Markdown file and can be rendered by GitHub, VS Code (with the Mermaid extension), or any Mermaid-compatible viewer.

## Diagram Index

| File | State Machine | Description |
|------|--------------|-------------|
| [01-p3a-core.md](01-p3a-core.md) | **p3a Core** | Top-level application state machine with sub-states for playback, provisioning, and OTA |
| [02-connectivity.md](02-connectivity.md) | **Connectivity Level** | Orthogonal state tracking WiFi/Internet/MQTT connectivity |
| [03-wifi-manager.md](03-wifi-manager.md) | **WiFi Manager** | WiFi connection lifecycle with retry, captive portal, and recovery |
| [04-playback-controller.md](04-playback-controller.md) | **Playback Controller** | Render source selection (animation vs PICO-8 vs idle) |
| [05-ota-firmware.md](05-ota-firmware.md) | **OTA Manager (Firmware)** | Firmware update lifecycle (check, download, verify, flash) |
| [06-ota-webui.md](06-ota-webui.md) | **OTA Manager (Web UI)** | Web UI storage partition update lifecycle |
| [07-makapix.md](07-makapix.md) | **Makapix** | Cloud provisioning, MQTT connection, and registration flow |
| [08-makapix-mqtt.md](08-makapix-mqtt.md) | **Makapix MQTT Client** | Low-level MQTT client lifecycle (create, start, stop, destroy) |

## Architecture Overview

p3a uses a **hierarchical state machine** centered on `p3a_core`, with several **orthogonal state dimensions** and **component-level state machines** that operate independently:

```
                    ┌─────────────────────────────────────────┐
                    │           p3a Core (global)              │
                    │  BOOT → ANIMATION_PLAYBACK ←→ OTA       │
                    │              ↕               ←→ PROV     │
                    │          PICO8_STREAMING     → ERROR     │
                    └──────────┬──────────────────────────────┘
                               │
          ┌────────────────────┼────────────────────┐
          │                    │                    │
   ┌──────▼──────┐    ┌───────▼──────┐    ┌───────▼───────┐
   │ Connectivity │    │  Playback    │    │  App Status   │
   │   (ortho)    │    │  Controller  │    │   (ortho)     │
   │  NO_WIFI →   │    │ NONE/ANIM/  │    │ READY/PROC/   │
   │  ... ONLINE  │    │ PICO8       │    │ ERROR         │
   └──────────────┘    └─────────────┘    └───────────────┘

   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
   │ WiFi Manager │    │ OTA Manager  │    │   Makapix    │
   │ (component)  │    │ (component)  │    │ (component)  │
   │ Implicit SM  │    │ FW + WebUI   │    │ Prov + MQTT  │
   └──────────────┘    └──────────────┘    └──────────────┘
```

### Key Design Patterns

- **Centralized global state**: `p3a_core` owns the top-level state; components coordinate through it
- **Orthogonal dimensions**: Connectivity level and app status are independent of the global state
- **Mutex protection**: All state transitions are thread-safe
- **Callback notification**: Components register for state change callbacks via `p3a_state_register_callback()`
- **Entry guards**: Transitions are validated (e.g., OTA can only be entered from ANIMATION_PLAYBACK)
