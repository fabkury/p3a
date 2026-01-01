# Play Scheduler — Implementation Progress

This document tracks the implementation progress of the Play Scheduler component.

---

## Status Overview

| Phase | Status | Started | Completed |
|-------|--------|---------|-----------|
| Phase 1: Core Infrastructure | 🟡 Not Started | - | - |
| Phase 2: Single-Channel Mode | 🟡 Not Started | - | - |
| Phase 3: Multi-Channel Support | 🟡 Not Started | - | - |
| Phase 4: NAE Integration | 🟡 Not Started | - | - |
| Phase 5: Auto-Swap Timer | 🟡 Not Started | - | - |
| Phase 6: RandomPick Mode | 🟡 Not Started | - | - |
| Phase 7: Cleanup & Polish | 🟡 Not Started | - | - |

**Legend**: 🟢 Complete | 🟡 Not Started | 🔵 In Progress | 🔴 Blocked

---

## Phase 1: Core Infrastructure

**Goal**: Create component skeleton with basic types and initialization.

### Tasks

- [ ] Create `components/play_scheduler/` directory
- [ ] Create `CMakeLists.txt`
- [ ] Create `Kconfig` with configuration options
- [ ] Create `include/play_scheduler.h` (public API)
- [ ] Create `include/play_scheduler_types.h` (type definitions)
- [ ] Implement `play_scheduler_init()`
- [ ] Implement `play_scheduler_deinit()`
- [ ] Add component to main dependencies
- [ ] Verify compilation

### Notes

_None yet._

---

## Phase 2: Single-Channel Mode (N=1)

**Goal**: Replace channel_player for single-channel use case.

### Tasks

- [ ] Implement History buffer (ring buffer)
- [ ] Implement Lookahead buffer (FIFO queue)
- [ ] Implement `play_scheduler_play_channel()`
- [ ] Implement RecencyPick mode
- [ ] Implement `play_scheduler_next()`
- [ ] Implement `play_scheduler_prev()`
- [ ] Implement `play_scheduler_peek()`
- [ ] Implement `play_scheduler_current()`
- [ ] Integrate with `animation_player_request_swap()`
- [ ] Update `http_api.c` POST /channel
- [ ] Update `http_api.c` POST /next
- [ ] Update `http_api.c` POST /back
- [ ] Test: Switch to SD card channel
- [ ] Test: Switch to "all" channel
- [ ] Test: Switch to "promoted" channel
- [ ] Test: Next/prev navigation

### Notes

_None yet._

---

## Phase 3: Multi-Channel Support

**Goal**: Enable N > 1 with weighted scheduling.

### Tasks

- [ ] Implement per-channel state structure
- [ ] Implement SWRR scheduler
- [ ] Implement EqE weight calculation
- [ ] Implement MaE weight calculation (dummy weights)
- [ ] Implement PrE weight calculation
- [ ] Implement `play_scheduler_set_channels()` API
- [ ] Handle inactive channels (Mi = 0)
- [ ] Test: Equal weight distribution
- [ ] Test: Weight-based selection

### Notes

_None yet._

---

## Phase 4: NAE Integration

**Goal**: Enable real-time new artwork events.

### Tasks

- [ ] Implement NAE pool data structure
- [ ] Implement `nae_insert()` with merge logic
- [ ] Implement eviction on pool overflow
- [ ] Implement NAE selection probability
- [ ] Implement priority decay
- [ ] Implement `play_scheduler_nae_insert()` public API
- [ ] Implement `play_scheduler_set_nae_enabled()`
- [ ] Connect to MQTT handler
- [ ] Test: NAE insertion
- [ ] Test: NAE selection probability
- [ ] Test: Priority decay

### Notes

_None yet._

---

## Phase 5: Auto-Swap Timer

**Goal**: Port timer functionality from channel_player.

### Tasks

- [ ] Create timer task
- [ ] Implement dwell-based auto-swap
- [ ] Implement touch event flags
- [ ] Implement `play_scheduler_touch_next()`
- [ ] Implement `play_scheduler_touch_back()`
- [ ] Implement timer reset on manual nav
- [ ] Implement `play_scheduler_set_dwell_time()`
- [ ] Implement `play_scheduler_get_dwell_time()`
- [ ] Test: Auto-swap after dwell timeout
- [ ] Test: Timer reset on manual navigation

### Notes

_None yet._

---

## Phase 6: RandomPick Mode

**Goal**: Add random sampling pick mode.

### Tasks

- [ ] Implement RandomPick with configurable window R
- [ ] Add PRNG_randompick stream
- [ ] Implement resample-on-repeat logic
- [ ] Implement `play_scheduler_set_pick_mode()`
- [ ] Test: Random pick distribution
- [ ] Test: Repeat avoidance

### Notes

_None yet._

---

## Phase 7: Cleanup & Polish

**Goal**: Remove old code, optimize, document.

### Tasks

- [ ] Delete `channel_player.c`
- [ ] Delete `channel_player.h`
- [ ] Remove all `channel_player` references from codebase
- [ ] Update `p3a_main.c` initialization
- [ ] Update `makapix.c` channel switch
- [ ] Update any remaining callers
- [ ] Performance profiling
- [ ] Memory usage optimization
- [ ] Update AGENTS.md if needed
- [ ] Final code review

### Notes

_None yet._

---

## Issues & Blockers

_None currently._

---

## Change Log

| Date | Change |
|------|--------|
| 2026-01-01 | Initial progress document created |

---

*Update this document as implementation progresses.*

