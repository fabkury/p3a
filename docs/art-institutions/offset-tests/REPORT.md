# Art-institution API offset capabilities — investigation report

- **Date:** 2026-05-12
- **Author:** Claude (Opus 4.7) for pub@kury.dev
- **Purpose:** Feasibility study for an upcoming per-channel-per-playset
  *channel offset* feature on art-institution channels. Establishes
  what each museum API actually supports before any design work begins.
- **Method:** Empirical probes against live APIs, cross-referenced
  against AIC's open-source server config. Helper scripts are
  preserved in this directory (`probe_aic.py`,
  `probe_aic_403_boundary.py`, `probe_aic_escape_hatches.py`,
  `probe_aic_bool_partition.py`, `probe_vam.py`, `probe_rijks.py`)
  along with their captured outputs (`*_results.txt`).

## TL;DR

| Museum | Offset supported? | Practical cap | Notes |
|---|---|---|---|
| **AIC** | Yes for shallow offsets via `from=N&size=M`; deep offsets only via **bool+range partitioning over POST DSL** (see §1.7) | `from + size ≤ 1000` per query (hard, server-side) | The 10 000 cap in AIC's own docs is the **authenticated** cap; **unauthenticated/public users hit 1 000** (confirmed in their open-source [config](https://github.com/art-institute-of-chicago/data-aggregator/blob/master/config/aic.php)). No public auth path exists. The bool+range escape hatch lets you cover an arbitrarily large facet by partitioning the ID space into <1 000-record buckets. |
| **V&A** | Yes — via `page=N&page_size=M` only | `page × page_size ≤ 10 000` (page_size ≤ 100) | Random-access pagination works. Deeper than 10 000 returns 500. No `start=`, `from=`, `cursor=`, `offset=`, or `sort=` (all silently ignored — they don't break, they just don't take effect). |
| **Rijksmuseum** | **No.** Cursor-walk only. | n/a | Every non-`pageToken` query parameter returns 400. Page size is fixed at 100. The `pageToken` is `base64({"token": "<HMO_URL>"})` and means "items strictly after this HMO" — useless as a random-access offset unless you already know an HMO id at the desired position. |

**Takeaway for the feature design:** offset works on AIC (within a tight
1 000-per-query budget that can be sidestepped by partitioning) and
V&A (broadly, up to offset 9 900). Rijks does **not** support true
offset queries; to honor a non-zero offset on Rijks the firmware would
have to walk `offset/100` extra pages on every refresh — feasible but
with a per-page HTTP cost.

## Decision (2026-05-12)

The channel-offset feature **will be implemented**, but not in this
work cycle. The chosen approach is recorded here so the next pass
doesn't have to re-derive it.

| Museum | Chosen approach for `channel_offset` | Notes |
|---|---|---|
| **AIC** | **Bool+range partitioning over POST /artworks/search.** Browse UI probes the term's ID distribution, splits it recursively until every bucket is `< 1 000` records, and bakes bucket boundaries into the channel spec (or recomputes them on each refresh). The C-side refresh walks one bucket at a time, ordered by id ascending, with `from + size ≤ 1 000` per query. Offset within a bucket is honored directly via `from=N`. | Confirmed working end-to-end (§1.7). Pivots the refresh model from one walk per term to N walks per term. Increases HTTP volume and refresh latency in proportion to bucket count (~4-8 for the largest facets). No public auth path exists — the 1 000 cap cannot be lifted, only partitioned around. |
| **V&A** | **Existing `?page=N&page_size=100` path with a non-1 starting page.** No new query shape needed; only the starting `page` value changes. | Trivial. Cap `offset + cache_size ≤ 10 000` at channel-creation time. |
| **Rijks** | **Walk `⌈offset/100⌉` discarded pages at the start of each refresh.** Linked-Art cursors are not synthesizable for arbitrary offsets (§3.3). Optional optimization: cache the resolved `pageToken` for `(set, offset)` pairs in the channel sidecar; invalidate on Rijks set version change (not signalled cleanly, so a TTL-based invalidation is the pragmatic choice). | Acceptable cost for offsets up to a few hundred (~5 s extra); becomes painful past offset 5 000 (~25 s). The largest curated Rijks set encountered in this study had 6 740 items, so offsets in the low thousands are realistic. The token-caching optimization is the deferred follow-up if field experience says the bare walk-cost is too high. |

**Open design questions still to answer at implementation time:**

1. Free integer for `channel_offset` vs. a small ordinal choice
   (0, 100, 250, 500, 1 000). Ordinal is simpler in the UI; free
   integer is more flexible.
2. UX when a user picks an offset that exceeds the museum's cap:
   hard block at channel-creation, soft warning, or silent clamp.
3. For AIC specifically: precompute bucket boundaries at browse time
   (stable, persisted in the channel spec) vs. recompute on each
   refresh (resilient to dataset growth, slightly more browse-time
   probing).
4. Whether the AIC bucket boundaries should be persisted in the
   `playset_store.c` channel spec (which would need a new field) or
   in a sidecar file under the channel's cache directory (no
   playset-format change required).
5. Whether to also implement the Rijks `pageToken` cache optimization
   in v1 or wait for field evidence that the bare walk-cost is too
   high.

## 1. Art Institute of Chicago (AIC)

### 1.1 What works

- `?from=N&size=M` — the documented Elasticsearch-style search params.
  Returns the exact same results as `?page=K&limit=M` where
  `K = N/M + 1`. AIC's pagination block even cross-translates: a
  request with `from=10` reports `current_page: 2` in the response.
- `?page=N&limit=M` — equivalent path. `from` and `page` are
  interchangeable for the firmware's purposes.
- `?sort=id` — produces a deterministic order (otherwise the order
  appears stable across requests but is not formally specified).
- `?fields=id,title,image_id,...` — works as documented.

### 1.2 What does NOT work

- `?offset=N` — silently ignored. The response comes back with
  `offset: 0` in the pagination block.
- `?search_after=...` — both as a query parameter and inside a
  POST body. Silently ignored; response returns page 1.
- Any `from + size > 1000` query — returns **HTTP 403** with body:
  ```
  {"status": 403, "error": "Invalid number of results",
   "detail": "You have requested too many results. Please refine your parameters."}
  ```
- Any `size > 100` query — returns **HTTP 403** with body:
  ```
  {"status": 403, "error": "Invalid limit",
   "detail": "You have requested too many resources per page. Please set a smaller limit."}
  ```
  (Note: distinct error message from the offset cap.)

### 1.3 The 1000-cap is exact, hard, and independent of facet size

| Query | `from + size` | Status |
|---|---|---|
| `from=900 size=100` | 1000 | 200 |
| `from=901 size=100` | 1001 | 403 |
| `from=999 size=1` | 1000 | 200 |
| `from=1000 size=1` | 1001 | 403 |
| `from=5000 size=1` | 5001 | 403 |
| `from=9999 size=1` | 10000 | 403 |
| `from=500 size=500` | 1000 | 403 (size>100 — different error) |
| `from=0 size=1000` | 1000 | 403 (size>100 — different error) |

The exact rule is **`from + size ≤ 1000`**, with `size ≤ 100`. This
holds across:

- Tiny facets (PC-4 department, 2 195 records — same cap).
- Big facets (artwork_type=1 Painting, 3 794 records — same cap).
- Unfiltered search (`/artworks/search` with no facet — same cap).
- Both `?from=N` and `?page=N&limit=M` paths.
- With and without `sort=id`.

### 1.4 The listing endpoint is a different animal

The plain `/artworks` listing endpoint has **no** offset cap:

| Query | Status | Result |
|---|---|---|
| `/artworks?page=11&limit=100` | 200 | items at offset 1000 |
| `/artworks?page=100&limit=100` | 200 | items at offset 9 900 |
| `/artworks?page=101&limit=100` | 200 | items at offset 10 000 |
| `/artworks?page=999&limit=100` | 200 | items at offset 99 800 |

But the listing endpoint doesn't accept facet filters
(`query[term][...]`), so it cannot substitute for `/artworks/search`
in the firmware's workflow — every saved channel is bound to a facet
term.

### 1.5 Why the cap is 1 000 and not 10 000 — the source of truth

AIC's data-aggregator is open source. The auth config
([`config/aic.php`](https://github.com/art-institute-of-chicago/data-aggregator/blob/master/config/aic.php))
sets the resource caps explicitly:

```php
'auth' => [
    'restricted' => env('APP_RESTRICTED', true),
    'max_resources_guest' => 1000,
    'max_resources_user'  => 10000,
    'login_whitelist_ips' => [...],
    'access_whitelist_ips' => [...],
],
```

And the search request validator
([`app/Http/Search/Request.php`](https://github.com/art-institute-of-chicago/data-aggregator/blob/master/app/Http/Search/Request.php),
lines ~495-500) picks the cap based on the auth gate:

```php
if (isset($size) && isset($from)) {
    if (Gate::allows('restricted-access')) {
        $maxResources = config('aic.auth.max_resources_user');   // 10000
    } else {
        $maxResources = config('aic.auth.max_resources_guest');  // 1000
    }
```

The `restricted-access` gate
([`app/Providers/AuthServiceProvider.php`](https://github.com/art-institute-of-chicago/data-aggregator/blob/master/app/Providers/AuthServiceProvider.php))
admits requests on three paths:

1. `aic.auth.restricted` is globally false (off for staging/dev only).
2. `Auth::check()` returns true — i.e. a valid Laravel auth token.
3. The client's IP is in the `access_whitelist_ips` list.

**None of these are accessible to public API consumers.** AIC does
not expose a self-serve token endpoint; tried `/auth`, `/login`,
`/register`, `/me`, `/access` — all 404 or 403. A bogus
`Authorization: Bearer ...` header is silently accepted but the
guard still treats the caller as guest. The 10 000 cap is reserved
for AIC's internal/partner users on whitelisted networks.

So the 1 000 number is **what the server enforces against everyone
on the open internet**, including the firmware. Every published guide
that quotes 10 000 is implicitly assuming auth.

### 1.6 Implication for the channel-offset feature (without escape hatch)

The firmware's default `ai_cache_size` is **1024**. Even with offset=0
the firmware already breaches the 1000-cap whenever the channel's
total population is large enough to fill the cache — which is exactly
why the current code treats a 403 after at least one merged page as a
partial-success terminal state (`finalized-design.md` §15.1).

So for AIC the picture is:

- **Practical maximum offset = 999** (with one 1-record fetch).
- **Maximum offset that still lets us pull a full 100-record page =
  900** (one page of 100, ending at index 999).
- **Cumulative offset+cache_size budget = 1000.** If `offset = X`,
  then realistic `cache_size` for this channel is at most `1000 - X`.
- The 10 000-record claim in `finalized-design.md` §9.1
  ("Pagination cap: AIC's Elasticsearch cap is 10 000 records") is
  **incorrect** for the search endpoint. The doc note should be
  updated.

**The decision (Decision table above) chose option 2 — bool+range
partitioning, verified in §1.7.** Options 1 and 3 are documented in
the git history of this file for completeness but are not the plan
of record.

### 1.7 The bool+range partitioning escape hatch

The 1 000-cap applies to a **single query**'s `from + size`. It does
**not** prevent issuing multiple narrower queries that each fit under
the cap.

AIC's POST endpoint accepts a partial Elasticsearch DSL body. The
following shape works:

```json
{
  "query": {
    "bool": {
      "must":   [{ "term":  { "artwork_type_id": 1 } }],
      "filter": [{ "range": { "id": { "gte": 200000, "lt": 300000 } } }]
    }
  },
  "sort": [{ "id": "asc" }],
  "from": 0, "size": 100,
  "fields": ["id"]
}
```

If a bucket's record count is < 1 000, you can fully traverse it
(`from + size ≤ count ≤ 1000`). Stitching all buckets together
recovers the full facet.

**Verified end-to-end on Painting (artwork_type_id=1, 3 794 records):**

| ID range | Painting records |
|---|---|
| `[0, 10 000)` | 157 |
| `[10 000, 30 000)` | 542 |
| `[30 000, 60 000)` | 523 |
| `[60 000, 100 000)` | 720 |
| `[100 000, 150 000)` | 618 |
| `[150 000, 200 000)` | 347 |
| `[200 000, 300 000)` | 887 |
| **Sum** | **3 794** ✓ |

The largest bucket (887 records) was traversed fully — 9 pages of
100 each — without hitting the cap.

**Caveats:**

- The cap stays at 1 000 *per query*; deep offsets within an
  oversized bucket still 403. So bucket boundaries must produce
  <1 000-record sub-results. The firmware (or browse UI) needs an
  algorithm that probes counts and subdivides recursively until
  every bucket is <1 000. Easy enough; not free.
- Buckets are not uniform-size — the distribution above is
  empirical. For terms with denser ID ranges (e.g. recent acquisitions
  clustered in `[200000, 300000)`) some buckets fail this constraint
  on the first try and need to be split.
- This pivots the firmware refresh model from "one walk per term" to
  "N walks per term" where N is the bucket count. For the median AIC
  term (likely <1 000 records) N=1. For Painting and the larger
  artwork-types it's 4-8. The download manager doesn't care since it
  consumes from `Ci`; the refresh loop is where the change lands.

**Other escape hatches tested and ruled out:**

| Hatch | Result |
|---|---|
| `?search_after=...` (GET) | Silently ignored; returns page 1. |
| `search_after` in POST body | Silently ignored; returns page 1. |
| `scroll` parameter in POST body | Silently ignored; no `_scroll_id` returned. |
| `aggregations` (`aggs`) in POST body | Accepted but the response only contains pagination — aggs are stripped. |
| `query[range][...]` in GET URL alongside `query[term][...]` | 400 "[term] malformed query"; URL-encoded compound queries can't be expressed (the path takes a flat `query` object). Range alone works. |
| IIIF Collection / Presentation manifests | `/iiif/2/collection`, `/iiif/3/collection` → 403. Per-artwork manifest at `/api/v1/artworks/{id}/manifest.json` works but is single-artwork only. |
| OAI-PMH (`/oai`, `/api/v1/oai`, `/api/oai`, etc.) | 404 across the API host; 403 on the public www host (Drupal/CDN error pages). AIC does not expose OAI-PMH publicly. |
| `Authorization: Bearer <token>` header | Bogus tokens silently accepted but the auth gate still treats the request as guest. No public token issuance endpoint (`/register`, `/auth`, `/login`, `/me` all 404). |
| Public `/artworks` listing endpoint | No 1 000 cap (tested to page 999, offset 99 800). **But does not accept facet filters.** Not a substitute for `/artworks/search`. |
| Data dump (S3) | AIC's [`api-data`](https://github.com/art-institute-of-chicago/api-data) repo. Full ~2.5 GB extracted. Official AIC recommendation for >10 000 records. Out of scope to put on the device; could be mirrored to our own server in a future v2. |

### 1.8 Verification scripts

- `probe_aic.py` — initial survey of AIC offset behavior
- `probe_aic_403_boundary.py` — boundary mapping sweep
- `probe_aic_escape_hatches.py` — first pass at range queries, POST
  DSL bodies, IIIF endpoints, OAI-PMH discovery
- `probe_aic_bool_partition.py` — exhaustive verification of the
  bool+range partitioning approach, plus auth/token probing

## 2. Victoria and Albert Museum (V&A)

### 2.1 What works

- `?page=N&page_size=M&id_category=THES48903` (and the analogous
  `id_collection` / `id_venue` filter params). Random-access — you
  can ask directly for page 100 without first walking pages 1-99.
- Order is stable between repeated identical requests.
- `images_exist=1` filter.

### 2.2 What does NOT work

- `?start=N` — Solr-style cursor parameter. Silently ignored.
- `?from=N` — silently ignored.
- `?offset=N` — silently ignored.
- `?cursor=*` — silently ignored.
- `?sort=systemNumber` / `?sort=_score` / `?sort=+systemNumber` —
  silently ignored. The response returns the same order regardless.
- `?page_size=200` (or any `page_size > 100`) — returns **HTTP 422**:
  ```
  {"detail":[{"loc":["query","page_size"],
              "msg":"ensure this value is less than or equal to 100",
              "type":"value_error.number.not_le",
              "ctx":{"limit_value":100}}]}
  ```
- Any `page × page_size > 10 000` (e.g. `page=200&page_size=100`) —
  returns **HTTP 500 Internal Server Error** (no helpful body).

### 2.3 The 10 000-cap

The cap is uniform regardless of facet size:

| Filter (record_count) | page=100 ps=100 (offset 9 900) | page=200 ps=100 (offset 19 900) |
|---|---|---|
| `THES48903` Prints (104 472) | 200 | 500 |

Tested up through page=5 000 (offset 499 900) — all return 500.
There is no 422 / 4xx body and no `Retry-After` — V&A's server
treats this as a programming error rather than user error, but in
practice the behavior is a hard wall at offset 10 000.

### 2.4 Implication for the channel-offset feature

V&A is the **friendliest of the three** for an offset feature:

- **Practical maximum offset = 9 900** (last full page).
- **Maximum offset that still lets us pull a full page_size=100 page
  = 9 900.**
- **Cumulative offset+cache_size budget = 10 000.** If `offset = X`,
  then `cache_size ≤ 10 000 - X`. The firmware's default cache_size
  of 1024 fits comfortably as long as `offset ≤ 8 976`.

So V&A supports offset essentially for free, as long as we cap the UI
and the firmware to keep `page × page_size ≤ 10 000`. The firmware's
existing `?page=N&page_size=100` path needs no change — only the
starting `page` value gets a non-1 default.

### 2.5 Verification script

- `probe_vam.py`

## 3. Rijksmuseum

### 3.1 What works

- The forward cursor walk via `pageToken` (returned in the `next`
  field of each `OrderedCollectionPage`). This is the only navigation
  path.
- The token is the base64 of a JSON object:
  ```json
  {"token": "https://id.rijksmuseum.nl/200112727"}
  ```
  It encodes the HMO URL of the **last** item on the previous page;
  the server returns items whose HMO URL is strictly greater than the
  token's HMO. (Confirmed by synthesizing a token from a known HMO
  and observing that the response begins with the next-greater HMO.)

### 3.2 What does NOT work — every common offset parameter

All of the following return **HTTP 400 `Unsupported query parameter:
<name>`**:

`from`, `offset`, `page`, `pageNumber`, `startIndex`, `start`, `ps`,
`pageSize`, `pageLimit`, `limit`, `maxItems`, `itemsPerPage`, `p`,
`skip`, `cursor`.

`pageToken` is accepted, but only as a literal cursor — passing
`pageToken=100` returns 400 too (it requires a properly base64-encoded
JSON envelope with a `token` field).

### 3.3 The token is not synthesizable for arbitrary offsets

Passing a synthesized token whose `token` field references a
**non-existent** small HMO id (e.g. `200000000`) returns 100 items as
if the cursor were at the beginning. This makes sense — "items
greater than 200000000" matches the whole set when no real HMO is
below that number. But the inverse — "items at position 1000" —
cannot be expressed because we don't know which HMO sits at
position 1000 without walking. The cursor is fundamentally a
**bookmark**, not an ordinal offset.

### 3.4 Legacy API is gone

The historical `https://www.rijksmuseum.nl/api/en/collection?p=N&ps=M`
endpoint returns **HTTP 410 Gone**. It cannot be used as a fallback.

### 3.5 Implication for the channel-offset feature

For Rijks, supporting a `channel_offset = X` setting requires the
firmware to **walk `⌈X / 100⌉` extra HTTP requests** at the start of
every refresh, discarding the results, before it starts merging
entries into the cache. At ~500 ms per request including TLS
handshake and JSON-LD parsing, this works out to:

| Offset | Extra walk HTTP calls | Approx. extra refresh time |
|---|---|---|
| 0 | 0 | 0 |
| 500 | 5 | ~2.5 s |
| 1 000 | 10 | ~5 s |
| 5 000 | 50 | ~25 s |
| 10 000 | 100 | ~50 s |

The total record counts of the largest Rijks sets matter here. The
set used in this study (`261231`) had `totalItems: 6 740`, so an
offset of 5 000 already covers most of the set. We did not enumerate
every Rijks set in this study — that would require parsing the
`webui/museum/rijks-sets.json` baked into the LittleFS image — but
the Rijks `/sets` corpus has many small sets and a few thousand-record
ones.

**Bottom line:** Rijks offset is feasible by walking but expensive,
and the walk cost compounds with `ai_refresh_sec` — every refresh
pays it again. Caching the resolved `pageToken` for a given (set,
offset) pair after the first walk would amortize the cost across
refreshes, but the cursor moves whenever Rijks reindexes its set,
so the bookmark is unstable in the worst case.

### 3.6 Verification script

- `probe_rijks.py`

## 4. Cross-cutting observations

### 4.1 Order stability

All three APIs return a stable order across repeated identical
requests (tested for V&A explicitly; observed empirically for AIC
and Rijks during the cursor walk). Without that guarantee, slicing
by offset would produce different artworks on every refresh —
which is the worst possible UX for a museum channel ("why does my
'Picasso at offset 200' channel keep changing?").

However, none of the three documents the order — it is just an
empirical artifact of the backend's default sort. A future
documentation pass or rebuild on any of them could change the
order. The feature design should treat order as **stable in
practice but not contractual** and not rely on offset stability
across long time horizons (e.g. months between cache rebuilds).

### 4.2 Page-size ceilings

| Museum | Max `size` / `page_size` / `limit` |
|---|---|
| AIC | 100 (search and listing) |
| V&A | 100 |
| Rijks | 100 (fixed; not configurable) |

100 is the universal ceiling. No museum permits a single large page
fetch as a shortcut to "everything past offset N".

### 4.3 Why the current firmware doesn't already feel the AIC cap

The firmware caps `ai_cache_size` at 1024 and walks pages 1..11
(100 entries per page). Pages 1-10 succeed (offset 0-999); page 11
fails. The existing field-observed fix in §15.1 treats a 403 after
≥1 merged page as partial success, so the channel ends up with ~1000
entries and works fine. **This works because offset is implicitly 0.**
A non-zero offset breaks the assumption: pages would 403 from the
start, the partial-success heuristic wouldn't kick in (no prior
pages merged), and the refresh would fail.

The feature implementation therefore needs to teach the partial-success
logic that a 403 from the very first page on AIC, when offset > 0,
should be treated as "this slice exceeds the AIC cap" — distinct from
"this slice is invalid" — and presumably surface the limit to the
user during channel creation.

## 5. Open questions for the design phase

The high-level approach is decided (see **Decision** section near
the top). What remains is implementation-detail design:

1. Free integer for `channel_offset` vs. small ordinal choice
   (0, 100, 250, 500, 1 000). Ordinal is simpler in the UI; free
   integer is more flexible.
2. UX when a user picks an offset that exceeds the museum's cap:
   hard block at channel-creation, soft warning, or silent clamp.
3. AIC bucket boundaries: precompute at browse time (stable,
   baked into the channel spec) vs. recompute each refresh
   (resilient to dataset growth).
4. Where AIC bucket boundaries live: new field in `playset_store.c`
   (v12 playset format) vs. sidecar file under the channel's cache
   directory (no playset-format change).
5. Rijks `pageToken` caching: v1 or wait for field evidence?

(A sixth question — whether to correct `finalized-design.md` §9.1's
10 000-record cap claim before the feature pass — was resolved
2026-05-12: §9.1 and §15.1 have been patched to reflect the real
1 000-cap and to cross-reference this report.)

## 6. Helper-script index

All scripts live in this directory and are reusable; they take no
arguments and emit human-readable trace to stdout. Captured outputs
are next to them.

| Script | What it does | Captured output |
|---|---|---|
| `probe_aic.py` | Initial survey of AIC offset behavior across `page` / `from` / `offset` / `size` / `limit` against PC-4 department and Painting artwork-type. | `aic_results.txt` |
| `probe_aic_403_boundary.py` | Sweeps page numbers and `from` offsets on Painting to find the exact 403 boundary (`from + size ≤ 1 000`). | `aic_boundary_results.txt` |
| `probe_aic_escape_hatches.py` | First pass at the four escape hatches: GET-side `query[range]` (broken on URL syntax), POST DSL bodies (`search_after`, `scroll`, `aggs`, `bool+range`), IIIF Collection / Presentation manifests, OAI-PMH discovery. | `aic_hatches_results.txt` |
| `probe_aic_bool_partition.py` | Exhaustive verification of the **decided approach**: probes the Painting ID distribution into <1 000-record buckets, traverses the largest bucket fully, confirms the cap is per-query (not per-result-set), revisits OAI-PMH after the previous script's DNS hiccup, probes for an authentication path (`Authorization: Bearer`, `/auth`, `/login`, `/register`, `/me`, `/access`). | `aic_partition_results.txt` |
| `probe_vam.py` | Surveys V&A `id_category=THES48903` (Prints) for `page` / `start` / `from` / `offset` / `cursor` / `sort`, plus deep-page and page-size limits, order stability. | `vam_results.txt` |
| `probe_rijks.py` | Walks the cursor on Rijks set `261231`, sweeps direct-offset parameter names (every common spelling returns 400), tests token synthesis (cursor is a bookmark, not an ordinal), and verifies the legacy API is gone (410). | `rijks_results.txt` |

All scripts are intentionally polite (1.0-1.5 s spacing between
requests). Re-running any of them is safe but takes a couple of
minutes for the longer ones.
