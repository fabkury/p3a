# Playlist Support Implementation Plan

## Document Status

**Last Updated**: 2025-12-10  
**Status**: Planning Phase - No Code Changes Yet

## Executive Summary

This document provides a comprehensive implementation plan for adding full playlist support to p3a. Currently, p3a treats each "post" as a single artwork. This plan extends the system to support posts that can be either:
- A single artwork (current behavior - fully supported)
- A playlist of 1-1024 artworks (new feature - not yet implemented)

## Table of Contents

1. [Background & Current Architecture](#background--current-architecture)
2. [Key Concepts & Requirements](#key-concepts--requirements)
3. [Data Model Changes](#data-model-changes)
4. [Core Abstractions & Algorithms](#core-abstractions--algorithms)
5. [Implementation Approach](#implementation-approach)
6. [Pain Points & Challenges](#pain-points--challenges)
7. [Open Questions](#open-questions)
8. [Testing Strategy](#testing-strategy)

---

## Background & Current Architecture

### Current Playback Behavior

p3a currently operates with the following playback model:

1. **Post = Single Artwork**: Each post in a channel contains exactly one artwork file
2. **Dwell Time**: After playing an animation/image for S seconds (default 30s), automatically swap to next post
3. **Channel Navigation**: Users cycle through posts using touch gestures (forward/back)
4. **No Playlist Support**: Multi-artwork posts are not supported

### Key Components

The current architecture consists of these major components:

#### 1. Channel System (`components/channel_manager/`)
- **`channel_player.c`**: High-level playback controller
  - Manages current position in channel
  - Handles advance/go_back operations
  - Supports both SD card and Makapix channels
  - Stores up to 1000 posts in memory
  - Current structure: `channel_player_state_t` with posts array and indices array

- **`sdcard_channel.c`**: Local SD card channel implementation
  - Scans SD card `/animations` directory
  - Represents each file as a `sdcard_post_t`
  - Currently: one file = one post

- **`channel_interface.h`**: Abstract channel interface
  - Defines `channel_handle_t` and operations
  - Supports multiple order modes: ORIGINAL, CREATED, RANDOM
  - Filter support for NSFW, file formats, etc.

#### 2. Animation Player (`main/`)
- **`animation_player.c`**: Main animation playback manager
  - Double-buffered loading (front/back buffers)
  - Manages animation lifecycle
  - Coordinates with channel_player for next/prev operations

- **`animation_player_loader.c`**: Async animation loading
  - Background task that loads animations from SD card
  - Prefetches next animation during playback
  - Handles format decoding (GIF, WebP, PNG, JPEG)

- **`playback_controller.c`**: Playback state coordinator
  - Tracks current playback source (local/PICO-8/none)
  - Manages animation metadata
  - Coordinates state transitions

#### 3. Display & Rendering (`main/`)
- **`display_renderer.c`**: Core rendering loop
  - Manages frame buffers and vsync
  - Dual-core upscaling workers
  - Frame timing and presentation

#### 4. State Management
- **`p3a_state.h`**: Global state machine
  - Four main states: ANIMATION_PLAYBACK, PROVISIONING, OTA, PICO8_STREAMING
  - Sub-states for each main state
  - State transition rules

#### 5. Configuration & Storage
- **`config_store.c`**: NVS-backed configuration
  - Stores rotation, dwell time, etc.
  - Atomic save/load operations
- **NVS keys used**:
  - `dwell_time`: Auto-swap interval (1-100000 seconds)
  - `rotation`: Screen rotation angle

### Current Data Structures

```c
// From sdcard_channel.h
typedef struct {
    char *name;           // Post name (filename)
    time_t created_at;    // File creation timestamp
    char *filepath;       // Full path for loading
    asset_type_t type;    // GIF, WebP, PNG, JPEG
    bool healthy;         // Load health flag
} sdcard_post_t;

// From channel_player.h
typedef struct {
    sdcard_post_t *posts;  // Array of loaded posts (up to 1000)
    size_t *indices;       // Playback order (indices into posts array)
    size_t count;          // Number of loaded posts
    size_t current_pos;    // Current position in playback order
    bool randomize;        // Randomization enabled
} channel_player_state_t;
```

**Key Observation**: The current `sdcard_post_t` structure represents a single file, not a container for multiple artworks. This is the fundamental limitation we need to address.

---

## Key Concepts & Requirements

### Play Queue (Conceptual, Not Pre-Built)

The **play queue** is a dynamic abstraction that provides an ordered list of **individual artworks** (not posts) for playback. It is NOT stored in memory as a complete array, but derived on-demand.

**Characteristics**:
- Determined by: `current_channel`, `play_order`, `p` (post index), `q` (in-playlist artwork index)
- Accessed only via `next(i)` / `previous(i)` functions
- "Unrolls" playlists according to the Playlist Expansion (PE) setting

**Example Visualization**:
```
Channel C with 10 posts (server order):
  p0 (artwork), p1 (artwork), p2 (artwork), p3 (artwork),
  p4 (playlist: 12 artworks), p5 (artwork), p6 (artwork),
  p7 (artwork), p8 (artwork), p9 (artwork)

If PE = 3 (expand first 3 artworks):
  Play Queue = [p0a, p1a, p2a, p3a, pa0, pa1, pa2, p5a, p6a, p7a, p8a, p9a]
               └─────────────────────┘ └────────┘ └──────────────────────┘
               Regular artworks         First 3    Regular artworks
                                       from p4 
```

### Play Order

Three ordering modes must be supported:

1. **Original Order** (server_seq): Files as ordered by server/SD card scan
2. **Creation Order**: Sorted by post creation date (newest first for channels, oldest first for playlists)
3. **Random Order (seeded)**: Fisher-Yates shuffle with persistent seed
   - **Critical**: Backward navigation must replay the exact same random sequence in reverse

### Playlist Expansion (PE)

**PE** is an integer setting (0-1023) that controls how many artworks from a playlist enter the play queue:
- **PE = 0**: Special value meaning "all artworks" (no limit)
- **PE = 1-1023**: Include first N artworks from the playlist

**Behavior**:
- When play queue encounters a playlist post with N artworks:
  - If PE = 0: Include all N artworks
  - If PE > 0: Include min(PE, N) artworks
- Applies consistently for forward and backward navigation

**Default**: PE = 0 (include all artworks)  
**Persisted**: Yes, stored in NVS

### Play Buffer (Conceptual)

The **play buffer** is the set of BN upcoming artworks that are:
1. Next in the play queue
2. Already downloaded and available on SD card

**Characteristics**:
- **BN** (Buffer Number) defaults to 6, configurable at runtime
- p3a maintains this buffer by prefetching upcoming artworks
- If buffer has < BN items, trigger download of next items
- Each completed download immediately adds to available buffer

### Position Tracking: p and q

Two indices track playback position:

- **p** (post index): Current post in the channel (0-based)
- **q** (in-playlist artwork index): Current artwork within playlist (0-based)
  - If current post is not a playlist: q = 0
  - If current post is a playlist: q = 0 to (count-1)

**Navigation Rules**:

| Operation | Current Context | New p | New q | Notes |
|-----------|----------------|-------|-------|-------|
| `next()` | Artwork → Artwork | p+1 | 0 | Simple advance |
| `next()` | Artwork → Playlist | p+1 | 0 | Enter playlist at first item |
| `next()` | Within Playlist | p | q+1 | Advance within playlist |
| `next()` | Last in Playlist → Artwork | p+1 | 0 | Exit playlist |
| `prev()` | Artwork → Artwork | p-1 | 0 | Simple backward |
| `prev()` | Artwork → Playlist | p-1 | min(PE, count)-1 | Enter playlist at last visible item |
| `prev()` | Within Playlist | p | q-1 | Back within playlist |
| `prev()` | First in Playlist → Artwork | p-1 | 0 | Exit playlist backward |

**Wraparound**:
- When `p` reaches end of channel, wrap to p=0 (re-shuffle if random mode)
- When `p` reaches beginning (going backward), wrap to last post

---

## Data Model Changes

### 1. Extended Post Structure

We need to extend `sdcard_post_t` to support playlists:

```c
typedef enum {
    POST_TYPE_ARTWORK,   // Single artwork (current behavior)
    POST_TYPE_PLAYLIST   // Playlist of artworks (new)
} post_content_type_t;

typedef struct {
    char *name;              // Post name or playlist name
    time_t created_at;       // Post creation timestamp
    post_content_type_t content_type;  // ARTWORK or PLAYLIST
    
    // For ARTWORK posts:
    char *filepath;          // Full path to artwork file
    asset_type_t type;       // GIF, WebP, PNG, JPEG
    bool healthy;            // Load health flag
    
    // For PLAYLIST posts:
    struct {
        char **artwork_paths;   // Array of filepath strings
        asset_type_t *types;    // Array of asset types
        bool *healthy;          // Array of health flags
        size_t count;           // Number of artworks (1-1024)
        char *metadata_path;    // Optional: path to playlist.json
    } playlist;
} sdcard_post_t;  // Extended structure
```

**Memory Implications**:
- Current: ~300 bytes per post × 1000 posts = ~300KB
- With playlists: Depends on playlist size
  - Worst case: 1000 posts × 1024 artworks × 256 bytes/path = ~250MB (NOT FEASIBLE)
  - **Solution**: Don't pre-load all playlist artwork paths; load on-demand

### 2. On-Demand Playlist Loading

To avoid memory explosion, playlists must be loaded lazily:

```c
// Lightweight playlist descriptor (stored in posts array)
typedef struct {
    char *playlist_dir;      // Directory containing numbered files
    size_t artwork_count;    // Total artworks in playlist
    char *index_file;        // Optional: index.json with explicit ordering
} playlist_descriptor_t;

// Extended post structure (memory-efficient)
typedef struct {
    char *name;
    time_t created_at;
    post_content_type_t content_type;
    
    union {
        // For ARTWORK posts
        struct {
            char *filepath;
            asset_type_t type;
            bool healthy;
        } artwork;
        
        // For PLAYLIST posts
        playlist_descriptor_t playlist;
    } content;
} sdcard_post_t;  // Revised structure

// Accessor function (loads on-demand)
esp_err_t playlist_get_artwork_path(const playlist_descriptor_t *playlist, 
                                   size_t index, 
                                   char *out_path, 
                                   size_t path_size,
                                   asset_type_t *out_type);
```

**On-Demand Loading Strategy**:
1. During channel scan, detect playlist directories (e.g., directories with numbered files)
2. Store only: playlist directory path + artwork count
3. When playback needs artwork at position `q`: construct path dynamically or read from index file

### 3. NVS Storage for New Settings

New NVS keys needed:

```c
#define PE_NVS_KEY "playlist_exp"     // Playlist Expansion (uint16_t, 0-1023)
#define PLAY_ORDER_NVS_KEY "play_order"  // Play order mode (uint8_t enum)
#define RANDOM_SEED_NVS_KEY "rand_seed"  // Random order seed (uint32_t)
#define BN_NVS_KEY "buffer_num"       // Play buffer size (uint8_t, 1-20)
```

### 4. Playback Position State

Extend channel_player state to track `p` and `q`:

```c
typedef struct {
    sdcard_post_t *posts;    // Array of loaded posts (up to 1000)
    size_t *indices;         // Playback order (indices into posts array)
    size_t count;            // Number of loaded posts
    
    // Position tracking
    size_t p;                // Current post index (in indices array)
    size_t q;                // Current artwork within playlist (0 if not playlist)
    
    // Playback settings
    bool randomize;          // Randomization enabled
    uint32_t random_seed;    // Seed for random order (for reproducibility)
    uint16_t playlist_expansion;  // PE value (0 = all, 1-1023 = limit)
    
    // Play buffer tracking
    uint8_t buffer_number;   // BN value (target buffer size)
    // Note: Actual buffer is managed by animation_player_loader
} channel_player_state_t;  // Extended structure
```

---

## Core Abstractions & Algorithms

### 1. Play Queue Iterator

The play queue is accessed through iterator functions:

```c
// Play queue item (what iterator returns)
typedef struct {
    size_t p;              // Post index
    size_t q;              // Artwork index within post (0 if artwork post)
    char *filepath;        // Path to artwork file
    asset_type_t type;     // Artwork type
} play_queue_item_t;

// Iterator functions
esp_err_t play_queue_current(channel_player_state_t *state, 
                             play_queue_item_t *out_item);

esp_err_t play_queue_next(channel_player_state_t *state, 
                          play_queue_item_t *out_item);

esp_err_t play_queue_previous(channel_player_state_t *state, 
                              play_queue_item_t *out_item);
```

**Pseudocode for `play_queue_next()`**:

```python
def play_queue_next(state, out_item):
    current_post = state.posts[state.indices[state.p]]
    
    if current_post.content_type == ARTWORK:
        # Simple case: advance to next post
        state.p = (state.p + 1) % state.count
        state.q = 0
        
        # Handle wraparound re-shuffle
        if state.p == 0 and state.randomize:
            reshuffle(state.indices, state.count, state.random_seed)
    
    elif current_post.content_type == PLAYLIST:
        # Within playlist
        effective_count = current_post.playlist.artwork_count
        if state.playlist_expansion > 0:
            effective_count = min(effective_count, state.playlist_expansion)
        
        if state.q + 1 < effective_count:
            # Stay in same playlist, advance q
            state.q += 1
        else:
            # Exit playlist, advance to next post
            state.p = (state.p + 1) % state.count
            state.q = 0
            
            if state.p == 0 and state.randomize:
                reshuffle(state.indices, state.count, state.random_seed)
    
    # Resolve to actual artwork path
    return resolve_to_artwork(state, state.p, state.q, out_item)
```

**Pseudocode for `play_queue_previous()`**:

```python
def play_queue_previous(state, out_item):
    if state.q > 0:
        # Within playlist, go back one artwork
        state.q -= 1
    else:
        # At beginning of post (or artwork post), go to previous post
        if state.p == 0:
            state.p = state.count - 1  # Wrap to end
        else:
            state.p -= 1
        
        prev_post = state.posts[state.indices[state.p]]
        if prev_post.content_type == PLAYLIST:
            # Enter playlist at last visible artwork
            effective_count = prev_post.playlist.artwork_count
            if state.playlist_expansion > 0:
                effective_count = min(effective_count, state.playlist_expansion)
            state.q = effective_count - 1
        else:
            state.q = 0
    
    return resolve_to_artwork(state, state.p, state.q, out_item)
```

**Helper: `resolve_to_artwork()`**:

```python
def resolve_to_artwork(state, p, q, out_item):
    post = state.posts[state.indices[p]]
    
    out_item.p = p
    out_item.q = q
    
    if post.content_type == ARTWORK:
        out_item.filepath = post.content.artwork.filepath
        out_item.type = post.content.artwork.type
    elif post.content_type == PLAYLIST:
        # Load artwork path on-demand
        playlist_get_artwork_path(
            post.content.playlist,
            q,
            out_item.filepath,
            sizeof(out_item.filepath),
            &out_item.type
        )
    
    return ESP_OK
```

### 2. Random Order with Reproducible Backward

**Challenge**: Random order must be reproducible when navigating backward.

**Solution**: Store random seed and regenerate the same shuffle sequence:

```python
def reshuffle(indices, count, seed):
    """Fisher-Yates shuffle with deterministic seed"""
    rng = Random(seed)
    for i in range(count - 1, 0, -1):
        j = rng.randint(0, i)
        indices[i], indices[j] = indices[j], indices[i]

def init_random_order(state):
    """Initialize random order on first use or channel reload"""
    if state.random_seed == 0:
        state.random_seed = esp_random()  # Generate new seed
        save_random_seed_to_nvs(state.random_seed)
    
    reshuffle(state.indices, state.count, state.random_seed)

def go_back_in_random_order(state):
    """Navigate backward in random order"""
    # Key insight: We track history of (p, q) positions
    # When going back, we restore the previous (p, q) from history
    # This is simpler than trying to "reverse" the shuffle
    
    # Implementation uses a fixed-size circular history buffer:
    state.history[(state.history_pos - 1) % HISTORY_SIZE]
```

**Alternative Approach (Simpler)**:
- Maintain a small history buffer (e.g., 20 recent positions)
- Going backward = pop from history
- Going forward beyond history = compute next normally
- Tradeoff: Limited backward navigation depth

### 3. Play Buffer Management

The play buffer ensures smooth playback by prefetching upcoming artworks:

```python
def maintain_play_buffer(state, animation_loader):
    """Ensure play buffer has BN artworks ready"""
    
    # Query what's currently available
    available = get_available_artwork_count(state)
    
    if available < state.buffer_number:
        # Need to download more
        needed = state.buffer_number - available
        
        # Iterate forward through play queue to find next 'needed' artworks
        temp_p, temp_q = state.p, state.q
        for i in range(needed):
            item = play_queue_peek_ahead(state, temp_p, temp_q, i+1)
            
            if not is_artwork_available(item.filepath):
                # Queue download
                queue_artwork_download(item.filepath, item.type)
            
            # Advance temp position
            temp_p, temp_q = compute_next_position(temp_p, temp_q)

def is_artwork_available(filepath):
    """Check if artwork is fully downloaded and ready"""
    # For SD card files: always available (no download needed)
    # For Makapix channels: check if file exists in vault cache
    return file_exists(filepath) and file_complete(filepath)
```

### 4. Playlist Directory Detection

During SD card channel scan, detect playlists:

```python
def scan_channel_directory(dir_path):
    """Scan directory and identify posts"""
    posts = []
    
    for entry in list_directory(dir_path):
        if is_directory(entry):
            # Check if this is a playlist directory
            if is_playlist_directory(entry):
                playlist = create_playlist_post(entry)
                posts.append(playlist)
        elif is_animation_file(entry):
            # Regular artwork post
            artwork = create_artwork_post(entry)
            posts.append(artwork)
    
    return posts

def is_playlist_directory(dir_path):
    """Heuristic to detect playlist directories"""
    # Strategy 1: Look for index.json or playlist.json
    if file_exists(join(dir_path, "index.json")):
        return True
    if file_exists(join(dir_path, "playlist.json")):
        return True
    
    # Strategy 2: Look for numbered files (001.gif, 002.gif, etc.)
    files = list_files(dir_path)
    animation_files = [f for f in files if is_animation_file(f)]
    
    # Check if files follow numbered pattern
    if len(animation_files) > 1:
        # Simple check: all files have numeric names
        all_numeric = all(is_numeric_name(f) for f in animation_files)
        if all_numeric:
            return True
    
    return False

def create_playlist_post(dir_path):
    """Create a playlist post descriptor"""
    # Count artworks in directory
    animation_files = find_animation_files(dir_path)
    animation_files.sort()  # Sort for consistent ordering
    
    return {
        "name": basename(dir_path),
        "created_at": get_dir_create_time(dir_path),
        "content_type": POST_TYPE_PLAYLIST,
        "playlist": {
            "playlist_dir": dir_path,
            "artwork_count": len(animation_files),
            "index_file": find_index_file(dir_path)  # Optional
        }
    }
```

**Playlist Directory Conventions**:
1. **Named files**: `001.gif`, `002.webp`, `003.png`, etc. (sorted alphabetically)
2. **With index file**: `index.json` specifies explicit ordering:
   ```json
   {
     "name": "My Playlist",
     "artworks": [
       {"path": "scene1.gif", "type": "gif"},
       {"path": "scene2.webp", "type": "webp"},
       {"path": "scene3.gif", "type": "gif"}
     ]
   }
   ```

---

## Implementation Approach

### Phase 1: Data Model & Storage (Foundation)

**Goal**: Extend data structures without breaking existing functionality

**Tasks**:
1. **Extend `sdcard_post_t`**:
   - Add `post_content_type_t` enum
   - Add union for artwork vs. playlist content
   - Implement accessor functions

2. **Add NVS keys**:
   - Playlist Expansion (PE)
   - Play Order mode
   - Random seed
   - Buffer Number (BN)

3. **Extend `channel_player_state_t`**:
   - Add `p` and `q` indices
   - Add `playlist_expansion`, `random_seed`, `buffer_number` fields

4. **Create helper module**: `playlist_utils.c`
   - `playlist_get_artwork_path()`: On-demand artwork path resolution
   - `playlist_detect_directory()`: Identify playlist directories
   - `playlist_load_index()`: Parse index.json if present

**Files to modify**:
- `components/channel_manager/include/sdcard_channel.h`
- `components/channel_manager/sdcard_channel_impl.c`
- `components/channel_manager/include/channel_player.h`
- `components/channel_manager/channel_player.c`
- Create: `components/channel_manager/playlist_utils.c` (new)

**Testing**:
- Unit tests for playlist detection
- Verify backward compatibility (existing artwork-only channels work)

### Phase 2: Play Queue Iterator (Core Logic)

**Goal**: Implement play queue navigation with playlist support

**Tasks**:
1. **Implement play queue functions**:
   - `play_queue_current()`
   - `play_queue_next()`
   - `play_queue_previous()`
   - Helper: `resolve_to_artwork()`

2. **Integrate into channel_player**:
   - Modify `channel_player_advance()` to use `play_queue_next()`
   - Modify `channel_player_go_back()` to use `play_queue_previous()`
   - Update `channel_player_get_current_post()` to resolve current (p, q)

3. **Position history tracking**:
   - Implement circular buffer for last 20 positions (for backward nav in random mode)
   - Store (p, q) pairs in history on each advance

**Files to modify**:
- `components/channel_manager/channel_player.c` (major refactor)
- Create: `components/channel_manager/play_queue.c` (new)

**Testing**:
- Test navigation through mixed channel (artworks + playlists)
- Test PE = 0 (all), PE = 3 (limited)
- Test wraparound behavior
- Test random order backward navigation

### Phase 3: Playlist Detection & Loading (Channel Scan)

**Goal**: Detect and load playlists during channel refresh

**Tasks**:
1. **Modify SD card scan**:
   - In `sdcard_channel_refresh()`, detect directories
   - Use `is_playlist_directory()` heuristic
   - Create `sdcard_post_t` with `POST_TYPE_PLAYLIST`

2. **Implement index.json parsing**:
   - Use cJSON library (already in project)
   - Parse artwork array
   - Handle missing/malformed index gracefully

3. **Update channel statistics**:
   - Channel stats should report total posts (not total artworks)
   - Add new stat: `total_artworks` (sum of all artworks across all posts)

**Files to modify**:
- `components/channel_manager/sdcard_channel_impl.c`
- `components/channel_manager/playlist_utils.c`

**Testing**:
- Test with sample playlist directories
- Test fallback when index.json missing
- Test mixed channels (artworks + playlists)

### Phase 4: Animation Player Integration (Buffer Management)

**Goal**: Update animation player to work with (p, q) positions

**Tasks**:
1. **Modify animation_player.c**:
   - Track current (p, q) instead of just post index
   - Update swap logic to advance via play queue

2. **Modify animation_player_loader.c**:
   - Implement play buffer management (maintain BN artworks ahead)
   - Prefetch using play queue iterator
   - Handle on-demand playlist artwork loading

3. **Update dwell time handling**:
   - Dwell time applies per-artwork (not per-post)
   - Auto-advance triggers `play_queue_next()`

**Files to modify**:
- `main/animation_player.c`
- `main/animation_player_loader.c`
- `main/animation_player_priv.h`

**Testing**:
- Test playback through playlists
- Test auto-advance from artwork to playlist
- Test buffer prefetching
- Verify dwell time behavior

### Phase 5: Settings & Configuration (User Control)

**Goal**: Add user-facing settings for playlist behavior

**Tasks**:
1. **Add HTTP API endpoints**:
   - `GET/PUT /settings/playlist_expansion`
   - `GET/PUT /settings/play_order`
   - `GET/PUT /settings/buffer_number`

2. **Update web UI**:
   - Add settings page for playlist configuration
   - Show current play order
   - Allow changing PE, play order, BN

3. **NVS persistence**:
   - Save/load settings from NVS
   - Apply settings on boot

**Files to modify**:
- `components/http_api/http_api.c`
- `webui/` (HTML/JavaScript files)
- `main/p3a_main.c` (load settings on boot)

**Testing**:
- Test settings persistence across reboot
- Test changing PE during playback
- Test play order switching

### Phase 6: Random Order & History (Advanced Navigation)

**Goal**: Implement reproducible random order with backward support

**Tasks**:
1. **Implement seeded random shuffle**:
   - Use `esp_random()` for seed generation
   - Implement Fisher-Yates with seed

2. **Position history buffer**:
   - Circular buffer of (p, q) pairs (size 20)
   - Push on advance, pop on go_back
   - Handle history exhaustion gracefully

3. **Save/restore random seed**:
   - Store in NVS
   - Regenerate same sequence on restart

**Files to modify**:
- `components/channel_manager/channel_player.c`
- Add: `components/channel_manager/random_order.c` (new)

**Testing**:
- Test random order reproducibility
- Test backward navigation in random mode
- Test history buffer wraparound
- Test seed persistence

### Phase 7: UI & UX Polish (User Experience)

**Goal**: Provide feedback and polish the user experience

**Tasks**:
1. **Add playlist indicators**:
   - Show "X of Y" when in playlist (e.g., "2 of 12")
   - Display playlist name on screen

2. **Loading messages**:
   - "Downloading artworks..." when buffer empty
   - Progress indicators during download

3. **Touch gesture enhancements**:
   - Consider: Different gesture for skip-playlist vs. next-in-playlist
   - (Optional) Long-press to skip entire playlist

4. **Error handling**:
   - Handle corrupted playlists gracefully
   - Skip missing artworks in playlist

**Files to modify**:
- `main/ugfx_ui.c` (add UI elements)
- `main/app_touch.c` (gesture handling)
- `components/p3a_core/p3a_state.c` (channel messages)

**Testing**:
- Manual testing of all UI elements
- Test error cases (missing files, corrupted playlists)

---

## Pain Points & Challenges

### 1. Memory Management

**Challenge**: Loading 1000 playlists × 1024 artworks = up to 250MB of path strings

**Solution**: On-demand loading strategy
- Store only directory paths + counts in memory
- Construct full paths dynamically when needed
- Memory footprint: ~1KB per playlist post

**Risk**: Performance degradation if constructing paths is slow
- **Mitigation**: Cache recently-used paths in LRU cache (e.g., last 10 artworks)

### 2. Backward Navigation in Random Order

**Challenge**: Random shuffle is non-reversible

**Solution Options**:
1. **History Buffer** (Chosen):
   - Store last N (p, q) positions
   - Trade-off: Limited backward depth
   - Pros: Simple, bounded memory
   - Cons: Can't go back beyond history size

2. **Regenerate Shuffle**:
   - Store seed, regenerate shuffle, then "walk backward"
   - Pros: Unlimited backward navigation
   - Cons: Complex, CPU-intensive

3. **Bidirectional Shuffle**:
   - Store both forward and reverse shuffle arrays
   - Pros: Fast, unlimited backward
   - Cons: 2× memory usage

**Recommendation**: Start with history buffer (20 positions), can upgrade later if needed

### 3. Playlist Detection Heuristics

**Challenge**: How to distinguish playlist directories from regular directories?

**Heuristics** (ordered by priority):
1. Presence of `index.json` or `playlist.json` → definitely a playlist
2. Directory contains 2+ animation files with numeric names → likely a playlist
3. Directory name matches pattern (e.g., "playlist_*") → maybe a playlist

**Risk**: False positives (treating non-playlist dirs as playlists)
- **Mitigation**: Require at least 2 artworks to consider something a playlist

**Risk**: False negatives (missing actual playlists)
- **Mitigation**: Document playlist naming conventions clearly

### 4. Dwell Time Semantics

**Current**: Dwell time applies per-post  
**New**: Dwell time applies per-artwork (within playlist)

**Challenge**: User expectation management
- If user has PE=0 and encounters 100-artwork playlist, it will play for 100 × dwell_time
- This might be unexpected

**Solution Options**:
1. Keep current behavior (per-artwork dwell time) - simpler, more consistent
2. Add "playlist dwell time multiplier" - more complex, more control
3. Add "max playlist play time" setting - prevents excessively long playlists

**Recommendation**: Start with option 1 (per-artwork), add option 3 later if users complain

### 5. Makapix Channel Integration

**Challenge**: Makapix channels currently use `channel_interface.h` abstraction, which doesn't have explicit playlist support

**Current State**:
- Makapix channels deliver artworks via MQTT + vault cache
- Channel implementation is in `makapix_channel_impl.c`
- Uses `channel_item_ref_t` (single item) not playlist-aware

**Integration Path**:
1. Extend `channel_item_ref_t` to support playlist flag + count
2. Makapix server must send playlist metadata via MQTT
3. Makapix channel impl must parse playlist messages
4. Vault cache must handle playlist downloads

**Risk**: Backend changes required (not just device-side)
- **Mitigation**: Phase 1-7 focus on SD card channels; Makapix integration is Phase 8

### 6. SD Card I/O Performance

**Challenge**: On-demand path construction requires frequent filesystem operations

**Concerns**:
- Listing directory contents to find numbered files
- Repeated stat() calls
- Potential SDIO bus contention

**Mitigation Strategies**:
1. **Cache directory listings**: When entering a playlist, enumerate once and cache
2. **Predictable naming**: Enforce numbered file convention (001.gif, 002.gif, ...)
3. **Async prefetch**: Load next artwork while current one is playing

**Measurement**: Profile actual I/O times on target hardware (ESP32-P4)

### 7. State Machine Complexity

**Challenge**: Adding (p, q) tracking increases state management complexity

**Current Complexity**: `channel_player_state_t` already has:
- posts array (1000 entries)
- indices array (1000 entries)  
- current position
- randomization flag

**New Complexity**: 
- Two position indices (p, q)
- History buffer (20 entries)
- PE and BN settings

**Risk**: Harder to debug, more edge cases

**Mitigation**:
1. **Extensive unit tests**: Cover all transition cases
2. **State validation**: Add assertions to detect invalid states
3. **Logging**: Trace position changes at debug level
4. **State dumping**: Add debug function to dump full state

---

## Open Questions

### 1. Playlist Metadata Format

**Question**: What format should `index.json` use?

**Option A - Simple array**:
```json
{
  "name": "My Playlist",
  "artworks": [
    "001.gif",
    "002.webp",
    "003.gif"
  ]
}
```

**Option B - Detailed objects**:
```json
{
  "name": "My Playlist",
  "created_at": "2025-12-10T00:00:00Z",
  "artworks": [
    {
      "path": "scene1.gif",
      "name": "Opening Scene",
      "duration_override": 5000
    },
    {
      "path": "scene2.webp",
      "name": "Main Scene"
    }
  ]
}
```

**Recommendation**: Start with Option A (simpler), extend to Option B later if needed

### 2. Nested Playlist Support?

**Question**: Should playlists be allowed to contain other playlists?

**Requirement says**: "A playlist cannot contain other playlists"

**Follow-up**: Should the system detect and reject nested playlists, or just not support them?

**Recommendation**: Don't actively support nesting; if detected, treat nested dirs as regular artworks or skip them

### 3. PE Persistence Scope

**Question**: Should PE be global or per-channel?

**Option A - Global**: One PE setting for all channels  
**Option B - Per-Channel**: Each channel remembers its own PE

**Pros of Global**: Simpler, consistent behavior  
**Pros of Per-Channel**: More flexible, user can set different PE for different contexts

**Recommendation**: Start with global (simpler), add per-channel later if requested

### 4. Play Order Transitions

**Question**: When user changes play order (e.g., from ORIGINAL to RANDOM), what happens to current position?

**Option A**: Keep playing current artwork, new order applies from next advance  
**Option B**: Immediately re-position in new order (might skip or repeat artworks)  
**Option C**: Restart channel from beginning with new order

**Recommendation**: Option A (least surprising, smoothest transition)

### 5. Download Progress UI

**Question**: When play buffer is filling (downloading artworks), what should UI show?

**Options**:
1. Continue playing current artwork, silent background download
2. Show "Buffering..." overlay with progress bar
3. Show "Downloading X of Y artworks..." message

**Recommendation**: 
- If buffer has ≥1 artwork: Option 1 (silent background)
- If buffer is empty: Option 3 (show download message)

### 6. Corrupted Playlist Handling

**Question**: If a playlist has some corrupted/missing artworks, what should happen?

**Options**:
1. Skip entire playlist
2. Skip only corrupted artworks, play the rest
3. Try to heal playlist (re-download if Makapix source)

**Recommendation**: Option 2 (skip corrupted, play healthy ones)

### 7. Maximum Playlist Size

**Question**: Should there be a configurable max playlist size (lower than 1024)?

**Concern**: 1024-artwork playlist with dwell_time=30s = 8.5 hours of continuous playback

**Recommendation**: 
- Support full 1024 as specified
- Add optional "max playlist time" setting (e.g., "skip to next post after N minutes")

---

## Testing Strategy

### Unit Tests

Create unit tests for core algorithms:

1. **Play Queue Navigation**:
   - Test `play_queue_next()` with various PE values
   - Test `play_queue_previous()` with playlists
   - Test wraparound behavior
   - Test PE boundary cases (PE=0, PE=1, PE>artwork_count)

2. **Playlist Detection**:
   - Test `is_playlist_directory()` with various directory structures
   - Test index.json parsing (valid, invalid, missing)
   - Test numbered file detection

3. **Random Order**:
   - Test shuffle reproducibility (same seed = same order)
   - Test history buffer (push/pop, wraparound)

4. **On-Demand Loading**:
   - Test `playlist_get_artwork_path()` with numbered files
   - Test with index.json
   - Test with missing files

### Integration Tests

Test full system behavior:

1. **Mixed Channel Playback**:
   - Create test SD card with: 5 artworks, 3 playlists (various sizes)
   - Test forward navigation through entire channel
   - Test backward navigation
   - Test wraparound

2. **PE Variations**:
   - Test PE=0 (all artworks)
   - Test PE=1 (one artwork per playlist)
   - Test PE=5 with mix of small and large playlists

3. **Play Order Modes**:
   - Test ORIGINAL order playback
   - Test CREATED order (by date)
   - Test RANDOM order (forward + backward)

4. **Settings Persistence**:
   - Change PE, play_order, BN
   - Reboot device
   - Verify settings restored correctly

### Manual Testing

Critical user scenarios:

1. **First-Time Experience**:
   - Fresh SD card with playlists
   - Verify detection and initial playback

2. **Navigation Feel**:
   - Test touch gestures (forward/back)
   - Verify smooth transitions
   - Check for any lag or stuttering

3. **Edge Cases**:
   - Empty playlists (0 artworks)
   - Single-artwork "playlists"
   - Very large playlists (100+ artworks)
   - Corrupted playlists (missing files)

4. **Settings UI**:
   - Change PE via web interface
   - Verify immediate effect
   - Test invalid inputs (negative PE, etc.)

### Performance Testing

Measure key metrics:

1. **Channel Scan Time**:
   - Measure `sdcard_channel_refresh()` duration with 100 posts (mix of artworks + playlists)
   - Target: < 5 seconds

2. **Navigation Latency**:
   - Measure time from touch gesture to next artwork appearing
   - Target: < 500ms

3. **Memory Usage**:
   - Measure heap usage with 1000 posts loaded (including playlists)
   - Target: < 500KB for channel data structures

4. **SD Card I/O**:
   - Measure I/O operations during playlist navigation
   - Ensure no blocking reads in render loop

---

## Implementation Checklist

### Phase 1: Foundation
- [ ] Extend `sdcard_post_t` with `post_content_type_t`
- [ ] Add union for artwork vs. playlist content
- [ ] Add NVS keys for PE, play_order, random_seed, BN
- [ ] Extend `channel_player_state_t` with p, q, PE, BN
- [ ] Create `playlist_utils.c` module
- [ ] Implement `playlist_get_artwork_path()`
- [ ] Unit tests for playlist utilities

### Phase 2: Core Logic
- [ ] Implement `play_queue_current()`
- [ ] Implement `play_queue_next()`
- [ ] Implement `play_queue_previous()`
- [ ] Implement `resolve_to_artwork()`
- [ ] Integrate into `channel_player_advance()`
- [ ] Integrate into `channel_player_go_back()`
- [ ] Add position history buffer
- [ ] Unit tests for play queue navigation

### Phase 3: Detection & Loading
- [ ] Modify `sdcard_channel_refresh()` for directory scanning
- [ ] Implement `is_playlist_directory()` heuristic
- [ ] Implement index.json parsing
- [ ] Create playlist post descriptors
- [ ] Update channel statistics
- [ ] Integration tests with sample playlists

### Phase 4: Animation Player
- [ ] Update animation_player to track (p, q)
- [ ] Modify swap logic for play queue
- [ ] Implement play buffer management
- [ ] Update prefetch logic
- [ ] Update dwell time handling
- [ ] Test playback through playlists

### Phase 5: Settings & API
- [ ] Add HTTP endpoints for PE, play_order, BN
- [ ] Update web UI settings page
- [ ] Implement NVS persistence
- [ ] Load settings on boot
- [ ] Test settings across reboot

### Phase 6: Random Order
- [ ] Implement seeded Fisher-Yates shuffle
- [ ] Implement position history buffer
- [ ] Save/restore random seed
- [ ] Test backward navigation in random mode

### Phase 7: Polish
- [ ] Add playlist indicators to UI
- [ ] Add loading/buffering messages
- [ ] Enhance error handling
- [ ] Add debug logging
- [ ] Manual testing and refinement

---

## Conclusion

Implementing full playlist support in p3a is a significant undertaking that touches many parts of the codebase. The key challenges are:

1. **Memory management**: Using on-demand loading to avoid memory explosion
2. **State complexity**: Tracking (p, q) positions and maintaining play queue abstraction
3. **Backward navigation**: Implementing reproducible random order with history
4. **User experience**: Smooth transitions and clear feedback

The phased approach allows for incremental development and testing. Each phase builds on the previous, with clear testing checkpoints.

**Estimated Development Time**:
- Phase 1-2: 1-2 weeks (foundation + core logic)
- Phase 3-4: 1-2 weeks (detection + animation integration)
- Phase 5-6: 1 week (settings + random order)
- Phase 7: 1 week (polish + testing)
- **Total**: 4-6 weeks

**Risk Level**: Medium-High
- High: Significant refactoring of core playback logic
- Mitigated by: Phased approach, extensive testing, backward compatibility focus

**Next Steps**:
1. Review this plan with stakeholders
2. Clarify open questions
3. Begin Phase 1 implementation
4. Set up test fixtures (sample playlists on SD card)
