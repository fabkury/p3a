# Introduce A Central Event Bus

## Goal
Replace ad-hoc cross-module calls with explicit events, reducing coupling and simplifying state transitions and UI updates.

## Current Cues In The Codebase
- `p3a_state` exists, but other modules still call each other directly (e.g., touch router calling Wi-Fi, provisioning, and display functions).
- Makapix state changes are monitored via a dedicated task in `main/p3a_main.c`.
- Download, OTA, and connectivity status are updated through direct function calls.

## First-Principles Rationale
In a system with multiple asynchronous subsystems, an event bus provides:
- A single place to observe system behavior.
- Isolation between producers and consumers.
- A way to log or replay behavior for debugging.

## Candidate Event Types
- Connectivity events: Wi-Fi up/down, internet reachable, MQTT connect/disconnect.
- Content events: channel loaded, download progress, asset decode failed.
- Playback events: swap requested, swap completed, dwell time changes.
- UI events: provisioning started, show code, OTA progress.

## Concrete Steps
1. Create an `event_bus` component with a queue and typed event struct.
2. Make `p3a_state` the consumer of high-level events.
3. Convert cross-module calls to events for a single integration path.
4. Provide a debug subscriber for logging and diagnostics.

## Risks And Mitigations
- Risk: event storms or queue overflow.
- Mitigation: backpressure strategies, event coalescing (e.g., latest-only for progress).

## Success Criteria
- Features can be added by emitting or consuming events without editing unrelated modules.
- State transitions always go through the same path.
