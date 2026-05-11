# Art Institution Channels — Design Plan

- **Status:** Working draft (historical record). **Authoritative spec → [finalized-design.md](./finalized-design.md).**
- **Started:** 2026-05-10
- **Last updated:** 2026-05-11
- **Owner:** pub@kury.dev
- **Source of truth:** this file. All design decisions land here as they are made.

p3a v1 supports artwork from Makapix, Giphy, and the local SD card. This plan
adds a fourth content source: **art institutions** that expose their collection
via the IIIF Image API, starting with the Art Institute of Chicago (AIC) and
the Rijksmuseum. The codebase term is `art_institution`; the user-facing label
is **Museums**.

The user picks a category (e.g. "Department: Modern and Contemporary Art")
through a browse interface in the playset editor. The selection becomes a
first-class channel that the play scheduler treats like any other — refreshing
artwork listings periodically, downloading images to local storage, and feeding
the picker.

## Contents

1. [Scope](#1-scope)
2. [Terminology](#2-terminology)
3. [Architecture](#3-architecture)
4. [Data model](#4-data-model)
   - 4.1 [Channel spec encoding (no breaking change)](#41-channel-spec-encoding-no-breaking-change)
   - 4.2 [Cache entry layout (no breaking change)](#42-cache-entry-layout-no-breaking-change)
   - 4.3 [Vault layout (per-museum, shared across channels)](#43-vault-layout-per-museum-shared-across-channels)
   - 4.4 [Lifecycle of vault files (eviction, not GC)](#44-lifecycle-of-vault-files-eviction-not-gc)
5. [Components](#5-components)
6. [REST API](#6-rest-api)
7. [Lifecycle](#7-lifecycle)
8. [NVS settings](#8-nvs-settings)
9. [Per-museum specifications](#9-per-museum-specifications)
10. [Image rendition strategy](#10-image-rendition-strategy)
11. [Error handling](#11-error-handling)
12. [Testing approach](#12-testing-approach)
13. [Future work](#13-future-work)
14. [Decision log](#14-decision-log)
15. [Implementation milestones](#15-implementation-milestones)
16. [Open questions](#16-open-questions)

## 1. Scope

### In v1

- Browse and persist channels for AIC and Rijksmuseum.
- Browse phase: per-museum JS adapters, browser-direct queries to museum APIs.
- Refresh phase: device-side C component that fetches artwork lists for saved
  channels and stores them in a binary cache, mirroring the Giphy refresh model.
- Download artwork JPEGs via IIIF, longest side `≤ 720 px`.
- 64×64 thumbnail previews of the first 8 artworks per term in the browse UI.
- Two new global NVS settings: `art_institution_refresh_interval_sec`,
  `art_institution_cache_size`.

### Deferred (tracked but not v1)

- Keyword search (museum APIs all support it; UI does not in v1).
- Museums beyond AIC and Rijks. The adapter pattern keeps additions cheap.
- Per-channel cache size / refresh override (currently global).
- Aggregator sources (Europeana, DPLA).
- Manifest synthesis for image-only IIIF (Princeton-style).
- Cross-channel mark-and-sweep vault GC (existing age-based eviction is
  used instead — see §4.4).
- `info.json`-aware rendition negotiation.

## 2. Terminology

| Term | Meaning |
|---|---|
| **Institution** / **Museum** | An external IIIF source (AIC, Rijks, ...). |
| **Axis** | A facet vocabulary the museum exposes (e.g. AIC's `departments`, `subjects`). |
| **Term** | A specific value within an axis (e.g. AIC department `PC-4`). |
| **Channel** | A persisted (museum, axis, term) selection that the play scheduler can play. |
| **Adapter** | The per-museum code that knows how to talk to one museum. Browser side (JS) and device side (C) each have an adapter per museum. |

A museum may expose only one axis (Rijks has just one — its set list); the
adapter declares its `axes` list, and the browse UI hides axis selection when
empty.

## 3. Architecture

```
┌─────────────┐  browse + thumbnail fetch (CORS) ┌──────────────┐
│  Web UI     │ ───────────────────────────────► │ Museum APIs  │
│  (browser)  │ ◄─────────────────────────────── │ (AIC, Rijks) │
└─────┬───────┘                                  └──────┬───────┘
      │                                                 │
      │ POST /playsets/{name}                           │
      │ body: { channels: [{ type:"institution",        │
      │                      name:"artic:departments",  │
      │                      identifier:"PC-4" }] }     │
      ▼                                                 │
┌─────────────────────────────────────┐                 │
│ p3a firmware                        │                 │
│  ┌──────────────┐ ┌───────────────┐ │ refresh + image │
│  │playset_store │ │art_institution│ │ download (HTTPS)│
│  │ (binary v11) │ │   component   │ ├─────────────────┘
│  └──────┬───────┘ └───────┬───────┘ │
│         │                 │         │
│         ▼                 ▼         │
│  ┌──────────────────────────────┐   │
│  │ play_scheduler + cache + LAi │   │
│  └──────────────────────────────┘   │
│                  │                  │
│                  ▼                  │
│         /sdcard/p3a/museum/...      │
└─────────────────────────────────────┘
```

**Browser** owns: per-museum browse logic, user-facing thumbnail previews, and
the API-quirk handling that `reference/museum-art/ubi-test/` already validates.

**Device** owns: persistence, periodic refresh, image download, playback,
LAi/Ci tracking, eviction.

This split mirrors the existing Makapix flow — the browser already calls
`makapix.club` directly for `verify-user` / `verify-hashtag`, and the firmware
handles delivery.

## 4. Data model

### 4.1 Channel spec encoding (no breaking change)

The existing playset binary format (`playset_store.c`, magic `P3PS`,
v11) ships with `name[33]` + `identifier[33]` slots per channel. Institution
channels reuse those slots:

| Field | Encoding |
|---|---|
| `type` | `PS_CHANNEL_TYPE_INSTITUTION = 7` (next ordinal; binary-compatible append) |
| `name` | `"{museum_id}:{axis}"` — e.g. `"artic:departments"`, `"rijks:set"` |
| `identifier` | `"{term_id}"` — e.g. `"PC-4"`, `"26118"` |
| `display_name` | `"{museum_short} · {Axis} · {Term label}"`, built by the editor — e.g. `"AIC · Departments · Arts of Greece, Rome, and Byzantium"`. Rijks (axis-less) collapses to `"Rijks · {Set name}"`. The 65-char slot drives `museum_short` choice ("AIC", "Rijks") because the full names would not fit alongside a long term label. Truncation rule: if the assembled string exceeds 64 chars, truncate the term label with an ellipsis at the tail. |
| `weight` | unchanged |

#### Trade-offs of breaking the playset format

| Pros of breaking now | Cons of breaking now |
|---|---|
| Variable-length fields would let us pack richer specs (search queries, multi-term composites). | Existing playsets get invalidated; users lose work on upgrade. |
| Per-channel filters / order overrides could be added. | Doubles the test surface for channel types we already ship. |
| One migration is cheaper than two. | Fits AIC + Rijks comfortably without a break. |
| | Future search query (≤33 chars) is adequate for typical use; if it isn't, that's a follow-up break with concrete data to justify it. |

**Decision:** Do not refactor the playset binary format for v1. Future search
fits the same `name="artic:search"`, `identifier="<query>"` mold. If a real
need for longer fields surfaces, bump the format then with concrete
requirements.

### 4.2 Cache entry layout (no breaking change)

Existing channel cache entries are 64 bytes (`makapix_channel_entry_t`,
reused by Giphy via `giphy_channel_entry_t`). Institutions get a sibling
layout `institution_channel_entry_t` of the same size, distinguished by a
new `PS_ENTRY_FORMAT_INSTITUTION` discriminator:

```c
typedef struct __attribute__((packed)) {
    int32_t  post_id;        // salted DJB2 hash of "{museum}:{iiif_key}"
    uint8_t  kind;           // 0 = artwork
    uint8_t  extension;      // 0=jpg, 1=webp (for IIIF servers that prefer it),
                             // 0xFF = unresolved (Rijks HMO awaiting Linked-Art walk),
                             // 0xFE = tombstone (3 resolution attempts failed)
    uint16_t width;          // pixels at the requested rendition (0 = unknown)
    uint16_t height;
    uint32_t created_at;     // Unix timestamp from museum metadata (0 = unknown)
    char     iiif_key[48];   // null-terminated; museum-specific identifier
    uint8_t  reserved[4];
} institution_channel_entry_t;
_Static_assert(sizeof(institution_channel_entry_t) == 64, "");
```

Why **48 bytes** for `iiif_key`:

| Source | Identifier | Length |
|---|---|---|
| AIC | `image_id` (UUID) | 36 chars |
| Rijksmuseum | micrio short id (e.g. `RFwqO`) | 5–12 chars |
| SMK (future) | JP2 path (e.g. `qz20sx771_kks5261.tif.jp2`) | ~25 chars |
| Wellcome (future) | b-number (e.g. `b18035723`) | ~10 chars |

48 bytes covers every Tier-1 source surveyed in `reference/museum-art/docs/museum-candidates.md`.

#### Trade-offs of breaking the cache format

| Pros of breaking now | Cons of breaking now |
|---|---|
| Could embed display title per entry (no external lookup). | Existing Makapix/Giphy caches get invalidated; users re-download from MQTT/Giphy. |
| Larger entries would support longer IIIF identifiers. | 48-byte key is enough for every Tier-1 museum. |
| Could carry richer metadata (artist, date) for in-device UI. | Larger entries → fewer entries in the same RAM/SD budget; cuts effective cache size. |
| | The sibling-format mechanism (`PS_ENTRY_FORMAT_*`) is the established pattern for this exact case. |

**Decision:** Keep 64 bytes per entry. Add `PS_ENTRY_FORMAT_INSTITUTION`.

### 4.3 Vault layout (per-museum, shared across channels)

Files land at:

```
/sdcard/p3a/museum/{museum_id}/{sha[0]}/{sha[1]}/{sha[2]}/{iiif_key}.{ext}
```

The shard prefix uses `SHA256(iiif_key)` for filesystem fan-out — same
convention as Makapix's vault and Giphy's cache.

#### Per-channel ownership vs per-museum sharing

The user proposed appending the channel ID to the artwork key so each channel
exclusively owns its files (clean unlink on channel delete).

| Aspect | Per-museum vault (recommended) | Per-channel ownership |
|---|---|---|
| Same artwork in N channels | Stored once. | Stored N times. |
| AIC realistic case | A given Picasso can appear in `departments:Modern Art`, `artwork-types:Painting`, `themes:African American artists`, `subjects:portrait`. 4× duplication if per-channel. | Each channel re-downloads. |
| Rijks realistic case | Objects can belong to multiple curated sets (193 sets, with overlap). Less severe but real. | Re-downloads. |
| Bandwidth on refresh | One download per (museum, artwork). | Up to 4× per artwork in heavy-overlap cases. |
| Channel-delete cleanup | Files become orphans (no current channel references them). | Trivial: `rm` the channel's prefix. |
| Disk freed on channel delete | None (orphans remain until eviction). | Immediate. |
| Implementation cost | Low — same model as Makapix vault. | Low. |

**Decision:** Per-museum vault, shared across channels. Orphan
accumulation is handled by reusing the codebase's two existing,
proven cleanup mechanisms — **no new GC code is needed**. See §4.4.

### 4.4 Lifecycle of vault files (eviction, not GC)

The brittle, costly cross-channel mark-and-sweep scheme prototyped in
an earlier draft was rejected. The codebase already has two cleanup
mechanisms that, combined, cover the actual orphan failure mode at
zero new computational cost:

**Mechanism 1 — Refresh-time intra-channel orphan eviction.**

After every full-refresh walk, the museum adapter calls the same
orphan-eviction pattern Makapix and Giphy already use
(`channel_cache_evict_orphans_makapix`, `giphy_evict_orphans` in
`components/channel_manager/channel_cache_evict.c` and
`components/giphy/giphy_refresh.c`): entries in `Ci` that the museum
no longer lists are dropped from the channel cache. Their vault files
become candidates for Mechanism 2 if no other channel references the
same `iiif_key`.

**Mechanism 2 — Age-based storage eviction (existing
`components/storage_eviction/`).**

`storage_eviction_check_and_run()` already walks the Makapix vault
(`sd_path_get_vault`) and the Giphy cache (`sd_path_get_giphy`) when
SD free space drops below `CONFIG_STORAGE_EVICTION_TARGET_MIB`. It
applies multi-pass age-based eviction, halving the age threshold from
`CONFIG_STORAGE_EVICTION_INITIAL_AGE_DAYS` down to
`CONFIG_STORAGE_EVICTION_MIN_AGE_HOURS` until free space is restored.

For museum channels, the integration is a one-line extension:

1. Add `sd_path_get_museum(char *out, size_t len)` to `sd_path` (returns
   `/sdcard/p3a/museum`).
2. Add one `evict_from_base_dir(path, cutoff, stats)` call inside
   `evict_old_files()` in `storage_eviction.c`, after the existing
   vault and giphy passes.

That's it. The recursive walk is base-directory-agnostic; the
museum subdirectory layout (`{museum_id}/{sha[0]}/{sha[1]}/{sha[2]}/`)
is structurally identical to vault and giphy, so the same
`evict_from_base_dir` works without changes.

**Channel cache file cleanup.** Already handled by
`channel_eviction_check_and_run()`, which deletes channel `.cache` /
`.json` / `.settings.json` / `.bin` files whose mtime is older than
`CONFIG_CHANNEL_EVICTION_AGE_DAYS`. Active-playset channels are
protected. Institution channel cache files use the existing channel
directory; no changes needed.

**Failure mode handling (replaces the rejected mark-and-sweep):**

| Event | What happens to vault files |
|---|---|
| Channel refreshed; museum dropped artwork X from listing | Mechanism 1 drops X from `Ci`. X's vault file is no longer referenced; Mechanism 2 will evict it once it ages past the threshold OR free space drops below target. |
| Playset deleted; no other playset references the channel | The channel's `.cache` file ages out via `channel_eviction_check_and_run()` (active-playset protection no longer applies). Once the cache is gone, its vault files become unreferenced and Mechanism 2 evicts them when needed. |
| `ai_cache_size` FIFO trim drops entries | Same as the refresh case — Mechanism 2 picks them up when storage pressure hits. |
| User deletes nothing; refresh adds new entries indefinitely | `ai_cache_size` cap holds `Ci` bounded; Mechanism 1 keeps `Ci` aligned with what the museum lists. Mechanism 2 trims as needed. |

**Trade-off accepted.** Orphan vault files can persist on the SD card
between the time they become unreferenced and the next storage
eviction pass. This is OK because:

- Storage eviction only runs when there's actual pressure, so unused
  files cost nothing as long as the SD has headroom.
- When pressure hits, age-based eviction picks the right files (the
  oldest, least-recently-used), which is exactly what orphans tend
  to be.
- This trades a small amount of SD slack for vastly less code and
  zero runtime cost on the happy path.

If field experience surfaces a case where the lazy scheme is
insufficient, the cross-channel mark-and-sweep is still a tractable
v1.1 addition — but it should be measurement-driven, not speculative.

## 5. Components

### 5.1 New C component: `components/art_institution/`

```
components/art_institution/
  CMakeLists.txt
  Kconfig
  art_institution.c              # public API + dispatch
  art_institution_refresh.c      # per-channel refresh entry point
  art_institution_download.c     # IIIF image fetch (HTTPS)
  art_institution_internal.h
  museums/
    artic.c                      # AIC adapter
    rijksmuseum.c                # Rijks adapter
    common.c                     # shared IIIF URL helpers
  include/
    art_institution.h            # public header
    art_institution_types.h      # institution_channel_entry_t, museum_id_t enum
```

Per-museum dispatch table (declared in `art_institution.c`):

```c
typedef struct {
    const char *id;          // "artic", "rijks"
    const char *display;     // "Art Institute of Chicago"
    esp_err_t (*refresh_channel)(const char *axis,
                                 const char *term_id,
                                 channel_cache_t *cache);
    esp_err_t (*build_iiif_url)(const institution_channel_entry_t *e,
                                int longest_side,
                                char *out, size_t len);
} art_institution_museum_t;

extern const art_institution_museum_t ART_INSTITUTION_MUSEUMS[];
extern const size_t ART_INSTITUTION_MUSEUM_COUNT;
```

The play scheduler refresh dispatcher (`play_scheduler_refresh.c`) gets a new
case for `PS_CHANNEL_TYPE_INSTITUTION` that parses `name` to extract the
museum id, looks up the museum in the dispatch table, and calls
`refresh_channel()`. Refresh is rate-gated by
`art_institution_refresh_interval_sec` (see §8).

### 5.2 New webui assets

```
webui/
  museum/
    index.js                    # registry, dispatch, helpers
    artic.js                    # AIC adapter (mirrors ubi-test/js/adapters/artic.js)
    rijksmuseum.js              # Rijks adapter
    rijks-sets.json             # baked OAI-PMH sets (mirrors ubi-test/js/data/)
    browse.js                   # the browse modal flow
    style.css
```

`browse.js` exports a function the playset editor opens as a modal when the
user picks `Channel Type = Museum`. The modal walks: museum → axis → term list
→ preview → confirm. On confirm it returns a `ps_channel_spec`-shaped object
that the editor appends to the playset.

The existing `playset-editor.html` is the only file that needs editing — it
adds a new `<option value="institution">Museum</option>` to the channel-type
select and includes the new module. Vanilla `<script type="module">` is fine
on the ESP32-served HTTP server; the `compat.js` shim is for older browsers
visiting the captive portal, not module support.

**Inline vs modular:** the existing `playset-editor.html` puts all logic in
one inline `<script>`. The museum flow is too large to keep inline cleanly
(~600 lines of adapters + browse code). It lives in separate
`webui/museum/*.js` files loaded as ES modules. The editor file remains the
orchestrator.

## 6. REST API

Two small endpoints are added to support the first-class rate-limit
mechanism (§11.1). Playsets continue to flow through the existing
`POST /playsets/{name}` route.

| Method | Path | Purpose |
|---|---|---|
| `GET`  | `/api/v1/museum/rate-limits` | Returns the cooldown table: `{ "artic": { "remaining_sec": N }, "rijks": { "remaining_sec": N } }`. The browse modal polls this before kicking off term-count probes. |
| `POST` | `/api/v1/museum/rate-limits/report-429` | Browser reports a 429 it received directly from a museum API. Body: `{ "museum": "artic", "retry_after_sec": 38 }`. The device merges this into its cooldown table so the next device-side refresh also waits. |

A future `POST /api/v1/museum/refresh-now/{channel_id}` (admin "refresh
now") is out of scope for v1 but explicitly reserved.

An institution channel serializes as:

```json
{
  "type": "institution",
  "name": "artic:departments",
  "identifier": "PC-4",
  "display_name": "AIC · Departments · Arts of Greece, Rome, and Byzantium",
  "weight": 100
}
```

`playset_json.c` gets one new case in `playset_parse_channel_type()` and
`playset_channel_type_str()`.

## 7. Lifecycle

### 7.1 Browse → save

1. User opens the playset editor, picks Channel Type = Museum.
2. Browse modal opens. User picks museum. Adapter returns axis list.
3. User picks axis (skipped if `axes` is null). Adapter calls
   `listCollections({axis})`.
4. Browser shows term list with counts. User clicks a term.
5. Browser calls `listArtworks(termId, {offset:0, rows:8})` and renders
   thumbnails at 64×64 via IIIF.
6. User clicks "Add". A channel spec is appended to the playset.
7. Editor saves the playset normally via `POST /playsets/{name}`.

### 7.2 Refresh

A newly persisted institution channel has `last_refresh = 0`, which the
dispatcher reads as "past its freshness window" — so it refreshes
immediately on the next dispatcher tick after the channel lands in the
active playset. This matches Makapix/Giphy first-refresh semantics: the
user has just made a choice, they expect artwork to start arriving without
waiting up to 8 hours for the periodic timer.

1. `play_scheduler_refresh.c` notices a channel with type `INSTITUTION` and
   `refresh_pending = true`, and that the channel is past its
   `ai_refresh_interval_sec` freshness window (always true on first run).
2. Dispatches to `art_institution_refresh_channel(name, identifier, cache)`.
3. The component parses `name`, looks up the museum, calls the adapter's
   `refresh_channel()`.
4. Adapter walks the museum's listing API, paginating up to
   `ai_cache_size` entries, building an array of
   `institution_channel_entry_t`.
5. Merges into `channel_cache_t` (same merge path Giphy uses,
   parameterized by entry size — already 64). Trim policy:
   **FIFO-by-insertion-order** — when the merged set exceeds
   `ai_cache_size`, oldest entries by insertion order are dropped, and
   the next refresh re-adds them if they're still listed. Matches
   Makapix/Giphy.
6. Schedules a debounced cache save.
7. Updates the `channel_metadata` last-refresh sidecar.

**Per-museum serialization.** When multiple institution channels are
eligible to refresh in the same dispatcher tick, the dispatcher
serializes them **per museum** (at most one in-flight refresh per
museum at a time). AIC's 60-req/min per-IP cap is the main reason: 4
AIC channels firing in parallel could each issue ~41 paginated
requests in seconds. Rijks's listing is light but the same mechanism
applies for uniformity. See §11.1 for the rate-limit infrastructure.

### 7.3 Download

The download manager runs continuously while a playset containing an
institution channel is active. It picks the oldest `Ci` entry that
isn't in `LAi` yet and downloads it, **one artwork at a time** (no
parallel downloads). When every entry of every active institution
channel is in `LAi`, the manager idles until the next refresh adds new
entries. This is the same continuous-fill model used elsewhere in p3a;
serialization keeps the Wi-Fi pipe predictable and the museum APIs
happy.

1. The download manager picks the next `Ci` entry not in `LAi`.
2. Looks up the museum dispatch entry, calls
   `build_iiif_url(entry, 720, ...)`.
3. Streams the JPEG via `esp_http_client` → vault path.
4. On success, calls `lai_add_entry(cache, entry->post_id)`.
5. Loops to step 1 until `Ci ⊆ LAi`.

Special case for Rijks: step 2 may need to perform the 3-hop Linked
Art walk first (see §9.2). The unresolved-entry sentinel
(`extension = 0xFF`) routes through that path. Three consecutive walk
failures for the same entry promote it to a permanent tombstone
(`extension = 0xFE`), which the download manager skips forever; the
next refresh may re-add the underlying HMO if it still appears in the
listing, restarting the resolution attempts with a fresh budget.

### 7.4 Play

- The pick path is unchanged. The picker reads from LAi, the renderer reads
  the file from the vault path, the JPEG decoder handles it.
- View tracking: institution `post_id`s do not correspond to a Makapix post,
  so view events are skipped (mirrors the Giphy pattern).

## 8. NVS settings

Two new `config_store` keys, both global:

| Key | Type | Default | Allowed values |
|---|---|---|---|
| `ai_refresh_sec` | uint32 | 86400 (1 day) | 28800 (8h), 86400 (1d), 172800 (2d), 345600 (4d) |
| `ai_cache_size` | uint32 | 1024 | 32, 64, 128, 256, 512, 1024, 2048, 4096 |

NVS keys are short (`ai_*` prefix) to fit the NVS 15-char key limit.

Both surface in `webui/settings.html` under a new "Museums" section. The
settings page groups content-source settings together (Makapix, Giphy,
Museums) rather than alphabetizing — that matches the user's mental
model. The refresh dispatcher reads `ai_refresh_sec` to gate refresh
eligibility for institution channels; the merge step trims to
`ai_cache_size`. The `CHANNEL_CACHE_HARD_CAP` of 4096 still applies as
the absolute upper bound. Default 1024 reflects that museum channels are
about breadth-of-collection; 256 is too small to show off the catalog.

## 9. Per-museum specifications

### 9.1 Art Institute of Chicago

- **id:** `artic`
- **display:** `Art Institute of Chicago`
- **API base:** `https://api.artic.edu/api/v1`
- **IIIF base:** `https://www.artic.edu/iiif/2`
- **Required header:** `AIC-User-Agent: p3a/{version} (pub@kury.dev)`
- **Axes (filterable, in browse order):**
  `departments`, `classifications`, `subjects`, `themes`, `galleries`,
  `artwork-types`
- **Excluded axis:** `exhibitions` — list-only (artwork side stores
  `exhibition_history` as free text), so a saved channel would always be
  empty.
- **Pagination cap:** AIC's Elasticsearch cap is 10,000 records. Our cache
  size ceiling is 4,096, so we never hit it.
- **Rate limit:** 60 req/min per IP. Browser-side concurrency limited to ≤6
  parallel requests during term-count probing.
- **Listing endpoint:**
  `GET /artworks/search?query[term][{filter_field}]={term_id}`
  `&page=N&limit=100&fields=id,title,image_id,artist_title,date_display`
- **IIIF URL:** `https://www.artic.edu/iiif/2/{image_id}/full/!720,720/0/default.jpg`
- **`iiif_key` value:** the `image_id` UUID
- **`extension`:** always 0 (jpg) in v1

### 9.2 Rijksmuseum

- **id:** `rijks`
- **display:** `Rijksmuseum`
- **API base:** `https://data.rijksmuseum.nl`
- **IIIF base:** Micrio — `https://iiif.micr.io/{micrio_id}`
- **No required headers** beyond `Accept: application/ld+json`.
- **Axes:** `null`. Single-list source; the only "axis" is the curated set
  list.
- **Set list — quirk (verified against ubi-test):** the canonical source is
  Rijks's OAI-PMH endpoint at `https://data.rijksmuseum.nl/oai?verb=ListSets`,
  which **does not return CORS headers**. A browser cannot fetch it directly.
  Therefore the firmware **must serve this list itself** as a static asset
  baked into the LittleFS image (`/webui/museum/rijks-sets.json`, ≈193
  entries — `{spec, name}` pairs). The browser-side Rijks adapter loads it
  from the device's own HTTP server, never from rijksmuseum.nl. This is the
  one piece of museum metadata that the firmware ships, and it is the
  reason webui builds include `webui/museum/rijks-sets.json` even though
  every other museum's metadata is browser-fetched. The build pipeline runs
  `reference/museum-art/ubi-test/tools/build_rijks_sets.py` (or an
  equivalent script ported into the p3a repo) to regenerate the file; it
  changes rarely (Rijks rarely revises its curated sets), so a manual
  refresh tied to firmware releases is sufficient — no in-field update
  mechanism in v1.
- **Listing endpoint:**
  `https://data.rijksmuseum.nl/search/collection?memberOfSetId=https://id.rijksmuseum.nl/{set_id}&imageAvailable=true`
  (cursor-walk via `pageToken` in `OrderedCollectionPage`).
- **IIIF URL discovery:** 3-hop Linked Art chain
  (HMO → VisualItem → DigitalObject → access_point).
- **Device-side resolution strategy:** the Linked Art walk is heavy. The
  refresh stores HMO IDs as `iiif_key` with a sentinel extension byte
  (`0xFF` = "unresolved"). The download path notices the sentinel, performs
  the 3-hop walk, updates the entry to the resolved micrio id, then proceeds
  with the download. Subsequent refreshes only walk the SET listing to find
  new artworks; existing entries keep their resolved micrio ids.
- **`iiif_key` value:** the micrio short id once resolved; the HMO id while
  unresolved (also fits in 48 bytes).

## 10. Image rendition strategy

User requirement (Q8): prefer the smallest size ≥ 720 px on the longest side;
fall back to `!720,720` if that is server-dependent.

IIIF Image API has no built-in "smallest discrete rendition ≥ N" parameter.
The closest mechanism is the `sizes` array in `info.json`, which lists
discrete pre-rendered tile sizes — but it is unevenly populated across
servers, and reading it costs an extra round trip per artwork.

**v1 implementation (confirmed Round 2):**
1. Default request: `…/full/!720,720/0/default.jpg`. Universally supported
   by every Tier-1 server in the survey.
2. The `info.json`-aware negotiator is **deferred to v1.1** with a
   measurement-driven decision: compare bandwidth saved against the extra
   round trip, per museum. Adding it later is non-breaking — the entry
   `width`/`height` fields already exist for future use.

Output format is JPEG. Museum IIIF servers reliably serve JPEG; WebP is
inconsistent.

## 11. Error handling

| Failure | Surface |
|---|---|
| Wi-Fi offline | Refresh skipped (existing dispatcher behavior). Browse modal shows a "Connect to Wi-Fi to browse museums" hint. |
| Museum API returns 5xx | Refresh logs the error, leaves cache unchanged, retries on the next cycle. |
| Museum API returns 429 | Per-museum cooldown (mirror of `giphy_set_rate_limited`). Browse UI surfaces "rate-limited, try again in N seconds". |
| TLS handshake failure | Logged. The `esp_crt_bundle` should cover all Tier-1 museum CDNs (`artic.edu`, `iiif.micr.io`, `data.rijksmuseum.nl`). Verification is a gating step before the first C-side commit (see §12.3); if a CDN isn't covered, the design doc gets updated with the specific cert addition required. |
| Empty cache after refresh | Channel marked inactive (existing pattern). UI shows "no artworks". |
| Image download 404 | Entry left out of LAi; another entry is picked at playback time. |
| Channel spec parses but museum is unknown (newer playset on older firmware) | Channel skipped at execute time, logged WARN. |
| Rijks Linked Art walk fails for a specific artwork | Entry skipped; logged. The artwork stays unresolved and is retried on the next download attempt. After 3 consecutive failures the entry is promoted to a tombstone (`extension = 0xFE`) and skipped forever — until the next refresh re-adds the underlying HMO with a fresh attempt budget. |

### 11.1 Rate-limit handling as a first-class concept

Mirrors the polish of `components/giphy/giphy_api.c` (`giphy_set_rate_limited` / `giphy_is_rate_limited` / `giphy_rate_limit_remaining_seconds`), generalized to a per-museum table. AIC's 60-req/min per-IP cap is tighter than Giphy's 100/hour, so this surface is load-bearing.

**Public API in `art_institution.h`:**

```c
void     art_institution_set_rate_limited(const char *museum_id,
                                          uint32_t cooldown_sec);
bool     art_institution_is_rate_limited(const char *museum_id);
uint32_t art_institution_rate_limit_remaining(const char *museum_id);
```

**Internal state:** a fixed-size table keyed by museum id (small N — 2 today, single digits forever), each entry carrying `cooldown_until_ms`. Process-wide, RAM-only (rebooting clears it, matching Giphy).

**When cooldown engages:**

| Trigger | Cooldown source |
|---|---|
| HTTP 429 with `Retry-After: N` | Honor `N` seconds. |
| HTTP 429 without header | Default per-museum: AIC = 60 s (one window), Rijks = 60 s. |
| Repeated connection failures (≥3 in 30 s) | 30 s defensive cooldown, prevents thrashing. |

**Where it's checked:**

- `art_institution_refresh.c` skips refresh of any channel whose museum is in cooldown, with `ESP_LOGW(TAG, "Skipping '%s': %s rate-limited (%us remaining)", …)` matching `giphy_refresh.c:255`.
- `art_institution_download.c` similarly defers IIIF downloads (the IIIF host has its own rate budget; AIC's `www.artic.edu/iiif/2` is generally permissive but the same gate covers it).
- Per-museum serialization (§7.2) further bounds in-flight requests so that the cooldown is reached *before* we issue a flood of 429s.

**Web UI awareness:**

- Browse modal: before kicking off term-count probes (AIC's expensive step), the modal reads cooldown state from the device (new `GET /api/v1/museum/rate-limits` endpoint, JSON `{ "artic": { "remaining_sec": 0 }, "rijks": { "remaining_sec": 12 } }`). If the picked museum is in cooldown, the modal renders a "rate-limited — try again in N seconds" message with a countdown.
- Settings page "Museums" section reuses Giphy's settings-hint pattern: documents AIC's 60-req/min limit and explains the math (~41 paginated requests per channel refresh, so 4+ channels firing in parallel risks a 429).
- The browser-side adapters self-rate-limit at the source: AIC's term-count probe is capped at concurrency 6 (already present in ubi-test), and 429 responses from the browser side are reported through the same endpoint so the device's cooldown state stays accurate even when the bandwidth came from the browser, not the device.
- **Landing-page channel-list badge rule.** The "current playset" view on the landing page shows a per-channel status indicator. For institution channels, the `"API rate limited"` badge is rendered **only when both** conditions hold:

  1. The channel's cache is stale — `now ≥ last_refresh + ai_refresh_sec`, i.e. the channel actually needs a refresh.
  2. The museum is currently in cooldown — `art_institution_rate_limit_remaining(museum) > 0`.

  If the cache is fresh, the channel has content to play; the rate-limit state isn't user-relevant. If the museum isn't in cooldown, the next dispatcher tick will refresh; no badge needed either. This mirrors how Giphy's landing-page channel list handles the equivalent state.

**Why this is more involved than Giphy's:**

| Difference | Implication |
|---|---|
| AIC limit is per-IP, not per-API-key. | Browser-issued requests and device-issued requests share the budget. Both surfaces must report 429s into the same cooldown state. |
| Tight 60 req/min window. | A naive parallel refresh trips it within seconds. Per-museum serialization (§7.2) is not optional. |
| `Retry-After` is sometimes provided. | Honor it when present; per-museum default fallback otherwise. |
| Multiple museums, each with its own quota. | The table is keyed by museum id, not global. |

## 12. Testing approach

The owner runs all testing manually; no CI harness is built in v1.

### 12.1 Browser-side adapters

Lean on the existing `reference/museum-art/ubi-test/tests/*.spec.js`
Playwright suite. Each port of an adapter into `webui/museum/` can be
sanity-checked by pointing ubi-test at the new module, or by manual smoke
test from a desktop browser pointed at the device's served UI.

### 12.2 Device-side refresh

Capture fixture JSON responses from real museum endpoints (saved under
`components/art_institution/test/fixtures/`) so the URL builder, JSON
parser, and cache-merge code can be exercised manually with a small
harness. No automated suite in v1 — the user will run these as needed.

### 12.3 TLS cert bundle verification (pre-merge gate)

Before the first C-side commit lands, verify `esp_crt_bundle` covers all
required CDNs: `api.artic.edu`, `www.artic.edu`, `iiif.micr.io`,
`data.rijksmuseum.nl`. If any CDN's chain root is missing, document the
explicit `esp_crt_bundle_attach` + custom cert workaround here before the
code is committed — finding this at flash time is a poor experience.

### 12.4 End-to-end manual test (release gate)

Before each release that includes museum changes, run this on-device
checklist:

1. Add an AIC Painting channel (axis: artwork-types, term: Painting).
2. Add an AIC Department channel (e.g. Modern and Contemporary Art).
3. Add a Rijks Set channel (e.g. the largest available set).
4. Confirm immediate first-refresh kicks in within one dispatcher tick.
5. Let the device run for 24 hours and confirm:
   - the picker rotates,
   - JPEG downloads succeed,
   - the periodic refresh completes without errors,
   - rate-limit cooldowns (if hit) are gracefully observed.

## 13. Future work

Tracked here for design awareness so v1 doesn't accidentally close these
doors.

- **Keyword search** across museums. Encoding fits the same channel spec
  (`name="artic:search"`, `identifier="<query>"`). UI is the bigger lift.
- **More museums:** SMK, V&A, Wellcome, Gallica, LoC, Harvard. Each adapter
  is ~300 lines of JS + ~200 lines of C plus a row in the dispatch table.
- **Aggregator sources:** Europeana, DPLA. Treat as institutions with a
  `museum_id` and a "source institution" badge in the UI.
- **Per-channel overrides** for refresh interval / cache size.
- **Manifest synthesis** for image-only IIIF (Princeton).
- **`info.json` size negotiation** for tighter rendition matching.
- **Cross-channel mark-and-sweep vault GC** if field experience shows
  the existing age-based eviction (§4.4) is insufficient for actual
  user patterns. Should be measurement-driven, not speculative.
- **Persistent device-side browse cache:** if browse-time term lists become
  expensive (AIC's facet probes are 30 parallel requests), cache them on the
  device so refreshing them is an admin action, not a UX cost on every open.

## 14. Decision log

| Date | Decision | Rationale |
|---|---|---|
| 2026-05-10 | Naming: `art_institution` (code), "Museums" (UI). | User-confirmed. |
| 2026-05-10 | Channel type ordinal `7` = `INSTITUTION`. | Append-only constraint of the existing playset binary format. |
| 2026-05-10 | Channel spec encoded as `name="{museum}:{axis}"`, `identifier="{term}"`. | Fits AIC + Rijks + future tier-1 museums in 33+33 chars. |
| 2026-05-10 | Cache entry: 64-byte `institution_channel_entry_t`. | Keeps existing cache file shapes; sibling-format pattern. |
| 2026-05-10 | Vault path: `/sdcard/p3a/museum/{museum}/...`, shared across channels. | Avoids 2-5× bandwidth duplication on overlapping AIC facets. |
| 2026-05-10 | Browse logic in browser, refresh + download in C. | Mirrors existing makapix.club + giphy split. |
| 2026-05-10 | NVS settings global, not per-channel. | Simpler v1; per-channel can be added later without breakage. |
| 2026-05-10 | Image rendition: `!720,720` JPEG; `info.json`-aware sizing deferred. | All Tier-1 servers support it; defer optimization. |
| 2026-05-10 | No refactor of playset binary format. | Adequate for v1 + likely v2 (search). Cost > benefit. |
| 2026-05-10 | No refactor of cache binary format. | Adequate via sibling layout. Cost > benefit. |
| 2026-05-10 | Rijks: lazy IIIF resolution at download time, not refresh time. | Refresh stays cheap; per-artwork Linked-Art walk happens once and is then cached in `iiif_key`. |
| 2026-05-11 | Three "no break" decisions (4.1, 4.2, 4.3) ratified. | Adequate for v1 + foreseeable v2. Round 2 Q1. |
| 2026-05-11 | Display-name format: `"{museum_short} · {Axis} · {Term}"`, axis-less for Rijks. | Reads consistently in the playset list; 65-char slot forces short museum tag. Round 2 Q2. |
| 2026-05-11 | First-refresh: immediate on first dispatcher tick after channel lands in an active playset. | Matches Makapix/Giphy UX expectation. Round 2 Q3. |
| 2026-05-11 | No standalone `/museum-browse` page in v1. | Surface-area minimal; cheap to add later. Round 2 Q4. |
| 2026-05-11 | No device-side pre-warm of axis lists. Browser fetches museum APIs directly. | Browse is browser-direct; localStorage caching is a cheaper future optimization. Round 2 Q5. |
| 2026-05-11 | Rijks set list (`rijks-sets.json`) shipped as a baked LittleFS asset. | OAI-PMH ListSets endpoint blocks CORS — only path that works. Round 2 Q5. |
| 2026-05-11 | TLS cert bundle verification is a pre-merge gate, not a post-fact check. | Surfacing a missing cert at flash time is wasteful. Round 2 Q6. |
| 2026-05-11 | `info.json`-aware rendition confirmed deferred to v1.1. | Non-breaking later addition; entry layout already reserves `width`/`height`. Round 2 Q7. |
| 2026-05-11 | No CI harness in v1; manual testing by the owner. | Mirrors current state of the rest of the project. Round 2 Q8. |
| 2026-05-11 | Implementation order: vertical slice — AIC end-to-end first, then Rijks. | Surfaces architecture mistakes earliest; Rijks reuses scaffolding. Round 2 Q9. |
| 2026-05-11 | Cache eviction policy: FIFO-by-insertion-order on `ai_cache_size` trim. | Matches existing Makapix/Giphy merge path. Round 3 Q1. |
| 2026-05-11 | Default `ai_cache_size` bumped from 256 → 1024. | Museum channels are about breadth-of-collection. Round 3 Q2. |
| 2026-05-11 | Download model: continuous serialized fill while playset active. One artwork at a time, oldest-not-in-LAi first. | User's explicit model; keeps Wi-Fi pipe predictable. Round 3 Q3. |
| 2026-05-11 | Rijks unresolvable: tombstone (`extension = 0xFE`) after N=3 walk failures. Re-attempted only when next refresh re-adds the HMO. | Avoids wasted bandwidth on removed artworks while staying self-healing. Round 3 Q4. |
| 2026-05-11 | Refresh dispatcher serializes per-museum (at most one in-flight refresh per museum). | AIC's 60-req/min cap is the constraint. Round 3 Q5. |
| 2026-05-11 | Rate-limiting is a first-class, generalized per-museum mechanism modeled on Giphy's. Browser and device share AIC's per-IP budget via a `/api/v1/museum/rate-limits` endpoint. | AIC's tight per-IP window + browser-direct browse make the polish bar high. Round 3 Q5. |
| 2026-05-11 | No on-device artist/title/date metadata storage in v1; the sidecar shape is not designed yet either. | Avoids speculative carrying cost; deferred entirely. Round 3 Q6. |
| 2026-05-11 | Thumbnail strip = confirmation step (Add button below it). ≤8 thumbnails, no captions. | UX confirmed. Round 3 Q7. |
| 2026-05-11 | Settings UI grouped by content source (Makapix, Giphy, Museums together). | Matches user's mental model. Round 3 Q8. |
| 2026-05-11 | (Superseded) Cross-channel mark-and-sweep vault GC — rejected as brittle and computationally costly. See next row. | Round 4 Q2, reversed Round 5. |
| 2026-05-11 | Vault file lifecycle handled by reusing existing `storage_eviction` (age-based, on SD pressure) plus refresh-time intra-channel orphan eviction. No new GC code. | Existing mechanisms already cover the failure mode; trading a small amount of SD slack for vastly less code is the right deal. Round 5. |
| 2026-05-11 | Landing-page "API rate limited" badge shown only when channel cache is stale AND museum is in cooldown. | Mirrors Giphy's landing-page rule; avoids alarming the user when there's nothing to do. Round 4 Q1c. |
| 2026-05-11 | Rate-limit state is browser↔device-shared (per Round 3 Q5) and RAM-only. | AIC limit is per-IP, so shared; short windows make RAM-only adequate. Round 4 Q1a/Q1b. |
| 2026-05-11 | Design docs commit separately before any code. M1 ships as one PR. | Clean baseline to reference; vertical slice is too coupled to split. Round 4 Q3. |

## 15. Implementation milestones

Driven by the Round 2 decision to vertical-slice AIC first.

### M1 — AIC end-to-end (target merge)

Smallest shippable surface. After M1 the device can play AIC channels.

1. C side:
   - `components/art_institution/` scaffold (CMakeLists, Kconfig, public header, dispatch table).
   - `museums/artic.c` adapter: `refresh_channel`, `build_iiif_url`.
   - Wire `PS_CHANNEL_TYPE_INSTITUTION = 7` into `playset_store.c`,
     `playset_json.c`, and `play_scheduler_refresh.c`.
   - New cache entry format `PS_ENTRY_FORMAT_INSTITUTION`.
   - Two new NVS settings (`ai_refresh_sec`, `ai_cache_size = 1024`).
   - Rate-limit infrastructure (§11.1): per-museum cooldown table,
     public API, `GET /api/v1/museum/rate-limits` endpoint, browser→device
     429 reporting endpoint.
   - Per-museum serialization in the refresh dispatcher (§7.2).
   - Continuous serialized download manager loop (§7.3).
   - Extend `components/storage_eviction/`: add `sd_path_get_museum()`
     and one call to `evict_from_base_dir()` so the existing age-based
     eviction also walks `/sdcard/p3a/museum/` (§4.4).
   - Wire institution refresh into the existing intra-channel orphan
     eviction pattern (§4.4 mechanism 1).
   - TLS cert bundle verified for `api.artic.edu`, `www.artic.edu`.
2. Web UI:
   - `webui/museum/index.js`, `webui/museum/artic.js`, `webui/museum/browse.js`.
   - Playset-editor `<option value="institution">Museum</option>` + modal wiring.
   - Cooldown-aware browse modal (reads `/api/v1/museum/rate-limits`,
     reports its own 429s back to the device).
   - Landing-page channel-list badge logic ("API rate limited" only
     when channel is stale **and** museum is in cooldown — §11.1).
   - Settings page "Museums" section grouped with Makapix/Giphy, with the
     two NVS keys and a settings-hint explaining AIC's 60-req/min limit.
3. Manual gate: 24-hour soak with an AIC Department + AIC Artwork-Type
   channel; picker rotates, downloads succeed, refresh completes.

### M2 — Rijksmuseum end-to-end (target merge)

Reuses the M1 scaffolding. Adds:

1. C side:
   - `museums/rijksmuseum.c` adapter (incl. lazy 3-hop Linked-Art walk at
     download time, sentinel-extension scheme from §9.2).
   - TLS cert bundle verified for `iiif.micr.io`, `data.rijksmuseum.nl`.
2. Web UI:
   - `webui/museum/rijksmuseum.js`.
   - `webui/museum/rijks-sets.json` baked into the LittleFS image, plus
     the build hook (or porter script) that regenerates it from
     `reference/museum-art/ubi-test/tools/build_rijks_sets.py`.
3. Manual gate: add a Rijks set channel to the soak playset; same checks.

### M3 — Release polish

- Settings copy / help text in `webui/settings.html`.
- Decision-log archive entry, version bump in root `CMakeLists.txt`.
- `docs/HOW-TO-USE.md` updated with the new channel type.

## 16. Open questions

Live working set tracked in [questions.md](./questions.md). Rounds 1 and 2
are closed; Round 3 is open and gating the start of M1.
