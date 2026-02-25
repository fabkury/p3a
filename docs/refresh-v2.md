# Channel Refresh Architecture v2

## Problem Statement

p3a needs to keep all channels in the active playset updated periodically. The refresh interval is a runtime-configurable setting. Timestamp persistence (used for cooldown checks) must not occur before SNTP time synchronization to avoid poisoning future cooldown decisions.

### Current Issues

1. **Three separate timing mechanisms** for what is logically one concern:
   - `REFRESH_INTERVAL_SECONDS` — hardcoded `3600` in `play_scheduler_refresh.c`, tracked via `s_last_full_refresh_complete` timestamp.
   - Makapix self-refresh loop — `refresh_task_impl()` in `makapix_channel_refresh.c` polls `vTaskDelay(1000)` in a counted loop for `config_store_get_refresh_interval_sec()` seconds.
   - Giphy cooldown — compares `channel_metadata.last_refresh` against `config_store_get_giphy_refresh_interval()` before each refresh.

2. **Three separate config keys** for the same concept:
   - `refresh_interval_sec` (Makapix, via NVS)
   - `giphy_refresh_interval` (Giphy, via NVS)
   - `REFRESH_INTERVAL_SECONDS` (Play Scheduler, hardcoded)

3. **SNTP handling scattered across multiple locations**: a `s_sntp_synced_observed` polling flag, a one-shot Giphy re-queue block, and a guard clause in `giphy_refresh.c` metadata save.

4. **Timing logic interleaved with work processing**: the periodic timer check was buried inside the worker's channel-processing control flow, leading to a bug where the check was unreachable in steady state.

---

## Design Principles

1. **Separate "when to refresh" from "how to refresh."** Timing decisions must not be entangled with refresh-processing control flow.

2. **No separate timer entity.** Use the worker task's existing `xEventGroupWaitBits()` timeout as the timer. This avoids the FreeRTOS timer daemon blocking hazard and keeps all logic in a single execution context.

3. **Keep Makapix loop-forever tasks.** Converting to one-shot introduces task creation churn and loses natural MQTT connection handling. Instead, replace the sleep loop with an event-driven wait.

4. **Allow per-channel-type intervals.** Giphy (API-rate-limited, slow-changing trending) and Makapix (user-posted, potentially fast-changing) have different optimal refresh frequencies. SD card channels rarely change at runtime and need even less frequent re-indexing.

5. **Gate only timestamp persistence on SNTP.** The periodic timer starts unconditionally. Refresh operations work without a valid clock. Only the `last_refresh` metadata save checks SNTP — if the clock is unsynchronized, the save is skipped, which causes the next cycle's cooldown check to allow the refresh (correct fail-open behavior).

---

## Architecture

### Overview

```
┌──────────────────────────────────────────────────────┐
│                  PS Refresh Worker Task               │
│                                                      │
│  ┌────────────┐    ┌─────────────────────────────┐   │
│  │  Wait with │    │  Per-channel scheduling:     │   │
│  │  timeout   │───>│  if now >= next_refresh_at   │   │
│  │  (1 sec)   │    │    mark refresh_pending      │   │
│  └────────────┘    └─────────────┬───────────────┘   │
│                                  │                    │
│                    ┌─────────────▼───────────────┐   │
│                    │  Process pending channels:   │   │
│                    │  SD card  → rebuild index    │   │
│                    │  Giphy    → call Giphy API   │   │
│                    │  Makapix  → signal task      │   │
│                    │  Artwork  → download file    │   │
│                    └─────────────────────────────┘   │
└──────────────────────────────────────────────────────┘
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
    ┌─────────────┐ ┌───────────┐ ┌──────────┐
    │  Makapix    │ │  Giphy    │ │  SD card  │
    │  refresh    │ │  refresh  │ │  index    │
    │  task       │ │  (sync)   │ │  (sync)   │
    │  (async,    │ └───────────┘ └──────────┘
    │  loop-      │
    │  forever)   │
    └─────────────┘
```

### Component 1 — Per-Channel Refresh Scheduling

Each `ps_channel_state_t` gains a new field:

```c
time_t next_refresh_at;  // Epoch time of next scheduled refresh (0 = refresh immediately)
```

The worker task checks this on every iteration. When `time() >= ch->next_refresh_at` and the channel is idle (not `refresh_in_progress`, not `refresh_pending`), the worker marks it `refresh_pending = true` and processes it. After a successful refresh, the worker sets:

```c
ch->next_refresh_at = time(NULL) + interval_for_channel_type(ch->type);
```

Channels start with `next_refresh_at = 0`, meaning "refresh immediately on first opportunity." This handles the boot case: all channels are processed as soon as the playset loads, without waiting for any timer.

#### Interval Lookup

```c
static uint32_t interval_for_channel_type(ps_channel_type_t type)
{
    switch (type) {
        case PS_CHANNEL_TYPE_GIPHY:
            return config_store_get_giphy_refresh_interval();
        case PS_CHANNEL_TYPE_SDCARD:
            return config_store_get_sdcard_refresh_interval();  // new, default 3600
        case PS_CHANNEL_TYPE_ARTWORK:
            return 0;  // artwork channels don't need periodic refresh
        default:
            return config_store_get_refresh_interval_sec();     // Makapix, default 3600
    }
}
```

This allows per-type intervals from separate config keys — preserving the existing `giphy_refresh_interval` and `refresh_interval_sec` keys — while using a single timing mechanism.

### Component 2 — Refresh Worker Task

The existing `ps_refresh` task, restructured so that **timing logic runs at the top of every iteration**, clearly separated from channel processing.

#### Pseudocode

```
refresh_task:
    while running:
        // ── PHASE 1: Wait for event or timeout ──
        bits = xEventGroupWaitBits(events,
                   WORK_AVAILABLE | SHUTDOWN,
                   clear=true, any=true,
                   timeout=1 second)

        if SHUTDOWN: break

        // ── PHASE 2: Poll Makapix async completions ──
        handle_makapix_async_completions()

        // ── PHASE 3: Schedule due channels ──
        now = time(NULL)
        take(mutex)
        for each channel:
            if channel is idle AND now >= channel.next_refresh_at:
                channel.refresh_pending = true
        give(mutex)

        // ── PHASE 4: Process pending channels ──
        take(mutex)
        ch_idx = find_next_pending()
        if ch_idx < 0:
            give(mutex)
            continue
        // ... process channel (existing logic) ...
```

Phases 1–3 run on **every** iteration, including when no channels are pending. Phase 3 is where the timing decision happens — it's a simple comparison at the top of the loop, impossible to skip via control flow.

#### What Gets Removed from the Worker

| Current mechanism | Disposition |
|---|---|
| `s_last_full_refresh_complete` timestamp | Removed. Replaced by per-channel `next_refresh_at`. |
| `s_sntp_synced_observed` flag | Removed. The worker doesn't need to know about SNTP (see [SNTP handling](#component-4--sntp-handling)). |
| `REFRESH_INTERVAL_SECONDS` hardcoded constant | Removed. Replaced by `interval_for_channel_type()` reading from config store. |
| One-shot SNTP re-queue block for Giphy | Removed. Giphy channels are scheduled by `next_refresh_at` like everything else. |
| Giphy-specific cooldown check against `channel_metadata.last_refresh` | Removed. The per-channel `next_refresh_at` already enforces proper spacing. The redundant cooldown inside `giphy_refresh_channel()` can be removed. |

### Component 3 — Makapix Refresh Task (Event-Driven Wake)

Currently, `refresh_task_impl()` in `makapix_channel_refresh.c` has a loop-forever structure with a 1-second-poll sleep loop between cycles (lines 387–397):

```c
// Current: polled sleep loop
uint32_t elapsed = 0;
while (elapsed < refresh_interval_sec && ch->refreshing) {
    if (makapix_channel_check_and_clear_refresh_immediate()) break;
    vTaskDelay(pdMS_TO_TICKS(1000));
    elapsed++;
}
```

This is replaced with an **event group wait**:

```c
// New: event-driven wait
EventBits_t bits = xEventGroupWaitBits(
    ch->refresh_event_group,
    MAKAPIX_REFRESH_WAKE | MAKAPIX_REFRESH_SHUTDOWN,
    pdTRUE,       // clear on exit
    pdFALSE,      // any bit
    portMAX_DELAY // block until signaled
);

if (bits & MAKAPIX_REFRESH_SHUTDOWN) break;
// else: MAKAPIX_REFRESH_WAKE — do another refresh cycle
```

Each `makapix_channel_t` gets a per-channel `EventGroupHandle_t refresh_event_group`, created in `makapix_channel_create()`.

The **PS refresh worker** signals this event group when it's time to refresh a Makapix channel. The existing `makapix_refresh_channel_index()` → `channel_request_refresh()` path is modified: if the task is already running and waiting, it sets the `MAKAPIX_REFRESH_WAKE` bit instead of trying to create a new task.

#### Benefits Over Current Design

- **No CPU-polling sleep loop.** The task blocks on an event group with zero CPU usage.
- **Instant wake.** When the PS worker decides it's time, the Makapix task wakes immediately — no 1-second polling granularity.
- **MQTT connection preserved.** The task stays alive between cycles, so MQTT state, cursor, and heap allocations are naturally preserved.
- **Clean shutdown.** Set `MAKAPIX_REFRESH_SHUTDOWN` bit → task exits immediately from its wait.

#### Benefits Over One-Shot Conversion

- **No task creation/destruction churn.** The task is created once and lives for the lifetime of the playset.
- **No MQTT reconnection gap.** If MQTT disconnects during the wait, the task handles reconnection naturally on its next cycle (existing `makapix_channel_wait_for_mqtt_or_shutdown()` call at line 404).
- **Cursor continuity.** The loop naturally preserves cursor state across cycles.

### Component 4 — SNTP Handling

SNTP is **not** a gate on the periodic timer or on refresh operations. Channels refresh regardless of whether the clock is synchronized.

SNTP only gates **timestamp persistence**:

```c
// In giphy_refresh.c — save last_refresh only with a valid clock
if (refresh_completed && sntp_sync_is_synchronized()) {
    channel_metadata_save(channel_id, channels_path, &meta);
}
```

```c
// In the PS worker — set next_refresh_at only with a valid clock.
// Without SNTP, next_refresh_at stays 0, causing the channel to be
// refreshed on every cycle. This is fail-open and correct: if we
// can't track time, we refresh conservatively.
if (sntp_sync_is_synchronized()) {
    ch->next_refresh_at = time(NULL) + interval;
} else {
    ch->next_refresh_at = 0;  // will refresh again next cycle
}
```

This approach means:

- **With SNTP:** Channels refresh at their configured interval. `last_refresh` metadata is persisted, enabling cooldown across reboots.
- **Without SNTP (no internet):** Channels refresh on every worker cycle (1 second). In practice, this doesn't cause problems: Giphy and Makapix require network connectivity anyway (guarded by `p3a_state_has_wifi()` and `makapix_mqtt_is_connected()` in `find_next_pending_refresh()`), and SD card rebuilds are cheap.

The `sntp_sync_wait()` API proposed in the v1 plan is still a useful addition for other consumers, but it is not required by the refresh system.

#### What Gets Removed

- `s_sntp_synced_observed` flag and its one-shot re-queue logic.
- The Giphy-specific SNTP re-evaluation block (lines 320–337 of `play_scheduler_refresh.c`). No longer needed because `next_refresh_at = 0` (from the pre-SNTP boot refresh) naturally causes re-evaluation on every cycle until SNTP syncs and a proper timestamp is set.

---

## Config Model

Two config keys with clear, distinct meanings:

| Config Key | Channel Types | Default | Range | Description |
|---|---|---|---|---|
| `refresh_interval_sec` | Makapix, SD card | 3600 | 60–14400 | General channel refresh interval |
| `giphy_refresh_interval` | Giphy | 3600 | 60–14400 | Giphy-specific refresh interval (may differ due to API rate limits) |

Both are already implemented in `config_store.c` and exposed in the Web UI. No config migration needed.

A new `sdcard_refresh_interval` key could be added for SD card channels, but since SD card re-indexing is cheap and infrequent file changes are the norm, reusing `refresh_interval_sec` is sufficient.

---

## Boot Sequence

```
1. Playset loads
   └─ All channels initialized with next_refresh_at = 0 (refresh immediately)
   └─ Worker signals WORK_AVAILABLE
   └─ Worker processes all channels (initial refresh, shows content fast)

2. After each channel completes:
   └─ If SNTP synced: next_refresh_at = now + interval_for_type
   └─ If not synced: next_refresh_at = 0 (will re-check next cycle)

3. SNTP synchronizes (eventually)
   └─ Next completed refresh sets next_refresh_at properly
   └─ Subsequent metadata saves persist last_refresh timestamps

4. Steady state
   └─ Worker wakes every 1 second
   └─ Checks each channel: time() >= next_refresh_at?
   └─ Marks due channels as refresh_pending and processes them
   └─ After completion: next_refresh_at = now + interval
```

---

## Edge Cases

| Scenario | Behavior |
|---|---|
| **No internet** | Giphy channels skipped (WiFi check in `find_next_pending_refresh`). Makapix channels skipped (MQTT check). SD card channels refresh normally. `next_refresh_at` stays 0 until SNTP syncs, but network-dependent channels are gated by connectivity, not by the timer. |
| **SNTP never syncs** | Channels that can refresh (SD card, any cached content) do so on every cycle. Network-dependent channels are blocked by connectivity checks, not by clock. No timestamp metadata is persisted. |
| **Interval changed at runtime** | Takes effect naturally. `interval_for_channel_type()` reads from config store on every call. Next time a channel completes refresh, `next_refresh_at` uses the new interval. |
| **New playset loaded** | All channels start with `next_refresh_at = 0`. Worker processes them immediately. |
| **Refresh takes longer than interval** | `next_refresh_at` is only set after refresh completes. No double-refresh: the clock starts from completion, not from the start of the refresh. |
| **Timer fires mid-cycle** | Not applicable — there is no separate timer. The worker checks `next_refresh_at` in Phase 3, but only for channels that are idle (not `refresh_in_progress`, not `refresh_pending`). A channel mid-refresh is naturally skipped. |
| **MQTT disconnects mid-refresh** | Makapix task handles reconnection internally. PS worker sees async failure and can re-queue. The channel's `next_refresh_at` is not updated (refresh didn't complete), so the worker retries on the next cycle. |
| **Device paused** | Worker continues running. Channels refresh in the background. Content stays fresh for resume. |
| **PICO-8 mode active** | Makapix refresh task exits its cycle cleanly (existing `P3A_STATE_PICO8_STREAMING` check). `next_refresh_at` is not updated. Worker retries after PICO-8 exits. |
| **Channel shared across playsets** | When the user switches playsets and a channel appears in the new playset, it starts with `next_refresh_at = 0`. The worker starts the refresh, but the channel's existing `last_refresh` metadata on disk (persisted from the previous playset) is checked inside the refresh function — if the channel was refreshed very recently, the refresh function can return early (a lightweight optimization, not a scheduling concern). |

---

## Detailed Changes by File

### `play_scheduler_types.h`

Add to `ps_channel_state_t`:

```c
time_t next_refresh_at;  // 0 = refresh on next opportunity
```

### `play_scheduler_refresh.c`

**Remove:**
- `REFRESH_INTERVAL_SECONDS` constant
- `s_last_full_refresh_complete` variable
- `s_sntp_synced_observed` variable
- The SNTP one-shot re-queue block (current lines 320–337)
- The periodic timer check block (current lines 410–441)
- `ps_refresh_reset_timer()` function (replaced by setting `next_refresh_at = 0`)

**Add:**
- `interval_for_channel_type()` helper function
- Phase 3 scheduling block at the top of the loop:

```c
// ── Phase 3: Schedule due channels ──
time_t now = time(NULL);
xSemaphoreTake(state->mutex, portMAX_DELAY);
for (size_t i = 0; i < state->channel_count; i++) {
    ps_channel_state_t *ch = &state->channels[i];
    if (!ch->refresh_pending && !ch->refresh_in_progress &&
        !ch->refresh_async_pending && now >= ch->next_refresh_at) {
        ch->refresh_pending = true;
    }
}
xSemaphoreGive(state->mutex);
```

- After successful refresh completion, set `next_refresh_at`:

```c
if (err == ESP_OK) {
    ch->refresh_in_progress = false;
    if (sntp_sync_is_synchronized()) {
        ch->next_refresh_at = time(NULL) + interval_for_channel_type(ch->type);
    } else {
        ch->next_refresh_at = 0;  // re-check next cycle
    }
    // ... existing post-refresh logic ...
}
```

**Modify:**
- `ps_refresh_reset_timer()` → `ps_refresh_reset_schedule()`: sets `next_refresh_at = 0` for all channels (used when a new playset is loaded).

### `makapix_channel_refresh.c`

**Remove:**
- The polled sleep loop (lines 387–397)

**Replace with:**

```c
// Wait for PS worker to signal next refresh cycle
EventBits_t bits = xEventGroupWaitBits(
    ch->refresh_event_group,
    MAKAPIX_REFRESH_WAKE | MAKAPIX_REFRESH_SHUTDOWN,
    pdTRUE, pdFALSE, portMAX_DELAY
);
if (bits & MAKAPIX_REFRESH_SHUTDOWN) break;
```

### `makapix_channel_impl.c`

**Add to `makapix_channel_t`:**

```c
EventGroupHandle_t refresh_event_group;  // per-channel event group
```

**Modify `makapix_channel_create()`:** Create the event group.

**Modify `makapix_impl_request_refresh()`:** If task is already running (waiting on event group), set `MAKAPIX_REFRESH_WAKE` bit instead of trying to create a new task.

**Modify `makapix_channel_stop_refresh()`:** Set `MAKAPIX_REFRESH_SHUTDOWN` bit.

### `makapix_refresh.c`

**Modify `makapix_refresh_channel_index()`:** When calling `channel_request_refresh()`, the implementation now signals the per-channel event group if the task is already waiting. No functional change to the caller.

### `config_store.c`

No changes. Both `refresh_interval_sec` and `giphy_refresh_interval` remain as separate, independently configurable keys.

### `giphy_refresh.c`

**Keep** the existing SNTP guard on metadata save (line 486):

```c
if (refresh_completed && sntp_sync_is_synchronized()) {
    channel_metadata_save(...);
}
```

**Remove** the redundant per-channel cooldown check that was previously in `play_scheduler_refresh.c` (the one comparing `giphy_meta.last_refresh` against the interval). The PS worker's `next_refresh_at` now handles refresh spacing.

### `sntp_sync.c`

**Optional enhancement:** Add event group API for efficient blocking wait:

```c
static EventGroupHandle_t s_sntp_events;
#define SNTP_EVENT_SYNCED (1 << 0)

bool sntp_sync_wait(TickType_t timeout);
```

This is useful for other consumers but **not required** by the refresh system, which only uses the existing `sntp_sync_is_synchronized()` polling function (called at most once per second).

---

## Summary of Removals

| Current Mechanism | File | Disposition |
|---|---|---|
| `REFRESH_INTERVAL_SECONDS` hardcoded 3600 | `play_scheduler_refresh.c` | Removed. Per-channel `next_refresh_at` + `interval_for_channel_type()` replaces it. |
| `s_last_full_refresh_complete` | `play_scheduler_refresh.c` | Removed. Per-channel `next_refresh_at` tracks timing individually. |
| `s_sntp_synced_observed` polling flag | `play_scheduler_refresh.c` | Removed. SNTP only gates timestamp persistence, not scheduling. |
| SNTP one-shot Giphy re-queue block | `play_scheduler_refresh.c` | Removed. `next_refresh_at = 0` naturally causes re-check. |
| Giphy cooldown check in PS worker | `play_scheduler_refresh.c` | Removed. `next_refresh_at` handles spacing. |
| Makapix `refresh_task_impl()` sleep loop | `makapix_channel_refresh.c` | Replaced with event group wait. Task stays alive, woken by PS worker. |
| `ps_refresh_reset_timer()` | `play_scheduler_refresh.c` | Renamed to `ps_refresh_reset_schedule()`, sets all `next_refresh_at = 0`. |

---

## Why This Approach

### vs. FreeRTOS Timer (v1 plan)

- **No timer daemon blocking.** A FreeRTOS timer callback taking the scheduler mutex would block all software timers in the system (including the auto-swap `dwell_timer`). Using the worker's wait timeout avoids this entirely.
- **No coordination between two execution contexts.** All scheduling and processing happen in the same task — no race conditions between a timer callback and the worker.
- **No mid-cycle double-refresh.** A FreeRTOS timer firing mid-cycle would re-mark already-refreshed channels as pending. With `next_refresh_at` set only after completion, channels that just finished are naturally excluded.

### vs. Makapix One-Shot Conversion (v1 plan)

- **No task creation churn.** The Makapix task is created once per playset lifetime.
- **No MQTT reconnection gap.** The loop-forever task handles MQTT disconnect/reconnect naturally.
- **Cursor and state continuity.** No need to reload cursor from metadata on each cycle — it's preserved in the task's local state.

### vs. Single Unified Interval (v1 plan)

- **Respects different refresh economics.** Giphy API has rate limits and trending changes slowly (2–4 hour interval is optimal). Makapix channels may update faster (15–60 minute interval). SD card rarely changes (1+ hour is fine).
- **No config migration.** Both existing config keys remain. No Web UI changes needed.
- **Single timing mechanism.** Despite having per-type intervals, there is exactly one timer (the worker's wait timeout) and one scheduling check (Phase 3). The per-type intervals only affect the value written to `next_refresh_at`.

### vs. Minimal Fix Only

This plan is more work than the targeted bug fix already applied. The justification: the structural issues (interleaved timing/processing, duplicate timing mechanisms, polled sleep loops) make the code fragile and hard to reason about. The per-channel `next_refresh_at` approach is a clean primitive that eliminates the entire class of "timing check unreachable" bugs and provides a foundation for future enhancements (per-channel intervals, priority-based refresh ordering, etc.).

---

## Summary

Per-channel `next_refresh_at` scheduling in the worker task, event-driven Makapix wake, per-type intervals from existing config keys, SNTP gates only timestamp saves. One task, one scheduling check per iteration, no separate timer entity, no sleep-loop polling.
