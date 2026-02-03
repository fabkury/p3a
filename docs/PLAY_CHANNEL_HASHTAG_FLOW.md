# Play Channel Hashtag Command Flow Analysis

This document answers: **How does p3a respond to a play_channel command containing a hashtag channel payload? What exactly is the payload of the ensuing query_posts command performed by the channel refresh mechanism that is ultimately triggered?**

## Overview

When p3a receives a `play_channel` command with a hashtag channel payload, it triggers a multi-stage process:

1. **Command Reception** → Parse hashtag from MQTT payload
2. **Channel Setup** → Create hashtag channel cache entry
3. **Refresh Task** → Background task queries posts from Makapix server
4. **Query Posts Loop** → Paginated queries using cursor-based pagination

---

## 1. play_channel Command Reception

**Location**: `components/http_api/http_api.c:217-279`

The `play_channel` command is received via MQTT in the `makapix_command_handler()` function.

### Input Payload Example

```json
{
  "command_type": "play_channel",
  "channel_name": "hashtag",
  "hashtag": "sunset"
}
```

### Processing Flow

1. Extract `channel_name` field → identifies channel type as "hashtag"
2. Extract `hashtag` field → gets the hashtag value (e.g., "sunset")
3. Call `play_scheduler_play_hashtag_channel(tag)` with the hashtag
4. Persist playset name as `"hashtag_sunset"` in NVS

**Code snippet**:
```c
else if (strcmp(channel, "hashtag") == 0) {
    cJSON *hashtag = cJSON_GetObjectItem(payload, "hashtag");
    if (!hashtag || !cJSON_IsString(hashtag)) {
        ESP_LOGE(HTTP_API_TAG, "play_channel hashtag: missing hashtag");
        return;
    }
    const char *tag = cJSON_GetStringValue(hashtag);
    err = play_scheduler_play_hashtag_channel(tag);
    snprintf(playset_name, sizeof(playset_name), "hashtag_%s", tag);
}
```

---

## 2. Channel Setup

**Location**: `components/play_scheduler/play_scheduler_commands.c`

The `play_scheduler_play_hashtag_channel()` function:

1. Builds channel ID: `"hashtag_{tag}"` (e.g., `"hashtag_sunset"`)
2. Creates or retrieves channel cache entry via channel manager
3. Sets channel type to `MAKAPIX_CHANNEL_HASHTAG`
4. Stores hashtag string in channel metadata
5. Triggers channel refresh task

---

## 3. Channel Refresh Mechanism

**Location**: `components/channel_manager/makapix_channel_refresh.c:480-720`

The refresh task (`refresh_task_impl()`) is a background FreeRTOS task that:

### Initialization Phase

1. **Waits for MQTT connection** before starting queries
2. **Determines channel type** from channel ID:
   - Identifies `"hashtag_"` prefix → sets `channel_type = MAKAPIX_CHANNEL_HASHTAG`
   - Extracts hashtag string (e.g., "sunset") from channel ID
3. **Loads saved cursor** from metadata file (for pagination resume)

**Code snippet**:
```c
else if (strncmp(ch->channel_id, "hashtag_", 8) == 0) {
    channel_type = MAKAPIX_CHANNEL_HASHTAG;
    strncpy(query_req.hashtag, ch->channel_id + 8, sizeof(query_req.hashtag) - 1);
}

query_req.channel = channel_type;
query_req.sort = MAKAPIX_SORT_SERVER_ORDER;
query_req.limit = 32;
query_req.has_cursor = false;
```

### Refresh Loop

The task loops until `TARGET_COUNT` artworks are cached or no more posts available:

1. **Construct query request** with hashtag
2. **Call `makapix_api_query_posts()`** via MQTT
3. **Merge batch** into channel cache
4. **Update cursor** for next pagination
5. **Signal download manager** to fetch artwork files
6. **Save metadata** (cursor position) for resume capability
7. **Check storage pressure** and evict if needed

---

## 4. query_posts Command Payload

**Location**: `components/makapix/makapix_api.c:482-526`

The `makapix_api_query_posts()` function constructs the MQTT request payload.

### Request Structure Definition

**Type**: `makapix_query_request_t` (from `components/makapix/makapix_api.h:36-46`)

```c
typedef struct {
    makapix_channel_type_t channel;  // MAKAPIX_CHANNEL_HASHTAG
    char user_sqid[64];              // (unused for hashtag)
    char hashtag[64];                // Required when channel == HASHTAG
    makapix_sort_mode_t sort;        // MAKAPIX_SORT_SERVER_ORDER
    bool has_cursor;                 // true after first query
    char cursor[64];                 // Pagination cursor
    uint8_t limit;                   // 1-32 (default: 32)
    bool random_seed_present;        // false for hashtag
    uint32_t random_seed;            // (unused for hashtag)
} makapix_query_request_t;
```

### JSON Payload (First Query)

For a hashtag channel refresh, the **initial** query_posts payload sent via MQTT:

```json
{
  "request_type": "query_posts",
  "channel": "hashtag",
  "hashtag": "sunset",
  "sort": "server_order",
  "cursor": null,
  "limit": 32
}
```

### JSON Payload (Subsequent Queries)

After receiving a response with `has_more: true` and a `next_cursor`:

```json
{
  "request_type": "query_posts",
  "channel": "hashtag",
  "hashtag": "sunset",
  "sort": "server_order",
  "cursor": "eyJvZmZzZXQiOjMyLCJ0aW1lc3RhbXAiOiIyMDI2LTAyLTAzVDIwOjAwOjAwWiJ9",
  "limit": 32
}
```

### Payload Construction Code

```c
cJSON_AddStringToObject(root, "request_type", "query_posts");
cJSON_AddStringToObject(root, "channel", channel_to_string(req->channel));  // "hashtag"
cJSON_AddStringToObject(root, "sort", sort_to_string(req->sort));           // "server_order"

if (req->channel == MAKAPIX_CHANNEL_HASHTAG) {
    cJSON_AddStringToObject(root, "hashtag", req->hashtag);  // "sunset"
}

if (req->has_cursor) {
    cJSON_AddStringToObject(root, "cursor", req->cursor);
} else {
    cJSON_AddNullToObject(root, "cursor");
}

uint8_t limit = req->limit;
if (limit == 0) limit = 30;
if (limit > 32) limit = 32;  // Maximum per spec
cJSON_AddNumberToObject(root, "limit", limit);
```

---

## 5. Response Structure

**Type**: `makapix_query_response_t` (from `components/makapix/makapix_api.h:90-98`)

### Response Fields

```c
typedef struct {
    bool success;                     // Operation success flag
    char error[96];                   // Error message if failed
    char error_code[48];              // Error code if failed
    makapix_post_t posts[50];         // Array of posts (artworks/playlists)
    size_t post_count;                // Number of posts in this batch
    bool has_more;                    // More results available?
    char next_cursor[64];             // Cursor for next batch
} makapix_query_response_t;
```

### Example Response

```json
{
  "success": true,
  "posts": [
    {
      "post_id": 12345,
      "kind": "artwork",
      "owner_handle": "artist123",
      "storage_key": "abc123def456",
      "art_url": "https://cdn.makapix.club/art/abc123def456.webp",
      "created_at": "2026-02-01T10:30:00Z",
      "artwork_modified_at": "2026-02-01T10:30:00Z"
    },
    // ... up to 32 artworks
  ],
  "post_count": 32,
  "has_more": true,
  "next_cursor": "eyJvZmZzZXQiOjMyLCJ0aW1lc3RhbXAiOiIyMDI2LTAyLTAzVDIwOjAwOjAwWiJ9"
}
```

---

## 6. Pagination Flow

The refresh task implements **cursor-based pagination**:

```
┌─────────────────────┐
│  Initial Query      │
│  cursor: null       │
│  limit: 32          │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│  Response           │
│  post_count: 32     │
│  has_more: true     │
│  next_cursor: "..." │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│  Next Query         │
│  cursor: "..."      │
│  limit: 32          │
└──────────┬──────────┘
           │
          ...
           │
           ▼
┌─────────────────────┐
│  Final Response     │
│  post_count: 15     │
│  has_more: false    │
└─────────────────────┘
```

### Key Behavior

- **Batch size**: 32 artworks per query (maximum allowed)
- **Target cache size**: Configurable via `config_store_get_channel_cache_size()` (default: 1024)
- **Sort order**: `server_order` (chronological by server processing time)
- **Cursor persistence**: Saved to SD card metadata for resume after reboot
- **Storage eviction**: Automatic when cache exceeds target or storage pressure detected

---

## 7. Complete Flow Diagram

```
User/Server                     p3a Device
    │                               │
    │  play_channel (MQTT)          │
    │  {channel: "hashtag",         │
    │   hashtag: "sunset"}          │
    ├──────────────────────────────>│
    │                               │
    │                        ┌──────▼──────┐
    │                        │ Parse payload│
    │                        │ Extract tag  │
    │                        └──────┬──────┘
    │                               │
    │                        ┌──────▼──────────┐
    │                        │ Create channel  │
    │                        │ ID: hashtag_... │
    │                        │ Type: HASHTAG   │
    │                        └──────┬──────────┘
    │                               │
    │                        ┌──────▼──────────┐
    │                        │ Start refresh   │
    │                        │ task (async)    │
    │                        └──────┬──────────┘
    │                               │
    │  query_posts (MQTT)           │
    │  {channel: "hashtag",         │
    │   hashtag: "sunset",          │
    │   cursor: null, limit: 32}    │
    │<──────────────────────────────┤
    │                               │
    │  Response (32 artworks)       │
    │  {has_more: true,             │
    │   next_cursor: "..."}         │
    ├──────────────────────────────>│
    │                               │
    │                        ┌──────▼──────────┐
    │                        │ Cache artworks  │
    │                        │ Save cursor     │
    │                        └──────┬──────────┘
    │                               │
    │  query_posts (MQTT)           │
    │  {channel: "hashtag",         │
    │   hashtag: "sunset",          │
    │   cursor: "...", limit: 32}   │
    │<──────────────────────────────┤
    │                               │
    │  ... (repeat until target)    │
    │                               │
```

---

## Summary

### Direct Answer to Problem Statement

**Q: How does p3a respond to a play_channel command containing a hashtag channel payload?**

**A**: p3a:
1. Parses the hashtag from the payload
2. Creates a channel cache entry with ID `"hashtag_{tag}"` and type `MAKAPIX_CHANNEL_HASHTAG`
3. Spawns a background refresh task that waits for MQTT connectivity
4. The refresh task enters a pagination loop to populate the channel cache

**Q: What exactly is the payload of the ensuing query_posts command?**

**A**: The query_posts payload for a hashtag channel contains:

```json
{
  "request_type": "query_posts",
  "channel": "hashtag",
  "hashtag": "{tag}",
  "sort": "server_order",
  "cursor": null,       // or cursor string for pagination
  "limit": 32
}
```

Where:
- `{tag}` is extracted from the original play_channel payload
- `cursor` is `null` for first query, then uses `next_cursor` from previous response
- `limit` is always 32 (maximum per Makapix API specification)
- `sort` is always "server_order" (chronological by server processing)

---

## Related Files

| File | Purpose |
|------|---------|
| `components/http_api/http_api.c` | MQTT command reception |
| `components/play_scheduler/play_scheduler_commands.c` | Channel routing |
| `components/channel_manager/makapix_channel_refresh.c` | Refresh task loop |
| `components/makapix/makapix_api.c` | query_posts construction |
| `components/makapix/makapix_api.h` | Type definitions |

---

*Generated: 2026-02-03*
