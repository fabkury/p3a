# Architecture Audit

State machine analysis, pipeline review, error propagation, boot sequence, and
configuration consistency.

---

## State Machine (components/p3a_core/p3a_state.c)

### States and Transitions

```
P3A_STATE_BOOT                 (defined but never entered)
P3A_STATE_ANIMATION_PLAYBACK   <-- can always enter from any state
P3A_STATE_PROVISIONING         <-- only from ANIMATION_PLAYBACK
P3A_STATE_OTA                  <-- only from ANIMATION_PLAYBACK
P3A_STATE_PICO8_STREAMING      <-- only from ANIMATION_PLAYBACK
P3A_STATE_ERROR                <-- can enter from any state (implicit)
```

Sub-states for Animation Playback:
- `P3A_PLAYBACK_PLAYING` — normal display
- `P3A_PLAYBACK_CHANNEL_MESSAGE` — status messages

---

## High

### 1. Boot Continues Past Failed Subsystem Inits

**File:** `main/p3a_main.c:430-511`

Seven subsystems can fail initialization without stopping the boot sequence:

| Line | Subsystem | On Failure |
|------|-----------|------------|
| 430 | SDIO bus | Logs warning, continues |
| 437 | State machine | Logs error, continues |
| 466 | Content service | Logs warning, continues |
| 471 | Playback service | Logs error, continues |
| 489 | LittleFS | Logs warning, continues |
| 500 | LCD/touch | Logs warning, continues |
| 511 | Render init | Logs warning, continues |

No aggregate health check exists. The device may appear to work but run in a
degraded state with no user-visible indication.

**Impact:** Hard-to-debug partial failures. Users see a "working" device that
silently drops functionality.

**Recommendation:** Add an aggregate health check after init. Show error UI if
critical subsystems fail.

---

## Medium

### 3. `P3A_STATE_BOOT` is Unreachable

**File:** `components/p3a_core/p3a_state.h:65`

`P3A_STATE_BOOT` is defined in the enum but never entered by any transition
function. Line 398 in `p3a_state.c` initializes directly to
`P3A_STATE_ANIMATION_PLAYBACK`.

**Impact:** Dead code. Confusing for maintainers reading the state diagram.

**Recommendation:** Remove it, or implement a proper boot state transition.

---

## Low

### 6. Same Error Code for 7 Different Preconditions

**File:** `main/animation_player.c:460-518`

`animation_player_request_swap()` returns `ESP_ERR_INVALID_STATE` for seven
different precondition failures:

- UI mode active
- SD card locked
- Swap already requested
- Display not initialized
- Buffer mutex unavailable
- etc.

Callers cannot distinguish between these cases.

**Impact:** Hard to diagnose failures. Poor error messages in logs.

**Recommendation:** Define specific error codes or add an error string output
parameter.

---

### 7. Kconfig Semantic Conflict

**File:** `main/Kconfig.projbuild:5, 91`

`P3A_AUTO_SWAP_INTERVAL_SECONDS` (default 30) and `P3A_MAX_SPEED_PLAYBACK`
(default enabled) have a semantic conflict:

- Max speed playback ignores frame delays.
- Auto-swap timing relies on frame delays.

Both can be enabled simultaneously with unclear resulting behavior.

**Recommendation:** Add a Kconfig warning or mutual exclusion between the two
options.

---

## Rendering Pipeline

### Layers (Top to Bottom)

```
play_scheduler_next()
  -> animation_player_request_swap(swap_request_t)
    -> display_renderer (via frame_callback)
      -> LCD DMA hardware
```

### Layering Violation

**File:** `main/animation_player.c:76-110`

The animation player render dispatch callback directly calls three different
subsystems in a fallback chain:

```c
p3a_render_frame(dest_buffer, stride, &rr);       // p3a_core
animation_player_render_frame_callback(...);        // internal
ugfx_ui_render_to_buffer(...);                      // UI component
```

No abstraction boundary between playback and rendering. Hard to test layers
independently.

---

## Boot Sequence Dependencies

### Initialization Order (p3a_main.c:398-562)

Notable ordering concerns:

2. **Event bus subscribers registered before producers:** Event bus handlers are
   registered (lines 446-461) before WiFi and MQTT are initialized (line 528).
   Safe for now, but fragile against reordering.

3. **Channel restore after SD mount:** Correct order observed. No bug, but no
   explicit dependency documentation either.

---

## Error Propagation Patterns

### Pattern: Silent Swallowing

In the play scheduler (`play_scheduler.c:168-223`), `activate_channel()` returns
errors that are logged but the caller discards the return value (line 431):

```c
activate_channel(i);  // return value discarded
```

### Pattern: Channel Messages Dropped During State Transitions

`p3a_state_set_channel_message()` (line 747) silently drops messages if the
current state is not `P3A_STATE_ANIMATION_PLAYBACK`. Download progress messages
are lost during OTA or provisioning.

### Weak Symbol NULL Checks Inconsistent

Some weak-linked function pointers are checked before use
(`animation_player_request_swap` in `play_scheduler_navigation.c:33-36`), while
others are called without NULL guards (`playback_controller.c:108`). A linking
change could introduce NULL dereferences.
