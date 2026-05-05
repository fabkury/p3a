# AIC and APOD Channel Sources — Design

**Status:** Draft, awaiting user approval
**Date:** 2026-05-05
**Scope:** Two new passive content sources for p3a's channel framework.

## 1. Goals and non-goals

### Goals

- Add two new public-API-backed image-feed channel types to p3a:
  - **AIC** — Art Institute of Chicago Public API (broad, large curated museum collection, random-pick model).
  - **APOD** — NASA Astronomy Picture of the Day (daily picture-of-the-day cadence).
- Each source is its own hand-coded module under `components/`, modeled on the existing `giphy/` component.
- Both must integrate cleanly with the existing channel framework: `play_scheduler`, `channel_interface`, `download_manager`, `storage_eviction`, vault sharding, and `event_bus`.
- Users can include AIC and APOD channels in playsets alongside existing Makapix/Giphy/SD-card channels with their own SWRR weights.

### Non-goals

- A generic "external feed" abstraction driven by config files. Each source remains hand-coded; per-source variation in auth, pagination, attribution, and content filtering does not factor cleanly into a config-only model.
- A pixel-art-strict source. Survey of free public APIs (Pixilart, Pixeljoint, Lospec, OpenGameArt) found no source with a usable live JSON feed for an embedded HTTPS client. Pixel-art identity remains served by Makapix, Giphy, and SD card.
- Per-channel `pick_mode`. The play scheduler's `pick_mode` is per-playset today; this spec does not change that. A user who wants "newest from AIC, random from Giphy" must put them in different playsets.
- Real-time push from these sources. Both are pulled on a refresh timer plus on-demand image fetch.
- Extracting shared scaffolding across `giphy/`, `aic/`, and `apod/`. Three samples is too few; a future PR can extract helpers once duplication patterns are stable.

## 2. Architecture overview

Two new components under `components/`:

```
components/aic/
  aic_api.c           HTTP + JSON parse for AIC search endpoint
  aic_cache.c         aic-<spec>.bin entry serialization
  aic_download.c      IIIF image fetch, atomic write to vault
  aic_refresh.c       Refresh-task lifecycle and timer
  CMakeLists.txt
  Kconfig
  include/aic.h

components/apod/
  apod_api.c          HTTP + JSON parse for APOD endpoint
  apod_cache.c        apod.bin entry serialization
  apod_download.c     hdurl/url image fetch with magic-byte sniff
  apod_refresh.c      Daily timer with backfill on first run
  CMakeLists.txt
  Kconfig
  include/apod.h
```

Two values appended to `ps_channel_type_t` (in `play_scheduler_types.h`):

```c
PS_CHANNEL_TYPE_AIC  = 7,
PS_CHANNEL_TYPE_APOD = 8,
```

Append-only is required because ordinals are persisted in playset files (see comment on the existing enum).

Two values appended to `ps_entry_format_t`:

```c
PS_ENTRY_FORMAT_AIC,
PS_ENTRY_FORMAT_APOD,
```

Both new components implement `channel_interface_t` (the same vtable that `sdcard_channel_impl` and `makapix_channel_impl` already implement). The play scheduler, `download_manager`, and `storage_eviction` see them only through that interface.

### Storage layout

Each source owns a top-level vault directory under `/sdcard/p3a/`, with the same 3-level SHA256 sharding used by `vault/` (Makapix) and `giphy/`:

```
/sdcard/p3a/aic/<aa>/<bb>/<cc>/<storage_key>.jpg
/sdcard/p3a/apod/<aa>/<bb>/<cc>/<storage_key>.<ext>
```

Channel metadata `.bin` files share the existing `/sdcard/p3a/channel/` directory, mirroring how Makapix and Giphy place their `.bin` files there today:

```
/sdcard/p3a/channel/aic-<spec_hash>.bin     one per AIC channel spec; spec_hash = SHA256("aic:<name>:<identifier>")[:16]
/sdcard/p3a/channel/apod.bin                singleton (only one APOD channel exists)
```

Storage key derivation:

- AIC: `aic:<object_id>` → SHA256 → first three bytes give the shard path; full hash gives the basename.
- APOD: `apod:<YYYY-MM-DD>` → SHA256 → same derivation.

`storage_eviction` is extended to walk `aic/` and `apod/` in addition to `vault/` and `giphy/`. The age-based logic is unchanged.

## 3. AIC channel module

### Channel spec

Two flavors via the existing `ps_channel_spec_t`:

| `name`            | `identifier`         | Behavior                                                                |
|-------------------|----------------------|-------------------------------------------------------------------------|
| `"public_domain"` | `""`                 | Random objects from the public-domain collection.                       |
| `"search"`        | user-supplied query  | Same, restricted to results matching the query (still public-domain).   |

Multiple search channels are explicitly supported. A user may add `(AIC, search, "ukiyo-e")` and `(AIC, search, "impressionism")` as independent channels in the same playset, each with its own weight.

### Refresh task (`aic_refresh.c`)

- **Triggers:** first include in a playset; periodic timer (`CONFIG_AIC_REFRESH_INTERVAL_HOURS`, default 168 = weekly); manual refresh from web UI.
- **Endpoint:** `POST https://api.artic.edu/api/v1/artworks/search`
  - Off-device validation found that the GET form with `sort=field:direction` is rejected with HTTP 400; AIC's ES wrapper interprets the whole string as a literal field name. The working form is to POST an Elasticsearch query DSL body and keep `limit`/`page` as URL params.
- **URL parameters:**
  - `limit=100`
  - `page=<random 1..MAX_RANDOM_PAGE>` for `public_domain` flavor; `page=1` for search flavor.
  - **MAX_RANDOM_PAGE is ~10**, not the API-reported `total_pages`. AIC enforces a hard offset cap of approximately 1000 results — page≥11 with `limit=100` returns HTTP 403 "Invalid number of results". The refresh task must clamp `page` to `[1, 10]` regardless of `pagination.total_pages` from the response.
- **JSON body (Content-Type: application/json):**
  ```json
  {
    "query": { "term": { "is_public_domain": true } },                       // public_domain flavor
    "query": { "bool": { "must": [
      { "term": { "is_public_domain": true } },
      { "query_string": { "query": "<identifier>" } }
    ] } },                                                                    // search flavor
    "sort":  [ { "source_updated_at": { "order": "desc" } } ],
    "fields": ["id", "title", "image_id", "date_display", "source_updated_at"]
  }
  ```
- **Headers:**
  - `User-Agent: p3a/<VERSION> (+<project URL>)` — AIC explicitly requests non-anonymous identification. Exact URL filled in at implementation time.
- **Processing:**
  - Skip entries whose `image_id` is null.
  - Append surviving entries to the channel cache; cap at `CONFIG_AIC_CACHE_SIZE` (default 500), oldest entries evicted when cap exceeded.
- **Rate limit posture:** AIC caps at 60 req/min/IP. The refresh task issues at most 1 search request per cycle plus per-image fetches on demand. Honor `Retry-After` on 429.

### On-demand image fetch (`aic_download.c`)

- Triggered when the play scheduler picks an AIC entry whose vault file is missing.
- Storage key: `aic:<object_id>` → SHA256 → shard path under `aic/`.
- Endpoint: `GET https://www.artic.edu/iiif/2/<image_id_uuid>/full/1024,/0/default.jpg`
  - 1024-wide IIIF resize, slightly larger than the 720 panel width to give the renderer headroom for portrait-orientation images.
- Atomic write: download to `<path>.tmp`, then rename to final path.
- Format: always JPEG; `extension` field hard-coded.

### Entry layout (`aic_channel_entry_t`, ~64 bytes)

```c
typedef struct __attribute__((packed)) {
    int32_t  object_id;          // AIC object ID                      (4 bytes)
    char     image_id[37];       // UUID hex + null                    (37 bytes)
    uint8_t  extension;          // always JPEG                        (1 byte)
    uint8_t  kind;               // 0=image, 0xFF=dead (404)           (1 byte)
    uint8_t  reserved[2];        //                                    (2 bytes)
    uint32_t created_at;         // when we cached it (Unix)           (4 bytes)
    uint32_t source_updated_at;  // AIC catalog timestamp; sort key    (4 bytes)
    uint32_t dwell_time_ms;      //                                    (4 bytes)
    uint8_t  pad[7];             // pad to 64 bytes                    (7 bytes)
} aic_channel_entry_t;
_Static_assert(sizeof(aic_channel_entry_t) == 64, "AIC entry must be 64 bytes");
```

### Pick semantics

- `PS_PICK_RECENCY` reads `source_updated_at` and walks newest-first, equivalent to "show me the latest AIC catalog updates".
- `PS_PICK_RANDOM` picks uniformly from the cache window.

### Web UI

`/aic` settings page mirroring `/giphy`:

- Enable toggle (runtime).
- Refresh interval (hours).
- Cache size (entries).
- Optional default search query (creates a `(AIC, search, <query>)` default channel for users who don't want to manually edit playsets).
- "Refresh now" button.
- Status panel: last refresh time, last refresh result, current cache size.

### Config keys (`config_store`)

- `aic.enabled` — bool.
- `aic.refresh_interval_hours` — uint32, default 168.
- `aic.cache_size` — uint32, default 500.
- `aic.default_search` — string, default empty.

### Kconfig

- `P3A_AIC_ENABLE` — compile-time enable, default `y`.
- `CONFIG_AIC_REFRESH_INTERVAL_HOURS` — default 168.
- `CONFIG_AIC_CACHE_SIZE` — default 500.

## 4. APOD channel module

### Channel spec

Single flavor (no search variant — APOD is strictly date-indexed):

| `name`    | `identifier` | Behavior                                |
|-----------|--------------|-----------------------------------------|
| `"daily"` | `""`         | Rolling history of APOD images.         |

### Refresh task (`apod_refresh.c`)

- **Triggers:** daily timer (24h ± random jitter to avoid global-key thundering-herd) and manual refresh.
- **Endpoint:** `GET https://api.nasa.gov/planetary/apod?api_key=<KEY>&date=<YYYY-MM-DD>`
- **First-run backfill:** walk back day-by-day until either `CONFIG_APOD_HISTORY_DAYS` reached or a fetch fails.
- **Steady-state:** fetch today's entry; if it exists in cache already, skip; otherwise download metadata, then queue image download.
- **Video days:** if `media_type != "image"`, write a skip-marker entry (`kind=1`) so that day is not retried indefinitely. The scheduler skips skip-markers on pick.
- **History trim:** entries older than `apod.history_days` are trimmed from the cache. Their image files become eligible for `storage_eviction` age-based reclamation on the next sweep.

### On-demand image fetch (`apod_download.c`)

- Triggered when the scheduler picks an APOD entry whose vault file is missing.
- Storage key: `apod:<YYYY-MM-DD>` → SHA256.
- URL precedence: `hdurl` if present and ends in a supported image extension; else `url`.
- Magic-byte sniff on first 16 bytes; if not in `{JPEG, PNG, GIF, WebP}`, treat as 404 and mark entry dead.
- Cap download at 16 MiB (matches existing `show_url` cap).
- Atomic write.

### Entry layout (`apod_channel_entry_t`, ~64 bytes)

```c
typedef struct __attribute__((packed)) {
    uint32_t date_epoch;     // 00:00 UTC of YYYY-MM-DD; sort key      (4 bytes)
    uint8_t  extension;      // 0=jpg, 2=png                           (1 byte)
    uint8_t  kind;           // 0=image, 1=video-skip, 0xFF=dead       (1 byte)
    uint8_t  reserved[2];    //                                        (2 bytes)
    uint32_t created_at;     // when we cached it (Unix)               (4 bytes)
    uint32_t dwell_time_ms;  //                                        (4 bytes)
    char     title[48];      // truncated APOD title, for UI display   (48 bytes)
} apod_channel_entry_t;
_Static_assert(sizeof(apod_channel_entry_t) == 64, "APOD entry must be 64 bytes");
```

### Pick semantics

- `PS_PICK_RECENCY` walks `date_epoch` newest-first → today's image, then yesterday's, etc.
- `PS_PICK_RANDOM` picks uniformly from the history window.
- Skip-marker (`kind=1`) and dead (`kind=0xFF`) entries are skipped by both modes.

### Copyright handling

APOD curates third-party images alongside NASA's own; some carry "© <photographer>" copyright lines. Per design decision, all APOD images are displayed regardless of copyright field. No on-screen credit overlay. Personal display falls within the spirit of APOD's publication permission.

### Web UI

`/apod` settings page:

- Enable toggle (runtime).
- API key field with link to `api.nasa.gov` and a one-line note on rate limits (DEMO_KEY = 30 req/hr globally; personal key = 1000 req/hr).
- History days slider — default 90, range 8–180.
- Refresh interval (typically left at 24h).
- Manual "Fetch now" button.
- Status panel: last refresh time, last result, key validity (HTTP 403 surfaces a "key invalid" banner).

### Config keys (`config_store`)

- `apod.enabled` — bool.
- `apod.api_key` — string. Default falls back to `CONFIG_APOD_API_KEY_DEFAULT` (which is `DEMO_KEY` in the shipped config).
- `apod.history_days` — uint32, default 90, clamped 8–180.
- `apod.refresh_interval_hours` — uint32, default 24.

### Kconfig

- `P3A_APOD_ENABLE` — compile-time enable, default `y`.
- `CONFIG_APOD_API_KEY_DEFAULT` — string, default `"DEMO_KEY"`.
- `CONFIG_APOD_HISTORY_DAYS_DEFAULT` — default 90.

## 5. Shared scaffolding (decision: none new)

The temptation to extract a "remote-source-channel" base library across `giphy/`, `aic/`, and `apod/` is explicitly rejected for this spec. Three samples is too few to abstract well, and a wrong abstraction is more expensive to undo than the duplication it replaces. After a fourth source ships and patterns stabilize, a separate PR may extract helpers.

Both new components reuse only existing project-wide primitives:

- `sdio_bus_acquire/release` — required for any SD-touching work.
- `playlist_manager`'s SHA256-shard helpers — vault path construction.
- `event_bus` — emit `CONTENT_NEW_AVAILABLE` on refresh completion (analogous to existing channels).
- `config_store` — typed get/set for the new keys above.
- The existing JSON parser (whichever Giphy uses today) — copy the include and use locally.

Concretely, `aic/` and `apod/` each duplicate ~150–250 lines of boilerplate from `giphy/`'s patterns (HTTP fetch, atomic write, refresh-task scaffolding, cache `.bin` read/write). Accepted as a deliberate cost.

## 6. Failure modes and error handling

| Failure                                       | Behavior                                                                                                                          |
|-----------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------|
| Network down during refresh                   | Silent fail with structured log; exponential backoff (60s → 300s → 1800s, capped); cached entries continue serving picks.        |
| HTTP 429 rate limit                           | Honor `Retry-After` (default 60s); after 3+ consecutive 429s, emit a `wifi_health` event for visibility.                          |
| API contract change (JSON parse fails)        | Log + skip; cache file untouched; surfaced in web UI as "last refresh failed" with timestamp.                                     |
| Cache `.bin` magic mismatch or short read     | Treat as empty cache, log warning, refresh from scratch on next cycle.                                                            |
| SD card full                                  | `storage_eviction` runs age-based sweep across all vaults including `aic/` and `apod/`. Refresh defers if free space < threshold.|
| Image URL 404 (object withdrawn)              | Mark entry kind=0xFF (dead); scheduler skips dead entries; periodic refresh evicts.                                               |
| AIC IIIF returns sporadic 403 for an image    | Off-device validation observed ~1% of `default.jpg` fetches return 403 even though `image_id` is non-null. Skip the entry on this fetch (do not mark dead permanently — try again on the next pick); log to wifi_health if rate exceeds threshold. |
| Image download returns HTML error page        | Magic-byte sniff on first 16 bytes; reject if not in `{JPEG, PNG, GIF, WebP}`; treat as 404.                                      |
| APOD API key invalid (HTTP 403)               | Log + show "key invalid" banner in `/apod`; refresh task pauses until config changes.                                             |
| AIC `image_id` is null on object              | Skip during refresh; never enters the cache.                                                                                       |
| APOD `media_type=video` for the day           | Write skip-marker entry; do not retry that date.                                                                                  |

All failures degrade gracefully. No crashing, no popups. User notices via web-UI status panels.

## 7. Testing

### Unit tests (host-target)

- JSON parse: representative AIC search response → expected entry array; representative APOD daily response (image day, video day, missing-key error, missing-`hdurl` day).
- Cache `.bin` write+read roundtrip for both formats.
- Magic-byte format sniff: positive cases (JPEG/PNG/GIF/WebP) and negative cases (HTML, partial bytes).

Fixtures live under `test/fixtures/aic/` and `test/fixtures/apod/`.

### Integration tests (on-device)

- Playset including AIC and APOD channels: refresh fires, entries land in cache, images download to the correct vault paths, scheduler picks them in both `RECENCY` and `RANDOM` modes.
- Eviction reclaims AIC and APOD vault files under simulated low-storage conditions.
- 24-hour soak with both channels active to catch refresh-timer drift, image-cache leaks, and SD churn.

### Manual smoke

- `/aic` and `/apod` settings pages: enable, configure, manual-refresh, key entry (APOD).
- Status-panel correctness after intentional failures (network down, bad key).

## 8. Rollout

- Version bump in root `CMakeLists.txt`.
- Both channels **disabled by default at runtime**. Compile-time `P3A_AIC_ENABLE` and `P3A_APOD_ENABLE` default to `y` (components built in); runtime `aic.enabled` / `apod.enabled` default to `false`. Existing devices upgrading see no behavior change until the user opts in via web UI or playset edit.
- No NVS migration needed: enum values are appended; new config keys come up unset and use defaults.
- Documentation: append a "Channel sources" section to `docs/HOW-TO-USE.md` covering how to add an AIC search channel and how to enable APOD with a free key.

### Implementation order

Two sequential PRs:

1. **PR 1 — AIC.** The more involved of the two. Exercises the channel-cache pattern with a search-flavored channel, IIIF integration, and a multi-channel-spec model.
2. **PR 2 — APOD.** Smaller. Validates that the patterns established by PR 1 are actually reusable. Single-channel singleton, daily-cadence refresh, copyright passthrough.

PR 1 must merge and bake on a few devices before PR 2 starts, so any scaffolding adjustments surface before they have to be retrofitted in two places.

## 9. Open questions

None. All architectural and product decisions captured during the brainstorm are in this spec.

## 9b. Off-device validation (2026-05-05)

Both pipelines were validated end-to-end with Python prototypes under `scripts/aic_test/` and `scripts/apod_test/` before C implementation. AIC: 4 runs × 25 images (2× public_domain, 2× search "impressionism"). APOD: 2 runs × 25-day backfill (44 image days + 6 video skip-markers across the two runs). Findings folded into §3 and §6 above:

- AIC `sort` param requires POST + ES JSON body (GET form rejected with 400).
- AIC offset capped at ~1000; random page must be clamped to `[1, 10]` with `limit=100`.
- AIC IIIF returns sporadic 403; pipeline must skip-and-retry-later, not mark dead.
- APOD spec verified accurate; magic-byte sniff distinguished a PNG day from JPEG days correctly.

## 10. Future work (out of scope, noted for context)

- Per-channel `pick_mode` in playsets — would let a single playset mix "newest from AIC" with "random from Giphy".
- Pixel-art-strict source revisited — possibly via a p3a-maintained CC0 mirror serving from a static endpoint, sidestepping the lack of upstream APIs.
- Generic "remote-source-channel" helper library, after a fourth source exists.
- Additional broad-image sources as drop-in components: Met Museum, Cleveland Museum, Smithsonian, Wikimedia POTD, Rijksmuseum.
