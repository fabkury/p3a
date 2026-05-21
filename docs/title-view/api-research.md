# Title-view: per-source API research

This document captures the per-source mechanism for fetching the *title* of a single
artwork given the identifier the firmware exposes for the currently playing post.
It is the up-front research deliverable for the title-view feature (eye-emoji button
between Pause and Swap-Next that reveals the current artwork's title on demand).

All requests are issued **browser-direct** from the p3a web UI; the firmware does not
proxy them. Where an API key is required, the browser reads it from `GET /config`
(the same path `webui/settings.html` already uses to populate the Settings tab —
see `webui/settings.html:807` for `giphy_api_key` and `webui/settings.html:887` for
`ham_api_key`). No new key-leaking mechanism is introduced.

If a source's API blocks cross-origin requests, the source is **dropped from the
feature** (its title button stays hidden, same as SD-card). The plan explicitly
disallows a firmware proxy fallback.

## Firmware-exposed identifier for each source

The starting point for every lookup is whatever the firmware already publishes in
the `current_artwork` JSON (`build_current_artwork_json()` in
`components/http_api/http_api_rest_playsets.c:69`). The title-view work extends that
JSON to expose three more fields:

| Source        | New field exposed              | Notes                                                                   |
|---------------|--------------------------------|-------------------------------------------------------------------------|
| Giphy         | already has `giphy_id`         | unchanged                                                               |
| Institution   | `museum_id` + `iiif_key`       | parsed from `channel_spec_name` and `artwork.storage_key`               |
| Makapix       | `storage_key`                  | already used to build the vault URL — just emit as a top-level field    |
| SD-card       | (none)                         | title button stays hidden                                               |

## Per-source endpoint table

| Source     | Lookup endpoint                                                                                                   | Title JSON path                  | Auth                              | CORS    | Rate limit            |
|------------|-------------------------------------------------------------------------------------------------------------------|----------------------------------|-----------------------------------|---------|------------------------|
| Giphy      | `GET https://api.giphy.com/v1/gifs/{giphy_id}?api_key={key}`                                                      | `data.title`                     | `api_key` query (from `/config`)  | yes     | ~100 req/hr beta tier |
| AIC        | `GET https://api.artic.edu/api/v1/artworks/search?query[term][image_id]={iiif_key}&fields=id,title&limit=1`        | `data[0].title`                  | `AIC-User-Agent` header           | yes     | 60 req/min per IP     |
| Rijks      | `GET https://id.rijksmuseum.nl/{hmo}` (Accept: `application/ld+json`)                                              | first `Name` in `identified_by[]` | none                              | yes     | (no documented limit) |
| V&A        | `GET https://api.vam.ac.uk/v2/objects/search?q={iiif_key}&page_size=1` (verify during impl — see §V&A below)       | `records[0]._primaryTitle`       | none                              | yes     | (no documented limit) |
| Wellcome   | `GET https://api.wellcomecollection.org/catalogue/v2/works?query={vid}&pageSize=1` (verify — see §Wellcome below)  | `results[0].title`               | none                              | yes     | (no documented limit) |
| SMK        | `GET https://api.smk.dk/api/v1/art/search?keys={filename}&rows=1` (verify — see §SMK below)                        | `items[0].titles[0].title`       | none                              | yes     | (no documented limit) |
| HAM        | `GET https://api.harvardartmuseums.org/object?apikey={key}&q=primaryimageurl:*{urn}*&size=1&fields=id,title`       | `records[0].title`               | `apikey` query (from `/config`)   | yes     | 2500 req/day per key  |
| Makapix    | `GET https://makapix.club/api/post/{storage_key}` (Accept: `application/json`)                                     | `title`                          | none                              | **TBD** | view-event/call       |

"CORS yes" entries are empirically confirmed because the existing
`webui/museum/*.js` browse adapters already fetch from those hosts. Makapix CORS is
**unverified at plan time** — the browser does not currently call `makapix.club`
endpoints; if a smoke test from the browser console (`fetch('https://makapix.club/api/post/...')`)
returns a CORS error, drop Makapix from title-view.

## Per-source detail

### Giphy

The existing `giphy_api_key` in NVS is leaked to the browser today via `/config`
(used by `settings.html`). Reuse the same retrieval path; no new endpoint.

- Endpoint: `GET https://api.giphy.com/v1/gifs/{giphy_id}?api_key={key}`
- Confirmed via `webfetch` of `https://developers.giphy.com/docs/api/schema/`:
  the GIF Object includes `title` ("The title that appears on giphy.com for this GIF").
- The CSS class `pill-pending` already exists in `webui/static/common.css:592`
  for the loading spinner.

### AIC (Art Institute of Chicago)

The stored `iiif_key` is the AIC `image_id` (a UUID-shaped string) — see
`components/art_institution/museums/artic.c:88`. The artwork's integer `id` is not
stored, so we look up by `image_id` via the search endpoint:

```
GET https://api.artic.edu/api/v1/artworks/search
    ?query[term][image_id]={iiif_key}&fields=id,title&limit=1
Header: AIC-User-Agent: p3a-museum-browse/1 (pub@kury.dev)
```

Title at `data[0].title`. The existing `webui/museum/artic.js:182+` adapter already
issues identical search requests for the browse modal; the new method
`fetchTitleByIiifKey()` slots in cleanly.

### Rijks (Rijksmuseum)

The stored `iiif_key` is the **micrio short id** (e.g. `RFwqO`) once the lazy
Linked-Art resolver has run. The micrio id alone is not enough to look up the
artwork's title — there is no public reverse mapping from micrio → HMO. The fix
shipped by this feature (plan §2c) is to encode the iiif_key as
`{micrio}|{hmo_int}` post-resolve. The lookup then runs:

```
GET https://id.rijksmuseum.nl/{hmo}
Header: Accept: application/ld+json
```

The response is a Linked-Art HumanMadeObject. Title extraction reuses the
existing `getTitle()` helper at `webui/museum/rijksmuseum.js:51`:

```js
function getTitle(hmo) {
  for (const n of (hmo && hmo.identified_by) || []) {
    if (n && n.type === 'Name' && n.content) return String(n.content);
  }
  return '(untitled)';
}
```

Legacy entries (resolved before §2c lands) have iiif_key without the `|` separator;
the title-fetch helper detects this and shows "Title unavailable" until the next
refresh re-resolves and re-stamps the iiif_key.

### V&A (Victoria & Albert Museum)

The stored `iiif_key` is `_primaryImageId` (e.g. `2007AT9374`). The V&A v2 API
exposes a `/v2/objects/search` endpoint that supports a free-text `q` parameter;
the imageId is a globally-unique alphanumeric token, so `q={iiif_key}` is expected
to return the matching record as the only hit.

```
GET https://api.vam.ac.uk/v2/objects/search?q={iiif_key}&page_size=1
```

Title at `records[0]._primaryTitle` (matches `getTitle()` in
`webui/museum/vam.js:55`).

**Verify-during-impl:** if the free-text query does not return the right hit
deterministically, switch to a structured filter (`?q_object_data=_primaryImageId:{id}`)
or fall back to fetching the object directly by its `systemNumber` — but that
requires either storing the systemNumber too (cache layout change, not in scope)
or a separate "find systemNumber by imageId" search call first.

### Wellcome Collection

The stored `iiif_key` is the IIIF `vid` (a bnumber-shaped string like `b18035723`)
extracted from `items[].locations[].url` — see `webui/museum/wellcome.js:32` and
`extractVid()` at line 93. The vid is **not** the work's catalogue id (which is a
short slug like `mtkdctvn`), so a direct `GET /catalogue/v2/works/{id}` does not
work. Search instead:

```
GET https://api.wellcomecollection.org/catalogue/v2/works?query={vid}&pageSize=1
```

Title at `results[0].title`. The existing `getTitle()` at
`webui/museum/wellcome.js:63` already trims paragraph-length titles to 140 chars.

**Verify-during-impl:** if a free-text query on the bnumber returns 0 hits, try
the alternate parameter `identifiers.value={vid}` — Wellcome catalogue items often
carry the bnumber as a parallel identifier in the work record.

### SMK (Statens Museum for Kunst)

The stored `iiif_key` is the JP2 filename suffix (e.g. `bc386p50w_kksgb22235.tif.jp2`),
extracted by `extractFilename()` in `webui/museum/smk.js:73`. Look up by free-text
search on the filename — it's unique enough to identify the record:

```
GET https://api.smk.dk/api/v1/art/search?keys={filename}&rows=1
```

Title at `items[0].titles[0].title` (matches `getTitle()` in `webui/museum/smk.js:83`).

**Verify-during-impl:** if free-text doesn't match, try a structured filter:
`?filters=[image_iiif_id:CONTAINS:{filename}]` (SMK supports a `filters` array
with `CONTAINS` operator per the adapter's existing filter syntax at line 164).

### HAM (Harvard Art Museums)

The stored `iiif_key` is the URN suffix (e.g. `urn-3:HUAM:79762_dynmc`), with the
NRS host prefix stripped — see `extractUrn()` in `webui/museum/ham.js:114`. The
URN is embedded in `primaryimageurl`, which is searchable:

```
GET https://api.harvardartmuseums.org/object
    ?apikey={key}&q=primaryimageurl:*{urn}*&size=1&fields=id,title
```

Title at `records[0].title`. The `apikey` is read from `cfg.ham_api_key` via
`/config`, the same way `webui/museum/ham.js:59` retrieves it today
(`loadConfigKey()`).

### Makapix

The Makapix Club team provided two endpoint variants:

- `GET https://makapix.club/api/p/{public_sqid}` — primary route
- `GET https://makapix.club/api/post/{storage_key}` — legacy route (same Post schema)

The firmware does not have the post's `public_sqid` (verified via grep — the only
SQID concept in `components/makapix/` refers to the **user**, not the post). We do
have `storage_key`, so the legacy route is the chosen path:

```
GET https://makapix.club/api/post/{storage_key}
Header: Accept: application/json
```

Title at top-level `title`. No auth header required.

Per Makapix team notes, each successful call records a view event against the post.
The plan does not optimize around this (no caching, no rate limiting beyond
"one fetch per user click").

**CORS:** unverified. The first implementation step for Makapix is a one-line
browser-console smoke test against a real `storage_key`. If the request is blocked
(no `Access-Control-Allow-Origin` header from `makapix.club`), Makapix is dropped
from the feature: its title button is hidden, same as SD-card. No firmware proxy.

## Error handling

- **HTTP 429** — Reuse the existing reporter at `POST /api/museum/rate-limits/report-429`
  with `{museum, retry_after_sec}` so the device's cooldown table stays in sync with
  the browser's experience (mirror the pattern in `webui/museum/artic.js:75-85`).
- **Network failure / non-2xx / empty response** — show "Title unavailable" in the
  title panel; do not block the rest of the UI.
- **Missing API key** (HAM) — show "Enter your Harvard Art Museums API key in Settings"
  (reuse the `userMessage` field from `makeNoKeyError()` in `webui/museum/ham.js:67`).

## Open items to verify at implementation

1. V&A: does `q={imageId}` return the correct record? If not, switch to
   `q_object_data=_primaryImageId:{id}` or a two-step lookup.
2. Wellcome: does `query={vid}` return the correct work? If not, try
   `identifiers.value={vid}`.
3. SMK: does `keys={filename}` find the record? If not, switch to
   `filters=[image_iiif_id:CONTAINS:{filename}]`.
4. Makapix: does `makapix.club` allow cross-origin browser requests? If not, drop.
