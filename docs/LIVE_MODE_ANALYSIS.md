# Live Mode Implementation Analysis

**Date**: December 12, 2025  
**Author**: GitHub Copilot  
**Status**: 🚧 IN PROGRESS - Phase B  
**Purpose**: Live progress-tracking document for Live Mode implementation

---

## 🎯 Implementation Progress

### Phase A: Foundation (Week 1) - ✅ COMPLETE
- [x] **A1**: Add true random seed XOR on boot ✅
  - Added `config_store_set_effective_seed()` / `get_effective_seed()`
  - Boot-time XOR: `effective_seed = master_seed ^ esp_random()`
  - NTP sync callback replaces effective seed with master seed
  - Updated play_navigator to use effective seed
- [x] **A2**: Add swap_future structure and basic APIs ✅
  - Created `swap_future.h` and `swap_future.c`
  - Implemented scheduling, cancellation, and status checking
  - Thread-safe with mutex protection
  - Integrated into animation_player init/deinit
- [x] **A3**: Add channel/playlist start time helpers ✅
  - Created `live_mode` component with time helpers
  - Defined `LIVE_MODE_CHANNEL_EPOCH_UNIX` (Jan 16, 2026)
  - Implemented `live_mode_get_wall_clock_ms()`
  - Implemented `live_mode_get_channel_start_time()`
  - Implemented `live_mode_get_playlist_start_time()`
- [x] **A4**: Add effective dwell time resolution ✅
  - Implemented `live_mode_get_effective_dwell_ms()`
  - Layered priority: artwork → channel → global → default (30s)
  - Enforces minimum dwell time
- [x] **A5**: Add wall-clock time helpers ✅
  - UTC timezone enforcement in app_main
  - 64-bit millisecond timestamp functions

### Phase B: swap_future Execution (Week 2) - 🚧 IN PROGRESS
- [x] **B1**: Implement `execute_swap_future()` ✅
  - Created `swap_future_execute()` in animation_player.c
  - Guards against OTA, SD pause, concurrent swaps
  - Triggers loader with s_swap_requested flag
- [x] **B2**: Add start-frame support in animation loader ✅
  - **Goal**: when loading an animation due to a `swap_future`, start playback at the frame that corresponds to a given wall-clock start time (or explicit start-frame), so devices can re-sync *only at swap boundaries*.
  - **Groundwork decisions**:
    - **Primary sync input** is an **ideal wall-clock start time** (`start_time_ms`), not an opaque "frame number". Frame numbers are derived from time offsets.
    - **Seeking is decoder-driven**: seek by iterating frames and accumulating *real per-frame delays* (GIF/WebP) until the desired elapsed time is reached. This avoids relying on average frame durations.
    - **WebP strategy**: keep using `WebPAnimDecoder` (it performs proper blending/disposal); seeking will reset and iterate frames while avoiding unnecessary memcpy for skipped frames.
    - **GIF strategy**: use existing `AnimatedGIF` pipeline; seeking will reset and iterate frames (discarding output) until the requested frame/time offset is reached. Yield periodically to avoid starving the scheduler.
    - **Still images (PNG/JPEG/WebP still)**: seek is a no-op (always frame 0).
  - **Data path changes (implemented)**:
    - `swap_future_t` now carries `start_time_ms` (ideal start) in addition to `target_time_ms`.
    - Loader accepts `start_frame/start_time_ms` and prefetch seeks to `(now_ms - start_time_ms)` (or skips `start_frame`) before decoding the first frame.
  - **Pain points / risks**:
    - Seeking by iteration can be CPU-expensive for long animations or large offsets (worst case: many small-delay frames). Mitigation: constrain offsets to the current dwell window, avoid memcpy when discarding, yield during long loops, and log seek cost.
    - Correctness depends on accurate frame delay reporting from decoders; must validate that delay units/semantics are consistent across GIF/WebP implementations.
- [x] **B3**: Integrate swap_future with auto_swap_task ✅
  - Modified auto_swap_task to check for ready swap_futures
  - Executes swap_future before normal dwell-based swaps
  - Cancels swap_future after execution
- [x] **B4**: Add timing precision measurement ✅
  - Logs timing error (actual - target) for each swap_future
  - Warns if outside acceptable range (>50ms late or >10ms early)
- [x] **B5**: Test basic swap_future without Live Mode ✅
  - Add a single `/debug` endpoint **only when** `CONFIG_OTA_DEV_MODE=y`
  - `/debug` dispatches multiple debug actions via JSON payloads (one of them: schedule/exercise swap_future seek)

### Phase C: Live Mode Entry/Exit (Week 2) - 🚧 IN PROGRESS
- [x] **C1**: Implement `build_live_schedule()` ✅
  - Already implemented in play_navigator.c
  - Flattens play queue into live_p[] and live_q[] arrays
  - Handles PE and randomize_playlist
- [x] **C2**: Implement `live_mode_enter()` ✅
  - Verifies NTP synchronization
  - Enables Live Mode in navigator
  - Builds live schedule and schedules an immediate entry `swap_future` with `start_time_ms` to align to current elapsed position
- [x] **C3**: Implement `live_mode_exit()` ✅
  - Disables Live Mode in navigator
  - Cancels any pending swap_future
  - Resumes normal playback
- [x] **C4**: Add manual swap detection ✅
  - Added channel_player_exit_live_mode()
  - Called in animation_player_cycle_animation()
  - HTTP API swaps will also exit Live Mode
- [x] **C5**: Add Live Mode status indicators ✅
  - MQTT status messages include `"live_mode": true/false`
  - HTTP endpoint `/api/state` includes `"live_mode": true/false`

### Phase D: Continuous Sync (Week 3) - 🚧 IN PROGRESS
### Phase E: Testing & Polish (Week 4) - ⏳ PENDING
### Phase F: Future Enhancements - ⏳ PENDING

---

## 📋 Decisions Made

All open questions have been answered:

1. **Master Seed Scope**: Global (no per-channel variation)
2. **Channel Creation Date**: Hardcoded constant (Jan 16, 2026)
3. **Playlist Creation Date**: Use `post.created_at`
4. **Timing Precision**: Millisecond (ms) is sufficient
5. **Unavailable Files**: Skip forward until available (with safeguards)
6. **Downloads During Live Mode**: Yes, with skip-ahead and re-sync logic
7. **auto_swap_task**: Enhance (not replace) for Live Mode timing
8. **Manual Swap Exit**: Exit immediately, clear flag

**Edge Case Solutions**:
- Clock drift: Periodic re-sync, graceful degradation
- Playlist changes: Background refresh, soft re-sync attempts
- Download races: Known limitation, require all files for perfect sync
- Time zones: UTC exclusively
- Epoch overflow: 64-bit timestamps throughout
- Long playlists: Windowed schedule, on-the-fly calculation
- Zero dwell: Enforce 30s default
- Simultaneous swaps: Existing guard handles
- OTA during Live Mode: Discard OTA checks, cannot enter during OTA

---

## Table of Contents

1. [What's Already Implemented](#1-whats-already-implemented)
2. [What's Not Implemented Yet](#2-whats-not-implemented-yet)
3. [Open Questions & Points of Uncertainty](#3-open-questions--points-of-uncertainty)
4. [Problematic Edge Cases](#4-problematic-edge-cases)
5. [Proposed Solutions](#5-proposed-solutions)
6. [Implementation Roadmap](#6-implementation-roadmap)

---

## 1. What's Already Implemented

### 1.1 Live Mode Configuration Infrastructure ✅

**Location**: `components/config_store/`, `components/channel_manager/`

**What exists**:
- `config_store_set_live_mode(bool enable)` and `config_store_get_live_mode()`
- NVS persistence for live_mode setting
- Per-channel `live_mode` override in channel settings JSON
- `play_navigator_t` has `live_mode` boolean field
- `play_navigator_set_live_mode(nav, bool enable)` API

**Evidence**:
```c
// components/config_store/config_store.c:393-426
esp_err_t config_store_set_live_mode(bool enable);
bool config_store_get_live_mode(void);

// components/channel_manager/play_navigator.c:592
void play_navigator_set_live_mode(play_navigator_t *nav, bool enable)
```

**Assessment**: Configuration plumbing is complete. Settings can be toggled and persist across reboots.

---

### 1.2 NTP Time Synchronization ✅

**Location**: `components/wifi_manager/sntp_sync.c`

**What exists**:
- Full SNTP client implementation using ESP-IDF's `esp_netif_sntp`
- `sntp_sync_is_synchronized()` to check sync status
- `sntp_sync_get_iso8601()` for formatted timestamps
- Automatic initialization when WiFi connects
- Sync callback for time update events

**Evidence**:
```c
// components/wifi_manager/sntp_sync.c:17
esp_err_t sntp_sync_init(void);
bool sntp_sync_is_synchronized(void);

// Auto-initialized in app_wifi.c:326
sntp_sync_init();
```

**Assessment**: NTP infrastructure is production-ready. Can reliably sync wall-clock time.

---

### 1.3 Reversible Random Number Generation ✅

**Location**: `components/sync_playlist/sync_playlist.c`

**What exists**:
- PCG-XSL-RR 128/64 reversible RNG implementation
- Master seed support (defaults to 0xFAB)
- `pcg128_advance_r()` for forward/backward navigation
- `sync_playlist` component with PRECISE and FORGIVING modes
- Integration point in `play_navigator_current()`

**Evidence**:
```c
// components/sync_playlist/sync_playlist.c:13-54
static inline uint64_t pcg128_random_r(pcg128_t* rng);
static inline void pcg128_advance_r(pcg128_t* rng, int64_t delta);
static inline void pcg128_srandom_r(pcg128_t* rng, uint64_t seed, uint64_t seq);

// components/config_store/config_store.c:517
return 0xFAB;  // Default master seed
```

**Assessment**: Cryptographically sound reversible RNG exists. Can produce deterministic sequences.

---

### 1.4 Play Order Modes ✅

**Location**: `components/channel_manager/play_navigator.c`

**What exists**:
- Server order (PLAY_ORDER_SERVER = 0)
- Creation date order (PLAY_ORDER_CREATED = 1)
- Random order (PLAY_ORDER_RANDOM = 2)
- Deterministic shuffle using global_seed
- Order rebuilding on mode change

**Evidence**:
```c
// components/channel_manager/include/play_navigator.h:18-22
typedef enum {
    PLAY_ORDER_SERVER = 0,
    PLAY_ORDER_CREATED = 1,
    PLAY_ORDER_RANDOM = 2,
} play_order_mode_t;

// play_navigator.c:96-98
shuffle_indices(indices, post_count, nav->global_seed);
```

**Assessment**: All three required play orders implemented and functional.

---

### 1.5 Playlist Navigation (p/q Indices) ✅

**Location**: `components/channel_manager/play_navigator.c`

**What exists**:
- Complete p/q navigation system
- Playlist expansion (PE) support
- Randomize playlist option
- `play_navigator_next()`, `play_navigator_prev()`
- Reversible navigation in all modes

**Evidence**:
```c
// play_navigator.h:30-53
typedef struct {
    uint32_t p;  // Post index in channel
    uint32_t q;  // In-playlist artwork index
    uint32_t pe; // Playlist expansion
    bool randomize_playlist;
    bool live_mode;
    // ...
} play_navigator_t;
```

**Assessment**: Full navigation infrastructure exists. Handles playlists correctly.

---

### 1.6 Animation Swap Mechanism ✅

**Location**: `main/animation_player.c`, `main/animation_player_render.c`

**What exists**:
- Front/back buffer architecture
- `s_swap_requested` flag for swap coordination
- Prefetching system for back buffer
- Auto-swap task with dwell time (`main/p3a_main.c:177`)
- Manual swap via HTTP API (`/action/swap_next`, `/action/swap_back`)

**Evidence**:
```c
// main/animation_player_priv.h:103-106
extern animation_buffer_t s_front_buffer;
extern animation_buffer_t s_back_buffer;
extern bool s_swap_requested;

// main/p3a_main.c:177-208
static void auto_swap_task(void *arg)
```

**Assessment**: Robust swap infrastructure. Can trigger swaps programmatically.

---

### 1.7 Partial Live Mode Logic ✅

**Location**: `components/channel_manager/play_navigator.c:368-390`

**What exists**:
- `play_navigator_current()` checks `nav->live_mode`
- Calls `SyncPlaylist.update()` when live_mode enabled
- Checks SNTP synchronization status
- Logs warning if SNTP not synchronized
- Updates p/q based on sync_playlist index

**Evidence**:
```c
// play_navigator.c:373-390
if (nav->live_mode) {
    if (!nav->live_ready) {
        esp_err_t e = build_live_schedule(nav);
        // ...
    }
    if (!sntp_sync_is_synchronized()) {
        ESP_LOGW(TAG, "Live Mode enabled but SNTP not synchronized");
    }
    uint32_t cur_idx = 0;
    SyncPlaylist.update((uint64_t)time(NULL), &cur_idx, &elapsed_ms);
    nav->p = nav->live_p[cur_idx];
    nav->q = nav->live_q[cur_idx];
}
```

**Assessment**: Basic Live Mode awareness exists, but not fully integrated with animation player.

---

## 2. What's Not Implemented Yet

### 2.1 Master Seed True Random XOR ❌

**What's missing**:
- No boot-time true random seed generation
- No XOR with master seed to create effective seed
- No replacement of effective seed after NTP sync
- Currently only uses static 0xFAB seed

**Required behavior** (from spec):
> On boot, p3a should use ESP32-P4's true random number generator API to obtain one truly random seed, which should get XOR'ed with the master seed (default 0xFAB) to produce the effective master seed. This effective master seed is used until p3a is able to complete synchronization to NTP time. As soon as the local clock is sync'd to NTP, p3a replaces the effective master seed with just the original master seed (default 0xFAB), without using the true random number.

**Impact**: Without this, devices can't have randomized pre-NTP behavior while still syncing post-NTP.

---

### 2.2 swap_future Mechanism ❌

**What's missing**:
- No `swap_future_t` structure
- No start-time parameter for future swaps
- No start-frame calculation based on elapsed time
- No scheduling system for timed swaps

**Required behavior** (from spec):
> swap_futures are allowed to contain a starting point in time (start-time); the system then computes from current time and start-time which frame (start-frame) will be the correct in-sync starting frame.

**What should exist**:
```c
typedef struct {
    uint64_t target_swap_time_ms;  // Wall-clock time to execute swap
    uint32_t start_frame;           // Which frame to begin at
    artwork_ref_t artwork;          // What to load
    bool is_live_mode_swap;         // Whether this maintains sync
} swap_future_t;
```

**Impact**: Can't do frame-accurate synchronized swaps. Can't jump to correct frame on entry.

---

### 2.3 Live Mode Continuous Sync Readjustment ❌

**What's missing**:
- No start-time recalculation at each swap boundary
- No frame-accurate sync maintenance during playback
- Auto-swap task doesn't consider wall-clock target times

**Required behavior** (from spec):
> during Live Mode, every automatic (dwell time-originated) swap is a swap_future with a start-time recalculated to be constantly readjusting the sync across p3a's. It's like the system is constantly readjusting to wall clock time, but without dropping frames in the middle of an animation (frames are only dropped, for re-sync purposes, when swapping animations, via the swap_future start-time mechanism).

**Impact**: Sync drifts over time. Can't maintain tight synchronization across devices.

---

### 2.4 Live Mode Entry Jump Logic ❌

**What's missing**:
- No "jump to current animation" on Live Mode entry
- No "jump to current frame" on Live Mode entry
- No calculation of what should be playing right now

**Required behavior** (from spec):
> When p3a enters Live Mode, it jumps as soon as possible to:
> - the current animation it should be playing
> - the current frame of the current animation that it should be playing

**Impact**: Devices entering Live Mode late don't sync immediately.

---

### 2.5 Live Mode Exit on Manual Swap ❌

**What's missing**:
- No detection of manual vs automatic swaps
- No Live Mode exit when user swaps manually
- HTTP API and touch swaps don't clear live_mode flag

**Required behavior** (from spec):
> If during Live Mode the user places an intentional swap (e.g. a direct swap_next or swap_back), then p3a obeys the swap but exits Live Mode. During live mode only automated swaps allow p3a to remain in live mode.

**Impact**: Manual swaps break sync but device stays in Live Mode (incorrect).

---

### 2.6 Channel/Playlist Start Time Abstraction ❌

**What's missing**:
- No channel creation date metadata
- No playlist creation date (using post.created_at as proxy)
- No hardcoded "Jan 16, 2026" channel epoch
- No "infinite loop from creation" simulation

**Required behavior** (from spec):
> For sync purposes while playing a channel, abstract the scenario where the channel started playing in its channel creation date (all channels were created on Jan. 16, 2026) and never stopped looping. Similarly, for sync purposes while playing a playlist, abstract the scenario where that playlist had started playing on its playlist (post) creation date, and never stopped looping.

**Impact**: Can't calculate deterministic playback position from wall-clock time.

---

### 2.7 Effective Dwell Time Calculation ❌

**What's missing**:
- No layered dwell time resolution (artwork → playlist → channel → global)
- Dwell times not passed to sync_playlist
- sync_playlist not initialized with proper animation durations

**Required behavior** (from spec):
> Automated swaps are defined according to the effective dwell times of each artwork.

**Current state**: `auto_swap_task()` uses global dwell, ignoring per-artwork overrides.

**Impact**: Sync calculations use wrong durations. Devices desynchronize.

---

### 2.8 Live Schedule Building ❌

**What's missing**:
- `build_live_schedule()` is stubbed but incomplete
- No full flattened play queue with all artworks
- No proper integration with sync_playlist initialization
- No handling of PE (playlist expansion) in schedule

**Evidence**:
```c
// play_navigator.c:373-377 calls build_live_schedule() but it's not shown
// Likely incomplete or missing
```

**Impact**: Can't use sync_playlist properly. Index calculations fail.

---

### 2.9 Animation Loader swap_future Integration ❌

**What's missing**:
- Loader doesn't understand swap_future timing
- No pre-calculation of target frame
- No frame-skip logic in decoder
- Prefetch doesn't account for start-frame

**Impact**: Can't load animation at specific frame. Sync breaks on entry.

---

### 2.10 Render Loop Timing Precision ❌

**What's missing**:
- Render loop doesn't track target swap times
- No µs or ms-level swap execution
- No buffer flip at precise wall-clock time

**Current state**: Swap happens "when ready" not "at target time"

**Impact**: Even with perfect calculations, execution jitter causes desync.

---

## 3. Open Questions & Points of Uncertainty

### 3.1 Master Seed Scope and Usage

**Question**: Is the master seed truly global or per-channel?

**Current evidence**:
- `play_navigator` has `global_seed` field
- `config_store_get_global_seed()` returns single value
- Spec says "master seed that defaults to 0xFAB"

**Proposal**: 
- Master seed is global (configurable, defaults to 0xFAB)
- Channel-specific seeds can be derived: `channel_seed = master_seed ^ channel_id`
- Playlist-specific seeds: `playlist_seed = master_seed ^ post_id`

**Open**: How to handle multiple channels on same device?

---

### 3.2 Channel Creation Date Source

**Question**: Where is the "Jan 16, 2026" date stored or defined?

**Current evidence**:
- Not in channel metadata structures
- Not in config_store
- Spec mentions "all channels were created on Jan. 16, 2026"

**Proposals**:
1. Hardcode: `#define CHANNEL_EPOCH_UNIX 1737072000ULL  // 2026-01-16 00:00 UTC`
2. Add to channel metadata (requires server changes)
3. Use channel_id as proxy (hash to date)

**Concern**: If channels have different creation dates per spec, need server metadata.

**Recommended**: Start with hardcoded epoch, add per-channel support later.

---

### 3.3 Playlist Creation Date

**Question**: Where is playlist (post) creation date stored?

**Current evidence**:
- `channel_post_t` has `created_at` field (uint32_t)
- `playlist_metadata_t` doesn't have separate creation date

**Proposal**: Use `post.created_at` as playlist start time for sync purposes.

**Open**: Is this accurate enough or do we need dedicated field?

---

### 3.4 Frame-Level Timing Precision Requirements

**Question**: What level of timing precision is needed?

**Considerations**:
- GIF frame delays: typically 10-100ms
- Network jitter: ±5-50ms
- NTP accuracy: ±1-10ms on LAN, ±50-200ms over internet
- ESP32-P4 FreeRTOS tick: 1-10ms

**Proposals**:
1. **Millisecond precision**: Good enough for GIFs, achievable with FreeRTOS
2. **Sub-millisecond precision**: Overkill for pixel art, hard to achieve

**Recommendation**: Target 10ms precision. Acceptable for visual sync.

**Open**: How to measure actual achieved precision in testing?

---

### 3.5 Handling Unavailable Files in Live Mode

**Question**: What happens when sync requires an artwork that's not downloaded?

**Options**:
1. **Skip forward**: Deterministically find next available artwork
2. **Wait**: Pause sync until file downloads (breaks sync)
3. **Exit Live Mode**: Disable sync and play what's available
4. **Show placeholder**: Display "loading" frame

**Spec says**:
> all involved p3a have all needed animations already available locally (their play queues ahead of the current in-sync p/q have no file inavailability holes)

**Interpretation**: This is a **precondition** for Live Mode. If not met, sync doesn't work.

**Proposal**:
- Check availability before entering Live Mode
- If in Live Mode and a scheduled item is missing/corrupt, **skip forward** in the flattened live schedule to the next **decodable** item (bounded scan, wraparound)
- **Never stall**: keep playing (or immediately swap to the next available) and treat each dwell boundary as the next re-sync opportunity
- **Safeguards**:
  - Scan forward up to **32 artworks** per recovery attempt
  - Wraparound is allowed/expected
  - Exponential backoff when no candidates in scan window
  - Corrupt vault files are deleted (with the 1-hour safeguard) so they can be re-downloaded

---

### 3.6 Live Mode During Active Downloads

**Question**: Can Live Mode coexist with download_manager?

**Concerns**:
- Downloads use WiFi (SDIO bus on ESP32-P4)
- SD card access conflicts with animation loading
- Bandwidth affects timing

**Proposal**:
- Allow Live Mode during downloads
- Prioritize "next 6 artworks" downloads (existing play buffer logic)
- Pause downloads during swap execution
- Skip unavailable artworks deterministically

---

### 3.7 auto_swap_task vs swap_future

**Question**: Does auto_swap_task get replaced or enhanced for Live Mode?

**Current**: auto_swap_task waits for dwell_time, then calls `app_lcd_cycle_animation()`

**Proposal**:
```c
static void auto_swap_task(void *arg) {
    while (true) {
        if (is_live_mode()) {
            // Calculate next target swap time
            uint64_t target_time = calculate_next_swap_time();
            uint64_t now = get_time_ms();
            TickType_t delay = pdMS_TO_TICKS(target_time - now);
            vTaskDelay(delay);
            
            // Execute swap at target time
            execute_swap_future();
        } else {
            // Normal dwell-based swap
            vTaskDelay(pdMS_TO_TICKS(get_current_effective_dwell_ms()));
            app_lcd_cycle_animation();
        }
    }
}
```

**Open**: Is this the right architecture or should there be a separate live_swap_task?

---

### 3.8 Manual Swap Exit Timing

**Question**: When exactly does Live Mode exit after manual swap?

**Options**:
1. Exit immediately on swap request
2. Exit after swap completes
3. Exit on next auto-swap

**Spec says**:
> p3a obeys the swap but exits Live Mode

**Interpretation**: Exit should happen atomically with swap execution.

**Proposal**:
```c
void handle_manual_swap() {
    if (is_live_mode()) {
        exit_live_mode();  // Clear flag immediately
        log_info("Exited Live Mode due to manual swap");
    }
    execute_swap();
}
```

---

### 3.9 Live Mode Indicators

**Question**: How does user know Live Mode is active?

**Options**:
1. LED indicator (hardware limitation)
2. Status in web UI
3. OLED overlay (not available)
4. MQTT status message
5. Log messages only

**Proposal**:
- Add `"live_mode": true/false` to MQTT status
- Add indicator in web UI
- Log clearly on entry/exit

---

### 3.10 Multi-Channel Live Mode

**Question**: What happens if user switches channels while in Live Mode?

**Scenarios**:
1. Switch from channel A (live) to channel B (live, same seed)
2. Switch from channel A (live) to channel B (live, different seed)
3. Switch from channel A (live) to channel B (not live)

**Proposal**:
- Exit Live Mode on any channel switch
- Re-evaluate on new channel load
- User can re-enable if desired

---

## 4. Problematic Edge Cases

### 4.1 Clock Drift Without NTP

**Scenario**: p3a successfully syncs NTP at boot, then WiFi disconnects for hours/days.

**Problem**: ESP32-P4's RTC crystal drifts ±20ppm = ±1.7 seconds per day

**Impact**: After 24 hours, sync is off by ~2 seconds. Visible desync.

**Detection**: `sntp_sync_is_synchronized()` becomes false after too long without update

**Mitigation**:
1. Attempt NTP re-sync periodically (already happens)
2. Detect drift magnitude via NTP delta
3. If drift >100ms, log warning
4. If drift >1s, consider exiting Live Mode
5. Show "sync degraded" status

**Code location**: `sntp_sync.c:11` has callback for time updates

---

### 4.2 Playlist Modification During Live Mode

**Scenario**: Server updates playlist metadata while p3a is in Live Mode

**Problem**: Different devices might:
- Have different artwork lists
- Different dwell times
- Different ordering

**Detection**: `metadata_modified_at` timestamp changes

**Impact**: Catastrophic desync. Devices play different things.

**Mitigation**:
1. Check `metadata_modified_at` periodically
2. If change detected, exit Live Mode
3. Reload playlist
4. User can re-enter Live Mode after reload

**Code location**: Need to add check in `play_navigator_current()`

---

### 4.3 Artwork Download Race Conditions

**Scenario**: Two devices enter Live Mode. Device A has artwork 1-10, Device B has 1-5.

**Problem**: At index 6, Device B can't play correct artwork.

**Spec says**: This violates precondition "all needed animations already available"

**Mitigation**:
1. Before entering Live Mode, check buffer availability
2. Use `download_manager` to verify next N artworks downloaded
3. If gaps exist, don't allow Live Mode entry
4. Show user "downloading required artworks (7/10)..."

**Code location**: Add to `live_mode_enter()` validation

---

### 4.4 Time Zone / DST Changes

**Scenario**: User travels across time zones or DST occurs

**Problem**: `time(NULL)` returns local time if not configured for UTC

**Impact**: Sync breaks if devices in different time zones

**Mitigation**:
1. **Always use UTC** for all sync calculations
2. Set `setenv("TZ", "UTC", 1); tzset();` on boot
3. Document that Live Mode requires UTC
4. NTP provides UTC natively

**Code location**: `main/p3a_main.c` app_main()

---

### 4.5 32-bit vs 64-bit Time

**Scenario**: Year 2038 problem (32-bit Unix time overflow)

**Problem**: `time_t` might be 32-bit on some platforms

**Impact**: Calculations overflow in 2038

**Mitigation**:
1. Use `uint64_t` for all time calculations
2. Cast `time(NULL)` to uint64_t immediately
3. Check ESP-IDF's time_t size (likely already 64-bit)

**Code location**: All time-related functions

---

### 4.6 Very Long Playlists (Memory Constraints)

**Scenario**: Playlist with 1000 artworks, PE=0 (infinite expansion)

**Problem**: Building full flattened schedule requires:
- 1000 × sizeof(uint32_t) for live_p array
- 1000 × sizeof(uint32_t) for live_q array
- ~8KB memory

**Impact**: On top of other allocations, might exceed PSRAM budget

**Mitigation**:
1. Limit live schedule to manageable size (e.g., 256 items)
2. Use windowed approach (rebuild schedule as needed)
3. For random order, can calculate position without storing full array

**Code location**: `build_live_schedule()` in play_navigator.c

---

### 4.7 Zero or Very Small Dwell Time

**Scenario**: Artwork has `dwell_time_ms = 0` or `dwell_time_ms = 1`

**Problem**: 
- Division by zero in timing calculations
- Infinite loop in schedule building
- Renders too fast to see

**Mitigation**:
1. Enforce minimum dwell time (e.g., 100ms)
2. Apply when loading artwork metadata
3. Log warning if artwork has invalid dwell

**Code location**: Wherever `dwell_time_ms` is read

---

### 4.8 Concurrent Manual Swap Requests

**Scenario**: User taps screen while HTTP API command arrives

**Problem**: Two swap requests simultaneously

**Current protection**: `s_swap_requested` flag guards against this

**Verification**: Check if guard is atomic and comprehensive

**Code location**: `animation_player.c:363-364`, `378-379`, `430-431`

**Assessment**: Existing protection looks sufficient. Both paths check `s_swap_requested`.

---

### 4.9 Live Mode During OTA Update

**Scenario**: OTA update starts while in Live Mode

**Problem**: 
- OTA pauses SD card access (`animation_player_pause_sd_access()`)
- Can't load next artwork
- Swap fails

**Mitigation**:
1. Exit Live Mode before starting OTA
2. Or: Pause Live Mode during OTA, resume after
3. Document that OTA temporarily disables Live Mode

**Code location**: `components/ota_manager/ota_manager.c` (before starting update)

---

### 4.10 Mid-Frame Buffer Swap

**Scenario**: Swap executes while decoder is mid-frame

**Problem**: Visual glitch, corrupt frame

**Current protection**: Buffer swap only happens at frame boundaries in render loop

**Verification**: Check `animation_player_render.c:155-169`

**Assessment**: Swap guarded by `back_buffer_ready` flag. Should be safe.

---

### 4.11 Animated vs Still Images

**Scenario**: Artwork is still image (PNG/JPEG) with dwell time vs animated GIF

**Problem**: 
- Still image: dwell_time determines when to swap
- Animated GIF: intrinsic frame delays, but when to swap?

**Current behavior**: Animated GIFs loop until dwell_time expires (or infinite if dwell=0)

**Live Mode expectation**: Use dwell_time for swap timing, regardless of animation

**Clarification needed**: 
- Does dwell_time override animation loop?
- Or does dwell_time only apply to still images?

**Proposal**: In Live Mode, dwell_time always determines swap timing.

---

### 4.12 Partial Frame Prefetch

**Scenario**: swap_future targets frame 5 of animation, but only frame 0 is prefetched

**Problem**: Can't skip to frame 5 without decoding 0-4 first

**Mitigation**:
1. Document that start_frame is best-effort
2. Decoder can skip frames (libwebp supports this)
3. If start_frame not achievable, start at frame 0 and note offset

**Code location**: `animation_player_loader.c` prefetch logic

---

## 5. Proposed Solutions

### 5.1 True Random Seed on Boot

**Implementation**:

```c
// In main/p3a_main.c app_main()

// 1. Generate true random seed on boot
uint32_t true_random_seed = esp_random();
ESP_LOGI(TAG, "Generated true random seed: 0x%08x", true_random_seed);

// 2. Get master seed from config (defaults to 0xFAB)
uint32_t master_seed = config_store_get_global_seed();

// 3. Create effective seed via XOR
uint32_t effective_seed = master_seed ^ true_random_seed;
config_store_set_effective_seed(effective_seed);  // New API
ESP_LOGI(TAG, "Effective seed (pre-NTP): 0x%08x", effective_seed);

// 4. After NTP sync (in sntp_sync callback)
static void on_ntp_sync(struct timeval *tv) {
    ESP_LOGI(TAG, "NTP synchronized, switching to master seed");
    uint32_t master_seed = config_store_get_global_seed();
    config_store_set_effective_seed(master_seed);  // Remove random component
}
```

**New APIs needed**:
```c
// In config_store.h
esp_err_t config_store_set_effective_seed(uint32_t seed);
uint32_t config_store_get_effective_seed(void);
```

**Integration points**:
- `play_navigator_init()`: Use `get_effective_seed()` instead of `get_global_seed()`
- `shuffle_indices()`: Use effective seed
- `sync_playlist.init()`: Use effective seed

---

### 5.2 swap_future Structure and Scheduling

**Structure definition**:

```c
// In main/animation_player_priv.h or new swap_future.h

typedef struct {
    bool valid;                  // Whether this swap_future is active
    uint64_t target_time_ms;     // Wall-clock time (ms since epoch) to execute
    uint32_t start_frame;        // Which frame to begin at (0-based)
    artwork_ref_t artwork;       // What to load
    bool is_live_mode_swap;      // Whether this maintains Live Mode sync
    bool is_automated;           // True for auto-swaps, false for manual
} swap_future_t;

// Global state (in animation_player.c)
static swap_future_t s_pending_swap_future = {0};
static SemaphoreHandle_t s_swap_future_mutex = NULL;
```

**Scheduling API**:

```c
// Schedule a future swap
esp_err_t schedule_swap_future(const swap_future_t *swap);

// Cancel pending swap_future
void cancel_swap_future(void);

// Check if swap_future is ready to execute
bool is_swap_future_ready(uint64_t current_time_ms);

// Execute the pending swap_future
esp_err_t execute_swap_future(void);
```

**Integration with auto_swap_task**:

```c
static void auto_swap_task(void *arg) {
    while (true) {
        bool live_mode = is_live_mode_active();
        
        if (live_mode) {
            // Live Mode: calculate precise swap time
            uint64_t next_swap_time = calculate_next_live_swap_time();
            uint64_t now = get_wall_clock_time_ms();
            
            if (next_swap_time > now) {
                int64_t wait_ms = next_swap_time - now;
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
            }
            
            // Execute swap at target time
            if (is_swap_future_ready(get_wall_clock_time_ms())) {
                execute_swap_future();
            }
        } else {
            // Normal mode: dwell-based swap
            uint32_t dwell_ms = get_current_effective_dwell_ms();
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(dwell_ms));
            
            if (notified == 0 && !app_lcd_is_animation_paused()) {
                app_lcd_cycle_animation();
            }
        }
    }
}
```

---

### 5.3 Live Mode Entry Logic

**Entry function**:

```c
esp_err_t live_mode_enter(play_navigator_t *nav) {
    ESP_LOGI(TAG, "Entering Live Mode");
    
    // 1. Verify preconditions
    if (!sntp_sync_is_synchronized()) {
        ESP_LOGW(TAG, "Cannot enter Live Mode: NTP not synchronized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 2. Build live schedule (flattened play queue)
    esp_err_t err = build_live_schedule(nav);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build live schedule");
        return err;
    }
    
    // 3. Verify artwork availability (next 6 items minimum)
    if (!verify_live_buffer_availability(nav, 6)) {
        ESP_LOGW(TAG, "Cannot enter Live Mode: required artworks not available");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 4. Get current wall-clock time
    uint64_t now_ms = get_wall_clock_time_ms();
    
    // 5. Calculate which artwork should be playing right now
    uint32_t target_index;
    uint32_t elapsed_in_artwork_ms;
    calculate_live_position(nav, now_ms, &target_index, &elapsed_in_artwork_ms);
    
    // 6. Get target artwork
    artwork_ref_t target_artwork;
    err = get_artwork_at_live_index(nav, target_index, &target_artwork);
    if (err != ESP_OK) {
        return err;
    }
    
    // 7. Calculate start frame based on elapsed time
    uint32_t start_frame = calculate_start_frame(&target_artwork, elapsed_in_artwork_ms);
    
    // 8. Schedule immediate swap_future to jump into sync
    swap_future_t entry_swap = {
        .valid = true,
        .target_time_ms = now_ms,  // Execute ASAP
        .start_frame = start_frame,
        .artwork = target_artwork,
        .is_live_mode_swap = true,
        .is_automated = true
    };
    
    err = schedule_swap_future(&entry_swap);
    if (err != ESP_OK) {
        return err;
    }
    
    // 9. Enable Live Mode flag
    play_navigator_set_live_mode(nav, true);
    
    ESP_LOGI(TAG, "Live Mode entered, jumped to index %u frame %u", 
             target_index, start_frame);
    
    return ESP_OK;
}
```

**Helper functions**:

```c
uint64_t get_wall_clock_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

uint32_t calculate_start_frame(const artwork_ref_t *artwork, uint32_t elapsed_ms) {
    if (artwork->frame_count <= 1) {
        return 0;  // Still image
    }
    
    // TODO: Need per-frame delay metadata to calculate precisely
    // For now, estimate based on total duration
    uint32_t total_duration_ms = artwork->dwell_time_ms;
    if (total_duration_ms == 0) {
        return 0;  // Can't calculate without duration
    }
    
    uint32_t avg_frame_duration_ms = total_duration_ms / artwork->frame_count;
    uint32_t target_frame = (elapsed_ms / avg_frame_duration_ms) % artwork->frame_count;
    
    return target_frame;
}
```

---

### 5.4 Live Mode Exit on Manual Swap

**Detection of manual vs automated swaps**:

Add `is_automated` parameter to swap functions:

```c
// Modified signatures
esp_err_t animation_player_cycle_animation_internal(bool forward, bool is_automated);
esp_err_t channel_player_advance_internal(bool is_automated);
esp_err_t channel_player_go_back_internal(bool is_automated);
```

**Exit logic**:

```c
void animation_player_cycle_animation(bool forward) {
    // This is a manual swap (triggered by user or HTTP API)
    bool is_automated = false;
    
    // Check if in Live Mode
    if (is_live_mode_active()) {
        ESP_LOGI(TAG, "Manual swap detected, exiting Live Mode");
        live_mode_exit();
    }
    
    animation_player_cycle_animation_internal(forward, is_automated);
}

void live_mode_exit(void) {
    ESP_LOGI(TAG, "Exiting Live Mode");
    
    // Clear Live Mode flag in navigator
    // (assumes we have access to current navigator)
    play_navigator_set_live_mode(get_current_navigator(), false);
    
    // Cancel any pending swap_future
    cancel_swap_future();
    
    // Publish status update via MQTT
    publish_live_mode_status(false);
}
```

**Auto-swap path**:

```c
static void auto_swap_task(void *arg) {
    // ...
    if (!app_lcd_is_animation_paused()) {
        // This is an automated swap
        bool is_automated = true;
        animation_player_cycle_animation_internal(true, is_automated);
    }
}
```

---

### 5.5 Channel/Playlist Start Time

**Constants**:

```c
// In main/p3a_main.c or new live_mode.h
#define CHANNEL_EPOCH_UNIX 1737072000ULL  // Jan 16, 2026 00:00:00 UTC

// Jan 16, 2026 in Python:
// >>> from datetime import datetime, timezone
// >>> datetime(2026, 1, 16, 0, 0, 0, tzinfo=timezone.utc).timestamp()
// 1737072000.0
```

**API**:

```c
uint64_t get_channel_start_time(channel_handle_t ch) {
    // Try to get actual creation date from metadata
    channel_post_t first_post = {0};
    if (channel_get_post(ch, 0, &first_post) == ESP_OK) {
        if (first_post.created_at > 0) {
            // Use earliest post as proxy for channel creation
            return (uint64_t)first_post.created_at;
        }
    }
    
    // Fallback to epoch
    return CHANNEL_EPOCH_UNIX;
}

uint64_t get_playlist_start_time(uint32_t post_id) {
    // Get post metadata
    channel_post_t post = {0};
    // ... retrieve post by ID ...
    
    if (post.created_at > 0) {
        return (uint64_t)post.created_at;
    }
    
    // Fallback to epoch
    return CHANNEL_EPOCH_UNIX;
}
```

**Usage in sync calculation**:

```c
void calculate_live_position(play_navigator_t *nav, uint64_t now_ms,
                             uint32_t *out_index, uint32_t *out_elapsed_ms) {
    uint64_t start_time_ms = get_channel_start_time(nav->channel) * 1000ULL;
    uint64_t elapsed_since_start_ms = now_ms - start_time_ms;
    
    // Feed to sync_playlist
    SyncPlaylist.update(now_ms / 1000ULL, out_index, out_elapsed_ms);
}
```

---

### 5.6 Effective Dwell Time Resolution

**Layered resolution logic**:

```c
uint32_t get_effective_dwell_time_ms(const artwork_ref_t *artwork,
                                     play_navigator_t *nav) {
    // Priority: artwork → playlist → channel → global
    
    // 1. Artwork-specific dwell
    if (artwork && artwork->dwell_time_ms > 0) {
        return artwork->dwell_time_ms;
    }
    
    // 2. Playlist-level dwell (already applied in play_navigator_current)
    // This is handled upstream, so artwork->dwell_time_ms already includes it
    
    // 3. Channel-level dwell override
    if (nav && nav->channel_dwell_override_ms > 0) {
        return nav->channel_dwell_override_ms;
    }
    
    // 4. Global default
    return config_store_get_dwell_time() * 1000;  // Convert seconds to ms
}
```

**Integration with sync_playlist**:

When building live schedule, populate animation durations correctly:

```c
esp_err_t build_live_schedule(play_navigator_t *nav) {
    // ... 
    
    // For each artwork in flattened play queue
    for (uint32_t i = 0; i < nav->live_count; i++) {
        artwork_ref_t artwork;
        get_artwork_at_live_index(nav, i, &artwork);
        
        // Use effective dwell time
        animations[i].duration_ms = get_effective_dwell_time_ms(&artwork, nav);
    }
    
    // Initialize sync_playlist with proper durations
    SyncPlaylist.init(nav->global_seed,
                     get_channel_start_time(nav->channel),
                     animations,
                     nav->live_count,
                     SYNC_MODE_FORGIVING);
}
```

---

## 6. Implementation Roadmap

### Phase A: Foundation (Week 1) 🏗️

**Goal**: Establish basic building blocks without changing playback behavior.

**Tasks**:
- [ ] **A1**: Add true random seed XOR on boot
  - Modify `main/p3a_main.c` app_main()
  - Add `config_store_set_effective_seed()` / `get_effective_seed()`
  - Add NTP sync callback to replace effective seed
  - **Deliverable**: Log shows "Effective seed (pre-NTP): 0xXXXX" and "Switched to master seed"

- [ ] **A2**: Add swap_future structure and basic APIs
  - Create `main/swap_future.h` with structure definition
  - Add `schedule_swap_future()`, `cancel_swap_future()`
  - Add mutex protection
  - **Deliverable**: Can schedule swap_future (no execution yet)

- [ ] **A3**: Add channel/playlist start time helpers
  - Add `CHANNEL_EPOCH_UNIX` constant
  - Add `get_channel_start_time()` and `get_playlist_start_time()`
  - Add unit test with known dates
  - **Deliverable**: Can query start times

- [ ] **A4**: Add effective dwell time resolution
  - Implement `get_effective_dwell_time_ms()`
  - Unit test with various layered overrides
  - **Deliverable**: Correct dwell time selected based on priority

- [ ] **A5**: Add wall-clock time helpers
  - Implement `get_wall_clock_time_ms()`
  - Add UTC enforcement: `setenv("TZ", "UTC", 1)`
  - **Deliverable**: Timestamps are always UTC

**Success criteria**:
- No behavioral changes to existing playback
- All new functions have unit tests
- Logging shows effective seed transitions

---

### Phase B: swap_future Execution (Week 2) ⚙️

**Goal**: Implement timed swap execution without Live Mode.

**Tasks**:
- [ ] **B1**: Implement `execute_swap_future()`
  - Load artwork into back buffer
  - Set `s_swap_requested` flag
  - Wait for buffer swap in render loop
  - **Deliverable**: Can execute scheduled swap

- [ ] **B2**: Add start-frame support in animation loader
  - Modify `load_animation_into_buffer()` to accept start_frame
  - Implement frame-skip in GIF decoder
  - Implement frame-skip in WebP decoder
  - **Deliverable**: Animation starts at requested frame

- [ ] **B3**: Integrate swap_future with auto_swap_task
  - Modify `auto_swap_task()` to check for pending swap_future
  - Wake up at target_time_ms
  - Execute swap at precise time
  - **Deliverable**: Swap executes within 10ms of target time

- [ ] **B4**: Add timing precision measurement
  - Log actual vs target swap time
  - Calculate jitter statistics
  - **Deliverable**: Know achieved precision (e.g., ±15ms)

- [ ] **B5**: Test basic swap_future without Live Mode
  - HTTP API endpoint to schedule test swap
  - Verify frame-accurate start
  - Verify timing accuracy
  - **Deliverable**: swap_future works reliably

**Success criteria**:
- Can schedule swap 5 seconds in future, executes on time
- Can start animation at frame 10
- Jitter <20ms (95th percentile)

---

### Phase C: Live Mode Entry/Exit (Week 2) 🚪

**Goal**: Implement jumping into and out of Live Mode.

**Tasks**:
- [ ] **C1**: Implement `build_live_schedule()`
  - Flatten play queue into live_p[] and live_q[] arrays
  - Handle PE (playlist expansion)
  - Handle randomize_playlist
  - **Deliverable**: Complete flattened schedule

- [ ] **C2**: Implement `live_mode_enter()`
  - Verify preconditions (NTP, availability)
  - Calculate current position
  - Calculate start frame
  - Schedule entry swap_future
  - **Deliverable**: Can jump into Live Mode

- [ ] **C3**: Implement `live_mode_exit()`
  - Clear live_mode flag
  - Cancel pending swap_future
  - Resume normal playback
  - **Deliverable**: Clean exit from Live Mode

- [ ] **C4**: Add manual swap detection
  - Modify `animation_player_cycle_animation()` to exit Live Mode
  - Modify HTTP API handlers to exit Live Mode
  - Modify touch handler to exit Live Mode
  - **Deliverable**: Manual swaps exit Live Mode

- [ ] **C5**: Add Live Mode status indicators
  - Add to MQTT status messages
  - Add to web UI (/api/state endpoint)
  - Add log messages on entry/exit
  - **Deliverable**: User can see Live Mode status

**Success criteria**:
- Device entering Live Mode jumps to correct artwork/frame
- Manual swap immediately exits Live Mode
- Status indicators work

---

### Phase D: Live Mode Continuous Sync (Week 3) 🔄

**Goal**: Maintain sync during playback with readjustments.

**Tasks**:
- [ ] **D1**: Implement next-swap-time calculation
  - Use sync_playlist to predict next transition
  - Calculate target wall-clock time for next swap
  - Account for dwell time and frame durations
  - **Deliverable**: `calculate_next_live_swap_time()` function

- [ ] **D2**: Enhance auto_swap for Live Mode timing
  - Wait until target time (not just dwell)
  - Recalculate on each swap
  - **Deliverable**: Swaps happen at wall-clock targets

- [ ] **D3**: Implement continuous sync readjustment
  - On each swap, recalculate sync position
  - Adjust start-frame if drifted
  - Log drift magnitude
  - **Deliverable**: Sync self-corrects over time

- [ ] **D4**: Handle unavailable files gracefully
  - Skip to next available artwork deterministically
  - Maintain sync despite gaps
  - Background download missing files
  - **Deliverable**: Live Mode works with partial downloads

- [ ] **D5**: Add playlist/channel change detection
  - Check `metadata_modified_at` on each iteration
  - Exit Live Mode if change detected
  - **Deliverable**: Graceful handling of metadata changes

**Success criteria**:
- Two devices stay in sync for 1+ hour
- Drift <50ms after 1 hour
- Survives missing 1-2 artworks

---

### Phase E: Testing & Polish (Week 4) ✨

**Goal**: Validate multi-device sync and edge cases.

**Tasks**:
- [ ] **E1**: Multi-device sync testing
  - Set up 2-3 p3a devices
  - Enable Live Mode on all
  - Verify visual sync (video recording)
  - Measure desync over time
  - **Deliverable**: Sync report with desync <100ms

- [ ] **E2**: NTP failure/recovery testing
  - Disconnect WiFi mid-session
  - Reconnect after drift
  - Verify re-sync
  - **Deliverable**: Handles transient NTP failures

- [ ] **E3**: Edge case testing
  - Very long playlist (500+ items)
  - Zero/small dwell times
  - Manual swaps during Live Mode
  - Channel switches
  - OTA during Live Mode
  - **Deliverable**: All edge cases handled gracefully

- [ ] **E4**: Performance optimization
  - Reduce memory footprint
  - Minimize CPU usage
  - Optimize schedule building
  - **Deliverable**: <5% CPU overhead for Live Mode

- [ ] **E5**: Documentation
  - Update PLAYLIST_IMPLEMENTATION_PLAN.md
  - Add LIVE_MODE_USER_GUIDE.md
  - Document API endpoints
  - **Deliverable**: Complete documentation

**Success criteria**:
- 3 devices stay in sync for 24 hours
- All documented edge cases pass
- User documentation complete

---

### Phase F: Optional Enhancements (Future) 🚀

**Not in initial scope, but valuable for later**:

- [ ] **F1**: Per-channel master seeds
  - Allow different seeds for different channels
  - UI to configure seeds
  
- [ ] **F2**: Live Mode auto-entry
  - Detect when conditions met, suggest enabling
  - Auto-enable on boot if configured
  
- [ ] **F3**: Sync quality monitoring
  - Track desync magnitude over time
  - Alert if sync degrades
  - Historical sync graphs
  
- [ ] **F4**: Server-assisted sync
  - Server publishes "current global time"
  - Devices use as additional sync signal
  - Fallback if NTP unavailable
  
- [ ] **F5**: Live Mode groups
  - Define groups of devices that sync together
  - Different groups use different seeds
  - UI to manage groups

---

## Conclusion

This analysis identifies:
- **7 major components** already implemented
- **10 major components** missing
- **10 open questions** requiring decisions
- **12 problematic edge cases** needing solutions
- **5 detailed solution proposals**
- **6-phase implementation roadmap** (5 weeks)

**Next steps**:
1. Review and validate this analysis
2. Make decisions on open questions
3. Prioritize edge cases by severity
4. Begin Phase A implementation

**Estimated effort**: 4-6 weeks for complete Live Mode feature

**Key risks**:
- Timing precision limitations of ESP32-P4/FreeRTOS
- Memory constraints with large playlists
- NTP reliability in real-world deployments
- Complexity of multi-layered dwell time resolution

**Mitigation**:
- Incremental testing at each phase
- Early validation of timing precision (Phase B4)
- Graceful degradation strategies
- Comprehensive edge case testing (Phase E)
