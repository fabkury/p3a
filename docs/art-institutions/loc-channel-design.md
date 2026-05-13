# Library of Congress Channel — Design

- **Status:** Design (pending implementation)
- **Date:** 2026-05-13
- **Owner:** pub@kury.dev
- **Context:** Extends `docs/art-institutions/finalized-design.md`. Adds
  the Library of Congress (LoC) as the sixth integrated museum, after
  AIC, Rijksmuseum, V&A, Wellcome Collection, and SMK.
- **Investigation report:** `docs/art-institutions/loc-investigation/REPORT.md`
  (probe scripts + raw API dumps).

## 1. Summary

The Library of Congress holds 30M+ digitized items, of which ~7M are
visual. Its JSON API and IIIF Image API are anonymous and well-behaved,
but the data is heterogeneous: items mix art, newspapers, sheet music,
audio, and text under one query surface. Three of those item categories
yield IIIF identifiers that fit our 48-char `iiif_key` slot and are
plausibly "art": **photo / print / drawing**, **manuscript / mixed
material**, and **3D object**.

The channel is single-axis (`format`) and follows the V&A/Wellcome
implementation pattern almost exactly: facet-keyed listing, IIIF id
surfaced inline (no resolution walk), JPEG output at 720 px.

The non-trivial LoC-specific behaviors all come from the API's
heterogeneity:

- Refresh must filter listing entries by IIIF prefix (most "photo"
  results have no IIIF service at all).
- Refresh must drop entries whose IIIF id exceeds 47 chars (newspaper
  and music batches are mixed into otherwise-art-bearing facets).
- The browser preview must do the same filtering on each listing page,
  paginating eagerly when a fetched page yields few previewable items.

The decision to filter rather than widen the cache-entry layout is
captured separately in [`docs/deferred/loc-iiif-key-48-char.md`](../deferred/loc-iiif-key-48-char.md).

## 2. Scope

### In v1

- New museum entry in the dispatch table (`loc`).
- Single facet axis: `format`. Three terms: `photo, print, drawing`,
  `manuscript/mixed material`, `3d object`.
- Browser-direct queries to LoC's JSON API for browse + preview.
- Device-side refresh paginating LoC's `/search/` endpoint with
  `fa=original-format:<value>` filtering.
- IIIF JPEG download at `!720,720`.
- Single-artwork preview with Previous / Next navigation (matches the
  existing single-artwork-preview pattern from §15.6 of
  finalized-design.md).
- TLS cert-bundle verification for `www.loc.gov` and `tile.loc.gov`.
- Per-museum cooldown plugged into the existing rate-limit table.

### Deferred

- Keyword search (also deferred for every other museum).
- The `partof` axis (LoC division/collection slug). The
  `partof:prints and photographs division` facet does not surface IIIF
  URLs in listing responses — see §5.3.
- Map and newspaper format facets. Maps are art-relevant but the user
  explicitly excluded them from v1; newspapers and notated music
  exceed the 48-char IIIF-id slot.
- Synthesis of IIIF id from `storage-services` thumbnail URLs.
  Empirically tested and confirmed broken — see investigation REPORT §Q3.

## 3. Per-museum specification

Slot in the format of `finalized-design.md` §9.x:

- **id:** `loc`
- **display:** `Library of Congress`
- **short:** `LoC`
- **API base:** `https://www.loc.gov`
- **IIIF base:** `https://tile.loc.gov/image-services/iiif`
- **Required header:**
  `User-Agent: p3a/{version} (pub@kury.dev)` — LoC does not require it,
  but no published rate limit exists and the polite identification
  matches what AIC mandates.
- **Axes (filterable, in browse order):** `format` (single axis)
- **Format terms (hardcoded in the browser adapter):**

  | Wire identifier | Display label | Approx. total | Approx. usable¹ |
  |---|---|---:|---:|
  | `photo, print, drawing` | Photo, Print, Drawing | 12,186 | ~731 |
  | `manuscript/mixed material` | Manuscript/Mixed Material | 4,951 | ~446 |
  | `3d object` | 3D Object | 2,705 | unverified |

  ¹ "Usable" = surfaces a IIIF URL in the listing response AND the IIIF
  id length < 48. Photo derived from the observed 8 % IIIF surface rate
  × 75 % fit_48 of those; Manuscript from 28 % × 32 %. The count shown
  to the user during browse is the LoC API's raw `pagination.total` —
  not the usable-after-filter number — matching the cardinality
  semantics used elsewhere.

- **Pagination cap:** 500 pages × 100 results = 50,000 results. This is
  defensive; the natural end-of-results limit always kicks in first
  because no eligible facet exceeds 13,000 total results.
- **Rate limit:** none published. Treated like Rijks (default 60 s
  cooldown on 429 without `Retry-After`; honor `Retry-After` when
  present).
- **Listing endpoint:**
  `GET /search/?fo=json&fa=original-format:<value>&c=100&sp=N`
- **IIIF URL:**
  `https://tile.loc.gov/image-services/iiif/{iiif_key}/full/!720,720/0/default.jpg`
- **`iiif_key` value:** the path segment between
  `https://tile.loc.gov/image-services/iiif/` and the next `/` in any
  `image_url[]` or `resources[].image` URL on the result. Entries
  whose key is empty or ≥ 48 chars are skipped at refresh and at
  browser-side preview population.
- **`extension`:** always 3 (jpg) — LoC's IIIF reliably serves JPEG.
- **`resolve_entry`:** NULL. IIIF id is inline; no Linked-Art walk.

## 4. Channel-spec encoding

Reuses the existing playset v11 binary format with no schema change:

| Field | Value |
|---|---|
| `type` | `PS_CHANNEL_TYPE_INSTITUTION = 7` |
| `name` | `"loc:format"` |
| `identifier` | one of `"photo, print, drawing"`, `"manuscript/mixed material"`, `"3d object"` |
| `display_name` | `"LoC · Photo, Print, Drawing"` / `"LoC · Manuscript/Mixed Material"` / `"LoC · 3D Object"` |
| `weight` | unchanged |

The identifier strings are 21, 25, and 9 characters respectively — all
fit inside the existing `identifier[33]` slot. The display-name format
matches the convention from finalized-design.md §4.1 (`{museum_short} ·
{Term label}`, axis omitted, 64-char truncate).

## 5. API quirks the firmware must handle

These are the LoC-specific quirks beyond the cross-museum patterns
already documented in `finalized-design.md`. The investigation report
expands each with raw data.

### 5.1 `fa=` filter syntax is brittle

LoC's facet filter parameter is silently dropped if the field name or
value isn't in the exact form the API expects:

- Field name: lowercase with **hyphens** — `original-format`,
  `online-format`, `partof`. Underscore form (`original_format`) is
  silently dropped.
- Value: lowercase display title with **spaces preserved** —
  `photo, print, drawing`. Slugged form
  (`photo-print-drawing` or `Photo+Print+Drawing`) is silently dropped.

The refresh code must construct the parameter exactly. URL-encoding
the value (`urlencode` → `photo%2C+print%2C+drawing`) is correct;
do **not** post-process the encoded value into a slug form.

### 5.2 Most listing results have no IIIF service

For the photo/print/drawing facet, ~92 % of listing entries return
only a `_150px.jpg` thumbnail in `image_url[]` and have no `tile.loc.gov/image-services/iiif/`
URL anywhere in the response. Those items don't have a IIIF service
and there is no reliable transformation from the storage URL to a
working IIIF id (tested in `probe_loc_iiif_derive.py` — 404s on info.json).

**Refresh behavior:** walk every result, accept only those whose
`image_url[]` or `resources[].image` contains a string starting with
`https://tile.loc.gov/image-services/iiif/`. Extract the segment
between that prefix and the next `/` as the `iiif_key`.

**Browser behavior:** same filter when populating the preview buffer.
The preview loop fetches additional pages eagerly when a page yields
fewer than the requested number of previewable items — for photo/print/drawing,
a `c=20` request typically yields 1–2 previewable items, so the browser
may issue 10+ pages to fill a 20-item preview buffer (still bounded by
the 500-page cap and the 12K-item facet total).

### 5.3 The `partof` axis cannot replace `format`

`partof:prints and photographs division` is the most prominent visual
facet on LoC (1.1M items), and intuitively the strongest art axis. In
practice it yields **0 %** IIIF-bearing entries in the listing
response: PPOC items uniformly surface only storage-services
thumbnails. This is why v1 does not expose a `partof` axis at all,
even though `fa=partof:` is otherwise a valid filter syntax.

### 5.4 IIIF id length is bimodal

Working IIIF ids fall into two clusters:

- **Short (27–47 chars):** `service:pnp:highsm:35300:35397` (Carol
  Highsmith photos), `service:gmd:gmd382:g3820:g3820:ct005994` (maps),
  `service:mss:mal:380:3803800:001` (manuscripts),
  `public:gdc:2018693977:0050000` (digital collections). All fit our
  48-char slot.
- **Long (60–125 chars):** newspaper batches
  (`service:ndnp:...`), music silent films
  (`service:music:mussilentfilms:...`), audio-folklore archives. These
  exceed the 48-char slot.

No facet cleanly separates the two — they're mixed inside otherwise
art-bearing facets. The refresh code drops any entry with
`strlen(iiif_key) >= 48`. The deferred decision around widening this
slot is captured in [`docs/deferred/loc-iiif-key-48-char.md`](../deferred/loc-iiif-key-48-char.md).

### 5.5 IIIF ids contain colons; FAT32 sanitization needed for the vault filename

LoC's IIIF identifier shape (`service:pnp:highsm:35300:35397`) embeds
colons as path separators. The cache entry stores the canonical form
so URL building stays correct, but the SD card uses FAT32 and FatFs
rejects `:` in filenames (see `ff.c` `create_name()` — the rejection
set is `* : < > | " ? \x7F`).

`art_institution_build_vault_path` in `art_institution.c` sanitizes
`:` → `_` in the filename portion only. The fix is in the shared
vault path builder rather than the LoC adapter because no other
museum's IIIF key contains FAT-reserved characters — sanitizing
unconditionally is a no-op for the other five. Other reserved chars
(`* < > | " ? \x7F`) have not been observed in LoC IIIF ids and are
not sanitized; if they appear in the future, extend the sanitization
loop in the same place.

### 5.6 Periodic transport flakiness

The investigation observed `IncompleteRead` and `Read timed out` on
sustained probing. The existing per-museum fetch retry pattern
(`s_fetch_backoff_ms = { 0, 1000, 3000 }`) is sufficient — no
LoC-specific change needed beyond reusing it.

## 6. Component changes

### 6.1 C side

**New files:**

- `components/art_institution/museums/loc.c`
  - `art_institution_loc_refresh_channel(channel_id, axis, term_id)`
  - `art_institution_loc_build_iiif_url(entry, longest_side, out, len)`

Structure mirrors `museums/vam.c` (closest sibling — single-axis with
inline IIIF id and JPEG output). Differences:

- Skip results without a IIIF URL in `image_url[]` or
  `resources[].image` (see §5.2 above).
- Skip results whose extracted `iiif_key` is empty or ≥ 48 chars
  (see §5.4).
- Use `c=100` page size (matches AIC/V&A/Wellcome conventions).
- 500-page defensive cap (see §3).
- Same partial-success treatment AIC uses for 403 on deep pages: if
  any prior page merged successfully, log WARN, persist
  `last_refresh`, return `ESP_OK`; skip orphan eviction.

**Modified files:**

- `components/art_institution/include/art_institution_types.h` —
  append `ART_INSTITUTION_MUSEUM_LOC` before `ART_INSTITUTION_NUM_MUSEUMS`.
- `components/art_institution/art_institution_internal.h` — declare
  the two new functions.
- `components/art_institution/art_institution.c` — append the dispatch
  entry to `ART_INSTITUTION_MUSEUMS[]`.
- `components/art_institution/CMakeLists.txt` — add `museums/loc.c`
  to the source list.

### 6.2 Web UI

**New files:**

- `webui/museum/loc.js` exporting a `LocAdapter` class with the same
  surface every other adapter implements (`id`, `displayName`,
  `shortName`, `axes`, `listCollections`, `listArtworks`,
  `thumbnailUrl`, `previewUrl`).

The adapter:

- Hardcodes the three format terms (display labels + lowercase wire
  identifiers).
- `listCollections({axis: 'format'})` returns the three terms
  immediately. Counts are populated by three parallel `c=1` probe
  requests (concurrency 3 since the list is fixed).
- `listArtworks(termId, {offset, rows, axis})` issues
  `GET /search/?fo=json&c={rows}&sp={page}&fa=original-format:{termId}`
  with proper URL-encoding. Filters returned `results[]` to those
  with a `tile.loc.gov/image-services/iiif/` URL whose extracted id
  is < 48 chars. Returns `{items, total}` matching the existing
  adapter contract.
- `previewUrl(item, size=400)` synchronously returns
  `${IIIF_HOST}/${imageId}/full/!${size},${size}/0/default.jpg`.

**Modified files:**

- `webui/museum/index.js` — import and register `LocAdapter` after the
  existing five adapters.

The browse modal (`webui/museum/browse.js`) requires no changes — it
already dispatches through the adapter surface uniformly.

## 7. Refresh lifecycle

Identical to V&A/Wellcome's refresh path with the two LoC-specific
filters layered on top. Pseudocode:

```c
page = 1
while total_fetched < cache_size and page <= 500:
    page_response = fetch(loc_url(filter, term, page, c=100))
    if 429: cooldown(); break
    if 403/401 and page > 1: partial_success; break
    if 200:
        for each result in page_response.results:
            iiif_key = extract_iiif_id(result)  # may return NULL
            if iiif_key is NULL or len(iiif_key) >= 48: continue
            entry = build_entry(iiif_key, museum="loc", ext=3)
            buffer.append(entry)
        if buffer.size > 0:
            merge_into_cache_under_lifecycle_lock()
            track_post_ids_in_Si()
            download_manager_rescan()
            total_fetched += buffer.size
        if not page_response.pagination.has_more: break
        sleep(200 ms)
        page += 1

if refresh_completed:
    evict_orphans(Si)
save_channel_metadata(last_refresh=now)
return ESP_OK
```

## 8. Error handling

All inherited from finalized-design.md §11. LoC-specific overlays:

| Failure | Surface |
|---|---|
| Listing result has no IIIF URL | Silently skipped during refresh and browser preview population. |
| Extracted `iiif_key` ≥ 48 chars | Silently skipped — same path. |
| `IncompleteRead` / timeout mid-fetch | Retried per the standard `s_fetch_backoff_ms` schedule. |
| 429 with no `Retry-After` | 60 s default cooldown engaged via `art_institution_set_rate_limited("loc", 0)`. |

## 9. NVS settings

No new keys. Reuses `ai_refresh_sec` and `ai_cache_size` from
finalized-design.md §8. The default `ai_cache_size = 1024` may be
larger than what LoC can actually fill for any one channel (only
photo/print/drawing has a theoretical ceiling of ~975 usable items)
— that's fine; the cache merge code already tolerates a smaller
working set.

## 10. Implementation milestones

Mirrors finalized-design.md §14 (small-milestone, vertical-slice
approach).

### M1 — LoC end-to-end

1. **C side**
   - `museums/loc.c` adapter with the IIIF-prefix and 48-char filters.
   - Wire `ART_INSTITUTION_MUSEUM_LOC` into the enum + dispatch table.
   - TLS cert bundle verified for `www.loc.gov` and `tile.loc.gov`.
2. **Web UI**
   - `webui/museum/loc.js` adapter.
   - `webui/museum/index.js` registration.
3. **Manual gate (release):** 24-hour soak with an LoC
   `photo, print, drawing` channel and an LoC `manuscript/mixed
   material` channel; picker rotates, downloads succeed, refresh
   completes within the page cap.

### M2 — Documentation polish

- Update `docs/HOW-TO-USE.md` with the new museum.
- Append §9.6 to `docs/art-institutions/finalized-design.md` so the
  master design doc mentions LoC (this can land alongside M1 or
  immediately after).

## 11. Testing approach

Same hands-on approach used for the other museum integrations
(`finalized-design.md` §12). No CI harness.

### 11.1 Browser-side adapter

Manual smoke test from a desktop browser pointed at the device's
served UI: open the playset editor, pick LoC, see three terms, click
through to preview, navigate Previous/Next, click Add. Confirm the
spec object returned to the editor has the expected shape.

### 11.2 Device-side refresh

Capture fixture JSON responses from real LoC endpoints under
`components/art_institution/test/fixtures/loc/` so the URL builder,
JSON parser, and cache-merge code can be exercised manually. The
`docs/art-institutions/loc-investigation/output/raw/` directory
already contains ~30 representative JSON dumps that can seed the
fixture set.

### 11.3 TLS cert bundle verification (pre-merge gate)

Before the first C-side commit lands, verify `esp_crt_bundle` covers
`www.loc.gov` and `tile.loc.gov`. If the chain root is missing,
document the explicit `esp_crt_bundle_attach` + custom cert
workaround before the code is committed.

### 11.4 Manual end-to-end (release gate)

1. Add an LoC Photo/Print/Drawing channel.
2. Add an LoC Manuscript/Mixed Material channel.
3. Confirm immediate first-refresh kicks in within one dispatcher tick.
4. Let the device run for 24 hours and confirm:
   - the picker rotates,
   - JPEG downloads succeed,
   - the periodic refresh completes without errors,
   - low IIIF yield does not exhaust dispatcher tick budget.

## 12. Future work / deferred

- **Widen the `iiif_key` slot** to admit newspaper, music, and
  multi-segment ids — captured in
  [`docs/deferred/loc-iiif-key-48-char.md`](../deferred/loc-iiif-key-48-char.md).
- **Map facet.** 60 K items, 97 % surface IIIF in listing, 35 % fit
  48-char slot. Excluded from v1 by the user but a low-cost addition
  later: just append `"map"` to the format-term hardcode in
  `webui/museum/loc.js`.
- **`partof` axis.** Would require either per-item IIIF discovery
  (expensive) or a curated collection-slug allowlist (Rijks-style
  baked file). Excluded from v1.
- **Keyword search.** Same deferral as every other museum; encoding
  fits the same channel spec as `loc:search`.
- **Discrete-size negotiation.** LoC's IIIF info.json does not
  advertise discrete `sizes[]`; we always request `!720,720`. If a
  future device needs a different rendition, the per-museum
  `build_iiif_url` is the place to adjust.
