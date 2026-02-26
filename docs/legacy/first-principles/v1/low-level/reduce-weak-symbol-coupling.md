# Reduce Weak Symbol Coupling

## Goal
Replace weak symbol calls with explicit registration of handlers to make dependencies clear and failure modes explicit.

## Current Cues In The Codebase
- `components/p3a_core/p3a_touch_router.c` uses many `__attribute__((weak))` externs for handlers.
- If a handler is missing, the call becomes a silent no-op.

## First-Principles Rationale
Weak symbols are convenient but obscure dependencies and make correctness hard to verify. Explicit registration makes it obvious which components are active.

## Concrete Steps
1. Define a `p3a_touch_handlers_t` struct with function pointers.
2. Add `p3a_touch_router_register_handlers()` and require explicit setup in `app_main`.
3. Assert (or log) if critical handlers are missing at boot.

## Risks And Mitigations
- Risk: more boilerplate.
- Mitigation: a single registration call and defaults for optional handlers.

## Success Criteria
- Touch routing errors are explicit instead of silent.
- Module dependencies are discoverable by reading initialization code.
