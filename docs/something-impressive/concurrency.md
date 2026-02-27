# Concurrency Audit

Race conditions, deadlocks, and synchronization issues found across the codebase.

---

## Critical

### 3. MQTT Callback Deadlock

**File:** `components/makapix/makapix_mqtt.c:262-322`

The MQTT event handler calls user callbacks (`s_command_callback`,
`s_response_callback`) while holding internal MQTT library locks. If the
callback tries to publish MQTT messages or disconnect, it deadlocks trying to
acquire the same lock.

**Impact:** System hang. May manifest as "Timed out waiting for render mode" in
logs.

**Fix:** Queue events and dispatch from a separate task context, not from the
MQTT event handler.

---

## High


### 6. Play Scheduler Mutex Order Violation

**Files:** `components/play_scheduler/play_scheduler.c:318`,
`components/channel_manager/download_manager.c:378`

Lock ordering inconsistency:

- **Path A:** `play_scheduler.c:378` takes `s_state.mutex`, then calls channel
  functions.
- **Path B:** `download_manager.c:307` takes registry mutex, calls
  `channel_cache_get_missing_batch()`.
- **Path C:** `channel_cache.c` may need to register cache, requiring
  `registry_mutex`.

If Path A holds play_scheduler mutex and tries to register a cache, while Path B
holds registry_mutex and tries to access play_scheduler state: deadlock.

**Impact:** System freeze during channel switches when downloads are active.

**Fix:** Establish global lock ordering: always acquire `registry_mutex` before
`play_scheduler` mutex, never the reverse.

---

## Moderate

### 7. 51 `volatile` Flags Without Atomics

**Files:** Multiple

51 instances of `volatile` variables used for inter-task communication without
mutexes or atomic operations:

- `s_mqtt_connected` (`makapix_mqtt.c:32`)
- `g_display_mode_request/active` (`display_renderer.c:27-28`)
- `s_channel_loading/s_channel_load_abort` (`makapix.c:45-46`)

`volatile` only prevents compiler optimization; it does NOT provide atomic
access or memory ordering guarantees on multi-core ESP32-P4.

**Impact:** Torn reads/writes, stale data, logic errors in state machines.

**Fix:** Replace with `atomic_bool` (C11 `<stdatomic.h>`) or protect with
mutexes.

---

### 8. Double-Check Lock Without Memory Barrier

**File:** `main/animation_player.c:626-634`

```c
bool locked = s_sd_export_active;           // unsynchronized read
if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
    locked = s_sd_export_active;            // synchronized read
```

Without `volatile` keyword or memory barriers, the compiler may optimize away
the second read. The variable is NOT declared volatile.

**Impact:** Stale reads of `s_sd_export_active` could allow SD access during USB
export, corrupting the filesystem.

**Fix:** Declare `s_sd_export_active` as `volatile` or eliminate the
double-check pattern.

---

### 10. `portMAX_DELAY` on Production Paths

**Files:** `animation_player.c:429`, `download_manager.c:114`, many others

Using `portMAX_DELAY` timeout on mutex acquisition in production-critical paths.
If the mutex holder crashes or deadlocks, the waiter blocks forever with no
watchdog recovery possible.

**Impact:** System hang requiring hardware reset.

**Fix:** Use reasonable timeouts (e.g., `pdMS_TO_TICKS(5000)`) and handle the
timeout case with error logging and recovery.
