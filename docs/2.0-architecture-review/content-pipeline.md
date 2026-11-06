# Content pipeline

Part of the [2.0 architecture review](README.md). This page follows an
artwork from "channel spec in a playset" to "decoded frame handed to the
display" and asks whether the layers along the way earn their place. It
covers `play_scheduler`, `channel_manager`, `content_cache`, `loader_service`,
`playback_queue`, the five source components (`makapix`, `giphy`, `klipy`,
`art_institution`, `pin_lists`), `show_url`, `storage_eviction` and
`main/animation_player*.c`.

Line numbers refer to `main` at `1692d43a`.

## 1. The core finding: six sources, no source interface

p3a plays content from six places: Makapix (four channel types), Giphy,
Klipy, museums, pinned lists, the SD card, plus the single-artwork "show
this" path. There is a `channel_ops_t` vtable in
`components/channel_manager/include/channel_interface.h`, but it is dead
weight (section 4). What actually dispatches on source is a type ladder
repeated wherever a source-specific decision is needed:

| File | `PS_CHANNEL_TYPE_*` comparisons | `PS_ENTRY_FORMAT_*` comparisons |
|---|---|---|
| `play_scheduler_refresh.c` | 63 | |
| `play_scheduler.c` | 16 | |
| `playset_json.c` | 15 | |
| `makapix/view_tracker.c` | 13 | |
| `play_scheduler_playsets.c` | 11 | 19 |
| `play_scheduler_lai.c` | 10 | 4 |
| `http_api_rest_playsets.c` | 9 | |
| `play_scheduler_pick.c` | 6 | 11 |
| `channel_cache.c` | 4 | |
| `play_scheduler_swrr.c` | | 3 |

Plus `pinned_source_t` switches in `pin_lists.c`, `pick.c` and
`http_api_rest_pinned.c`, and about twelve indirect dispatches in the download
manager through `play_scheduler_is_{giphy,klipy,institution}_channel()`, each
a linear `strcmp` scan of the channel array taken without the mutex
(`play_scheduler_lai.c:480-557`), run three times per batch entry
(`download_manager.c:449-451`).

Five enums describe "which source": `ps_channel_type_t` (four of whose values
mean Makapix), `ps_entry_format_t`, `channel_cache_t.channel_type`,
`post_source_t`, `pinned_source_t`. Converters live at `pick.c:48-69` and
`view_tracker.c:505-518`.

The per-source duplication this produces:

| Concern | Copies | Where |
|---|---|---|
| Merge refreshed listing into cache | 3 near-identical | `giphy_refresh.c:69`, `klipy_refresh.c:60`, `art_institution_refresh.c:76` |
| Evict entries not seen this refresh | 3 | `giphy_refresh.c:160`, `klipy_refresh.c:131`, `art_institution_refresh.c:173` |
| Build the Makapix vault path | 9 | `pick.c:188`, `download_manager.c:190`, `channel_cache_merge.c:46`, `makapix_channel_impl.c:447`, `makapix_channel_utils.c:142,212,226`, `playsets.c:1176`, `makapix_single_artwork.c:37`, `playlist_manager.c:66` |
| `file_exists()` | 3 | `play_scheduler.c:59`, `pick.c:32`, `download_manager.c:145` |
| Refresh arm in the type ladder | Giphy and Klipy arms are copies | `play_scheduler_refresh.c:1326-1456` (130 lines) vs `1457-1549` (93 lines) |
| Pick functions | 3 recency + 3 random | `pick.c:242-780` |
| Download arm | 4 | `download_manager.c:857-911` |
| Soft-cap special case | per source | `channel_cache.c:410, 556, 558` |

Each new source so far (Giphy, then museums, then Klipy, then pinned) has
added one arm to every ladder and one copy of each helper. Klipy, the most
recent, needed `strstr(filepath, "/sticker/")` in the download manager to
recover its own product type (`download_manager.c:878`).

**Why it got here.** The scheduler was written for Makapix and SD card. Each
later source was added under time pressure by cloning the nearest existing
arm. That is an ordinary history; the result is still a structure where the
cost of the seventh source is the sum of the first six.

**Fresh design.** One `source_ops_t` table per source, registered at boot:

```c
typedef struct {
    const char *id;                       // "makapix", "giphy", ...
    esp_err_t (*refresh)(channel_ctx_t *, record_sink_t *);   // list into the index
    esp_err_t (*url)(const record_t *, char *out, size_t);    // where to fetch bytes
    esp_err_t (*path)(const record_t *, char *out, size_t);   // where bytes live on SD
    uint32_t  (*refresh_interval_s)(void);
    bool      (*wants_bilinear)(const record_t *);            // upscale policy
} source_ops_t;
```

The scheduler, the download manager, the loader, the view tracker and the
HTTP layer call through the table and never name a source. The ladders
collapse into a loop. A new source is one file that fills the table.

**Migration cost.** Medium. The sources already have the functions; they are
just called by name from the ladders. The work is defining the table, moving
the six per-source blocks behind it and deleting the ladders. `pick.c` and
`download_manager.c` shrink by roughly half.

**Verdict.** Would not build it this way. Fix in 2.0. This is the change that
makes items B, C and D (longer identifiers, repetition suppression, aspect
filter) one-place changes instead of six-place changes.

## 2. One record, five layouts, cast by convention

The cache entry is nominally one 64-byte record. In fact there are five
layouts (definitions on the storage page). Four share `post_id@0`, `kind@4`,
`extension@5` and a timestamp at offset 8 by convention; `channel_cache_t` is
typed on `makapix_channel_entry_t *` and the other three sources `memcpy`
their structs into those slots (`giphy_refresh.c:104-115`,
`klipy_refresh.c:82-97`, `art_institution_refresh.c:89-92`) and cast back
when reading (`pick.c:414-428`, `download_manager.c:230-238`,
`channel_cache.c:410-418`). The pinned entry does not follow the convention
(timestamp at 4, type byte at 8) and is kept outside `channel_cache_t`, in
the slot meant for the SD-card index (`playsets.c:479-517`). The discriminator
`PS_ENTRY_FORMAT_*` is not in the cache file; it exists only in the
scheduler's live state (`playsets.c:444-471`), so a `.cache` file cannot be
interpreted without the scheduler.

Growing the entry to 192 bytes (item B) in 1.x means touching three merge
functions, the batch buffer at `download_manager.c:340`, `lai.c:470`,
`cache_trim_to_cap`, the pinned union's hand-counted `pad[]` sizes, the file
format comment and `CHANNEL_CACHE_VERSION`. There is no single place.

**Fresh design.** One `record_t` with a `source` byte, a fixed common header
and a per-source `union` for identity, defined once, with the discriminator in
the file. RAM cost at the 2048 soft cap per channel: 128 KB at 64 bytes, 384
KB at 192 bytes, in PSRAM. At 64 channels the theoretical maximum is 24 MB of
a 32 MB PSRAM, so 192 bytes needs either a lower per-channel cap or a
variable-length identifier region (see the storage page for the trade).

**Verdict.** Would not build it this way. Fix in 2.0; it is required for B
anyway.

## 3. The Makapix refresh path is five layers deep for a one-layer job

Giphy, Klipy and museums refresh synchronously on the refresh task. Makapix
goes: `refresh_makapix_channel` (`play_scheduler_refresh.c:774`) creates or
reuses a legacy `channel_handle_t`, `channel_load` spawns a per-channel
FreeRTOS task with a 12 KB stack (`makapix_channel_impl.c:330-357`), the task
merges pages, `makapix_ps_refresh_mark_complete` writes to a pending table
(`makapix_refresh.c:145-234`), and the refresh task polls the table
(`play_scheduler_refresh.c:961-1092`). The `ESP_ERR_NOT_FINISHED` async/sync
split, the timeout sweep (`:1098-1113`), the reap-after-park machinery and the
tracked-handle array with eviction at 64 all exist to carry one result across
that gap.

**Why it got here.** Makapix listings arrive over MQTT in pages, so the
refresh genuinely is asynchronous. But the mechanism predates the refresh task
and was never collapsed into it.

**Fresh design.** The refresh task owns Makapix refresh like every other
source. It publishes the page request and blocks on a queue with a timeout;
the MQTT handler pushes pages into the queue. One function, no per-channel
task, no pending table, no poll loop.

**Migration cost.** Medium. The MQTT page handler is reusable; the deletion
list is `makapix_channel_impl.c` (731 lines), most of `makapix_refresh.c`, the
`NOT_FINISHED` branches in the refresh task and the reap/park code.

**Verdict.** Would not build it this way. Fix in 2.0.

## 4. Dead and near-dead layers

Each of these costs reading time on every change and some of them cost
runtime:

| Layer | Evidence | Status |
|---|---|---|
| `content_cache` component | seven one-line forwarders to `download_manager` (`content_cache.c:12-45`); `cached_item_t` unused | dead wrapper |
| `playback_queue` | `next/prev/peek` have zero callers; `current` has one (`animation_player_loader.c:437`); `map_artwork_to_request` duplicates the dwell logic of `prepare_and_request_swap` | three quarters dead |
| `channel_ops_t` vtable | three implementers; live callers use only `load` and `destroy`; `start_playback` and `get_navigator` are DEPRECATED stubs (`makapix_channel_impl.c:298, 549-554`) | dead abstraction |
| `sdcard_channel_impl.c` (605 lines) | reachable only from `load_channel_by_id` (`play_scheduler.c:257-267`) on the dead `set_channels` path | dead |
| `makapix_channel_switch.c` (589 lines) | entry point `makapix_request_channel_switch` has no callers; the `ch_switch` task waits forever on a semaphore nobody gives (`makapix_connection.c:100`); only `makapix_show_artwork` is live and it delegates to `play_scheduler_play_artwork` | dead, with a live task |
| `makapix_single_artwork.c` | creator is `__attribute__((unused))` (`:142`) | dead |
| `playlist_manager.c` (652 lines) | only `init`/`deinit` are called; playlist entries are skipped by the picker (`pick.c:394, 675`) | dead |
| `ps_channel_config_t`, `play_scheduler_set_channels`, `play_scheduler_play_channel` | zero external callers; header says "Legacy" | dead |
| `ps_channel_state_t.handle`, `ps_state_t.current_channel` | written only on the dead path | dead fields |
| `download_manager_set_next_callback` | explicit no-op (`download_manager.c:1071-1082`) | dead |

Roughly 3,000 lines. In a greenfield build none of it exists. In 2.0 it
should be deleted before anything else is touched, because several of the
cycles in the dependency graph run through these files.

## 5. Locking: one documented rule, violated three times

Mutexes in this area: `ps_state_t.mutex` (held across picks), `s_switch_mutex`,
per-cache `mutex`, `registry_mutex`, `lifecycle_mutex`, the download manager's
`s_mutex` (taken with a 100 ms timeout and silently falling through to
unlocked writes on timeout, `download_manager.c:154-164, 1113-1118`),
`s_verify_mutex`, per-Makapix-channel `index_io_lock`, `reap_lock`,
`refresh_parked_sem`, `s_tracked_mutex`, `s_ps_pending_mutex`, and
`pin_lists` `s_mutex`.

Two orderings are documented. `channel_cache.h:393`: take the state mutex
before the lifecycle lock. `lai_verify.c:28-35`: "Never take the play
scheduler's state mutex while holding the cache lifecycle lock
(execute_playset holds them in the opposite order)." `execute_playset` does
state then lifecycle (`play_scheduler_playsets.c:566-580`).

`giphy_refresh.c:484-489` takes the lifecycle lock, calls
`giphy_evict_orphans`, which calls
`play_scheduler_compensate_cursor_after_lai_remove` (`:230`), which takes the
state mutex (`play_scheduler_lai.c:306`). Lifecycle then state, on the refresh
task, while a playset switch does state then lifecycle. The same shape is in
`klipy_refresh.c:193` and `art_institution_refresh.c:233`. This is the exact
inversion `lai_verify` was written to avoid, and it has not been observed on
hardware; the window is a playset switch landing during the eviction phase of
a refresh. It should be fixed in 1.x regardless of 2.0.

Fragility markers (TODO, HACK, legacy, workaround, "stale pointer", "must
hold"): `refresh.c` 26, `download_manager.c` 19, `navigation.c` 9,
`makapix_channel_events.c` 7. "Stale pointers after batch merges" appears at
`play_scheduler_types.h:312`, `lai.c:457`, `pick.c:334, 628`.

**Fresh design.** One owner per piece of state and a message queue between
owners. The library (index, blob store, download) owns records; the player
asks it for "the next playable record" and gets a copy. No shared pointers
into a cache another task can reallocate; no lifecycle lock.

## 6. Concepts tracked in more than one place

- **LAi** (locally available index): `cache->available_post_ids` plus a hash
  for cache-backed sources; `ps_channel_state_t.available_post_ids/count` for
  SD and pinned, where the array is never populated and the count mirrors
  `entry_count`. Four eviction paths: pre-swap `stat()` (`navigation.c:203-240`),
  loader corrupt-delete (`loader.c:233-265`), pick-time hash miss
  (`pick.c:376-389, 659-671`), the verification sweep.
- **"Playable"** means LAi membership, or `stat()` at pick, or `ch->active`,
  depending on source. Up to three `stat()` calls per swap (`navigation.c:252`,
  `loader.c:528`, plus pick for SD and pinned).
- **Cursor**: `ch->cursor` (meaning differs by source: LAi index vs entry
  index), compensated at six sites; `dl_cursor` per channel in the download
  manager; the Makapix pagination cursor string in the JSON sidecar. Three
  unrelated things named cursor.
- **Freshness** is decided twice per refresh: `refresh_channel_is_fresh` uses
  the RAM copy of `last_refresh`, then dispatch re-reads the sidecar from disk
  (`refresh.c:1352, 1476, 1570, 1642`).
- **Refresh state** is four booleans (`refresh_pending`, `refresh_in_progress`,
  `refresh_async_pending`, plus `active`) plus `fail_streak`, `retry_at_us`,
  `last_refresh`, source-wide latches (Giphy/Klipy cooldown, auth, no-key, the
  per-museum rate table, the per-entry museum `download_fails`, the disk-full
  latch, the `lai_verify` cooldown). The SNTP, PICO-8, periodic and timeout
  one-shots in `refresh.c:902-958, 1098-1113, 1225-1278` reconcile flags that
  drifted.
- **Offset** is persisted in the playset and hashed into `channel_id`, then
  applied with a different meaning per source: slice of a local list (SD,
  pinned), API start offset modulo a cap (Giphy, Klipy), adapter-defined
  (museum), ignored (Makapix).

**Fresh design.** One `refresh_state_t` enum per channel (`IDLE`, `DUE`,
`RUNNING`, `BACKOFF(until)`, `BLOCKED(reason)`) with one deadline. One
availability model: a record is playable if and only if its blob is present in
the blob store's index; the file system is consulted at load time only, and a
miss there is a repair event, not a pick-time branch.

## 7. The loader commits history before the bytes are read

`ps_history_push` and `last_played_id` run at `navigation.c:357-362` as soon
as `animation_player_request_swap` accepts the request
(`animation_player.c:515`), before any byte is read. Every loader-stage
failure therefore leaves a history entry for an artwork never shown, and
Previous can land on it. `docs/deferred/aspect-ratio-filter.md` records this
as an accepted defect for that feature; it is a defect of the pipeline.

The loader task (`animation_player_loader.c:336-596`) has roughly nine
terminal paths classified by `esp_err_t` value plus path-substring matching.
The loader and the render task coordinate through shared flags
(`prefetch_pending`, `prefetch_in_progress`, `s_swap_requested`,
`s_load_override`) under one mutex, with a 15 s "deadlock breaker" timeout
(`:374-390`) and a comment describing the use-after-free it prevents.

**Fresh design.** The pipeline is a chain of owned hand-offs: scheduler picks
a record and emits a `swap_intent`; the loader decodes the first frame into a
staging buffer it owns and emits `swap_ready(record, buffer)`; the render task
adopts the buffer on its next frame boundary and emits `swap_shown(record)`;
history, `last_played_id`, the view tracker and the repetition-suppression
counters (item C) all key off `swap_shown`. No shared flags, no timeouts as
synchronization.

**Migration cost.** Medium. The display pipeline below the render task does
not change. The loader task and `animation_player_render.c` are rewritten
around explicit messages; roughly 1,500 lines touched.

**Verdict.** Would not build it this way. Fix in 2.0. Item C depends on an
honest "shown" event and 1.x does not have one.

## 8. Where items C and D land

With sections 1, 2 and 7 in place:

- **Suppress repetitions (C).** A RAM-only ring of recently shown record
  identities with timestamps, fed by `swap_shown`. The picker asks
  `recency_penalty(record)` and scales the random weight. One function in the
  picker, one ring in the player, no per-source code, reset by reboot as
  specified. The random pick today is a uniform index into LAi
  (`pick_random_*`); weighted sampling over a window is a small change once
  there is one pick function instead of three.
- **Filter aspect ratio (D).** `docs/deferred/aspect-ratio-filter.md` is a
  sound design for 1.x and most of its traps disappear in 2.0: the sentinel
  error exists (section 8 of the firmware page), the loader does not match
  path strings, and the measurement memo is a field of `record_t` rather than
  a side channel into six entry structs. The filter itself is one predicate in
  the picker.

## 9. What is good and should be kept

- Smooth weighted round robin with credit rollback on rejected picks
  (`play_scheduler_swrr.c`, `navigation.c:333-375`) is correct and worth
  keeping as the channel-selection core.
- The LAi idea (an index of locally available records so picks need no file
  system I/O) is right; it just needs one implementation.
- The download manager's snapshot-then-commit pattern with an epoch guard
  (`download_manager.c:272-306`) is the right way to run file I/O outside a
  lock.
- The failure-mode split between user-initiated (loud) and automatic (silent
  with bounded retries) swaps is good product behaviour and should survive
  the loader rewrite.
- `http_fetch_to_file` with atomic rename is the right download primitive.
- `storage_eviction` being layout-unaware (walk, allowlist by extension, age
  by mtime, rmdir on the way out) is a good property to preserve.
