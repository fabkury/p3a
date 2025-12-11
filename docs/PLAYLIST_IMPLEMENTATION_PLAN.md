# Playlist Support Implementation Plan for p3a

> **Date**: December 2025  
> **Status**: Planning Phase  
> **Author**: Implementation Analysis

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Current Architecture Analysis](#2-current-architecture-analysis)
3. [Design Overview](#3-design-overview)
4. [Component Design](#4-component-design)
   - 4.1 [Play Order Navigator](#41-play-order-navigator)
   - 4.2 [Play Buffer Manager](#42-play-buffer-manager)
   - 4.3 [Playlist Storage Handler](#43-playlist-storage-handler)
   - 4.4 [Live Mode Synchronizer](#44-live-mode-synchronizer)
5. [Data Structures](#5-data-structures)
6. [Implementation Phases](#6-implementation-phases)
7. [Pseudocode](#7-pseudocode)
8. [Open Questions](#8-open-questions)
9. [Pain Points and Challenges](#9-pain-points-and-challenges)
10. [Edge Cases](#10-edge-cases)
11. [Testing Strategy](#11-testing-strategy)
12. [Alternative Approaches](#12-alternative-approaches)

---

## 1. Executive Summary

This document outlines the implementation plan for adding full playlist support to p3a, transforming it from a simple artwork-by-artwork player into a sophisticated player that handles:

- **Playlists**: Posts that contain 1-1024 artworks
- **Play Queue**: On-the-fly navigation through posts and in-playlist artworks
- **Play Buffer**: Maintains BN (default: 6) artworks ready for playback
- **Play Order**: Original, Creation, or Random order (persisted in NVS)
- **Playlist Expansion (PE)**: Controls how many artworks from each playlist enter the play queue
- **Live Mode**: Synchronized playback across devices using `sync_playlist` component

### Key Design Principles

1. **Minimal Memory Footprint**: No pre-built play queue; compute next/prev on-demand
2. **On-Demand Loading**: Cache only current playlist metadata, load others as needed
3. **Non-Disruptive Downloads**: Continue playback while buffering artworks
4. **Thread Safety**: Mutexes on all shared state (navigator, buffer)
5. **Graceful Degradation**: Reset to (p=0, q=0) on invalid states

---

## 2. Current Architecture Analysis

### 2.1 Existing Components

| Component | File | Purpose |
|-----------|------|---------|
| `channel_player` | `components/channel_manager/channel_player.c` | Manages post-level navigation, shuffle |
| `sdcard_channel` | `components/channel_manager/sdcard_channel.c` | Enumerates local animation files |
| `makapix_channel_impl` | `components/channel_manager/makapix_channel_impl.c` | Remote channel with index.bin storage |
| `channel_interface` | `components/channel_manager/include/channel_interface.h` | Polymorphic channel abstraction |
| `animation_player` | `main/animation_player.c` | Loads/renders animations, front/back buffer |
| `sync_playlist` | `components/sync_playlist/sync_playlist.c` | PCG-based deterministic random for Live Mode |
| `config_store` | `components/config_store/config_store.c` | NVS JSON config persistence |
| `p3a_state` | `components/p3a_core/p3a_state.c` | Global state machine |

### 2.2 Current Flow

```
app_main
  └─> animation_player_init
        └─> channel_player_init
        └─> sdcard_channel_init / makapix_channel_create
        └─> load_first_animation
  └─> auto_swap_task (dwell timer)
        └─> channel_player_advance
        └─> animation_loader_task (prefetch next)
```

### 2.3 Current Limitations

1. **No playlist concept**: Each post = one artwork
2. **No in-playlist navigation**: Only `p` (post index), no `q` (artwork index)
3. **Fixed playback order**: Shuffle happens at channel_player level only
4. **No play buffer abstraction**: Only front/back buffer for rendering
5. **No PE (playlist expansion)**: All posts treated equally
6. **No Live Mode integration**: sync_playlist exists but unused

---

## 3. Design Overview

### 3.1 Conceptual Model

```
┌─────────────────────────────────────────────────────────────┐
│                        CHANNEL                               │
│  ┌─────┐ ┌─────┐ ┌─────────────────────────┐ ┌─────┐ ┌─────┐│
│  │Post0│ │Post1│ │       Post2 (Playlist)   │ │Post3│ │Post4││
│  │Art  │ │Art  │ │ ┌────┐ ┌────┐ ┌────┐    │ │Art  │ │Art  ││
│  └─────┘ └─────┘ │ │Art0│ │Art1│ │Art2│... │ └─────┘ └─────┘│
│                  │ └────┘ └────┘ └────┘    │                │
│                  └─────────────────────────┘                │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                     PLAY QUEUE (PE=2)                        │
│  ┌─────┐ ┌─────┐ ┌────┐ ┌────┐ ┌─────┐ ┌─────┐             │
│  │p0,q0│ │p1,q0│ │p2,q0│ │p2,q1│ │p3,q0│ │p4,q0│             │
│  │Art0 │ │Art1 │ │Art2a│ │Art2b│ │Art3 │ │Art4 │             │
│  └─────┘ └─────┘ └────┘ └────┘ └─────┘ └─────┘             │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   PLAY BUFFER (BN=6)                         │
│  Current + next 5 artworks that are downloaded and ready    │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 State Variables

| Variable | Type | Persistence | Description |
|----------|------|-------------|-------------|
| `p` | `uint16_t` | Session only | Current post index in channel |
| `q` | `uint16_t` | Session only | Current artwork index in playlist (0 if not playlist) |
| `play_order` | `enum` | NVS | ORIGINAL, CREATED, RANDOM |
| `PE` | `uint16_t` | NVS | Playlist expansion (0=infinity, 1-1023) |
| `BN` | `uint8_t` | NVS | Buffer size target (default 6) |
| `dwell_time` | `uint32_t` | NVS | Seconds per artwork (default 30) |
| `randomize_playlist` | `bool` | NVS | Randomize internal playlist order |
| `live_mode` | `bool` | Session only | Enable synchronized playback |
| `random_seed` | `uint64_t` | NVS | Seed for random order |

---

## 4. Component Design

### 4.1 Play Order Navigator

**Location**: New component `components/play_navigator/`

**Responsibility**: Compute next/prev artwork given current (p, q), order, and PE.

```c
// play_navigator.h

typedef enum {
    PLAY_ORDER_ORIGINAL,   // Server/on-disk order
    PLAY_ORDER_CREATED,    // Ordered by created_at (newest first)
    PLAY_ORDER_RANDOM,     // Deterministic random with seed
} play_order_t;

typedef struct {
    uint16_t p;            // Post index
    uint16_t q;            // In-playlist artwork index (0 if not playlist)
} play_position_t;

typedef struct play_navigator_s *play_navigator_handle_t;

// Initialization
esp_err_t play_navigator_init(void);
void play_navigator_deinit(void);

// Configuration
esp_err_t play_navigator_set_order(play_order_t order);
esp_err_t play_navigator_set_pe(uint16_t pe);  // 0 = infinity
esp_err_t play_navigator_set_seed(uint64_t seed);
esp_err_t play_navigator_set_randomize_playlist(bool enable);

// Navigation (thread-safe)
esp_err_t play_navigator_next(play_position_t *current, play_position_t *out_next);
esp_err_t play_navigator_prev(play_position_t *current, play_position_t *out_prev);
esp_err_t play_navigator_reset(play_position_t *out_pos);  // Returns (0, 0)

// Query
play_order_t play_navigator_get_order(void);
uint16_t play_navigator_get_pe(void);
bool play_navigator_is_playlist(uint16_t p);
uint16_t play_navigator_get_playlist_length(uint16_t p);
```

**Key Logic for `next()`**:

```pseudocode
function next(current: play_position_t) -> play_position_t:
    p, q = current.p, current.q
    post = get_post(p)
    
    if post.is_playlist:
        effective_pe = (PE == 0) ? post.artwork_count : min(PE, post.artwork_count)
        if q + 1 < effective_pe:
            // Stay in playlist
            return (p, q + 1)
        else:
            // Exit playlist, go to next post
            next_p = get_next_post_index(p)  // Respects play_order
            return (next_p, 0)
    else:
        // Not a playlist, advance post
        next_p = get_next_post_index(p)
        next_post = get_post(next_p)
        if next_post.is_playlist:
            return (next_p, 0)  // Enter playlist at first artwork
        else:
            return (next_p, 0)
```

**Key Logic for `prev()`**:

```pseudocode
function prev(current: play_position_t) -> play_position_t:
    p, q = current.p, current.q
    
    if q > 0:
        // Move backward within playlist
        return (p, q - 1)
    else:
        // Exit current post (or playlist), go to previous post
        prev_p = get_prev_post_index(p)  // Respects play_order
        prev_post = get_post(prev_p)
        if prev_post.is_playlist:
            effective_pe = (PE == 0) ? prev_post.artwork_count : min(PE, prev_post.artwork_count)
            return (prev_p, effective_pe - 1)  // Enter at last artwork
        else:
            return (prev_p, 0)
```

### 4.2 Play Buffer Manager

**Location**: New component `components/play_buffer/`

**Responsibility**: Maintain BN artworks ready to play, trigger background downloads.

```c
// play_buffer.h

typedef struct {
    char filepath[256];           // Path to artwork file
    play_position_t position;     // (p, q) in play queue
    bool available;               // File downloaded and ready
    bool download_in_progress;    // Currently downloading
} buffer_entry_t;

typedef struct play_buffer_s *play_buffer_handle_t;

esp_err_t play_buffer_init(uint8_t buffer_size);  // BN
void play_buffer_deinit(void);

// Buffer management
esp_err_t play_buffer_set_target_size(uint8_t bn);
uint8_t play_buffer_get_available_count(void);
bool play_buffer_is_position_ready(const play_position_t *pos);

// Current position
esp_err_t play_buffer_set_current(const play_position_t *pos);
esp_err_t play_buffer_get_current_filepath(char *out_path, size_t max_len);

// Advance/retreat
esp_err_t play_buffer_advance(void);
esp_err_t play_buffer_retreat(void);

// Background download control
esp_err_t play_buffer_start_background_downloads(void);
esp_err_t play_buffer_pause_downloads(void);
esp_err_t play_buffer_resume_downloads(void);

// Callbacks
typedef void (*buffer_ready_cb_t)(const play_position_t *pos, const char *filepath);
typedef void (*buffer_download_progress_cb_t)(const play_position_t *pos, int percent);
esp_err_t play_buffer_set_ready_callback(buffer_ready_cb_t cb);
```

**Buffer Strategy**:

1. Current position is always index 0 in conceptual buffer
2. On advance: shift buffer, add new entry at end, start download if needed
3. On retreat: shift buffer other direction, may need to re-download
4. Downloads prioritize: current → current+1 → current+2 → ...

### 4.3 Playlist Storage Handler

**Location**: `components/channel_manager/playlist_storage.c`

**Responsibility**: Store/load playlist metadata from `/sdcard/playlists/`

```c
// playlist_storage.h

typedef struct {
    char playlist_id[64];         // Unique identifier (from server)
    uint16_t artwork_count;       // Number of artworks (1-1024)
    char **artwork_storage_keys;  // Array of storage_key UUIDs
    char **artwork_urls;          // Array of download URLs
    uint32_t *artwork_created_at; // Array of timestamps
    uint32_t playlist_created_at; // Playlist creation timestamp
} playlist_metadata_t;

// Storage path: /sdcard/playlists/{playlist_id}/
//   - metadata.json    (playlist info)
//   - index.bin        (binary index for fast loading)
//   - artworks/        (downloaded artwork files)

esp_err_t playlist_storage_init(void);
esp_err_t playlist_storage_load(const char *playlist_id, playlist_metadata_t *out_meta);
esp_err_t playlist_storage_save(const char *playlist_id, const playlist_metadata_t *meta);
esp_err_t playlist_storage_exists(const char *playlist_id, bool *out_exists);
void playlist_storage_free_metadata(playlist_metadata_t *meta);

// Artwork file operations
esp_err_t playlist_storage_get_artwork_path(const char *playlist_id, 
                                             uint16_t artwork_index,
                                             char *out_path, size_t max_len);
bool playlist_storage_artwork_exists(const char *playlist_id, uint16_t artwork_index);
```

### 4.4 Live Mode Synchronizer

**Location**: Enhance existing `components/sync_playlist/`

**Responsibility**: Synchronize playback across devices using shared seed + NTP time.

The existing `sync_playlist.c` already implements:
- PCG-XSL-RR 128/64 reversible random number generator
- Time-based synchronization (PRECISE and FORGIVING modes)
- Forward/backward navigation without storing history

**Integration Plan**:

```c
// In play_navigator.c

static bool s_live_mode_enabled = false;

void play_navigator_set_live_mode(bool enable) {
    s_live_mode_enabled = enable;
    if (enable) {
        SyncPlaylist.enable_live(true);
        // Re-sync to current time
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint32_t idx;
        SyncPlaylist.update(tv.tv_sec, &idx, NULL);
    } else {
        SyncPlaylist.enable_live(false);
    }
}

esp_err_t play_navigator_next(play_position_t *current, play_position_t *out_next) {
    if (s_live_mode_enabled && s_play_order == PLAY_ORDER_RANDOM) {
        // Let sync_playlist determine next
        SyncPlaylist.next();
        // Map sync_playlist index to (p, q)
        // ...
    } else {
        // Normal navigation
        // ...
    }
}
```

**Challenges**:
- `sync_playlist` operates on flat index, needs mapping to (p, q) space
- Must handle PE (playlist expansion) in synchronization
- All devices need same PE, order, seed, and start_time configuration

---

## 5. Data Structures

### 5.1 Post Type Detection

Posts from Makapix server will include a `type` field:

```json
{
  "post_id": 12345,
  "type": "artwork",       // or "playlist"
  "storage_key": "abc-123",
  "art_url": "https://...",
  "created_at": "2025-12-10T00:00:00Z"
}

// For playlist type:
{
  "post_id": 12346,
  "type": "playlist",
  "playlist_id": "def-456",
  "artwork_count": 12,
  "artworks": [
    {"storage_key": "...", "art_url": "...", "created_at": "..."},
    // ...
  ]
}
```

### 5.2 Extended Channel Entry (44 bytes → 48 bytes)

```c
typedef struct __attribute__((packed)) {
    uint8_t sha256[32];          // Storage key UUID (first 16 bytes)
    uint32_t created_at;         // Unix timestamp
    uint16_t flags;              // Filter flags
    uint8_t extension;           // File extension enum
    uint8_t post_type;           // NEW: 0=artwork, 1=playlist
    uint16_t artwork_count;      // NEW: For playlists, number of artworks
    uint8_t reserved[2];         // Reserved (reduced from 5)
} channel_entry_extended_t;
```

### 5.3 NVS Keys for New Settings

```c
#define NVS_KEY_PLAY_ORDER       "play_order"     // uint8_t: 0=original, 1=created, 2=random
#define NVS_KEY_PE               "pe"             // uint16_t: 0-1023
#define NVS_KEY_BN               "bn"             // uint8_t: 1-20 (default 6)
#define NVS_KEY_RANDOM_SEED      "rand_seed"      // uint64_t
#define NVS_KEY_RANDOMIZE_PLAYLIST "rand_playlist" // uint8_t: 0 or 1
```

---

## 6. Implementation Phases

### Phase 1: Foundation (Estimated: 3-4 days)

1. **Create `play_navigator` component**
   - Implement (p, q) position tracking
   - Implement play order enum and switching
   - Implement `next()/prev()` for non-playlist posts only
   - Add mutex protection
   - Persist settings to NVS

2. **Extend channel entry structure**
   - Add `post_type` and `artwork_count` fields
   - Maintain backward compatibility with existing index.bin

3. **Unit tests**
   - Navigation correctness for various orders
   - NVS persistence tests

### Phase 2: Playlist Support (Estimated: 4-5 days)

1. **Create `playlist_storage` component**
   - Implement storage layout in `/sdcard/playlists/`
   - Load/save playlist metadata
   - Parse playlist JSON from server

2. **Update `play_navigator` for playlists**
   - Implement PE (playlist expansion)
   - Implement `next()/prev()` with playlist handling
   - Load playlist metadata on-demand

3. **Update API parsing**
   - Detect playlist-type posts
   - Store playlist metadata when received

### Phase 3: Play Buffer (Estimated: 3-4 days)

1. **Create `play_buffer` component**
   - Implement buffer state tracking
   - Integrate with `play_navigator`
   - Background download task

2. **Update download manager**
   - Priority-based downloads (play queue order)
   - 3 retries with exponential backoff (1s, 5s, 15s)

3. **UI feedback**
   - "Downloading artworks..." message when buffer empty

### Phase 4: Live Mode (Estimated: 2-3 days)

1. **Integrate `sync_playlist` with `play_navigator`**
   - Map flat index to (p, q) with PE consideration
   - Time-based resync

2. **Add Live Mode toggle**
   - Only meaningful when play_order == RANDOM
   - Shared seed management

### Phase 5: Integration & Polish (Estimated: 3-4 days)

1. **Update `animation_player`**
   - Use `play_buffer` instead of direct channel queries
   - Update auto-swap to use navigator

2. **Update `channel_player`**
   - Deprecate internal shuffle, defer to navigator
   - Bridge for backward compatibility

3. **Testing & Edge Cases**
   - Synthetic 1024-artwork playlists
   - Memory pressure tests
   - Power-loss recovery

---

## 7. Pseudocode

### 7.1 Main Playback Loop (Modified)

```pseudocode
// In animation_player.c

function on_dwell_timeout():
    // Use navigator instead of channel_player
    current_pos = play_navigator_get_current()
    next_pos = play_navigator_next(current_pos)
    
    if play_buffer_is_position_ready(next_pos):
        filepath = play_buffer_get_filepath(next_pos)
        load_and_display(filepath)
        play_navigator_set_current(next_pos)
        play_buffer_advance()  // Shift buffer, trigger downloads
    else:
        // Not ready yet, show "Downloading..." or skip
        show_downloading_message()
        // Don't advance; buffer will callback when ready
```

### 7.2 Navigator next() with Playlists

```pseudocode
function navigator_next(current: position) -> position:
    p, q = current.p, current.q
    post = channel_get_post(p)
    
    if post.type == PLAYLIST:
        meta = playlist_storage_load(post.playlist_id)
        actual_count = meta.artwork_count
        effective_pe = (PE == 0) ? actual_count : min(PE, actual_count)
        
        if randomize_playlist:
            // Need to track random order within playlist
            next_q = get_next_random_q_in_playlist(p, q, effective_pe)
        else:
            next_q = q + 1
        
        if next_q < effective_pe:
            return (p, next_q)
        else:
            // Exit playlist
            next_p = get_next_post_index_by_order(p)
            return (next_p, 0)
    else:
        // Artwork post
        next_p = get_next_post_index_by_order(p)
        return (next_p, 0)

function get_next_post_index_by_order(current_p):
    match play_order:
        ORIGINAL: return (current_p + 1) % total_posts
        CREATED:  return get_next_by_created_date(current_p)
        RANDOM:   return get_next_random_post(current_p)
```

### 7.3 Buffer Download Task

```pseudocode
function buffer_download_task():
    while true:
        wait_for_signal_or_timeout(5 seconds)
        
        if download_paused:
            continue
        
        available_count = play_buffer_get_available_count()
        if available_count >= BN:
            continue  // Buffer full
        
        // Find highest-priority missing artwork
        for i in range(BN):
            pos = play_buffer_get_entry(i).position
            if not play_buffer_is_position_ready(pos):
                if not is_download_in_progress(pos):
                    start_download(pos)
                break  // Only start one at a time
        
function start_download(pos):
    set_download_in_progress(pos, true)
    
    filepath = get_artwork_filepath(pos)
    url = get_artwork_url(pos)
    
    for retry in [1, 5, 15]:  // Exponential backoff
        result = download_file(url, filepath)
        if result == SUCCESS:
            set_download_in_progress(pos, false)
            notify_buffer_ready(pos, filepath)
            return
        sleep_seconds(retry)
    
    // Failed after retries
    set_download_in_progress(pos, false)
    log_error("Download failed for position (%d, %d)", pos.p, pos.q)
```

---

## 8. Open Questions

### 8.1 Server Protocol Questions

1. **Q**: How are playlists identified in the server response?
   - **Assumption**: Posts have a `type` field: `"artwork"` or `"playlist"`

2. **Q**: Does the server send all playlist artworks in one response?
   - **Assumption**: Yes, up to 1024 artworks in the `artworks[]` array

3. **Q**: Can PE be set remotely via MQTT command?
   - **Recommendation**: Yes, add `set_pe` MQTT command

4. **Q**: Should channels remember their own PE setting?
   - **Recommendation**: Global PE setting, not per-channel

### 8.2 Implementation Questions

1. **Q**: When randomize_playlist is ON and play_order is RANDOM, how do we maintain reversibility?
   - **Answer**: Use separate PCG instance for in-playlist randomization with playlist-specific seed derived from `hash(main_seed, playlist_id)`

2. **Q**: How to handle a playlist that hasn't finished downloading when we need to play from it?
   - **Answer**: Play available artworks first, download rest in background. If PE=3 and only 1 artwork downloaded, play that one repeatedly until more ready.

3. **Q**: Memory budget for playlist metadata cache?
   - **Recommendation**: Cache only current playlist. With 1024 artworks × ~100 bytes each = ~100KB worst case, acceptable for PSRAM.

---

## 9. Pain Points and Challenges

### 9.1 High-Impact Challenges

1. **Reversible Random with Playlists**
   - Going backward through random order while respecting PE and randomize_playlist
   - Solution: Use PCG's reversibility; store playlist random state separately

2. **Download Priority Changes**
   - User may go backward, changing which artworks need to be buffered next
   - Solution: Cancel non-critical downloads, reprioritize queue

3. **Memory Constraints**
   - ESP32-P4 has 32MB PSRAM but must share with frame buffers
   - Solution: Strict single-playlist caching policy, lazy loading

4. **Thread Safety**
   - Multiple tasks access navigator (auto-swap, touch, MQTT commands)
   - Solution: Mutex on all navigator state access

### 9.2 Medium-Impact Challenges

1. **Index.bin Format Migration**
   - Existing index.bin has 44-byte entries, new format needs 48 bytes
   - Solution: Version header in index.bin, migration on first load

2. **Live Mode Position Mapping**
   - sync_playlist operates on flat index, need to map to (p, q)
   - Solution: Build mapping table on Live Mode enable

3. **Partially Downloaded Playlists**
   - User switches away before all artworks downloaded
   - Solution: Track download state per-artwork in playlist metadata

### 9.3 Lower-Impact Challenges

1. **NVS Storage Limits**
   - Many new settings to persist
   - Solution: Keep settings minimal, use compact encoding

2. **UI Feedback During Downloads**
   - User needs to know something is happening
   - Solution: "Downloading artworks..." message, no progress overlay

---

## 10. Edge Cases

### 10.1 Navigation Edge Cases

| Scenario | Expected Behavior |
|----------|-------------------|
| Last artwork in playlist, next pressed | Move to next post (p+1, q=0) |
| First artwork in playlist, prev pressed | Move to prev post's last artwork (p-1, q=PE-1) |
| PE > playlist.artwork_count | Use actual count as limit |
| PE = 0 (infinity), 1000-artwork playlist | Play all 1000 artworks |
| Only playlist in channel | Loop through playlist forever |
| Empty playlist (0 artworks) | Skip to next post |
| Playlist download fails completely | Skip to next post, log error |

### 10.2 Buffer Edge Cases

| Scenario | Expected Behavior |
|----------|-------------------|
| Channel has only 2 artworks, BN=6 | Buffer size = 2 |
| All BN artworks are from same playlist | Valid; buffer respects PE |
| User rapidly presses next 10 times | Buffer drains, may show "Downloading..." |
| Download in progress when user goes back | Don't cancel; re-prioritize queue |

### 10.3 Live Mode Edge Cases

| Scenario | Expected Behavior |
|----------|-------------------|
| Device clock drift > 10 seconds | FORGIVING mode handles gracefully |
| NTP sync fails | Continue with last known time |
| User presses next/prev in Live Mode | Disable Live Mode, manual control takes over |
| PE changes during Live Mode | Re-sync required; may cause brief desync |

---

## 11. Testing Strategy

### 11.1 Unit Tests

```c
// test_play_navigator.c

TEST_CASE("next() with single artworks only", "[navigator]") {
    // Setup: channel with 5 artwork posts
    // Test: next() from each position returns correct next position
}

TEST_CASE("next() enters playlist at q=0", "[navigator]") {
    // Setup: channel with [artwork, playlist(10), artwork]
    // Test: next() from post 0 returns (1, 0)
}

TEST_CASE("next() respects PE limit", "[navigator]") {
    // Setup: playlist with 10 artworks, PE=3
    // Test: after (1, 2), next() returns (2, 0), not (1, 3)
}

TEST_CASE("prev() exits playlist at q=PE-1", "[navigator]") {
    // Setup: channel with [artwork, playlist(10), artwork], PE=3
    // Test: prev() from (2, 0) returns (1, 2)
}

TEST_CASE("random order is reversible", "[navigator]") {
    // Setup: random order, navigate forward 100 steps
    // Test: navigate backward 100 steps, verify positions match
}
```

### 11.2 Integration Tests

```c
TEST_CASE("buffer maintains BN artworks", "[buffer][integration]") {
    // Setup: channel with 100 artworks, BN=6
    // Test: after advance(), buffer has 6 ready artworks
}

TEST_CASE("download retries with backoff", "[buffer][integration]") {
    // Setup: mock server that fails first 2 requests
    // Test: download succeeds on 3rd retry after 1s + 5s delays
}
```

### 11.3 Synthetic Load Tests

```c
TEST_CASE("1024-artwork playlist performance", "[performance]") {
    // Setup: generate synthetic playlist with 1024 artworks
    // Test: next() completes in < 10ms
    // Test: Memory usage < 200KB
}
```

---

## 12. Alternative Approaches

### 12.1 Pre-built Play Queue (Rejected)

**Approach**: Build entire play queue in memory on channel load.

**Pros**:
- Simple O(1) next/prev
- No on-the-fly computation

**Cons**:
- Memory: 1000 posts × up to 1024 artworks each = millions of entries
- Rebuild on order change or PE change

**Decision**: Rejected due to memory constraints.

### 12.2 Flat Index File for Playlists (Considered)

**Approach**: Pre-expand playlists into channel's index.bin.

**Pros**:
- Unified handling of all artworks
- No special playlist logic in navigator

**Cons**:
- Duplicates artwork entries across channels
- PE changes require index rebuild
- Loses playlist grouping semantics

**Decision**: Rejected; maintain playlist as distinct entity.

### 12.3 Streaming Playlist Metadata (Considered)

**Approach**: Don't cache playlist metadata; query server each time.

**Pros**:
- Always up-to-date
- Zero local storage

**Cons**:
- Network dependency
- Latency on playlist entry

**Decision**: Rejected; cache current playlist locally for offline resilience.

### 12.4 Recommended Approach: On-Demand Navigation with Caching

**This is the chosen approach**, as detailed throughout this document:

1. Compute next/prev on demand using navigator
2. Cache only current playlist metadata
3. Maintain BN-sized play buffer
4. Download in background based on play queue order
5. Persist settings (order, PE, seed) in NVS

This balances memory efficiency, responsiveness, and implementation complexity.

---

## Appendix A: File Listing

New files to create:

```
components/play_navigator/
  CMakeLists.txt
  Kconfig
  include/play_navigator.h
  play_navigator.c

components/play_buffer/
  CMakeLists.txt
  Kconfig
  include/play_buffer.h
  play_buffer.c
  buffer_download_task.c

components/channel_manager/
  playlist_storage.c                    (NEW)
  include/playlist_storage.h            (NEW)
  include/channel_entry_extended.h      (NEW)
```

Files to modify:

```
components/channel_manager/
  channel_player.c                      (integrate with navigator)
  makapix_channel_impl.c                (parse playlist posts)

components/sync_playlist/
  sync_playlist.c                       (integration hooks)

main/
  animation_player.c                    (use play_buffer)
  p3a_main.c                            (init new components)
```

---

## Appendix B: Settings API (HTTP/MQTT)

New HTTP endpoints:

```
GET  /api/playback/order          → {"order": "random", "pe": 0, "bn": 6}
POST /api/playback/order          ← {"order": "original|created|random"}
POST /api/playback/pe             ← {"pe": 0-1023}
POST /api/playback/bn             ← {"bn": 1-20}
POST /api/playback/live_mode      ← {"enabled": true|false}
POST /api/playback/randomize_playlist ← {"enabled": true|false}
```

New MQTT commands:

```json
{"cmd": "set_order", "order": "random"}
{"cmd": "set_pe", "pe": 5}
{"cmd": "set_live_mode", "enabled": true}
{"cmd": "set_randomize_playlist", "enabled": true}
```

---

## Appendix C: Version Migration

To handle existing installations:

1. On boot, check index.bin version header (add if missing)
2. If v1 (44-byte entries), migrate to v2 (48-byte):
   - Read all entries
   - Add `post_type=ARTWORK`, `artwork_count=0` to each
   - Write back with v2 header
3. For missing settings in NVS, use defaults:
   - `play_order`: ORIGINAL
   - `PE`: 0 (infinity)
   - `BN`: 6
   - `randomize_playlist`: false

---

*End of Implementation Plan*
