# Make Animation Player State Private

## Goal
Reduce the surface area of mutable globals in `animation_player.c` and expose explicit accessors.

## Current Cues In The Codebase
- `main/animation_player.c` declares many file-scope variables without `static` (e.g., buffers and flags).

## First-Principles Rationale
Unscoped globals make it easy to introduce hidden dependencies. Private state with narrow accessors improves modularity.

## Concrete Steps
1. Mark file-scope variables as `static` where possible.
2. Provide accessors only when other files need data.
3. Keep synchronization and locking inside the module.

## Risks And Mitigations
- Risk: external modules might be relying on globals implicitly.
- Mitigation: use `rg` to confirm usage and introduce accessors for legitimate needs.

## Success Criteria
- Animation player internals are not reachable from other modules without intent.
- Thread safety improves by centralizing state updates.
