# Create A Testable Core

## Goal
Make critical logic (scheduler, channel selection, metadata) runnable on a host without ESP-IDF.

## Current Cues In The Codebase
- `play_scheduler` is already deterministic and algorithmic, which is good for unit testing.
- Many modules still rely on FreeRTOS types or ESP headers even when not strictly needed.

## First-Principles Rationale
Deterministic logic should be verifiable without hardware. This reduces regression risk and increases confidence in behavior.

## Concrete Steps
1. Isolate pure logic in `components` that avoid ESP dependencies.
2. Add small adapter headers for FreeRTOS types where needed.
3. Add host-side tests that simulate channel inputs and scheduler outputs.

## Risks And Mitigations
- Risk: additional build complexity.
- Mitigation: use a separate `host_tests` target or CMake option.

## Success Criteria
- Core logic can be built and tested on a desktop.
- Deterministic behavior is validated with unit tests.
