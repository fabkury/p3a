# Smithsonian Open Access — deferred options

Record of integration choices made during the v1 Smithsonian wiring (2026-05-22)
that intentionally left content on the table. Revisit before adding more units
or extending the axis model. Phase A data lives in `output/report.md`.

## v1 unit list (shipped)

| Unit code | Display name | Online items | IIIF coverage |
|---|---|---:|---:|
| `CHNDM` | Cooper Hewitt, Smithsonian Design Museum | 54,608 | 90% |
| `SAAM` | Smithsonian American Art Museum | 12,989 | 80% |
| `NPG` | National Portrait Gallery | 14,641 | 80% |
| `NMAAHC` | National Museum of African American History and Culture | 4,647 | 90% |
| `HMSG` | Hirshhorn Museum and Sculpture Garden | 449 | 90% |
| `NMAfA` | National Museum of African Art | 113 | 90% |

## Excluded — broken

### `NMAI` — National Museum of the American Indian

- 180 online items (out of 237,742 total — only 0.08% open-access)
- 100% of sampled hits have `idsId`, **0% of probed `info.json` returns 200**
- Cause: NMAI records use ARK-format identifiers (`ark:/65665/om8...`) that the
  `ids.si.edu/ids/iiif/` endpoint does not resolve.
- To unblock: Smithsonian would need to add ARK→IDS resolution at the IIIF
  endpoint, or NMAI would need to re-tag records with IDS-format identifiers.
  Re-probe `output/report.md` section A annually. If `info.json OK` climbs
  above 80%, wire NMAI to the same `unit` axis with no other code changes.

### `FSG` — Freer Gallery of Art + Sackler (National Museum of Asian Art)

- 0 items in Open Access dataset (verified 2026-05-22)
- Smithsonian's official OpenAccess GitHub repo lists `FSG` as a valid unit
  code, but the actual dataset has no FSG records as of this writing — the
  Freer/Sackler collection has not been added to Open Access.
- To unblock: re-probe `_count_for_query("unit_code:FSG")` periodically. If
  nonzero, wire FSG to the `unit` axis with no other code changes.

## Deferred — other units with online content (non-art)

Counts from `output/report.md` §1. These units are excluded from v1 because
their content is non-art (specimens, archives, books, postal items, etc.) and
would dilute the art channel experience. Several have substantial volume and
could be considered later for a separate "non-art Smithsonian" channel type or
as opt-in unit additions.

| Unit code | Display name | Online items | Notes |
|---|---|---:|---|
| `SIL` | Smithsonian Libraries | 13,567 | Mostly book digitizations; some illustration plates would be visually interesting |
| `NMAH` | National Museum of American History | 10,907 | Material culture, photos, ephemera; mixed visual quality |
| `NPM` | National Postal Museum | 8,576 | Stamps, postal artifacts; visually rich but very narrow theme |
| `SIA` | Smithsonian Institution Archives | 5,477 | Institutional photos; weak as art |
| `NZP` | National Zoo & Conservation Biology Institute | 1,061 | Animal photos; not art |
| `NASM` | National Air and Space Museum | 992 | Aircraft, instruments; visually compelling but not art |
| `HAC` | Smithsonian Gardens | 430 | Plant photography |
| `ACM` | Anacostia Community Museum | 247 | Community-history objects; small |
| `FBR` | Smithsonian Field Book Project | 37 | Mostly text, almost no images |

Zero online items, listed for completeness: `NMNH` (Natural History — surprising),
`AAA` (Archives of American Art), `NAA`, `EEPA`, `HSFA`, `CFCHFOLKLIFE`.

To wire any of these: add to `ART_BEARING_UNITS` in `webui/museum/smithsonian.js`.
The firmware adapter is generic — accepts any `unit_code` value as `term_id`,
so no C-side changes are needed for additional units.

## Deferred — additional axes (v2 ideas)

v1 ships **one axis**: `si:unit` (24 units total, 6 wired). Phase A confirmed
the API supports richer Solr-style faceting. Future axes to consider:

### `si:classification` — cross-unit topical browse

Smithsonian records carry `content.indexedStructured.object_type` (paintings,
photographs, sculpture, etc.). A `si:classification` axis would let users say
"all paintings across all Smithsonian art museums" rather than "paintings in
SAAM specifically". Requires:

1. Phase A re-probe to enumerate the classification vocabulary and per-term
   counts under `online_visual_material:true`.
2. Modal: terms step shows classification names; user picks "Paintings".
3. Firmware filter: `q=object_type:"Painting" AND online_visual_material:true`
   (no unit restriction, returns hits from all units).
4. Decision: should the firmware filter out non-art units in this mode, or
   trust the classification filter to do the right thing? Probably the latter
   — classifications are inherently art-ish — but probe sample results first.

### `si:topic` — thematic browse (e.g., "Civil War", "Aviation")

Smithsonian records have `topic[]` and `culture[]` fields in `freetext` and
`indexedStructured`. A topic axis would enable browse-by-subject across units.
Likely useful for NMAAHC and NPG content where historical themes dominate.
Verify field shape and term cardinality before committing.

### `si:keyword` — free-text search as a channel

Probe-able today via `q="{keyword}" AND online_visual_material:true`. Could
be modeled as a degenerate axis (one synthetic "term" per saved keyword).
Useful if users want a "Vermeer" or "Hokusai" channel without picking a unit.

## Deferred — rights handling

Phase A probe D showed:

| Filter | SAAM count |
|---|---:|
| `unit_code:SAAM` | 13,779 |
| `unit_code:SAAM AND online_visual_material:true` | 12,989 |
| `unit_code:SAAM AND online_visual_material:true AND usage:CC0` | **0** |

The `usage` field is **not** Solr-indexed and cannot be used as a filter
clause. v1 trusts `online_visual_material:true` as a proxy for displayable
content and leaves per-item rights inspection to a future v2.

Observed rights values in the sampled downloads:

- `CC0` (majority — SAAM, CHNDM, NPG, HMSG, NMAfA samples are all CC0)
- `No known copyright restrictions` (some NMAAHC items)
- `Public domain` (some NMAAHC items)
- `No Known Copyright Restrictions` (case variant, some NMAAHC items)

All are effectively public-domain for the device's display use case. A strict
CC0-only policy would require parsing `content.freetext.objectRights[*].content`
per-record at refresh time and skipping non-CC0 hits — adds CPU + bandwidth
cost, drops NMAAHC coverage significantly. Not worth it for v1.

## Deferred — download retry / connection resilience

Phase A measured **~63% download success rate** (17/27) with
`RemoteDisconnected`-class errors from `ids.si.edu` accounting for most
failures. The firmware uses the existing 3-attempt backoff `{0, 1000, 3000}`
shared with HAM/SMK, which is adequate for v1.

If user reports show empty channels or unusually slow refreshes, consider:

- Bumping `SI_FETCH_MAX_ATTEMPTS` to 5.
- Adding HEAD-probe-then-GET to skip dead IIIF URLs before the heavyweight
  image fetch. Adds latency but saves bandwidth on broken records.
- Maintaining a per-`idsId` blacklist (in NVS or a side file on SD) for
  IDs that 404 repeatedly, so they're not retried indefinitely.

None of these are needed for v1 — surface as work if real-world reliability
issues are reported.
