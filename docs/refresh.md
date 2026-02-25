# Channel Refresh Architecture

## Problem Statement

p3a needs to keep all channels in the active playset updated periodically (e.g. every 60 minutes). The refresh interval is a runtime-configurable setting. Refreshes must not occur before SNTP time synchronization to avoid spurious timestamps and unnecessary network requests.

### Current Issues

The existing implementation has several structural problems:

1. **Three separate timing mechanisms** that are conceptually the same thing:
   - `REFRESH_INTERVAL_SECONDS` — hardcoded `3600` in `play_scheduler_refresh.c`, tracked via a `s_last_full_refresh_complete` timestamp compared against `time()`.
   - Makapix self-refresh loop — `refresh_task_impl()` in `makapix_channel_refresh.c` sleeps in a 1-second poll loop for `config_store_get_refresh_interval_sec()` seconds, then re-queries MQTT.
   - Giphy cooldown — inside the Play Scheduler worker, compares `channel_metadata.last_refresh` against `config_store_get_giphy_refresh_interval()`.

2. **Three separate config keys** for what is logically one setting:
   - `refresh_interval_sec` (Makapix self-refresh)
   - `giphy_refresh_interval` (Giphy cooldown)
   - `REFRESH_INTERVAL_SECONDS` (Play Scheduler periodic, hardcoded)

3. **Ad-hoc SNTP handling**: a `s_sntp_synced_observed` one-shot flag, polling `sntp_sync_is_synchronized()` every second, and guard clauses scattered in Giphy metadata save logic.

4. **Timing logic interleaved with work processing**: the periodic timer check was buried deep inside a complex control-flow loop in the refresh worker task, making it fragile and hard to reason about (a bug caused the periodic check to be unreachable in steady state).

## Proposed Architecture

### Core Principle

**Separate *when to refresh* from *how to refresh*.**

Two components with clearly distinct responsibilities: a **Refresh Timer** that decides when channels need refreshing, and a **Refresh Worker** that processes the actual refresh operations.

---

### Component 1 — Refresh Timer

A single **FreeRTOS software timer** that fires every X seconds, where X is read from `config_store_get_refresh_interval_sec()`.

The project already uses FreeRTOS timers for auto-swap (`dwell_timer`), so this is a familiar and consistent pattern.

#### Timer Callback

Runs in the FreeRTOS timer daemon task. Must be lightweight — only sets flags and signals:

```c
static void periodic_refresh_timer_cb(TimerHandle_t xTimer)
{
    ps_mark_all_channels_refresh_pending();  // sets refresh_pending = true for all playset channels
    ps_refresh_signal_work();                // wakes the worker task via event group
}
```

#### Lifecycle Rules

| Event | Action |
|---|---|
| Play Scheduler init | Timer is **created** but **not started** |
| SNTP synchronizes | Timer is **started** (event-driven via `sntp_sync_wait()`) |
| New playset loaded | Timer is **reset** (`xTimerReset`) so the interval counts from the moment the new playset finishes its initial refresh |
| Refresh interval config changed | Timer period is **updated** via `xTimerChangePeriod()` |
| No playset active | Timer is **stopped** |

The key property: **the timer never fires before SNTP sync.** This eliminates all timestamp-guarding complexity.

---

### Component 2 — Refresh Worker Task

The existing `ps_refresh` task, **stripped of all timing logic**. It becomes a pure event-driven worker:

```
loop:
    wait for REFRESH_EVENT_WORK_AVAILABLE or REFRESH_EVENT_SHUTDOWN
    poll for Makapix async completions
    for each channel with refresh_pending == true:
        dispatch to appropriate refresh function (SD card / Giphy / Makapix / Artwork)
    if all channels done AND timer not yet started AND SNTP synced:
        start the periodic timer
```

The worker has **zero awareness of intervals or clocks**. It processes channels that have `refresh_pending = true` and nothing else.

#### What the Worker Removes

- `s_last_full_refresh_complete` and all its timestamp tracking
- `s_sntp_synced_observed` and the one-shot SNTP re-queue block for Giphy
- Giphy-specific cooldown check (or optionally kept as a lightweight per-channel optimization — see [Per-Channel Cooldown](#per-channel-cooldown-optional) below)

---

### Component 3 — SNTP Gate

Add an event group to `sntp_sync` so other components can efficiently wait for synchronization without polling:

```c
// In sntp_sync.c
static EventGroupHandle_t s_sntp_events;
#define SNTP_EVENT_SYNCED (1 << 0)

static void sntp_sync_time_cb(struct timeval *tv)
{
    s_synchronized = true;
    xEventGroupSetBits(s_sntp_events, SNTP_EVENT_SYNCED);
}

// New public API
bool sntp_sync_wait(TickType_t timeout)
{
    if (!s_sntp_events) return false;
    return xEventGroupWaitBits(s_sntp_events, SNTP_EVENT_SYNCED,
                               pdFALSE, pdTRUE, timeout) & SNTP_EVENT_SYNCED;
}
```

The refresh worker calls `sntp_sync_wait(0)` (non-blocking) on each iteration to know when to start the periodic timer. No polling loop needed — the event group is checked naturally as part of the existing work loop.

---

### Makapix Channel Refresh Changes

Currently, `refresh_task_impl()` in `makapix_channel_refresh.c` runs as a **loop-forever** task per channel: it queries MQTT, sleeps for the configured interval, then queries again.

Under the new architecture, `refresh_task_impl()` becomes a **one-shot** task:

1. Wait for MQTT connection.
2. Query posts in batches until the target count is reached.
3. Save metadata, signal completion.
4. Exit (task deletes itself).

Periodic re-triggering is handled by the unified timer, which marks the channel as `refresh_pending`, causing the worker to call `makapix_refresh_channel_index()` again. This starts a new one-shot refresh task.

The self-contained sleep loop (lines 388–397 of `makapix_channel_refresh.c`) is removed entirely.

---

### Config Model

**One unified interval:** `refresh_interval_sec`

- Default: `3600` (1 hour)
- Range: `60` – `14400` (1 minute – 4 hours)
- Stored in NVS via `config_store_get_refresh_interval_sec()` / `config_store_set_refresh_interval_sec()`
- Applied to all channel types uniformly via the FreeRTOS timer

The separate `giphy_refresh_interval` config key is eliminated. If backward compatibility with existing Web UI settings is needed, `giphy_refresh_interval` can be kept as a legacy alias that maps to `refresh_interval_sec`.

---

## Boot Sequence

```
1. Playset loads
   └─ All channels get refresh_pending = true
   └─ Worker processes them immediately (shows content fast, before SNTP)

2. SNTP synchronizes
   └─ Worker detects sync, starts the periodic timer

3. Timer fires every X minutes
   └─ Marks all channels refresh_pending = true
   └─ Signals the worker
   └─ Worker processes them

4. Repeat step 3
```

The initial boot refresh happens **before** SNTP sync so the user sees content quickly. The periodic timer only starts **after** SNTP sync, ensuring all subsequent timestamp operations use a valid clock.

---

## Edge Cases

| Scenario | Behavior |
|---|---|
| **No internet** | SNTP never syncs → timer never starts → no wasted API calls. Initial boot refresh still happens (loads cached data from SD card). |
| **Interval changed at runtime** | Call `xTimerChangePeriod()`. Takes effect on next tick. |
| **New playset loaded** | Reset timer via `xTimerReset()`. Mark new channels pending. Worker processes them. Timer counts from the new playset's initial refresh completion. |
| **Refresh takes longer than interval** | Timer fires, channels re-marked pending, but `refresh_in_progress` flag prevents double-processing. Worker picks them up when the current cycle finishes. |
| **MQTT disconnects mid-refresh** | Makapix channels fail gracefully, get re-queued as `refresh_pending`. Timer re-triggers them on next cycle. |
| **Device paused** | Timer continues firing. Worker continues refreshing in the background. Channels stay fresh for when the user resumes — no stale content flash on resume. |
| **PICO-8 mode active** | Worker skips Makapix channels (existing `P3A_STATE_PICO8_STREAMING` check). Timer continues firing; skipped channels will be retried on the next cycle after PICO-8 exits. |

---

## Per-Channel Cooldown (Optional)

Even with a unified timer, there is one scenario where a per-channel cooldown is useful: when the user loads a new playset that includes a channel that was refreshed just minutes ago (e.g. switching between playsets that share a Giphy channel). The timer marks it pending, but the worker can check `channel_metadata.last_refresh` and skip if the channel was refreshed very recently.

This is a lightweight optimization inside the worker, not a timing mechanism. The timer remains the single authority on "when."

```c
// Inside the worker, before dispatching a channel refresh:
if (channel_was_recently_refreshed(ch, MIN_COOLDOWN_SECONDS)) {
    ch->refresh_pending = false;
    continue;
}
```

---

## Why FreeRTOS Timer

| Alternative | Trade-off |
|---|---|
| **Timestamp tracking in the worker loop** (current approach) | Timer check is coupled to control flow. The bug that prompted this redesign — the periodic check being unreachable in steady state — is a structural consequence of this approach. |
| **`esp_timer`** | Both work for a 60-minute period. FreeRTOS timer is simpler (no ISR-context callback concerns) and consistent with existing usage (`dwell_timer`). |
| **Dedicated timer task** | Unnecessary overhead. The FreeRTOS timer daemon task already exists and handles this pattern efficiently. |
| **`vTaskDelay` / sleep loop** (Makapix current approach) | Blocks the task, can't be reconfigured without restarting the task, doesn't compose well with event-driven work processing. |

---

## Summary of Removals

| Current Mechanism | File | Disposition |
|---|---|---|
| `REFRESH_INTERVAL_SECONDS` hardcoded 3600 | `play_scheduler_refresh.c` | Replaced by `config_store_get_refresh_interval_sec()` feeding the FreeRTOS timer |
| `s_last_full_refresh_complete` timestamp | `play_scheduler_refresh.c` | Removed — the timer handles periodicity |
| `s_sntp_synced_observed` polling flag | `play_scheduler_refresh.c` | Removed — replaced by event-driven `sntp_sync_wait()` |
| Makapix `refresh_task_impl()` sleep loop | `makapix_channel_refresh.c` | `refresh_task_impl` becomes one-shot. Periodic re-triggering comes from the unified timer. |
| `giphy_refresh_interval` config key + cooldown | `config_store.c`, `giphy_refresh.c` | Removed or mapped as legacy alias to `refresh_interval_sec` |
| SNTP re-queue for Giphy channels | `play_scheduler_refresh.c` | Removed — timer doesn't start until SNTP syncs, so there is no pre-SNTP periodic refresh to re-evaluate |

## Summary

One timer, one worker, one config, one SNTP gate. The timer is the single authority on "when," the worker is the single authority on "how," and SNTP sync is the prerequisite for the timer to start.
