# Play Scheduler — Design Decisions

This document captures all binding design decisions made during the planning phase.

---

## Quick Reference Table

| # | Topic | Decision | Rationale |
|---|-------|----------|-----------|
| 1 | Relationship with `channel_player.c` | **Replace entirely** | Clean slate, avoid dual code paths |
| 2 | Local Cache format | **Use existing `makapix_channel_entry_t`** | Reuse proven binary format |
| 3 | Channel identification | **String identifiers** (max 32 chars) | Consistency with current system |
| 4 | NAE scope | **Global pool** | Simplicity, artworks can come from any channel |
| 5 | NAE persistence | **In-memory only** | Avoids flash wear, NAEs are transient |
| 6 | MaE weights | **From scheduler command** | UI later; command carries weights |
| 7 | PrE counts | **From server `query_posts` response** | Server provides `total_count`, `recent_count` |
| 8 | History on reset | **Preserved** across commands | Better UX, prev() still works |
| 9 | SD card in PrE | **recent_count = 0** | No server-side recency data |
| 10 | Playlist support | **Deferred** | Complex feature, implement core first |
| 11 | Integration point | **Call `animation_player_request_swap()` directly** | Match existing behavior |
| 12 | Auto-swap timer | **Part of Play Scheduler** | Single component owns playback |
| 13 | HTTP API | **Update to use new API** | Maintain web UI functionality |
| 14 | Channel ID (Named) | `"{name}"` → `"all"`, `"featured"` | Simple, matches existing usage |
| 15 | Channel ID (User) | `"user:{sqid}"` → `"user:uvz"` | Unique, parse-able |
| 16 | Channel ID (Hashtag) | `"hashtag:{tag}"` → `"hashtag:sunset"` | Unique, parse-able |
| 17 | SD card channel caching | **Has `.bin` file** | Consistent with Makapix channels |
| 18 | Multi-channel refresh | **Sequential** (one at a time) | Simpler, avoid bus contention |
| 19 | Intermediate progress | **Entries become eligible** | No regeneration needed |
| 20 | Download trigger location | **download_manager** | PS signals, DM acts |
| 21 | Cancel downloads on switch | **No** (let complete) | Avoids wasted bandwidth |
| 22 | Keep caches on switch | **Yes** | For future use |
| 23 | Clear lookahead on command | **Yes** | Fresh queue per command |
| 24 | Skip: no cache | **weight=0** until cache arrives | Lenient, non-blocking |
| 25 | Skip: no local file | **Skip during playback** | Keep in lookahead for later |
| 26 | Skip: 404 | **Remove permanently** | Marker file `{path}.404` |
| 27 | Cache eviction | **Best-effort LRU** | Track via mtime |
| 28 | SD card refresh triggers | **(a) On switch, (c) On upload** | Not on boot |
| 29 | History across commands | **prev() can go back** to old items | History is a log |
| 30 | 404 tracking | **Marker file** (`{path}.404`) | Current codebase approach |
| 31 | Scheduler command params | **Channel list + exposure + pick** | No NAE enabled, no dwell time |

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
- SD card channel uses the same format

---

### 3. Channel Identification

**Decision**: Use string identifiers with max 32 characters.

**Format**:
- Named: `"{name}"` → `"all"`, `"featured"`
- User: `"user:{sqid}"` → `"user:uvz"`
- Hashtag: `"hashtag:{tag}"` → `"hashtag:sunset"`
- SD card: `"sdcard"`

---

### 4-5. NAE Scope & Persistence

**Decision**: NAE is a **global pool**, **in-memory only**.

**Implications**:
- Single NAE pool (not per-channel)
- Reset on reboot
- Fast insertions, no flash wear

---

### 6. Manual Weights (MaE)

**Decision**: Weights come from the **scheduler command**.

**Implementation**:
- `ps_channel_spec_t.weight` field
- Web UI for configuration is deferred
- For now, can use test values

---

### 7. Proportional Counts (PrE)

**Decision**: `total_count` and `recent_count` from **server `query_posts` response**.

**Location**: First payload of MQTT response contains:
```json
{
  "total_count": 1234,
  "recent_count": 56,
  ...
}
```

---

### 8. History Preservation

**Decision**: History buffer **preserved** across scheduler commands.

**Implications**:
- Command execution clears: lookahead, credits, cursors
- Command execution preserves: history buffer
- User can navigate back to items from previous commands

---

### 9. SD Card in PrE Mode

**Decision**: SD card has `recent_count = 0`.

**Effect**: SD card gets weight based only on `total_count`, no recency boost.

---

### 10. Playlist Support

**Decision**: **Deferred** to later implementation.

---

### 11. Integration Point

**Decision**: Play Scheduler calls `animation_player_request_swap()` directly.

---

### 12. Auto-Swap Timer

**Decision**: Timer task is **part of Play Scheduler** component.

**Located in**: `play_scheduler_timer.c`

---

### 13. HTTP API Update

**Decision**: Update `http_api.c` to call Play Scheduler API.

---

### 14-16. Channel ID Formats

**Decision**: Channel IDs follow consistent format:

| Type | Format | Example |
|------|--------|---------|
| Named | `"{name}"` | `"all"`, `"featured"` |
| User | `"user:{sqid}"` | `"user:uvz"` |
| Hashtag | `"hashtag:{tag}"` | `"hashtag:sunset"` |
| SD Card | `"sdcard"` | `"sdcard"` |

**Sanitization**: User sqid and hashtag must be sanitized for filesystem:
- Replace non-alphanumeric with `_`
- Accept rare collision risk (deferred)

---

### 17. SD Card Channel Caching

**Decision**: SD card channel **has a `.bin` cache file**, just like Makapix channels.

**File**: `/sdcard/p3a/channel/sdcard.bin`

**Built by**: Scanning `/sdcard/p3a/animations/` folder

---

### 18. Multi-Channel Refresh Strategy

**Decision**: Refresh channels **sequentially** (one at a time).

**Rationale**:
- Simpler implementation
- Avoids WiFi/SD bus contention
- Easier to track progress

---

### 19. Intermediate Refresh Progress

**Decision**: When a channel finishes refreshing, its **entries become eligible** for the scheduler.

**No regeneration**: The lookahead isn't regenerated. Newly-eligible entries will be picked on future generation rounds.

---

### 20. Download Trigger Location

**Decision**: **download_manager** handles actual downloads; Play Scheduler signals when lookahead changes.

**Flow**:
1. PS generates lookahead entries
2. PS signals "lookahead changed"
3. DM checks for items needing download
4. DM downloads one at a time

---

### 21. Cancel Downloads on Channel Switch

**Decision**: **No** — let in-flight downloads complete.

**Rationale**: Avoids wasted bandwidth; downloaded file may be useful later.

---

### 22. Keep Caches on Channel Switch

**Decision**: **Yes** — cache files are preserved.

**Rationale**: User may switch back; avoid re-downloading metadata.

---

### 23. Clear Lookahead on New Command

**Decision**: **Yes** — flush lookahead when new command received.

**Rationale**: Fresh queue reflects new command's channels and weights.

---

### 24. Skip Behavior: No Cache

**Decision**: Channel gets **weight=0** until its cache arrives.

**Effect**: Channel is skipped in SWRR until background refresh completes.

---

### 25. Skip Behavior: No Local File

**Decision**: **Skip during playback**, but keep item in lookahead.

**Rationale**: File may arrive soon (being downloaded); don't lose track of it.

---

### 26. Skip Behavior: 404

**Decision**: **Remove permanently** from consideration.

**Implementation**: Create marker file `{filepath}.404`

**Effect**: Item filtered out of all future picks.

---

### 27. Cache Eviction

**Decision**: **Best-effort LRU** using file mtime.

**Implementation**:
- Touch cache file mtime on access
- Manual cleanup if needed (or future automatic eviction)

---

### 28. SD Card Refresh Triggers

**Question**: When should SD card channel be refreshed?

**Decision**:
- ✅ (a) When user switches to SD card channel
- ❌ (b) On boot (if SD card was last active)
- ✅ (c) When files are added via upload endpoint

---

### 29. History Across Scheduler Commands

**Question**: If user was playing A+B, then switches to C, can prev() go back to A+B items?

**Decision**: **Yes** — history is a log, prev() navigates the entire log.

**Implication**: prev() works across command boundaries.

---

### 30. 404 Tracking

**Question**: How to persist 404 failures?

**Decision**: **Marker file** approach — create `{artwork_path}.404`

**Matches**: Current codebase approach.

---

### 31. Scheduler Command Parameters

**Question**: What parameters make up a scheduler command?

**Decision**: A scheduler command contains:
- **Channel list**: Array of `ps_channel_spec_t` with:
  - `type`: named | user | hashtag | sdcard
  - `name`: e.g., `"all"`, `"user"`, `"hashtag"`, `"sdcard"`
  - `identifier`: (for user/hashtag) e.g., `"uvz"`, `"sunset"`
  - `weight`: (for MaE mode)
- **Exposure mode**: EqE | MaE | PrE
- **Pick mode**: Recency | Random

**NOT included** (for now):
- NAE enabled flag
- Dwell time

---

## Non-Decisions (Deferred)

These items were discussed but explicitly deferred:

1. **Numeric channel IDs** - May revisit later
2. **Playlist handling** - After core is stable
3. **Per-channel settings** - Dwell override per channel
4. **Persistent history** - Save/restore across reboots
5. **Live Mode** - Synchronized playback
6. **Automatic cache eviction** - When disk is full
7. **Channel ID collision handling** - For sanitized names

---

*Last updated: 2026-01-01 (Revision 2)*
