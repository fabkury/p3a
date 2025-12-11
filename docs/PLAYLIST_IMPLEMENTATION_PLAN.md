# Playlist Support Feature - Implementation Plan

## Document Status

**Last Updated**: 2025-12-11

**Purpose**: This document provides a comprehensive implementation plan for adding full playlist support to p3a, including play queue abstraction, playlist expansion, multiple play orders (original, creation, random), and Live Mode synchronization.

## Overview

This document outlines the implementation of playlist support for p3a. Currently, p3a treats posts as equivalent to individual artworks. This implementation will enable posts to be either individual artworks OR playlists (containing 1-1024 artworks). The implementation introduces:

1. **Play Queue Abstraction**: On-the-fly derivation of artwork sequence based on current state (post index, in-playlist index, play order, playlist expansion)
2. **Playlist Expansion (PE)**: Configurable parameter (0-1023) controlling how many artworks from a playlist are included in play queue (0 = all)
3. **Play Orders**: Original (server), creation date, and random with reversible PCG-based randomization
4. **Play Buffer**: Maintains BN (default 6) artworks ready for playback, downloading on-demand
5. **Live Mode**: Synchronized random playback across all p3a devices using the sync_playlist component

## Background

### Current System Architecture

The existing p3a codebase has:

- **channel_player** (`components/channel_manager/channel_player.c`): Manages post-level navigation
  - Loads posts from sdcard_channel or Makapix channel
  - Implements advance/go_back navigation
  - Supports randomization (Fisher-Yates shuffle)
  - Currently treats each post as a single artwork

- **animation_player** (`main/animation_player.c`): Handles animation loading and rendering
  - Manages front/back buffer for smooth transitions
  - Auto-swap mechanism with dwell time
  - Loader task for async loading from SD card

- **playback_controller** (`main/playback_controller.c`): Tracks playback source and metadata
  - Switches between different playback sources (animations, PICO-8 streaming)

- **sync_playlist** (`components/sync_playlist/`): Reversible PCG-based synchronized playlist
  - Currently unused but ready for Live Mode
  - Implements perfect synchronization using only NTP + shared seed
  - Supports forward/backward navigation with deterministic reversibility

- **makapix_api** (`components/makapix/makapix_api.c`): MQTT-based server communication
  - Query posts with pagination
  - Currently doesn't expose PE parameter

### Current Post Structure

From `components/channel_manager/include/sdcard_channel.h`:
```c
typedef struct {
    char *name;           // Post name
    time_t created_at;    // Creation timestamp
    char *filepath;       // Full path for loading
    asset_type_t type;    // GIF, WebP, etc.
    bool healthy;         // Load health flag
} sdcard_post_t;
```

**Key Issue**: No field exists to distinguish between artwork-type posts and playlist-type posts.

### Current Navigation Flow

1. User triggers swap (dwell time expires, touch gesture, etc.)
2. `channel_player_advance()` increments post index
3. `animation_player_request_swap_current()` loads current post's artwork
4. Loader task loads animation into back buffer
5. Swap occurs on next render cycle

**Key Issue**: No concept of navigating within a playlist (q index).

## Requirements Summary

### Core Concepts

1. **Post Types**:
   - **Artwork-type post**: Single artwork file
   - **Playlist-type post**: 1-1024 artworks, stored in `/sdcard/playlists/{playlist_id}/`

2. **Play Queue**:
   - Virtual sequence of artworks (not pre-built in memory)
   - Determined by: current channel, play order, post index (p), in-playlist index (q), PE
   - Accessed via `next(i)` and `prev(i)` functions

3. **Navigation Indices**:
   - **p (post index)**: Current post in channel
   - **q (in-playlist artwork index)**: Current artwork within playlist (0 if post is artwork)

4. **Playlist Expansion (PE)**:
   - Integer 0-1023 (0 = "infinite expansion", all artworks)
   - Determines how many artworks from each playlist appear in play queue
   - Global per-device setting (persisted in NVS)
   - Server queries include PE to pre-fetch correct number of playlist artworks

5. **Play Orders**:
   - **Original**: Server order (server_seq)
   - **Creation**: Ordered by created_at timestamp
   - **Random**: PCG-based randomization with seed
     - Must be reversible (backward navigation retraces exact sequence)
     - Separate setting: **randomize_playlist** (default OFF)
       - OFF: Only post order is randomized, playlist internal order preserved
       - ON: Playlists are internally randomized using playlist-specific seed

6. **Play Buffer**:
   - Target size: BN artworks (default 6, runtime configurable)
   - Contains artworks scheduled next in play queue AND already downloaded
   - Downloads prioritize artworks coming sooner in play order
   - No interruption of ongoing downloads

7. **Live Mode**:
   - Binary ON/OFF setting (only meaningful when play order = RANDOM)
   - All p3a devices play same artworks at same time
   - Leverages `sync_playlist` component
   - Requires NTP synchronization

### Detailed Behavior Specifications

#### Navigation Behavior

**Swapping Forward (next)**:
- If current post is artwork (q=0):
  - Increment p, check if new post is playlist or artwork
  - If new post is artwork: q remains 0
  - If new post is playlist: q = 0 (first artwork in playlist)
  
- If current post is playlist (q > 0):
  - If q < PE-1 (or q < playlist_length-1 if PE=0): increment q
  - Else: increment p, set q = 0

**Swapping Backward (prev)**:
- If current post is artwork (q=0):
  - Decrement p, check if new post is playlist or artwork
  - If new post is artwork: q remains 0
  - If new post is playlist: q = min(PE, playlist_length) - 1 (last expanded artwork)
  
- If current post is playlist (q > 0):
  - If q > 0: decrement q
  - Else: decrement p, set q appropriately for new post

#### Random Order with Reversibility

**Post-Level Randomization**:
- Use PCG instance with channel-level seed
- Maintain PCG state to ensure backward navigation exactly reverses forward
- When wrapping around end of channel, reseed PCG for new cycle

**Playlist-Level Randomization** (when randomize_playlist = ON):
- Each playlist has its own seed (from metadata or derived from playlist_id)
- Use separate PCG instance per playlist
- Internal order must be reversible independently of post-level order

#### Server Communication

**Query Posts with PE**:
```json
{
  "channel": "promoted",
  "sort": "random",
  "limit": 50,
  "PE": 3,  // Request first 3 artworks from each playlist
  "random_seed": 42
}
```

**Response Structure** (hypothetical):
```json
{
  "posts": [
    {
      "post_id": 123,
      "kind": "artwork",
      "storage_key": "abc123.webp",
      "art_url": "https://...",
      "created_at": "2024-01-15T10:30:00Z"
    },
    {
      "post_id": 456,
      "kind": "playlist",
      "playlist_id": "pl-789",
      "playlist_seed": 67890,  // Optional: for randomization
      "created_at": "2024-01-20T14:00:00Z",
      "artworks": [
        {
          "storage_key": "def456.gif",
          "art_url": "https://...",
          "created_at": "2024-01-20T13:45:00Z"
        },
        {
          "storage_key": "ghi789.webp",
          "art_url": "https://...",
          "created_at": "2024-01-20T13:50:00Z"
        },
        {
          "storage_key": "jkl012.webp",
          "art_url": "https://...",
          "created_at": "2024-01-20T13:55:00Z"
        }
      ]
    }
  ]
}
```

#### Storage Structure

**Playlists on SD Card**:
```
/sdcard/
├── vault/                 # Individual artworks (existing)
│   ├── ab/cd/abc123.webp
│   └── de/f4/def456.gif
└── playlists/             # Playlists (NEW)
    ├── pl-789/            # One folder per playlist
    │   ├── metadata.json  # Playlist metadata
    │   ├── index.bin      # Binary index (optional)
    │   ├── artwork_0.webp # First artwork (symlink or copy)
    │   ├── artwork_1.gif  # Second artwork
    │   └── artwork_2.webp # Third artwork
    └── pl-abc/
        └── ...
```

**metadata.json Structure**:
```json
{
  "playlist_id": "pl-789",
  "post_id": 456,
  "created_at": "2024-01-20T14:00:00Z",
  "playlist_seed": 67890,
  "total_artworks": 12,
  "artworks_downloaded": 3,
  "artworks": [
    {
      "index": 0,
      "storage_key": "def456.gif",
      "art_url": "https://...",
      "downloaded": true,
      "local_path": "/sdcard/playlists/pl-789/artwork_0.gif"
    },
    {
      "index": 1,
      "storage_key": "ghi789.webp",
      "art_url": "https://...",
      "downloaded": true,
      "local_path": "/sdcard/playlists/pl-789/artwork_1.webp"
    },
    {
      "index": 2,
      "storage_key": "jkl012.webp",
      "art_url": "https://...",
      "downloaded": false,
      "local_path": null
    }
  ]
}
```

### Technical Requirements

1. **Performance**:
   - Play queue navigation O(1) for simple cases, O(n) for worst case
   - No pre-building of entire play queue in memory
   - Efficient caching of current playlist metadata only

2. **Memory Constraints**:
   - Cache only current playlist metadata (not all playlists)
   - Maximum 1024 artworks per playlist × ~256 bytes = ~256KB per playlist metadata

3. **Thread Safety**:
   - All navigation state protected by mutex
   - Download task coordinates with loader task to avoid SD conflicts

4. **Persistence**:
   - PE persisted in NVS
   - Play order persisted in NVS
   - randomize_playlist persisted in NVS
   - Live mode state persisted in NVS
   - Current p/q indices do NOT persist (reset to 0,0 on boot)
   - Buffer state does NOT persist (rebuild on boot)

5. **Download Prioritization**:
   - Downloads ordered by proximity in play queue
   - No interruption of ongoing downloads
   - 3 retries with exponential backoff (1s, 5s, 15s)

## Implementation Plan

### Phase 1: Data Structures and Core Abstractions

#### 1.1 Extend Post Structure to Support Playlists

**File**: `components/channel_manager/include/sdcard_channel.h`

Add playlist-aware post structure:

```c
typedef enum {
    POST_KIND_ARTWORK,
    POST_KIND_PLAYLIST
} post_kind_t;

typedef struct {
    char *storage_key;        // Storage key for artwork
    char *art_url;            // Download URL (may be NULL if already local)
    time_t created_at;        // Artwork creation timestamp
    char *local_path;         // Full local path (or NULL if not downloaded)
    bool downloaded;          // True if file exists locally
    asset_type_t type;        // Determined from storage_key extension
} playlist_artwork_t;

typedef struct {
    char *playlist_id;        // Unique playlist identifier (e.g., "pl-789")
    uint32_t playlist_seed;   // Seed for randomization (0 if not set)
    uint16_t total_artworks;  // Total artworks in playlist (may be > downloaded count)
    uint16_t downloaded_count;// Number of artworks currently downloaded
    playlist_artwork_t *artworks; // Array of artworks (only downloaded ones initially)
    size_t artworks_capacity; // Capacity of artworks array
} playlist_metadata_t;

// Extended post structure (replaces sdcard_post_t)
typedef struct {
    post_kind_t kind;         // Artwork or playlist
    char *name;               // Post name
    int32_t post_id;          // Server post ID (-1 if local)
    time_t created_at;        // Post creation timestamp
    bool healthy;             // Load health flag
    
    union {
        // For artwork-type posts
        struct {
            char *filepath;       // Full path for loading
            asset_type_t type;    // GIF, WebP, etc.
        } artwork;
        
        // For playlist-type posts
        struct {
            playlist_metadata_t *metadata;  // Playlist metadata (loaded on-demand)
            char *metadata_path;            // Path to metadata.json
        } playlist;
    } data;
} post_t;
```

**Rationale**: Keeps backward compatibility by using union, minimizes memory for artwork-only posts.

#### 1.2 Create Navigator Component

**New Component**: `components/playlist_navigator/`

**Files**:
- `include/playlist_navigator.h` - Public API
- `playlist_navigator.c` - Implementation

**Purpose**: Encapsulates all play queue logic, navigation (p, q), play order, PE.

**API Design**:

```c
// Public types
typedef enum {
    PLAY_ORDER_ORIGINAL,   // Server order (server_seq)
    PLAY_ORDER_CREATION,   // By creation date (newest first)
    PLAY_ORDER_RANDOM      // Random with seed
} play_order_t;

typedef struct {
    size_t p;              // Post index in channel
    size_t q;              // In-playlist artwork index (0 if not in playlist)
} nav_position_t;

typedef struct {
    post_t *post;          // Current post
    playlist_artwork_t *artwork; // Current artwork (NULL if not available)
    nav_position_t position;
    bool is_playlist;      // True if current post is playlist
    bool artwork_available; // True if artwork file exists locally
} nav_item_t;

// Initialization
esp_err_t playlist_navigator_init(void);
void playlist_navigator_deinit(void);

// Configuration
esp_err_t playlist_navigator_set_play_order(play_order_t order);
play_order_t playlist_navigator_get_play_order(void);

esp_err_t playlist_navigator_set_pe(uint16_t pe);  // 0-1023, 0=infinite
uint16_t playlist_navigator_get_pe(void);

esp_err_t playlist_navigator_set_randomize_playlist(bool enable);
bool playlist_navigator_get_randomize_playlist(void);

esp_err_t playlist_navigator_set_random_seed(uint32_t seed);

// Channel loading
esp_err_t playlist_navigator_load_channel(post_t *posts, size_t count);
void playlist_navigator_unload_channel(void);

// Navigation
esp_err_t playlist_navigator_get_current(nav_item_t *out_item);
esp_err_t playlist_navigator_next(nav_item_t *out_item);
esp_err_t playlist_navigator_prev(nav_item_t *out_item);
esp_err_t playlist_navigator_jump_to(nav_position_t position, nav_item_t *out_item);

// Position tracking
nav_position_t playlist_navigator_get_position(void);

// Play buffer query
esp_err_t playlist_navigator_get_next_n(size_t n, nav_item_t *out_items, size_t *out_count);

// Metadata management
esp_err_t playlist_navigator_load_playlist_metadata(size_t post_index);
void playlist_navigator_unload_playlist_metadata(size_t post_index);
```

**Implementation Notes**:

1. **Navigator State**:
```c
typedef struct {
    // Channel data
    post_t *posts;            // Array of posts (owned by channel_player)
    size_t post_count;
    
    // Current position
    nav_position_t position;  // Current (p, q)
    
    // Play order state
    play_order_t play_order;
    size_t *post_indices;     // Mapping for current play order
    uint16_t pe;              // Playlist expansion
    bool randomize_playlist;
    
    // Random state
    uint32_t channel_seed;
    pcg128_t channel_rng;     // For post-level randomization
    pcg128_t *playlist_rng;   // For current playlist (allocated on-demand)
    
    // Cached playlist metadata
    playlist_metadata_t *current_playlist; // Only current playlist
    size_t current_playlist_post_idx;      // Which post it belongs to
    
    // Thread safety
    SemaphoreHandle_t mutex;
} navigator_state_t;
```

2. **next() Implementation Pseudocode**:
```c
esp_err_t playlist_navigator_next(nav_item_t *out_item) {
    LOCK(mutex);
    
    post_t *curr_post = &posts[post_indices[position.p]];
    
    if (curr_post->kind == POST_KIND_ARTWORK) {
        // Move to next post
        position.p++;
        if (position.p >= post_count) {
            position.p = 0;  // Wrap around
            if (play_order == PLAY_ORDER_RANDOM) {
                reshuffle_post_indices();  // New random cycle
            }
        }
        position.q = 0;
        
        post_t *next_post = &posts[post_indices[position.p]];
        if (next_post->kind == POST_KIND_PLAYLIST) {
            load_playlist_metadata_if_needed(position.p);
            // position.q already 0, pointing to first artwork
        }
    }
    else if (curr_post->kind == POST_KIND_PLAYLIST) {
        playlist_metadata_t *pl = current_playlist;
        size_t effective_length = (pe == 0) ? pl->downloaded_count : MIN(pe, pl->downloaded_count);
        
        if (position.q + 1 < effective_length) {
            // Stay in playlist, advance q
            position.q++;
        }
        else {
            // Leave playlist, advance to next post
            position.p++;
            if (position.p >= post_count) {
                position.p = 0;
                if (play_order == PLAY_ORDER_RANDOM) {
                    reshuffle_post_indices();
                }
            }
            position.q = 0;
            
            // Unload old playlist, load new if needed
            unload_playlist_metadata(current_playlist_post_idx);
            post_t *next_post = &posts[post_indices[position.p]];
            if (next_post->kind == POST_KIND_PLAYLIST) {
                load_playlist_metadata_if_needed(position.p);
            }
        }
    }
    
    // Fill out_item with current artwork info
    populate_nav_item(out_item);
    
    UNLOCK(mutex);
    return ESP_OK;
}
```

3. **prev() Implementation**: Similar logic but reversed direction.

4. **Random Order**: Use pcg128_t from sync_playlist for reversibility.

5. **Playlist Metadata Loading**:
   - Parse metadata.json from `/sdcard/playlists/{playlist_id}/metadata.json`
   - Only load when entering a playlist
   - Unload when leaving playlist

#### 1.3 Create Playlist Manager Component

**New Component**: `components/playlist_manager/`

**Files**:
- `include/playlist_manager.h` - Public API
- `playlist_manager.c` - Implementation
- `playlist_storage.c` - Filesystem operations

**Purpose**: Manages playlist storage, downloading, and metadata persistence.

**API Design**:

```c
// Initialization
esp_err_t playlist_manager_init(void);
void playlist_manager_deinit(void);

// Playlist creation/update
esp_err_t playlist_manager_create_playlist(const char *playlist_id, 
                                          uint32_t post_id,
                                          uint32_t playlist_seed,
                                          time_t created_at);

esp_err_t playlist_manager_add_artworks(const char *playlist_id,
                                       const char **storage_keys,
                                       const char **art_urls,
                                       const time_t *created_ats,
                                       size_t count);

// Metadata operations
esp_err_t playlist_manager_load_metadata(const char *playlist_id, 
                                        playlist_metadata_t **out_metadata);
void playlist_manager_free_metadata(playlist_metadata_t *metadata);

esp_err_t playlist_manager_save_metadata(const char *playlist_id, 
                                        const playlist_metadata_t *metadata);

// Download operations
esp_err_t playlist_manager_download_artwork(const char *playlist_id,
                                           size_t artwork_index,
                                           const char *art_url,
                                           const char *storage_key);

// Query operations
bool playlist_manager_artwork_exists(const char *playlist_id, size_t artwork_index);
esp_err_t playlist_manager_get_artwork_path(const char *playlist_id,
                                           size_t artwork_index,
                                           char *out_path,
                                           size_t path_len);

// Cleanup
esp_err_t playlist_manager_delete_playlist(const char *playlist_id);
esp_err_t playlist_manager_cleanup_old_playlists(size_t max_playlists);
```

**Implementation Notes**:

1. **Filesystem Layout**:
   - Base dir: `/sdcard/playlists/`
   - Per-playlist dir: `/sdcard/playlists/{playlist_id}/`
   - Metadata file: `metadata.json`
   - Artwork files: `artwork_{index}.{ext}` (e.g., `artwork_0.webp`)

2. **Metadata Persistence**:
   - Use cJSON for parsing/serialization
   - Atomic writes (write to temp file, rename)
   - Validate on load

3. **Download Integration**:
   - Reuse existing `makapix_artwork_download_with_progress()` from makapix_artwork.c
   - Download to playlist folder instead of vault
   - Update metadata.json after each successful download

### Phase 2: Integration with Existing Components

#### 2.1 Modify channel_player to Use Navigator

**File**: `components/channel_manager/channel_player.c`

**Changes**:

1. Replace internal state with navigator:
```c
// Old state (remove):
// channel_player_state_t state;
// size_t *indices;
// size_t current_pos;

// New state (add):
static playlist_navigator_handle_t s_navigator = NULL;
```

2. Update initialization:
```c
esp_err_t channel_player_init(void) {
    // ... existing code ...
    
    esp_err_t err = playlist_navigator_init(&s_navigator);
    if (err != ESP_OK) {
        return err;
    }
    
    // ... rest of init ...
}
```

3. Update advance():
```c
esp_err_t channel_player_advance(void) {
    // Handle Makapix channel source
    if (s_player.source_type == CHANNEL_PLAYER_SOURCE_MAKAPIX) {
        return channel_next_item(s_player.makapix_channel, &item);
    }
    
    // Use navigator for SD card source
    nav_item_t nav_item;
    esp_err_t err = playlist_navigator_next(s_navigator, &nav_item);
    if (err != ESP_OK) {
        return err;
    }
    
    // Update cached current post
    update_current_post_from_nav_item(&nav_item);
    
    return ESP_OK;
}
```

4. Similar updates for go_back(), get_current_post(), etc.

#### 2.2 Modify animation_player to Handle Playlists

**File**: `main/animation_player_loader.c`

**Changes**:

1. Update loader task to understand playlists:
```c
void animation_loader_task(void *arg) {
    while (true) {
        // ... wait for semaphore ...
        
        // Get current navigation item (not just post)
        nav_item_t nav_item;
        esp_err_t err = channel_player_get_current_nav_item(&nav_item);
        if (err != ESP_OK || !nav_item.artwork_available) {
            ESP_LOGW(TAG, "Current artwork not available, skipping");
            discard_failed_swap_request(ESP_ERR_NOT_FOUND);
            continue;
        }
        
        // Load artwork from nav_item
        const char *filepath = nav_item.artwork->local_path;
        asset_type_t type = nav_item.artwork->type;
        
        err = load_animation_into_buffer(filepath, type, &s_back_buffer);
        
        // ... rest of loader logic ...
    }
}
```

#### 2.3 Add Download Manager Task

**New File**: `main/playlist_downloader.c`

**Purpose**: Background task that maintains the play buffer by downloading upcoming artworks.

**Task Logic**:

```c
static void playlist_downloader_task(void *arg) {
    const size_t buffer_size = 6;  // BN = 6 by default
    
    while (true) {
        // Sleep for a bit between checks
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
        
        // Get next BN items from navigator
        nav_item_t items[buffer_size];
        size_t count = 0;
        esp_err_t err = playlist_navigator_get_next_n(s_navigator, buffer_size, 
                                                      items, &count);
        if (err != ESP_OK) {
            continue;
        }
        
        // Count how many are already available
        size_t available = 0;
        for (size_t i = 0; i < count; i++) {
            if (items[i].artwork_available) {
                available++;
            }
        }
        
        ESP_LOGD(TAG, "Play buffer: %zu/%zu artworks available", available, buffer_size);
        
        // If buffer is full, nothing to do
        if (available >= buffer_size) {
            continue;
        }
        
        // Find first unavailable artwork and download it
        for (size_t i = 0; i < count; i++) {
            if (!items[i].artwork_available && items[i].artwork->art_url) {
                ESP_LOGI(TAG, "Downloading artwork %s for buffer", 
                        items[i].artwork->storage_key);
                
                // Download (this blocks)
                if (items[i].is_playlist) {
                    playlist_manager_download_artwork(
                        items[i].post->data.playlist.metadata->playlist_id,
                        items[i].position.q,
                        items[i].artwork->art_url,
                        items[i].artwork->storage_key
                    );
                } else {
                    // Download regular artwork to vault
                    char path[256];
                    makapix_artwork_download(
                        items[i].artwork->art_url,
                        items[i].artwork->storage_key,
                        path, sizeof(path)
                    );
                }
                
                // Only download one at a time
                break;
            }
        }
    }
}
```

### Phase 3: Server Integration and MQTT

#### 3.1 Extend Makapix API for Playlists

**File**: `components/makapix/makapix_api.h`

**Changes**:

1. Add PE parameter to query request:
```c
typedef struct {
    makapix_channel_type_t channel;
    char user_handle[64];
    makapix_sort_mode_t sort;
    bool has_cursor;
    char cursor[64];
    uint8_t limit;
    bool random_seed_present;
    uint32_t random_seed;
    uint16_t pe;              // NEW: Playlist expansion (0-1023)
} makapix_query_request_t;
```

2. Add playlist data to post response:
```c
typedef struct {
    char storage_key[64];
    char art_url[256];
    time_t created_at;
} makapix_artwork_t;

typedef struct {
    int post_id;
    char post_kind[16];       // "artwork" or "playlist"
    
    // For artwork posts
    char storage_key[64];
    char art_url[256];
    
    // For playlist posts
    char playlist_id[64];
    uint32_t playlist_seed;
    uint16_t total_artworks;
    makapix_artwork_t artworks[32];  // Up to 32 artworks per playlist in response
    uint16_t artworks_count;
    
    // Common fields
    char canvas[16];
    int width;
    int height;
    int frame_count;
    bool has_transparency;
    char owner_handle[64];
    char created_at[40];
} makapix_post_t;
```

**File**: `components/makapix/makapix_api.c`

**Changes**:

1. Update query serialization to include PE:
```c
cJSON *payload = cJSON_CreateObject();
// ... existing fields ...
if (req->pe > 0) {
    cJSON_AddNumberToObject(payload, "PE", req->pe);
}
```

2. Update response parsing to handle playlists:
```c
static esp_err_t parse_post(cJSON *post_json, makapix_post_t *out_post) {
    // ... existing parsing ...
    
    cJSON *kind = cJSON_GetObjectItem(post_json, "kind");
    if (kind && cJSON_IsString(kind)) {
        strncpy(out_post->post_kind, cJSON_GetStringValue(kind), 
                sizeof(out_post->post_kind) - 1);
    } else {
        strcpy(out_post->post_kind, "artwork");  // Default
    }
    
    if (strcmp(out_post->post_kind, "playlist") == 0) {
        // Parse playlist-specific fields
        cJSON *playlist_id = cJSON_GetObjectItem(post_json, "playlist_id");
        if (playlist_id && cJSON_IsString(playlist_id)) {
            strncpy(out_post->playlist_id, cJSON_GetStringValue(playlist_id),
                    sizeof(out_post->playlist_id) - 1);
        }
        
        cJSON *playlist_seed = cJSON_GetObjectItem(post_json, "playlist_seed");
        if (playlist_seed && cJSON_IsNumber(playlist_seed)) {
            out_post->playlist_seed = playlist_seed->valueint;
        }
        
        cJSON *artworks = cJSON_GetObjectItem(post_json, "artworks");
        if (artworks && cJSON_IsArray(artworks)) {
            size_t count = MIN(cJSON_GetArraySize(artworks), 32);
            out_post->artworks_count = count;
            
            for (size_t i = 0; i < count; i++) {
                cJSON *artwork = cJSON_GetArrayItem(artworks, i);
                parse_artwork(artwork, &out_post->artworks[i]);
            }
        }
    }
    
    return ESP_OK;
}
```

#### 3.2 Add MQTT Commands for Playlist Control

**File**: `components/makapix/makapix_mqtt.c`

**New Commands**:

1. `set_pe <value>`: Set playlist expansion (0-1023)
2. `set_play_order <original|creation|random>`: Set play order
3. `set_randomize_playlist <on|off>`: Enable/disable playlist randomization
4. `set_live_mode <on|off>`: Enable/disable Live Mode

**Implementation**:
```c
static void handle_set_pe(cJSON *params) {
    cJSON *pe_json = cJSON_GetObjectItem(params, "pe");
    if (!pe_json || !cJSON_IsNumber(pe_json)) {
        return;
    }
    
    int pe = pe_json->valueint;
    if (pe < 0 || pe > 1023) {
        ESP_LOGW(TAG, "Invalid PE value: %d", pe);
        return;
    }
    
    playlist_navigator_set_pe(pe);
    config_store_set_pe(pe);  // Persist to NVS
    
    ESP_LOGI(TAG, "Set PE to %d", pe);
}
```

### Phase 4: Configuration and Persistence

#### 4.1 Extend config_store for Playlist Settings

**File**: `components/config_store/config_store.h`

**New APIs**:
```c
// Playlist expansion
esp_err_t config_store_set_pe(uint16_t pe);
uint16_t config_store_get_pe(void);  // Default: 0 (infinite)

// Play order
esp_err_t config_store_set_play_order(play_order_t order);
play_order_t config_store_get_play_order(void);  // Default: PLAY_ORDER_ORIGINAL

// Randomize playlist
esp_err_t config_store_set_randomize_playlist(bool enable);
bool config_store_get_randomize_playlist(void);  // Default: false

// Live mode
esp_err_t config_store_set_live_mode(bool enable);
bool config_store_get_live_mode(void);  // Default: false

// Random seed (for current playback session)
esp_err_t config_store_set_random_seed(uint32_t seed);
uint32_t config_store_get_random_seed(void);  // Default: 0 (generate new)
```

**Implementation**:
```c
esp_err_t config_store_set_pe(uint16_t pe) {
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    
    cJSON_AddNumberToObject(cfg, "pe", pe);
    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    
    return err;
}

uint16_t config_store_get_pe(void) {
    cJSON *cfg = NULL;
    if (config_store_load(&cfg) != ESP_OK) {
        return 0;  // Default: infinite expansion
    }
    
    cJSON *pe = cJSON_GetObjectItem(cfg, "pe");
    uint16_t value = (pe && cJSON_IsNumber(pe)) ? pe->valueint : 0;
    cJSON_Delete(cfg);
    
    return value;
}
```

### Phase 5: Live Mode Implementation

#### 5.1 Integrate sync_playlist Component

**File**: `components/playlist_navigator/playlist_navigator.c`

**Integration**:

1. Add Live Mode state:
```c
typedef struct {
    // ... existing navigator state ...
    
    // Live Mode
    bool live_mode_enabled;
    bool sync_playlist_initialized;
    animation_t *sync_animations;  // Array for sync_playlist
    size_t sync_animation_count;
} navigator_state_t;
```

2. Initialize sync_playlist when enabling Live Mode:
```c
esp_err_t playlist_navigator_enable_live_mode(bool enable) {
    LOCK(mutex);
    
    if (enable && !s_state.sync_playlist_initialized) {
        // Build animation array for sync_playlist
        // Only include artworks from play queue (expanded according to PE)
        
        size_t anim_count = estimate_play_queue_size();
        s_state.sync_animations = calloc(anim_count, sizeof(animation_t));
        
        // Populate with durations (use default dwell time for stills)
        // This requires walking through entire play queue once
        build_sync_animation_array(s_state.sync_animations, anim_count);
        
        // Get NTP time
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t unix_time = tv.tv_sec;
        
        // Initialize sync_playlist
        SyncPlaylist.init(
            s_state.channel_seed,    // Master seed
            unix_time,               // Playlist start time
            s_state.sync_animations,
            anim_count,
            SYNC_MODE_FORGIVING      // Use forgiving mode for robustness
        );
        
        SyncPlaylist.enable_live(true);
        s_state.sync_playlist_initialized = true;
    }
    else if (!enable && s_state.sync_playlist_initialized) {
        SyncPlaylist.enable_live(false);
        free(s_state.sync_animations);
        s_state.sync_animations = NULL;
        s_state.sync_playlist_initialized = false;
    }
    
    s_state.live_mode_enabled = enable;
    
    UNLOCK(mutex);
    return ESP_OK;
}
```

3. Update navigation to use sync_playlist when in Live Mode:
```c
esp_err_t playlist_navigator_get_current(nav_item_t *out_item) {
    LOCK(mutex);
    
    if (s_state.live_mode_enabled && s_state.sync_playlist_initialized) {
        // Query sync_playlist for current animation
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t unix_time = tv.tv_sec;
        
        uint32_t sync_index = 0;
        uint32_t elapsed_ms = 0;
        bool changed = SyncPlaylist.update(unix_time, &sync_index, &elapsed_ms);
        
        if (changed) {
            ESP_LOGI(TAG, "Live Mode: Jumped to sync index %u", sync_index);
        }
        
        // Convert sync_index to (p, q) position
        nav_position_t pos = convert_sync_index_to_position(sync_index);
        s_state.position = pos;
    }
    
    // Use normal navigation logic to get item at current position
    esp_err_t err = get_item_at_position(s_state.position, out_item);
    
    UNLOCK(mutex);
    return err;
}
```

#### 5.2 Add Live Mode UI Indicator

**File**: `main/ugfx_ui.c`

**Changes**:
- Add visual indicator when Live Mode is active
- Show sync status (synced / not synced)
- Display via overlay or corner icon

### Phase 6: HTTP API Extensions

#### 6.1 Add REST Endpoints for Playlist Control

**File**: `components/http_api/http_api.c`

**New Endpoints**:

1. `GET /api/playlist/config`: Get current playlist configuration
   ```json
   {
     "pe": 3,
     "play_order": "random",
     "randomize_playlist": false,
     "live_mode": true,
     "random_seed": 42
   }
   ```

2. `POST /api/playlist/config`: Update configuration
   ```json
   {
     "pe": 5,
     "play_order": "creation",
     "randomize_playlist": true
   }
   ```

3. `GET /api/playlist/status`: Get current playback status
   ```json
   {
     "position": {
       "p": 3,
       "q": 2
     },
     "current_post": {
       "kind": "playlist",
       "playlist_id": "pl-789",
       "name": "Cool Animations"
     },
     "buffer_status": {
       "available": 5,
       "target": 6
     }
   }
   ```

4. `POST /api/playlist/live_mode`: Enable/disable Live Mode
   ```json
   {
     "enable": true
   }
   ```

### Phase 7: Testing and Edge Cases

#### 7.1 Unit Tests

**New Test Files**:
- `test/test_playlist_navigator.c`: Test navigation logic
- `test/test_playlist_manager.c`: Test storage operations
- `test/test_pcg_reversibility.c`: Test random order reversibility

**Key Test Cases**:

1. **Navigation**:
   - Navigate through channel with mixed artwork/playlist posts
   - Verify p/q indices update correctly
   - Test wraparound at channel boundaries
   - Test playlist expansion with different PE values

2. **Random Order**:
   - Generate random sequence, navigate forward N times
   - Navigate backward N times, verify exact reversal
   - Test with different seeds

3. **Playlist Metadata**:
   - Load/save metadata from JSON
   - Handle partially downloaded playlists
   - Handle invalid/corrupted metadata

4. **Play Buffer**:
   - Verify buffer maintains target size
   - Test download prioritization
   - Handle download failures gracefully

#### 7.2 Integration Tests

1. **End-to-End Playback**:
   - Load channel with playlists from server
   - Verify smooth playback through playlists
   - Test playlist entry/exit transitions

2. **Live Mode Synchronization**:
   - Run two p3a devices in Live Mode
   - Verify they show same artwork at same time (within tolerance)
   - Test forward/backward navigation during Live Mode

3. **Download During Playback**:
   - Start playback with partially downloaded playlists
   - Verify background downloads complete
   - Verify seamless integration of newly downloaded artworks

#### 7.3 Edge Cases and Error Handling

1. **Empty Playlists**:
   - Handle playlists with 0 artworks downloaded
   - Display "Downloading..." message
   - Skip to next post if download fails repeatedly

2. **Network Failures**:
   - Handle download failures gracefully
   - Retry with exponential backoff
   - Continue playback with available artworks

3. **Storage Full**:
   - Detect when SD card is full
   - Implement LRU eviction of old playlists
   - Display error message to user

4. **Invalid Metadata**:
   - Handle corrupted metadata.json files
   - Re-fetch from server if possible
   - Fall back to treating as unavailable

5. **Rapid Navigation**:
   - Handle user rapidly cycling through posts
   - Avoid loading/unloading same playlist repeatedly
   - Implement hysteresis/debouncing

6. **Live Mode Edge Cases**:
   - Handle NTP sync failures
   - Detect clock drift and resync
   - Handle transition in/out of Live Mode during playback

## Implementation Timeline

### Week 1: Core Abstractions
- Day 1-2: Implement post_t structure and basic playlist types
- Day 3-5: Implement playlist_navigator component (navigation logic)
- Day 6-7: Unit tests for navigator

### Week 2: Storage and Management
- Day 1-3: Implement playlist_manager component
- Day 4-5: Integrate with channel_player
- Day 6-7: Filesystem tests and metadata handling

### Week 3: Server Integration
- Day 1-2: Extend makapix_api for playlists
- Day 3-4: Implement playlist downloader task
- Day 5-7: Integration testing with mock server

### Week 4: Live Mode
- Day 1-3: Integrate sync_playlist component
- Day 4-5: Implement Live Mode UI indicators
- Day 6-7: Synchronization testing

### Week 5: Polish and Testing
- Day 1-3: HTTP API extensions
- Day 4-5: Edge case testing
- Day 6-7: Performance optimization and documentation

## Pain Points and Challenges

### 1. Memory Management

**Challenge**: Playlists can have up to 1024 artworks, each with metadata.

**Mitigation**:
- Cache only current playlist metadata
- Lazy-load playlist data on-demand
- Implement efficient cleanup when leaving playlists
- Monitor heap usage carefully

**Estimated Memory**:
- Navigator state: ~4KB
- Current playlist metadata (worst case): ~256KB (1024 × 256 bytes)
- Sync animations array (worst case): ~8KB (1024 × 8 bytes)
- Total: ~268KB (acceptable given 32MB PSRAM)

### 2. Random Order Reversibility

**Challenge**: Backward navigation must exactly retrace random sequence.

**Mitigation**:
- Use PCG128 from sync_playlist (proven reversible)
- Maintain PCG state carefully
- Separate post-level and playlist-level randomization
- Extensive testing of forward-backward cycles

**Complexity**: Medium-high. PCG advance() function handles this but requires careful integration.

### 3. Download Prioritization

**Challenge**: Download artworks in play queue order without interrupting ongoing downloads.

**Mitigation**:
- Simple priority queue: download next unavailable artwork in buffer
- No interruption of ongoing downloads
- Accept that priority isn't perfect (trade-off for simplicity)

**Complexity**: Low. Simple greedy algorithm sufficient.

### 4. Live Mode Synchronization

**Challenge**: Keep devices in sync despite network latency, clock drift, NTP failures.

**Mitigation**:
- Use SYNC_MODE_FORGIVING (±10-15s tolerance)
- Periodic NTP sync checks
- Detect drift and resync automatically
- UI indicator for "not synced" state

**Complexity**: Medium. sync_playlist handles hard parts, but integration requires care.

### 5. Partial Playlist Downloads

**Challenge**: User enters playlist before all artworks are downloaded.

**Mitigation**:
- Treat downloaded count as effective playlist length
- Expand playlist progressively as artworks download
- May exit playlist earlier than expected (acceptable trade-off)
- Display "downloading" indicator when buffer is empty

**Complexity**: Low. Clean handling of partial state.

### 6. Play Queue Size Estimation

**Challenge**: Some operations need to know total play queue size (e.g., Live Mode init).

**Mitigation**:
- Accept O(n) walk through channel to compute size
- Cache result until channel changes
- For Live Mode, compute once at enable time

**Complexity**: Medium. One-time cost is acceptable.

### 7. Thread Safety

**Challenge**: Multiple threads access navigation state (UI, loader, downloader, MQTT).

**Mitigation**:
- Single mutex protects entire navigator state
- Short critical sections (no I/O under lock)
- Careful lock ordering to avoid deadlocks

**Complexity**: Medium. Standard threading challenges.

### 8. Server Response Format

**Challenge**: Server response format is not yet finalized.

**Mitigation**:
- Design flexible parsing that handles missing fields gracefully
- Version response format if needed
- Mock server for testing

**Complexity**: Low. API evolution is expected.

### 9. SD Card Contention

**Challenge**: Loader task, downloader task, and user operations all access SD card.

**Mitigation**:
- Reuse existing SD access pausing mechanism (animation_player_pause_sd_access)
- Prioritize user operations (e.g., file upload)
- Downloader backs off if loader is busy

**Complexity**: Low. Existing mechanisms sufficient.

### 10. Filesystem Space Management

**Challenge**: Playlists consume significant SD card space.

**Mitigation**:
- Implement LRU eviction (playlist_manager_cleanup_old_playlists)
- Monitor free space before downloads
- User-configurable cache size (via config_store)

**Complexity**: Medium. Requires careful bookkeeping.

## Open Questions

### 1. Server Response Format

**Question**: What is the exact JSON structure for playlist posts in server responses?

**Assumption**: Format described in Section "Server Communication" above.

**Resolution Needed**: Confirm with backend team or reverse-engineer from Makapix Club API.

### 2. Playlist Seeds

**Question**: Do playlists come with a seed, or should we derive from playlist_id?

**Assumption**: Server may provide optional `playlist_seed` field. If absent, use `hash(playlist_id)` as seed.

**Resolution Needed**: Confirm server behavior.

### 3. PE Defaults

**Question**: Should PE default to 0 (infinite) or some small value (e.g., 3)?

**Assumption**: Default to 0 (infinite) to avoid surprising users with truncated playlists.

**Resolution Needed**: User research or product decision.

### 4. Live Mode Master Seed

**Question**: Where does the channel-level seed come from for Live Mode?

**Options**:
- (A) Hardcoded constant (all p3a devices use same seed)
- (B) Generated from channel ID
- (C) Provided by server

**Assumption**: (A) Hardcoded constant for simplicity. All p3a devices globally synchronized.

**Resolution Needed**: Product decision on sync scope (global vs. per-channel).

### 5. NTP Sync Reliability

**Question**: What if NTP is not available or fails?

**Fallback Plan**:
- Disable Live Mode if NTP not synced
- Display "Live Mode unavailable (no time sync)" message
- Re-enable when NTP sync succeeds

**Resolution Needed**: Define UX for NTP failure case.

### 6. Playlist Metadata Updates

**Question**: Can playlists change after initial download (e.g., artworks added/removed)?

**Assumption**: Yes. Server may add artworks to playlist over time.

**Strategy**:
- Periodically re-query server for updated playlist metadata
- Compare with local metadata, download new artworks
- Don't delete local artworks that server no longer lists (user may want to keep)

**Resolution Needed**: Define refresh strategy and frequency.

### 7. Backward Compatibility

**Question**: How to handle servers that don't support PE parameter yet?

**Fallback Plan**:
- Send PE in request, but don't fail if server ignores it
- Parse response, count artworks returned per playlist
- Treat response as "server chose PE for us"

**Resolution Needed**: Coordinate with backend team on rollout strategy.

### 8. Dwell Time for Playlist Artworks

**Question**: Should artworks within a playlist have individual dwell times, or use global dwell time?

**Assumption**: Use global dwell time for simplicity. Future enhancement could support per-artwork dwell times.

**Resolution Needed**: Product decision.

### 9. Playlist Navigation UI

**Question**: Should UI indicate position within playlist (e.g., "3/12")?

**Requirement from Problem Statement**: No. Device displays animation alone, no overlays.

**Resolution**: No UI change needed. Keep it simple.

### 10. Storage Location: Vault vs. Playlists Folder

**Question**: Should playlist artworks be stored in vault (deduplicated) or separate folder?

**Requirement from Problem Statement**: Separate `/sdcard/playlists/` folder.

**Trade-off**: Duplication vs. simplicity. Separate folder is cleaner for playlist management.

**Resolution**: Use separate folder as specified.

## Recommended Approach

### Phase 1: Minimal Viable Implementation (MVP)

**Goal**: Get basic playlist support working end-to-end.

**Scope**:
- post_t structure with artwork/playlist distinction
- Simple playlist_navigator (original order only, no randomization yet)
- Basic playlist_manager (storage, metadata, download)
- Integration with channel_player and animation_player
- Server API extension (PE parameter)
- Config persistence (PE, play order)

**Exclusions** (for later phases):
- Live Mode
- Random order with reversibility
- Randomize playlist setting
- Advanced download prioritization
- HTTP API extensions

**Timeline**: 2-3 weeks

**Rationale**: Prove core architecture works before adding complexity.

### Phase 2: Navigation and Ordering

**Goal**: Add play orders and randomization.

**Scope**:
- Creation date ordering
- Random order with PCG128 reversibility
- Randomize playlist setting
- Play buffer optimization

**Timeline**: 1-2 weeks

**Rationale**: Navigation logic is critical, needs careful testing.

### Phase 3: Live Mode

**Goal**: Enable synchronized playback.

**Scope**:
- sync_playlist integration
- NTP sync checks
- Live Mode UI
- Synchronization testing

**Timeline**: 1-2 weeks

**Rationale**: Independent feature, can be added after core works.

### Phase 4: Polish and Optimization

**Goal**: Production-ready quality.

**Scope**:
- HTTP API extensions
- Error handling and edge cases
- Performance optimization
- User documentation
- Extensive testing

**Timeline**: 1-2 weeks

**Rationale**: Incremental polish based on real-world usage.

## Alternative Approaches Considered

### Alternative 1: Pre-build Entire Play Queue in Memory

**Approach**: Compute entire play queue at channel load time, store as flat array.

**Pros**:
- O(1) navigation (simple array indexing)
- Easy to implement
- Predictable memory usage

**Cons**:
- High memory consumption (could be 10,000+ items with playlists)
- Doesn't handle partial playlist downloads well
- Wasteful if user doesn't navigate through entire queue

**Verdict**: Rejected. Too memory-hungry, not flexible enough.

### Alternative 2: Treat Playlists as Sub-Channels

**Approach**: Model playlists as nested channels, reuse channel_player logic recursively.

**Pros**:
- Code reuse (channel_player already works)
- Conceptually clean (playlists are channels)

**Cons**:
- Complex nesting (channels contain playlists but not playlists in playlists)
- Hard to implement p/q indices cleanly
- Difficult to enforce playlist expansion (PE)

**Verdict**: Rejected. Added complexity without clear benefit.

### Alternative 3: Separate Playlist Playback Mode

**Approach**: When entering playlist, switch to "playlist mode" that plays only that playlist until complete.

**Pros**:
- Simple mental model
- Easy to implement

**Cons**:
- Doesn't match requirement (seamless playlist entry/exit)
- Doesn't support PE (partial playlist inclusion)
- Poor UX (user forced to finish playlist)

**Verdict**: Rejected. Doesn't meet requirements.

### Alternative 4: Always Download Full Playlists

**Approach**: When encountering playlist, download all artworks before showing any.

**Pros**:
- Simpler logic (no partial state)
- Guarantees smooth playback within playlist

**Cons**:
- High latency (user waits for full download)
- Wastes bandwidth for large playlists
- Doesn't match requirement (play available artworks first)

**Verdict**: Rejected. Poor UX, doesn't meet requirements.

### Alternative 5: Server-Side Play Queue Generation

**Approach**: Server pre-computes play queue, sends flat array to device.

**Pros**:
- Simple device logic
- Server has more resources for computation
- Easy to update play queue remotely

**Cons**:
- High bandwidth (large arrays)
- Server becomes stateful (per-device state)
- Doesn't support local SD card channels
- Not responsive to local configuration changes (PE, play order)

**Verdict**: Rejected. Doesn't fit p3a's architecture (local-first design).

## Conclusion

This implementation plan provides a comprehensive roadmap for adding full playlist support to p3a. The recommended approach is phased, starting with an MVP that proves the core architecture, then incrementally adding features.

The key architectural decisions are:

1. **On-the-fly play queue**: No pre-building, compute navigation on-demand
2. **Two-index system (p, q)**: Clean separation of post-level and playlist-level navigation
3. **Lazy playlist loading**: Cache only current playlist metadata
4. **PCG-based randomization**: Reversible random order for seamless backward navigation
5. **Separate storage**: `/sdcard/playlists/` folder for playlist artworks
6. **sync_playlist integration**: Leverage existing component for Live Mode

The main challenges are memory management, thread safety, and random order reversibility, but all are manageable with careful implementation.

The plan addresses all requirements from the problem statement:
- ✅ Playlist support (1-1024 artworks)
- ✅ Play queue abstraction (next/prev navigation)
- ✅ Playlist expansion (PE, 0-1023)
- ✅ Play orders (original, creation, random)
- ✅ Play buffer (BN artworks, default 6)
- ✅ Download prioritization
- ✅ Live Mode (synchronized random playback)
- ✅ Randomize playlist setting
- ✅ Config persistence (NVS)
- ✅ Server integration (MQTT, PE parameter)

Next steps: Review this plan with stakeholders, get approval, and begin Phase 1 implementation.
