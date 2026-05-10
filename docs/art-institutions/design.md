# Art Institution Channels — Design Plan

- **Status:** Draft v0.1
- **Started:** 2026-05-10
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
15. [Open questions](#15-open-questions)

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
- Per-museum vault garbage collection on channel delete.
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
┌─────────────┐  browse + thumbnail fetch (CORS)  ┌──────────────┐
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
│  ┌──────────────┐  ┌──────────────┐ │ refresh + image │
│  │playset_store │  │art_institution│ │ download (HTTPS)│
│  │ (binary v11) │  │   component   │ ├─────────────────┘
│  └──────┬───────┘  └──────┬───────┘ │
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
| `display_name` | `"{museum} · {axis} · {term label}"`, built by the editor |
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
    uint8_t  extension;      // 0=jpg, 1=webp (for IIIF servers that prefer it)
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

**Decision:** Per-museum vault, shared across channels. Channel deletion does
not unlink files — orphans remain until eviction or a future mark-and-sweep
pass. This matches the existing Makapix vault contract, and it respects the
heaviest cost on the device (Wi-Fi bandwidth + museum API quotas) at the
expense of some SD card slack.

If orphan accumulation becomes a real problem in the field, add a manifest-
based GC: each cache file lists the storage_keys it owns; a "purge orphans"
admin action walks all caches, builds the union, and unlinks vault files
outside the union. This is deferred.

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

No new REST endpoints in v1. The browser already has direct read/write access
to playsets via `POST /playsets/{name}`. An institution channel serializes as:

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

1. `play_scheduler_refresh.c` notices a channel with type `INSTITUTION` and
   `refresh_pending = true`, and that the channel is past its
   `ai_refresh_interval_sec` freshness window.
2. Dispatches to `art_institution_refresh_channel(name, identifier, cache)`.
3. The component parses `name`, looks up the museum, calls the adapter's
   `refresh_channel()`.
4. Adapter walks the museum's listing API, paginating up to
   `ai_cache_size` entries, building an array of
   `institution_channel_entry_t`.
5. Merges into `channel_cache_t` (same merge path Giphy uses,
   parameterized by entry size — already 64).
6. Schedules a debounced cache save.
7. Updates the `channel_metadata` last-refresh sidecar.

### 7.3 Download

1. The download manager picks an entry from Ci that is not in LAi.
2. Looks up the museum dispatch entry, calls
   `build_iiif_url(entry, 720, ...)`.
3. Streams the JPEG via `esp_http_client` → vault path.
4. On success, calls `lai_add_entry(cache, entry->post_id)`.

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
| `ai_cache_size` | uint32 | 256 | 32, 64, 128, 256, 512, 1024, 2048, 4096 |

NVS keys are short (`ai_*` prefix) to fit the NVS 15-char key limit.

Both surface in `webui/settings.html` under a new "Museums" section, matching
the existing Giphy section. The refresh dispatcher reads `ai_refresh_sec` to
gate refresh eligibility for institution channels; the merge step trims to
`ai_cache_size`. The `CHANNEL_CACHE_HARD_CAP` of 4096 still applies as the
absolute upper bound.

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
- **Set list:** baked from OAI-PMH `verb=ListSets` once (193 entries). Same
  `rijks-sets.json` as ubi-test. Stored at `webui/museum/rijks-sets.json`
  and served as a static asset.
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

**v1 implementation:**
1. Default request: `…/full/!720,720/0/default.jpg`. Universally supported
   by every Tier-1 server in the survey.
2. The `info.json`-aware negotiator is **deferred to v1.1** with a
   measurement-driven decision: compare bandwidth saved against the extra
   round trip, per museum.

Output format is JPEG. Museum IIIF servers reliably serve JPEG; WebP is
inconsistent.

## 11. Error handling

| Failure | Surface |
|---|---|
| Wi-Fi offline | Refresh skipped (existing dispatcher behavior). Browse modal shows a "Connect to Wi-Fi to browse museums" hint. |
| Museum API returns 5xx | Refresh logs the error, leaves cache unchanged, retries on the next cycle. |
| Museum API returns 429 | Per-museum cooldown (mirror of `giphy_set_rate_limited`). Browse UI surfaces "rate-limited, try again in N seconds". |
| TLS handshake failure | Logged. The `esp_crt_bundle` should cover all Tier-1 museum CDNs (`artic.edu`, `iiif.micr.io`, `data.rijksmuseum.nl`). Verify per-CDN before shipping. |
| Empty cache after refresh | Channel marked inactive (existing pattern). UI shows "no artworks". |
| Image download 404 | Entry left out of LAi; another entry is picked at playback time. |
| Channel spec parses but museum is unknown (newer playset on older firmware) | Channel skipped at execute time, logged WARN. |
| Rijks Linked Art walk fails for a specific artwork | Entry skipped; logged. The artwork stays unresolved and is retried on the next download attempt. |

## 12. Testing approach

- **Browser-side adapters:** mirror the existing `ubi-test/tests/*.spec.js`
  Playwright suite. Each adapter has unit tests for `listCollections`,
  `listArtworks`, `getArtwork`. CI for these tests can run from the same
  workspace as ubi-test.
- **Device-side refresh:** unit-test the URL builder, the JSON parser, and
  the cache merge logic with fixture responses captured from real museum
  endpoints (saved under `components/art_institution/test/fixtures/`).
- **End-to-end:** manual on-device test before each release — add an AIC
  Painting channel, an AIC Department channel, and a Rijks Set channel; run
  for 24 hours; verify the picker rotates, downloads succeed, and refresh
  completes.

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
- **Vault GC on channel delete** via per-channel manifest mark-and-sweep.
- **`info.json` size negotiation** for tighter rendition matching.
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

## 15. Open questions

These are pending for the next round with the user.

1. **Confirm "no breaking change" decisions** above (sections 4.1, 4.2, 4.3).
2. **Channel display name format.** Current proposal: `"AIC · Departments · Arts of Greece, Rome, and Byzantium"`. Alternatives: `"Painting (AIC)"`, `"AIC / Department / Painting"`. Affects the `display_name[65]` slot.
3. **First-refresh trigger.** When a playset containing an institution channel is first activated, does it immediately schedule a refresh, or wait for the periodic timer? Recommendation: immediate (matches Makapix/Giphy).
4. **Standalone "Browse Museums" page.** Should the WebUI top nav include a dedicated browse page (`/museum-browse`) for browsing without committing to a playset edit? Recommendation: no in v1; only via the Add Channel modal.
5. **Pre-warm of axis lists.** Should the device proactively fetch the AIC department list and Rijks set list on first Wi-Fi connect, so the browse modal opens instantly? Recommendation: no — browse is browser-direct, the device doesn't need them.
6. **TLS cert bundle.** Verify `esp_crt_bundle` covers `api.artic.edu`, `www.artic.edu`, `iiif.micr.io`, `data.rijksmuseum.nl` before merging the C side. If not, document which CDN needs an explicit cert addition.
7. **`info.json`-aware rendition.** Confirm v1 ships with the `!720,720` fallback only, with the smarter negotiator deferred to v1.1.
