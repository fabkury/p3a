# Play Scheduler — Implementation Task Brief

**For**: Implementation AI  
**Created**: 2026-01-01  
**Status**: Ready for Implementation (Phases 8-14)

---

## Summary

The Play Scheduler foundation (Phases 1-6) is complete. You need to implement Phases 8-14 to complete the channel architecture alignment. This document summarizes what you need to do.

**Reference documents**:
- `IMPLEMENTATION_PLAN.md` - Full architecture details
- `DECISIONS.md` - All design decisions (numbered 1-31)
- `PROGRESS.md` - Task checklists and status
- `SPECIFICATION.md` - Original specification

---

## Key Architecture Points

### Channel Types

There are 2 kinds of channels:

| Type | Storage | Source |
|------|---------|--------|
| **SD Card** | Local folder scan | `/sdcard/p3a/animations/` |
| **Makapix** | MQTT queries | Remote server |

Makapix has 3 subtypes:
- **Named**: `"all"`, `"featured"` 
- **User**: `"user:{sqid}"` (e.g., `"user:uvz"`)
- **Hashtag**: `"hashtag:{tag}"` (e.g., `"hashtag:sunset"`)

### Scheduler Commands

A **scheduler command** contains:
```c
typedef struct {
    ps_channel_spec_t channels[PS_MAX_CHANNELS];  // List of channels
    size_t channel_count;                          // 1 to 64
    ps_exposure_mode_t exposure_mode;              // EqE, MaE, or PrE
    ps_pick_mode_t pick_mode;                      // Recency or Random
} ps_scheduler_command_t;
```

Each channel spec:
```c
typedef struct {
    ps_channel_type_t type;    // NAMED, USER, HASHTAG, SDCARD
    char name[33];             // "all", "featured", "user", "hashtag", "sdcard"
    char identifier[33];       // For USER: sqid, For HASHTAG: tag
    uint32_t weight;           // For MaE mode
} ps_channel_spec_t;
```

### Cache Files

All channels have `.bin` cache files in `/sdcard/p3a/channel/`:
```
all.bin, featured.bin, user_uvz.bin, hashtag_sunset.bin, sdcard.bin
```

Format: Use existing `makapix_channel_entry_t` (64 bytes per entry).

### Core Behaviors

1. **Immediate Playback**: Use cached data immediately, refresh in background
2. **Sequential Refresh**: One channel at a time
3. **Lenient Skip**: Skip unavailable artworks, never block
4. **Event-Driven Downloads**: Prefetch based on lookahead changes

---

## What to Implement

### Phase 8: Channel Architecture Alignment

**Files to modify**: `play_scheduler_types.h`, `play_scheduler.h`, `play_scheduler.c`, `http_api_rest.c`

**Tasks**:
1. Add `ps_channel_type_t` enum: `NAMED`, `USER`, `HASHTAG`, `SDCARD`
2. Add `ps_channel_spec_t` struct with type, name, identifier, weight
3. Add `ps_scheduler_command_t` struct
4. Implement `play_scheduler_execute_command()`:
   - Flush lookahead (history preserved!)
   - Load existing `.bin` caches
   - Set `active=true` for cached channels, `weight=0` for uncached
   - Calculate SWRR weights
   - Start background refresh task
5. Implement `ps_build_channel_id()` helper
6. Add convenience functions: `play_scheduler_play_named_channel()`, etc.
7. Update `http_api_rest.c` POST /channel to build commands

### Phase 9: SD Card Index Building

**Files to create**: `play_scheduler_cache.c` (or `sdcard_channel_cache.c`)

**Tasks**:
1. Scan `/sdcard/p3a/animations/` for animation files
2. Build `makapix_channel_entry_t` array
3. Write to `/sdcard/p3a/channel/sdcard.bin`
4. Trigger refresh:
   - When user switches to SD card channel
   - After HTTP file upload

### Phase 10: Background Refresh Task

**Files to create**: `play_scheduler_refresh.c`

**Tasks**:
1. Create FreeRTOS task for background refresh
2. Refresh channels sequentially (one at a time)
3. For Makapix: MQTT `query_posts` → update `.bin`
4. For SD card: folder scan → update `sdcard.bin`
5. When cache arrives: set `active=true`, recalculate weights

### Phase 11: Lenient Skip Behavior

**Files to modify**: `play_scheduler.c`

**Tasks**:
1. In `next()`: if file doesn't exist, skip but keep in lookahead
2. If 404 marker exists: remove from lookahead permanently
3. Channel without cache: `weight=0` (excluded from SWRR)
4. Add safety limit to skip loop

### Phase 12: Download Integration

**Files to modify**: `play_scheduler.c`, `download_manager.c`

**Tasks**:
1. Add `play_scheduler_signal_lookahead_changed()` internal
2. Add `play_scheduler_get_next_prefetch()` public API
3. Signal after lookahead generation and after skip
4. Update download_manager to listen and trigger downloads
5. One download at a time, soonest item first
6. On 404: create marker file `{path}.404`

### Phase 13: Cache LRU

**Tasks**:
1. Touch cache file mtime on access
2. Document manual cleanup

### Phase 14: Final Cleanup

**Tasks**:
1. Delete `channel_player.c` and `channel_player.h`
2. Remove all channel_player references
3. Connect NAE to MQTT handler

---

## Key Decisions Reference

| # | Decision |
|---|----------|
| 8 | **History preserved** across scheduler commands |
| 23 | **Lookahead flushed** on new command |
| 24 | Channel without cache → **weight=0** |
| 25 | No local file → **skip during playback**, keep in lookahead |
| 26 | 404 → **remove permanently** (marker file) |
| 28 | SD card refresh: **(a) on switch, (c) on upload** |
| 29 | **prev() can go back** across command boundaries |

---

## Existing Files Reference

The Play Scheduler component already exists:

```
components/play_scheduler/
├── CMakeLists.txt
├── Kconfig
├── include/
│   ├── play_scheduler.h
│   ├── play_scheduler_types.h
│   └── play_scheduler_internal.h
├── play_scheduler.c              ← Modify for commands
├── play_scheduler_buffers.c      ← History/Lookahead (done)
├── play_scheduler_pick.c         ← Pick modes (done)
├── play_scheduler_swrr.c         ← SWRR (done)
├── play_scheduler_nae.c          ← NAE pool (done)
└── play_scheduler_timer.c        ← Auto-swap (done)
```

Files to create:
- `play_scheduler_cache.c` - SD card index building
- `play_scheduler_refresh.c` - Background refresh task

---

## Build Command

```powershell
cd "D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo"
. "C:\Users\Fab\esp\v5.5.1\esp-idf\export.ps1"
idf.py build
```

---

## Testing Priority

1. Single named channel command ("all")
2. SD card channel command
3. Skip behavior when file missing
4. Background refresh completes
5. Download prefetch triggered

---

*Start with Phase 8. Work through phases sequentially. Update PROGRESS.md as you complete tasks.*

