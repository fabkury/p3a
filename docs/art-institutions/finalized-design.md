# Art Institution Channels — Finalized Design

- **Status:** Final (source of truth for implementation)
- **Last updated:** 2026-05-11
- **Owner:** pub@kury.dev
- **History:** Design evolution and Q&A transcript are preserved in
  `design.md` and `questions.md` alongside this file.

p3a v1 supports artwork from Makapix, Giphy, and the local SD card. This
plan adds a fourth content source: **art institutions** that expose their
collections via the IIIF Image API, starting with the Art Institute of
Chicago (AIC) and the Rijksmuseum. The codebase term is `art_institution`;
the user-facing label is **Museums**.

The user picks a category (e.g. "Department: Modern and Contemporary Art")
through a browse interface in the playset editor. The selection becomes a
first-class channel that the play scheduler treats like any other —
refreshing artwork listings periodically, downloading images to local
storage, and feeding the picker.

## Contents

1. [Scope](#1-scope)
2. [Terminology](#2-terminology)
3. [Architecture](#3-architecture)
4. [Data model](#4-data-model)
   - 4.1 [Channel spec encoding](#41-channel-spec-encoding)
   - 4.2 [Cache entry layout](#42-cache-entry-layout)
   - 4.3 [Vault layout](#43-vault-layout)
   - 4.4 [Vault file lifecycle](#44-vault-file-lifecycle)
5. [Components](#5-components)
6. [REST API](#6-rest-api)
7. [Lifecycle](#7-lifecycle)
8. [NVS settings](#8-nvs-settings)
9. [Per-museum specifications](#9-per-museum-specifications)
10. [Image rendition strategy](#10-image-rendition-strategy)
11. [Error handling](#11-error-handling)
12. [Testing approach](#12-testing-approach)
13. [Future work](#13-future-work)
14. [Implementation milestones](#14-implementation-milestones)

## 1. Scope

### In v1

- Browse and persist channels for AIC and Rijksmuseum.
- Browse phase: per-museum JS adapters, browser-direct queries to museum
  APIs.
- Refresh phase: device-side C component that fetches artwork lists for
  saved channels and stores them in a binary cache, mirroring the Giphy
  refresh model.
- Download artwork JPEGs via IIIF, longest side `≤ 720 px`.
- Single-artwork preview in the browse UI, navigable via Previous / Next
  buttons. Per-artwork preview URLs are resolved on demand: AIC and V&A
  use the inline image id from the listing response; Rijks performs a
  3-hop Linked-Art walk lazily, one artwork at a time.
- Two new global NVS settings: `ai_refresh_sec`, `ai_cache_size`.
- First-class per-museum rate-limit handling shared between browser and
  device (§11.1).

### Deferred

- Keyword search.
- Museums beyond AIC and Rijks.
- Per-channel cache size / refresh override.
- Aggregator sources (Europeana, DPLA).
- Manifest synthesis for image-only IIIF (Princeton-style).
- Cross-channel mark-and-sweep vault GC (existing age-based eviction is
  used instead — see §4.4).
- `info.json`-aware rendition negotiation.
- On-device storage of artist / title / date metadata.
- Standalone `/museum-browse` web page.

## 2. Terminology

| Term | Meaning |
|---|---|
| **Institution** / **Museum** | An external IIIF source (AIC, Rijks, ...). |
| **Axis** | A facet vocabulary the museum exposes (e.g. AIC's `departments`, `subjects`). |
| **Term** | A specific value within an axis (e.g. AIC department `PC-4`). |
| **Channel** | A persisted (museum, axis, term) selection that the play scheduler can play. |
| **Adapter** | The per-museum code that knows how to talk to one museum. Browser side (JS) and device side (C) each have an adapter per museum. |

A museum may expose only one axis (Rijks has just one — its set list); the
adapter declares its `axes` list, and the browse UI hides axis selection
when empty.

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

**Browser** owns: per-museum browse logic, user-facing thumbnail previews,
and the API-quirk handling that `reference/museum-art/ubi-test/` already
validates.

**Device** owns: persistence, periodic refresh, image download, playback,
LAi/Ci tracking, eviction.

This split mirrors the existing Makapix flow — the browser already calls
`makapix.club` directly for `verify-user` / `verify-hashtag`, and the
firmware handles delivery.

## 4. Data model

### 4.1 Channel spec encoding

Institution channels reuse the existing playset binary format
(`playset_store.c`, magic `P3PS`, v11) without any format break:

| Field | Encoding |
|---|---|
| `type` | `PS_CHANNEL_TYPE_INSTITUTION = 7` (next ordinal; append-only) |
| `name` | `"{museum_id}:{axis}"` — e.g. `"artic:departments"`, `"rijks:set"` |
| `identifier` | `"{term_id}"` — e.g. `"PC-4"`, `"26118"` |
| `display_name` | `"{museum_short} · {Term label}"`, built by the editor — e.g. `"AIC · Arts of Greece, Rome, and Byzantium"`. Rijks (axis-less) takes the same shape: `"Rijks · {Set name}"`. The axis is intentionally dropped from the displayed label — it's an organizational facet for the browse modal, not user-meaningful in the rendered channel row. The 65-char slot drives the `museum_short` choice ("AIC", "Rijks") because the full names would not fit alongside a long term label. If the assembled string exceeds 64 chars, truncate the term label with an ellipsis at the tail. |
| `weight` | unchanged |

Future keyword search fits the same shape: `name="artic:search"`,
`identifier="<query>"`.

### 4.2 Cache entry layout

Institutions get a sibling layout `institution_channel_entry_t` that
fits the existing 64-byte cache slot, distinguished by a new
`PS_ENTRY_FORMAT_INSTITUTION` discriminator:

```c
typedef struct __attribute__((packed)) {
    int32_t  post_id;        // offset  0 — salted DJB2 hash of "{museum}:{iiif_key}"
    uint8_t  kind;           // offset  4 — 0 = artwork
    uint8_t  extension;      // offset  5 — 0=webp, 1=gif, 2=png, 3=jpg (matches the
                             //              makapix / giphy entry encoding so the picker can
                             //              use one get_asset_type_from_extension helper).
                             //              AIC uses 3 (jpg). 0xFF = unresolved (Rijks HMO
                             //              awaiting Linked-Art walk), 0xFE = tombstone
                             //              (3 resolution attempts failed).
    uint16_t width;          // offset  6 — pixels at requested rendition (0 = unknown)
    uint32_t created_at;     // offset  8 — Unix timestamp from museum metadata (0 = unknown)
    uint16_t height;         // offset 12
    char     iiif_key[48];   // offset 14 — null-terminated; museum-specific identifier
    uint8_t  reserved[2];    // offset 62
} institution_channel_entry_t;
_Static_assert(sizeof(institution_channel_entry_t) == 64, "");
```

Field order keeps all multi-byte members naturally aligned under
`__attribute__((packed))` — same alignment trick `giphy_channel_entry_t`
uses (`created_at` lives at offset 8 by swapping with `height`).

The 48-byte `iiif_key` covers every Tier-1 museum surveyed:

| Source | Identifier | Length |
|---|---|---|
| AIC | `image_id` (UUID) | 36 chars |
| Rijksmuseum | micrio short id (e.g. `RFwqO`) | 5–12 chars |
| SMK (future) | JP2 path (e.g. `qz20sx771_kks5261.tif.jp2`) | ~25 chars |
| Wellcome (future) | b-number (e.g. `b18035723`) | ~10 chars |

### 4.3 Vault layout

Files land at:

```
/sdcard/p3a/museum/{museum_id}/{d0}/{d1}/{iiif_key}.{ext}
```

The shard prefix uses the shared `sd_path_build_sharded()` hash scheme
(FNV-1a-64 of the sanitized iiif_key, 6-bit decimal dirs) for filesystem
fan-out — same convention as Makapix's vault and Giphy's cache. (This doc
originally specified `SHA256(iiif_key)` with 3 hex levels; the shard scheme
changed globally for v1.0.)

The vault is **per-museum and shared across channels**. A given
artwork — e.g. a Picasso painting that appears in `departments:Modern
Art`, `artwork-types:Painting`, `themes:African American artists`, and
`subjects:portrait` — is stored once. This avoids the 2–5× bandwidth
duplication that per-channel ownership would impose on overlapping AIC
facets.

### 4.4 Vault file lifecycle

Two existing codebase mechanisms handle vault file cleanup. No new GC
code is added.

**Mechanism 1 — Refresh-time intra-channel orphan eviction.**

After every full-refresh walk, the museum adapter calls the same
orphan-eviction pattern Makapix and Giphy already use
(`channel_cache_evict_orphans_makapix`,
`giphy_evict_orphans` in
`components/channel_manager/channel_cache_evict.c` and
`components/giphy/giphy_refresh.c`): entries in `Ci` that the museum no
longer lists are dropped from the channel cache. Their vault files
become candidates for Mechanism 2 if no other channel references the
same `iiif_key`.

**Mechanism 2 — Age-based storage eviction (existing
`components/storage_eviction/`).**

`storage_eviction_check_and_run()` already walks the Makapix vault and
the Giphy cache when SD free space drops below
`CONFIG_STORAGE_EVICTION_TARGET_MIB`, applying multi-pass age-based
eviction (halving the age threshold from
`CONFIG_STORAGE_EVICTION_INITIAL_AGE_DAYS` down to
`CONFIG_STORAGE_EVICTION_MIN_AGE_HOURS` until free space is restored).

Integration:

1. Add `sd_path_get_museum(char *out, size_t len)` to `sd_path`
   (returns `/sdcard/p3a/museum`).
2. Add a thin `evict_museum_root()` wrapper in `storage_eviction.c` that
   opens `/sdcard/p3a/museum/`, iterates the per-museum subdirectories
   (`artic/`, `rijks/`, ...), and delegates each to the existing
   `evict_from_base_dir()`. Call it from `evict_old_files()` after the
   vault and giphy passes.

The museum vault has an extra `{museum_id}` segment at the top compared
to the vault and giphy layouts, so a single `evict_from_base_dir` call
on `/sdcard/p3a/museum/` would not reach the hash-sharded leaves. The
wrapper is small (≈10 lines) and reuses the hash-sharded walker per
museum_id.

**Channel cache file cleanup** is already handled by
`channel_eviction_check_and_run()`, which deletes stale channel `.cache`
/ `.json` / `.settings.json` / `.bin` files whose mtime is older than
`CONFIG_CHANNEL_EVICTION_AGE_DAYS`. Channels in the active playset are
protected. Institution channel cache files use the existing channel
directory; no changes needed.

**Failure-mode coverage:**

| Event | What happens to vault files |
|---|---|
| Channel refreshed; museum dropped artwork X from listing | Mechanism 1 drops X from `Ci`. X's vault file is no longer referenced; Mechanism 2 evicts it once it ages past the threshold or free space drops below target. |
| Playset deleted; no other playset references the channel | The channel's `.cache` file ages out via `channel_eviction_check_and_run()`. Once the cache is gone, its vault files become unreferenced and Mechanism 2 evicts them when needed. |
| `ai_cache_size` FIFO trim drops entries | Same as the refresh case — Mechanism 2 picks them up when storage pressure hits. |
| User deletes nothing; refresh adds new entries indefinitely | `ai_cache_size` cap holds `Ci` bounded; Mechanism 1 keeps `Ci` aligned with what the museum lists. Mechanism 2 trims as needed. |

## 5. Components

### 5.1 New C component: `components/art_institution/`

```
components/art_institution/
  CMakeLists.txt
  Kconfig
  art_institution.c              # public API + dispatch
  art_institution_refresh.c      # per-channel refresh entry point
  art_institution_download.c     # IIIF image fetch (HTTPS)
  art_institution_rate_limit.c   # per-museum cooldown table
  art_institution_internal.h
  museums/
    artic.c                      # AIC adapter
    rijksmuseum.c                # Rijks adapter
    common.c                     # shared IIIF URL helpers
  include/
    art_institution.h            # public header
    art_institution_types.h      # institution_channel_entry_t, museum_id_t enum
  test/
    fixtures/                    # captured JSON responses for manual testing
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

The play scheduler refresh dispatcher (`play_scheduler_refresh.c`) gets
a new case for `PS_CHANNEL_TYPE_INSTITUTION` that parses `name` to
extract the museum id, looks up the museum in the dispatch table, and
calls `refresh_channel()`. Refresh is rate-gated by
`ai_refresh_sec` (see §8) and serialized per-museum (§7.2).

### 5.2 New web UI assets

```
webui/
  museum/
    index.js                    # registry, dispatch, helpers
    artic.js                    # AIC adapter (mirrors ubi-test/js/adapters/artic.js)
    rijksmuseum.js              # Rijks adapter
    rijks-sets.json             # baked OAI-PMH sets (see §9.2)
    browse.js                   # the browse modal flow
    style.css
```

`browse.js` exports a function the playset editor opens as a modal when
the user picks `Channel Type = Museum`. The modal walks: museum →
axis → term list → preview → confirm. On confirm it returns a
`ps_channel_spec`-shaped object that the editor appends to the playset.

The existing `playset-editor.html` adds a new
`<option value="institution">Museum</option>` to the channel-type
select and includes the new module. Vanilla `<script type="module">` is
fine on the ESP32-served HTTP server — the playset editor is only
served to LAN-connected modern browsers, never via the boot-time
captive portal.

The museum flow is too large to keep inline cleanly (~600 lines of
adapters + browse code). It lives in separate `webui/museum/*.js` files
loaded as ES modules; the editor file remains the orchestrator.

## 6. REST API

Playsets continue to flow through the existing `POST /playsets/{name}`
route. Two small endpoints are added for the rate-limit mechanism
(§11.1):

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/museum/rate-limits` | Returns the cooldown table: `{ "artic": { "remaining_sec": N }, "rijks": { "remaining_sec": N } }`. The browse modal polls this before kicking off term-count probes. |
| `POST` | `/api/museum/rate-limits/report-429` | Browser reports a 429 it received directly from a museum API. Body: `{ "museum": "artic", "retry_after_sec": 38 }`. The device merges this into its cooldown table so the next device-side refresh also waits. |

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

`playset_json.c` gets one new case in `playset_parse_channel_type()`
and `playset_channel_type_str()`.

## 7. Lifecycle

### 7.1 Browse → save

1. User opens the playset editor, picks Channel Type = Museum.
2. Browse modal opens. User picks museum. Adapter returns axis list.
3. User picks axis (skipped if `axes` is null). Adapter calls
   `listCollections({axis})`.
4. Browser shows term list with counts. User clicks a term.
5. Browser calls `listArtworks(termId, {offset:0, rows:20})` and renders
   a single-artwork preview with Previous / Next navigation. The preview
   image is rendered at IIIF `!400,400`. Caption shows title, artist,
   and date. Additional pages are fetched lazily on Next when the local
   buffer is exhausted. AIC's `from + size ≤ 1000` public-caller cap
   (see `docs/art-institutions/offset-tests/REPORT.md`) is enforced on
   the browser side so Next disables at the 1000th record.
6. User clicks "Add" beneath the strip — strip is the confirmation
   step. A channel spec is appended to the playset.
7. Editor saves the playset normally via `POST /playsets/{name}`.

### 7.2 Refresh

A newly persisted institution channel has `last_refresh = 0`, which the
dispatcher reads as "past its freshness window" — so it refreshes
immediately on the next dispatcher tick after the channel lands in the
active playset.

1. `play_scheduler_refresh.c` notices a channel with type `INSTITUTION`
   and `refresh_pending = true`, and that the channel is past its
   `ai_refresh_sec` freshness window (always true on first run).
2. Dispatches to `art_institution_refresh_channel(name, identifier,
   cache)`.
3. The component parses `name`, looks up the museum, calls the
   adapter's `refresh_channel()`.
4. Adapter walks the museum's listing API, paginating up to
   `ai_cache_size` entries, building an array of
   `institution_channel_entry_t`.
5. Merges into `channel_cache_t` (same merge path Giphy uses,
   parameterized by entry size — already 64). Trim policy:
   **FIFO-by-insertion-order** — when the merged set exceeds
   `ai_cache_size`, oldest entries by insertion order are dropped.
6. Calls the intra-channel orphan eviction (§4.4 mechanism 1).
7. Schedules a debounced cache save.
8. Updates the `channel_metadata` last-refresh sidecar.

**Per-museum serialization.** When multiple institution channels are
eligible to refresh in the same dispatcher tick, the dispatcher
serializes them per museum (at most one in-flight refresh per museum
at a time). AIC's 60-req/min per-IP cap is the constraint that drives
this. Rijks's listing is light but the same mechanism applies for
uniformity.

### 7.3 Download

The download manager runs continuously while a playset containing an
institution channel is active. It picks the oldest `Ci` entry that
isn't in `LAi` yet and downloads it, **one artwork at a time** (no
parallel downloads). When every entry of every active institution
channel is in `LAi`, the manager idles until the next refresh adds new
entries.

1. The download manager picks the next `Ci` entry not in `LAi`.
2. Looks up the museum dispatch entry, calls
   `build_iiif_url(entry, 720, ...)`.
3. Streams the JPEG via `esp_http_client` → vault path.
4. On success, calls `lai_add_entry(cache, entry->post_id, NULL)`.
5. Loops to step 1 until `Ci ⊆ LAi`.

**Rijks resolution.** Step 2 may need to perform the 3-hop Linked Art
walk first (see §9.2). The unresolved-entry sentinel
(`extension = 0xFF`) routes through that path. Three consecutive walk
failures for the same entry promote it to a permanent tombstone
(`extension = 0xFE`), which the download manager skips forever; the
next refresh may re-add the underlying HMO if it still appears in the
listing, restarting the resolution attempts with a fresh budget.

### 7.4 Play

- The pick path is unchanged. The picker reads from LAi, the renderer
  reads the file from the vault path, the JPEG decoder handles it.
- View tracking: institution `post_id`s do not correspond to a Makapix
  post, so view events are skipped (mirrors the Giphy pattern).

## 8. NVS settings

Two new `config_store` keys, both global:

| Key | Type | Default | Allowed values |
|---|---|---|---|
| `ai_refresh_sec` | uint32 | 172800 (2 days) | 86400 (1d), 172800 (2d), 345600 (4d), 691200 (8d) |
| `ai_cache_size` | uint32 | 1024 | 32, 64, 128, 256, 512, 1024, 2048, 4096 |

NVS keys are short (`ai_*` prefix) to fit the NVS 15-char key limit.

Both surface in `webui/settings.html` under a new "Museums" section.
The settings page groups content-source settings together (Makapix,
Giphy, Museums) rather than alphabetizing. The refresh dispatcher reads
`ai_refresh_sec` to gate refresh eligibility for institution channels;
the merge step trims to `ai_cache_size`. The `CHANNEL_CACHE_HARD_CAP`
of 4096 still applies as the absolute upper bound.

The default `ai_cache_size = 1024` reflects that museum channels are
about breadth-of-collection.

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
  `exhibition_history` as free text), so a saved channel would always
  be empty.
- **Pagination cap:** AIC's Elasticsearch cap is 10,000 records. Our
  cache size ceiling is 4,096, so we never hit it.
- **Rate limit:** 60 req/min per IP. Browser-side concurrency limited
  to ≤6 parallel requests during term-count probing.
- **Listing endpoint:**
  `GET /artworks/search?query[term][{filter_field}]={term_id}`
  `&page=N&limit=100&fields=id,title,image_id,artist_title,date_display`
- **IIIF URL:** `https://www.artic.edu/iiif/2/{image_id}/full/!720,720/0/default.jpg`
- **`iiif_key` value:** the `image_id` UUID
- **`extension`:** always 3 (jpg) — uses the shared makapix/giphy/institution byte encoding

### 9.2 Rijksmuseum

- **id:** `rijks`
- **display:** `Rijksmuseum`
- **API base:** `https://data.rijksmuseum.nl`
- **IIIF base:** Micrio — `https://iiif.micr.io/{micrio_id}`
- **No required headers** beyond `Accept: application/ld+json`.
- **Axes:** `null`. Single-list source; the only "axis" is the curated
  set list.
- **Set list — CORS quirk:** the canonical source is Rijks's OAI-PMH
  endpoint at `https://data.rijksmuseum.nl/oai?verb=ListSets`, which
  does **not** return CORS headers. A browser cannot fetch it
  directly. Therefore the firmware **must serve this list itself** as
  a static asset baked into the LittleFS image
  (`/webui/museum/rijks-sets.json`, ≈193 entries — `{spec, name}`
  pairs). The browser-side Rijks adapter loads it from the device's
  own HTTP server, never from rijksmuseum.nl. The build pipeline runs
  `scripts/build_rijks_sets.py` to regenerate the file; it changes
  rarely (Rijks rarely revises its curated sets), so a manual refresh
  tied to firmware releases is sufficient.
- **Listing endpoint:**
  `https://data.rijksmuseum.nl/search/collection?memberOfSetId=https://id.rijksmuseum.nl/{set_id}&imageAvailable=true`
  (cursor-walk via `pageToken` in `OrderedCollectionPage`).
- **IIIF URL discovery:** 3-hop Linked Art chain
  (HMO → VisualItem → DigitalObject → access_point).
- **Device-side resolution strategy:** the Linked Art walk is heavy. The
  refresh stores HMO IDs as `iiif_key` with `extension = 0xFF` (sentinel
  for "unresolved"). The download path notices the sentinel, performs
  the 3-hop walk, updates the entry to the resolved micrio id and
  `extension = 0`, then proceeds with the download. Subsequent
  refreshes only walk the SET listing to find new artworks; existing
  entries keep their resolved micrio ids. Three consecutive walk
  failures promote the entry to a tombstone (`extension = 0xFE`).
- **`iiif_key` value:** the micrio short id once resolved; the HMO id
  while unresolved (also fits in 48 bytes).

### 9.3 Victoria and Albert Museum

- **id:** `vam`
- **display:** `Victoria and Albert Museum`
- **API base:** `https://api.vam.ac.uk/v2`
- **IIIF base:** `https://framemark.vam.ac.uk/collections`
- **No required headers.**
- **Axes (filterable, in browse order):**
  `collection`, `category`, `venue`
- **Filter param map:** `collection` → `id_collection`,
  `category` → `id_category`, `venue` → `id_venue`.
- **Listing endpoint:**
  `GET /objects/search?page=N&page_size=100&images_exist=1&{filter_param}={term_id}`
- **IIIF URL:** `https://framemark.vam.ac.uk/collections/{image_id}/full/!720,720/0/default.jpg`
- **`iiif_key` value:** the search record's `_primaryImageId` field —
  returned inline by the listing endpoint, so no equivalent of Rijks's
  resolver walk is needed. Refresh stores entries fully resolved with
  `extension = 3` and the IIIF id straight from the listing.
- **`extension`:** always 3 (jpg) — V&A's framemark IIIF serves JPEG only.
- **Venue facet quirk:** the V&A search API returns `count=0` for venue
  terms when combined with `images_exist=1` (the API doesn't compute that
  combination). The browse adapter enumerates venue terms without
  `images_exist=1`, then re-probes each term with the filter to populate
  per-term counts (bounded concurrency, same shape as AIC's term-count
  probe). Actual artwork listings inside a venue facet still apply
  `images_exist=1` so saved channels never include image-less records.
- **Rate limit:** none published; treated like Rijks (default 60 s
  cooldown on a 429 with no `Retry-After` header).

### 9.4 Harvard Art Museums

- **id:** `ham`
- **display:** `Harvard Art Museums`
- **API base:** `https://api.harvardartmuseums.org`
- **IIIF base:** `https://nrs.harvard.edu/` (the NRS host 303-redirects
  IIIF requests to `https://ids.lib.harvard.edu/mps/...`; the download
  path's redirect shim follows it).
- **Required header:** none beyond `Accept: application/json`.
- **Required query:** `apikey=<uuid>` on every API call. The key is
  **user-supplied** (BYOK) — stored in NVS under `ham_api_key`, entered
  via the "Museums" tab in `webui/settings.html`. No key is shipped with
  the firmware. When the saved key is empty, HAM channel refresh is a
  no-op (`ESP_LOGI` + return `ESP_OK`, no `last_refresh` write) and the
  browse modal surfaces "enter your key in Settings" instead of axes.
  Channels saved while the key is configured remain persistent across
  reboots; clearing the key only dormants the refresh path.
- **Axes (filterable, in browse order):**
  `classification`, `century`, `culture`, `period`, `place`, `medium`,
  `technique`, `worktype`, `group`, `gallery`. The browser-side adapter
  ships a display-label map and a skip-list (`color`, `person`); term
  enumeration within each axis is driven entirely by what the HAM API
  returns at runtime — endpoint name == filter-param name uniformly, so
  no per-axis filter mapping is needed. See `docs/art-institutions/
  ham-investigation/REPORT.md` for the design rationale.
- **Filter param map:** identity (`classification` → `classification`,
  etc.).
- **Term-id field map:** the term-resource records use axis-specific id
  field names (`classificationid`, `galleryid`, `periodid`, ...), but a
  generic `id` field is always present too. The adapter reads `id` for
  uniformity.
- **Term ordering:** axes whose vocabulary surfaces a populated
  `objectcount` (classification, century, culture, period, place,
  medium, technique, gallery) are sorted by count descending. For
  `worktype` and `group` the vocabulary has `objectcount=0` on every
  term; the adapter sorts those alphabetically.
- **Label-length filter:** terms whose `name` exceeds 32 chars are
  dropped at enumeration time (the 33-byte playset identifier slot).
  Affects `period` (~22 % of top-100), `gallery` (~27 %), `technique`
  (~2 %); the other axes are unaffected.
- **Image-permission gate:** every `/object` listing call MUST include
  `q=imagepermissionlevel:0`. Without it, ~half of `hasimage=1` records
  come back with `primaryimageurl: null` (permission-restricted) and the
  refresh stores entries with no buildable URL.
- **Listing endpoint:**
  `GET /object?apikey={KEY}&size=100&page=N&hasimage=1&q=imagepermissionlevel:0`
  `&{axis}={term_id}&sort=id&sortorder=asc&fields=id,primaryimageurl`
- **IIIF URL:**
  `https://nrs.harvard.edu/{iiif_key}/full/!720,720/0/default.jpg`.
  Resolves to `ids.lib.harvard.edu` via a single 303 hop; the
  `art_institution_download` redirect shim handles the chain.
- **`iiif_key` value:** the URN portion of `images[0].baseimageurl`
  (== `primaryimageurl`) — the substring after `https://nrs.harvard.edu/`
  (typically `urn-3:HUAM:NNNN_dynmc`, 17-26 chars).
- **`extension`:** always 3 (jpg).
- **Resolve hook:** none. The URN→IDS redirect is part of the download
  path, not a `resolve_entry` walk.
- **Rate limit:** **2 500 req/day per API key**, per-user with BYOK. No
  `Retry-After` headers observed; engage default 60 s cooldown on a 429
  (matches AIC/Rijks).

### 9.5 Smithsonian Open Access

- **id:** `si`
- **display:** `Smithsonian`
- **API base:** `https://api.si.edu/openaccess/api/v1.0`
- **IIIF base:** `https://ids.si.edu/ids/iiif/` (no redirect — the IDS host
  serves IIIF directly).
- **Required headers:** `Accept: application/json` **and** `User-Agent:
  p3a/{version} (pub@kury.dev)`. The UA is mandatory: api.si.edu sits
  behind an F5 BIG-IP ASM WAF that returns HTTP 200 with a "Request
  Rejected" HTML body (not a 4xx) when the UA is empty or default. The
  adapter's `si_user_agent()` mirrors AIC's `aic_user_agent()`.
- **Required query:** `api_key=<key>` on every search call. The key is
  **user-supplied** (BYOK from api.data.gov — one key covers any
  api.data.gov service: Smithsonian, NASA, NOAA, etc.) — stored in NVS
  under `si_api_key`, entered via the "Museums" tab in
  `webui/settings.html`. No key is shipped. `DEMO_KEY` is intentionally
  rate-capped at ~30 req/hour/IP and will throttle the first refresh
  mid-flight, so users must register their own (free, instant signup at
  api.data.gov/signup/). When the saved key is empty, refresh is a no-op
  (`ESP_LOGI` + return `ESP_OK`, no `last_refresh` write) and the browse
  modal surfaces "enter your key in Settings" instead of the unit list.
  Behavior on key clear mirrors HAM.
- **Axes:** one — `unit` (Smithsonian's administrative units). The v1
  wired set in `webui/museum/smithsonian.js` is six art-bearing units:
  CHNDM (Cooper Hewitt), SAAM (American Art), NPG (Portrait Gallery),
  NMAAHC (African American History), HMSG (Hirshhorn), NMAfA (African
  Art). Excluded units (NMAI's broken IIIF, FSG's empty dataset), the
  field-shape audit, and v2 axis ideas (cross-unit classification,
  topic, keyword) live in
  `reference/museum-art/source/smithsonian/DEFERRED.md`.
- **Filter param map:** the axis is folded into the Solr query string,
  not a separate URL param: `q=unit_code:{term_id} AND online_visual_material:true`.
  Phase A's probe D confirmed `usage:CC0` is **not** a Solr-indexed field
  (returns 0 hits as an AND clause), so per-item rights filtering is left
  to a future v2.
- **Term-id:** Smithsonian unit codes (`SAAM`, `CHNDM`, …). The curated
  list lives in the browser adapter; the firmware accepts any
  `unit_code` and trusts the curation.
- **Listing endpoint:**
  `GET /search?api_key={KEY}&q=<encoded-q>&start=N&rows=50`
- **Pagination:** native offset (`start` + `rows`). Phase A's probe E
  (`reference/museum-art/source/smithsonian/output/report.md` §E)
  confirmed deep `start=10000` returns valid results — no AIC-style 10K
  cap. Modulo-wrap on `channel_offset` mirrors HAM.
- **Page size:** 50 (smaller than HAM's 100). Smithsonian records nest a
  deeply-faceted `freetext` + `indexedStructured` +
  `descriptiveNonRepeating` set; per-record averages ~5 KB but verbose
  units (NMAAHC provenance, CHNDM design metadata) can push individual
  records over 10 KB. 50/page comfortably fits the 1 MB response buffer.
- **IIIF URL:**
  `https://ids.si.edu/ids/iiif/{iiif_key}/full/!720,720/0/default.jpg`.
  No redirect.
- **`iiif_key` value:** the IDS id extracted from
  `content.descriptiveNonRepeating.online_media.media[*].idsId`. The
  `media` field is either an object (single media file) or a list of
  objects — the adapter handles both shapes. IDs are typically 18-30
  chars (e.g. `SAAM-1935.13.211_1`, `CHSDM-6C6C1A2D27BB2-000001`).
- **`extension`:** always 3 (jpg).
- **Resolve hook:** none. The idsId is returned inline in the search
  response.
- **Rate limit:** **1 000 req/hour per API key** (api.data.gov default
  for registered keys). 429 carries `Retry-After` in seconds; engaged
  via the standard `art_institution_set_rate_limited("si", ...)` flow.

## 10. Image rendition strategy

The device requests `…/full/!720,720/0/default.jpg`. Universally
supported by every Tier-1 IIIF server surveyed. Output format is JPEG;
museum IIIF servers reliably serve JPEG, less reliably WebP.

## 11. Error handling

| Failure | Surface |
|---|---|
| Wi-Fi offline | Refresh skipped (existing dispatcher behavior). Browse modal shows a "Connect to Wi-Fi to browse museums" hint. |
| Museum API returns 5xx | Refresh logs the error, leaves cache unchanged, retries on the next cycle. |
| Museum API returns 429 | Per-museum cooldown engages (§11.1). Browse UI surfaces "rate-limited, try again in N seconds". |
| TLS handshake failure | Logged. The `esp_crt_bundle` should cover all Tier-1 museum CDNs (`artic.edu`, `iiif.micr.io`, `data.rijksmuseum.nl`, `vam.ac.uk`, `api.harvardartmuseums.org`, `nrs.harvard.edu`, `api.si.edu`, `ids.si.edu`). Verification is a gating step before the first C-side commit for each museum (§12.3). |
| Empty cache after refresh | Channel marked inactive (existing pattern). UI shows "no artworks". |
| Image download 404 | Entry left out of LAi; another entry is picked at playback time. |
| Channel spec parses but museum is unknown (newer playset on older firmware) | Channel skipped at execute time, logged WARN. |
| Rijks Linked Art walk fails for a specific artwork | Entry left unresolved; retried on the next download attempt. After 3 consecutive failures, entry is tombstoned (`extension = 0xFE`) and skipped forever until the next refresh re-adds the underlying HMO with a fresh attempt budget. |

### 11.1 Rate-limit handling (first-class)

Per-museum cooldown infrastructure, modeled on the polish of
`components/giphy/giphy_api.c`
(`giphy_set_rate_limited` / `giphy_is_rate_limited` /
`giphy_rate_limit_remaining_seconds`), generalized to a per-museum
cooldown table. AIC's 60-req/min per-IP cap is tighter than Giphy's
100/hour, so this surface is load-bearing.

**Public API in `art_institution.h`:**

```c
void     art_institution_set_rate_limited(const char *museum_id,
                                          uint32_t cooldown_sec);
bool     art_institution_is_rate_limited(const char *museum_id);
uint32_t art_institution_rate_limit_remaining(const char *museum_id);
```

**Internal state:** a fixed-size table keyed by museum id (small N — 2
today, single digits forever), each entry carrying `cooldown_until_ms`.
Process-wide, RAM-only (rebooting clears it, matching Giphy).

**When cooldown engages:**

| Trigger | Cooldown source |
|---|---|
| HTTP 429 with `Retry-After: N` | Honor `N` seconds. |
| HTTP 429 without header | Default per-museum: AIC = 60 s (one window), Rijks = 60 s. |
| Repeated connection failures (≥3 in 30 s) | 30 s defensive cooldown, prevents thrashing. |

**Where it's checked:**

- `art_institution_refresh.c` skips refresh of any channel whose museum
  is in cooldown, with `ESP_LOGW(TAG, "Skipping '%s': %s rate-limited
  (%us remaining)", …)` matching `giphy_refresh.c`.
- `art_institution_download.c` similarly defers IIIF downloads.
- Per-museum serialization (§7.2) further bounds in-flight requests so
  that the cooldown is reached *before* we issue a flood of 429s.

**Web UI:**

- **Browse modal:** before kicking off term-count probes (AIC's
  expensive step), the modal reads cooldown state from the device
  (`GET /api/museum/rate-limits`). If the picked museum is in
  cooldown, the modal renders a "rate-limited — try again in N
  seconds" message with a countdown.
- **Settings page** "Museums" section reuses Giphy's settings-hint
  pattern: documents AIC's 60-req/min limit and explains the math
  (~41 paginated requests per channel refresh, so 4+ channels firing
  in parallel risks a 429).
- **Browser-side self-limiting:** AIC's term-count probe is capped at
  concurrency 6. 429 responses received by the browser are reported
  to the device via `POST /api/museum/rate-limits/report-429` so
  the device's cooldown state stays accurate even when the bandwidth
  came from the browser, not the device. This sharing matters because
  AIC's limit is per-IP — browser-issued and device-issued requests
  share the budget.
- **Landing-page channel-list badge rule.** The "current playset" view
  on the landing page shows a per-channel status indicator. For
  institution channels, the `"API rate limited"` badge is rendered
  **only when both** conditions hold:

  1. The channel's cache is stale —
     `now ≥ last_refresh + ai_refresh_sec`.
  2. The museum is currently in cooldown —
     `art_institution_rate_limit_remaining(museum) > 0`.

  Fresh channels never show the badge even if the API is throttled
  (they have content to play); non-throttled stale channels never show
  it either (the next dispatcher tick will fix them). Mirrors Giphy's
  landing-page rule.

## 12. Testing approach

The owner runs all testing manually. No CI harness is built.

### 12.1 Browser-side adapters

Lean on the existing `reference/museum-art/ubi-test/tests/*.spec.js`
Playwright suite. Each port of an adapter into `webui/museum/` can be
sanity-checked by pointing ubi-test at the new module, or by manual
smoke test from a desktop browser pointed at the device's served UI.

### 12.2 Device-side refresh

Capture fixture JSON responses from real museum endpoints under
`components/art_institution/test/fixtures/` so the URL builder, JSON
parser, and cache-merge code can be exercised manually with a small
harness.

### 12.3 TLS cert bundle verification (pre-merge gate)

Before the first C-side commit lands, verify `esp_crt_bundle` covers
all required CDNs: `api.artic.edu`, `www.artic.edu`, `iiif.micr.io`,
`data.rijksmuseum.nl`. If any CDN's chain root is missing, document
the explicit `esp_crt_bundle_attach` + custom cert workaround before
the code is committed.

### 12.4 End-to-end manual test (release gate)

Before each release that includes museum changes:

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

Tracked for design awareness so v1 doesn't accidentally close these
doors.

- **Keyword search** across museums. Encoding fits the same channel
  spec (`name="artic:search"`, `identifier="<query>"`).
- **More museums:** Gallica. (Harvard Art Museums shipped — see §9.4.)
- **Aggregator sources:** Europeana, DPLA.
- **Per-channel overrides** for refresh interval / cache size.
- **Manifest synthesis** for image-only IIIF (Princeton).
- **`info.json` size negotiation** for tighter rendition matching.
- **Cross-channel mark-and-sweep vault GC**, if field experience shows
  the existing age-based eviction (§4.4) is insufficient for actual
  user patterns. Measurement-driven, not speculative.
- **Standalone `/museum-browse` page** for casual browsing outside of
  the playset editor.
- **Persistent device-side browse cache:** if browse-time term lists
  become expensive (AIC's facet probes are 30 parallel requests),
  cache them on the device.
- **On-device artist/title/date metadata** (sidecar file) for a "now
  showing" overlay or info screen.
- **Admin "refresh now" action** for institution channels.
- **Gallica (BnF):** SRU/XML adapter. Revisit trigger: a lightweight
  XML parser becomes available in ESP-IDF, or content-diversity value
  justifies the integration cost. Deferred design notes in
  [`docs/deferred/gallica.md`](../deferred/gallica.md).
- **Wellcome long labels:** lifting the 32-char identifier limit so
  Wellcome terms with longer labels become selectable. Revisit
  trigger: enough valuable Wellcome terms get hidden in real usage to
  justify a playset format bump. Deferred design notes in
  [`docs/deferred/wellcome-long-labels.md`](../deferred/wellcome-long-labels.md).

## 14. Implementation milestones

Vertical-slice approach: AIC end-to-end first, then Rijks. **M1 and M2
landed.** Field-observed fixes that emerged during implementation are
captured in §15; the milestone descriptions below are kept as the
original implementation plan for historical reference.

### M1 — AIC end-to-end — LANDED

Smallest shippable surface. After M1 the device can play AIC channels.

1. **C side:**
   - `components/art_institution/` scaffold (CMakeLists, Kconfig,
     public header, dispatch table).
   - `museums/artic.c` adapter: `refresh_channel`, `build_iiif_url`.
   - Wire `PS_CHANNEL_TYPE_INSTITUTION = 7` into `playset_store.c`,
     `playset_json.c`, and `play_scheduler_refresh.c`.
   - New cache entry format `PS_ENTRY_FORMAT_INSTITUTION`.
   - Two new NVS settings (`ai_refresh_sec`, `ai_cache_size = 1024`).
   - Rate-limit infrastructure (§11.1): per-museum cooldown table,
     public API, `GET /api/museum/rate-limits` endpoint,
     browser→device 429 reporting endpoint.
   - Per-museum serialization in the refresh dispatcher (§7.2).
   - Continuous serialized download manager loop (§7.3).
   - Extend `components/storage_eviction/`: add `sd_path_get_museum()`
     and one call to `evict_from_base_dir()` so the existing
     age-based eviction also walks `/sdcard/p3a/museum/` (§4.4).
   - Wire institution refresh into the existing intra-channel orphan
     eviction pattern (§4.4 mechanism 1).
   - TLS cert bundle verified for `api.artic.edu`, `www.artic.edu`.
2. **Web UI:**
   - `webui/museum/index.js`, `webui/museum/artic.js`,
     `webui/museum/browse.js`.
   - Playset-editor `<option value="institution">Museum</option>` +
     modal wiring.
   - Cooldown-aware browse modal (reads `/api/museum/rate-limits`,
     reports its own 429s back to the device).
   - Landing-page channel-list badge logic ("API rate limited" only
     when channel is stale **and** museum is in cooldown — §11.1).
   - Settings page "Museums" section grouped with Makapix/Giphy, with
     the two NVS keys and a settings-hint explaining AIC's 60-req/min
     limit.
3. **Manual gate:** 24-hour soak with an AIC Department + AIC
   Artwork-Type channel; picker rotates, downloads succeed, refresh
   completes.

### M2 — Rijksmuseum end-to-end — LANDED

Reuses the M1 scaffolding.

1. **C side:**
   - `museums/rijksmuseum.c` adapter, including lazy 3-hop Linked-Art
     walk at download time and the sentinel-extension scheme from
     §9.2.
   - TLS cert bundle verified for `iiif.micr.io`,
     `data.rijksmuseum.nl`.
2. **Web UI:**
   - `webui/museum/rijksmuseum.js`.
   - `webui/museum/rijks-sets.json` baked into the LittleFS image,
     regenerated by `scripts/build_rijks_sets.py`.
3. **Manual gate:** add a Rijks set channel to the soak playset; same
   checks.

### M3 — Release polish

- Settings copy / help text in `webui/settings.html`.
- Version bump in root `CMakeLists.txt`.
- `docs/HOW-TO-USE.md` updated with the new channel type.

## 15. Field-observed fixes

Issues that only surfaced once the firmware was running against the
real museum APIs and the on-device decode pipeline. Each is captured
here so a future reader doesn't have to git-archeology the rationale
out of commit messages.

### 15.1 AIC `/artworks/search` returns 403 on deep pages

Empirically AIC returns HTTP 403 on `?page=N` requests past page ~10
for facets with very large result sets (e.g. `artwork_type_id=1`
"Painting"), independent of the documented 10 000-record offset cap.
Treating a 403 (or 401) that lands *after* at least one page merged
as a partial success — skip orphan eviction, still save
`last_refresh` so the dispatcher waits the full `ai_refresh_sec`
window before retrying, return `ESP_OK` so the dispatcher's UI does
not render a hard error. A 403/401 on the very first page is still
fatal. See `museums/artic.c` and the commit that introduced it.

### 15.2 Rijks HMO URLs require manual redirect handling

`https://id.rijksmuseum.nl/{id}` returns HTTP 303 with the actual
Linked-Art document served from the `Location` header (typically
`data.rijksmuseum.nl/…`). The ESP-IDF HTTP client only follows
redirects automatically when you call `esp_http_client_perform()`;
the `open/fetch_headers/read` pattern used elsewhere in this
codebase does not, and on IDF v5.5.2 the `disable_auto_redirect`
flag does not prevent `fetch_headers` from internally consuming the
`Location` header before user code can read it via
`esp_http_client_get_header()`. `museums/rijksmuseum.c` works around
this by attaching an `HTTP_EVENT_ON_HEADER` event handler that
captures `Location` into a per-request scratch struct — the parser
dispatches the event synchronously during header parsing, regardless
of how the client later treats the status code.

### 15.3 ESP-IDF JPEG decoder NULL-deref during cleanup

When `jpeg_new_decoder_engine()` fails partway through (observed
when DMA2D pool acquisition fails under concurrent TLS + JPEG
pressure), its `err:` cleanup calls `jpeg_del_decoder_engine()`,
which calls `jpeg_release_codec_handle(decoder_engine->codec_base)`
with `codec_base` still NULL. The IDF function checks the global
`s_jpeg_platform.jpeg_codec` (non-NULL — earlier decodes set it) but
not the parameter, and dereferences NULL on line 94 of
`jpeg_common.c`. Worked around in
`components/animation_decoder/idf_jpeg_release_null_fix.c` with a
linker `--wrap` shim that returns `ESP_OK` on NULL input. Remove
when IDF fixes the function upstream.

### 15.4 Channel cache loader rejected institution sentinels

`channel_cache.c`'s per-entry validator treated any `extension > 4`
as corrupt and discarded the whole cache file on the next load. For
Rijks channels every entry persists with `extension=0xFF` (or
`0xFE` for tombstones), so the loader was wiping the cache on every
reboot. Validator now accepts both reserved sentinels alongside the
0-4 file-type range. Makapix/Giphy entries never use these values,
so this is a no-op for those channel types.

### 15.5 Cosmetic: `esp-x509-crt-bundle` info spam

Every TLS handshake emitted an info-level "Certificate validated"
line that drowned out actually-useful events. Lifted to
`ESP_LOG_WARN` at `app_main` start (warnings/errors still surface).

### 15.6 Browse preview UX: 8-thumbnail grid → single-artwork preview

The original design (§7.1, M1) used an 8-thumbnail 4×2 grid at 64×64.
Two field-observed issues drove the redesign:

1. Latency. Fetching 8 artworks visibly stalls the preview, especially
   for Rijks — its IIIF resolution requires a 3-hop Linked-Art walk
   per artwork, so populating an 8-tile grid would cost 24 extra HTTP
   requests. The original Rijks implementation worked around this by
   skipping image previews entirely and rendering a textual card list,
   which made Rijks's preview qualitatively different from AIC and V&A.
2. Mobile readability. 64×64 thumbnails in a 4-column grid inside a
   ≤560 px modal are too small to evaluate the artwork.

The replacement shows one artwork at a time at IIIF `!400,400`, with
Previous / Next navigation. Per-artwork preview URLs are resolved
lazily — AIC and V&A use the inline `image_id` from the listing
response (synchronous); Rijks performs the 3-hop walk on demand and
caches the resolved micrio id per adapter instance. The Add button
still commits the channel (museum, axis, term), not the visible
artwork. See
`docs/superpowers/specs/2026-05-12-museum-single-artwork-preview-design.md`.
