# Encapsulate Display Renderer Globals

## Goal
Improve maintainability by grouping the many globals in `display_renderer.c` into a single state struct.

## Current Cues In The Codebase
- `main/display_renderer.c` has a large set of global variables for panel state, buffers, timing, and tasks.

## First-Principles Rationale
A single state struct makes it easier to reason about initialization, lifetime, and concurrency. It also simplifies testing and future refactors.

## Concrete Steps
1. Create a `display_renderer_state_t` struct with all current globals.
2. Replace globals with `static display_renderer_state_t s_display`.
3. Pass `s_display` to helper functions or access via internal getters.

## Risks And Mitigations
- Risk: large mechanical change.
- Mitigation: do it in a single file with careful compile checks.

## Success Criteria
- One place to inspect display state.
- Reduced chance of accidental cross-module access.
