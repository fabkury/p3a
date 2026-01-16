# Lookahead Concept - Remaining References Report

**Generated:** 2026-01-16  
**Status:** The codebase has migrated to a next()/peek()-only approach but contains several remaining references to the legacy "Lookahead" concept.

## Executive Summary

While p3a's implementation has successfully migrated away from using a lookahead buffer (confirmed by `lookahead_count = 0` in the stats), the concept still lingers in:
- Configuration files (Kconfig, sdkconfig)
- API documentation and comments
- Type definitions (ps_stats_t)
- Legacy documentation

## Detailed Findings

### 1. Configuration System

#### components/play_scheduler/Kconfig (Lines 11-17)

**Status:** UNUSED - Configuration defined but never referenced in code

```kconfig
config PLAY_SCHEDULER_LOOKAHEAD_SIZE
    int "Lookahead buffer size"
    default 32
    range 8 128
    help
        Number of upcoming artworks to pre-generate.
        Larger values reduce generation overhead but use more memory.
```

**Impact:** Creates a configuration option that has no effect. The `CONFIG_PLAY_SCHEDULER_LOOKAHEAD_SIZE` symbol is never used in any C/H files.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/Kconfig` lines 11-17

#### sdkconfig (Line 2583)

**Status:** UNUSED - Generated configuration value

```
CONFIG_PLAY_SCHEDULER_LOOKAHEAD_SIZE=32
```

**Impact:** Auto-generated from Kconfig. Has no effect on the build or runtime.

**Location:** `/home/runner/work/p3a/p3a/sdkconfig` line 2583

---

### 2. Type Definitions

#### components/play_scheduler/include/play_scheduler_types.h (Line 168)

**Status:** ACTIVE - Field exists in stats structure but always returns 0

```c
typedef struct {
    size_t channel_count;
    size_t history_count;
    size_t lookahead_count;  // Line 168
    size_t nae_pool_count;
    uint32_t epoch_id;
    const char *current_channel_id;
    // ... more fields
} ps_stats_t;
```

**Impact:** API consumers may expect this field to be meaningful. The field is populated in `play_scheduler_get_stats()`.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/include/play_scheduler_types.h` line 168

---

### 3. Implementation Code

#### components/play_scheduler/play_scheduler.c (Line 1432)

**Status:** ACTIVE - Explicitly sets lookahead_count to 0

```c
esp_err_t play_scheduler_get_stats(ps_stats_t *out_stats) {
    // ...
    out_stats->channel_count = s_state.channel_count;
    out_stats->history_count = s_state.history_count;
    out_stats->lookahead_count = 0;  // No longer using lookahead buffer (Line 1432)
    out_stats->nae_pool_count = s_state.nae_count;
    // ...
}
```

**Impact:** Correctly reports that lookahead is not used, but the field remains in the API.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/play_scheduler.c` line 1432

#### main/animation_player_loader.c (Line 284)

**Status:** ACTIVE - Code checks lookahead_count

```c
ps_stats_t stats = {0};
play_scheduler_get_stats(&stats);
if (stats.lookahead_count == 0) {  // Line 284
    ESP_LOGW(TAG, "Deferred cycle ignored: no animations available");
    discard_failed_swap_request(ESP_ERR_NOT_FOUND, false);
    continue;
}
```

**Impact:** This check always evaluates to true since `lookahead_count` is always 0. The logic appears to be checking if there are no animations available, but the field name is misleading.

**Location:** `/home/runner/work/p3a/p3a/main/animation_player_loader.c` line 284

---

### 4. Documentation Comments

#### play_scheduler_types.h (Line 143)

**Status:** ACTIVE - Comment in type definition

```c
/**
 * @brief Scheduler command
 *
 * Contains all parameters needed to produce a play queue.
 * Executing a command flushes lookahead but preserves history.  // Line 143
 */
typedef struct {
    ps_channel_spec_t channels[PS_MAX_CHANNELS];
    // ...
} ps_command_t;
```

**Impact:** Documentation suggests lookahead flushing happens, which is misleading.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/include/play_scheduler_types.h` line 143

#### play_scheduler.h (Line 11)

**Status:** ACTIVE - Header documentation

```c
/**
 * The Play Scheduler is a streaming generator that selects artworks from multiple
 * followed channels for presentation. Key features:
 *
 * - On-demand computation (no pre-built lookahead buffer)  // Line 11
 * - Availability masking: only locally downloaded files are visible
 * - History buffer for back-navigation
 * - Multi-channel fairness via Smooth Weighted Round Robin (SWRR)
 */
```

**Impact:** Correctly describes current architecture (mentions NO lookahead), but still references the term.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/include/play_scheduler.h` line 11

#### play_scheduler.h (Line 62)

**Status:** ACTIVE - Function documentation

```c
/**
 * @brief Execute a scheduler command
 *
 * This is the primary API for changing what the scheduler plays.
 * Flushes lookahead, preserves history, begins new play queue.  // Line 62
 *
 * @param command Scheduler command parameters
 * @return ESP_OK on success
 */
```

**Impact:** Documentation incorrectly describes lookahead flushing behavior.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/include/play_scheduler.h` line 62

#### play_scheduler.h (Line 162)

**Status:** ACTIVE - Comment about download manager

```c
// Download Manager is now decoupled and owns its own state.
// Use download_manager_set_channels() to configure active channels.
// No lookahead-based prefetch - downloads work independently.  // Line 162
```

**Impact:** Correctly explains that lookahead-based prefetch is not used.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/include/play_scheduler.h` line 162

#### play_scheduler.c (Line 66)

**Status:** ACTIVE - Comment about future Live Mode

```c
// When implementing Live Mode in Play Scheduler:
// 1. Add live_mode flag to ps_state_t
// 2. Use SNTP time sync for coordination (sntp_sync.h)
// 3. Build flattened schedule from lookahead entries  // Line 66
// 4. Calculate start_time_ms and start_frame for swap requests
// 5. Wire into swap_future.c for scheduled swaps
```

**Impact:** Future planning comment references lookahead entries.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/play_scheduler.c` line 66

#### play_scheduler.c (Line 1166)

**Status:** ACTIVE - Comment about download manager

```c
// Download Manager now has its own cursors and round-robin logic.
// See download_manager_set_channels() for configuration.
// No longer using lookahead-based prefetch.  // Line 1166
```

**Impact:** Correctly documents migration away from lookahead.

**Location:** `/home/runner/work/p3a/p3a/components/play_scheduler/play_scheduler.c` line 1166

#### main/animation_player.c (Line 343)

**Status:** ACTIVE - Comment about play_scheduler behavior

```c
// Start playback via play_scheduler.
// play_scheduler_play_named_channel() will:
// 1. Load the channel cache
// 2. Generate lookahead  // Line 343
// 3. Call play_scheduler_next() which triggers animation_player_request_swap()
```

**Impact:** Incorrectly describes play_scheduler as generating lookahead.

**Location:** `/home/runner/work/p3a/p3a/main/animation_player.c` line 343

#### download_manager.c (Line 10)

**Status:** ACTIVE - File header comment

```c
/**
 * Download Manager
 *
 * Downloads files one at a time using round-robin across channels.
 * Owns its own channel list and download cursors - fully decoupled from
 * Play Scheduler (no lookahead dependency).  // Line 10
 */
```

**Impact:** Correctly describes decoupling from lookahead.

**Location:** `/home/runner/work/p3a/p3a/components/channel_manager/download_manager.c` line 10

---

### 5. Legacy Documentation

Multiple documentation files in the `docs/legacy/` directory contain detailed references to the lookahead concept. These are explicitly marked as legacy/archived material:

- `docs/legacy/play-scheduler/SPECIFICATION.md`
- `docs/legacy/play-scheduler/DECISIONS.md`
- `docs/legacy/play-scheduler/TASK_BRIEF.md`
- `docs/legacy/play-scheduler/PROGRESS.md`
- `docs/legacy/play-scheduler/IMPLEMENTATION_PLAN.md`
- `docs/legacy/IMPLEMENTATION_TASK.md`
- `docs/legacy/MIGRATION_AND_QA.md`

**Status:** ARCHIVED - Historical documentation

**Impact:** These files document the old architecture and migration process. They serve as historical record.

---

### 6. Design Documentation

#### docs/first-principles/v2/high-level/03-content-pipeline-refactoring.md (Line 49)

**Status:** ACTIVE - Design documentation

```
│  │ - SD card    │    │ - Vault      │    │ - History    │
│  │ - Makapix    │    │ - Prefetch   │    │ - Lookahead  │  // Line 49
│  │ - Artwork    │    │ - LRU evict  │    │ - SWRR       │
```

**Impact:** Design doc shows lookahead as part of the Playback Queue component in the proposed architecture diagram.

**Location:** `/home/runner/work/p3a/p3a/docs/first-principles/v2/high-level/03-content-pipeline-refactoring.md` line 49

---

### 7. Other Files

#### docs/web-flasher/esptool-bundle.js

**Status:** EXTERNAL - Minified JavaScript library

Contains the term "lookahead" within a minified JavaScript bundle. This appears to be part of an external library (esptool) and is not related to p3a's lookahead concept.

**Impact:** None - this is third-party code.

**Location:** `/home/runner/work/p3a/p3a/docs/web-flasher/esptool-bundle.js`

---

## Summary by Category

### Active Code (Needs Review)
1. **Kconfig** (lines 11-17): Unused configuration option
2. **sdkconfig** (line 2583): Generated config value with no effect
3. **play_scheduler_types.h** (line 168): `lookahead_count` field in `ps_stats_t`
4. **play_scheduler.c** (line 1432): Hardcoded `lookahead_count = 0`
5. **animation_player_loader.c** (line 284): Check for `stats.lookahead_count == 0`

### Active Comments/Documentation (Needs Update)
1. **play_scheduler_types.h** (line 143): Comment about flushing lookahead
2. **play_scheduler.h** (line 11): Describes no lookahead (correct but mentions term)
3. **play_scheduler.h** (line 62): Comment about flushing lookahead
4. **play_scheduler.h** (line 162): Comment about no lookahead-based prefetch (correct)
5. **play_scheduler.c** (line 66): Live Mode planning comment
6. **play_scheduler.c** (line 1166): Comment about migration (correct)
7. **animation_player.c** (line 343): Incorrect comment about generating lookahead
8. **download_manager.c** (line 10): Correct comment about decoupling

### Design Documentation
1. **03-content-pipeline-refactoring.md** (line 49): Architecture diagram shows lookahead

### Archived/Legacy
- Multiple files in `docs/legacy/` directory (appropriate as historical record)

### External
- `esptool-bundle.js`: Third-party library (not relevant)

---

## Recommendations

### High Priority
1. **Remove unused Kconfig option**: Delete `PLAY_SCHEDULER_LOOKAHEAD_SIZE` from Kconfig
2. **Update incorrect comments**: Fix misleading comments in animation_player.c and play_scheduler.h
3. **Consider API cleanup**: Evaluate if `lookahead_count` field should be removed from `ps_stats_t`

### Medium Priority
1. **Update design docs**: Revise architecture diagram in content-pipeline-refactoring.md
2. **Review animation_player_loader.c**: The check for `lookahead_count == 0` seems to be a proxy for "no animations available" - consider using a more appropriate field or method

### Low Priority
1. **Clarify comments**: Comments that correctly describe the lack of lookahead could be reworded to avoid referencing the legacy term

### No Action Required
1. **Legacy documentation**: Keep as historical record
2. **Third-party code**: Ignore esptool-bundle.js

---

## Conclusion

The p3a codebase has successfully **migrated away from implementing lookahead functionality** (confirmed by the hardcoded `lookahead_count = 0`), but several **artifacts of the concept remain**:

- **Configuration**: An unused Kconfig option that has no effect
- **API**: A stats field that always returns 0
- **Comments**: Mix of correct descriptions (explaining the migration) and incorrect descriptions (suggesting lookahead still exists)
- **Design docs**: Architecture diagrams showing lookahead as a future component

The core implementation is correct - there is no lookahead buffer. The remaining references are primarily documentation/configuration artifacts that should be cleaned up for clarity.
