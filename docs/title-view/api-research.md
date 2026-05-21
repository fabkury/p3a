# Artwork info view: per-source API research

> File name kept as `api-research.md` (no rename) so existing references stay valid.
> The feature was originally "title-view"; it now shows title + artist + date and
> is surfaced in the UI as **Show artwork info** behind an ℹ️ button.

This document captures the per-source mechanism for fetching the title, artist, and
creation date of a single artwork given the identifier the firmware exposes for the
currently playing post. Missing fields render as `—` (em-dash) in the panel.

All requests are issued **browser-direct** from the p3a web UI; the firmware does not
proxy them. Where an API key is required, the browser reads it from `GET /config`
(the same path `webui/settings.html` already uses to populate the Settings tab —
see `webui/settings.html:807` for `giphy_api_key` and `webui/settings.html:887` for
`ham_api_key`). No new key-leaking mechanism is introduced.

If a source's API blocks cross-origin requests, the source is **dropped from the
feature** (its info button stays hidden, same as SD-card). The design explicitly
disallows a firmware proxy fallback.

## Firmware-exposed identifier for each source

The starting point for every lookup is whatever the firmware already publishes in
the `current_artwork` JSON (`build_current_artwork_json()` in
`components/http_api/http_api_rest_playsets.c:69`):

| Source        | Identifier in JSON              | Notes                                                                   |
|---------------|---------------------------------|-------------------------------------------------------------------------|
| Giphy         | `giphy_id`                      | existing                                                                |
| Institution   | `museum_id` + `iiif_key`        | parsed from `channel_spec_name` and `artwork.storage_key`               |
| Makapix       | `storage_key`                   | already used to build the vault URL — emitted as a top-level field      |
| SD-card       | (none)                          | info button stays hidden                                                |

## Per-source endpoint table

| Source     | Lookup endpoint                                                                                                              | Title path                  | Artist path                                            | Date path                                       | Auth                              | CORS    |
|------------|------------------------------------------------------------------------------------------------------------------------------|-----------------------------|--------------------------------------------------------|-------------------------------------------------|-----------------------------------|---------|
| Giphy      | `GET https://api.giphy.com/v1/gifs/{giphy_id}?api_key={key}`                                                                  | `data.title`                | `data.user.display_name` + `(@username)` formatted     | `data.create_datetime` → YYYY-MM-DD             | `api_key` query (from `/config`)  | yes     |
| AIC        | `GET https://api.artic.edu/api/v1/artworks/search?query[term][image_id]={iiif_key}&fields=id,title,artist_title,date_display&limit=1` | `data[0].title`             | `data[0].artist_title`                                 | `data[0].date_display`                          | `AIC-User-Agent` header           | yes     |
| Rijks      | `GET https://id.rijksmuseum.nl/{hmo}` (Accept: `application/ld+json`)                                                         | first `Name` in `identified_by[]` | first `_label` in `produced_by.carried_out_by[]` | first `Name` in `produced_by.timespan.identified_by[]` | none                              | yes     |
| V&A        | `GET https://api.vam.ac.uk/v2/objects/search?q={iiif_key}&page_size=1`                                                        | `records[0]._primaryTitle`  | `records[0]._primaryMaker.name`                        | `records[0]._primaryDate`                       | none                              | yes     |
| Wellcome   | `GET https://api.wellcomecollection.org/catalogue/v2/works?query={vid}&pageSize=1&items.locations.locationType=iiif-image&include=items` | `results[0].title`          | `results[0].contributors[0].agent.label`               | `results[0].production[0].dates[0].label`       | none                              | yes     |
| SMK        | `GET https://api.smk.dk/api/v1/art/search?keys={filename}&rows=1`                                                              | `items[0].titles[0].title`  | `items[0].production[].creator`/`creator_forename`/`creator_surname` | `items[0].production[0].creation_date_text`     | none                              | yes     |
| HAM        | `GET https://api.harvardartmuseums.org/object?apikey={key}&q=primaryimageurl:*{urn}*&size=1&fields=id,title,people,dated`     | `records[0].title`          | `records[0].people[0].displayname`                     | `records[0].dated`                              | `apikey` query (from `/config`)   | yes     |
| Makapix    | `GET https://makapix.club/player/post/{storage_key}` (Accept: `application/json`)                                              | `title`                     | `owner.handle` (formatted as `@handle`)                | `created_at` → YYYY-MM-DD                       | none                              | yes (player route) |

"CORS yes" entries are empirically confirmed because the existing
`webui/museum/*.js` browse adapters already fetch from those hosts. Makapix CORS
required switching from the `/api/post/{storage_key}` route (same-origin only)
to `/player/post/{storage_key}`, which the Makapix team ships with permissive
CORS headers specifically for player/embed use cases. Response schema is
identical between the two routes.

## Date formatting

`formatArtworkDate(raw)` (defined inline in `webui/index.html`):

- If the input starts with `YYYY-MM-DD`, return that prefix (trims Giphy's
  `"2017-01-13 12:34:56"` and Makapix's `"2026-05-10T15:07:14.972807Z"` to
  `2017-01-13` and `2026-05-10` respectively).
- Otherwise pass through unchanged. Museum date strings (`"1934"`, `"c. 1800"`,
  `"before 1900"`, `"Edo period"`) have no leading YYYY-MM-DD and stay verbatim.

## Artist formatting

- **Museums**: whatever the per-museum helper returns (creator name, contributor
  label, "people" display name). No prefix.
- **Giphy** — combine `user.display_name` (real name) with `username` (handle):
  - both present → `Display Name (@username)`
  - only username → `@username`
  - only display_name → `Display Name`
  - neither → `null` (renders as `—`)
- **Makapix** — `owner.handle` as `@handle`. Confirmed via live response: the
  field is `owner.handle` (returns "Fab", not "@Fab"); the `@` prefix is applied
  by the formatter. No `display_name` field is present on the public-profile owner
  object.

## Per-source detail

### Giphy

The existing `giphy_api_key` in NVS is leaked to the browser today via `/config`
(used by `settings.html`). Reuse the same retrieval path; no new endpoint.

- Endpoint: `GET https://api.giphy.com/v1/gifs/{giphy_id}?api_key={key}`
- Fields: `data.title`, `data.user.display_name`, `data.username`, `data.create_datetime`.

### AIC (Art Institute of Chicago)

The stored `iiif_key` is the AIC `image_id` (a UUID-shaped string) — see
`components/art_institution/museums/artic.c:88`. The artwork's integer `id` is not
stored, so we look up by `image_id` via the search endpoint:

```
GET https://api.artic.edu/api/v1/artworks/search
    ?query[term][image_id]={iiif_key}
    &fields=id,title,artist_title,date_display&limit=1
Header: AIC-User-Agent: p3a-museum-browse/1 (pub@kury.dev)
```

AIC's listing returns title, artist, and date inline. The existing
`webui/museum/artic.js:182+` adapter already issues identical search requests for
the browse modal; `fetchMetadataByIiifKey()` adds the artist + date fields to the
existing search-by-image_id call.

### Rijks (Rijksmuseum)

The stored `iiif_key` is the **micrio short id** (e.g. `RFwqO`) once the lazy
Linked-Art resolver has run. The micrio id alone is not enough to look up the
artwork — there is no public reverse mapping from micrio → HMO. The fix is to
encode the iiif_key as `{micrio}|{hmo_int}` post-resolve. The lookup then runs:

```
GET https://id.rijksmuseum.nl/{hmo}
Header: Accept: application/ld+json
```

Title, artist, and date are extracted by the existing helpers in
`webui/museum/rijksmuseum.js` (`getTitle`, `getArtist`, `getDate`):

```js
function getTitle(hmo) { /* identified_by[].content where type=='Name' */ }
function getArtist(hmo) { /* produced_by.carried_out_by[]._label */ }
function getDate(hmo)   { /* produced_by.timespan.identified_by[].content */ }
```

Legacy entries (resolved before the HMO-preservation change) have iiif_key
without the `|` separator; `fetchMetadataByIiifKey()` throws, surfacing the
"all em-dash" state until the next refresh re-resolves and re-stamps the iiif_key.

### V&A (Victoria & Albert Museum)

The stored `iiif_key` is `_primaryImageId` (e.g. `2007AT9374`). The V&A v2 API
exposes a `/v2/objects/search` endpoint that supports a free-text `q` parameter;
the imageId is a globally-unique alphanumeric token, so `q={iiif_key}` is expected
to return the matching record as the only hit.

```
GET https://api.vam.ac.uk/v2/objects/search?q={iiif_key}&page_size=1
```

Fields: `records[0]._primaryTitle`, `records[0]._primaryMaker.name`, `records[0]._primaryDate`.

**Verify-during-impl:** if the free-text query does not return the right hit
deterministically, switch to a structured filter
(`?q_object_data=_primaryImageId:{id}`) or fall back to fetching the object
directly by its `systemNumber` (requires storing the systemNumber too — out of
scope today).

### Wellcome Collection

The stored `iiif_key` is the IIIF `vid` (a bnumber-shaped string like `b18035723`)
extracted from `items[].locations[].url` — see `webui/museum/wellcome.js:32` and
`extractVid()` at line 93. The vid is not the work's catalogue id (which is a
short slug like `mtkdctvn`), so search instead:

```
GET https://api.wellcomecollection.org/catalogue/v2/works
    ?query={vid}&pageSize=1
    &items.locations.locationType=iiif-image
    &include=items
```

Fields: `results[0].title` (paragraph-trimmed by `getTitle`),
`results[0].contributors[0].agent.label`, `results[0].production[0].dates[0].label`.

**Verify-during-impl:** if a free-text query on the bnumber returns 0 hits, try
the alternate parameter `identifiers.value={vid}` — Wellcome catalogue items often
carry the bnumber as a parallel identifier.

### SMK (Statens Museum for Kunst)

The stored `iiif_key` is the JP2 filename suffix (e.g. `bc386p50w_kksgb22235.tif.jp2`),
extracted by `extractFilename()` in `webui/museum/smk.js:73`. Look up by free-text
search on the filename — it's unique enough to identify the record:

```
GET https://api.smk.dk/api/v1/art/search?keys={filename}&rows=1
```

Fields: `items[0].titles[0].title`, `items[0].production[].creator*` (via
`getArtist` which walks `creator`, `creator_forename`, `creator_surname`),
`items[0].production[0].creation_date_text`.

**Verify-during-impl:** if free-text doesn't match, try a structured filter:
`?filters=[image_iiif_id:CONTAINS:{filename}]`.

### HAM (Harvard Art Museums)

The stored `iiif_key` is the URN suffix (e.g. `urn-3:HUAM:79762_dynmc`), with the
NRS host prefix stripped — see `extractUrn()` in `webui/museum/ham.js:114`. The
URN is embedded in `primaryimageurl`, which is searchable:

```
GET https://api.harvardartmuseums.org/object
    ?apikey={key}&q=primaryimageurl:*{urn}*&size=1
    &fields=id,title,people,dated
```

Fields: `records[0].title`, `getPeopleDisplay(record)` (uses `people[0].displayname`),
`records[0].dated`. The `apikey` is read from `cfg.ham_api_key` via `/config`, the
same way `webui/museum/ham.js:59` retrieves it today (`loadConfigKey()`).

### Makapix

The Makapix Club team provided several endpoint variants:

- `GET https://makapix.club/api/p/{public_sqid}` — primary route (same-origin CORS only)
- `GET https://makapix.club/api/post/{storage_key}` — same-origin CORS only
- `GET https://makapix.club/player/post/{storage_key}` — **permissive CORS**, intended for
  player / embed use cases. Same Post schema as the other two.

The firmware does not have the post's `public_sqid` (verified via grep — the only
SQID concept in `components/makapix/` refers to the **user**, not the post). We do
have `storage_key`, and the player route is the only one with cross-origin CORS,
so that's the chosen path:

```
GET https://makapix.club/player/post/{storage_key}
Header: Accept: application/json
```

Confirmed via live response (`GET https://makapix.club/api/p/HqeD`):

- `title`: plain string (`"MS3 Marco at the beach"`).
- `owner.handle`: plain string (`"Fab"`). The formatter prepends `@` for display.
  No `display_name` / `name` / other-attribution field is present.
- `created_at`: ISO 8601 with timezone (`"2026-05-10T15:07:14.972807Z"`). The
  `formatArtworkDate` helper trims this to `YYYY-MM-DD`.

Per Makapix team notes, each successful call records a view event against the post.
We do not optimize around this (no caching, no rate limiting beyond "one fetch per
user click").

**CORS:** the `/api/post/{storage_key}` and `/api/p/{public_sqid}` routes are
same-origin only, which blocked the original implementation. The Makapix team
exposed `/player/post/{storage_key}` specifically with permissive CORS for
embed / player use cases, and that's the route this feature uses. Response
schema is unchanged.

## Error handling

- **HTTP 429** — Reuse the existing reporter at `POST /api/museum/rate-limits/report-429`
  with `{museum, retry_after_sec}` so the device's cooldown table stays in sync with
  the browser's experience (mirror the pattern in `webui/museum/artic.js:75-85`).
- **Network failure / non-2xx / empty response** — render `—` in all three rows
  (no chatty error message); do not block the rest of the UI.
- **Missing API key** (HAM) — propagates the existing `makeNoKeyError()` from
  `webui/museum/ham.js:67`; surfaces as `—` in the panel.

## Open items to verify at implementation

1. V&A: does `q={imageId}` return the correct record? If not, switch to
   `q_object_data=_primaryImageId:{id}` or a two-step lookup.
2. Wellcome: does `query={vid}` return the correct work? If not, try
   `identifiers.value={vid}`.
3. SMK: does `keys={filename}` find the record? If not, switch to
   `filters=[image_iiif_id:CONTAINS:{filename}]`.
