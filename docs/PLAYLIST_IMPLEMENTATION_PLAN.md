# Playlist Support Implementation Plan

## Executive Summary

This document outlines a comprehensive plan to implement playlist support in p3a, transforming it from a single-artwork-per-post player into a system that can handle posts containing either individual artworks or playlists of up to 1024 artworks. The implementation requires significant architectural changes to the playback system, channel management, and buffering strategies.

## Table of Contents

1. [Current State Analysis](#current-state-analysis)
2. [Requirements Summary](#requirements-summary)
3. [Key Concepts](#key-concepts)
4. [Architectural Design](#architectural-design)
5. [Implementation Strategy](#implementation-strategy)
6. [Data Structures](#data-structures)
7. [Core Algorithm Pseudocode](#core-algorithm-pseudocode)
8. [Component Changes](#component-changes)
9. [Pain Points & Edge Cases](#pain-points--edge-cases)
10. [Open Questions](#open-questions)
11. [Testing Strategy](#testing-strategy)
12. [Implementation Phases](#implementation-phases)

---

## Current State Analysis

### Existing Architecture

**Current Playback Flow:**
```
Channel (e.g., sdcard_channel or makapix_channel)
    ↓
channel_player (manages post iteration)
    ↓
animation_player_loader (loads individual files)
    ↓
animation_player (renders frames)
    ↓
display_renderer (outputs to screen)
```

**Key Components:**
- `channel_interface.h/c`: Abstract interface for channels with next/prev navigation
- `channel_player.h/c`: Manages post iteration and playback order
- `sdcard_channel`: Scans local SD card for animation files
- `makapix_channel_impl`: Downloads artworks from Makapix Club server
- `animation_player_loader.c`: Loads animation files and handles swapping
- `playback_controller`: Tracks current playback metadata
- `sync_playlist`: Existing synchronized playlist engine (currently unused)

**Current Limitations:**
1. One post = one animation file (no playlist support)
2. No concept of "in-playlist artwork index" (q)
3. Play buffer is implicit (next file loads when needed)
4. No playlist expansion setting (PE)
5. Posts are always treated as individual artworks

---

## Requirements Summary

### Core Requirements

1. **Playlist Support**: Posts can be either individual artworks OR playlists (1-1024 artworks)
2. **Play Queue Abstraction**: Derive ordered list of artworks on-the-fly using p/q indices
3. **Playlist Expansion (PE)**: Setting to limit how many artworks from a playlist are included in the play queue
4. **Play Buffer**: Maintain at least BN (default 6) artworks buffered and ready to play
5. **Seamless Navigation**: Enter/exit playlists seamlessly according to play order
6. **Three Play Orders**: Original (server_seq), Creation date, Random (with reproducible seed)
7. **Persistent Settings**: PE and play order persist across reboots (NVS)

### Non-Requirements

- Playlists CANNOT contain other playlists (one level only)
- No pre-built in-memory play queue (must be derived on-demand)
- No direct index-based access to play queue (only next/prev operations)

---

## Key Concepts

### Play Queue

**Definition**: The ordered list of individual artworks (not posts) currently playing or scheduled to play.

**Properties:**
- Not stored in memory in its entirety
- Derived on-the-fly using next(i)/prev(i) operations
- Determined by: current_channel, play_order, p (post index), q (in-playlist artwork index)
- Handles playlist expansion according to PE setting

**Example:**
```
Channel C has 10 posts: p0, p1, p2, p3, p4, p5, p6, p7, p8, p9
- p4 is a playlist with 12 artworks: pa0, pa1, pa2...pa11
- All other posts are individual artworks
- Play order: server order
- PE = 3

Play queue (derived):
p0a, p1a, p2a, p3a, pa0, pa1, pa2, p5a, p6a, p7a, p8a, p9a

Note: Instead of p4a, we have pa0, pa1, pa2 (first 3 from playlist)
```

### Post Index (p) and In-Playlist Index (q)

**p (Post Index)**: Current post in the channel (0-based)
**q (In-Playlist Index)**: Current artwork within playlist (0-based, 0 if not a playlist)

**Navigation Rules:**
- Swapping outside playlist: p changes, q remains 0
- Swapping inside playlist: q changes, p stays same
- Entering playlist (next): p changes, q = 0
- Exiting playlist (next): p changes, q = 0
- Entering playlist (back): p changes, q = min(PE, playlist_length) - 1
- Exiting playlist (back): p changes, q = 0

### Playlist Expansion (PE)

**Definition**: Setting that determines how many artworks from a playlist are included in the play queue.

**Values:**
- PE = 0: "all artworks" (infinity) - include entire playlist
- PE = 1-1023: Include exactly PE artworks from the playlist
- Default: 0 (all artworks)

**Storage**: Persisted in NVS

### Play Buffer

**Definition**: Ordered list of artworks that are:
1. Scheduled to come next in the play queue
2. Already available locally (downloaded and ready)

**Properties:**
- Not pre-built in memory (derived from play queue)
- Target size: BN artworks (default 6)
- Actual size may be less if fewer artworks available
- Downloads triggered when buffer < BN

**Example:**
```
Current p=2, q=0, BN=6, PE=3
Current post: p2 (artwork)
Play buffer: p3a, pa0, pa1, pa2, p5a, p6a
(Next 6 artworks in play queue after p2)
```

### Play Order

**Three modes:**
1. **Original Order (server_seq)**: Server-provided order
2. **Creation Order**: Sorted by post creation date
3. **Random Order**: Randomized using seed (deterministic, reversible)

**Storage**: Persisted in NVS
**Default**: Original order

---

## Architectural Design

### High-Level Design Philosophy

**Lazy Evaluation Approach:**
- Play queue is NOT stored in memory
- Only current (p, q) position is tracked
- next()/prev() operations compute next position algorithmically
- Playlist boundaries handled during navigation

**Separation of Concerns:**
```
┌─────────────────────────────────────────┐
│    Channel Layer                        │  (Posts: artworks OR playlists)
│    - channel_interface                  │
│    - sdcard_channel / makapix_channel   │
└──────────────┬──────────────────────────┘
               │
               ↓
┌─────────────────────────────────────────┐
│    Playlist Navigator (NEW)             │  (Handles p/q, PE, play order)
│    - playlist_navigator.h/c             │
│    - Translates (p,q) → filepath        │
└──────────────┬──────────────────────────┘
               │
               ↓
┌─────────────────────────────────────────┐
│    Buffer Manager (NEW/MODIFIED)        │  (Maintains play buffer)
│    - playback_buffer.h/c                │
│    - Prefetches next BN artworks        │
└──────────────┬──────────────────────────┘
               │
               ↓
┌─────────────────────────────────────────┐
│    Animation Player/Loader              │  (Loads & renders artworks)
│    - animation_player_loader.c          │
│    - animation_player.c                 │
└─────────────────────────────────────────┘
```

### New Components

#### 1. Playlist Navigator (`playlist_navigator.h/c`)

**Responsibilities:**
- Maintain current position (p, q)
- Implement next()/prev() operations
- Handle playlist expansion (PE)
- Translate (p, q) to actual filepath
- Respect play order (original, created, random)
- Detect playlist boundaries

**Key APIs:**
```c
// Initialize with channel and settings
esp_err_t playlist_nav_init(channel_handle_t channel, 
                            channel_order_mode_t order,
                            uint32_t playlist_expansion);

// Get current artwork filepath
esp_err_t playlist_nav_get_current(char *filepath, size_t filepath_len);

// Navigate to next artwork
esp_err_t playlist_nav_next(void);

// Navigate to previous artwork
esp_err_t playlist_nav_prev(void);

// Get current position
void playlist_nav_get_position(uint32_t *out_p, uint32_t *out_q);

// Check if more artworks available
bool playlist_nav_has_next(void);
bool playlist_nav_has_prev(void);
```

#### 2. Playback Buffer Manager (`playback_buffer.h/c`)

**Responsibilities:**
- Maintain list of next BN buffered artworks
- Trigger downloads when buffer < BN
- Track which artworks are locally available
- Coordinate with playlist navigator

**Key APIs:**
```c
// Initialize buffer manager
esp_err_t playback_buffer_init(size_t target_size);

// Get next buffered artwork (removes from buffer)
esp_err_t playback_buffer_get_next(char *filepath, size_t filepath_len);

// Check buffer status
size_t playback_buffer_get_count(void);
bool playback_buffer_is_full(void);

// Trigger buffer refill
esp_err_t playback_buffer_refill(void);
```

#### 3. Playlist Metadata Handler (`playlist_metadata.h/c`)

**Responsibilities:**
- Load/save playlist JSON metadata
- Parse playlist artwork lists
- Provide playlist information to navigator

**Key APIs:**
```c
// Check if post is a playlist
bool playlist_is_playlist(const char *post_storage_key);

// Load playlist metadata
esp_err_t playlist_load_metadata(const char *post_storage_key,
                                 playlist_info_t *out_info);

// Get artwork from playlist by index
esp_err_t playlist_get_artwork_at(const playlist_info_t *playlist,
                                  uint32_t index,
                                  artwork_ref_t *out_artwork);
```

### Modified Components

#### 1. Channel Interface

**Changes:**
- Add `is_playlist` flag to `channel_item_ref_t`
- Add `playlist_length` field to `channel_item_ref_t`
- Extend to provide playlist metadata

#### 2. Channel Player

**Changes:**
- Replace with or delegate to `playlist_navigator`
- Remove post-level iteration logic
- Focus on channel-level operations only

#### 3. Animation Player Loader

**Changes:**
- Interface with `playback_buffer` instead of channel_player
- Handle "Downloading artworks..." display when buffer empty
- Trigger buffer refills when needed

#### 4. Makapix Channel Implementation

**Changes:**
- Recognize playlist posts from server metadata
- Download playlist JSON files
- Store playlists in separate folder structure
- Implement playlist metadata parsing

---

## Data Structures

### Core Structures

```c
// Represents a playlist post
typedef struct {
    char post_id[64];           // Post ID
    char storage_key[96];       // Storage key for playlist metadata
    bool is_playlist;           // True if this is a playlist
    uint32_t playlist_length;   // Number of artworks (0 if not playlist)
    char metadata_path[256];    // Path to playlist JSON file
} post_info_t;

// Represents a single artwork within a playlist
typedef struct {
    char artwork_id[64];        // Artwork ID
    char storage_key[96];       // Storage key for artwork file
    char filepath[256];         // Full path to artwork file
    asset_type_t type;          // File format (webp, gif, etc.)
    uint32_t index;             // Index within playlist
} artwork_ref_t;

// Playlist metadata (loaded from JSON)
typedef struct {
    char post_id[64];           // Parent post ID
    uint32_t artwork_count;     // Number of artworks in playlist
    artwork_ref_t *artworks;    // Array of artwork references
} playlist_info_t;

// Current playback position
typedef struct {
    uint32_t p;                 // Post index in channel
    uint32_t q;                 // In-playlist artwork index (0 if not playlist)
    bool in_playlist;           // Currently in a playlist
} playback_position_t;

// Navigator state
typedef struct {
    channel_handle_t channel;           // Current channel
    channel_order_mode_t order_mode;    // Current play order
    uint32_t playlist_expansion;        // PE setting
    playback_position_t position;       // Current position (p, q)
    
    // Cached data
    post_info_t current_post;           // Current post info
    playlist_info_t *current_playlist;  // Current playlist (if in_playlist)
    
    // Random order state
    uint32_t random_seed;               // Seed for random order
    uint32_t *random_order;             // Precomputed random order (if mode==RANDOM)
} playlist_navigator_t;

// Buffer entry
typedef struct {
    char filepath[256];         // Path to artwork file
    asset_type_t type;          // File format
    bool available;             // File downloaded and ready
    playback_position_t pos;    // Position (p, q) this artwork came from
} buffer_entry_t;

// Buffer manager state
typedef struct {
    buffer_entry_t *entries;    // Array of buffered entries
    size_t capacity;            // Target buffer size (BN)
    size_t count;               // Current entries in buffer
    size_t head;                // Read position
    size_t tail;                // Write position
} playback_buffer_t;
```

### NVS Storage

```c
// Settings stored in NVS
typedef struct {
    channel_order_mode_t play_order;    // Current play order
    uint32_t playlist_expansion;        // PE setting
    uint32_t random_seed;               // Seed for random mode
    uint32_t buffer_size;               // BN setting
} playback_settings_t;

// NVS keys
#define NVS_KEY_PLAY_ORDER           "play_order"
#define NVS_KEY_PLAYLIST_EXPANSION   "playlist_exp"
#define NVS_KEY_RANDOM_SEED          "random_seed"
#define NVS_KEY_BUFFER_SIZE          "buffer_size"
```

### Playlist Storage Structure

```
/sdcard/
├── animations/              # Local artworks (SD card channel)
├── vault/                   # Downloaded individual artworks
├── channels/
│   ├── <channel_id>/
│   │   ├── index.bin       # Channel index
│   │   └── metadata.json   # Channel metadata
└── playlists/               # Playlist storage
    ├── <post_storage_key>/
    │   ├── metadata.json    # Playlist metadata from server
    │   └── artworks/        # Symlinks or references to vault files
    │       ├── 0 -> ../../vault/<artwork_storage_key>.webp
    │       ├── 1 -> ../../vault/<artwork_storage_key>.gif
    │       └── ...
```

**Playlist metadata.json format:**
```json
{
  "post_id": "12345",
  "storage_key": "abc123def456",
  "type": "playlist",
  "artwork_count": 12,
  "artworks": [
    {
      "index": 0,
      "artwork_id": "67890",
      "storage_key": "xyz789abc123",
      "filename": "xyz789abc123.webp",
      "type": "webp"
    },
    {
      "index": 1,
      "artwork_id": "67891",
      "storage_key": "xyz789abc124",
      "filename": "xyz789abc124.gif",
      "type": "gif"
    }
    // ... more artworks
  ]
}
```

---

## Core Algorithm Pseudocode

### Navigation: next()

```
function playlist_nav_next():
    current_post = get_post_at_index(p)
    
    if current_post.is_playlist:
        // Currently in a playlist
        q++
        
        if q >= min(PE, current_post.playlist_length):
            // Reached end of playlist expansion
            p++  // Move to next post
            q = 0
            if p >= total_posts:
                p = 0  // Wrap to beginning
                if play_order == RANDOM:
                    regenerate_random_order()
        
        return get_artwork_at(p, q)
    else:
        // Current post is an artwork
        p++  // Move to next post
        q = 0
        
        if p >= total_posts:
            p = 0  // Wrap to beginning
            if play_order == RANDOM:
                regenerate_random_order()
        
        next_post = get_post_at_index(p)
        if next_post.is_playlist:
            q = 0  // Start at beginning of playlist
        
        return get_artwork_at(p, q)
```

### Navigation: prev()

```
function playlist_nav_prev():
    if q > 0:
        // Currently inside a playlist, move back within it
        q--
        return get_artwork_at(p, q)
    else:
        // At beginning of current post, move to previous post
        p--
        
        if p < 0:
            p = total_posts - 1  // Wrap to end
        
        prev_post = get_post_at_index(p)
        if prev_post.is_playlist:
            // Entering playlist from the end
            q = min(PE, prev_post.playlist_length) - 1
        else:
            q = 0
        
        return get_artwork_at(p, q)
```

### Get Artwork at Position

```
function get_artwork_at(p, q):
    // Apply play order to get actual post index
    actual_p = apply_play_order(p)
    
    post = get_post_at_index(actual_p)
    
    if not post.is_playlist:
        // Simple artwork
        return post.filepath
    else:
        // Playlist - need to get specific artwork
        playlist = load_playlist_metadata(post.storage_key)
        
        if q >= playlist.artwork_count:
            return ERROR  // Invalid index
        
        artwork = playlist.artworks[q]
        return artwork.filepath
```

### Apply Play Order

```
function apply_play_order(p):
    switch play_order:
        case ORIGINAL:
            return p  // Use as-is
        
        case CREATED:
            // Posts are pre-sorted by creation date
            return p
        
        case RANDOM:
            // Use precomputed random order
            if random_order == NULL:
                generate_random_order()
            return random_order[p]
```

### Buffer Refill

```
function playback_buffer_refill():
    while buffer.count < buffer.capacity:
        // Peek at next position without advancing
        next_p = p
        next_q = q
        
        // Simulate next() to get next position
        simulate_next(next_p, next_q)
        
        filepath = get_artwork_at(next_p, next_q)
        
        if not file_exists(filepath):
            // Not downloaded yet - trigger download
            trigger_download(next_p, next_q)
            break  // Wait for download to complete
        
        // Add to buffer
        buffer_entry = {
            filepath: filepath,
            available: true,
            pos: {p: next_p, q: next_q}
        }
        buffer.add(buffer_entry)
        
        // Check if there are more artworks
        if not has_more_artworks():
            break
```

### Download Trigger

```
function trigger_download(p, q):
    post = get_post_at_index(p)
    
    if not post.is_playlist:
        // Download single artwork
        download_artwork(post.storage_key)
    else:
        // Download artwork from playlist
        playlist = load_playlist_metadata(post.storage_key)
        artwork = playlist.artworks[q]
        download_artwork(artwork.storage_key)
```

---

## Component Changes

### 1. playlist_navigator.c (NEW)

**Purpose**: Core navigation logic for p/q handling

**Implementation Notes:**
- Maintain (p, q) state
- Implement next()/prev() operations
- Handle play order transformations
- Cache current post and playlist metadata
- Detect playlist boundaries
- Respect PE setting

**Key Functions:**
```c
esp_err_t playlist_nav_init(...)
esp_err_t playlist_nav_next(void)
esp_err_t playlist_nav_prev(void)
esp_err_t playlist_nav_get_current(...)
void playlist_nav_get_position(...)
esp_err_t playlist_nav_set_expansion(uint32_t pe)
esp_err_t playlist_nav_set_order(channel_order_mode_t order)
```

### 2. playback_buffer.c (NEW)

**Purpose**: Manage play buffer and prefetching

**Implementation Notes:**
- Circular buffer or linked list
- Track available vs. pending artworks
- Interface with download manager
- Coordinate with playlist navigator
- Handle buffer underflow gracefully

**Key Functions:**
```c
esp_err_t playback_buffer_init(size_t capacity)
esp_err_t playback_buffer_get_next(...)
esp_err_t playback_buffer_peek_next(...)
size_t playback_buffer_get_count(void)
esp_err_t playback_buffer_refill(void)
void playback_buffer_clear(void)
```

### 3. playlist_metadata.c (NEW)

**Purpose**: Handle playlist JSON files

**Implementation Notes:**
- Parse playlist JSON from server
- Cache loaded playlists
- Provide artwork references
- Handle missing/corrupted metadata

**Key Functions:**
```c
bool playlist_is_playlist(const char *storage_key)
esp_err_t playlist_load_metadata(...)
esp_err_t playlist_get_artwork_at(...)
void playlist_free_metadata(playlist_info_t *info)
esp_err_t playlist_save_metadata(...)
```

### 4. channel_interface.h (MODIFIED)

**Changes:**
```c
// Add to channel_item_ref_t:
typedef struct {
    char filepath[256];
    char storage_key[96];
    uint32_t item_index;
    channel_filter_flags_t flags;
    
    // NEW: Playlist support
    bool is_playlist;           // True if this is a playlist
    uint32_t playlist_length;   // Number of artworks (0 if not playlist)
    char metadata_path[256];    // Path to playlist metadata JSON
} channel_item_ref_t;
```

### 5. makapix_channel_impl.c (MODIFIED)

**Changes:**
- Detect playlist posts from server API
- Download playlist metadata JSON
- Store playlists in `/sdcard/playlists/<storage_key>/`
- Parse and cache playlist information
- Set `is_playlist` flag in channel items
- Download playlist artworks on demand

**New Functions:**
```c
static bool is_post_playlist(const makapix_post_t *post)
static esp_err_t download_playlist_metadata(...)
static esp_err_t parse_playlist_json(...)
```

### 6. sdcard_channel_impl.c (MODIFIED)

**Changes:**
- Recognize playlists in local storage
- Scan `/sdcard/playlists/` directory
- Load playlist metadata from JSON
- Set `is_playlist` flag in channel items

### 7. animation_player_loader.c (MODIFIED)

**Changes:**
- Replace channel_player calls with playback_buffer calls
- Get next artwork from buffer instead of channel
- Trigger buffer refills when needed
- Display "Downloading artworks..." when buffer empty
- Handle buffer underflow gracefully

**Modified Logic:**
```c
void animation_loader_task(void *arg) {
    while (true) {
        // Wait for swap request or buffer refill signal
        xSemaphoreTake(s_loader_sem, portMAX_DELAY);
        
        // Check buffer status
        if (playback_buffer_get_count() == 0) {
            // Buffer empty - show loading screen
            display_loading_message("Downloading artworks...");
            
            // Trigger buffer refill
            playback_buffer_refill();
            
            // Wait for at least one artwork
            while (playback_buffer_get_count() == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        
        // Get next artwork from buffer
        char filepath[256];
        esp_err_t err = playback_buffer_get_next(filepath, sizeof(filepath));
        if (err != ESP_OK) {
            continue;
        }
        
        // Load animation into back buffer
        err = load_animation_into_buffer(filepath, ...);
        // ... rest of loading logic
        
        // Trigger buffer refill if needed
        if (playback_buffer_get_count() < 3) {
            playback_buffer_refill();
        }
    }
}
```

### 8. channel_player.c (DEPRECATED/MODIFIED)

**Option A**: Deprecate and replace with playlist_navigator
**Option B**: Modify to be a thin wrapper around playlist_navigator

**Recommended**: Option A - clean break, avoid confusion

### 9. config_store.c (MODIFIED)

**Changes:**
- Add NVS storage for PE setting
- Add NVS storage for play order
- Add NVS storage for random seed
- Add NVS storage for buffer size (BN)

**New Functions:**
```c
esp_err_t config_store_get_playlist_expansion(uint32_t *out_pe)
esp_err_t config_store_set_playlist_expansion(uint32_t pe)
esp_err_t config_store_get_play_order(channel_order_mode_t *out_order)
esp_err_t config_store_set_play_order(channel_order_mode_t order)
esp_err_t config_store_get_random_seed(uint32_t *out_seed)
esp_err_t config_store_set_random_seed(uint32_t seed)
esp_err_t config_store_get_buffer_size(uint32_t *out_bn)
esp_err_t config_store_set_buffer_size(uint32_t bn)
```

### 10. http_api.c (MODIFIED)

**Changes:**
- Add REST endpoints for playlist expansion
- Add REST endpoints for play order
- Add REST endpoints for buffer size

**New Endpoints:**
```
GET  /api/settings/playlist_expansion
POST /api/settings/playlist_expansion
GET  /api/settings/play_order
POST /api/settings/play_order
GET  /api/settings/buffer_size
POST /api/settings/buffer_size
```

---

## Pain Points & Edge Cases

### Major Pain Points

#### 1. Complexity of Navigation Logic

**Problem**: The next()/prev() operations must correctly handle:
- Post-to-post navigation
- Entering playlists (from both directions)
- Exiting playlists (from both directions)
- Playlist expansion limits
- Play order transformations
- Wraparound at channel boundaries

**Mitigation**:
- Extensive unit testing with various scenarios
- Clear state machine documentation
- Defensive coding with assertions
- Logging of all state transitions

#### 2. Buffer Management Complexity

**Problem**: The buffer must:
- Track which artworks are available vs. pending
- Handle downloads completing out-of-order
- Deal with download failures
- Maintain correct order despite async operations
- Avoid blocking playback

**Mitigation**:
- Separate "available" and "pending" buffer states
- Use callbacks/events for download completion
- Implement fallback strategies (skip unavailable, retry, etc.)
- Careful synchronization with mutexes

#### 3. Random Order Reversibility

**Problem**: Random order must be:
- Deterministic (same seed = same order)
- Reversible (going back retraces exact path)
- Consistent across playlist boundaries
- Efficient (no full shuffle on every prev())

**Mitigation**:
- Use existing `sync_playlist` PCG-XSL-RR algorithm
- Precompute random order for posts
- Handle playlists deterministically within random posts
- Store random state in navigator

#### 4. Memory Constraints

**Problem**: ESP32-P4 has limited RAM:
- Cannot store full play queue
- Cannot cache all playlist metadata
- Buffer must be bounded
- Channel index can be large

**Mitigation**:
- Lazy evaluation of play queue
- Cache only current playlist
- Bounded buffer size (default 6)
- Stream playlist metadata from SD card as needed

#### 5. Download Coordination

**Problem**: Downloads must be:
- Prioritized (buffer first, then background)
- Non-blocking (don't stall playback)
- Efficient (avoid re-downloading)
- Coordinated with SDIO bus access

**Mitigation**:
- Priority queue for downloads
- Async download system
- Check file existence before download
- Respect existing SD pause mechanism

### Critical Edge Cases

#### 1. Empty Playlist

**Scenario**: A post is marked as playlist but has 0 artworks
**Handling**: Treat as error, skip to next post, log warning

#### 2. Playlist Expansion > Playlist Length

**Scenario**: PE=10 but playlist only has 5 artworks
**Handling**: Use min(PE, playlist_length) as effective limit

#### 3. All Artworks Unavailable

**Scenario**: Buffer refill finds no downloadable artworks
**Handling**: Display "No artworks available", wait for downloads, retry

#### 4. Corrupted Playlist Metadata

**Scenario**: Playlist JSON is malformed or missing
**Handling**: Treat as single-artwork post (fallback), log error, trigger re-download

#### 5. Mixed Available/Unavailable in Playlist

**Scenario**: Playlist has 10 artworks, but only 3 are downloaded
**Handling**: Buffer what's available, continue downloading, skip unavailable

#### 6. Play Order Change Mid-Playlist

**Scenario**: User changes play order while inside a playlist
**Handling**: 
- Option A: Finish current playlist with old order, apply new order after
- Option B: Exit playlist immediately, apply new order
- **Recommended**: Option A (less disruptive)

#### 7. PE Change Mid-Playlist

**Scenario**: User changes PE while inside a playlist (e.g., from 10 to 3)
**Handling**:
- If q < new_PE: Continue normally
- If q >= new_PE: Jump to next post
- Update buffer accordingly

#### 8. Random Seed Change

**Scenario**: User changes random seed mid-playback
**Handling**: Regenerate random order, reset to p=0, q=0

#### 9. Playlist at End of Channel

**Scenario**: Last post in channel is a long playlist
**Handling**: After reaching PE limit, wrap to first post

#### 10. Single Post Channel

**Scenario**: Channel has only 1 post (which might be a playlist)
**Handling**: Loop within that post, handle wraparound correctly

#### 11. All Posts Are Playlists

**Scenario**: Channel with 100% playlist posts
**Handling**: Normal operation, ensure PE is respected

#### 12. Rapid Next/Prev Spamming

**Scenario**: User rapidly taps next/prev buttons
**Handling**: Queue navigation operations, prevent race conditions

#### 13. Download Failure During Buffer Refill

**Scenario**: Network error, corrupted file, storage full
**Handling**: Log error, skip artwork, continue with next, retry later

#### 14. File Deleted During Playback

**Scenario**: Artwork file deleted externally while queued
**Handling**: Detect at load time, skip, trigger re-download

#### 15. Playlist Updated on Server

**Scenario**: Server changes playlist contents (add/remove artworks)
**Handling**: Detect on channel refresh, update local metadata, adjust q if needed

---

## Open Questions

### Design Decisions Needed

1. **Play Order Application**
   - Q: Should random order apply within playlists, or only to post order?
   - **Recommendation**: Only to post order. Playlists are always sequential internally.
   - **Rationale**: Simpler, preserves artist intent for playlist ordering.

2. **PE=0 Semantics**
   - Q: Should PE=0 mean "no expansion" or "unlimited expansion"?
   - **Requirement**: PE=0 means "all artworks" (unlimited).
   - **Confirmed**: This is the required behavior.

3. **Buffer Persistence Across Reboots**
   - Q: Should buffer state persist across reboots?
   - **Recommendation**: No. Rebuild on boot.
   - **Rationale**: Simpler, avoids stale data issues.

4. **Playlist Caching Strategy**
   - Q: Cache all playlists in RAM or load on-demand?
   - **Recommendation**: Load on-demand, cache only current.
   - **Rationale**: Memory constraints.

5. **Download Priority**
   - Q: Prioritize buffer downloads or background refresh?
   - **Recommendation**: Buffer downloads always have priority.
   - **Rationale**: User experience - playback should not stall.

6. **Failed Download Retry Logic**
   - Q: How many retries? Exponential backoff?
   - **Recommendation**: 3 retries with exponential backoff (1s, 5s, 15s).
   - **Rationale**: Balance between UX and server load.

7. **Playlist Display UI**
   - Q: Show "3/12" progress within playlist?
   - **Recommendation**: Yes, if metadata available.
   - **Rationale**: Better UX, helps users understand position.

8. **Back Button in Playlist**
   - Q: Does back go to previous artwork or previous post?
   - **Answer**: Previous artwork (according to requirements).
   - **Confirmed**: Must navigate within playlist.

9. **Partial Playlist Download**
   - Q: Can playback start before all playlist artworks downloaded?
   - **Answer**: Yes, buffer first BN artworks, download rest in background.
   - **Confirmed**: This is the intended behavior.

10. **Playlist Metadata Storage Location**
    - Q: Store in vault or separate playlists folder?
    - **Recommendation**: Separate `/sdcard/playlists/` folder.
    - **Rationale**: Cleaner organization, easier management.

### Implementation Questions

11. **Component Initialization Order**
    - Q: What's the startup sequence for new components?
    - **Recommendation**: 
      1. config_store (load settings)
      2. channel (load channel)
      3. playlist_navigator (init with channel+settings)
      4. playback_buffer (init with navigator)
      5. animation_player_loader (init with buffer)

12. **Thread Safety**
    - Q: Which components need mutexes?
    - **Recommendation**: All shared components (navigator, buffer) need mutexes.

13. **Error Recovery Strategy**
    - Q: What happens when navigator enters invalid state?
    - **Recommendation**: Reset to (p=0, q=0), log error, notify user.

14. **Performance Testing**
    - Q: How to test with 1024-artwork playlists?
    - **Recommendation**: Synthetic test playlists, performance benchmarks.

15. **Backward Compatibility**
    - Q: Support channels without playlist metadata?
    - **Answer**: Yes, treat all posts as single artworks.
    - **Implementation**: Check metadata, default to `is_playlist=false`.

---

## Testing Strategy

### Unit Tests

1. **Playlist Navigator Tests**
   - Test next() with various scenarios
   - Test prev() with various scenarios
   - Test playlist boundary detection
   - Test PE limits (0, 1, 1024)
   - Test play orders (original, created, random)
   - Test random reversibility
   - Test wraparound behavior
   - Test edge cases (empty, single post, etc.)

2. **Buffer Manager Tests**
   - Test buffer fill/drain
   - Test buffer underflow handling
   - Test download completion callbacks
   - Test buffer clear
   - Test capacity limits

3. **Playlist Metadata Tests**
   - Test JSON parsing
   - Test corrupted metadata handling
   - Test missing files
   - Test artwork reference resolution

### Integration Tests

1. **End-to-End Playback**
   - Load channel with playlists
   - Navigate through play queue
   - Verify correct artwork sequence
   - Test all play orders

2. **Download Integration**
   - Test buffer refill during playback
   - Test download failure recovery
   - Test partial playlist availability

3. **Settings Persistence**
   - Test PE changes
   - Test play order changes
   - Test settings persist across reboot

### Manual Testing Scenarios

1. **Basic Playlist**
   - Channel with 1 playlist (10 artworks)
   - Navigate forward through all
   - Navigate backward through all

2. **Mixed Content**
   - Channel with artworks and playlists intermixed
   - Navigate in all play orders

3. **Large Playlist**
   - Playlist with 100+ artworks
   - Test performance
   - Test memory usage

4. **Edge Cases**
   - All scenarios from edge case list above

---

## Implementation Phases

### Phase 1: Foundation (Week 1-2)

**Goal**: Set up core data structures and basic navigation

**Tasks**:
1. Create `playlist_navigator.h/c` skeleton
2. Define data structures (post_info_t, playlist_info_t, etc.)
3. Implement basic next()/prev() without playlists
4. Add NVS storage for settings
5. Unit tests for basic navigation

**Deliverable**: Navigator can iterate through posts (no playlists yet)

### Phase 2: Playlist Metadata (Week 2-3)

**Goal**: Load and parse playlist metadata

**Tasks**:
1. Create `playlist_metadata.h/c`
2. Implement JSON parsing
3. Define playlist storage structure
4. Implement playlist detection
5. Unit tests for metadata handling

**Deliverable**: Can load and parse playlist JSON files

### Phase 3: Playlist Navigation (Week 3-4)

**Goal**: Navigate within playlists using p/q indices

**Tasks**:
1. Implement playlist boundary detection
2. Implement q index tracking
3. Implement PE logic
4. Handle playlist entry/exit
5. Unit tests for playlist navigation

**Deliverable**: Navigator can move through playlists correctly

### Phase 4: Play Orders (Week 4-5)

**Goal**: Implement all three play orders

**Tasks**:
1. Implement original order (pass-through)
2. Implement creation order (pre-sort)
3. Implement random order (use sync_playlist algorithm)
4. Handle order changes
5. Unit tests for each order

**Deliverable**: All play orders work correctly

### Phase 5: Buffer Manager (Week 5-6)

**Goal**: Implement play buffer and prefetching

**Tasks**:
1. Create `playback_buffer.h/c`
2. Implement circular buffer
3. Interface with playlist navigator
4. Implement buffer refill logic
5. Unit tests for buffer operations

**Deliverable**: Buffer prefetches next BN artworks

### Phase 6: Makapix Integration (Week 6-7)

**Goal**: Support playlists from Makapix server

**Tasks**:
1. Extend makapix_channel to recognize playlists
2. Download playlist metadata from server
3. Download playlist artworks
4. Store in `/sdcard/playlists/`
5. Integration tests with live server

**Deliverable**: Can play playlists from Makapix

### Phase 7: Animation Player Integration (Week 7-8)

**Goal**: Connect buffer to animation player

**Tasks**:
1. Modify animation_player_loader to use buffer
2. Implement "Downloading..." display
3. Handle buffer underflow
4. Trigger buffer refills
5. Integration tests

**Deliverable**: Full playback pipeline works

### Phase 8: SD Card Channel Support (Week 8)

**Goal**: Support local playlists on SD card

**Tasks**:
1. Extend sdcard_channel to scan playlists folder
2. Load local playlist metadata
3. Integration tests

**Deliverable**: Can play local playlists

### Phase 9: Polish & Edge Cases (Week 9-10)

**Goal**: Handle all edge cases and polish UX

**Tasks**:
1. Implement all edge case handlers
2. Add error recovery logic
3. Improve logging
4. Performance optimization
5. Memory optimization

**Deliverable**: Robust, production-ready system

### Phase 10: Testing & Documentation (Week 10-11)

**Goal**: Comprehensive testing and documentation

**Tasks**:
1. Full test coverage
2. Manual testing of all scenarios
3. Performance testing
4. Update documentation
5. Create user guide

**Deliverable**: Tested, documented system ready for release

---

## Recommended Implementation Approach

### 1. Incremental Development

- Build each phase on previous foundation
- Maintain working system at each phase
- Avoid "big bang" integration
- Use feature flags to enable/disable playlist support

### 2. Test-Driven Development

- Write unit tests before implementation
- Run tests continuously during development
- Maintain high test coverage (>80%)
- Automate testing where possible

### 3. Code Review & Documentation

- Document all major decisions
- Add inline comments for complex logic
- Review all changes before merging
- Update this plan as needed

### 4. Performance Monitoring

- Track memory usage at each phase
- Profile critical paths (navigation, buffer refill)
- Optimize bottlenecks
- Ensure smooth 60fps playback

### 5. User Feedback Loop

- Test with real users early
- Gather feedback on UX
- Iterate on design
- Be willing to adjust requirements

---

## Alternative Approaches Considered

### Alternative 1: Pre-built Play Queue

**Approach**: Build entire play queue in memory on channel load

**Pros**:
- Simpler navigation (direct indexing)
- Faster next/prev operations
- Easier to implement random order

**Cons**:
- High memory usage (could be 100,000+ entries)
- Not feasible on ESP32-P4
- Slow channel loading
- Wasted computation for unused entries

**Verdict**: ❌ Not viable due to memory constraints

### Alternative 2: Playlists as Separate Channels

**Approach**: Treat each playlist as a mini-channel

**Pros**:
- Reuse existing channel infrastructure
- Simpler code changes
- Clear separation

**Cons**:
- Cannot seamlessly navigate across playlists
- PE setting doesn't make sense
- Breaks play queue abstraction
- Poor UX (user has to "enter" playlists)

**Verdict**: ❌ Doesn't meet requirements

### Alternative 3: Flatten Playlists on Channel Load

**Approach**: Expand all playlists into individual items when loading channel

**Pros**:
- Simple implementation
- No special navigation logic needed

**Cons**:
- PE setting cannot be changed dynamically
- Wastes memory
- Cannot change PE without reloading channel
- Slow channel loading for large playlists

**Verdict**: ❌ Not flexible enough

### Alternative 4: Hybrid Approach (Chosen)

**Approach**: Lazy evaluation with cached current state

**Pros**:
- Low memory usage
- Dynamic PE changes
- Fast navigation
- Meets all requirements

**Cons**:
- More complex implementation
- Requires careful state management

**Verdict**: ✅ **Recommended** - Best balance of features and constraints

---

## Conclusion

Implementing playlist support in p3a is a significant architectural change that touches many components. The recommended approach uses lazy evaluation of the play queue, explicit p/q position tracking, and a bounded play buffer to balance functionality with the ESP32-P4's memory constraints.

Key success factors:
1. **Incremental implementation** - Build in phases
2. **Comprehensive testing** - Cover all edge cases
3. **Careful state management** - Track p/q consistently
4. **Memory efficiency** - Avoid pre-building large structures
5. **User experience** - Handle failures gracefully

The estimated timeline is **10-11 weeks** for a complete implementation, including testing and documentation. The most challenging aspects will be the navigation logic complexity, buffer management, and ensuring smooth UX even when downloads are pending.

This plan provides a solid foundation for successful implementation while remaining flexible enough to adapt to discoveries during development.
