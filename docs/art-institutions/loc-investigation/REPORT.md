# Library of Congress — API investigation report

*Investigation performed 2026-05-13 against the live LoC JSON API and IIIF
service. All probe scripts and raw JSON dumps live alongside this file.*

## Headline findings

1. **Anonymous fetches work** with a polite `User-Agent` header. No 403s
   like the museum-art research notes flagged.
2. **The `iiif_key` 48-char slot is the binding constraint.** IIIF
   identifier length is bimodal: 27–47 chars (image-rich "service" and
   "public" subtrees) or 67–123 chars (newspapers, music). Whether an
   item fits depends entirely on which subtree of LoC's storage it lives
   in, not on the surface facet (format / collection).
3. **The listing JSON does not always surface IIIF URLs.** Many items
   return only a `_150px.jpg` thumbnail in `image_url`. The IIIF service
   exists for some of those items but not all — there is no reliable
   way to tell from the listing alone.
4. **Two query surfaces exist for the same data**: `fa=<field>:<value>`
   query parameters and `/{path}/`-prefixed URLs. Both rewrite to the
   same canonical search and are equivalent.

## API surface — what works

| Capability | Endpoint | Notes |
|---|---|---|
| Site-wide search | `GET /search/?fo=json` | Returns paginated results envelope. |
| Format-filtered search | `GET /search/?fo=json&fa=original-format:<value>` | Or shortcut `/photos/`, `/maps/`, `/newspapers/`. |
| Collection listing | `GET /collections/{slug}/?fo=json` | Equivalent to `fa=partof:<title>`. |
| Item detail | `GET /item/{id}/?fo=json` | Per-item JSON, may include `resources[].files[][]`. |
| Collections index | `GET /collections/?fo=json&c=160&sp=1` | ~640 total collections across 4 pages of 160. |
| IIIF info | `GET https://tile.loc.gov/image-services/iiif/{id}/info.json` | Image API v2 level 2, JPEG/WebP/etc. |
| IIIF size cap | `GET .../{id}/full/!720,720/0/default.jpg` | Returns real 720px JPEG; verified. |
| Facet enumeration | `GET /search/?fo=json&c=1` | Top-level facets list with counts. |

## Quirks the firmware/web UI must accommodate

### Q1 — Facet filter syntax is brittle

LoC's `fa=<field>:<value>` parameter is **silently dropped** unless both
sides match its expected form:

| Tried | Worked? | Outcome |
|---|---|---|
| `fa=original_format:photo` (underscore) | ❌ | Filter dropped, results unfiltered. |
| `fa=original-format:photo` (dash, lowercase singular) | ❌ | `total=1` empty envelope. |
| `fa=original-format:photo, print, drawing` (dash + actual facet title) | ✅ | `total=12,186`. |
| `fa=original-format:Photo, Print, Drawing` (Title Case) | ✅ | Same as above; case-insensitive. |
| `fa=partof:prints and photographs division` | ✅ | `total=11,225`. |
| `fa=partof:prints-and-photographs-division` (dashes) | ❌ | `total=1`. |

**Rule:** field name uses **dashes** (`original-format`, `online-format`,
`partof`); value is the **lowercase display title with spaces**
verbatim (commas, slashes preserved). The web UI must encode the value
exactly — letting `urlencode` turn spaces into `+` and commas into `%2C`
both work.

The verification signal is `facet_trail` in the response: if the trail
contains an entry matching the requested filter, it was applied.

### Q2 — IIIF id lengths are bimodal

Measured on 100-result samples per filter, where the listing surfaces a
IIIF URL at all:

| Filter | Total | with-IIIF in listing | fit_48 of those |
|---|---|---|---|
| `/photos/` = `original-format:photo, print, drawing` | 12,186 | 8 % | 75 % |
| `/maps/` = `original-format:map` | 600 | 97 % | 35 % |
| `partof:prints and photographs division` + `online-format:image` | 11,225 | 0 % | n/a |
| `original-format:manuscript/mixed material` | 4,951 | 28 % | 32 % |
| `original-format:notated music` | 50 | 86 % | 2.3 % |

Two distinct shapes appear in working IIIF ids:

- **Short ids (27–47 chars, fit_48)**: `service:pnp:highsm:35300:35397`,
  `service:gmd:gmd382:g3820:g3820:ct005994`,
  `service:mss:mal:380:3803800:001`, `public:gdc:2018693977:0050000`.
  Pattern: 4–6 colon-separated segments, sourced from
  Prints/Photographs (`pnp:`), General Maps Division (`gmd:`),
  Manuscript/Mixed (`mss:`), digital collections gateway (`gdc:`).
- **Long ids (60–125 chars, won't fit)**: newspaper batches
  (`service:ndnp:lu:batch_lu_connolly_ver01:data:sn86079068:00200299486:1869080701:0111`),
  music silent films
  (`service:music:mussilentfilms:mu:ss:il:en:tf:il:ms:-2:02:05:62:32:6:mussilentfilms-2020562326:mussilentfilms-2020562326_0001`),
  multi-segment audio-folklore archives.

There is **no facet** that cleanly separates these. A given format
filter mixes both. The only reliable filter is **prefix-based on the
IIIF id itself**: items beginning with `service:pnp:`, `service:gmd:`,
`service:mss:` (subset), and `public:gdc:` reliably fit.

### Q3 — Items without IIIF surface only a thumbnail

PPOC results (and many photo-format results) carry only a
`storage-services` thumbnail URL like
`https://tile.loc.gov/storage-services/service/pnp/hlb/00000/00055_150px.jpg`
and no IIIF URL in the listing JSON. Per-item JSON has an empty
`resources[0].files[]` array — these items genuinely have no IIIF
service.

Tested transformations from the storage URL into a candidate IIIF id
(strip `_150px.jpg`, replace `/` with `:`, prepend `service:`) yielded
404s on `info.json` for the Herblock cartoons example. The thumbnail
URL is not a reliable IIIF-id signal.

The only reliable detection of "has IIIF" is to look for a URL starting
with `https://tile.loc.gov/image-services/iiif/` in either
`image_url[]` or `resources[].image`.

### Q4 — Pagination and listing limits

- Page size `c` ∈ {25, 50, 100, 150} per `perpage_options`.
- Page number `sp` is 1-based.
- Pagination metadata: `pagination.total` is the result count;
  `pagination.last` is the last-page URL.
- A search response defaults to `digitized:Available Online` — this
  background filter applies even when no other `fa=` is set, which
  matches what we want (no point indexing non-digital items).
- No documented rate limit. The `User-Agent` is the only required
  header. Periodic read timeouts and `IncompleteRead` errors were
  observed under sustained probing; refresh code needs the same
  retry-with-backoff pattern as the existing museum adapters.

### Q5 — IIIF service capabilities

`info.json` reports IIIF Image API **v2 level 2** compliance on
`tile.loc.gov`. Supported features verified:

- Formats: `jpg, txt, jp2, tif, pdf, gif, png, webp`.
- Sizes: any (`sizeByW`, `sizeByH`, `sizeByPct`, `sizeByForcedWh`,
  `sizeByWh`). `/full/!720,720/0/default.jpg` returns a valid JPEG
  (verified by `ffd8ffe0` magic bytes) at HTTP 200.
- Source resolutions are high (e.g. 8,209×5,526 for a Highsmith print;
  8,575×6,054 for a map; 4,934×6,302 for a manuscript page). Requesting
  720px on the longest side is safe everywhere.
- `sizes_count: 0` — LoC's IIIF does **not** advertise a discrete sizes
  list, just the `supports` array. The firmware uses the same hard-coded
  `!720,720` IIIF size request that other museums use, no
  `info.json`-driven negotiation.

### Q6 — Collection inventory

The `/collections/` index returns ~640 collections across 4 pages of
160 each. Slugs follow `{slug}/about-this-collection/` in the inventory
response; strip the `/about-this-collection` suffix to get the
collection-listing slug (e.g. `civil-war-maps/`).

Counts per collection vary wildly: `civil-war-maps` 94 items,
`baseball-cards` 426 items, large collections like
`abraham-lincoln-papers` have 20K+ items but most are text. Many
collections are not IIIF-bearing at all (text-only).

## Constraints implied for the p3a integration

1. **48-char `iiif_key`** rules out newspapers, music, audio-folklore,
   and most multi-segment items. The channel must filter to IIIF-id
   prefixes that empirically fit: `service:pnp:`, `service:gmd:`,
   `service:mss:`, `public:gdc:`.

2. **No reliable per-result "has IIIF" facet.** The refresh adapter
   must filter items where `image_url[]` contains a
   `tile.loc.gov/image-services/iiif/` URL (same logic the existing
   `pick_iiif_from_result` in our probe scripts uses). Items without
   IIIF in the listing are dropped silently.

3. **Both facet-keyed and path-based queries are valid surfaces** for
   the same data. Internal code can pick whichever is easier to
   construct.

4. **Image-bearing yield is low for some facets** (8% for the photo
   format). Refresh code that iterates pages until cache is full may
   issue 10× the requests vs. AIC. Throttling at 200ms between pages
   (same as other museums) keeps us within polite limits.

5. **The `partof:prints and photographs division` facet does not
   surface IIIF in listings.** It cannot be used as a productive
   refresh axis — only formats that surface IIIF URLs directly
   (`photo, print, drawing`, `map`, `manuscript/mixed material`,
   selected collections) can.

## Recommended scope (for design discussion)

Given the constraints, the channel should expose only the subset of
LoC that:

1. Has a high enough IIIF surface rate to make refresh productive.
2. Yields IIIF ids that fit in 48 chars at a meaningful percentage.

The cleanest options:

- **Format axis**: `original-format` filter, but limited to format
  values that empirically yield IIIF (`photo, print, drawing`, `map`,
  selected others). Browser-side adapter filters out non-IIIF results
  before showing the preview; firmware does the same during refresh.
- **Curated-collection axis**: a baked-in allowlist of LoC collection
  slugs (similar to the `rijks-sets.json` baked into the LittleFS
  image) that are known to be image-rich and IIIF-bearing. The browser
  picks from the list, fetches counts on demand, and presents them as
  the "terms" inside the axis.
- **IIIF-prefix filter at refresh time**: regardless of axis, the
  C-side refresh drops any entry whose IIIF id doesn't start with one
  of the known-short prefixes.

A "free-form keyword search" axis is out of scope for v1 (consistent
with the existing museum channels), even though LoC supports `q=`
search natively.

## Per-probe artifacts

- `probe_loc_basic.py` → `output/basic.md` + `output/raw/01_*..07_*.json`
- `probe_loc_detail.py` → broken mid-run, partial data in `output/raw/A_*.json`
- `probe_loc_focused.py` → `output/focused.md` + `output/raw/Q1..Q5_*.json`
- `probe_loc_facets.py` → `output/raw/fa_*..coll_*..path_*.json`
- `probe_loc_final.py` → `output/final.md` + `output/raw/F1..F4_*.json`
- `probe_loc_iiif_derive.py` → `output/iiif_derive.md` (verified the
  storage-URL → IIIF-id transformation does **not** work in general)
- `probe_loc_item_iiif.py` → `output/item_iiif.md` (per-item JSON shape)
