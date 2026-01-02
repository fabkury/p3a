# Play Scheduler — Implementation Progress

This document tracks the implementation progress of the Play Scheduler component.

---

## Status Overview

| Phase | Status | Started | Completed |
|-------|--------|---------|-----------|
| Phase 1: Core Infrastructure | 🟢 Complete | 2026-01-01 | 2026-01-01 |
| Phase 2: Single-Channel Mode | 🟢 Complete | 2026-01-01 | 2026-01-01 |
| Phase 3: Multi-Channel Support | 🟢 Complete | 2026-01-01 | 2026-01-01 |
| Phase 4: NAE Integration | 🟢 Complete | 2026-01-01 | 2026-01-01 |
| Phase 5: Auto-Swap Timer | 🟢 Complete | 2026-01-01 | 2026-01-01 |
| Phase 6: RandomPick Mode | 🟢 Complete | 2026-01-01 | 2026-01-01 |
| Phase 7: Cleanup & Polish | 🔵 Partial | 2026-01-01 | - |
| **Phase 8: Channel Architecture Alignment** | 🟢 Complete | 2026-01-01 | 2026-01-01 |
| **Phase 9: SD Card Index Building** | 🟢 Complete | 2026-01-01 | 2026-01-01 |
| **Phase 10: Background Refresh Task** | 🟢 Complete | 2026-01-02 | 2026-01-02 |
| **Phase 11: Lenient Skip Behavior** | 🟢 Complete | 2026-01-02 | 2026-01-02 |
| **Phase 12: Download Integration** | 🟢 Complete | 2026-01-02 | 2026-01-02 |
| **Phase 13: Cache LRU** | 🟢 Complete | 2026-01-02 | 2026-01-02 |
| **Phase 14: Final Cleanup** | 🔵 Deferred | - | - |

**Legend**: 🟢 Complete | 🟡 Not Started | 🔵 In Progress / Partial | 🔴 Blocked

---

## Phase 1-6: Foundation (Complete)

These phases established the core Play Scheduler infrastructure:

- ✅ Component skeleton with types and init
- ✅ History and Lookahead buffers
- ✅ RecencyPick and RandomPick modes
- ✅ SWRR channel selection (EqE, MaE, PrE)
- ✅ NAE pool with priority decay
- ✅ Auto-swap timer task

**Files Created**:
- `components/play_scheduler/CMakeLists.txt`
- `components/play_scheduler/Kconfig`
- `components/play_scheduler/include/play_scheduler.h`
- `components/play_scheduler/include/play_scheduler_types.h`
- `components/play_scheduler/include/play_scheduler_internal.h`
- `components/play_scheduler/play_scheduler.c`
- `components/play_scheduler/play_scheduler_buffers.c`
- `components/play_scheduler/play_scheduler_pick.c`
- `components/play_scheduler/play_scheduler_swrr.c`
- `components/play_scheduler/play_scheduler_nae.c`
- `components/play_scheduler/play_scheduler_timer.c`

---

## Phase 7: Cleanup & Polish (Partial)

**Goal**: Remove old code, update call sites.

### Completed Tasks

- [x] Update `http_api_rest.c` for navigation (next/back)
- [x] Update `p3a_main.c` to call `play_scheduler_init()`
- [x] Project builds successfully

### Remaining Tasks (Deferred to Phase 14)

- [ ] Delete `channel_player.c` and `channel_player.h`
- [ ] Remove all `channel_player` references from `makapix.c`
- [ ] Update remaining callers
- [ ] Connect NAE to MQTT handler

**Notes**: These tasks are deferred until after the new channel architecture is implemented. The channel_player is still used for some channel switching logic.

---

## Phase 8: Channel Architecture Alignment 🟢

**Goal**: Align codebase with new channel specification structure.

### Completed Tasks

- [x] Define `ps_channel_type_t` enum (NAMED, USER, HASHTAG, SDCARD)
- [x] Define `ps_channel_spec_t` struct (type, name, identifier, weight)
- [x] Define `ps_scheduler_command_t` struct (channels, count, exposure, pick)
- [x] Implement `play_scheduler_execute_command()` API
- [x] Implement `ps_build_channel_id()` helper (derives ID from spec)
- [x] Implement identifier sanitization (for filesystem safety)
- [x] Implement cache file path building
- [x] Add convenience functions:
  - [x] `play_scheduler_play_named_channel()`
  - [x] `play_scheduler_play_user_channel()`
  - [x] `play_scheduler_play_hashtag_channel()`
- [x] Update `http_api_rest.c` POST /channel to use scheduler commands

### Testing (Deferred to Manual Testing)

- [ ] Test: Execute command with single named channel
- [ ] Test: Execute command with user channel
- [ ] Test: Execute command with hashtag channel
- [ ] Test: Execute command with SD card channel

### Key Code Changes

**New in `play_scheduler_types.h`**:
```c
typedef enum {
    PS_CHANNEL_TYPE_NAMED,
    PS_CHANNEL_TYPE_USER,
    PS_CHANNEL_TYPE_HASHTAG,
    PS_CHANNEL_TYPE_SDCARD,
} ps_channel_type_t;

typedef struct {
    ps_channel_type_t type;
    char name[33];
    char identifier[33];
    uint32_t weight;
} ps_channel_spec_t;

typedef struct {
    ps_channel_spec_t channels[PS_MAX_CHANNELS];
    size_t channel_count;
    ps_exposure_mode_t exposure_mode;
    ps_pick_mode_t pick_mode;
} ps_scheduler_command_t;
```

---

## Phase 9: SD Card Index Building 🟢

**Goal**: SD card channel gets a `.bin` cache file like Makapix channels.

### Completed Tasks

- [x] Create `play_scheduler_cache.c`
- [x] Implement folder scan of `/sdcard/p3a/animations/`
- [x] Build `makapix_channel_entry_t` array from files
- [x] Write to `/sdcard/p3a/channel/sdcard.bin` with atomic write
- [x] Hook refresh trigger: `play_scheduler_refresh_sdcard_cache()` API
- [x] Hook refresh trigger: after HTTP file upload in `http_api_upload.c`
- [x] Add `ps_touch_cache_file()` for LRU tracking

### Testing (Deferred to Manual Testing)

- [ ] Test: Index built correctly from folder contents
- [ ] Test: Index updates after file upload

---

## Phase 10: Background Refresh Task 🟢

**Goal**: Sequential cache refresh for all channels in a command.

### Completed Tasks

- [x] Create `play_scheduler_refresh.c`
- [x] Implement background FreeRTOS task with event group
- [x] Implement refresh queue via `refresh_pending` flag on channels
- [x] Implement sequential refresh logic (one at a time)
- [x] Implement SD card refresh: calls `ps_build_sdcard_index()`
- [x] Implement Makapix refresh: triggers existing Makapix refresh
- [x] Update channel `active` flag when cache arrives
- [x] Recalculate SWRR weights after refresh completes
- [x] Hook into init/deinit to start/stop task
- [x] Signal work from `play_scheduler_execute_command()`

### Testing (Deferred to Manual Testing)

- [ ] Test: Background refresh completes for single channel
- [ ] Test: Background refresh completes for multiple channels
- [ ] Test: Newly cached entries become playable

---

## Phase 11: Lenient Skip Behavior 🟢

**Goal**: Gracefully skip unavailable artworks.

### Completed Tasks

- [x] Implement skip logic in `play_scheduler_next()`:
  - [x] If file exists → play it
  - [x] If 404 marker exists → remove from lookahead, try next
  - [x] If file not downloaded → rotate to end, try next
- [x] Implement `has_404_marker()` helper function
- [x] Implement `ps_lookahead_rotate()` buffer operation
- [x] Add safety limit to skip loop (`PS_LOOKAHEAD_SIZE` iterations)
- [x] SWRR already uses `weight=0` for channels without cache (Phase 8)

### Testing (Deferred to Manual Testing)

- [ ] Test: Skip works when file missing
- [ ] Test: 404 items removed permanently
- [ ] Test: Channel with no cache is skipped

---

## Phase 12: Download Integration 🟢

**Goal**: Event-driven lookahead prefetching.

### Completed Tasks

- [x] Add `ps_prefetch_request_t` type to play_scheduler.h
- [x] Add `play_scheduler_get_next_prefetch()` public API
- [x] Add `play_scheduler_signal_lookahead_changed()` function
- [x] Call signal after lookahead generation in `next()`
- [x] Call signal after skip rotation in `next()`
- [x] Update `download_manager.c` to try Play Scheduler first
- [x] Implement "find soonest item without local file" logic
- [x] Maintain backwards compatibility with legacy callback
- [x] 404 marker creation already handled by existing download_manager

### Testing (Deferred to Manual Testing)

- [ ] Test: Download triggered after lookahead fill
- [ ] Test: Download triggered after previous download completes
- [ ] Test: Soonest item downloaded first
- [ ] Test: 404 creates marker file

---

## Phase 13: Cache LRU 🟢

**Goal**: Best-effort LRU tracking for cache files.

### Completed Tasks

- [x] Implement `ps_touch_cache_file()` (done in Phase 9)
- [x] Call touch on cache file access in `ps_load_channel_cache()`

### Notes

The touch function uses a simple fopen/fclose approach which updates mtime on most filesystems.
Manual cleanup can be done by sorting cache files by mtime and removing oldest ones.

---

## Phase 14: Final Cleanup 🔵

**Goal**: Remove legacy code, polish, document.

**Status**: Deferred pending testing of Phases 8-13. The channel_player is still integrated
in 18 files with 160+ function calls. This cleanup should only proceed after confirming
the new Play Scheduler architecture works correctly.

### Tasks

- [ ] Delete `components/channel_manager/channel_player.c`
- [ ] Delete `components/channel_manager/include/channel_player.h`
- [ ] Update `makapix.c` to remove channel_player calls
- [ ] Update any remaining callers (18 files)
- [ ] Connect NAE to MQTT handler for real-time insertion
- [ ] Update `AGENTS.md` if needed
- [ ] Performance profiling
- [ ] Memory usage optimization
- [ ] Final integration testing

### Dependencies

Phase 14 requires that Phases 8-13 work correctly. Test the following before proceeding:
1. Channel switching via POST /channel API
2. SD card channel playback
3. Background refresh completion
4. Lenient skip behavior
5. Download prefetch from lookahead

---

## Issues & Blockers

_None currently._

---

## Files to Create (Phases 8-14)

| File | Phase | Purpose |
|------|-------|---------|
| `play_scheduler_cache.c` | 9 | SD card index building |
| `play_scheduler_refresh.c` | 10 | Background refresh task |

---

## Files to Modify (Phases 8-14)

| File | Phase | Change |
|------|-------|--------|
| `play_scheduler_types.h` | 8 | Add channel spec, command types |
| `play_scheduler.h` | 8 | Add execute_command API |
| `play_scheduler.c` | 8, 10, 11 | Command handling, skip logic |
| `http_api_rest.c` | 8 | Build scheduler commands |
| `download_manager.c` | 12 | Event-driven prefetch |
| `channel_player.c` | 14 | DELETE |
| `channel_player.h` | 14 | DELETE |
| `makapix.c` | 14 | Remove channel_player refs |

---

## Testing Checklist (Phases 8-14)

### Channel Architecture Tests
- [ ] Single named channel command (e.g., "all")
- [ ] User channel command (e.g., "user:uvz")
- [ ] Hashtag channel command (e.g., "hashtag:sunset")
- [ ] SD card channel command
- [ ] Multi-channel command

### Cache Tests
- [ ] SD card index built from folder
- [ ] Index updates after file upload
- [ ] Makapix cache created from MQTT

### Skip Behavior Tests
- [ ] File missing → skipped
- [ ] 404 marker → removed
- [ ] Channel no cache → weight=0

### Download Tests
- [ ] Prefetch triggered on lookahead fill
- [ ] Prefetch triggered on download complete
- [ ] 404 creates marker file

### Integration Tests
- [ ] End-to-end: switch channel → plays immediately
- [ ] End-to-end: background refresh completes
- [ ] End-to-end: prefetch downloads upcoming artworks

---

## Change Log

| Date | Change |
|------|--------|
| 2026-01-01 | Initial progress document |
| 2026-01-01 | Phases 1-6 implemented |
| 2026-01-01 | Added Phases 8-14 for channel architecture |
| 2026-01-01 | Detailed task breakdowns for new phases |
| 2026-01-01 | Phase 8 complete: types, execute_command, http_api updated |
| 2026-01-02 | Phase 9 complete: play_scheduler_cache.c, sdcard.bin index |
| 2026-01-02 | Phase 10 complete: play_scheduler_refresh.c, background refresh task |
| 2026-01-02 | Phase 11 complete: lenient skip behavior, 404 markers, rotate |
| 2026-01-02 | Phase 12 complete: download integration, prefetch API |
| 2026-01-02 | Phase 13 complete: cache LRU mtime touch |
| 2026-01-02 | Phase 14 deferred: awaiting testing of Phases 8-13 |

---

*Update this document as implementation progresses.*
