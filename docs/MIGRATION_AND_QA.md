# p3a Migration Analysis: Channel-Based → Play Scheduler Architecture

**Date**: 2026-01-03  
**Status**: Active Migration - Phase 14 (Final Cleanup) Pending

---

## Executive Summary

The p3a codebase is in the midst of a significant architectural migration from a **channel-based playback system** (`channel_player`/`channel_navigator`) to a **Play Scheduler-based system** (`play_scheduler`). The Play Scheduler implementation is largely complete (Phases 1-13 done), but several integration gaps and legacy code remnants remain.

### Migration Status at a Glance

| Area | Status | Notes |
|------|--------|-------|
| Play Scheduler Core | ✅ Complete | History/Lookahead buffers, SWRR, Pick modes, NAE pool |
| Scheduler Commands | ✅ Complete | `play_scheduler_execute_command()`, convenience APIs |
| SD Card Index | ✅ Complete | `sdcard.bin` building, refresh on upload |
| Background Refresh | ⚠️ Partial | One-time refresh works; periodic 1-hour cycle unclear |
| Download Integration | ✅ Complete | Event-driven prefetch from lookahead |
| HTTP API | ✅ Complete | POST /channel uses scheduler commands |
| Legacy Code Removal | ❌ Pending | Phase 14 deferred |
| NAE MQTT Connection | ❌ Not Done | NAE pool exists but not fed by MQTT |
| Periodic Index Refresh | ⚠️ Fragmented | Lives in `makapix_channel_refresh.c`, not Play Scheduler |

---

## 1. Documentation Review (`docs/play-scheduler/`)

### 1.1 SPECIFICATION.md
- Defines the Play Scheduler as a **streaming generator** with History (H) and Lookahead (L) buffers
- Specifies **exposure modes**: EqE (Equal), MaE (Manual), PrE (Proportional with Recency)
- Specifies **pick modes**: RecencyPick, RandomPick
- Defines **NAE (New Artwork Events)** for responsive handling of newly published content
- Key constants: H=32, L=32, K=1024, J=32 (NAE pool)

### 1.2 IMPLEMENTATION_PLAN.md
- Comprehensive architecture for scheduler commands
- Channel types: NAMED, USER, HASHTAG, SDCARD
- Cache files at `/sdcard/p3a/channel/{channel_id}.bin`
- Sequential background refresh (one channel at a time)
- Lenient skip behavior (skip unavailable, never block)
- Event-driven download integration

### 1.3 PROGRESS.md
- **Phases 1-13**: Complete
- **Phase 14** (Final Cleanup): Deferred
  - Delete `channel_player.c/h`
  - Remove all `channel_player` references
  - Connect NAE to MQTT handler

### 1.4 DECISIONS.md
- 31 binding decisions documented
- Key decisions:
  - Decision #8: History preserved across commands
  - Decision #23: Lookahead flushed on new command
  - Decision #28: SD card refresh on switch and upload (not on boot)
  - Decision #29: `prev()` can navigate across command boundaries

---

## 2. Areas Needing Work or Rework

### 2.1 ❌ Periodic 1-Hour Index Refresh Not Integrated into Play Scheduler

**Current State:**
- `makapix_channel_refresh.c` has a refresh loop with `refresh_interval_sec` (default 1 hour)
- `play_scheduler_refresh.c` only handles one-time `refresh_pending` flags
- When a scheduler command is executed, channels are marked `refresh_pending = true` once
- After initial refresh completes, there's no mechanism to re-trigger refresh after 1 hour

**Problem:**
The user's requirement states: *"1 hour after finishing updating the indices of all channels, Play Scheduler initiates that process again, to stay updated."*

This is NOT happening in the current implementation. The Play Scheduler's refresh task processes pending flags but never sets them again.

**Location:** `components/play_scheduler/play_scheduler_refresh.c`

**Suggested Fix:**
Add a timer or timestamp check to re-set `refresh_pending = true` for all active channels after 1 hour:

```c
// Track when all channels finished refreshing
static time_t s_last_full_refresh_time = 0;

// In refresh_task loop, after all channels done:
if (find_next_pending_refresh(state) < 0) {
    // All channels refreshed
    time_t now = time(NULL);
    if (s_last_full_refresh_time == 0) {
        s_last_full_refresh_time = now;
    } else if (now - s_last_full_refresh_time >= 3600) {
        // 1 hour elapsed - re-queue all channels
        for (size_t i = 0; i < state->channel_count; i++) {
            state->channels[i].refresh_pending = true;
        }
        s_last_full_refresh_time = 0;  // Reset
        ps_refresh_signal_work();
    }
}
```

---

### 2.2 ⚠️ `ps_refresh_makapix_channel()` Uses Legacy Channel Switch Mechanism

**Current State:**
```c
// play_scheduler_refresh.c:101-109
makapix_request_channel_switch("all", NULL);
makapix_request_channel_switch("by_user", sqid);
makapix_request_channel_switch("hashtag", tag);
```

This calls into the legacy `makapix_request_channel_switch()` function, which is part of the old architecture.

**Problem:**
- Tight coupling between Play Scheduler and legacy channel system
- Doesn't directly query MQTT and update `.bin` files
- Relies on side effects from the old system

**Location:** `components/play_scheduler/play_scheduler_refresh.c` lines 99-113

**Suggested Fix:**
Refactor to directly call `makapix_api_query_posts()` and update the cache file, similar to what `makapix_channel_refresh.c:refresh_task_impl()` does. Alternatively, create a dedicated refresh API in the makapix component that doesn't involve "channel switching."

---

### 2.3 ❌ NAE Not Connected to MQTT Handler

**Current State:**
- `play_scheduler_nae_insert()` API exists
- NAE pool with priority decay is implemented
- But nothing calls `play_scheduler_nae_insert()` when new artwork notifications arrive via MQTT

**Problem:**
The NAE system is designed to give temporary exposure boost to newly published artworks. Without MQTT integration, this feature is dead code.

**Location:** Should be added to `components/makapix/makapix_mqtt.c` or similar

**Suggested Fix:**
When an MQTT notification for a new artwork arrives:
```c
// In MQTT handler for new artwork notifications
ps_artwork_t artwork = {0};
// ... fill artwork from notification ...
play_scheduler_nae_insert(&artwork);
```

---

### 2.4 ⚠️ `play_navigator.c` Still Present

**Current State:**
- `components/channel_manager/play_navigator.c` exists with full implementation
- Contains p/q index navigation, order modes, Live Mode schedule
- Used internally by channel implementations

**Assessment:**
This appears to be **internal infrastructure** used by `sdcard_channel_impl.c` and `makapix_channel_impl.c`, not the deprecated external API. The file header says: *"External code should use play_scheduler.h APIs for navigation."*

**Verdict:**
This is likely OK to keep as internal implementation detail. However, review if it's actually used by the new Play Scheduler flow or only by legacy code paths.

---

### 2.5 ⚠️ Legacy `channel_handle_t` Still Used in Play Scheduler

**Current State:**
```c
// play_scheduler_internal.h
channel_handle_t current_channel;

// play_scheduler.c:134
static esp_err_t load_channel_by_id(..., channel_handle_t *out_handle)
```

The Play Scheduler still creates and uses `channel_handle_t` objects via `sdcard_channel_create()` and `makapix_channel_create()`.

**Problem:**
This couples the Play Scheduler to the old channel abstraction layer.

**Assessment:**
This may be intentional for Phase 14 gradual migration. The Play Scheduler loads channels to access their index, but the actual picking logic (`play_scheduler_pick.c`) reads directly from `ch->entries` (the loaded `makapix_channel_entry_t` array), not through the channel interface.

**Suggested Action:**
Low priority. Can be cleaned up after Phase 14 if desired.

---

### 2.6 ❌ Multi-Channel SWRR Selection Partially Used

**Current State:**
```c
// play_scheduler.c:255-261 in ps_generate_batch()
// For single-channel mode (N=1), just pick from the first active channel
// Multi-channel SWRR will be added in Phase 3
for (size_t i = 0; i < state->channel_count && !found; i++) {
    if (!state->channels[i].active) continue;
    found = ps_pick_artwork(state, i, &candidate);
}
```

The code iterates linearly through channels rather than using SWRR selection.

**Problem:**
The SWRR algorithm (`ps_swrr_select_channel()`) exists in `play_scheduler_swrr.c` but isn't called during batch generation.

**Location:** `components/play_scheduler/play_scheduler.c` lines 251-277

**Suggested Fix:**
Replace the linear iteration with SWRR selection:
```c
void ps_generate_batch(ps_state_t *state) {
    for (int b = 0; b < PS_LOOKAHEAD_SIZE; b++) {
        ps_artwork_t candidate;
        bool found = false;
        
        // Try NAE first (if enabled)
        if (state->nae_enabled && ps_nae_try_select(state, &candidate)) {
            found = true;
        }
        
        // Use SWRR to select channel
        if (!found) {
            int ch_idx = ps_swrr_select_channel(state);
            if (ch_idx >= 0) {
                found = ps_pick_artwork(state, ch_idx, &candidate);
            }
        }
        
        // ... rest of logic ...
    }
}
```

---

### 2.7 ⚠️ Status Messages During Boot Could Be More Informative

**Current State:**
Messages exist in `download_manager.c` and `makapix_channel_refresh.c`:
- "Downloading artwork..."
- "Updating channel index..."

**User's Desired Behavior:**
> *"No artworks available: display informative messages with current status until at least 1 artwork has become available to play"*
> *"Statuses: Connecting to Makapix Club (for Makapix Club channels) → Refreshing channel index → Downloading artwork"*

**Assessment:**
The state machine for boot messages exists but may not be complete. Need to verify:
1. "Connecting to Makapix Club" message is shown during MQTT wait
2. State transitions are clear and sequential

**Location:** Various - `download_manager.c`, `makapix_channel_refresh.c`, needs audit

---

### 2.8 ❌ Phase 14 Tasks Pending

Per `PROGRESS.md`, these tasks are deferred:

- [ ] Delete `channel_player.c` and `channel_player.h` - **NOTE: These files appear to already be deleted**
- [ ] Update `makapix.c` to remove channel_player calls
- [ ] Update remaining callers (18 files identified)
- [ ] Connect NAE to MQTT handler
- [ ] Performance profiling
- [ ] Memory usage optimization

---

## 3. Open Questions and Suggested Answers

### Q1: Where should the 1-hour periodic refresh timer live?

**Context:** The spec says Play Scheduler should re-trigger index refresh 1 hour after completing the initial refresh cycle.

**Options:**
1. **In `play_scheduler_refresh.c`** - Add timer logic to the existing refresh task
2. **In `makapix_channel_refresh.c`** - Keep existing behavior, integrate with PS
3. **New timer component** - Separate timer that signals PS

**Suggested Answer:** Option 1 - Add to `play_scheduler_refresh.c`. The refresh task already runs continuously and can track elapsed time since last full refresh. This keeps all scheduling logic in the Play Scheduler component.

---

### Q2: Should `play_navigator.c` be deleted as part of migration?

**Context:** The file exists and contains navigation logic, but Play Scheduler has its own navigation.

**Options:**
1. **Delete it** - If Play Scheduler completely replaces its functionality
2. **Keep it** - If it's used as internal implementation by channel modules
3. **Refactor** - Extract reusable parts, delete the rest

**Suggested Answer:** Option 2 - Keep it for now. The file serves as internal infrastructure for channel implementations. External code uses Play Scheduler APIs, but internally channels may still need p/q navigation for their own state management. Review actual usage before deletion.

---

### Q3: How should NAE insertion from MQTT work?

**Context:** NAE pool exists but isn't fed by MQTT notifications.

**Suggested Answer:**
1. In the MQTT message handler (likely in `makapix_mqtt.c`), detect new artwork notifications
2. Parse the notification to extract artwork metadata
3. Build a `ps_artwork_t` structure with available fields
4. Call `play_scheduler_nae_insert(&artwork)`
5. The NAE pool will handle priority, decay, and cap enforcement

Example integration point:
```c
// In MQTT callback for new artwork
if (msg_type == MQTT_NEW_ARTWORK_NOTIFICATION) {
    ps_artwork_t art = {0};
    art.artwork_id = notification->post_id;
    art.post_id = notification->post_id;
    // ... fill other fields ...
    play_scheduler_nae_insert(&art);
}
```

---

### Q4: What happens if all channels have no cache (fresh device)?

**Context:** On a fresh device with no `.bin` cache files, all channels have `weight=0`.

**Expected Behavior per Spec:**
> *"No artworks available: display informative messages with current status until at least 1 artwork has become available to play"*

**Current Behavior:**
- Channels without cache get `weight=0` and are excluded from SWRR
- If ALL channels have `weight=0`, `ps_generate_batch()` produces no items
- `play_scheduler_next()` shows "No Artworks" message

**Suggested Answer:** Current behavior is correct, but verify the message flow:
1. Boot → "Connecting to Makapix Club" message
2. MQTT connected → "Refreshing channel index" message
3. First cache arrives → "Downloading artwork" message
4. First file downloaded → Playback begins, messages clear

---

### Q5: Should download continue when switching channels?

**Context:** Decision #21 says "No (let complete)" - in-flight downloads should complete.

**Current Implementation:** `download_manager.c` doesn't cancel downloads on channel switch.

**Suggested Answer:** ✅ Current implementation is correct. Downloaded files may be useful later if user switches back. The lookahead is flushed, but the physical file persists in the vault.

---

### Q6: How to handle channels_set with multiple channels?

**Context:** The user mentioned `channels_set = C1, C2, C3... Cn` but the HTTP API currently only supports single channel switching.

**Suggested Answer:** 
The infrastructure exists (`ps_scheduler_command_t` supports up to 64 channels), but the HTTP API (`POST /channel`) only builds single-channel commands. To support multi-channel:

1. Extend HTTP API to accept array of channels
2. Build `ps_scheduler_command_t` with multiple `ps_channel_spec_t` entries
3. Call `play_scheduler_execute_command()`

Example new API:
```json
POST /channel
{
  "channels": [
    {"name": "all"},
    {"name": "sdcard"}
  ],
  "exposure_mode": "equal"
}
```

---

### Q7: Is the current SWRR implementation being used?

**Context:** Code exists but batch generation uses linear iteration.

**Suggested Answer:** Currently NO - the SWRR selection is not called during batch generation. This should be fixed (see Section 2.6). The weights are calculated but `ps_swrr_select_channel()` isn't invoked in the hot path.

---

### Q8: What's the relationship between Play Scheduler and makapix_channel_refresh?

**Context:** Two refresh mechanisms exist:
1. `play_scheduler_refresh.c` - One-time refresh via `refresh_pending` flag
2. `makapix_channel_refresh.c` - Continuous refresh with 1-hour interval

**Suggested Answer:** This is a **transitional state**. The long-term architecture should have Play Scheduler own all refresh scheduling:

**Current (Fragmented):**
- Play Scheduler triggers initial refresh
- makapix_channel_refresh handles ongoing refresh

**Target (Unified):**
- Play Scheduler owns refresh scheduling (including 1-hour cycle)
- makapix_channel provides refresh execution (MQTT query + cache update)
- Clean separation: PS schedules, makapix executes

---

## 4. Recommended Next Steps

### Immediate Priority (Critical Path)

1. **Fix SWRR usage in batch generation** - Replace linear iteration with `ps_swrr_select_channel()`
2. **Add 1-hour periodic refresh** - Track time since last full refresh, re-queue all channels
3. **Connect NAE to MQTT** - Wire up new artwork notifications

### Medium Priority (Phase 14 Completion)

4. **Audit `makapix_request_channel_switch` usage** - Consider direct MQTT query for refresh
5. **Verify boot message state machine** - Ensure proper transitions
6. **Performance profiling** - Measure SD I/O, PSRAM usage

### Low Priority (Polish)

7. **Multi-channel HTTP API** - Extend POST /channel for arrays
8. **Clean up channel_handle_t usage** - Optional, after everything works
9. **Documentation update** - Reflect actual implementation state

---

## 5. Code Health Summary

| Component | Health | Issues |
|-----------|--------|--------|
| `play_scheduler.c` | 🟡 | SWRR not used in generation |
| `play_scheduler_buffers.c` | 🟢 | Working correctly |
| `play_scheduler_pick.c` | 🟢 | Working correctly |
| `play_scheduler_swrr.c` | 🟡 | Implemented but unused |
| `play_scheduler_nae.c` | 🟡 | Implemented but not fed |
| `play_scheduler_timer.c` | 🟢 | Working correctly |
| `play_scheduler_cache.c` | 🟢 | Working correctly |
| `play_scheduler_refresh.c` | 🟡 | Missing periodic refresh |
| `download_manager.c` | 🟢 | Working correctly |
| `http_api_rest.c` | 🟢 | Using new APIs |

**Legend:** 🟢 Good | 🟡 Needs Attention | 🔴 Broken

---

*Document generated by analysis of p3a codebase on 2026-01-03*

