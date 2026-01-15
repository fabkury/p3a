# Deduplicate Includes And Config Flags

## Goal
Reduce redundant includes and centralize configuration flags for easier maintenance.

## Current Cues In The Codebase
- `main/p3a_main.c` includes `freertos/task.h` twice.
- Debug flags like `DEBUG_PROVISIONING_ENABLED` appear in multiple files.

## First-Principles Rationale
Centralizing configuration avoids drift and ensures build-time flags are coherent. Removing duplicate includes reduces compile noise.

## Concrete Steps
1. Create a shared config header (e.g., `p3a_config.h`) for internal flags.
2. Move debug flags into Kconfig where appropriate.
3. Remove duplicate or unused includes after verifying build.

## Risks And Mitigations
- Risk: changing flags can affect debug workflows.
- Mitigation: keep defaults identical and document any changes.

## Success Criteria
- No repeated includes in critical modules.
- All debug toggles are configured in one place.
