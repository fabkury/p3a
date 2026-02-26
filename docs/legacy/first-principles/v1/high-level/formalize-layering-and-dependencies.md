# Formalize Layering And Dependencies

## Goal
Make subsystem boundaries explicit so data flows in one direction and dependencies are obvious. This reduces accidental coupling and makes refactors safer.

## Current Cues In The Codebase
- `main/p3a_main.c` initializes many subsystems directly, which implies cross-layer knowledge in one place.
- Modules often call each other directly instead of via narrow interfaces (e.g., state, rendering, and network are intertwined).
- The state machine exists (`components/p3a_core/include/p3a_state.h`) but not all flows are routed through it.

## First-Principles Rationale
A real-time device like p3a is easier to reason about when each layer has a single responsibility:
- Hardware and device drivers should never depend on application logic.
- Application logic should not reach into device-specific details.
- Services should expose minimal, explicit interfaces.

## Candidate Architecture Shape
- HAL: `p3a_board_*`, display/touch drivers, USB, SDIO, SD card.
- System services: NVS, filesystems, time sync, task/timer helpers.
- Connectivity: Wi-Fi, captive portal, MQTT, HTTP.
- Content and storage: channel caches, downloads, vault storage, playlist metadata.
- Playback planning: `play_scheduler` as a domain service.
- Media pipeline: decoders + render pipeline + display renderer.
- UI and interaction: global state machine, touch router, web UI, REST API.

## Concrete Steps
1. Define service boundaries with clear headers (e.g., `display_service.h`, `content_service.h`).
2. Reduce direct includes across layers; move those calls behind service APIs.
3. Move orchestration from `app_main` into a boot manager that starts services in phases.
4. Enforce one-way dependency rules in CMake (e.g., no `main` headers included from `components` unless explicitly allowed).

## Risks And Mitigations
- Risk: large refactor surface area.
- Mitigation: iterate service-by-service, starting with low-churn subsystems (display, touch).

## Success Criteria
- You can trace data flow from input to output without circular dependencies.
- The boot sequence is staged and modular rather than monolithic.
