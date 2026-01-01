# Play Scheduler — Design Decisions

This document captures all binding design decisions made during the planning phase.

---

## Quick Reference Table

| # | Topic | Decision | Rationale |
|---|-------|----------|-----------|
| 1 | Relationship with `channel_player.c` | **Replace entirely** | Clean slate, avoid dual code paths |
| 2 | Local Cache format | **Use existing `makapix_channel_entry_t`** | Reuse proven binary format |
| 3 | Channel identification | **String identifiers** (`"all"`, `"promoted"`, `"sdcard"`) | Consistency with current system |
| 4 | NAE scope | **Global pool** | Simplicity, artworks can come from any channel |
| 5 | NAE persistence | **In-memory only** | Avoids flash wear, NAEs are transient by nature |
| 6 | MaE weights | **Dummy/random for now** | Defer UI until core is working |
| 7 | PrE counts | **From server `query_posts` response** | Server already provides this data |
| 8 | History on reset | **Preserved** | Better UX, user can still go back |
| 9 | SD card in PrE | **recent_count = 0** | No server-side recency data for local files |
| 10 | Playlist support | **Deferred** | Complex feature, implement core first |
| 11 | Integration point | **Call `animation_player_request_swap()` directly** | Match existing behavior |
| 12 | Auto-swap timer | **Part of Play Scheduler** | Single component owns playback |
| 13 | HTTP API | **Update to use new API** | Maintain web UI functionality |

---

## Detailed Decisions

### 1. Code Relationship

**Decision**: Play Scheduler **replaces** `channel_player.c` entirely.

**Implications**:
- `channel_player.c` and `channel_player.h` will be deleted
- All references to channel_player APIs must be updated
- No compatibility shim needed

---

### 2. Binary Format

**Decision**: Use existing `makapix_channel_entry_t` format for local cache.

**Implications**:
- No new binary format needed
- Existing channel index files remain valid
- SD card channel will use similar abstraction

---

### 3. Channel Identification

**Decision**: Use string identifiers like `"all"`, `"promoted"`, `"sdcard"`.

**Known IDs**:
- `"all"` - Recent artworks (all followed)
- `"promoted"` - Promoted/featured artworks
- `"sdcard"` - Local SD card files
- Future: `"hashtag:<tag>"`, `"user:<sqid>"`

---

### 4. NAE Scope

**Decision**: NAE is a **global pool** across all channels.

**Implications**:
- Single NAE pool in scheduler
- NAE entries don't track source channel
- Simplifies insertion logic

---

### 5. NAE Persistence

**Decision**: NAE pool is **in-memory only**, reset on reboot.

**Implications**:
- No flash storage for NAE
- Fast insertions
- Clean slate after power cycle

---

### 6. Manual Weights (MaE)

**Decision**: Use **dummy/random weights** for now.

**Implementation**:
```c
// Temporary: assign random weights
for (int i = 0; i < channel_count; i++) {
    channels[i].manual_weight = (pcg32_next_u32(&rng) % 100) + 1;
}
```

**Future**: Web UI for weight configuration.

---

### 7. Proportional Counts (PrE)

**Decision**: `total_count` and `recent_count` come from **server `query_posts` response**.

**Location**: First payload of the response contains:
```json
{
  "total_count": 1234,
  "recent_count": 56,
  ...
}
```

---

### 8. History Preservation

**Decision**: History buffer is **preserved** across snapshot resets.

**Implications**:
- Reset clears: lookahead, credits, cursors, NAE pool
- Reset preserves: history buffer
- User can still go back after channel switch

---

### 9. SD Card in PrE Mode

**Decision**: SD card has `recent_count = 0` in PrE calculations.

**Effect**: SD card gets weight based only on `total_count`, no recency boost.

**Alternative considered**: Synthesize from file timestamps — rejected for simplicity.

---

### 10. Playlist Support

**Decision**: **Deferred** to later implementation.

**Current behavior**: Playlists are ignored; only standalone artworks are scheduled.

**Future options**:
- (a) Flatten playlists into individual artworks
- (b) Treat playlist as single schedulable unit
- (c) Hybrid approach

---

### 11. Integration Point

**Decision**: Play Scheduler calls `animation_player_request_swap()` directly.

**Same pattern as channel_player**:
```c
esp_err_t play_scheduler_next(ps_artwork_t *out) {
    // ... selection logic ...
    
    swap_request_t request = { ... };
    return animation_player_request_swap(&request);
}
```

---

### 12. Auto-Swap Timer

**Decision**: Timer task is **part of Play Scheduler** component.

**Located in**: `play_scheduler_timer.c`

**Responsibilities**:
- Monitor dwell time
- Check touch event flags
- Call `play_scheduler_next()` on timeout

---

### 13. HTTP API Update

**Decision**: Update `http_api.c` to call Play Scheduler API.

**Mapping**:
| Endpoint | Old Call | New Call |
|----------|----------|----------|
| `POST /channel` | `channel_player_switch_channel()` | `play_scheduler_play_channel()` |
| `POST /next` | `channel_player_swap_next()` | `play_scheduler_next()` |
| `POST /back` | `channel_player_swap_back()` | `play_scheduler_prev()` |

---

## Non-Decisions (Deferred)

These items were discussed but explicitly deferred:

1. **Numeric channel IDs** - May revisit later
2. **Playlist handling** - Implement after core is stable
3. **Per-channel settings** - PE, dwell override per channel
4. **Persistent history** - Save/restore across reboots
5. **Live Mode** - Synchronized playback across devices

---

*Last updated: 2026-01-01*

