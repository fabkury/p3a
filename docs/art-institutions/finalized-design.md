# Art Institution Channels — Finalized Design

- **Status:** Final (source of truth for implementation)
- **Last updated:** 2026-08-25
- **Owner:** pub@kury.dev
- **History:** Design evolution and Q&A transcript are preserved in
  `design.md` and `questions.md` alongside this file.

> **Status update 2026-08-14 — AIC disabled.** `www.artic.edu/iiif/2` (AIC's
> image host) now rejects every non-browser client with a Cloudflare managed
> challenge (HTTP 403 + `Cross-Origin-Resource-Policy: same-origin`, so even
> cross-origin `<img>` loads fail); the metadata API at `api.artic.edu` is
> unaffected. Firmware marks AIC unavailable via `unavailable_reason` in
> `art_institution.c`'s dispatch table (refresh/downloads skipped, cached art
> keeps playing, badge + banner in the web UI, browse gated in
> `webui/museum/artic.js`). Revert by clearing that field and
> `ARTIC_UNAVAILABLE` if AIC unblocks access — tracked upstream at
> art-institute-of-chicago/data-aggregator#151. AIC design content below is
> unchanged and still authoritative for the (currently dormant) adapter.

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
   - 9.1–9.7 AIC, Rijksmuseum, V&A, Harvard, Smithsonian, Cleveland, Minneapolis
   - 9.8 [Statens Museum for Kunst](#98-statens-museum-for-kunst-smk)
   - 9.9 [Wellcome Collection](#99-wellcome-collection)
10. [Image rendition strategy](#10-image-rendition-strategy)
    - 10.1 [Request shape](#101-request-shape)
    - 10.2 [Dimension metadata availability](#102-dimension-metadata-availability-surveyed-2026-08-25)
11. [Error handling](#11-error-handling)
12. [Testing approach](#12-testing-approach)
13. [Future work](#13-future-work)
14. [Implementation milestones](#14-implementation-milestones)
15. [Field-observed fixes](#15-field-observed-fixes)
16. [Aspect-ratio filter](#16-aspect-ratio-filter--designed-not-implemented) — **designed, not implemented**

## 1. Scope

> **Scope as shipped.** The lists below are the *original* v1 plan, kept
> for historical reference. Nine museums ship today — `artic`, `rijks`,
> `vam`, `wellcome`, `smk`, `ham`, `si`, `cma`, `mia` — so several
> "Deferred" entries have since landed: museums beyond AIC and Rijks
> (§9), and two non-IIIF sources (Cleveland and Minneapolis) that the
> original scope did not anticipate at all. The dispatch table in
> `art_institution.c` is the authoritative museum list; `artic` is
> currently gated by `unavailable_reason` (see the status note above).

### In v1 (original plan)

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

### Deferred (as of the original plan)

- Keyword search.
- ~~Museums beyond AIC and Rijks.~~ Landed: seven more, see §9.
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
    uint8_t  resolve_fails;  // offset 62 — Rijks: consecutive failed Linked-Art
                             //              walks; promotes to 0xFE at 3
    uint8_t  download_fails; // offset 63 — all museums: consecutive permanent
                             //              image-download failures (true HTTP 403,
                             //              or 404/empty); promotes to 0xFE at 5.
                             //              Refresh replaces the entry (fresh budget
                             //              each ai_refresh_sec window)
} institution_channel_entry_t;
_Static_assert(sizeof(institution_channel_entry_t) == 64, "");
```

Field order keeps all multi-byte members naturally aligned under
`__attribute__((packed))` — same alignment trick `giphy_channel_entry_t`
uses (`created_at` lives at offset 8 by swapping with `height`).

The 48-byte `iiif_key` covers all nine shipped museums. Examples are real
values observed during the §10.2 survey:

| Source | Identifier | Length |
|---|---|---|
| AIC | `image_id` (UUID) | 36 chars |
| Rijksmuseum | micrio short id, or `{micrio}\|{hmo}` post-resolve (e.g. `RNmjH\|200100969`) | 5–20 chars |
| V&A | `_primaryImageId` (e.g. `2016JL6700`) | ~10 chars |
| Wellcome | IIIF `vid` from `items[].locations[]` (e.g. `V0014156`, or `b28664036_0001.jp2` for b-number scans) | 8–25 chars |
| SMK | JP2 filename (e.g. `qz20sx771_kks5261.tif.jp2`) | ~25 chars |
| Harvard | NRS URN (e.g. `urn-3:HUAM:79762_dynmc`) | 17–26 chars |
| Smithsonian | `idsId` (e.g. `SAAM-1935.13.211_1`) | 18–30 chars |
| Cleveland | accession number (e.g. `1931.205`) | 8–12 chars |
| Minneapolis | numeric object id (e.g. `4418`) | 1–6 chars |

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
2. Call `evict_from_base_dir()` on `/sdcard/p3a/museum` from
   `evict_old_files()` after the vault and giphy passes.

The museum vault has an extra `{museum_id}` segment at the top compared
to the vault and giphy layouts. The eviction walker is layout-unaware
(it recurses into whatever directories exist and deletes by extension
allowlist + age), so the extra segment needs no special handling — it
is just one more directory level. (An earlier revision used a dedicated
`evict_museum_root()` wrapper around a fixed-depth shard walker; the
walker became layout-unaware for v1.0 and the wrapper was removed.)

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
    vam.js                      # V&A adapter
    wellcome.js                 # Wellcome adapter
    smk.js                      # SMK adapter
    ham.js                      # Harvard Art Museums adapter (BYOK)
    smithsonian.js              # Smithsonian adapter (BYOK)
    cma.js                      # Cleveland Museum of Art adapter (non-IIIF)
    cma-terms.json              # baked department/type terms (see §9.6)
    mia.js                      # Minneapolis Institute of Art adapter (non-IIIF, live aggs)
    browse.js                   # the browse modal flow (injects its own CSS)
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
| `ai_refresh_sec` | uint32 | 345600 (4 days) | 86400 (1d), 172800 (2d), 345600 (4d), 691200 (8d) |
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

### 9.6 Cleveland Museum of Art

- **id:** `cma`
- **display:** `Cleveland Museum of Art`
- **API base:** `https://openaccess-api.clevelandart.org/api/artworks/`
- **Image base:** `https://openaccess-cdn.clevelandart.org` — **the first
  non-IIIF museum.** There is no IIIF Image API and no size-parameterized
  delivery; every record carries fixed CDN rendition URLs (`web` ≈
  750–1300 px longest side, plus `print`/`full`, ignored). The `web`
  rendition follows a stable template derivable from the accession
  number, so `build_iiif_url` ignores `longest_side` and emits the
  template. The download path treats URLs as opaque strings, so nothing
  else changes. (This retires the implicit "museums must speak IIIF"
  assumption; `reference/museum-art/docs/museum-candidates.md` originally
  excluded Cleveland on exactly that ground.)
- **Required headers:** `Accept: application/json` plus
  `User-Agent: ai_user_agent()` on search legs (polite identification of
  an anonymous keyless bulk consumer; no WAF requirement observed).
- **Required query:** `cc0=1&has_image=1` on every listing call — scopes
  the catalog to the ~41.5k CC0-licensed works with images (the lowest
  licensing risk of any shipped museum: CC0 covers metadata *and*
  images).
- **Axes (filterable, in browse order):** `department`, `type`. CMA has
  **no facet-enumeration endpoint** (`/api/departments/` 404s), so the
  term vocabularies are baked into `webui/museum/cma-terms.json` at
  release time by `scripts/build_cma_terms.py` (a full-corpus scan, ~42
  requests at `limit=1000`; the rijks-sets.json pattern). Counts are
  baked as of the scan date; the live `info.total` takes over once a
  term is opened.
- **Filter param map:** identity (`department` → `department`,
  `type` → `type`). Values are **exact-match full names** — partial
  matches return 0 results.
- **Term-id / long-name expansion:** the playset identifier is the term
  name itself. Two department names exceed the 32-char identifier slot
  (`Egyptian and Ancient Near Eastern Art`, 37 chars;
  `Modern European Painting and Sculpture`, 38 chars). Their baked
  entries store a 32-char truncated `id` plus a `query` field carrying
  the full name; `museums/cma.c` mirrors the mapping in its static
  `CMA_TERM_EXPANSION` table and expands before building the query URL.
  `build_cma_terms.py` prints the table it baked (and fails hard on a
  truncation collision) so the C mirror stays reviewable.
- **Listing endpoint:**
  `GET /api/artworks/?cc0=1&has_image=1&skip=N&limit=100&{axis}={term}`
  `&fields=accession_number,images` — the `fields=` trim is load-bearing
  for the 256 KB response buffer (full records can reach ~3 KB each).
- **Pagination:** native `skip` + `limit`. Deep offsets verified working
  (`skip=40000` returns valid results — no AIC-style cap), so
  `channel_offset` uses the Smithsonian page-align + modulo-wrap scheme.
- **Image URL:**
  `https://openaccess-cdn.clevelandart.org/{accession}/{accession}_web.jpg`
  (template verified 100/100 against listing `filename` fields). Always
  JPEG; sizes vary (median ~300 KB, ~20 % over 500 KB, max ~1 MB
  observed) — heavier per artwork than the IIIF museums' `!720,720`,
  accepted by design.
- **`iiif_key` value:** the accession number (≤14 chars observed,
  charset `[A-Za-z0-9.-]` — FAT-safe, so vault/pin filenames round-trip
  without a `p3a_pin_dispatcher` un-sanitize branch).
- **`extension`:** always 3 (jpg).
- **Resolve hook:** none.
- **Metadata quirk:** `images.web.width`/`height`/`filesize` are
  **strings, occasionally empty** (~2 % of records) — parsed with
  `strtol` guards, 0 = unknown. Records missing `accession_number` or
  `images.web.url` are skipped at refresh (the rebuilt template would
  404).
- **Single-record lookup:** `GET /api/artworks/{accession}` — used by
  the web UI's `fetchMetadataByIiifKey` for the title view.
- **Rate limit:** none published; default 60 s cooldown on a 429
  (matches Rijks/V&A treatment).

### 9.7 Minneapolis Institute of Art

- **id:** `mia`
- **display:** `Minneapolis Institute of Art`
- **API base:** `https://search.artsmia.org/` — a raw **Elasticsearch
  passthrough**: the whole ES query string travels URL-encoded in the
  path segment, with `?size=N&from=M` pagination. Standard ES envelope
  (`hits.total.{value,relation}`, `hits.hits[]._source`).
- **Image base:** `https://1.api.artsmia.org` — the second non-IIIF
  museum. Images are pre-rendered S3 objects at fixed width buckets
  `/{400|800|full}/{id}.jpg` (other widths return S3 403). The device
  downloads the **800 bucket** (~50 KB, 800 px longest side — the
  lightest delivery of any shipped museum); the browse modal previews
  at 400. `build_iiif_url` ignores `longest_side`.
- **Required headers:** `Accept: application/json` plus
  `User-Agent: ai_user_agent()` (polite identification; no WAF
  requirement observed).
- **Required scope:** every query appends
  `AND rights_type:"Public Domain" AND image:valid` — ~34.5k
  public-domain works with confirmed images; the licensing gate is
  fully server-side.
- **Axes (filterable, in browse order):** `classification`,
  `department`, `country`, `style` — axis name == ES field name. Term
  vocabularies are enumerated **live** (first museum with no baked list
  and no vocabulary endpoints): one cached
  `{scope}?size=0&aggs=all` call returns capitalized aggregation groups
  (`Classification`, `Department`, `Country`, `Style`, …) of up to 200
  `{key, doc_count}` buckets, already scoped to the PD-with-image
  filter.
- **Term-id / identifier gates:** the identifier is the facet value
  itself ("Paintings", "Asian Art"). The browse adapter drops buckets
  whose key exceeds **32 UTF-8 bytes** (byte length, not chars — Mia
  has multi-byte keys), contains `"` or `\` (phrase-query breakers), or
  is not well-formed Unicode (Mia's Style facet contains mojibake with
  unpaired surrogates). The C adapter re-rejects `"`/`\` terms
  (`ESP_ERR_INVALID_ARG`) — dual gate. Slashes in keys
  ("Jewelry/Adornment") are fine: identifiers never touch the
  filesystem (channel_id is a hash; `iiif_key` is the numeric id).
- **Listing query:**
  `{axis}:"{term}" AND rights_type:"Public Domain" AND image:valid`
  → `GET /{encoded}?size=50&from=N`. Page size 50 with a 512 KB
  response buffer — full ES records (~4 KB each) cannot be
  `_source`-trimmed.
- **Pagination / ES window:** `from+size ≤ 10 000`; **past the window
  the endpoint fails inconsistently — a bare `[]`** (not the envelope,
  parsed as an empty final page) **or HTTP 500** (normal fetch-error
  path). `hits.total.value` display-caps at 10 000 with
  relation `"gte"`. `channel_offset` is capped+wrapped VAM-style into
  the window (cap 9950), then first-page modulo-total re-fetch applies
  only when relation is `"eq"`.
- **`iiif_key` value:** the numeric object id (≤7 digits — trivially
  FAT-safe).
- **`extension`:** always 3 (jpg).
- **Resolve hook:** none.
- **Entry width/height:** stored 0 (unknown) — Mia's metadata carries
  original scan dims, not the 800-bucket rendition, and no downstream
  consumer reads institution entry dims.
- **Single-record lookup:** query `id:{id}` on the same search endpoint
  — used by `fetchMetadataByIiifKey`.
- **Rate limit:** none published; default 60 s cooldown on a 429.

### 9.8 Statens Museum for Kunst (SMK)

Numbered after Mia rather than in shipping order so the existing §9.N
cross-references stay valid.

- **id:** `smk`
- **display:** `Statens Museum for Kunst`
- **API base:** `https://api.smk.dk/api/v1`
- **IIIF base:** `https://iip.smk.dk/iiif/jp2/`
- **No required headers** beyond `Accept: application/json`. No API key.
- **Axes:** one — `collection`. The browse modal auto-advances past the
  axis step. SMK's facets endpoint returns the collection pairs in three
  different shapes (list of pairs, dict, list of dicts); the browser
  adapter handles all three, mirroring the Python reference.
- **Filter syntax:** the axis is folded into a single bracketed filter
  expression, not separate URL params:
  `filters=[collection:{term_id}],[has_image:true]`. The whole expression
  is one query value, so it is built unencoded and percent-encoded once.
- **Listing endpoint:**
  `GET /art/search?keys=*&offset=N&rows=50&filters=<encoded>`
- **Page size 50, 512 KB buffer.** SMK returns full records (~4 KB each:
  production, materials, notes, titles) and its `fields` param does *not*
  trim them — passing it returns empty items. So the page cannot be
  reduced request-side. A 50-record page measures ~205 KB for
  metadata-rich collections, hence the modest row count and the large
  buffer.
- **IIIF URL:**
  `https://iip.smk.dk/iiif/jp2/{filename}/full/!720,720/0/default.jpg`
- **`iiif_key` value:** the JP2 filename only, i.e. everything after the
  **last** `/iiif/jp2/` in the record's `image_iiif_id` (defensive against
  the marker appearing twice). Records without `image_iiif_id` are
  skipped; some carry only a UUID thumbnail.
- **`extension`:** always 3 (jpg). SMK's IIPImage backend advertises WebP
  in `info.json` but returns HTTP 400 for `.webp` requests, so stay on
  JPEG.
- **Dimensions:** `image_width` / `image_height` are present in the
  listing response on 148/148 sampled records and match the delivered
  rendition's ratio 6/6. Currently parsed and discarded — see §10.2.
- **Rate limit:** none published; standard per-museum cooldown applies
  (§11.1), 60 s default on a 429 without `Retry-After`.

### 9.9 Wellcome Collection

- **id:** `wellcome`
- **display:** `Wellcome Collection`
- **API base:** `https://api.wellcomecollection.org/catalogue/v2`
- **IIIF base:** `https://iiif.wellcomecollection.org/image/`
- **No required headers.** No API key.
- **Axes (filterable, in browse order):**
  `workType`, `genres`, `subjects`, `contributors`
- **Filter param map:** `workType` → `workType`, `genres` →
  `genres.label`, `subjects` → `subjects.label`, `contributors` →
  `contributors.agent.label`.
- **Term-id quirk:** for `workType` the filter value is the term `id`
  (e.g. `k`); for the other three axes Wellcome exposes no stable short
  id, so the term **label itself** is the filter value. Labels longer
  than 32 chars are hidden by the browse modal to fit the playset
  `identifier[33]` slot — see
  [`docs/deferred/wellcome-long-labels.md`](../deferred/wellcome-long-labels.md).
- **Listing endpoint:**
  `GET /works?page=N&pageSize=100&items.locations.locationType=iiif-image`
  `&include=items&{filter_param}={term_id}`
- **IIIF URL:**
  `https://iiif.wellcomecollection.org/image/{vid}/full/!720,720/0/default.jpg`
- **`iiif_key` value:** the `vid` extracted from `items[].locations[]`
  where `locationType.id == "iiif-image"`, taking the path segment after
  the host. Returned inline, so no resolver walk (`resolve_entry = NULL`).
- **`extension`:** always 3 (jpg).
- **Dimensions:** none on `/works`, at any `include=` level —
  `include=images` yields only `{id, type}`. The `/images` endpoint does
  publish `aspectRatio`, but it is **not** a drop-in replacement: it
  silently ignores `workType` and `genres.label` (returning the full
  unfiltered corpus rather than an error) and expects `source.`-prefixed
  fields instead. See §10.2 before migrating any axis to it.
- **Rate limit:** none published; standard per-museum cooldown (§11.1).

## 10. Image rendition strategy

### 10.1 Request shape

The device requests `…/full/!720,720/0/default.jpg`. Universally
supported by every Tier-1 IIIF server surveyed. Output format is JPEG;
museum IIIF servers reliably serve JPEG, less reliably WebP.

Two museums are not size-parameterized and ignore `longest_side`
entirely: CMA serves a single mid-size `_web.jpg` derivative, and Mia
serves pre-rendered S3 buckets of which the device always takes `/800/`
(longest side 800, not fixed width — see the measurements below).

### 10.2 Dimension metadata availability (surveyed 2026-08-25)

`institution_channel_entry_t` reserves `width` at offset 6 and `height`
at offset 12, but only CMA populates them; the other eight parsers
store 0 ("unknown"). Any feature that needs an artwork's shape *before*
it is decoded — an aspect-ratio filter, layout preselection, rendition
negotiation — depends on closing that gap, so the nine APIs were
surveyed against ground truth rather than against their documentation.

**Method.** For each museum: fetch a live page through the exact query
string the refresh path builds, derive `iiif_key` with the same rule
the parser uses, build the download URL through the same
`build_iiif_url()` contract at `longest_side = 720`, fetch that exact
rendition, and read its true dimensions from the JPEG SOF marker. About
140 images were measured. Reported dimensions were then compared as
*ratios* against the delivered file, since the ratio, not the absolute
size, is what the metadata has to predict correctly.

| Museum | Dims in the response the parser already reads? | Field | Ground-truth check |
|---|---|---|---|
| `smk` | **Yes, unused today** | `image_width` / `image_height` | 6/6 ratios match; present on 148/148 |
| `cma` | **Yes, already stored** | `images.web.width` / `height` (string-typed) | 6/6 pixel-exact; present on 300/300 |
| `mia` | **Yes, discarded by the parser** | `image_width` / `image_height` | 28/30 ratios match; present on 150/150 |
| `artic` | No, but same request | add `thumbnail.width,thumbnail.height` to `fields=` | 10/10; present on 299/299 |
| `ham` | No, but same request | add `images.width,images.height` to `fields=` | 36/36; present on 300/300 |
| `wellcome` | Only on a *different* endpoint | `/images` → `aspectRatio` | 10/10; present on 300/300 |
| `vam` | **No** | `_images` carries only `imageResolution: "high"`; the `museumobject/{id}` detail endpoint has no pixel dims either | `info.json` 8/8 |
| `si` | **No** | media object exposes `idsId` / `thumbnail` / `resources` only | `info.json` 8/8 |
| `rijks` | **No** | Linked-Art `DigitalObject` carries only `access_point` / `digitally_shows`; the HMO's `dimension` array is physical centimetres, not pixels | `info.json` 8/8 |

**`!720,720` never crops.** Across roughly 90 reported-versus-delivered
comparisons, the delivered aspect ratio equalled the master aspect
ratio for every IIIF museum. Master-dimension metadata is therefore a
valid *ratio* source even though it is the wrong absolute size, and no
per-entry `info.json` round trip is needed wherever the listing already
carries master dims.

**Mia is the one metadata/file divergence, and it is not IIIF.** Its
`image_width`/`image_height` describe the original scan, while the
`/800/` bucket was rendered from a differently cropped source in 2 of
30 sampled records (reported 1.21 vs delivered 1.32; reported 1.21 vs
delivered 1.40). Both errors are small in absolute terms and would not
flip a verdict at any plausible threshold, but Mia's dims are an
approximation of the delivered file, not a description of it.

**Subfield selection removes the payload objection for AIC and HAM.**
Both APIs honour dot-notation in `fields=`, and neither truncates or
reorders the array:

| Request | Page size (100 records) |
|---|---|
| HAM, current `fields=id,primaryimageurl` | 11.0 KB |
| HAM, naive `…,images` | 114.2 KB |
| HAM, `…,images.width,images.height` | 37.5 KB (buffer is 256 KB) |
| AIC, current `fields=id,title,image_id,artist_title,date_display` | 8.9 KB |
| AIC, naive `…,thumbnail` | 56.8 KB (the `lqip` base64 blob dominates) |
| AIC, `…,thumbnail.width,thumbnail.height` | 13.0 KB (buffer is 192 KB) |

**HAM's `images[0]` is the correct element.** 15.7% of HAM records
carry more than one image, and siblings differ genuinely (one record
spans 1.63 and 1.92). Ten multi-image records were checked specifically:
`images[0]` matched the image actually delivered for the record's
`primaryimageurl` in 10/10, including a 27-image record.

**Wellcome's `aspectRatio` lives behind a query-surface change.** The
`/images` endpoint returns the field directly and populated it on
300/300 records, but it is not a drop-in for the `/works` endpoint the
refresh path uses: it **silently ignores** `workType` and
`genres.label`, returning the full unfiltered corpus (126 878 results)
instead of erroring, and expects `source.genres.label` /
`source.subjects.label` instead. Migrating axes to it without
per-axis verification would quietly turn every Wellcome channel into
an unfiltered firehose.

**Rijks, V&A and Smithsonian have no listing-level path.** Their only
source is a per-entry `info.json` (~390 bytes, one extra HTTPS request),
which was verified accurate 8/8 for each. Rijks is the mild case: it
already performs a 3-hop Linked-Art resolve per entry (§9.2), so an
`info.json` read folds in as a 4th hop rather than introducing a new
per-entry request pattern.

**Coverage summary.** 3 museums free today (`smk`, `cma`, `mia`), 2 for
a one-string `fields=` change (`artic`, `ham`), 1 behind an endpoint
migration with a verification burden (`wellcome`), 3 requiring a
per-entry request (`vam`, `si`, `rijks`).

One incidental observation: AIC's IIIF host served all 10 sampled
renditions to a desktop client during this survey without a Cloudflare
challenge. This does *not* invalidate the `unavailable_reason` gate in
§9.1 — the block is client-fingerprint based and the device is the
fingerprint that gets refused — but it is worth re-testing on hardware
before assuming AIC is permanently lost.

## 11. Error handling

| Failure | Surface |
|---|---|
| Wi-Fi offline | Refresh skipped (existing dispatcher behavior). Browse modal shows a "Connect to Wi-Fi to browse museums" hint. |
| Museum API returns 5xx | Refresh logs the error, leaves cache unchanged, retries on the next cycle. |
| Museum API returns 429 | Per-museum cooldown engages (§11.1). Browse UI surfaces "rate-limited, try again in N seconds". |
| TLS handshake failure | Logged. The `esp_crt_bundle` should cover all Tier-1 museum CDNs (`artic.edu`, `iiif.micr.io`, `data.rijksmuseum.nl`, `vam.ac.uk`, `api.harvardartmuseums.org`, `nrs.harvard.edu`, `api.si.edu`, `ids.si.edu`, `openaccess-api.clevelandart.org`, `openaccess-cdn.clevelandart.org`, `search.artsmia.org`, `1.api.artsmia.org`). Verification is a gating step before the first C-side commit for each museum (§12.3). |
| Empty cache after refresh | Channel marked inactive (existing pattern). UI shows "no artworks". |
| Image download 404 / empty body | Entry left out of LAi; a persistent `.404` marker skips it on future scans; another entry is picked at playback time. Also counts toward `download_fails`. |
| Image download 403 (permanently dead image — e.g. a museum index listing renditions that don't exist, Mia's `image:valid` gaps) | Entry's persisted `download_fails` increments (true 403 only — 401 is excluded via the raw HTTP status threaded through the download path; transient TLS/timeout/5xx/429 never count). At 5 consecutive failures the entry is tombstoned (`extension = 0xFE`) and skipped by scan/pick. The next refresh replaces the entry, granting a fresh 5-attempt budget per `ai_refresh_sec` window — self-healing if the museum fixes its index. |
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
- **More museums:** Gallica. (Nine have shipped — see §9.)
- **Aggregator sources:** Europeana, DPLA.
- **Per-channel overrides** for refresh interval / cache size.
- **Manifest synthesis** for image-only IIIF (Princeton).
- **`info.json` size negotiation** for tighter rendition matching. See
  §10.2 for which museums would actually need the extra round trip
  (`vam`, `si`, `rijks`) and which already publish usable dimensions in
  their listing responses.
- **Aspect-ratio filter:** hide artworks too elongated to display well
  on the square panel. Design is closed and recorded in §16; no code
  written.
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

## 16. Aspect-ratio filter — DESIGNED, NOT IMPLEMENTED

> **Status: no code written.** This section is a closed design ready for
> implementation, recorded so the decisions and the traps survive. Nothing
> described here exists in the firmware today.

Artworks with an extreme aspect ratio (a 4:1 banner, a 1:6 scroll) are
letterboxed correctly onto the square 720×720 panel and still display
badly — mostly empty screen. The filter lets the user set a maximum
ratio past which a museum artwork is simply not shown.

### 16.1 Why not filter on museum metadata

The obvious design — read `entry->width`/`height` at pick time — fails
on its own terms: the §10.2 survey found that eight of the nine museums
leave those fields at 0, and the one museum whose listing dims are an
approximation rather than a description (Mia) is precisely the kind of
source a filter should not silently trust. Teaching six parsers to fill
the fields would cover six museums and leave three fail-open forever.

The dimensions the renderer actually uses are already computed, exactly
once, in the load path. Filtering there needs no parser, no extra
request, and no trust in any museum's metadata.

### 16.2 Architecture: measure in the loader, memoize in RAM, filter in the picker

1. **Measure.** `load_animation_into_buffer()` checks immediately after
   `loader_service_load()` returns, reading
   `loaded.info.canvas_width`/`canvas_height` — the same pair
   `build_upscale_maps_for_buffer()` consumes, so there is no second
   opinion that can disagree with the first. The check sits *before*
   `init_animation_decoder_for_buffer()`, so a rejected artwork never
   allocates `native_frame_b1` / `b2`.
2. **Skip, don't fail.** On rejection the loader reuses the existing
   blocklist-skip shape (`clear_pending_swap_state()` then
   `event_bus_emit_simple(P3A_EVENT_SWAP_NEXT)`), sharing the existing
   `MAX_BLOCKLIST_SKIPS` (6) budget. See §16.4 for why the failure path
   must not be reused.
3. **Memoize.** The loader reports the measurement back via a new
   `ps_report_institution_dims(post_id, w, h)`, which walks all active
   channels and fills `width`/`height` on every matching institution
   entry — mirroring `animation_loader_evict_from_lai()`, which already
   solves the same-artwork-in-two-channels case. It deliberately does
   **not** set `cache->dirty`: the memo is RAM-only and is recomputed
   after a reboot, so no cache-format change and no carve-out in
   `art_institution_merge_entries()` is needed.
4. **Filter.** `play_scheduler_pick.c` rejects any institution entry
   whose stored dims exceed the threshold. Pure integer comparison,
   zero I/O.

`entry->width`/`height` therefore means "best known dimensions from any
source": CMA's refresh parser fills them (verified pixel-exact in
§10.2), the loader fills them for everyone else.

**Cost.** Each *distinct* elongated artwork costs one full decode per
boot, then is filtered at pick and never loaded again. Artworks that
pass the filter cost nothing extra, ever. CMA costs nothing at all.
`jpeg_decoder_init()` fully decodes (HW `jpeg_decoder_process`, or the
SW fallback), so that one decode is not free — it is simply bounded.

**Accepted defect.** `ps_history_push()` and `last_played_id` are
committed when a swap is *queued* (`prepare_and_request_swap()` returns
`ESP_OK`), not when it displays. A rejected artwork therefore enters
history once per boot without ever appearing, so Previous can land on
an artwork the user never saw. Accepted rather than moving the commit
point, which would touch a core path well beyond this feature.

### 16.3 Behaviour

| Decision | Value |
|---|---|
| Scope | Museum (`PS_CHANNEL_TYPE_INSTITUTION`) channels only |
| Exempt | Pinned (`PS_CHANNEL_TYPE_PINNED`) and single-artwork push (`PS_CHANNEL_TYPE_ARTWORK`, show_url) — both explicit user intent. Manual Next is **not** exempt. |
| Unknown dims | Fail open — `width == 0 \|\| height == 0` shows the artwork |
| Comparison | `ratio > max` rejects; `ratio == max` passes |
| Range | 2.5 – 10.0, step 0.1, default **4.0**, filter **ON** by default |
| Downloads | Not gated. Elongated art still downloads and caches, so raising the threshold takes effect immediately with no refetch. |
| Empty channel | No safety valve. If everything is filtered the channel reports exhausted through the existing giveup path. |
| Browse modal | Deliberately unchanged — the playset editor still previews artworks that will not play. |

Exemptions are free: the loader already receives `channel_type`, and
pinned and single-artwork arrive under their own types.

Rotation is irrelevant: the panel is square, so a 4:1 artwork is
equally poor at any rotation.

**Settings.** `config_store` is a single JSON blob, not raw NVS keys,
so this is two fields alongside `ai_refresh_sec` / `ai_cache_size`:

```json
{ "ai_ar_filter": true, "ai_max_ar_tenths": 40 }
```

Integer tenths (40 = 4.0) keeps the comparison exact integer maths
(`w * 10 > max_tenths * h`) and matches every other setting being an
integer. The web UI renders `value / 10` with one decimal. Both the UI
and the setter clamp to the 2.5–10.0 range. The settings hint states
that the filter applies to museum channels only and never to pinned
artworks or artworks opened directly — neither is guessable from the
control itself.

Changing the threshold needs no cache invalidation: the memo stores
dimensions, not verdicts.

### 16.4 Implementation traps

These are the ways this feature goes wrong quietly. All were found by
reading the load path, not by testing.

- **The rejection needs its own sentinel `esp_err_t`.**
  `ESP_ERR_INVALID_SIZE` is already returned by `loader_service` for
  oversized and truncated files, which are genuine corruption and must
  still reach `animation_loader_try_delete_corrupt_cached_file()`.
  Reusing it makes the two indistinguishable. The project has no custom
  error base yet, so one needs defining.
- **Never route the rejection through the failure path.** Three
  landmines in `animation_player_loader.c`:
  1. `animation_loader_try_delete_corrupt_cached_file()` fires for any
     failing path containing `/museum/`. An elongated artwork would be
     deleted from the vault and evicted from LAi, then re-downloaded on
     the next refresh, and deleted again — a permanent bandwidth loop
     that also reads as corruption in the logs.
  2. `MAX_AUTO_RETRIES = 3` — three elongated picks in a row would
     paint "Playback Error" for what is a deliberate policy decision.
  3. `proc_notif_fail_if_processing()` would report failure on the
     processing indicator.
- `SWAP_FAIL_LOUD` is set only by `execute_playset()` on a
  user-initiated playset switch, not by a manual Next press — so the
  on-screen-error risk is narrower than it first appears, but it is not
  zero.
- Rejection chains are self-terminating: every rejection memoizes, so
  the worst case is that the loader measures a channel once and the
  picker then reports exhausted. The `MAX_BLOCKLIST_SKIPS` bound exists
  to cap the *burst* of decodes, not to prevent a loop.
- Per-swap pick logging stays at INFO (operator-facing), consistent
  with the existing `ps_chsel` / `ps_pick` lines.

### 16.5 Verification

Set the threshold to its 2.5 floor on-device: ordinary landscape works
past 5:2 then trip the filter, so the path is exercised without needing
a curated set of extreme artworks. Confirm that (a) the rejected
artwork is skipped silently with no error overlay, (b) it is not
deleted from the vault, (c) a second encounter with the same artwork
costs no decode, and (d) pinning that artwork makes it play.
