# p3a Playlist Support Implementation Plan

## Executive Summary

This document outlines the comprehensive implementation plan for adding full playlist support to p3a. Playlists allow a single post to contain multiple artworks (1-1024), which are expanded according to configurable rules and navigated seamlessly within the channel playback flow.

**Implementation Status: Phase 1 In Progress (Core Infrastructure)**

**Key Changes:**
- Posts can be either artworks (individual) or playlists (collections)
- Play queue navigates through artworks, expanding playlists according to PE (playlist expansion)
- New settings: PE (playlist expansion), randomize_playlist, live_mode
- Play buffer maintains 6+ artworks ready to play
- Background downloading prioritizes upcoming artworks
- Live Mode synchronizes random playback across devices using NTP time

**Recent Progress (Latest 3 Commits):**
1. Added playlist_manager and play_navigator headers with complete APIs
2. Implemented core playlist_manager.c and play_navigator.c components
3. Added NVS settings persistence for all playlist configuration options

---

## Table of Contents

1. [Concepts and Terminology](#concepts-and-terminology)
2. [JSON Structure for Playlist Posts](#json-structure-for-playlist-posts)
3. [Architecture Overview](#architecture-overview)
4. [Component Design](#component-design)
5. [Navigation Logic (p/q Indices)](#navigation-logic-pq-indices)
6. [Play Buffer and Download Management](#play-buffer-and-download-management)
7. [Live Mode Implementation](#live-mode-implementation)
8. [Storage and Caching](#storage-and-caching)
9. [MQTT Command Handlers](#mqtt-command-handlers)
10. [Settings Persistence (NVS)](#settings-persistence-nvs)
11. [Playlist Update/Refresh Logic](#playlist-updaterefresh-logic)
12. [Edge Cases and Error Handling](#edge-cases-and-error-handling)
13. [Testing Strategy](#testing-strategy)
14. [Implementation Phases](#implementation-phases)
15. [Open Questions](#open-questions)

---

## Concepts and Terminology

### Post Types
- **Artwork Post**: A single animation file (WebP/GIF/PNG/JPEG)
- **Playlist Post**: A collection of 1-1024 artwork posts

### Play Queue
The ordered sequence of individual **artworks** (not posts) scheduled to play. Derived on-the-fly from:
- `current_channel`: Active channel
- `play_order`: Server order, creation order, or random
- `p`: Post index in channel
- `q`: In-playlist artwork index (0 if current post is not a playlist)

### Play Order Modes
1. **Server Order (original)**: Posts ordered as received from server (`server_seq`)
2. **Creation Order**: Posts sorted by `created_at` timestamp
3. **Random Order**: Posts shuffled using PCG-XSL-RR with seed

### Playlist Expansion (PE)
Integer from 0-1023 expressing how many artworks from a playlist to include in the play queue:
- `PE = 0`: Infinite expansion (all artworks in playlist)
- `PE = 1-1023`: Include first N artworks from playlist

### Play Buffer (BN)
Minimum number of artworks to keep downloaded and ready (default: 6). The system continuously ensures at least BN artworks are buffered ahead in the play queue.

### Live Mode
When `play_order = RANDOM` and `live_mode = ON`, all devices with the same channel play synchronized random sequences using NTP time and shared seed.

### Randomize Playlist
Boolean setting:
- `OFF` (default): Playlists maintain internal server order
- `ON`: Each playlist's artworks are also randomized using a playlist-specific seed

---

## JSON Structure for Playlist Posts

### Server API Request
When querying posts from server, client can specify PE:

```json
POST /api/v1/query_posts
{
  "channel": "all",
  "sort": "server_order",
  "limit": 50,
  "PE": 8,
  "random_seed": 0xFAB12345  // Optional, for random mode
}
```

### Server Response - Artwork Post
```json
{
  "post_id": 12345,
  "kind": "artwork",
  "storage_key": "a1b2c3d4e5f6...",
  "art_url": "https://cdn.makapix.club/vault/ab/cd/a1b2c3d4e5f6...webp",
  "canvas": "128x128",
  "width": 128,
  "height": 128,
  "frame_count": 24,
  "has_transparency": true,
  "owner_handle": "@pixelartist",
  "created_at": "2026-01-15T08:30:00Z",
  "metadata_modified_at": "2026-01-15T08:30:00Z",
  "artwork_modified_at": "2026-01-15T08:30:00Z",
  "dwell_time_ms": 5000  // 0 = use parent default
}
```

### Server Response - Playlist Post
```json
{
  "post_id": 67890,
  "kind": "playlist",
  "owner_handle": "@curator",
  "created_at": "2026-01-16T12:00:00Z",
  "metadata_modified_at": "2026-01-16T14:00:00Z",
  "total_artworks": 24,
  "dwell_time_ms": 8000,  // Default for artworks in this playlist
  "artworks": [
    {
      "post_id": 11111,
      "storage_key": "f1e2d3c4b5a6...",
      "art_url": "https://cdn.makapix.club/vault/f1/e2/f1e2d3c4b5a6...gif",
      "canvas": "64x64",
      "width": 64,
      "height": 64,
      "frame_count": 12,
      "has_transparency": false,
      "owner_handle": "@artist1",
      "created_at": "2026-01-10T10:00:00Z",
      "metadata_modified_at": "2026-01-10T10:00:00Z",
      "artwork_modified_at": "2026-01-10T10:00:00Z",
      "dwell_time_ms": 0  // 0 = use playlist default (8000ms)
    },
    {
      "post_id": 22222,
      "storage_key": "a9b8c7d6e5f4...",
      "art_url": "https://cdn.makapix.club/vault/a9/b8/a9b8c7d6e5f4...webp",
      "canvas": "64x64",
      "width": 64,
      "height": 64,
      "frame_count": 8,
      "has_transparency": true,
      "owner_handle": "@artist2",
      "created_at": "2026-01-11T15:30:00Z",
      "metadata_modified_at": "2026-01-11T15:30:00Z",
      "artwork_modified_at": "2026-01-11T15:30:00Z",
      "dwell_time_ms": 6000
    }
    // ... up to PE artworks included
  ]
}
```

**Notes:**
- Server includes first `PE` artworks in the `artworks` array
- If `PE = 0`, server includes all artworks (up to 1024 max)
- If no `PE` specified, server defaults to `PE = 1`
- Client should handle responses where `artworks.length <= total_artworks`
- Each artwork in playlist has its own `dwell_time_ms` (0 = use playlist default)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    Channel Manager Layer                     │
│  (channel_player, channel_interface, makapix_channel_impl)  │
└──────────────────────┬──────────────────────────────────────┘
                       │
         ┌─────────────┼─────────────┐
         │                           │
┌────────▼─────────┐      ┌──────────▼──────────┐
│  Play Navigator  │      │  Playlist Manager   │
│  - p/q indices   │◄────►│  - Load metadata    │
│  - next/prev     │      │  - Expand artworks  │
│  - play_queue    │      │  - Cache current    │
└────────┬─────────┘      └──────────┬──────────┘
         │                           │
         │        ┌──────────────────┘
         │        │
┌────────▼────────▼──────────┐
│     Play Buffer Manager    │
│  - Maintain BN artworks    │
│  - Download prioritization │
│  - Availability tracking   │
└────────┬───────────────────┘
         │
┌────────▼───────────────────┐
│   Download Orchestrator    │
│  - Priority queue          │
│  - Retry with backoff      │
│  - Vault storage           │
└────────┬───────────────────┘
         │
┌────────▼───────────────────┐
│      Vault Storage         │
│  /sdcard/vault/            │
│  - Artwork files (dedup)   │
│  - JSON sidecars           │
└────────────────────────────┘

┌────────────────────────────┐
│  /sdcard/playlists/        │
│  - Playlist metadata JSON  │
│  - Per-playlist index      │
└────────────────────────────┘

┌────────────────────────────┐
│      Live Mode Sync        │
│  (sync_playlist component) │
│  - PCG random with seed    │
│  - NTP time sync           │
└────────────────────────────┘
```

---

## Component Design

### 1. Playlist Manager (`playlist_manager.c/h`)

**Responsibilities:**
- Load and cache playlist metadata from `/sdcard/playlists/`
- Fetch playlist metadata from server via `makapix_api`
- Detect playlist updates (compare `metadata_modified_at`)
- Expand playlists according to PE
- Track which artworks in a playlist are downloaded

**API:**
```c
typedef struct {
    int32_t post_id;
    int32_t total_artworks;
    int32_t loaded_artworks;  // How many artworks we have metadata for
    int32_t available_artworks;  // How many are fully downloaded
    uint32_t dwell_time_ms;
    time_t metadata_modified_at;
    artwork_ref_t *artworks;  // Array of artwork references
} playlist_metadata_t;

typedef struct {
    int32_t post_id;
    char storage_key[96];
    char art_url[256];
    uint32_t dwell_time_ms;
    time_t metadata_modified_at;
    time_t artwork_modified_at;
    bool downloaded;  // Is artwork file in vault?
} artwork_ref_t;

// Initialize playlist manager
esp_err_t playlist_manager_init(void);

// Load playlist from cache or fetch from server
esp_err_t playlist_get(int32_t post_id, uint32_t pe, playlist_metadata_t **out_playlist);

// Release cached playlist
void playlist_release(playlist_metadata_t *playlist);

// Check if playlist needs update
bool playlist_needs_update(int32_t post_id, time_t server_modified_at);

// Queue background update
esp_err_t playlist_queue_update(int32_t post_id);

// Get specific artwork from playlist
esp_err_t playlist_get_artwork(playlist_metadata_t *playlist, uint32_t index, 
                               artwork_ref_t **out_artwork);
```

### 2. Play Navigator (`play_navigator.c/h`)

**Responsibilities:**
- Maintain `p` (post index) and `q` (in-playlist artwork index)
- Implement `next()` and `prev()` operations
- Handle playlist expansion according to PE
- Support all play orders (server, created, random)
- Handle reversibility in random mode

**API:**
```c
typedef struct {
    channel_handle_t channel;
    play_order_mode_t order;
    uint32_t pe;  // Playlist expansion
    bool randomize_playlist;
    uint32_t p;  // Post index
    uint32_t q;  // In-playlist artwork index
    uint32_t channel_seed;  // For random mode
    bool live_mode;
} play_navigator_t;

// Initialize navigator
esp_err_t play_navigator_init(play_navigator_t *nav);

// Get current artwork reference
esp_err_t play_navigator_current(play_navigator_t *nav, artwork_ref_t *out_artwork);

// Advance to next artwork
esp_err_t play_navigator_next(play_navigator_t *nav, artwork_ref_t *out_artwork);

// Go back to previous artwork
esp_err_t play_navigator_prev(play_navigator_t *nav, artwork_ref_t *out_artwork);

// Jump to specific post/artwork
esp_err_t play_navigator_jump(play_navigator_t *nav, uint32_t p, uint32_t q);

// Update settings
void play_navigator_set_pe(play_navigator_t *nav, uint32_t pe);
void play_navigator_set_order(play_navigator_t *nav, play_order_mode_t order);
void play_navigator_set_randomize_playlist(play_navigator_t *nav, bool enable);
void play_navigator_set_live_mode(play_navigator_t *nav, bool enable);
```

### 3. Play Buffer Manager (`play_buffer.c/h`)

**Responsibilities:**
- Maintain list of BN artworks ahead in play queue
- Track which artworks are downloaded and ready
- Request downloads for missing artworks
- Signal when buffer is ready to start playback

**API:**
```c
typedef struct {
    play_navigator_t *navigator;
    uint32_t buffer_size;  // Target: BN (default 6)
    artwork_ref_t *buffer[MAX_BUFFER_SIZE];
    uint8_t ready_count;  // How many are downloaded
} play_buffer_t;

// Initialize buffer
esp_err_t play_buffer_init(play_buffer_t *buffer, play_navigator_t *nav, uint32_t bn);

// Refresh buffer (call after navigator changes)
esp_err_t play_buffer_refresh(play_buffer_t *buffer);

// Check if buffer is ready for playback
bool play_buffer_is_ready(play_buffer_t *buffer);

// Get next ready artwork
esp_err_t play_buffer_pop_next(play_buffer_t *buffer, artwork_ref_t *out_artwork);

// Background task: maintain buffer
void play_buffer_task(void *pvParameters);
```

### 4. Download Orchestrator (`download_manager.c/h`)

**Responsibilities:**
- Priority queue for artwork downloads
- Retry logic with exponential backoff (1s, 5s, 15s)
- Download from `art_url` to vault storage
- Update playlist manager when artwork becomes available

**API:**
```c
typedef enum {
    DOWNLOAD_PRIORITY_HIGH,    // In current play buffer
    DOWNLOAD_PRIORITY_MEDIUM,  // Next BN artworks
    DOWNLOAD_PRIORITY_LOW,     // Background refresh
} download_priority_t;

typedef struct {
    int32_t post_id;
    char storage_key[96];
    char art_url[256];
    download_priority_t priority;
    uint8_t retry_count;
    time_t next_retry_at;
} download_request_t;

// Initialize download manager
esp_err_t download_manager_init(void);

// Queue download request
esp_err_t download_queue(const artwork_ref_t *artwork, download_priority_t priority);

// Cancel download
esp_err_t download_cancel(const char *storage_key);

// Background download task
void download_task(void *pvParameters);
```

---

## Navigation Logic (p/q Indices)

### State Variables
```c
uint32_t p;  // Post index in channel (0-based)
uint32_t q;  // Artwork index within playlist (0-based, 0 if not in playlist)
```

### Navigation Rules

#### Next Operation
```
current_post = channel.posts[p]

if current_post.kind == "artwork":
    // Simple case: move to next post
    p = (p + 1) % channel.post_count
    q = 0
    
else if current_post.kind == "playlist":
    playlist = get_playlist(current_post.post_id)
    effective_size = min(playlist.available_artworks, PE if PE > 0 else playlist.total_artworks)
    
    if q + 1 < effective_size:
        // Stay in playlist, advance q
        q = q + 1
    else:
        // Exit playlist, move to next post
        p = (p + 1) % channel.post_count
        q = 0
```

#### Previous Operation
```
if q > 0:
    // We're inside a playlist, move back within it
    q = q - 1
    
else if p > 0:
    // Move to previous post
    p = p - 1
    
    // Check if previous post is a playlist
    prev_post = channel.posts[p]
    if prev_post.kind == "playlist":
        playlist = get_playlist(prev_post.post_id)
        effective_size = min(playlist.available_artworks, PE if PE > 0 else playlist.total_artworks)
        q = effective_size - 1  // Jump to last artwork in playlist
    else:
        q = 0
        
else:
    // Wrap to last post in channel
    p = channel.post_count - 1
    
    last_post = channel.posts[p]
    if last_post.kind == "playlist":
        playlist = get_playlist(last_post.post_id)
        effective_size = min(playlist.available_artworks, PE if PE > 0 else playlist.total_artworks)
        q = effective_size - 1
    else:
        q = 0
```

### Random Order Navigation

For random post order, use PCG-XSL-RR algorithm (already implemented in `sync_playlist.c`):

```c
typedef struct {
    pcg128_t rng;
    uint32_t channel_seed;  // Derived from 0xFAB + channel_id
    uint32_t *post_history;  // For reversibility
    uint32_t history_pos;
} random_navigator_t;

// Initialize for channel
void random_nav_init(random_navigator_t *nav, uint32_t channel_id);

// Get next random post index
uint32_t random_nav_next(random_navigator_t *nav, uint32_t channel_size);

// Get previous (must be exactly reversible)
uint32_t random_nav_prev(random_navigator_t *nav);
```

For **randomize_playlist** mode, each playlist gets its own PCG instance:
```c
uint32_t playlist_seed = hash(channel_seed, post_id);
```

This ensures:
1. Same seed → same random sequence
2. Different playlists have different sequences
3. Fully reversible with `prev()`

---

## Play Buffer and Download Management

### Buffer Maintenance Loop

```
while (true):
    // 1. Refresh buffer state
    buffer_refresh()
    
    // 2. Check how many artworks are ready
    ready_count = count_ready_artworks(buffer)
    
    if ready_count < BN:
        // 3. Request downloads for missing artworks
        for each artwork in buffer:
            if not artwork.downloaded:
                download_queue(artwork, PRIORITY_HIGH)
    
    // 4. Sleep and repeat
    vTaskDelay(pdMS_TO_TICKS(1000))
```

### Download Prioritization

Priority levels:
1. **HIGH**: Artworks in current play buffer (next BN)
2. **MEDIUM**: Artworks slightly ahead (next 6-12)
3. **LOW**: Background refresh of updated artworks

When picking next download:
```
1. Select highest priority pending request
2. Check retry timer (exponential backoff)
3. Download to temp file in vault
4. Verify SHA256 (if provided)
5. Atomic rename to final location
6. Update availability tracking
7. Notify play buffer manager
```

### Retry Strategy

```c
uint32_t get_retry_delay_ms(uint8_t retry_count) {
    switch(retry_count) {
        case 0: return 1000;   // 1 second
        case 1: return 5000;   // 5 seconds
        case 2: return 15000;  // 15 seconds
        default: return 0;      // Give up after 3 retries
    }
}
```

---

## Live Mode Implementation

Live Mode synchronizes random playback across all p3a devices without direct communication.

### Requirements
1. NTP time synchronization
2. Shared channel seed (derived from channel ID)
3. PCG-XSL-RR random number generator
4. Playlist start time (channel creation time or device boot)

### Implementation Using `sync_playlist` Component

The existing `sync_playlist.c` already implements the core logic. We need to integrate it:

```c
// At channel start
uint64_t channel_seed = 0xFAB ^ channel_id;
uint64_t start_time = get_channel_start_time();  // Or device boot time

// Build animation list from play queue
animation_t animations[MAX_ANIMATIONS];
uint32_t anim_count = 0;

for (uint32_t i = 0; i < play_queue_size && anim_count < MAX_ANIMATIONS; i++) {
    artwork_ref_t artwork;
    if (play_navigator_get_at_index(nav, i, &artwork) == ESP_OK) {
        animations[anim_count].duration_ms = artwork.dwell_time_ms;
        anim_count++;
    }
}

// Initialize sync playlist
SyncPlaylist.init(channel_seed, start_time, animations, anim_count, SYNC_MODE_FORGIVING);
SyncPlaylist.enable_live(true);

// In playback loop
uint64_t current_time = get_ntp_time();
uint32_t anim_index;
uint32_t elapsed_ms;
bool changed = SyncPlaylist.update(current_time, &anim_index, &elapsed_ms);

if (changed) {
    // Load and display new animation
    load_animation(animations[anim_index]);
}
```

### NTP Time Handling

```c
// Try to get NTP time
esp_err_t get_ntp_time(uint64_t *out_time) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Check if time was ever set (year > 2025)
    if (timeinfo.tm_year + 1900 < 2025) {
        // Time never synchronized, use fallback
        *out_time = 1737072000;  // Jan 16, 2026 00:00 UTC
        return ESP_ERR_INVALID_STATE;
    }
    
    *out_time = (uint64_t)now;
    return ESP_OK;
}

// Retry NTP sync with exponential backoff
void ntp_sync_task(void *pvParameters) {
    uint32_t retry_delay_s = 60;  // Start with 1 minute
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(retry_delay_s * 1000));
        
        esp_err_t err = sntp_sync();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "NTP sync successful");
            retry_delay_s = 3600;  // Check again in 1 hour
        } else {
            ESP_LOGW(TAG, "NTP sync failed, retry in %lu seconds", retry_delay_s);
            retry_delay_s = MIN(retry_delay_s * 2, 3600);  // Cap at 1 hour
        }
    }
}
```

---

## Storage and Caching

### Directory Structure

```
/sdcard/
├── vault/              # Artwork files (SHA256-based, deduplicated)
│   ├── ab/
│   │   ├── cd/
│   │   │   ├── abcd1234...webp
│   │   │   ├── abcd1234...json  (sidecar metadata)
│   │   │   └── ...
│   └── ...
│
├── playlists/          # Playlist metadata
│   ├── 67890.json      # Playlist post_id 67890
│   ├── 67891.json
│   └── ...
│
└── animations/         # Legacy SD card channel (optional)
    └── ...
```

### Playlist Metadata File Format

`/sdcard/playlists/67890.json`:
```json
{
  "post_id": 67890,
  "total_artworks": 24,
  "loaded_artworks": 24,
  "metadata_modified_at": "2026-01-16T14:00:00Z",
  "dwell_time_ms": 8000,
  "artworks": [
    {
      "post_id": 11111,
      "storage_key": "f1e2d3c4b5a6...",
      "art_url": "https://cdn.makapix.club/vault/f1/e2/...",
      "dwell_time_ms": 0,
      "metadata_modified_at": "2026-01-10T10:00:00Z",
      "artwork_modified_at": "2026-01-10T10:00:00Z"
    },
    ...
  ]
}
```

### Caching Strategy

**Memory usage constraints:**
- Cache only **current** playlist in RAM
- Load other playlists on-demand from `/sdcard/playlists/`
- Artwork files always accessed from vault (not cached in RAM)

```c
static playlist_metadata_t *s_current_playlist = NULL;
static int32_t s_current_playlist_id = -1;

esp_err_t playlist_get(int32_t post_id, uint32_t pe, playlist_metadata_t **out_playlist) {
    if (s_current_playlist && s_current_playlist_id == post_id) {
        // Return cached playlist
        *out_playlist = s_current_playlist;
        return ESP_OK;
    }
    
    // Free old cached playlist
    if (s_current_playlist) {
        playlist_free(s_current_playlist);
        s_current_playlist = NULL;
    }
    
    // Try to load from disk
    esp_err_t err = playlist_load_from_disk(post_id, &s_current_playlist);
    
    if (err != ESP_OK || playlist_needs_update(s_current_playlist)) {
        // Fetch from server
        err = playlist_fetch_from_server(post_id, pe, &s_current_playlist);
        if (err == ESP_OK) {
            playlist_save_to_disk(s_current_playlist);
        }
    }
    
    s_current_playlist_id = post_id;
    *out_playlist = s_current_playlist;
    return ESP_OK;
}
```

---

## MQTT Command Handlers

### New Commands

Add handlers in `makapix.c`:

```c
void on_mqtt_command(const char *command_type, cJSON *payload) {
    if (strcmp(command_type, "set_pe") == 0) {
        cJSON *pe_item = cJSON_GetObjectItem(payload, "pe");
        if (pe_item && cJSON_IsNumber(pe_item)) {
            uint32_t pe = (uint32_t)cJSON_GetNumberValue(pe_item);
            if (pe <= 1023) {
                play_navigator_set_pe(&s_navigator, pe);
                nvs_set_u32("appcfg", "pe", pe);
                ESP_LOGI(TAG, "Set PE to %lu", pe);
            }
        }
    }
    else if (strcmp(command_type, "set_randomize_playlist") == 0) {
        cJSON *enable_item = cJSON_GetObjectItem(payload, "enable");
        if (enable_item && cJSON_IsBool(enable_item)) {
            bool enable = cJSON_IsTrue(enable_item);
            play_navigator_set_randomize_playlist(&s_navigator, enable);
            nvs_set_u8("appcfg", "rand_playlist", enable ? 1 : 0);
            ESP_LOGI(TAG, "Set randomize_playlist to %s", enable ? "ON" : "OFF");
        }
    }
    else if (strcmp(command_type, "set_live_mode") == 0) {
        cJSON *enable_item = cJSON_GetObjectItem(payload, "enable");
        if (enable_item && cJSON_IsBool(enable_item)) {
            bool enable = cJSON_IsTrue(enable_item);
            play_navigator_set_live_mode(&s_navigator, enable);
            nvs_set_u8("appcfg", "live_mode", enable ? 1 : 0);
            ESP_LOGI(TAG, "Set live_mode to %s", enable ? "ON" : "OFF");
        }
    }
    else if (strcmp(command_type, "set_play_order") == 0) {
        cJSON *order_item = cJSON_GetObjectItem(payload, "order");
        if (order_item && cJSON_IsString(order_item)) {
            const char *order_str = cJSON_GetStringValue(order_item);
            play_order_mode_t order;
            
            if (strcmp(order_str, "server") == 0) {
                order = PLAY_ORDER_SERVER;
            } else if (strcmp(order_str, "created") == 0) {
                order = PLAY_ORDER_CREATED;
            } else if (strcmp(order_str, "random") == 0) {
                order = PLAY_ORDER_RANDOM;
            } else {
                return;
            }
            
            play_navigator_set_order(&s_navigator, order);
            nvs_set_u8("appcfg", "play_order", (uint8_t)order);
            ESP_LOGI(TAG, "Set play_order to %s", order_str);
        }
    }
    // ... existing commands ...
}
```

---

## Settings Persistence (NVS)

### New NVS Keys

Namespace: `appcfg`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `pe` | uint32 | 8 | Playlist expansion (0-1023) |
| `play_order` | uint8 | 0 | 0=server, 1=created, 2=random |
| `rand_playlist` | uint8 | 0 | Randomize playlist contents |
| `live_mode` | uint8 | 0 | Enable Live Mode sync |
| `dwell_time` | uint32 | 30000 | Default dwell time (ms) |
| `buffer_size` | uint8 | 6 | Play buffer size (BN) |

### Load Settings on Boot

```c
esp_err_t settings_load_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("appcfg", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No settings in NVS, using defaults");
        return ESP_OK;
    }
    
    uint32_t pe = 8;
    nvs_get_u32(handle, "pe", &pe);
    play_navigator_set_pe(&s_navigator, pe);
    
    uint8_t order = 0;
    nvs_get_u8(handle, "play_order", &order);
    play_navigator_set_order(&s_navigator, (play_order_mode_t)order);
    
    uint8_t rand_playlist = 0;
    nvs_get_u8(handle, "rand_playlist", &rand_playlist);
    play_navigator_set_randomize_playlist(&s_navigator, rand_playlist);
    
    uint8_t live_mode = 0;
    nvs_get_u8(handle, "live_mode", &live_mode);
    play_navigator_set_live_mode(&s_navigator, live_mode);
    
    nvs_close(handle);
    return ESP_OK;
}
```

---

## Playlist Update/Refresh Logic

### Detecting Stale Playlists

When loading posts from server, compare `metadata_modified_at`:

```c
bool playlist_needs_update(playlist_metadata_t *playlist, time_t server_modified_at) {
    if (!playlist) return true;
    
    // Check if server version is newer
    return difftime(server_modified_at, playlist->metadata_modified_at) > 0;
}
```

### Lazy Update Strategy

Don't proactively check for updates. Instead, update when:
1. Query response shows newer `metadata_modified_at`
2. User manually refreshes channel

```c
void on_channel_query_response(makapix_query_response_t *resp) {
    for (size_t i = 0; i < resp->post_count; i++) {
        makapix_post_t *post = &resp->posts[i];
        
        if (strcmp(post->kind, "playlist") == 0) {
            // Check if we have this playlist cached
            playlist_metadata_t *cached = playlist_get_cached(post->post_id);
            
            if (cached && playlist_needs_update(cached, post->metadata_modified_at)) {
                ESP_LOGI(TAG, "Playlist %d is out of date, queueing update", post->post_id);
                playlist_queue_update(post->post_id);
            }
        }
    }
}
```

### Update Process

1. Fetch new metadata from server
2. Compare artwork lists (old vs new)
3. Delete removed artworks from vault (if not used elsewhere)
4. Queue downloads for new artworks
5. Update `metadata_modified_at` timestamps
6. Save updated metadata to disk

```c
esp_err_t playlist_update(int32_t post_id) {
    // Fetch latest from server
    playlist_metadata_t *new_playlist;
    esp_err_t err = playlist_fetch_from_server(post_id, 0, &new_playlist);
    if (err != ESP_OK) {
        return err;
    }
    
    // Load old version (if exists)
    playlist_metadata_t *old_playlist;
    playlist_load_from_disk(post_id, &old_playlist);
    
    // Compare and update artworks
    for (uint32_t i = 0; i < new_playlist->loaded_artworks; i++) {
        artwork_ref_t *artwork = &new_playlist->artworks[i];
        
        // Check if artwork file needs re-download
        if (old_playlist) {
            artwork_ref_t *old_artwork = find_artwork_by_post_id(old_playlist, artwork->post_id);
            if (old_artwork && 
                difftime(artwork->artwork_modified_at, old_artwork->artwork_modified_at) > 0) {
                // Artwork file was updated, re-download
                ESP_LOGI(TAG, "Artwork %d was modified, re-downloading", artwork->post_id);
                download_queue(artwork, DOWNLOAD_PRIORITY_LOW);
            }
        }
        
        // Queue download if not in vault
        if (!vault_file_exists(artwork->storage_key)) {
            download_queue(artwork, DOWNLOAD_PRIORITY_LOW);
        }
    }
    
    // Save updated playlist to disk
    playlist_save_to_disk(new_playlist);
    
    if (old_playlist) {
        playlist_free(old_playlist);
    }
    
    return ESP_OK;
}
```

---

## Edge Cases and Error Handling

### 1. Playlist Download Incomplete

**Problem**: User is at a playlist but not all artworks downloaded yet.

**Solution**: 
- Treat playlist as having `available_artworks` count instead of `total_artworks`
- Play only downloaded artworks
- As each artwork finishes downloading, update `available_artworks` and continue
- Navigator checks `available_artworks` when computing effective playlist size

```c
uint32_t get_effective_playlist_size(playlist_metadata_t *playlist, uint32_t pe) {
    uint32_t max_size = (pe == 0) ? playlist->total_artworks : pe;
    return MIN(playlist->available_artworks, max_size);
}
```

### 2. Empty Play Buffer

**Problem**: All artworks in buffer fail to download, nothing to play.

**Solution**:
- Display "Downloading artworks..." message
- Keep trying to download with exponential backoff
- Log errors for user to investigate (SD card full? Network down?)

### 3. Invalid Navigator State

**Problem**: `p` or `q` point to non-existent post/artwork.

**Solution**:
- Detect invalid state on each navigation call
- Reset to `(p=0, q=0)` and log error
- Continue playback from start

```c
esp_err_t play_navigator_validate(play_navigator_t *nav) {
    if (nav->p >= nav->channel->post_count) {
        ESP_LOGE(TAG, "Invalid p=%lu (max=%lu), resetting to 0", 
                 nav->p, nav->channel->post_count);
        nav->p = 0;
        nav->q = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    post_t *post = &nav->channel->posts[nav->p];
    if (post->kind == PLAYLIST) {
        playlist_metadata_t *playlist = playlist_get(post->post_id);
        uint32_t effective_size = get_effective_playlist_size(playlist, nav->pe);
        
        if (nav->q >= effective_size) {
            ESP_LOGE(TAG, "Invalid q=%lu (max=%lu), resetting to 0", 
                     nav->q, effective_size);
            nav->q = 0;
            return ESP_ERR_INVALID_STATE;
        }
    }
    
    return ESP_OK;
}
```

### 4. Dwell Time Hierarchy

**Problem**: Dwell time can be specified at multiple levels.

**Solution**: Clear precedence order:
```
artwork.dwell_time_ms > 0 ? artwork.dwell_time_ms
: playlist.dwell_time_ms > 0 ? playlist.dwell_time_ms
: channel.dwell_time_ms > 0 ? channel.dwell_time_ms
: global_dwell_time_ms
```

```c
uint32_t get_effective_dwell_time(artwork_ref_t *artwork, 
                                   playlist_metadata_t *playlist,
                                   uint32_t global_default) {
    if (artwork->dwell_time_ms > 0) {
        return artwork->dwell_time_ms;
    }
    if (playlist && playlist->dwell_time_ms > 0) {
        return playlist->dwell_time_ms;
    }
    return global_default;
}
```

### 5. NTP Sync Failure

**Problem**: Device can't reach NTP servers, time is not set.

**Solution**:
- If time was never synchronized since boot: assume Jan 16, 2026 00:00 UTC
- If time was synchronized before: keep using current time, retry NTP later
- Live Mode still works (may be slightly off if device clock drifts)

### 6. Network Interruption During Download

**Problem**: HTTPS download fails mid-stream.

**Solution**:
- Use atomic writes (`.tmp` + rename)
- If download fails, leave `.tmp` file (will be cleaned up lazily)
- Retry with exponential backoff
- After 3 failures, mark artwork as unavailable and skip in play queue

### 7. SD Card Full

**Problem**: No space left for new artworks.

**Solution**:
- Detect `ESP_ERR_NO_MEM` or `ENOSPC` during vault write
- Log error clearly: "SD card full, cannot download artwork"
- Display message to user (if no artworks playing): "SD card full"
- Continue playing existing artworks
- User must manually free space

### 8. Playlist Larger Than Memory

**Problem**: 1024-artwork playlist metadata won't fit in RAM.

**Solution**:
- Design `playlist_metadata_t` to be small:
  - Each `artwork_ref_t` ~ 400 bytes
  - 1024 artworks ~ 400 KB
  - ESP32-P4 has 32 MB PSRAM, so this is fine
- If memory is truly constrained, implement paged loading:
  - Keep first N artworks in RAM
  - Load additional pages on demand

---

## Testing Strategy

### Unit Tests

1. **Play Navigator Tests**
   - Test next/prev with artwork-only channel
   - Test next/prev with playlist-only channel
   - Test next/prev with mixed channel
   - Test PE=0, PE=1, PE=5 scenarios
   - Test wrap-around at channel boundaries
   - Test random mode reversibility

2. **Playlist Manager Tests**
   - Test loading from disk cache
   - Test fetching from server
   - Test detecting stale playlists
   - Test update process

3. **Play Buffer Tests**
   - Test buffer refresh
   - Test download prioritization
   - Test availability tracking

### Integration Tests

1. **Single Playlist Channel**
   - Channel with 1 playlist (10 artworks)
   - PE=3: Should play first 3, then wrap
   - PE=0: Should play all 10, then wrap

2. **Mixed Channel**
   - Channel with 5 artworks + 1 playlist (8 artworks) + 3 artworks
   - PE=4: Expected order: a0, a1, a2, a3, a4, p0, p1, p2, p3, a5, a6, a7
   - Test forward and backward navigation

3. **Random Order**
   - Enable random order
   - Navigate forward 20 times
   - Navigate backward 20 times
   - Verify exact reversibility

4. **Live Mode Sync**
   - Run 2 devices with same channel ID
   - Enable Live Mode on both
   - Verify they play same artwork at same time (±5s tolerance)

### Performance Tests

1. **Large Playlist (1024 artworks)**
   - Load time < 2 seconds
   - Navigation latency < 50ms
   - Memory usage < 1 MB per cached playlist

2. **Download Performance**
   - 10 artworks (50 KB each) download in < 30 seconds on Wi-Fi
   - Retry logic correctly backs off

3. **Disk I/O**
   - Playlist save/load < 500ms
   - Vault file access < 100ms
   - No filesystem corruption after 1000 power cycles

### Stress Tests

1. **Channel Switching**
   - Rapidly switch between 5 channels
   - Verify no memory leaks
   - Verify correct playback after switching

2. **Network Failures**
   - Simulate packet loss, timeouts, DNS failures
   - Verify retry logic
   - Verify graceful degradation

---

## Implementation Phases

### Phase 1: Core Infrastructure (Week 1) - 80% Complete
- [x] Create `playlist_manager.h` - Header with API definitions
- [x] Create `play_navigator.h` - Header with API definitions
- [x] Implement `playlist_manager.c` - Core logic (basic version)
- [x] Implement `play_navigator.c` - Navigation logic (basic version)
- [x] Update CMakeLists.txt to build new components
- [x] Add NVS settings for PE, play_order, randomize_playlist, live_mode, dwell_time
- [x] Implement JSON parsing for playlist posts in makapix_api.c (PE parameter + playlist parsing)
- [ ] Complete playlist server fetching integration
- [ ] Enhance navigator with full playlist support
- [ ] Test Phase 1 components

### Phase 2: Navigation Logic (Week 1)
- [ ] Implement p/q indices tracking
- [ ] Implement next() with playlist expansion
- [ ] Implement prev() with reversibility
- [ ] Add random order support
- [ ] Add randomize_playlist support
- [ ] Unit tests for navigation

### Phase 3: Play Buffer & Downloads (Week 2)
- [ ] Create `play_buffer.c/h`
- [ ] Create `download_manager.c/h`
- [ ] Implement priority queue
- [ ] Implement retry with exponential backoff
- [ ] Integrate with vault storage
- [ ] Background download task

### Phase 4: Server Integration (Week 2)
- [ ] Update `makapix_api.c` for PE parameter
- [ ] Parse playlist posts from server response
- [ ] Implement playlist caching
- [ ] Implement lazy update detection

### Phase 5: Live Mode (Week 3)
- [ ] Integrate `sync_playlist` component
- [ ] NTP sync with fallback time
- [ ] Live Mode enable/disable
- [ ] Test synchronization between devices

### Phase 6: MQTT Commands (Week 3)
- [ ] Add `set_pe` command handler
- [ ] Add `set_play_order` command handler
- [ ] Add `set_randomize_playlist` command handler
- [ ] Add `set_live_mode` command handler
- [ ] Test via MQTT

### Phase 7: UI & UX (Week 4)
- [ ] Display "Downloading artworks..." when buffer empty
- [ ] Handle errors gracefully (SD full, network down)
- [ ] HTTP API endpoints for playlist settings
- [ ] Update web UI

### Phase 8: Testing & Documentation (Week 4)
- [ ] Unit tests for all components
- [ ] Integration tests with synthetic playlists
- [ ] Performance tests (1024-artwork playlists)
- [ ] Update user documentation
- [ ] Update developer documentation

---

## Open Questions - RESOLVED

All questions have been answered by stakeholder:

### 1. Server-Side Implementation

**Q**: Does the server already support the `PE` parameter and playlist post structure?

**A**: ✅ Yes. Server has been implemented as per our needs.

### 2. Channel-Level vs Global Settings

**Q**: Should some settings (PE, randomize_playlist) be per-channel or global?

**A**: ✅ All settings are **global** (not per-channel).

### 3. Playlist-in-Playlist

**Q**: What happens if server accidentally returns a playlist containing another playlist?

**A**: ✅ Skip the nested playlist (treat as unavailable). This should never happen but handle gracefully.

### 4. Artwork Deduplication

**Q**: If same artwork appears in multiple playlists, how do we track usage for deletion?

**A**: ✅ Use reference counting:
- Each `storage_key` in vault has a refcount
- Increment when adding to playlist  
- Decrement when removing from playlist
- Delete file only when refcount reaches 0
- Store refcounts in the artwork's **sidecar JSON** (not separate vault_index.json)

### 5. Maximum Download Parallelism

**Q**: How many concurrent downloads should we allow?

**A**: ✅ **One** concurrent download.

### 6. Playlist Artwork Ordering

**Q**: When randomize_playlist is OFF, what order do artworks follow?

**A**: ✅ Server order (the order in the `artworks` array from server response).

### 7. Live Mode Channel Seed

**Q**: Should channel seed be per-channel or global?

**A**: ✅ Channel-level seed is generated from hardcoded constant (0xFAB) and channel ID:
```c
uint64_t channel_seed = 0xFAB ^ ((uint64_t)channel_id);
```

### 8. Handling Deleted Playlists and Eviction

**Q**: What if user navigates to a playlist that was deleted from server?

**A**: ✅ Complex reference-counting based eviction system:
- Channels cache up to 1024 artworks (not posts) according to play order
- Artworks in playlists inherit references from the playlist's channel reference
- When channel updates, posts no longer in channel lose their reference
- Playlists/artworks without references are queued for background eviction
- Eviction runs asynchronously (many file operations, cannot block SDIO bus)
- If playlist loses all channel references, it and its unreferenced artworks get evicted

---

## Recommended Approaches

### 1. Incremental Implementation
Start with simplest cases and gradually add complexity:
1. Artwork-only channels (existing behavior)
2. Single playlist channel (PE=0, no randomization)
3. Mixed channels (artworks + playlists)
4. Randomization (post-level, then playlist-level)
5. Live Mode (after all above work)

### 2. Defensive Programming
- Validate all indices before dereferencing
- Check for NULL pointers religiously
- Use assertions for invariants
- Log errors verbosely for debugging

### 3. Memory Management
- Use static allocation where possible (avoid heap fragmentation)
- Free playlists not currently in use
- Monitor heap usage with `esp_get_free_heap_size()`
- Use PSRAM for large buffers

### 4. Testing First
- Write unit tests before implementation
- Use mocks for hardware dependencies (SD card, network)
- Create synthetic test data (JSON files, playlists)
- Automate testing with CI/CD

### 5. Backward Compatibility
- Existing artwork-only channels must work unchanged
- Settings should have sensible defaults
- Gracefully handle servers that don't support PE yet

---

## Pain Points and Difficulties

### 1. Complexity of Navigation Logic
The p/q indices add significant complexity, especially when handling:
- Wrap-around at channel boundaries
- Entering/exiting playlists
- Reversibility in random mode
- Partial playlist downloads

**Mitigation**: Extensive unit tests, clear state machine documentation.

### 2. SDIO Bus Contention
Wi-Fi and SD card share same SDIO bus on ESP32-P4. Concurrent access causes crashes.

**Mitigation**: Use `sdio_bus_lock()` consistently, serialize all SD/Wi-Fi operations.

### 3. Memory Constraints
32 MB PSRAM seems large, but loading 1024-artwork playlists eats memory fast.

**Mitigation**: 
- Cache only current playlist
- Use paged loading if needed
- Profile memory usage regularly

### 4. Synchronization in Live Mode
NTP drift, network latency, device clock skew all affect sync quality.

**Mitigation**:
- Use FORGIVING mode (±10s tolerance)
- Retry NTP sync regularly
- Log sync errors for debugging

### 5. Server API Changes
Playlist feature requires server changes. Mismatch between client and server versions could break things.

**Mitigation**:
- Version the API
- Gracefully handle missing PE support (use default PE=1)
- Test with both old and new server versions

### 6. Testing Coverage
Huge state space: 3 play orders × 2 randomize modes × variable PE × partial downloads × ...

**Mitigation**:
- Focus on common scenarios
- Use property-based testing where possible
- Crowdsource beta testing

---

## Conclusion

Playlist support is a significant feature that touches many parts of the p3a codebase. The modular design proposed here aims to:
- Keep changes localized and testable
- Maintain backward compatibility
- Scale to large playlists (1024 artworks)
- Provide smooth UX even with slow network

Key success factors:
1. **Thorough testing** at every phase
2. **Clear abstractions** (navigator, buffer, download manager)
3. **Defensive error handling** (validate everything)
4. **Incremental delivery** (ship phases 1-4 before Live Mode)

This plan is a living document and should be updated as implementation progresses.

---

**Document Version**: 1.1  
**Date**: 2025-12-11  
**Author**: GitHub Copilot Agent  
**Status**: Phase 1 In Progress - Core Infrastructure

---

## Implementation Progress Summary

### Completed (Phase 1 - 60% Complete)

**New Files Created:**
- `components/channel_manager/include/playlist_manager.h` - Playlist metadata management API
- `components/channel_manager/include/play_navigator.h` - p/q navigation API
- `components/channel_manager/playlist_manager.c` - Playlist caching, JSON I/O, server fetch
- `components/channel_manager/play_navigator.c` - Navigation logic with PCG random
- Updated `components/makapix/makapix_api.h` - Added PE parameter and playlist post types
- Updated `components/config_store/config_store.h/.c` - Added NVS settings functions

**Features Implemented:**
- ✅ Playlist metadata structure (artwork_ref_t, playlist_metadata_t)
- ✅ Playlist disk caching (/sdcard/playlists/*.json)
- ✅ Play navigator with p/q indices
- ✅ PCG32 random state for reversible random mode
- ✅ NVS settings: pe, play_order, randomize_playlist, live_mode, dwell_time
- ✅ JSON serialization/deserialization for playlists
- ✅ Availability tracking (downloaded vs total artworks)

### In Progress (Phase 1 - Remaining 40%)

**Next Steps:**
1. Update `makapix_api.c` to parse playlist JSON from server responses
2. Complete `playlist_fetch_from_server()` implementation
3. Enhance `play_navigator.c` with full playlist expansion logic
4. Add playlist kind detection in navigator
5. Test Phase 1 components in isolation

### Upcoming (Phase 2-8)

**Phase 2:** Full next/prev navigation with playlist expansion  
**Phase 3:** Play buffer and download manager  
**Phase 4:** Server integration and MQTT commands  
**Phase 5:** Live Mode with sync_playlist  
**Phase 6:** UI feedback and error handling  
**Phase 7:** Integration testing  
**Phase 8:** Performance optimization and documentation

---
