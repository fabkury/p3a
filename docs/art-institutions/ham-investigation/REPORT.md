# Harvard Art Museums — API investigation report

*Investigation performed 2026-05-18 against the live HAM REST API
(`api.harvardartmuseums.org`) and IIIF image service. All probe scripts
and raw JSON dumps live alongside this file.*

## Headline findings

1. **HAM passes every Required item from `requirements.md`.** Stage 3
   (adapter implementation) is unblocked.
2. **Image-id length is comfortable**: 17–26 chars in the sampled
   cohort — about half the 47-char `iiif_key` budget. The bimodal
   distribution that bit LoC does not appear in HAM.
3. **There is one notable quirk**: `hasimage=1` returns records whose
   image is permission-restricted (no `primaryimageurl`, no `images[]`).
   About 48 % of the photographs cohort is affected. The fix is one
   additional listing parameter: `q=imagepermissionlevel:0`.
4. **The image URL flow is one 303 redirect** (`nrs.harvard.edu` URN →
   `ids.lib.harvard.edu` IDS path). The Rijks adapter already implements
   manual `Location`-header capture; we reuse the pattern. No
   `resolve_entry` walk is needed because the redirect itself is the
   resolution.
5. **No bool+range partitioning is needed.** HAM's Elasticsearch backend
   accepts `from + size ≤ 500 000`, well above p3a's worst case
   (`channel_offset 4096 + ai_cache_size 4096 = 8192`).

## Capability check against `requirements.md`

| # | Capability | Tier | Result |
|---|---|---|---|
| **A1** | Enumerate facet axes | R | ✅ HAM exposes 12 axes via top-level endpoints; 10 are usable. See [Axis strategy](#axis-strategy--hybrid-discovery). |
| **A2** | List terms with image-bearing counts | R | ✅ Term endpoints return `objectcount`. Image-bearing counts via a per-term `?hasimage=1&size=1` probe (≤ 1 request per term). |
| **A3** | List artworks for (axis, term) with offset/page | R | ✅ `GET /object?<axis>=<id>&page=N&size=≤100`. |
| **A4** | Filter to image-bearing records | R | ✅ `hasimage=1`, but see [Q1](#q1-imagepermissionlevel-gating) — must combine with `q=imagepermissionlevel:0`. |
| **A5** | Inline caption metadata | R | ✅ `title`, `dated`, `people[].displayname` all inline on `GET /object`. |
| **A6** | CORS allows browser-direct calls | P | ✅ `Access-Control-Allow-Origin: *` on `api.harvardartmuseums.org`. |
| **A7** | Browser-fetchable preview image | R | ✅ NRS URN + IIIF size syntax → IDS JPEG. Verified from desktop. |
| **B1** | HTTPS reachable via esp_http_client + esp_crt_bundle | R | ✅ Roots: Let's Encrypt R12 (API), Amazon RSA 2048 M04 (NRS), InCommon RSA Server CA 2 (IDS). All in Mozilla's default bundle. |
| **B2** | Random-access pagination ≥ 4096 offset | R | ✅ Tested to page 770 (≈ 77 000 entries) for the most-populous term; HTTP 200 throughout. |
| **B3** | Total record count in envelope | R | ✅ `info.totalrecords`, `info.pages`. |
| **B4** | Rate-limit signaling | R | ⚠️ Documented limit **2 500 req/day per API key** (no per-IP layer). 401 on missing/invalid key; no rate-limit headers observed in 200 responses. See [Q2](#q2-rate-limit-shape) for handling. |
| **B5** | Stable artwork identifiers across refreshes | P | ✅ In-run (page 50 returned identical first id on a repeat call). True week-over-week stability not measured in Stage 2 — see [Future verification](#future-verification). |
| **B6** | JSON response | R | ✅ `Content-Type: application/json; charset=utf-8`. |
| **B7** | Deep-offset workaround if cap is low | P | n/a — natural cap is 500 000 entries (see [Q5](#q5-elasticsearch-500-000-window)). |
| **C1** | IIIF v2 URL serves JPEG at `!720,720` | R | ✅ Confirmed `image/jpeg`, longest side = 720 px on 8/8 samples. |
| **C2** | Image identifier fits in 47 chars | R | ✅ 17–26 chars sampled, 0/54 over 47. |
| **C3** | Per-image size ≤ 16 MiB | P | ✅ Sample sizes 72–77 KB at `!720,720`. Well under 16 MiB. |
| **C4** | TLS chain covered by esp_crt_bundle | R | ✅ All three CDN roots in Mozilla's default bundle. |
| **C5** | Image identifier discoverable from listing | R | ✅ Inline via `images[0].baseimageurl`. One 303 redirect at download time (not a multi-hop walk). |
| **D1** | API-key model documented | R | ✅ `apikey=<uuid>` query param; 401 without it. Per-key quota of 2 500 req/day. |
| **D2** | No CPU-heavy per-request auth | R | ✅ Static query param only. |
| **D3** | Acceptable Use Policy permits our use case | R | See [Q6](#q6-attribution-and-aup). Standard educational/non-commercial display is documented as permitted. |
| **D4** | Cipher support compatible with ESP-IDF mbedTLS | P | Not directly tested. Roots are mainstream (LE, Amazon, InCommon) so cipher policies should be standard. Verify during M1 manual gate. |

## Quirks

### Q1 — imagepermissionlevel gating

`hasimage=1` matches every record where HAM tracks an image internally,
including those whose image cannot be publicly displayed
(`imagepermissionlevel = 1`). These records come back with
`primaryimageurl: null` and `images: []`, so the refresh would store
zero displayable entries even though `info.totalrecords` looked healthy.

Fix: add `q=imagepermissionlevel:0` to every `/object` listing request.

Effect on counts (verified):

| Filter | Photographs (classification=17) | All classifications |
|---|---|---|
| `hasimage=1` | 77 076 | — |
| `hasimage=1` + `q=imagepermissionlevel:0` | 40 522 | 167 020 |

So the effective collection size is roughly half the headline numbers,
but still vast (167 k displayable images across all classifications).

### Q2 — Rate-limit shape

HAM publishes **2 500 requests per day per API key**. We did not observe
any `X-RateLimit-*` response headers, so the firmware can't read live
remaining-quota; it must self-budget.

Two attack vectors share the same key:

- Device refresh: `floor(ai_cache_size / 100) + 1` requests per refresh.
  Default `ai_cache_size = 1024` → 11 requests. At `ai_refresh_sec =
  86400` (1 day) that's 11/day per channel. Ten channels = 110/day.
- Browser browse: each axis enumeration is ~30 term-count probes (one
  per term in the visible top-30). Each visit to the browse modal is
  perhaps 30–100 requests, depending on how many axes the user opens.

With shared key across all p3a installations, the daily budget is
divided across the entire installed base. Worst case (estimated ~50
active devices today): 50 devices × 110/day = 5 500/day, exceeds the
per-key cap. **This is a real ship-time risk and needs a key-management
strategy** — see [Open question](#open-question-key-budget-management).

When a 429 does land, the existing rate-limit cooldown table
(`art_institution_rate_limit.c`) treats it the same as the other
museums: engage the cooldown, surface "rate-limited" in the browse
modal, skip refresh until the deadline. Default cooldown when no
`Retry-After` header is present: 60 s, matching AIC and Rijks.

### Q3 — `worktype` vocabulary has zeroed objectcounts (filter still works)

The `/worktype` endpoint returns 414 terms but every record has
`objectcount: 0` in the vocabulary listing. **The filter itself is
functional** — `GET /object?worktype=100` returns 18 902 records — so
the axis ships, but term *ranking by popularity* requires per-term
probes since the vocabulary doesn't surface counts. Two acceptable
options for M1:

- **Alphabetical-only term ordering** for this axis; the modal shows
  terms in the order HAM returns them. Cheap (one `/worktype?size=100`
  call); user has to skim the picker instead of seeing top hits first.
- **Per-term count probe** with bounded concurrency (same shape as AIC's
  term-count probe). 414 probes at concurrency 6 ≈ 70 s on a cold
  open, then cached per browser session. Expensive on first open;
  parity with AIC's UX afterwards.

The adapter ships option 1 by default; option 2 can be enabled per-axis
via a config flag if usage data later shows the alphabetical ordering
is friction.

### Q4 — `/color` vocabulary exists but doesn't filter `/object`

The `/color` endpoint returns 147 terms (CSS color names like
`aliceblue` `#f0f8ff`), but **the `color` filter on `/object` returns
0 records for every value tried** — both the integer `colorid`
(34838376) and the hex string (`#f0f8ff`). The `/color` vocabulary
appears decorative; HAM exposes per-object color data via the
`colors[]` array on each object record and probably needs the
aggregations API to surface populated colors. Excluded from v1 as a
browse axis. Could be revisited if we ever add image-similarity
features (the per-object `colors[]` array is well-populated; it just
isn't a queryable filter today).

### Q4b — `/group` is fine (probe-bug correction)

An earlier draft of this report excluded `/group` as "empty" because
the facet-probe script sorted by `objectcount desc` and got 0 records
back. The vocabulary endpoint actually returns 30 curator-defined
groupings (e.g. "Analog Culture", "The Bauhaus: In America", "Alan
Burroughs Collection of X-Radiographs") and the filter works:
`group=2040005` returns 2 903 records. The `objectcount` field isn't
populated on the term records, same situation as `worktype` — same
M1 mitigation (alphabetical-only ordering, or per-term probe).

### Q5 — Elasticsearch 500 000 window

Page numbers high enough to make `from + size > 500 000` return an
ES `illegal_argument_exception` JSON envelope (still HTTP 200). p3a's
worst case is `(4096 + 4096) / 100 = 82 pages` so this is purely a
boundary curiosity — captured here so a future engineer doesn't
duplicate the test.

### Q6 — Attribution and AUP

HAM's IIIF Manifest carries `"attribution": "Harvard Art Museums"` per
IIIF presentation API conventions. The API terms (per the docs site)
require attribution and prohibit redistribution as a bulk archive.
p3a's use case is per-image display with the museum already credited in
the channel label (`HAM · Photographs`), which is consistent with the
typical museum-API attribution model. No additional overlay required.

### Q7 — `imagepermissionlevel` not a direct filter param

The doc-suggested filter set on `/object` does **not** include
`imagepermissionlevel`. Passing it as `imagepermissionlevel=0` is
silently ignored (totalrecords unchanged across 0/1/2). The
Elasticsearch `q=imagepermissionlevel:0` syntax is what works. Same
underlying ES backend, different surface: terse filters get a
hard-coded allowlist, `q` parameter accepts free-form ES Query String.

## Storage strategy for `iiif_key`

Option (a) from the image probe — **store the URN portion** (e.g.
`urn-3:HUAM:79762_dynmc`) — is the cleaner choice.

- `iiif_key` is 17–26 chars in the sample (much room to spare).
- C-side `build_iiif_url`: `snprintf(..., "https://nrs.harvard.edu/%s/full/!720,720/0/default.jpg", entry->iiif_key)`.
- Download path uses the existing Rijks `HTTP_EVENT_ON_HEADER` shim
  to capture the 303 `Location` and continue to the IDS URL. Single
  redirect per image; no `resolve_entry` dispatch hook needed.
- `extension` = 3 (jpg). No sentinel values used. Reuses the AIC/V&A
  shape.

Option (b) — pre-resolve at refresh time and store the IDS path — was
rejected: the FIFO cache stores 1024 entries by default but the player
consumes ~1 per swap interval (default ≥ 30 s). Most cached entries
never get downloaded, so the refresh-time HEAD round trips would be
wasted bandwidth.

## Axis strategy — hybrid discovery

Unlike the other adapters (AIC, V&A, Wellcome, SMK, Rijks), HAM's API
surface is uniform: endpoint name == filter-param name == term-resource
path. Per-axis quirks are limited to display labels, term-id field
naming, and a handful of broken/unusable endpoints. That makes a
**hybrid** axis model practical:

- **Adapter declares (compile-time):**
  - A **display-label map** so the UI shows "Classifications" instead
    of `classification`.
  - A **skip-list** of endpoints that are broken or wrong-shape for
    the picker (see [Excluded](#excluded) below).
- **Adapter discovers (runtime):**
  - Term lists per axis via the endpoint of the same name.
  - Per-term image-bearing counts via `?hasimage=1&q=imagepermissionlevel:0`
    probes.
  - Label-length filtering (drop terms whose `name` exceeds 32 chars,
    the identifier-slot ceiling) is applied uniformly across all axes.

This means if HAM adds a new well-shaped endpoint in the future, the
browse modal picks it up automatically as long as it follows the same
shape as the existing ten.

### Ship in M1 (HAM v1):

The ten axes that survive the skip-list, in curated browse order:

| Axis | Display label | Total terms | Max label | Top image-bearing count | Notes |
|---|---|---|---|---|---|
| **classification** | Classifications | 64 | 26 chars | 89 % at Photographs | Most natural starting point. |
| **century** | Centuries | 47 | 20 chars | 88 % at 20th c. | Temporal. |
| **culture** | Cultures | 255 | 21 chars | 89 % at American | Cultural; page-1 covers top 100. |
| **period** | Periods | 324 | 76 chars | 91 % at Edo | **22 % of top-100 labels exceed 32 chars and are dropped** — surviving terms still meaningful. |
| **place** | Places | 2 549 | 28 chars | 154 %† at U.S. | All labels fit. Top-100 by count is the practical surface. |
| **medium** | Media | 325 | 31 chars | 74 % at Metal | All labels fit. |
| **technique** | Techniques | 321 | 36 chars | 91 % at Gelatin silver print | 2 % of top-100 labels dropped. |
| **worktype** | Work types | 414 | 29 chars | filter works; vocabulary unranked | All labels fit. **Term ordering is alphabetical-only by default** (vocabulary `objectcount=0`); see Q3. |
| **group** | Groups | 30 | unmeasured (≥ 43 in 5-sample) | filter works; vocabulary unranked | Curator-defined collections. Same ordering caveat as worktype (Q4b). Small enough to ship without pagination. |
| **gallery** | Galleries | 64 | 47 chars | 95 % at Special Exhibitions | 27 % of labels dropped; tiny counts (top 295). Niche but functional. |

† `place=2028213` (United States) returns more `hasimage=1` records
than its `objectcount` because the place filter follows the TGN
hierarchy (sub-places match too) while `objectcount` is direct hits
only.

### Excluded (skip-list):

- **`color`** — vocabulary exists (147 CSS color names) but the
  `color` filter on `/object` is inert (returns 0 records for every
  value). The `colors[]` data lives per-object only; would need a
  different surface to expose. See Q4.
- **`person`** — 42 549 terms. Vocabulary is populated and the filter
  works (top: Lyonel Feininger 24 062 works), but a flat browse picker
  can't reasonably enumerate 42 k entries. UX exclusion, not technical.
  Could be unlocked with the deferred keyword-search feature or a
  "top-N most-prolific artists" cap.

## Per-museum specification draft for `finalized-design.md`

Pre-filled so M1 mostly becomes a copy-paste. Goes into §9 of
`finalized-design.md`:

```
### 9.X Harvard Art Museums

- **id:** `ham`
- **display:** `Harvard Art Museums`
- **API base:** `https://api.harvardartmuseums.org`
- **NRS image base:** `https://nrs.harvard.edu/`
- **Required header:** none beyond Accept: application/json.
- **Required query:** `apikey=<uuid>` on every API call. The key is
  **user-supplied** (BYOK) — stored in NVS under `ham_api_key`,
  entered by the user via a new "Museums" section in
  `webui/settings.html`. No key is shipped with the firmware. When
  the saved key is empty, HAM browse is gated in the modal and HAM
  channel refresh is a no-op (channels stay persistent and reactivate
  when the user enters a key).
- **Axes (filterable, in browse order):**
  `classification`, `century`, `culture`, `period`, `place`, `medium`,
  `technique`, `worktype`, `group`, `gallery`. The browse adapter ships
  a display-label map and a skip-list (`color`, `person`); axis
  discovery / term enumeration is otherwise driven by what the API
  actually returns. See [Axis strategy](#axis-strategy--hybrid-discovery).
- **Filter param map:** same as axis name (`classification` →
  `classification`, etc.).
- **Term-id field map:** the term-resource records use axis-specific
  id field names (`classificationid`, `galleryid`, `periodid`,
  `techniqueid`, `worktypeid`, `mediumid`, `placeid`), but a generic
  `id` field is also always present. The adapter reads `id` for
  uniformity.
- **Term ordering:** for axes whose vocabulary surfaces a populated
  `objectcount` (classification, century, culture, period, place,
  medium, technique, gallery), terms are sorted by count descending.
  For `worktype` and `group` the vocabulary has `objectcount=0`; terms
  are shown alphabetically by `name`. See Q3 / Q4b for the rationale
  and the per-term-probe alternative.
- **Label-length filter:** terms whose `name` exceeds 32 chars are
  dropped at enumeration time (33-byte playset identifier slot).
  Affects `period` (~22 % of top-100), `gallery` (~27 %),
  `technique` (~2 %), `group` (unmeasured at full vocabulary; some
  long curatorial labels expected); others 0 %.
- **Image-permission gate:** every `/object` listing call MUST include
  `q=imagepermissionlevel:0` to suppress permission-restricted records
  that would otherwise come back with empty `primaryimageurl`.
- **Listing endpoint:**
  `GET /object?apikey={KEY}&size=100&page=N&hasimage=1&q=imagepermissionlevel:0`
  `&{axis}={term_id}&sort=id&sortorder=asc&fields=id,objectnumber,title,dated,people,primaryimageurl,images`
- **IIIF URL:** `https://nrs.harvard.edu/{iiif_key}/full/!720,720/0/default.jpg`
  (resolves to `ids.lib.harvard.edu` via a single 303).
- **`iiif_key` value:** the URN portion of `images[0].baseimageurl` —
  the substring after `https://nrs.harvard.edu/` (typically
  `urn-3:HUAM:NNNN_dynmc`, 17–26 chars).
- **`extension`:** always 3 (jpg).
- **Resolve hook:** none. URN → IDS is one 303 hop handled in the
  downloader's redirect shim (shared with Rijks).
- **Rate limit:** **2 500 req/day per API key**. Per-user with BYOK,
  so the budget belongs entirely to one user/device. No `Retry-After`
  headers; engage default 60 s cooldown on a 429 (matches AIC/Rijks).
```

## Go / no-go for Stage 3

**Status: GO.**

Every R item passes. The key-management strategy is locked
(see [Decision](#decision--byok-bring-your-own-key)); future-verification
items below are not blockers.

### Decision — BYOK (bring-your-own-key)

No API key is shipped with the firmware. Each user registers for a
free HAM API key and enters it in `settings.html` under a new "Museums"
section. Mirrors the Giphy key-management model.

**Implications for M1:**

- New NVS setting `ham_api_key` (string, max ~40 chars to fit a UUID
  with padding). Cleared by default.
- New section in `webui/settings.html` with the input field plus a
  short hint linking to HAM's key-request page.
- New REST endpoint `GET /api/museum/ham/key` (LAN-only by virtue of
  the device's HTTP server scope) so the browser-side adapter can
  read the saved key.
  - **Or** the playset editor includes the key in the page-render
    fetch alongside the existing settings load. Implementation
    detail; either works.
- **Browse modal empty-state**: when the saved key is empty, the HAM
  entry in the museum picker is either hidden or shows a hint:
  "Enter your HAM API key in Settings to browse Harvard Art Museums."
- **Refresh skip when empty**: `art_institution_ham_refresh_channel()`
  returns `ESP_ERR_INVALID_STATE` (or similar) with an `ESP_LOGI`
  line if the key is empty; dispatcher treats it like a transient
  no-op (no orphan eviction, no `last_refresh` update). When the user
  later enters a key, the next dispatcher tick picks up the channel.
- **Channel persistence semantics**: a HAM channel saved in a playset
  remains valid across reboots / firmware updates even when the key
  is cleared — only its refresh path is dormant. Re-entering the key
  reactivates it without re-saving the playset.
- **2 500 req/day quota** is no longer a shared-budget concern, but
  the firmware still respects 429s from any single user blowing
  their own quota: existing rate-limit cooldown infrastructure
  applies unchanged.

### Future verification

The following can't be answered from a single Stage 2 run; flagged so
M1 doesn't accidentally regress on them:

- **Week-over-week ID stability.** Re-fetch page 50 with `sort=id` after
  a real refresh cycle (≥ 1 week) and confirm `info.totalrecords` and
  the first-id sequence haven't drifted. Drift would churn the orphan
  eviction.
- **Pagination beyond 500 k window.** Will not impact p3a's normal
  range but worth re-checking if the channel-offset ceiling changes
  in some future feature.
- **TLS handshake from ESP32-P4.** All three CDNs use mainstream roots,
  but ESP-IDF mbedTLS occasionally trips on specific cipher policies.
  Verified during M1's manual gate per the existing testing approach.

## What's next

Stage 3 deliverables — when scheduled — would mirror the existing
M1/M2 pattern (`docs/art-institutions/finalized-design.md` §14):

1. **C side:** new `museums/ham.c` adapter with `refresh_channel` and
   `build_iiif_url`; new museum enum value
   `ART_INSTITUTION_MUSEUM_HAM = 5`; `Kconfig` for the shipped API
   key.
2. **Web UI:** new `webui/museum/ham.js` adapter. Shape mirrors
   `vam.js` for `listArtworks` / `previewUrl` / `thumbnailUrl`, but
   the `axes` and `listCollections` methods implement the hybrid
   discovery described above (compile-time display-label map +
   skip-list; runtime term enumeration with label-length filtering).
   Expected size: ≈ 250 lines.
3. **Manual gate:** 24-hour soak with channels spanning the discovery
   shape: at least one count-ranked axis (e.g. HAM · Classifications ·
   Photographs), one alphabetical-ordered axis (e.g. HAM · Work types ·
   _some-term_ or HAM · Groups · _some-group_), and one with active
   label-length filtering (e.g. HAM · Periods · _some-period_). This
   exercises every code path the hybrid discovery introduces.
