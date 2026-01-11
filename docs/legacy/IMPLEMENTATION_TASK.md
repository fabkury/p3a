# p3a Play Scheduler Migration - Implementation Task

**Priority**: High  
**Estimated Scope**: Multiple files across several components  
**Context**: ESP-IDF project for ESP32-P4, pixel art player called p3a

---

## Background and Context

p3a is undergoing a migration from a **channel-based architecture** to a **Play Scheduler architecture**:

- **OLD (DEPRECATED)**: `channel_player`, `channel_navigator`, `play_navigator`, channel-based navigation, "switching from channel A to B", `play_channel` commands
- **NEW (ACTIVE)**: Play Scheduler, scheduler commands, EqE/PrE/MaE exposure modes, NAE (New Artwork Events), lookahead-based downloads

The Play Scheduler core is implemented (Phases 1-13 complete), but integration gaps remain. Your task is to complete the migration by fixing these gaps.

### Key Architecture Points

1. **Play Scheduler** (`components/play_scheduler/`) owns playback decisions
2. **Scheduler Commands** replace "channel switching" - a command contains channel(s), exposure mode, and pick mode
3. **Lookahead Buffer (L=32)** drives download prefetching - download the soonest not-yet-available entry
4. **Background Refresh** updates channel indices sequentially, then every 1 hour
5. **SWRR (Smooth Weighted Round Robin)** selects which channel to pick from based on exposure weights

---

## Tasks to Implement

### Task 1: Create Dedicated Makapix Refresh API (No Channel Switching)

**Problem**: `play_scheduler_refresh.c` calls `makapix_request_channel_switch()` to trigger index refresh. This is wrong - it uses the legacy channel-switching mechanism.

**Current Code** (`components/play_scheduler/play_scheduler_refresh.c:99-113`):
```c
if (strcmp(ch->channel_id, "all") == 0) {
    makapix_request_channel_switch("all", NULL);
} else if (strcmp(ch->channel_id, "promoted") == 0) {
    makapix_request_channel_switch("promoted", NULL);
} else if (strncmp(ch->channel_id, "user:", 5) == 0) {
    const char *sqid = ch->channel_id + 5;
    makapix_request_channel_switch("by_user", sqid);
} else if (strncmp(ch->channel_id, "hashtag:", 8) == 0) {
    const char *tag = ch->channel_id + 8;
    makapix_request_channel_switch("hashtag", tag);
}
```

**Required Changes**:

1. **Create new API in `components/makapix/`** - Add a function like:
   ```c
   /**
    * @brief Refresh a channel's index cache (no channel switching)
    * 
    * Queries MQTT for posts and updates the channel's .bin cache file.
    * Does NOT change the "current channel" or trigger navigation.
    * 
    * @param channel_type "all", "promoted", "by_user", "hashtag"
    * @param identifier User sqid or hashtag (NULL for all/promoted)
    * @return ESP_OK on success
    */
   esp_err_t makapix_refresh_channel_index(const char *channel_type, const char *identifier);
   ```

2. **Implementation** should:
   - Use existing `makapix_api_query_posts()` to fetch posts
   - Update the appropriate `.bin` cache file at `/sdcard/p3a/channel/`
   - NOT call any channel switching or navigation functions
   - Reference `makapix_channel_refresh.c:refresh_task_impl()` for how the query and index update works

3. **Update `play_scheduler_refresh.c`** to call the new API instead of `makapix_request_channel_switch()`

**Files to modify**:
- `components/makapix/makapix.h` - Add declaration
- `components/makapix/makapix.c` - Add implementation
- `components/play_scheduler/play_scheduler_refresh.c` - Use new API

---

### Task 2: Implement SWRR Channel Selection in Batch Generation

**Problem**: The SWRR algorithm exists (`ps_swrr_select_channel()` in `play_scheduler_swrr.c`) but isn't actually used during batch generation.

**Current Code** (`components/play_scheduler/play_scheduler.c:243-280`):
```c
void ps_generate_batch(ps_state_t *state)
{
    // ... 
    for (int b = 0; b < PS_LOOKAHEAD_SIZE; b++) {
        ps_artwork_t candidate;
        bool found = false;

        // For single-channel mode (N=1), just pick from the first active channel
        // Multi-channel SWRR will be added in Phase 3  <-- THIS COMMENT IS OUTDATED
        for (size_t i = 0; i < state->channel_count && !found; i++) {
            if (!state->channels[i].active) continue;
            found = ps_pick_artwork(state, i, &candidate);
        }
        // ...
    }
}
```

**Required Changes**:

Replace the linear iteration with SWRR selection:

```c
void ps_generate_batch(ps_state_t *state)
{
    if (!state || state->channel_count == 0) {
        return;
    }

    ESP_LOGD(TAG, "Generating batch of %d items", PS_LOOKAHEAD_SIZE);

    for (int b = 0; b < PS_LOOKAHEAD_SIZE; b++) {
        ps_artwork_t candidate;
        bool found = false;

        // Use SWRR to select channel (respects exposure weights)
        int ch_idx = ps_swrr_select_channel(state);
        if (ch_idx >= 0) {
            found = ps_pick_artwork(state, (size_t)ch_idx, &candidate);
        }

        // Fallback: if SWRR returned -1 (no active channels) or pick failed,
        // try any active channel
        if (!found) {
            for (size_t i = 0; i < state->channel_count && !found; i++) {
                if (!state->channels[i].active) continue;
                found = ps_pick_artwork(state, i, &candidate);
            }
        }

        if (found) {
            // Check immediate repeat
            if (candidate.artwork_id == state->last_played_id) {
                // Try once more with a different channel
                ch_idx = ps_swrr_select_channel(state);
                if (ch_idx >= 0) {
                    ps_pick_artwork(state, (size_t)ch_idx, &candidate);
                }
            }

            ps_lookahead_push(state, &candidate);
        }
    }

    ESP_LOGD(TAG, "Generation complete, lookahead now has %zu items", state->lookahead_count);
}
```

**Files to modify**:
- `components/play_scheduler/play_scheduler.c` - Fix `ps_generate_batch()`

---

### Task 3: Add 1-Hour Periodic Refresh Timer

**Problem**: After the initial refresh cycle completes, channels are never refreshed again. The requirement is to re-refresh all channels 1 hour after completing the previous cycle.

**Current Code** (`components/play_scheduler/play_scheduler_refresh.c`):
The refresh task processes `refresh_pending` flags but never re-sets them.

**Required Changes**:

Add timestamp tracking and periodic re-triggering:

```c
// Add near top of file
static time_t s_last_full_refresh_complete = 0;
#define REFRESH_INTERVAL_SECONDS 3600  // 1 hour

// Modify refresh_task() - after the existing logic, add periodic check:
static void refresh_task(void *arg)
{
    ps_state_t *state = ps_get_state();
    ESP_LOGI(TAG, "Refresh task started");
    s_task_running = true;

    while (s_task_running) {
        // ... existing wait and work logic ...

        // After processing, check if all refreshes are complete
        xSemaphoreTake(state->mutex, portMAX_DELAY);
        int pending_idx = find_next_pending_refresh(state);
        
        if (pending_idx < 0 && state->channel_count > 0) {
            // All channels done - check if we should schedule next cycle
            time_t now = time(NULL);
            
            if (s_last_full_refresh_complete == 0) {
                // First time completing all refreshes
                s_last_full_refresh_complete = now;
                ESP_LOGI(TAG, "All channels refreshed. Next refresh in %d seconds.", REFRESH_INTERVAL_SECONDS);
            } else if (now - s_last_full_refresh_complete >= REFRESH_INTERVAL_SECONDS) {
                // Time for periodic refresh
                ESP_LOGI(TAG, "Starting periodic refresh cycle (1 hour elapsed)");
                for (size_t i = 0; i < state->channel_count; i++) {
                    state->channels[i].refresh_pending = true;
                }
                s_last_full_refresh_complete = 0;  // Reset to track next completion
                xSemaphoreGive(state->mutex);
                ps_refresh_signal_work();
                continue;  // Skip the give below, we already did it
            }
        }
        xSemaphoreGive(state->mutex);
        
        // ... rest of loop ...
    }
}
```

Also reset `s_last_full_refresh_complete = 0` when a new scheduler command is executed (in `play_scheduler.c:play_scheduler_execute_command()`), so that a new command triggers immediate refresh and resets the 1-hour timer.

**Files to modify**:
- `components/play_scheduler/play_scheduler_refresh.c` - Add periodic timer logic
- `components/play_scheduler/play_scheduler.c` - Reset timer on new command

---

### Task 4: Delete play_navigator.c and Related References

**Problem**: `play_navigator.c` contains deprecated concepts (p/q indices, order modes, playlist expansion). It should be deleted. Live Mode is also deprecated but should be preserved as comments elsewhere.

**Required Actions**:

1. **Delete these files**:
   - `components/channel_manager/play_navigator.c`
   - `components/channel_manager/include/play_navigator.h`

2. **Remove from CMakeLists.txt**:
   - `components/channel_manager/CMakeLists.txt` - Remove `play_navigator.c` from SRCS

3. **Update files that include play_navigator.h** - Remove the include and any usage. Search for:
   ```
   #include "play_navigator.h"
   ```

4. **Preserve Live Mode notes** - Create a comment block in `components/play_scheduler/play_scheduler.c` or a `DEFERRED_FEATURES.md` file:
   ```c
   /*
    * DEFERRED: Live Mode Synchronized Playback
    * 
    * Live Mode was a feature for synchronized playback across multiple devices.
    * Key concepts that were in play_navigator.c:
    * - live_mode flag on navigator
    * - live_p/live_q arrays for flattened schedule
    * - play_navigator_set_live_mode() to enable
    * - play_navigator_mark_live_dirty() when schedule needs rebuild
    * 
    * When implementing Live Mode in Play Scheduler:
    * - Add live_mode flag to ps_state_t
    * - Use SNTP time sync for coordination
    * - Build flattened schedule from lookahead
    * - Calculate start_time_ms and start_frame for swap requests
    * 
    * See docs/LIVE_MODE_ANALYSIS.md for full analysis.
    */
   ```

**Files to modify/delete**:
- DELETE: `components/channel_manager/play_navigator.c`
- DELETE: `components/channel_manager/include/play_navigator.h`
- MODIFY: `components/channel_manager/CMakeLists.txt`
- MODIFY: Any files that include `play_navigator.h`

---

### Task 5: Remove All channel_player References

**Problem**: `channel_player.c` and `channel_player.h` were already deleted, but references may remain in other files.

**Required Actions**:

1. **Search for all references**:
   ```
   grep -r "channel_player" components/
   grep -r "channel_player" main/
   ```

2. **Remove all occurrences**:
   - Remove `#include` statements for `channel_player.h`
   - Remove function calls to `channel_player_*` APIs
   - Remove any comments referencing channel_player (unless historical context is valuable)

3. **Key files likely affected** (based on PROGRESS.md mentioning 18 files with 160+ calls):
   - `components/makapix/makapix.c`
   - `components/http_api/http_api_rest.c` - Already updated but verify
   - `main/p3a_main.c` - Already updated but verify
   - Any other files found by grep

4. **If code relied on channel_player**, replace with Play Scheduler equivalents:
   - `channel_player_next()` → `play_scheduler_next()`
   - `channel_player_prev()` → `play_scheduler_prev()`
   - `channel_player_play_channel()` → `play_scheduler_play_named_channel()`

**Files to search and modify**: All files in `components/` and `main/`

---

### Task 6: Update makapix.c to Remove channel_player Calls

**Problem**: `makapix.c` likely still has references to the old channel_player system.

**Required Actions**:

1. **Search `components/makapix/makapix.c`** for:
   - `channel_player`
   - `play_channel`
   - Any navigation functions that should use Play Scheduler

2. **Replace with Play Scheduler equivalents**

3. **Review `makapix_request_channel_switch()`** - This function exists and is called by Play Scheduler refresh. After Task 1 is complete, assess if this function is still needed or can be simplified/removed.

**Files to modify**:
- `components/makapix/makapix.c`
- `components/makapix/makapix.h` (if declarations need updating)

---

### Task 7: Verify Boot Message Flow

**Problem**: Need to confirm the boot message state machine shows correct progression.

**Expected Flow**:
1. Boot → "Connecting to Makapix Club" message (during WiFi/MQTT wait)
2. MQTT connected → "Refreshing channel index" message  
3. First cache arrives → "Downloading artwork" message
4. First file downloaded → Playback begins, messages clear

**Required Actions**:

1. **Trace the message calls** - Search for `p3a_render_set_channel_message` calls
2. **Verify state transitions** occur in correct order
3. **Check edge cases**:
   - SD card channel (no MQTT needed)
   - No WiFi configured
   - All files already cached

**Files to review**:
- `components/channel_manager/download_manager.c` - Has some messages
- `components/channel_manager/makapix_channel_refresh.c` - Has some messages
- `components/play_scheduler/play_scheduler_refresh.c` - May need messages
- `main/p3a_main.c` - Boot sequence

---

## Important Notes

### What NOT to Do (Deferred Features)

- **NAE MQTT Connection** - Do NOT implement. The NAE pool exists but wiring it to MQTT is deferred.
- **Multi-channel HTTP API** - Do NOT extend POST /channel to accept arrays. Deferred.
- **Performance Profiling** - Deferred.
- **Memory Optimization** - Deferred.
- **Live Mode** - Deferred (but preserve notes per Task 4).

### Build Command

```powershell
cd "D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo"
. "C:\Users\Fab\esp\v5.5.1\esp-idf\export.ps1"
idf.py build
```

### Key Files Reference

| File | Purpose |
|------|---------|
| `components/play_scheduler/play_scheduler.c` | Core scheduler, batch generation |
| `components/play_scheduler/play_scheduler_refresh.c` | Background refresh task |
| `components/play_scheduler/play_scheduler_swrr.c` | SWRR algorithm |
| `components/play_scheduler/play_scheduler_pick.c` | Artwork picking |
| `components/play_scheduler/include/play_scheduler.h` | Public API |
| `components/play_scheduler/include/play_scheduler_internal.h` | Internal state |
| `components/makapix/makapix.c` | Makapix integration |
| `components/makapix/makapix_channel_refresh.c` | Index refresh logic |
| `components/channel_manager/download_manager.c` | Download management |
| `components/http_api/http_api_rest.c` | HTTP API handlers |

### Documentation Reference

- `docs/play-scheduler/SPECIFICATION.md` - Original specification
- `docs/play-scheduler/IMPLEMENTATION_PLAN.md` - Architecture details
- `docs/play-scheduler/DECISIONS.md` - Design decisions (31 numbered)
- `docs/play-scheduler/PROGRESS.md` - Task status
- `MIGRATION_AND_QA.md` - Migration analysis (in repo root)

---

## Success Criteria

After completing all tasks:

1. ✅ `idf.py build` succeeds with no errors
2. ✅ No references to `channel_player` remain in codebase
3. ✅ No references to `play_navigator` remain in codebase (files deleted)
4. ✅ `ps_generate_batch()` uses `ps_swrr_select_channel()` for channel selection
5. ✅ Background refresh re-triggers every 1 hour after initial completion
6. ✅ Makapix has a clean `makapix_refresh_channel_index()` API that doesn't involve channel switching
7. ✅ Boot messages flow correctly through connection → refresh → download → playback

---

## Order of Implementation

Recommended order to minimize conflicts:

1. **Task 4** - Delete play_navigator.c (clears out deprecated code first)
2. **Task 5** - Remove channel_player references (continues cleanup)
3. **Task 6** - Update makapix.c (part of cleanup)
4. **Task 1** - Create dedicated refresh API (enables Task 3)
5. **Task 2** - Implement SWRR usage (independent improvement)
6. **Task 3** - Add 1-hour periodic refresh (uses Task 1's new API)
7. **Task 7** - Verify boot messages (final verification)

---

*End of Implementation Task Document*

