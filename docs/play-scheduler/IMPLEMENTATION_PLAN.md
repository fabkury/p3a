# Play Scheduler — Implementation Plan

**Status**: 🟡 Planning Complete, Implementation Pending  
**Last Updated**: 2026-01-01  
**Author**: AI Assistant + Human Review

---

## Table of Contents

1. [Overview](#1-overview)
2. [Design Decisions](#2-design-decisions)
3. [Architecture](#3-architecture)
4. [Component Breakdown](#4-component-breakdown)
5. [Data Structures](#5-data-structures)
6. [API Design](#6-api-design)
7. [Algorithm Details](#7-algorithm-details)
8. [Migration Strategy](#8-migration-strategy)
9. [Implementation Phases](#9-implementation-phases)
10. [Testing Strategy](#10-testing-strategy)
11. [Open Questions & Future Work](#11-open-questions--future-work)

---

## 1. Overview

### 1.1 Purpose

The **Play Scheduler (PS)** is a deterministic playback engine that replaces the existing `channel_player.c` component. It selects artworks from multiple followed channels for presentation on the p3a player device.

### 1.2 Key Design Principles

- **Virtual play queue**: Not materialized in full; uses streaming generation
- **Bounded memory**: History buffer (H) and Lookahead buffer (L)
- **Multi-channel fairness**: Weighted exposure across channels via SWRR
- **Responsiveness**: New Artwork Events (NAE) for newly published content
- **Determinism**: Reversible PRNGs enable reproducible sequences
- **Embedded-optimized**: Designed for SD + WiFi bus contention, PSRAM usage

### 1.3 What It Replaces

The Play Scheduler **completely replaces** `channel_player.c`, absorbing:
- Navigation logic (next/prev)
- Auto-swap timer
- Channel switching
- Play order management
- Live Mode integration (future)

---

## 2. Design Decisions

These decisions were made through Q&A discussion and are binding for implementation.

| # | Question | Decision |
|---|----------|----------|
| 1 | Relationship with `channel_player.c` | **Replace entirely** |
| 2 | Local Channel Cache format | **Use existing `makapix_channel_entry_t`** |
| 3 | Channel identification | **Use string identifiers** (`"all"`, `"promoted"`, `"sdcard"`) |
| 4 | NAE scope | **Global pool** across all channels |
| 5 | NAE persistence | **In-memory only** (reset on reboot) |
| 6 | MaE weights source | **Dummy/random weights** for now |
| 7 | PrE counts source | **Server `query_posts` response** (total_count, recent_count) |
| 8 | History on reset | **Preserved** across snapshot resets |
| 9 | SD card in PrE mode | **recent_count = 0** (no recency bias) |
| 10 | Playlist support | **Deferred** to later |
| 11 | Integration point | **Call `animation_player_request_swap()` directly** |
| 12 | Auto-swap timer | **Part of Play Scheduler** component |
| 13 | HTTP API | **Update `http_api.c`** to call new PS API |

---

## 3. Architecture

### 3.1 High-Level Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         Web UI / Touch                          │
└─────────────────────────────┬───────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                         http_api.c                              │
│  POST /channel  →  play_scheduler_set_channels()                │
│  POST /next     →  play_scheduler_next()                        │
│  POST /back     →  play_scheduler_prev()                        │
└─────────────────────────────┬───────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      PLAY SCHEDULER                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐             │
│  │   History   │  │  Generator  │  │  Lookahead  │             │
│  │  Buffer(H)  │◄─┤   Engine    ├─►│  Buffer(L)  │             │
│  └─────────────┘  └──────┬──────┘  └─────────────┘             │
│                          │                                      │
│  ┌───────────────────────┼───────────────────────┐             │
│  │                       ▼                       │             │
│  │  ┌─────────┐   ┌──────────────┐   ┌────────┐ │             │
│  │  │   NAE   │   │     SWRR     │   │  Pick  │ │             │
│  │  │  Pool   │   │  Scheduler   │   │ Modes  │ │             │
│  │  └─────────┘   └──────────────┘   └────────┘ │             │
│  └───────────────────────────────────────────────┘             │
│                          │                                      │
│  ┌───────────────────────┼───────────────────────┐             │
│  │            Per-Channel State (1..N)           │             │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐       │             │
│  │  │Channel 1│  │Channel 2│  │Channel N│       │             │
│  │  │ cursor  │  │ cursor  │  │ cursor  │       │             │
│  │  │ credit  │  │ credit  │  │ credit  │       │             │
│  │  │   Ei    │  │   Ei    │  │   Ei    │       │             │
│  │  └─────────┘  └─────────┘  └─────────┘       │             │
│  └───────────────────────────────────────────────┘             │
│                          │                                      │
│  ┌───────────────────────┼───────────────────────┐             │
│  │              Auto-Swap Timer Task             │             │
│  └───────────────────────────────────────────────┘             │
└─────────────────────────────┬───────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    animation_player.c                           │
│              animation_player_request_swap()                    │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Data Flow

```
User Action (next/touch/timer)
        │
        ▼
┌───────────────────┐
│  PS: next()       │
│  1. Check history │◄─── If walking forward through history, return cached
│  2. Check L < min │
│  3. Generate batch│◄─── If L low, generate L new items
│  4. Pop from L    │
│  5. Push to H     │
│  6. Return artwork│
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│ animation_player  │
│ _request_swap()   │
└───────────────────┘
```

### 3.3 File Layout

```
components/
└── play_scheduler/
    ├── CMakeLists.txt
    ├── Kconfig
    ├── include/
    │   ├── play_scheduler.h          # Public API
    │   ├── play_scheduler_types.h    # Data structures
    │   └── nae_pool.h                # NAE pool interface
    ├── play_scheduler.c              # Main implementation
    ├── play_scheduler_swrr.c         # SWRR algorithm
    ├── play_scheduler_pick.c         # Pick modes (Recency, Random)
    ├── play_scheduler_nae.c          # NAE pool management
    ├── play_scheduler_buffers.c      # History & Lookahead
    └── play_scheduler_timer.c        # Auto-swap timer task
```

---

## 4. Component Breakdown

### 4.1 Core Scheduler (`play_scheduler.c`)

**Responsibilities:**
- Initialize/deinitialize scheduler
- Manage channel set and weights
- Coordinate generation engine
- Expose public API: `next()`, `prev()`, `peek()`
- Handle snapshot resets
- Integrate with animation_player

**Key Functions:**
```c
esp_err_t play_scheduler_init(void);
void play_scheduler_deinit(void);
esp_err_t play_scheduler_set_channels(const ps_channel_config_t *channels, size_t count, ps_exposure_mode_t mode);
esp_err_t play_scheduler_next(ps_artwork_t *out);
esp_err_t play_scheduler_prev(ps_artwork_t *out);
esp_err_t play_scheduler_peek(size_t n, ps_artwork_t *out, size_t *out_count);
void play_scheduler_reset(void);
```

### 4.2 SWRR Scheduler (`play_scheduler_swrr.c`)

**Responsibilities:**
- Implement Smooth Weighted Round Robin algorithm
- Maintain per-channel credit counters
- Select next channel based on weights

**Algorithm:**
```
Wsum = 65536
credit[i] = 0 initially

For each step:
  1. credit[i] += W[i] for all i
  2. j = argmax(credit[i]), tie-break: lowest channel ID
  3. Emit channel j
  4. credit[j] -= Wsum
```

### 4.3 Pick Modes (`play_scheduler_pick.c`)

**Responsibilities:**
- RecencyPick: cursor-based newest→older traversal
- RandomPick: uniform sampling from newest R_eff records
- Immediate repeat avoidance (per-mode rules)

**RecencyPick:**
- Cursor starts at newest record
- Each pick: return at cursor, move toward older, wrap on exhaustion
- On immediate repeat: skip up to 2 records

**RandomPick:**
- R_eff = min(R, Mi) where R is configurable window
- Sample uniformly from newest R_eff records
- Uses `PRNG_randompick` stream
- On immediate repeat: up to 5 resample attempts

### 4.4 NAE Pool (`play_scheduler_nae.c`)

**Responsibilities:**
- Maintain priority queue of new artwork events
- Handle MQTT-triggered insertions
- Selection probability calculation
- Priority decay on selection

**Data Structure:**
```c
typedef struct {
    ps_artwork_t artwork;
    float priority;           // (0, 1]
    uint64_t insertion_time;  // For tie-breaking
} nae_entry_t;

#define NAE_POOL_SIZE 32
```

**Key Operations:**
- `nae_insert()`: Add/merge entry, evict lowest if over capacity
- `nae_select()`: Probabilistic selection based on Σpriorities
- `nae_decay()`: Halve priority, remove if < 2%

### 4.5 Buffers (`play_scheduler_buffers.c`)

**Responsibilities:**
- Manage History buffer (H) - ring buffer of committed items
- Manage Lookahead buffer (L) - queue of pre-generated items
- History navigation (prev/forward without mutation)

**History Buffer (H):**
- Default size: 32 items (configurable)
- Ring buffer implementation
- Tracks "current position" for prev/next within history

**Lookahead Buffer (L):**
- Default size: 32 items (configurable)
- FIFO queue
- Batch generation when size < L

### 4.6 Timer Task (`play_scheduler_timer.c`)

**Responsibilities:**
- Monitor dwell time
- Trigger auto-swap via `play_scheduler_next()`
- Handle touch event flags
- Reset on manual navigation

---

## 5. Data Structures

### 5.1 Artwork Reference

```c
typedef struct {
    int32_t artwork_id;           // Globally unique artwork ID
    int32_t post_id;              // Post ID for view tracking
    char filepath[256];           // Local path to file
    char storage_key[96];         // Vault storage key
    uint32_t created_at;          // Unix timestamp
    uint32_t dwell_time_ms;       // Per-artwork dwell (0 = use default)
    asset_type_t type;            // WEBP, GIF, PNG, JPEG
    uint8_t channel_index;        // Which channel this came from
} ps_artwork_t;
```

### 5.2 Channel Configuration

```c
typedef struct {
    char channel_id[64];          // "all", "promoted", "sdcard", etc.
    uint32_t weight;              // For MaE mode (0 = auto-calculate)
    uint32_t total_count;         // From server or local scan
    uint32_t recent_count;        // From server (0 for SD card)
} ps_channel_config_t;
```

### 5.3 Per-Channel State

```c
typedef struct {
    char channel_id[64];
    channel_handle_t handle;      // Existing channel_interface handle
    
    // SWRR state
    int32_t credit;
    uint32_t weight;              // Normalized weight (out of 65536)
    
    // Pick state
    uint32_t cursor;              // For RecencyPick
    pcg32_rng_t pick_rng;         // For RandomPick
    
    // Cache info
    size_t entry_count;           // Mi: local cache size
    bool active;                  // Has local data?
} ps_channel_state_t;
```

### 5.4 Scheduler State

```c
typedef struct {
    // Configuration
    ps_exposure_mode_t exposure_mode;
    ps_pick_mode_t pick_mode;
    uint32_t history_size;        // H
    uint32_t lookahead_size;      // L
    uint32_t random_window;       // R for RandomPick
    
    // Channels
    ps_channel_state_t channels[PS_MAX_CHANNELS];
    size_t channel_count;
    
    // Buffers
    ps_artwork_t *history;
    size_t history_head;
    size_t history_count;
    int32_t history_position;     // -1 = at head, 0+ = steps back
    
    ps_artwork_t *lookahead;
    size_t lookahead_head;
    size_t lookahead_tail;
    size_t lookahead_count;
    
    // NAE
    nae_entry_t nae_pool[NAE_POOL_SIZE];
    size_t nae_count;
    bool nae_enabled;
    
    // PRNGs
    pcg32_rng_t prng_nae;
    uint32_t global_seed;
    uint32_t epoch_id;
    
    // Repeat avoidance
    int32_t last_played_id;
    
    // Timer
    TaskHandle_t timer_task;
    uint32_t dwell_time_seconds;
    volatile bool touch_next;
    volatile bool touch_back;
    
    // Synchronization
    SemaphoreHandle_t mutex;
    bool initialized;
} ps_state_t;
```

---

## 6. API Design

### 6.1 Public API (`play_scheduler.h`)

```c
// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize the Play Scheduler
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_init(void);

/**
 * @brief Deinitialize and free all resources
 */
void play_scheduler_deinit(void);

// ============================================================================
// Channel Configuration
// ============================================================================

/**
 * @brief Exposure modes for channel weighting
 */
typedef enum {
    PS_EXPOSURE_EQUAL,        // EqE: Equal exposure
    PS_EXPOSURE_MANUAL,       // MaE: Manual weights
    PS_EXPOSURE_PROPORTIONAL, // PrE: Proportional with recency bias
} ps_exposure_mode_t;

/**
 * @brief Pick modes for per-channel artwork selection
 */
typedef enum {
    PS_PICK_RECENCY,          // Newest → older cursor
    PS_PICK_RANDOM,           // Random from window
} ps_pick_mode_t;

/**
 * @brief Set the active channel set and exposure mode
 * 
 * This rebuilds the play queue. History is preserved.
 * 
 * @param channels Array of channel configurations
 * @param count Number of channels (1-64)
 * @param mode Exposure mode
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_set_channels(
    const ps_channel_config_t *channels,
    size_t count,
    ps_exposure_mode_t mode
);

/**
 * @brief Set pick mode for per-channel selection
 * @param mode Pick mode
 */
void play_scheduler_set_pick_mode(ps_pick_mode_t mode);

// ============================================================================
// Navigation
// ============================================================================

/**
 * @brief Get the next artwork for playback
 * 
 * Advances playback position. Triggers generation if lookahead low.
 * Also calls animation_player_request_swap().
 * 
 * @param out_artwork Artwork reference (can be NULL if only triggering swap)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no artworks available
 */
esp_err_t play_scheduler_next(ps_artwork_t *out_artwork);

/**
 * @brief Go back to previous artwork
 * 
 * Only navigates within history buffer.
 * 
 * @param out_artwork Artwork reference
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if at history start
 */
esp_err_t play_scheduler_prev(ps_artwork_t *out_artwork);

/**
 * @brief Peek at upcoming artworks without advancing
 * 
 * Returns up to n items from lookahead. Does NOT trigger generation.
 * 
 * @param n Maximum items to return
 * @param out_artworks Array to receive artworks
 * @param out_count Actual count returned
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_peek(
    size_t n,
    ps_artwork_t *out_artworks,
    size_t *out_count
);

/**
 * @brief Get current artwork without navigation
 * @param out_artwork Artwork reference
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_current(ps_artwork_t *out_artwork);

// ============================================================================
// NAE (New Artwork Events)
// ============================================================================

/**
 * @brief Enable/disable NAE
 * @param enable true to enable
 */
void play_scheduler_set_nae_enabled(bool enable);

/**
 * @brief Insert a new artwork event (called from MQTT handler)
 * @param artwork Artwork reference
 */
void play_scheduler_nae_insert(const ps_artwork_t *artwork);

// ============================================================================
// Timer & Dwell
// ============================================================================

/**
 * @brief Set dwell time for auto-swap
 * @param seconds Dwell time (0 = disable auto-swap)
 */
void play_scheduler_set_dwell_time(uint32_t seconds);

/**
 * @brief Get current dwell time
 * @return Dwell time in seconds
 */
uint32_t play_scheduler_get_dwell_time(void);

/**
 * @brief Reset the auto-swap timer (called after manual navigation)
 */
void play_scheduler_reset_timer(void);

// ============================================================================
// Touch Events (lightweight signals from touch handler)
// ============================================================================

/**
 * @brief Signal touch-triggered next
 */
void play_scheduler_touch_next(void);

/**
 * @brief Signal touch-triggered back
 */
void play_scheduler_touch_back(void);

// ============================================================================
// Status & Debugging
// ============================================================================

/**
 * @brief Get scheduler statistics
 */
typedef struct {
    size_t channel_count;
    size_t history_count;
    size_t lookahead_count;
    size_t nae_pool_count;
    uint32_t epoch_id;
    const char *current_channel_id;
} ps_stats_t;

esp_err_t play_scheduler_get_stats(ps_stats_t *out_stats);
```

### 6.2 Convenience Functions

```c
/**
 * @brief Switch to a single channel (N=1 use case)
 * 
 * Convenience wrapper for play_scheduler_set_channels() with count=1.
 * 
 * @param channel_id Channel identifier
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_channel(const char *channel_id);
```

---

## 7. Algorithm Details

### 7.1 Exposure Weight Calculation

#### EqE (Equal Exposure)
```
For each active channel i:
    weight[i] = 1
For inactive channels:
    weight[i] = 0
Normalize: W[i] = weight[i] / Σweight * 65536
```

#### MaE (Manual Exposure)
```
weight[i] = max(0, manual_weight[i])
For inactive channels: weight[i] = 0
Normalize: W[i] = weight[i] / Σweight * 65536
```

#### PrE (Proportional Exposure with Recency Bias)
```
Parameters:
    α = 0.35 (recency blend factor)
    p_min = 0.02
    p_max = 0.40

1. Normalize totals:
   p_total[i] = total_count[i] / Σtotal_count
   p_recent[i] = recent_count[i] / Σrecent_count
   (if Σrecent_count = 0, all p_recent[i] = 0)

2. Blend:
   p_raw[i] = (1 - α) * p_total[i] + α * p_recent[i]

3. Clamp:
   p_clamped[i] = clamp(p_raw[i], p_min, p_max)

4. Normalize:
   W[i] = p_clamped[i] / Σp_clamped * 65536
```

### 7.2 SWRR Channel Selection

```c
int select_channel(void) {
    // Add credits
    for (int i = 0; i < channel_count; i++) {
        channels[i].credit += channels[i].weight;
    }
    
    // Find max credit (tie-break: lowest index)
    int best = -1;
    int32_t best_credit = INT32_MIN;
    for (int i = 0; i < channel_count; i++) {
        if (channels[i].active && channels[i].credit > best_credit) {
            best_credit = channels[i].credit;
            best = i;
        }
    }
    
    // Deduct
    if (best >= 0) {
        channels[best].credit -= WSUM;  // 65536
    }
    
    return best;
}
```

### 7.3 NAE Selection

```c
bool try_nae_select(ps_artwork_t *out) {
    if (!nae_enabled || nae_count == 0) return false;
    
    // Sum priorities
    float P = 0;
    for (int i = 0; i < nae_count; i++) {
        P += nae_pool[i].priority;
    }
    P = fminf(P, 1.0f);
    
    // Random check
    float r = pcg32_next_u32(&prng_nae) / (float)UINT32_MAX;
    if (r >= P) return false;
    
    // Select highest priority (tie: oldest insertion)
    int best = 0;
    for (int i = 1; i < nae_count; i++) {
        if (nae_pool[i].priority > nae_pool[best].priority ||
            (nae_pool[i].priority == nae_pool[best].priority &&
             nae_pool[i].insertion_time < nae_pool[best].insertion_time)) {
            best = i;
        }
    }
    
    *out = nae_pool[best].artwork;
    
    // Decay priority
    nae_pool[best].priority /= 2;
    if (nae_pool[best].priority < 0.02f) {
        // Remove entry
        nae_pool[best] = nae_pool[--nae_count];
    }
    
    return true;
}
```

### 7.4 Generation Batch

```c
void generate_batch(void) {
    for (int b = 0; b < L; b++) {
        ps_artwork_t candidate;
        
        // 1. Try NAE
        if (try_nae_select(&candidate)) {
            // Check repeat
            if (candidate.artwork_id == last_played_id) {
                // Fallback to base scheduler
                if (!select_from_base(&candidate)) {
                    // Accept repeat
                }
            }
        } else {
            // 2. Base scheduler
            int ch = select_channel();
            if (ch < 0) continue;  // No active channels
            
            pick_from_channel(ch, &candidate);
        }
        
        // 3. Repeat avoidance
        // (handled in pick_from_channel)
        
        // 4. Commit to lookahead
        lookahead_push(&candidate);
    }
}
```

---

## 8. Migration Strategy

### 8.1 Files to Modify

| File | Change |
|------|--------|
| `components/channel_manager/channel_player.c` | **DELETE** (replaced by play_scheduler) |
| `components/channel_manager/channel_player.h` | **DELETE** |
| `components/http_api/http_api.c` | Update to call play_scheduler APIs |
| `main/p3a_main.c` | Replace channel_player_init() with play_scheduler_init() |
| `main/animation_player.c` | Remove channel_player references |
| `components/makapix/makapix.c` | Update channel switch to use play_scheduler |

### 8.2 Files to Create

| File | Purpose |
|------|---------|
| `components/play_scheduler/CMakeLists.txt` | Component build |
| `components/play_scheduler/Kconfig` | Configuration options |
| `components/play_scheduler/include/play_scheduler.h` | Public API |
| `components/play_scheduler/include/play_scheduler_types.h` | Type definitions |
| `components/play_scheduler/play_scheduler.c` | Core implementation |
| `components/play_scheduler/play_scheduler_swrr.c` | SWRR algorithm |
| `components/play_scheduler/play_scheduler_pick.c` | Pick modes |
| `components/play_scheduler/play_scheduler_nae.c` | NAE pool |
| `components/play_scheduler/play_scheduler_buffers.c` | History & Lookahead |
| `components/play_scheduler/play_scheduler_timer.c` | Auto-swap timer |

### 8.3 Backward Compatibility

The following APIs must be preserved or have direct equivalents:

| Old API | New API |
|---------|---------|
| `channel_player_init()` | `play_scheduler_init()` |
| `channel_player_swap_next()` | `play_scheduler_next()` |
| `channel_player_swap_back()` | `play_scheduler_prev()` |
| `channel_player_set_dwell_time()` | `play_scheduler_set_dwell_time()` |
| `channel_player_switch_channel()` | `play_scheduler_play_channel()` |
| `auto_swap_reset_timer()` | `play_scheduler_reset_timer()` |

---

## 9. Implementation Phases

### Phase 1: Core Infrastructure ✅ Planning

**Goal**: Create component skeleton with basic types and initialization.

**Tasks**:
- [ ] Create `components/play_scheduler/` directory structure
- [ ] Write CMakeLists.txt and Kconfig
- [ ] Define all type definitions in `play_scheduler_types.h`
- [ ] Implement `play_scheduler_init()` / `deinit()`
- [ ] Add to main component dependencies

**Deliverable**: Component compiles, init/deinit work, no functional changes yet.

### Phase 2: Single-Channel Mode (N=1)

**Goal**: Replace channel_player for single-channel use case.

**Tasks**:
- [ ] Implement `play_scheduler_play_channel()` for N=1
- [ ] Implement History buffer (H)
- [ ] Implement Lookahead buffer (L)
- [ ] Implement RecencyPick mode
- [ ] Implement `next()`, `prev()`, `peek()`, `current()`
- [ ] Integrate with animation_player_request_swap()
- [ ] Update http_api.c to use play_scheduler

**Deliverable**: Web UI can switch channels, navigate artworks using PS.

### Phase 3: Multi-Channel Support

**Goal**: Enable N > 1 with weighted scheduling.

**Tasks**:
- [ ] Implement SWRR scheduler
- [ ] Implement EqE weight calculation
- [ ] Implement MaE weight calculation (dummy weights)
- [ ] Implement PrE weight calculation
- [ ] Add `play_scheduler_set_channels()` API
- [ ] Handle inactive channels (Mi = 0)

**Deliverable**: PS can play from multiple channels with fair weighting.

### Phase 4: NAE Integration

**Goal**: Enable real-time new artwork events.

**Tasks**:
- [ ] Implement NAE pool data structure
- [ ] Implement `nae_insert()` with merge logic
- [ ] Implement NAE selection probability
- [ ] Implement priority decay
- [ ] Connect to MQTT handler
- [ ] Add `play_scheduler_set_nae_enabled()`

**Deliverable**: New artworks from MQTT get probabilistic boost.

### Phase 5: Auto-Swap Timer

**Goal**: Port timer functionality from channel_player.

**Tasks**:
- [ ] Create timer task
- [ ] Implement dwell-based auto-swap
- [ ] Handle touch event flags
- [ ] Implement timer reset on manual nav

**Deliverable**: Auto-swap works as before.

### Phase 6: RandomPick Mode

**Goal**: Add random sampling pick mode.

**Tasks**:
- [ ] Implement RandomPick with configurable window R
- [ ] Add PRNG_randompick stream
- [ ] Implement resample-on-repeat logic
- [ ] Add `play_scheduler_set_pick_mode()`

**Deliverable**: Users can choose random vs recency pick.

### Phase 7: Cleanup & Polish

**Goal**: Remove old code, optimize, document.

**Tasks**:
- [ ] Delete channel_player.c/h
- [ ] Remove all channel_player references
- [ ] Update AGENTS.md / CLAUDE.md if needed
- [ ] Performance profiling
- [ ] Memory usage optimization

**Deliverable**: Clean codebase with only Play Scheduler.

---

## 10. Testing Strategy

### 10.1 Unit Tests (Host-based)

Since ESP-IDF supports Unity, create test cases for:

- SWRR weight distribution
- NAE pool insertion/eviction
- History buffer wraparound
- Lookahead generation batching
- Repeat avoidance logic

### 10.2 Integration Tests (On-device)

- Single channel playback (SD card, Makapix)
- Channel switching via Web UI
- Multi-channel weighted playback
- NAE injection via MQTT simulation
- Auto-swap timer accuracy
- Memory usage under various configurations

### 10.3 Regression Tests

- All existing Web UI functions still work
- Touch navigation works
- Dwell time setting persists
- OTA update path unaffected

---

## 11. Open Questions & Future Work

### 11.1 Deferred Features

| Feature | Notes |
|---------|-------|
| Playlist support | Treat playlists as single units or flatten |
| Live Mode | Synchronize playback across devices |
| Per-channel settings | PE, dwell override, pick mode per channel |
| Persistent history | Save/restore history across reboots |
| Manual weight UI | Web UI for MaE configuration |

### 11.2 Performance Considerations

- **SD I/O**: Batch reads, PSRAM caching
- **Memory**: H + L buffers in PSRAM
- **PRNG**: PCG32 is fast on RISC-V

### 11.3 Future API Extensions

```c
// Potential future additions:
esp_err_t play_scheduler_save_state(void);
esp_err_t play_scheduler_restore_state(void);
esp_err_t play_scheduler_set_channel_settings(const char *channel_id, const ps_channel_settings_t *settings);
```

---

## Appendix A: Original Specification

The original Play Scheduler specification is preserved in:
`docs/play-scheduler/SPECIFICATION.md`

---

## Appendix B: Change Log

| Date | Change |
|------|--------|
| 2026-01-01 | Initial plan created |

---

*This document is a living document. Update it as implementation progresses.*

