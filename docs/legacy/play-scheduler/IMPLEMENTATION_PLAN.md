# Play Scheduler — Implementation Plan

**Status**: 🔵 Revision 2 — Channel Architecture Clarified  
**Last Updated**: 2026-01-01  
**Author**: AI Assistant + Human Review

---

## Table of Contents

1. [Overview](#1-overview)
2. [Channel Architecture](#2-channel-architecture)
3. [Design Decisions](#3-design-decisions)
4. [Scheduler Commands](#4-scheduler-commands)
5. [System Architecture](#5-system-architecture)
6. [Data Structures](#6-data-structures)
7. [API Design](#7-api-design)
8. [Algorithm Details](#8-algorithm-details)
9. [Cache Management](#9-cache-management)
10. [Download Integration](#10-download-integration)
11. [Migration Strategy](#11-migration-strategy)
12. [Implementation Phases](#12-implementation-phases)
13. [Testing Strategy](#13-testing-strategy)
14. [Open Questions & Future Work](#14-open-questions--future-work)

---

## 1. Overview

### 1.1 Purpose

The **Play Scheduler (PS)** is a deterministic playback engine that replaces the existing `channel_player.c` component. It selects artworks from multiple channels for presentation on the p3a player device.

### 1.2 Key Design Principles

- **Virtual play queue**: Not materialized in full; uses streaming generation
- **Bounded memory**: History buffer (H) and Lookahead buffer (L)
- **Multi-channel fairness**: Weighted exposure across channels via SWRR
- **Responsiveness**: New Artwork Events (NAE) for newly published content
- **Immediate playback**: Use cached data immediately, refresh in background
- **Lenient behavior**: Skip unavailable content, never block
- **Event-driven downloads**: Lookahead-aware prefetching

### 1.3 What It Replaces

The Play Scheduler **completely replaces** `channel_player.c`, absorbing:
- Navigation logic (next/prev)
- Auto-swap timer
- Channel switching
- Play order management
- Channel cache management

---

## 2. Channel Architecture

### 2.1 Channel Types

There are **two kinds** of channels:

| Type | Storage | Source | Cache File |
|------|---------|--------|------------|
| **SD Card Channel** | Local folder scan | SD card `/animations/` folder | `sdcard.bin` |
| **Makapix Channels** | MQTT queries | Remote server | `{channel_id}.bin` |

### 2.2 Makapix Channel Subtypes

| Subtype | Name Field | Identifier Field | Cache File Name |
|---------|------------|------------------|-----------------|
| **Named** | `"all"`, `"featured"` | (none) | `all.bin`, `featured.bin` |
| **User** | `"user"` | `user_sqid` (e.g., `"uvz"`) | `user_uvz.bin` |
| **Hashtag** | `"hashtag"` | `hashtag` (e.g., `"sunset"`) | `hashtag_sunset.bin` |

### 2.3 Channel Identification

Every channel has a **unique string identifier** (max 32 characters):

```
Named:    "{name}"              → "all", "featured"
User:     "user:{user_sqid}"    → "user:uvz"
Hashtag:  "hashtag:{tag}"       → "hashtag:sunset"
SD Card:  "sdcard"              → "sdcard"
```

**Sanitization**: User and hashtag identifiers must be sanitized for filesystem compatibility:
- Replace non-alphanumeric characters with `_`
- Accept risk of rare collisions (deferred)

### 2.4 Channel Cache Files (Ei)

Each channel has a local `.bin` cache file on SD card:

```
/sdcard/p3a/channel/
├── all.bin              # Named channel: Recent
├── featured.bin         # Named channel: Promoted
├── user_uvz.bin         # User channel
├── hashtag_sunset.bin   # Hashtag channel
└── sdcard.bin           # SD card channel
```

**Format**: Uses existing `makapix_channel_entry_t` (64 bytes per entry).

**SD Card Channel**: Gets a `.bin` index file just like Makapix channels. Refreshed by scanning the `/animations/` folder.

### 2.5 Channel Lifecycle

```
┌─────────────────────────────────────────────────────────────────┐
│                    CHANNEL LIFECYCLE                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. SCHEDULER COMMAND RECEIVED                                  │
│     └─► Lookahead flushed                                       │
│     └─► History preserved                                       │
│                                                                 │
│  2. IMMEDIATE: Use cached data                                  │
│     └─► Load .bin files that exist                              │
│     └─► Channels without cache: weight=0 (skip)                 │
│     └─► Begin playback immediately                              │
│                                                                 │
│  3. BACKGROUND: Refresh caches (sequential, one at a time)      │
│     └─► For each channel in command:                            │
│         ├─► If Makapix: MQTT query_posts                        │
│         └─► If SD card: Scan /animations/ folder                │
│     └─► As each cache completes: entries become eligible        │
│                                                                 │
│  4. ONGOING: Download artworks for lookahead                    │
│     └─► Event-driven, not polling                               │
│     └─► One download at a time                                  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. Design Decisions

### Quick Reference Table (Decisions 1-24)

| # | Topic | Decision |
|---|-------|----------|
| 1 | Relationship with `channel_player.c` | **Replace entirely** |
| 2 | Local Cache format | **Use existing `makapix_channel_entry_t`** |
| 3 | Channel identification | **String identifiers** (max 32 chars) |
| 4 | NAE scope | **Global pool** across all channels |
| 5 | NAE persistence | **In-memory only** (reset on reboot) |
| 6 | MaE weights source | **From scheduler command** (UI later) |
| 7 | PrE counts source | **Server `query_posts` response** |
| 8 | History on reset | **Preserved** across scheduler commands |
| 9 | SD card in PrE mode | **recent_count = 0** |
| 10 | Playlist support | **Deferred** |
| 11 | Integration point | **Call `animation_player_request_swap()` directly** |
| 12 | Auto-swap timer | **Part of Play Scheduler** |
| 13 | HTTP API | **Update to use new API** |
| 14 | Channel ID format (Named) | `"{name}"` → `"all"`, `"featured"` |
| 15 | Channel ID format (User) | `"user:{sqid}"` → `"user:uvz"` |
| 16 | Channel ID format (Hashtag) | `"hashtag:{tag}"` → `"hashtag:sunset"` |
| 17 | SD card channel caching | **Has `.bin` file** like Makapix |
| 18 | Multi-channel refresh | **Sequential** (one at a time) |
| 19 | Intermediate progress | **Entries become eligible** (no regen) |
| 20 | Download trigger location | **download_manager** (PS signals) |
| 21 | Channel switch: cancel downloads | **No** (let complete) |
| 22 | Channel switch: keep caches | **Yes** (for future use) |
| 23 | Channel switch: clear lookahead | **Yes** (flush on command) |
| 24 | Skip behavior: no cache | **weight=0** until cache arrives |
| 25 | Skip behavior: no local file | **Skip during playback**, keep in lookahead |
| 26 | Skip behavior: 404 | **Remove permanently** (marker file) |
| 27 | Cache eviction | **Best-effort LRU** |
| 28 | SD card refresh triggers | **On switch, on upload** |
| 29 | History across commands | **prev() can go back** to old items |
| 30 | 404 tracking | **Marker file** (`{path}.404`) |
| 31 | Scheduler command params | **Channel list + exposure mode + pick mode** |

---

## 4. Scheduler Commands

### 4.1 Concept

A **Scheduler Command** is a single instruction containing all parameters needed to produce a play queue. When received, the scheduler:

1. **Flushes** the lookahead buffer
2. **Preserves** the history buffer
3. **Begins** building a new play queue

### 4.2 Command Parameters

```c
typedef struct {
    // Channel list (1 to PS_MAX_CHANNELS)
    ps_channel_spec_t channels[PS_MAX_CHANNELS];
    size_t channel_count;
    
    // Exposure mode
    ps_exposure_mode_t exposure_mode;  // EqE, MaE, or PrE
    
    // Pick mode
    ps_pick_mode_t pick_mode;          // Recency or Random
} ps_scheduler_command_t;
```

### 4.3 Channel Specification

```c
typedef enum {
    PS_CHANNEL_TYPE_NAMED,    // "all", "featured"
    PS_CHANNEL_TYPE_USER,     // "user" + user_sqid
    PS_CHANNEL_TYPE_HASHTAG,  // "hashtag" + hashtag
    PS_CHANNEL_TYPE_SDCARD,   // "sdcard"
} ps_channel_type_t;

typedef struct {
    ps_channel_type_t type;
    char name[33];            // Channel name (max 32 + null)
    char identifier[33];      // For user/hashtag types
    uint32_t weight;          // For MaE mode
} ps_channel_spec_t;
```

### 4.4 Derived Channel ID

```c
// Build unique channel_id from spec
void ps_build_channel_id(const ps_channel_spec_t *spec, char *out_id, size_t max_len) {
    switch (spec->type) {
        case PS_CHANNEL_TYPE_NAMED:
            snprintf(out_id, max_len, "%s", spec->name);
            break;
        case PS_CHANNEL_TYPE_USER:
            snprintf(out_id, max_len, "user:%s", sanitize(spec->identifier));
            break;
        case PS_CHANNEL_TYPE_HASHTAG:
            snprintf(out_id, max_len, "hashtag:%s", sanitize(spec->identifier));
            break;
        case PS_CHANNEL_TYPE_SDCARD:
            snprintf(out_id, max_len, "sdcard");
            break;
    }
}
```

---

## 5. System Architecture

### 5.1 High-Level Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         Web UI / Touch                          │
└─────────────────────────────┬───────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                         http_api.c                              │
│  POST /channel  →  play_scheduler_execute_command()             │
│  POST /next     →  play_scheduler_next()                        │
│  POST /back     →  play_scheduler_prev()                        │
└─────────────────────────────┬───────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      PLAY SCHEDULER                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐             │
│  │   History   │  │  Generator  │  │  Lookahead  │──────┐      │
│  │  Buffer(H)  │◄─┤   Engine    ├─►│  Buffer(L)  │      │      │
│  └─────────────┘  └──────┬──────┘  └─────────────┘      │      │
│                          │                               │      │
│  ┌───────────────────────┼───────────────────────┐      │      │
│  │                       ▼                       │      │      │
│  │  ┌─────────┐   ┌──────────────┐   ┌────────┐ │      │      │
│  │  │   NAE   │   │     SWRR     │   │  Pick  │ │      │      │
│  │  │  Pool   │   │  Scheduler   │   │ Modes  │ │      │      │
│  │  └─────────┘   └──────────────┘   └────────┘ │      │      │
│  └───────────────────────────────────────────────┘      │      │
│                          │                               │      │
│  ┌───────────────────────┼───────────────────────┐      │      │
│  │     Per-Channel State + Cache (.bin files)    │      │      │
│  └───────────────────────────────────────────────┘      │      │
│                          │                               │      │
│  ┌───────────────────────┴───────────────────────┐      │      │
│  │         Background Cache Refresh Task         │      │      │
│  │    (Sequential: MQTT queries / SD scan)       │      │      │
│  └───────────────────────────────────────────────┘      │      │
└─────────────────────────────┬───────────────────────────┼──────┘
                              │                           │
                              ▼                           │
┌─────────────────────────────────────────────────────────┼──────┐
│                    animation_player.c                   │      │
│              animation_player_request_swap()            │      │
└─────────────────────────────────────────────────────────┼──────┘
                                                          │
                              ┌────────────────────────────┘
                              │ Signal: lookahead changed
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                     download_manager.c                          │
│  Event triggers:                                                │
│    1. Lookahead entries added (while no download active)        │
│    2. Download completed                                        │
│  Action: Find soonest lookahead item without local file,        │
│          submit for download                                    │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 Data Flow: Scheduler Command

```
Scheduler Command Received
        │
        ▼
┌───────────────────────────────────┐
│  1. Flush lookahead buffer        │
│  2. Reset per-channel state       │
│  3. Load existing .bin caches     │
│  4. Set active=true for cached    │
│  5. Set weight=0 for uncached     │
│  6. Calculate SWRR weights        │
│  7. Start background refresh task │
└───────────────────────────────────┘
        │
        ├──────────────────────────────────┐
        │                                  │
        ▼                                  ▼
┌─────────────────────┐      ┌─────────────────────────────┐
│  Immediate playback │      │  Background: Refresh caches │
│  from cached data   │      │  (one channel at a time)    │
└─────────────────────┘      └─────────────────────────────┘
```

### 5.3 Data Flow: Navigation

```
User Action (next/touch/timer)
        │
        ▼
┌───────────────────────────────────────────────────────┐
│  PS: next()                                           │
│  1. If walking forward in history → return cached     │
│  2. If lookahead.count < L → generate_batch(L)        │
│  3. Pop artwork from lookahead                        │
│  4. If artwork.file_exists:                           │
│       → Push to history, request swap                 │
│     Else:                                             │
│       → Skip (keep in lookahead for later)            │
│       → Try next item                                 │
│  5. Signal download_manager: lookahead_changed        │
└───────────────────────────────────────────────────────┘
```

### 5.4 File Layout

```
components/
├── play_scheduler/
│   ├── CMakeLists.txt
│   ├── Kconfig
│   ├── include/
│   │   ├── play_scheduler.h              # Public API
│   │   ├── play_scheduler_types.h        # Data structures
│   │   └── play_scheduler_internal.h     # Internal APIs
│   ├── play_scheduler.c                  # Core + command handling
│   ├── play_scheduler_swrr.c             # SWRR algorithm
│   ├── play_scheduler_pick.c             # Pick modes
│   ├── play_scheduler_nae.c              # NAE pool
│   ├── play_scheduler_buffers.c          # History & Lookahead
│   ├── play_scheduler_cache.c            # Cache management (NEW)
│   ├── play_scheduler_refresh.c          # Background refresh (NEW)
│   └── play_scheduler_timer.c            # Auto-swap timer
│
└── channel_manager/
    ├── sdcard_channel_cache.c            # SD card .bin builder (NEW)
    └── ... (existing files)
```

---

## 6. Data Structures

### 6.1 Channel Specification (Input)

```c
typedef enum {
    PS_CHANNEL_TYPE_NAMED,
    PS_CHANNEL_TYPE_USER,
    PS_CHANNEL_TYPE_HASHTAG,
    PS_CHANNEL_TYPE_SDCARD,
} ps_channel_type_t;

typedef struct {
    ps_channel_type_t type;
    char name[33];              // "all", "featured", "user", "hashtag", "sdcard"
    char identifier[33];        // user_sqid or hashtag (sanitized)
    uint32_t weight;            // For MaE mode
} ps_channel_spec_t;
```

### 6.2 Scheduler Command

```c
typedef struct {
    ps_channel_spec_t channels[PS_MAX_CHANNELS];
    size_t channel_count;
    ps_exposure_mode_t exposure_mode;
    ps_pick_mode_t pick_mode;
} ps_scheduler_command_t;
```

### 6.3 Per-Channel State (Internal)

```c
typedef struct {
    // Identity
    char channel_id[64];            // Derived: "all", "user:uvz", etc.
    ps_channel_type_t type;
    
    // Cache state
    bool cache_loaded;              // .bin file loaded into memory
    bool cache_exists;              // .bin file exists on disk
    size_t entry_count;             // Number of entries in cache
    makapix_channel_entry_t *entries;  // Loaded entries (NULL if not loaded)
    
    // SWRR state
    int32_t credit;
    uint32_t weight;                // Normalized (out of 65536)
    bool active;                    // Has usable data (entry_count > 0)
    
    // Pick state
    uint32_t cursor;                // For RecencyPick
    uint64_t pick_rng_state;        // For RandomPick
    
    // Refresh state
    bool refresh_pending;
    bool refresh_in_progress;
    uint32_t total_count;           // From server (for PrE)
    uint32_t recent_count;          // From server (for PrE)
} ps_channel_state_t;
```

### 6.4 Artwork Reference

```c
typedef struct {
    int32_t artwork_id;             // Globally unique
    int32_t post_id;                // For view tracking
    char filepath[256];             // Local path
    char storage_key[96];           // Vault key
    uint32_t created_at;            // Unix timestamp
    uint32_t dwell_time_ms;         // Per-artwork (0 = default)
    asset_type_t type;              // WEBP, GIF, PNG, JPEG
    uint8_t channel_index;          // Which channel
    bool file_available;            // Local file exists?
} ps_artwork_t;
```

### 6.5 Scheduler State

```c
typedef struct {
    // Current command
    ps_scheduler_command_t current_command;
    
    // Channels
    ps_channel_state_t channels[PS_MAX_CHANNELS];
    size_t channel_count;
    
    // Configuration
    ps_exposure_mode_t exposure_mode;
    ps_pick_mode_t pick_mode;
    uint32_t history_size;          // H (default 32)
    uint32_t lookahead_size;        // L (default 32)
    
    // Buffers
    ps_artwork_t *history;
    size_t history_head;
    size_t history_count;
    int32_t history_position;       // For prev/next navigation
    
    ps_artwork_t *lookahead;
    size_t lookahead_head;
    size_t lookahead_tail;
    size_t lookahead_count;
    
    // NAE
    nae_entry_t nae_pool[NAE_POOL_SIZE];
    size_t nae_count;
    bool nae_enabled;
    uint64_t prng_nae_state;
    
    // Repeat avoidance
    int32_t last_played_id;
    
    // Background refresh
    TaskHandle_t refresh_task;
    size_t refresh_queue_head;      // Which channel to refresh next
    volatile bool refresh_abort;
    
    // Timer
    TaskHandle_t timer_task;
    uint32_t dwell_time_seconds;
    volatile bool touch_next;
    volatile bool touch_back;
    
    // Sync
    SemaphoreHandle_t mutex;
    uint32_t epoch_id;
    bool initialized;
} ps_state_t;
```

---

## 7. API Design

### 7.1 Scheduler Commands

```c
/**
 * @brief Execute a scheduler command
 * 
 * This is the primary API for changing what the scheduler plays.
 * Flushes lookahead, preserves history, begins new play queue.
 * 
 * @param command Scheduler command parameters
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_execute_command(const ps_scheduler_command_t *command);

/**
 * @brief Convenience: Play a single named channel
 * 
 * Creates a command with one channel in EqE mode.
 * 
 * @param channel_name "all", "featured", "sdcard"
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_named_channel(const char *channel_name);

/**
 * @brief Convenience: Play a user channel
 * 
 * @param user_sqid User's sqid
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_user_channel(const char *user_sqid);

/**
 * @brief Convenience: Play a hashtag channel
 * 
 * @param hashtag Hashtag (without #)
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_hashtag_channel(const char *hashtag);
```

### 7.2 Navigation

```c
/**
 * @brief Get next artwork for playback
 * 
 * Skips artworks without local files. Triggers generation if needed.
 * Calls animation_player_request_swap().
 */
esp_err_t play_scheduler_next(ps_artwork_t *out_artwork);

/**
 * @brief Go back to previous artwork
 * 
 * Navigates through history (including items from previous commands).
 */
esp_err_t play_scheduler_prev(ps_artwork_t *out_artwork);

/**
 * @brief Peek at upcoming artworks
 * 
 * Does NOT trigger generation. Returns what's in lookahead.
 */
esp_err_t play_scheduler_peek(size_t n, ps_artwork_t *out, size_t *count);

/**
 * @brief Get current artwork
 */
esp_err_t play_scheduler_current(ps_artwork_t *out_artwork);
```

### 7.3 Cache Management

```c
/**
 * @brief Trigger SD card channel refresh
 * 
 * Called when files are uploaded or user switches to SD card.
 */
esp_err_t play_scheduler_refresh_sdcard_cache(void);

/**
 * @brief Get cache status for a channel
 */
esp_err_t play_scheduler_get_cache_status(
    const char *channel_id,
    bool *out_exists,
    size_t *out_entry_count,
    time_t *out_last_modified
);
```

### 7.4 Download Integration

```c
/**
 * @brief Signal that lookahead has changed
 * 
 * Called internally after generation or after skipping an item.
 * download_manager listens for this to trigger prefetch.
 */
void play_scheduler_signal_lookahead_changed(void);

/**
 * @brief Get next artwork in lookahead that needs download
 * 
 * Called by download_manager.
 * 
 * @param out_request Download request to fill
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if all downloaded
 */
esp_err_t play_scheduler_get_next_prefetch(download_request_t *out_request);
```

---

## 8. Algorithm Details

### 8.1 SWRR Channel Selection

Same as before, but only considers channels with `active=true`:

```c
int select_channel(void) {
    // Add credits to active channels only
    for (int i = 0; i < channel_count; i++) {
        if (channels[i].active) {
            channels[i].credit += channels[i].weight;
        }
    }
    
    // Find max credit among active channels
    int best = -1;
    int32_t best_credit = INT32_MIN;
    for (int i = 0; i < channel_count; i++) {
        if (channels[i].active && channels[i].credit > best_credit) {
            best_credit = channels[i].credit;
            best = i;
        }
    }
    
    if (best >= 0) {
        channels[best].credit -= WSUM;
    }
    
    return best;  // -1 if no active channels
}
```

### 8.2 Lenient Skip Behavior

```c
// During playback (next())
while (true) {
    ps_artwork_t *item = lookahead_peek();
    if (!item) {
        return ESP_ERR_NOT_FOUND;  // No items at all
    }
    
    if (file_exists(item->filepath)) {
        // Good - play it
        lookahead_pop();
        history_push(item);
        animation_player_request_swap(item);
        return ESP_OK;
    }
    
    if (has_404_marker(item->filepath)) {
        // Permanently unavailable - remove from lookahead
        lookahead_pop();
        continue;
    }
    
    // File not downloaded yet - skip but keep for later
    // Move to next item in lookahead
    lookahead_rotate();
    
    // Safety: don't loop forever
    if (++skip_count >= lookahead_count) {
        return ESP_ERR_NOT_FOUND;  // All items unavailable
    }
}
```

### 8.3 Background Refresh (Sequential)

```c
void refresh_task(void *arg) {
    while (!refresh_abort) {
        // Find next channel needing refresh
        int ch = find_next_pending_refresh();
        if (ch < 0) {
            // All done, wait for signal
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        
        channels[ch].refresh_in_progress = true;
        
        if (channels[ch].type == PS_CHANNEL_TYPE_SDCARD) {
            refresh_sdcard_cache(ch);
        } else {
            refresh_makapix_cache(ch);  // MQTT query
        }
        
        channels[ch].refresh_in_progress = false;
        channels[ch].refresh_pending = false;
        
        // Entries now eligible (no regeneration)
        // Just update active flag and weights
        update_channel_state(ch);
    }
}
```

---

## 9. Cache Management

### 9.1 Cache File Location

All cache files stored in `/sdcard/p3a/channel/`:

```
/sdcard/p3a/channel/
├── all.bin
├── featured.bin
├── user_uvz.bin
├── hashtag_sunset.bin
├── sdcard.bin
└── .lru                # LRU metadata file
```

### 9.2 LRU Tracking (Best-Effort)

Simple approach: Update file's mtime on access.

```c
void touch_cache_file(const char *channel_id) {
    char path[256];
    snprintf(path, sizeof(path), "/sdcard/p3a/channel/%s.bin", channel_id);
    
    // Update access time
    struct utimbuf times = { time(NULL), time(NULL) };
    utime(path, &times);
}
```

### 9.3 SD Card Index Building

```c
esp_err_t build_sdcard_index(void) {
    DIR *dir = opendir("/sdcard/p3a/animations");
    if (!dir) return ESP_ERR_NOT_FOUND;
    
    // Scan for animation files
    size_t count = 0;
    makapix_channel_entry_t *entries = NULL;
    
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (is_animation_file(ent->d_name)) {
            // Add to entries array
            entries = realloc(entries, (count + 1) * sizeof(*entries));
            fill_entry_from_file(&entries[count], ent->d_name);
            count++;
        }
    }
    closedir(dir);
    
    // Write to sdcard.bin
    write_cache_file("sdcard", entries, count);
    free(entries);
    
    return ESP_OK;
}
```

### 9.4 SD Card Refresh Triggers

- **User switches to SD card channel**: Refresh immediately
- **File uploaded via HTTP**: Refresh after upload completes

---

## 10. Download Integration

### 10.1 Event-Driven Prefetch

The download_manager monitors two events:

1. **Lookahead entries added** (while no download active)
2. **Download completed**

On either event:

```c
void on_download_event(void) {
    if (download_in_progress) return;
    
    download_request_t req;
    if (play_scheduler_get_next_prefetch(&req) == ESP_OK) {
        start_download(&req);
    }
}
```

### 10.2 Prefetch Priority

Find the **soonest-coming** artwork in lookahead without local file:

```c
esp_err_t play_scheduler_get_next_prefetch(download_request_t *out) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    
    for (size_t i = 0; i < lookahead_count; i++) {
        ps_artwork_t *art = &lookahead[(lookahead_head + i) % lookahead_size];
        
        if (!file_exists(art->filepath) && !has_404_marker(art->filepath)) {
            // Found one needing download
            fill_download_request(out, art);
            xSemaphoreGive(mutex);
            return ESP_OK;
        }
    }
    
    xSemaphoreGive(mutex);
    return ESP_ERR_NOT_FOUND;
}
```

### 10.3 404 Handling

When download fails with 404:

```c
void on_download_failed(const char *filepath, int http_status) {
    if (http_status == 404) {
        // Create marker file
        char marker[280];
        snprintf(marker, sizeof(marker), "%s.404", filepath);
        FILE *f = fopen(marker, "w");
        if (f) fclose(f);
    }
}
```

---

## 11. Migration Strategy

### 11.1 Files to Modify

| File | Change |
|------|--------|
| `components/channel_manager/channel_player.c` | **DELETE** (after migration complete) |
| `components/channel_manager/channel_player.h` | **DELETE** |
| `components/http_api/http_api_rest.c` | Use `play_scheduler_execute_command()` |
| `main/p3a_main.c` | Use `play_scheduler_init()` only |
| `components/makapix/makapix.c` | Remove channel_player calls |
| `components/channel_manager/download_manager.c` | Add event-driven prefetch |

### 11.2 Files to Create/Modify

| File | Purpose |
|------|---------|
| `play_scheduler_cache.c` | Cache file management |
| `play_scheduler_refresh.c` | Background refresh task |
| `sdcard_channel_cache.c` | SD card index building |

---

## 12. Implementation Phases

### Phase 8: Channel Architecture Alignment

**Goal**: Align codebase with new channel architecture.

**Tasks**:
- [ ] Implement `ps_channel_spec_t` and `ps_scheduler_command_t`
- [ ] Implement `play_scheduler_execute_command()`
- [ ] Implement channel ID derivation and sanitization
- [ ] Implement cache file path building
- [ ] Update `http_api_rest.c` POST /channel to build scheduler commands
- [ ] Test: Single named channel command
- [ ] Test: User channel command
- [ ] Test: Hashtag channel command

### Phase 9: SD Card Index Building

**Goal**: SD card channel gets `.bin` cache file.

**Tasks**:
- [ ] Create `sdcard_channel_cache.c`
- [ ] Implement `/animations/` folder scanning
- [ ] Implement `sdcard.bin` writing
- [ ] Add refresh trigger on channel switch
- [ ] Add refresh trigger after HTTP upload
- [ ] Test: SD card index built correctly

### Phase 10: Background Refresh Task

**Goal**: Sequential background cache refresh.

**Tasks**:
- [ ] Create `play_scheduler_refresh.c`
- [ ] Implement refresh task (sequential, one at a time)
- [ ] Implement Makapix channel refresh (MQTT query)
- [ ] Implement "entries become eligible" notification
- [ ] Test: Background refresh completes
- [ ] Test: New entries become available

### Phase 11: Lenient Skip Behavior

**Goal**: Skip unavailable artworks gracefully.

**Tasks**:
- [ ] Implement skip logic in `next()`
- [ ] Implement 404 marker file handling
- [ ] Update weight=0 for channels without cache
- [ ] Test: Skip works correctly
- [ ] Test: 404 items removed permanently

### Phase 12: Download Integration

**Goal**: Event-driven lookahead prefetch.

**Tasks**:
- [ ] Add `play_scheduler_signal_lookahead_changed()`
- [ ] Add `play_scheduler_get_next_prefetch()`
- [ ] Update download_manager for event-driven triggering
- [ ] Test: Downloads triggered on lookahead change
- [ ] Test: Downloads triggered on completion

### Phase 13: Cache LRU

**Goal**: Best-effort LRU for cache files.

**Tasks**:
- [ ] Implement mtime-based LRU tracking
- [ ] Document manual cleanup procedure
- [ ] Test: LRU tracking works

### Phase 14: Final Cleanup

**Goal**: Remove legacy code, polish.

**Tasks**:
- [ ] Delete `channel_player.c/h`
- [ ] Remove all `channel_player` references
- [ ] Update documentation
- [ ] Final integration testing

---

## 13. Testing Strategy

### 13.1 Channel Command Tests

- Execute command with single named channel
- Execute command with user channel
- Execute command with hashtag channel
- Execute command with SD card channel
- Execute command with multiple channels
- Command with nonexistent channel (should skip gracefully)

### 13.2 Cache Tests

- Cache file created on first access
- Cache file loaded correctly
- SD card scan produces correct index
- LRU mtime updated on access

### 13.3 Skip Behavior Tests

- Artwork without file is skipped
- 404 artwork removed permanently
- Channel without cache gets weight=0
- Eventually plays when file arrives

### 13.4 Download Integration Tests

- Download triggered after lookahead fill
- Download triggered after completion
- Correct priority (soonest first)
- 404 creates marker file

---

## 14. Open Questions & Future Work

### 14.1 Deferred Features

| Feature | Notes |
|---------|-------|
| Playlist support | Treat as units or flatten |
| Live Mode | Synchronized playback |
| Cache eviction | Automatic cleanup when disk full |
| Multi-channel weights UI | Web UI for MaE configuration |

### 14.2 Future Considerations

- **Offline mode**: What if WiFi never connects?
- **Cache corruption**: Recovery strategy?
- **Large channels**: Paging for > 8K entries?

---

## Appendix A: Original Specification

See `docs/play-scheduler/SPECIFICATION.md`

---

## Appendix B: Change Log

| Date | Change |
|------|--------|
| 2026-01-01 | Initial plan created |
| 2026-01-01 | Revision 2: Channel architecture clarified |
| 2026-01-01 | Added scheduler commands, cache management, download integration |

---

*This document is a living document. Update it as implementation progresses.*
