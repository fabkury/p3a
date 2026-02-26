# Clarify UI Transitions Across States

## Goal
Ensure UI mode transitions are consistent and owned by the state machine.

## Current Cues In The Codebase
- Provisioning flow enters and exits UI mode from multiple places (`p3a_main.c`, `p3a_touch_router.c`).
- The render pipeline can fall back to UI rendering, but UI mode also changes display ownership.

## First-Principles Rationale
UI transitions are a visible user experience; inconsistent transitions can cause flicker or blank frames. Centralizing them improves stability.

## Concrete Steps
1. Make `p3a_state` responsible for entering/exiting UI mode.
2. Convert touch-triggered UI changes into state transitions.
3. Ensure `p3a_render` is always the rendering authority.

## Risks And Mitigations
- Risk: short-term regressions in UI flows.
- Mitigation: keep fallbacks during migration and log transitions.

## Success Criteria
- No direct UI mode toggles outside state transitions.
- Rendering is smooth and deterministic across modes.
