# Make The State Machine The Single Source Of Truth

## Goal
Route all UI, interaction, and playback transitions through `p3a_state` so the system has one authoritative model.

## Current Cues In The Codebase
- `p3a_state` defines global and sub-states, but UI mode and provisioning are also managed elsewhere.
- `main/p3a_main.c` has a Makapix monitor task that manually transitions UI mode.
- `p3a_render` decides what to draw based on `p3a_state`, but animation rendering sometimes bypasses it.

## First-Principles Rationale
A real-time device benefits from a single state model that drives:
- Rendering decisions
- Touch routing
- Activity gating (OTA, USB, PICO-8)

## Concrete Steps
1. Use `p3a_state` transitions to enter and exit UI mode (no direct UI toggles elsewhere).
2. Ensure `p3a_render` is the only component deciding what is rendered per frame.
3. Convert ad-hoc tasks (e.g., Makapix monitor) into state event handlers.

## Risks And Mitigations
- Risk: accidental regressions during migration.
- Mitigation: move one flow at a time (e.g., provisioning) and keep fallback paths temporarily.

## Success Criteria
- Any UI or mode change is explainable by a state transition.
- Touch routing uses state exclusively, with no hidden branching.
