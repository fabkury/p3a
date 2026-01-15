# Clarify Resource Ownership And Contention

## Goal
Explicitly define which module owns shared resources like SDIO, SD card, and download queues.

## Current Cues In The Codebase
- `sdio_bus` exists to coordinate SDIO access.
- OTA and downloads can overlap with SD card access and playback.
- Some subsystems coordinate indirectly rather than through a single arbitration point.

## First-Principles Rationale
Shared resources should have one owner and explicit access policies. This prevents subtle contention bugs.

## Concrete Steps
1. Centralize SDIO arbitration in a single module with a documented API.
2. Make OTA and download flows request access rather than locking internally.
3. Expose resource usage state to the state machine (for gating).

## Risks And Mitigations
- Risk: deadlocks or priority inversions.
- Mitigation: use timeouts and diagnostics, log who holds the resource.

## Success Criteria
- Contention events are logged and recoverable.
- Playback does not stall due to untracked SDIO usage.
