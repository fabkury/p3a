# Museum Channels — Wellcome + SMK Design

- **Status:** Approved (proceeds to writing-plans)
- **Date:** 2026-05-12
- **Owner:** pub@kury.dev
- **Companion to:** [`docs/art-institutions/finalized-design.md`](../../art-institutions/finalized-design.md)
- **Scope:** add Wellcome Collection and SMK (Statens Museum for Kunst) as the
  fourth and fifth museum channels in p3a. Gallica was considered and is
  deferred (see `docs/deferred/gallica.md`).

## 1. Scope

### In

- End-to-end Wellcome Collection channel (browse → persist → refresh →
  download → play), four facet axes.
- End-to-end SMK channel, single facet axis (`collection`).
- New `docs/deferred/` folder for design decisions revisited later, seeded
  with two entries (Gallica, Wellcome long labels).
- TLS cert-bundle verification for the four new CDNs.

### Out

- **Gallica.** SRU is XML, not JSON; ESP-IDF has no XML parser shipped with
  the codebase. Documented in `docs/deferred/gallica.md`.
- **Keyword search**, for any museum. Continues to match the v1 deferral in
  `finalized-design.md` §1.
- **Wellcome terms whose label > 32 chars.** The playset `identifier[33]`
  field can't hold them; the browse modal filters them out. Documented in
  `docs/deferred/wellcome-long-labels.md`.

## 2. No format breaks

The following are reused verbatim from `finalized-design.md`:

- `PS_CHANNEL_TYPE_INSTITUTION = 7` channel type ordinal.
- 64-byte `institution_channel_entry_t` cache entry layout.
- `{museum_id}:{axis}` spec name encoding (e.g. `"wellcome:genres"`,
  `"smk:collection"`).
- Vault layout `/sdcard/p3a/museum/{museum_id}/{sha[0..2]}/{iiif_key}.{ext}`.
- The two NVS settings (`ai_refresh_sec`, `ai_cache_size`).
- The per-museum rate-limit table, cooldown API, browse-modal cooldown
  banner, and landing-page "API rate limited" badge logic.
- The orphan-eviction pattern (intra-channel + age-based storage eviction).
- The browse-modal flow (museum → axis → term → single-artwork preview with
  Prev/Next → Add).

This is purely additive work: two C adapters, two JS adapters, two dispatch
entries, two enum values.

## 3. Per-museum specifications

### 3.1 Wellcome Collection

| Field | Value |
|---|---|
| **id** (wire) | `wellcome` |
| **display** | `Wellcome Collection` |
| **short** | `Wellcome` |
| **API base** | `https://api.wellcomecollection.org/catalogue/v2/works` |
| **IIIF base** | `https://iiif.wellcomecollection.org/image/` |
| **Required headers** | none beyond `Accept: application/json` |
| **Published rate limit** | none; default 60 s cooldown on 429 |

**Axes (browse order):** `workType`, `genres`, `subjects`, `contributors`.

**Filter-param map:**

| Axis | Filter param | Filter value source |
|---|---|---|
| `workType` | `workType` | term `id` (`k`, `q`, `h`, `r`, `hdig`) |
| `genres` | `genres.label` | term `label` |
| `subjects` | `subjects.label` | term `label` |
| `contributors` | `contributors.agent.label` | term `label` |

For non-`workType` axes, Wellcome's aggregation buckets carry only a label
(no stable short id), so the label IS the filter value. This is why the
32-char hide-from-browse rule exists.

**Term enumeration (JS).** Single request:

```
GET /works?pageSize=1
  &items.locations.locationType=iiif-image
  &aggregations=workType(100),genres.label(100),
                subjects.label(100),contributors.agent.label(100)
```

`(100)` is Wellcome's aggregations size suffix. No per-term count probe is
needed. The JS adapter caches the parsed result per axis for the session.

**Listing endpoint (C and JS):**

```
GET /works?page=N&pageSize=100
  &items.locations.locationType=iiif-image
  &include=items
  &{filter_param}={filter_value}
```

`include=items` is what surfaces the IIIF Image service location on each
result.

**`iiif_key` value.** The vid extracted from each result's
`items[].locations[]` where `locationType.id == "iiif-image"`. Match against
`^https://iiif\.wellcomecollection\.org/image/([^/]+)(/info\.json)?$`;
capture group 1 is the vid (8–12 chars in practice). Records without a
matching location are skipped.

**`extension`.** Always `3` (jpg). Inline resolution, no `resolve_entry`
callback.

**IIIF URL.**

```
https://iiif.wellcomecollection.org/image/{vid}/full/!720,720/0/default.jpg
```

**Long-label gate.** The JS adapter's `listCollections()` filters terms
whose `label.length > 32` before returning. This caps at the playset
`identifier[33]` field ceiling. Filter applies to all four axes (workType
labels are short enough in practice but pass through the same filter for
uniformity).

**Pagination cap.** None published; refresh walks until `ai_cache_size` or
`totalResults` is reached.

### 3.2 SMK (Statens Museum for Kunst)

| Field | Value |
|---|---|
| **id** (wire) | `smk` |
| **display** | `Statens Museum for Kunst` |
| **short** | `SMK` |
| **API base** | `https://api.smk.dk/api/v1` |
| **IIIF base** (prefix) | `https://iip.smk.dk/iiif/jp2/` |
| **Required headers** | none beyond `Accept: application/json` |
| **Published rate limit** | none; default 60 s cooldown on 429 |

**Axis.** Single axis: `collection`. Modal auto-advances past the axis step
(existing behavior for axis-less / single-axis museums).

**Term enumeration (JS):**

```
GET /art/search?keys=*&rows=0&facets=collection
```

Returns alternating `[name, count, name, count, ...]` array under
`facets.collection` (SMK can also return a list of pairs or a dict; the
adapter handles all three forms, mirroring the reference run.py). Filters
out terms with `count == 0` and labels with `length > 32`, sorts descending
by count.

**Listing endpoint (C and JS):**

```
GET /art/search?keys=*&filters=[collection:{name}],[has_image:true]
  &offset=N&rows=100
```

Native `offset+rows` pagination. `[has_image:true]` keeps image-less
records out of the cache.

**`iiif_key` value.** Extracted from each result's `image_iiif_id` URL
(e.g. `https://iip.smk.dk/iiif/jp2/bc386p50w_kksgb22235.tif.jp2`). The
adapter:

1. Verifies the URL contains `/iiif/jp2/`; skips records that don't match.
2. Stores the substring after the last `/jp2/` as `iiif_key`
   (e.g. `bc386p50w_kksgb22235.tif.jp2`).
3. Skips records whose extracted filename ≥ 48 chars.

Observed lengths in the reference run: 27–46 chars (max
`fn107176m_kksgb12522.tif.reconstructed.tif.jp2` = 46). Fits the 48-byte
slot with 1–2 chars of headroom.

**`extension`.** Always `3` (jpg). SMK's IIPImage backend advertises WebP
in `info.json` but returns HTTP 400 for `.webp` requests; stay on JPEG
(captured in the SMK reference report). No `resolve_entry` callback.

**IIIF URL.**

```
https://iip.smk.dk/iiif/jp2/{iiif_key}/full/!720,720/0/default.jpg
```

**Pagination cap.** None published; refresh walks until `ai_cache_size` or
`found` is reached.

## 4. C-side implementation

### 4.1 New files

```
components/art_institution/museums/
  wellcome.c        # mirrors museums/vam.c (inline IIIF id, no resolver)
  smk.c             # mirrors museums/vam.c (inline IIIF id, no resolver)
```

### 4.2 Dispatch table (`art_institution.c`)

Append two entries to `ART_INSTITUTION_MUSEUMS[]`, in release order
(Wellcome before SMK), both with `resolve_entry = NULL`:

```c
{
    .id              = "wellcome",
    .display         = "Wellcome Collection",
    .museum_enum     = ART_INSTITUTION_MUSEUM_WELLCOME,
    .refresh_channel = art_institution_wellcome_refresh_channel,
    .build_iiif_url  = art_institution_wellcome_build_iiif_url,
    .resolve_entry   = NULL,
},
{
    .id              = "smk",
    .display         = "Statens Museum for Kunst",
    .museum_enum     = ART_INSTITUTION_MUSEUM_SMK,
    .refresh_channel = art_institution_smk_refresh_channel,
    .build_iiif_url  = art_institution_smk_build_iiif_url,
    .resolve_entry   = NULL,
},
```

### 4.3 Enum (`art_institution_types.h`)

Append two values to `museum_id_t` — never renumber existing entries.

```c
ART_INSTITUTION_MUSEUM_WELLCOME = 3,
ART_INSTITUTION_MUSEUM_SMK      = 4,
ART_INSTITUTION_MUSEUM_COUNT
```

The rate-limit table in `art_institution_rate_limit.c` is sized by
`ART_INSTITUTION_MUSEUM_COUNT`, so it grows automatically.

### 4.4 Internal decls (`art_institution_internal.h`)

Add four function prototypes (refresh + URL builder for each museum). No
`resolve_entry` decls.

### 4.5 CMakeLists

Append `museums/wellcome.c` and `museums/smk.c` to `SRCS` in
`components/art_institution/CMakeLists.txt`.

### 4.6 Refresh body shape

Both refresh functions follow the V&A template exactly:

1. Validate inputs; cooldown gate; verify cache exists.
2. Allocate response buffer + page-entry buffer in PSRAM with malloc
   fallback.
3. Page loop: fetch one page → parse JSON → extract entries (per museum's
   field mapping) → re-resolve cache under `channel_cache_lifecycle_lock()`
   → merge → grow `Si` hash → `download_manager_rescan()` → 200 ms inter-
   page delay → advance page.
4. On HTTP 429: parse `Retry-After`, fall back to 60 s, engage cooldown,
   return `ESP_ERR_INVALID_RESPONSE`.
5. On HTTP 5xx / network error: retry up to 3 times with exponential
   backoff (0/1000/3000 ms); if still failing after a successful prior
   page, treat as partial-with-content.
6. On complete walk: run orphan eviction. On partial-with-content: skip
   orphan eviction (entries that simply weren't re-fetched would be
   misidentified as evicted).
7. Persist `last_refresh` whenever any content was fetched, gated on
   `sntp_sync_is_synchronized()`.

Response buffer sizes: Wellcome ≈ 256 KB (rich records with includes),
SMK ≈ 192 KB (lighter than V&A).

### 4.7 TLS cert bundle (pre-merge gate)

Verify `esp_crt_bundle` covers, before the first commit of each museum:

- **Wellcome:** `api.wellcomecollection.org`, `iiif.wellcomecollection.org`
- **SMK:** `api.smk.dk`, `iip.smk.dk`

If any chain root is missing, document the explicit
`esp_crt_bundle_attach` + custom-cert workaround before merging that
museum's code (same gate AIC and Rijks went through).

## 5. Web UI implementation

### 5.1 New files

```
webui/museum/
  wellcome.js       # 4-axis adapter, single aggregations call
  smk.js            # 1-axis adapter, single facets call
```

### 5.2 Registry (`index.js`)

Append two imports and two `new ...Adapter()` entries to `ADAPTERS`, in
release order matching the C dispatch table.

### 5.3 Adapter contract (unchanged shape)

Each new adapter exports a class implementing:

| Member | Wellcome | SMK |
|---|---|---|
| `id` | `'wellcome'` | `'smk'` |
| `displayName` | `'Wellcome Collection'` | `'Statens Museum for Kunst'` |
| `shortName` | `'Wellcome'` | `'SMK'` |
| `axes` | 4 entries (workType / genres / subjects / contributors) | 1 entry (collection) |
| `listCollections({axis})` | per §3.1 | per §3.2 |
| `listArtworks(termId, {offset, rows, axis})` | per §3.1 | per §3.2 |
| `thumbnailUrl(imageId, size)` | direct IIIF `!size,size` URL | direct IIIF `!size,size` URL |
| `previewUrl(item, size=400)` | synchronous `thumbnailUrl` | synchronous `thumbnailUrl` |

Both adapters' `fetch` wrappers report 429s to
`POST /api/museum/rate-limits/report-429` with the matching `museum` id and
parsed `Retry-After` (matching V&A's pattern).

### 5.4 Long-label gate

`listCollections()` filters out terms whose `label.length > 32` before
returning. Applies to all axes. Pure browse-side filter — never reaches the
device.

### 5.5 Settings page

`webui/settings.html`'s "Museums" section's settings-hint paragraph is
updated to name all five museums (AIC, Rijks, V&A, Wellcome, SMK). No new
controls; the two NVS keys still cover all museums.

### 5.6 Landing-page badge / browse modal

No code changes. The badge rule already keys on the museum id from the
channel spec; the rate-limit table grows to 5 entries but the rendering
logic is unchanged. The browse modal's single-artwork preview with
Prev/Next already works for any adapter whose `listArtworks` returns paged
items and whose `previewUrl` resolves synchronously.

## 6. Error handling

Identical to `finalized-design.md` §11:

| Failure | Surface |
|---|---|
| Wi-Fi offline | Refresh skipped; browse modal shows "Connect to Wi-Fi" hint |
| 5xx | Refresh logs, retries on next cycle |
| 429 | Per-museum cooldown engages; UI banner with countdown |
| TLS handshake failure | Logged; covered by pre-merge cert-bundle gate |
| Empty cache after refresh | Channel marked inactive (existing pattern) |
| Image download 404 | Entry left out of LAi; another picked at playback |
| Newer playset on older firmware (unknown museum) | Channel skipped, WARN |

Neither new museum needs AIC's "403 on deep pages, partial success"
exception in its refresh body; the partial-with-content branch still
applies naturally if any page fails after at least one successful page.

## 7. New `docs/deferred/` folder

A new top-level documentation folder for design decisions that were
considered, intentionally not done, and worth revisiting later. Each entry
captures: what was deferred, the cost of doing it now, the trigger to
revisit.

```
docs/deferred/
  README.md                      # one-line index of deferred decisions
  gallica.md                     # XML parser cost
  wellcome-long-labels.md        # 32-char identifier ceiling
```

**`docs/deferred/README.md` skeleton:**

> # Deferred design decisions
>
> Decisions we considered, intentionally didn't ship, and want to revisit.
> Each entry names: what was deferred, why now isn't the right time, what
> would change that.
>
> - [Gallica integration](gallica.md) — XML/SRU parser dependency.
> - [Wellcome long-label terms](wellcome-long-labels.md) — terms whose
>   label exceeds the playset 32-char identifier limit.

**`docs/deferred/gallica.md` contents:**

- **Deferred:** Gallica as the sixth museum channel.
- **Why:** Gallica's API is SRU/XML; the rest of the museum surface is
  JSON. No XML parser is included in the codebase or ESP-IDF; adding one
  (or hand-rolling an SRU-specific parser) is a substantial new dependency
  with non-trivial PSRAM and code-size cost.
- **Revisit when:** a lightweight XML parser becomes available in IDF, a
  user volume justifies the integration cost, or the existing five
  museums together show a content-diversity gap that Gallica would
  uniquely fill.
- **Reference materials kept under:** `reference/museum-art/source/gallica/`
  (run.py + report + sample images).

**`docs/deferred/wellcome-long-labels.md` contents:**

- **Deferred:** Wellcome terms whose label exceeds 32 chars.
- **Why:** The playset binary format's `identifier[33]` field (32 chars +
  null) can't hold them. For non-`workType` Wellcome axes, the label IS
  the filter value (no stable short ID), so we can't normalize. Hiding
  them from the browse modal is the cheapest correct behavior.
- **Estimated impact:** small but non-zero — a fraction of long-tail
  subject / contributor terms (e.g. multi-affiliation names) become
  unselectable.
- **Revisit when:** field usage shows enough valuable terms hidden to
  justify a playset format bump (P3PS v11 → v12) with a larger identifier
  field. Bumping identifier touches the binary format, several internal
  structs, the channel_id hash input, and the JSON serializer; not a
  small change.

## 8. Testing

Manual only, no CI harness. Per museum:

1. **Browser smoke (LAN, real device).** Open playset editor → Channel
   Type = Museum → pick the new museum → walk axis (Wellcome) / single
   facet (SMK) → pick a term → preview renders, Prev/Next works, Add
   commits.
2. **Device refresh.** Save the channel, watch first refresh kick in
   within one dispatcher tick. Confirm logs show clean page walk, no
   429s under normal load, orphan eviction on complete walks.
3. **TLS bundle gate** per §4.7 before first commit.
4. **Long-label hide gate.** Open a Wellcome subject or contributor axis;
   verify no item with `label.length > 32` is rendered.
5. **End-to-end soak (release gate).** Append one Wellcome channel and one
   SMK channel to the existing M2 soak playset. Run 24 h; confirm picker
   rotation, JPEG downloads, refresh completion, graceful rate-limit
   handling.

Fixture JSON for each museum dropped under
`components/art_institution/test/fixtures/` alongside the existing
AIC/Rijks/V&A captures, for offline hand-exercising of the
parser/merge code.

## 9. Implementation milestones

| | Surface | Manual gate |
|---|---|---|
| **M4 — Wellcome end-to-end** | `museums/wellcome.c` + dispatch entry + enum value; `webui/museum/wellcome.js` + index registration; `docs/deferred/wellcome-long-labels.md`; TLS bundle verified for `api.wellcomecollection.org`, `iiif.wellcomecollection.org` | 24 h soak on Wellcome workType=Pictures + Wellcome genres=Engraving channel |
| **M5 — SMK end-to-end** | `museums/smk.c` + dispatch entry + enum value; `webui/museum/smk.js` + index registration; TLS bundle verified for `api.smk.dk`, `iip.smk.dk` | 24 h soak on SMK collection=Gammel bestand channel |
| **M6 — Release polish** | Settings-hint copy update naming all five museums; `docs/HOW-TO-USE.md` mentions Wellcome and SMK; `docs/deferred/gallica.md` + `docs/deferred/README.md` written. No version bump (the project is already at 0.10.0 unreleased) | Smoke check on the updated UI strings |

## 10. Future work added to the finalized design's §13

Append to `docs/art-institutions/finalized-design.md` §13:

- **Gallica:** XML/SRU parser on ESP-IDF. Revisit trigger: a lightweight
  XML parser becomes available or value-of-Gallica justifies the cost.
  Deferred design notes in `docs/deferred/gallica.md`.
- **Wellcome long labels:** lifting the 32-char identifier limit. Revisit
  trigger: enough valuable Wellcome terms get hidden in real usage to
  justify a playset format bump. Deferred design notes in
  `docs/deferred/wellcome-long-labels.md`.
