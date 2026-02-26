# Standardize Error Handling Strategy

## Goal
Define which failures are fatal and which are recoverable, and apply that consistently.

## Current Cues In The Codebase
- Some init calls use `ESP_ERROR_CHECK`, others log and continue even for significant failures.
- Boot sequence mixes critical and optional service initialization.

## First-Principles Rationale
A device should be explicit about its minimum viable functionality. Consistent error handling improves predictability and reliability.

## Concrete Steps
1. Define boot phases and what is required per phase.
2. Use a common helper (e.g., `p3a_boot_check`) for logging and policy decisions.
3. Ensure failures are surfaced to the UI or logs in a consistent way.

## Risks And Mitigations
- Risk: stricter checks may prevent boot in some edge cases.
- Mitigation: provide fallback paths and visible error UI.

## Success Criteria
- Boot logs show consistent patterns for failure and recovery.
- Users receive actionable feedback when a critical subsystem fails.
