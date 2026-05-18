# HAM — facet axis inventory

*Run at 2026-05-18 10:27:27 UTC*

Enumerates each candidate facet axis and probes image-bearing counts for the top-3 most-populous terms in each. Used to choose the 1–4 axes that ship in HAM v1.

## Axis: `classification`

- Total terms in vocabulary: **64**
- Returned in page 1 (size=100): **64**
- Sample term fields: `['classificationid', 'id', 'lastupdate', 'name', 'objectcount']`
- Terms with `objectcount > 0` in page 1: **64**
- Label field `name`: 64 populated, max len=26, >32 chars: 0/64
- Term id field: `classificationid`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 17 | `Photographs` | 86437 | 77076 |
| 2 | 23 | `Prints` | 72257 | 59162 |
| 3 | 21 | `Drawings` | 34062 | 29919 |

## Axis: `century`

- Total terms in vocabulary: **47**
- Returned in page 1 (size=100): **47**
- Sample term fields: `['id', 'lastupdate', 'name', 'objectcount', 'temporalorder']`
- Terms with `objectcount > 0` in page 1: **47**
- Label field `name`: 47 populated, max len=20, >32 chars: 0/47
- Term id field: `id`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 37525815 | `20th century` | 131584 | 116080 |
| 2 | 37525806 | `19th century` | 54028 | 42438 |
| 3 | 37525797 | `18th century` | 19561 | 16442 |

## Axis: `culture`

- Total terms in vocabulary: **255**
- Returned in page 1 (size=100): **100**
- Sample term fields: `['id', 'lastupdate', 'name', 'objectcount']`
- Terms with `objectcount > 0` in page 1: **100**
- Label field `name`: 100 populated, max len=21, >32 chars: 0/100
- Term id field: `id`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 37526778 | `American` | 91844 | 81347 |
| 2 | 37527453 | `German` | 38785 | 34888 |
| 3 | 37527426 | `French` | 26051 | 18928 |

## Axis: `gallery`

- Total terms in vocabulary: **64**
- Returned in page 1 (size=100): **64**
- Sample term fields: `['createdate', 'displayname', 'donorname', 'floor', 'galleryid', 'gallerynumber', 'id', 'labeltext', 'lastupdate', 'name', 'objectcount', 'theme', 'url']`
- Terms with `objectcount > 0` in page 1: **55**
- Label field `name`: 64 populated, max len=47, >32 chars: 17/64
- Term id field: `galleryid`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 3500 | `Special Exhibitions Gallery` | 295 | 280 |
| 2 | 1740 | `Early Chinese Art` | 167 | 167 |
| 3 | 3400 | `Ancient Mediterranean and Middle Eastern Art` | 102 | 100 |

## Axis: `period`

- Total terms in vocabulary: **324**
- Returned in page 1 (size=100): **100**
- Sample term fields: `['id', 'lastupdate', 'name', 'objectcount', 'periodid']`
- Terms with `objectcount > 0` in page 1: **100**
- Label field `name`: 100 populated, max len=76, >32 chars: 22/100
- Term id field: `periodid`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 248 | `Edo period, 1615-1868` | 5947 | 5411 |
| 2 | 786 | `Roman Imperial period` | 3233 | 3104 |
| 3 | 374 | `Byzantine period` | 3032 | 880 |

## Axis: `technique`

- Total terms in vocabulary: **321**
- Returned in page 1 (size=100): **100**
- Sample term fields: `['id', 'lastupdate', 'name', 'objectcount', 'techniqueid']`
- Terms with `objectcount > 0` in page 1: **100**
- Label field `name`: 100 populated, max len=36, >32 chars: 2/100
- Term id field: `techniqueid`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 123 | `Gelatin silver print` | 25831 | 23571 |
| 2 | 7320 | `Struck` | 23075 | 18604 |
| 3 | 107 | `Negative, gelatin silver (film)` | 17735 | 17301 |

## Axis: `worktype`

- Total terms in vocabulary: **414**
- Returned in page 1 (size=100): **100**
- Sample term fields: `['id', 'lastupdate', 'name', 'objectcount', 'worktypeid']`
- Terms with `objectcount > 0` in page 1: **0**
- Label field `name`: 100 populated, max len=29, >32 chars: 0/100
- Term id field: `worktypeid`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|

## Axis: `place`

- Total terms in vocabulary: **2549**
- Returned in page 1 (size=100): **100**
- Sample term fields: `['geo', 'haschildren', 'id', 'lastupdate', 'level', 'name', 'objectcount', 'parentplaceid', 'pathforward', 'placeid', 'tgn_id']`
- Terms with `objectcount > 0` in page 1: **100**
- Label field `name`: 100 populated, max len=28, >32 chars: 0/100
- Term id field: `placeid`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 2028213 | `United States` | 28703 | 44216 |
| 2 | 2028304 | `Japan` | 9738 | 8883 |
| 3 | 2028317 | `China` | 5497 | 5743 |

## Axis: `medium`

- Total terms in vocabulary: **325**
- Returned in page 1 (size=100): **100**
- Sample term fields: `['haschildren', 'id', 'lastupdate', 'level', 'mediumid', 'name', 'objectcount', 'parentmediumid', 'pathforward']`
- Terms with `objectcount > 0` in page 1: **100**
- Label field `name`: 100 populated, max len=31, >32 chars: 0/100
- Term id field: `mediumid`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 2028222 | `Metal` | 31433 | 23376 |
| 2 | 2028195 | `Ink` | 13618 | 12107 |
| 3 | 2028594 | `AE` | 11107 | 10368 |

## Axis: `color`

- Total terms in vocabulary: **0**
- Returned in page 1 (size=100): **0**
- (no terms returned)

## Axis: `group`

- Total terms in vocabulary: **0**
- Returned in page 1 (size=100): **0**
- (no terms returned)

## Axis: `person`

- Total terms in vocabulary: **42549**
- Returned in page 1 (size=100): **100**
- Sample term fields: `['alphasort', 'birthplace', 'createdate', 'culture', 'datebegin', 'dateend', 'deathplace', 'displaydate', 'displayname', 'gender', 'id', 'lastupdate', 'lcnaf_id', 'names', 'objectcount', 'personid', 'roles', 'ulan_id', 'url', 'viaf_id', 'wikidata_id', 'wikipedia_id']`
- Terms with `objectcount > 0` in page 1: **100**
- Label field `culture`: 91 populated, max len=13, >32 chars: 0/91
- Term id field: `personid`

**Top-3 most-populous terms — image-bearing probe:**

| # | id | label | objectcount | hasimage=1 count |
|---|---|---|---|---|
| 1 | 20002 | `American` | 24062 | 23235 |
| 2 | 34147 | `?` | 21046 | 18486 |
| 3 | 28639 | `American` | 7776 | 7135 |

## Axis ranking & recommendation

Ranking signals:

1. **objectcount populated** — required so the browse UI can rank terms    without per-term probes during enumeration.
2. **Term labels ≤ 32 chars** — required for the 33-byte identifier slot.    When axis terms exceed this on average, the editor would have to filter    them out, shrinking the usable surface.
3. **Image-bearing fraction > 10%** — terms where most artworks lack images    waste pagination budget on records the refresh has to skip.
4. **Single-page enumeration** — total terms ≤ 100 lets the browse modal    render the picker without paginating.

*Selection rationale and the final shipping recommendation are written into REPORT.md after this script's data is combined with the deep-offset and image probes.*
