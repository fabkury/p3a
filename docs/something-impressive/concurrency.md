# Concurrency Audit

Race conditions, deadlocks, and synchronization issues found across the codebase.

---

## Critical

### 1. Prefetch Use-After-Free Window

**Files:** `main/animation_player_loader.c:357`, `main/animation_player_render.c:441`

The prefetch mechanism has a use-after-free window:

- The loader sets `s_back_buffer.prefetch_pending = true` under mutex.
- The render task sets `s_back_buffer.prefetch_in_progress = true` under mutex.
- Between releasing the mutex and calling `prefetch_first_frame()`, the loader
  task could call `unload_animation_buffer()` if a new swap comes in.
- The render task then accesses freed memory (`buf->decoder`,
  `buf->native_frame_b1`).

**Impact:** Heap corruption leading to crashes in `tlsf_free()`. The code
acknowledges this risk in comments (loader lines 201-208), but the protection is
incomplete.

**Fix:** The loader should check BOTH `prefetch_pending` AND
`prefetch_in_progress` before starting a new load, not just after acquiring the
mutex.

---

### 2. ISR Modifies Triple-Buffer State Without Atomics

**File:** `main/display_renderer.c:429-464` (`display_panel_refresh_done_cb`)

Called from DPI panel ISR context, this callback directly modifies
`g_buffer_info[prev_displaying].state` and `g_displaying_idx` without atomic
operations. The `g_buffer_info` array is also accessed by the render task (lines
484-485, 549-551) without mutexes.

ESP32-P4 is dual-core. `volatile` alone does NOT guarantee atomic access or
memory ordering.

**Impact:** Buffer state corruption — showing wrong buffer, triple-buffering
logic breaks, tearing or stale frames.

**Fix:** Use C11 atomic operations or move buffer state tracking entirely to
ISR-safe structures with proper memory barriers.

---

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

### 4. SDIO Bus Lock Race Condition

**File:** `components/sdio_bus/sdio_bus.c:15-16`

```c
static volatile bool s_initialized = false;
static volatile const char *s_current_holder = NULL;
```

No mutex protecting these variables. Multiple callers check and set them from
different tasks. Two tasks could both read `s_current_holder == NULL`, then both
set it, believing they acquired the lock.

**Impact:** Concurrent SD card access leading to filesystem corruption.

**Fix:** Add a proper mutex to `sdio_bus_lock()` / `sdio_bus_unlock()`.

---

### 5. Download Manager Channel-Switch Race

**File:** `components/channel_manager/download_manager.c:838-863`

`download_manager_set_channels()` updates global state while the download task
is running. The download task's snapshot mechanism (lines 198-219) protects
against mid-iteration changes, BUT there's a window where:

1. Download task releases mutex after taking snapshot.
2. `set_channels()` changes the channel list.
3. Download task commits cursor updates for OLD channels to NEW channel list
   (lines 243-251).

**Impact:** Cursor corruption, download loop getting stuck, or crash due to
mismatched `channel_id` lookups.

**Fix:** Use an epoch/generation counter — only commit snapshot if
`s_dl_channel_epoch` matches.

---

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

### 9. Upscale Worker Shared State

**File:** `main/display_renderer.c:40-62`

Upscale worker shared state (`g_upscale_src_buffer`, `g_upscale_dst_buffer`,
`g_upscale_lookup_x/y`) is written by the main render task and read by two
worker tasks on different cores. Declared `volatile` but accessed without
mutexes or atomic operations.

**Impact:** Workers may see partial updates to pointer/lookup table state,
leading to out-of-bounds access, garbage pixels, or crashes.

**Fix:** Use task notifications or message passing instead of shared memory, or
add `portMEMORY_BARRIER()` and C11 atomics for flags.

---

### 10. `portMAX_DELAY` on Production Paths

**Files:** `animation_player.c:429`, `download_manager.c:114`, many others

Using `portMAX_DELAY` timeout on mutex acquisition in production-critical paths.
If the mutex holder crashes or deadlocks, the waiter blocks forever with no
watchdog recovery possible.

**Impact:** System hang requiring hardware reset.

**Fix:** Use reasonable timeouts (e.g., `pdMS_TO_TICKS(5000)`) and handle the
timeout case with error logging and recovery.
