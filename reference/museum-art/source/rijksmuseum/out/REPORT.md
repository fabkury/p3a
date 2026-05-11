# Rijksmuseum exploration report

- Run started: `2026-05-07 15:28:29Z`
- Source: Rijksmuseum (Amsterdam) — anonymous Linked Open Data + IIIF
- Endpoints: `https://data.rijksmuseum.nl` (Linked Art + OAI-PMH), `https://iiif.micr.io/` (IIIF Image API v2)
- Image size requested: longest side = 720 px (`/full/!720,720/0/default.jpg`)

## Feature support summary

| UBI feature | Native? | Notes |
|---|---|---|
| List collections | partial | OAI-PMH `verb=ListSets` enumerates curated Sets in a single XML response. No JSON-LD listing endpoint. |
| List artworks in a collection | cursor only | `search/collection?memberOfSetId=…` returns 100 items per page with a `pageToken` cursor. **Range-style pagination must be emulated** by walking the cursor and indexing locally. |
| Keyword search | emulated | No free-text `q`. The API accepts scoped fields (`title`, `description`, `creator`, `aboutActor`, `material`, `technique`, `type`, `creationDate`, `objectNumber`). This script fans out across `title` + `description` and unions the results. |

## 1. List collections (curated Sets)

`GET https://data.rijksmuseum.nl/oai?verb=ListSets` returned **193 sets** (Dutch + English names mixed).

First 15 sets:

| setSpec | setName |
|---|---|
| `261231` | keramiek (collectie) |
| `261221` | varia |
| `261222` | papier (collectie) |
| `261233` | porselein |
| `261228` | textiel |
| `261234` | kraakporselein |
| `261229` | gelegenheidsgrafiek |
| `261220` | poppenhuis |
| `261219` | schutterij |
| `261217` | Van Gelder |
| `261218` | Engels aardewerk |
| `261207` | Atlas J.M. Coffeng (KOG) |
| `261206` | Atlas Van Eck (KOG) |
| `261205` | maritieme collecties |
| `26118` | Flemish Paintings in the Rijksmuseum |

Chosen for demo: `26118` (Flemish Paintings in the Rijksmuseum), `26121` (Dutch Paintings of the Seventeenth Century in the Rijksmuseum)

## 2. List artworks in 2 collections (cursor pagination)

### Set `26118` — Flemish Paintings in the Rijksmuseum

- Endpoint: `GET https://data.rijksmuseum.nl/search/collection?memberOfSetId=https://id.rijksmuseum.nl/26118&imageAvailable=true`
- Total items in set with images: **129**
- Cursor pages fetched: 1; collected first 10 item IDs.
- Sample IDs: `200107829`, `200107873`, `200107990`, `200108010`, `200108183`

### Set `26121` — Dutch Paintings of the Seventeenth Century in the Rijksmuseum

- Endpoint: `GET https://data.rijksmuseum.nl/search/collection?memberOfSetId=https://id.rijksmuseum.nl/26121&imageAvailable=true`
- Total items in set with images: **727**
- Cursor pages fetched: 1; collected first 10 item IDs.
- Sample IDs: `200106086`, `200107756`, `200107757`, `200107761`, `200107762`

## 3. Keyword search

**Emulation note:** The data.rijksmuseum.nl API has no free-text `q` parameter — only scoped fields are accepted. Each keyword below is dispatched against `title` and `description` separately, and the script unions the result sets client-side. A real UBI adapter would either index the corpus locally for true full-text search or always present search as scoped.

### Search: `Vermeer`

- Endpoint per field: `GET https://data.rijksmuseum.nl/search/collection?<field>=Vermeer&imageAvailable=true`
- field `title` → 27 total hits
- field `description` → 23 total hits
- Union (deduped) sample IDs: `200142152`, `200144420`, `200319221`, `200353901`, `200367177`

### Search: `tulip`

- Endpoint per field: `GET https://data.rijksmuseum.nl/search/collection?<field>=tulip&imageAvailable=true`
- field `title` → 25 total hits
- field `description` → 15 total hits
- Union (deduped) sample IDs: `200142727`, `20015956`, `200218550`, `200220173`, `20023705`

## 4. Image downloads (720px longest side)

**IIIF discovery chain** (3 hops in Linked Art):
  `HumanMadeObject.shows[]` → `VisualItem.digitally_shown_by[]` → `DigitalObject.access_point[]` → IIIF URL on `iiif.micr.io`.

**Image request**: `GET {iiif_base}/full/!720,720/0/default.jpg` (IIIF Image API v2 `!w,h` = best-fit within box, so longest side = 720).

| Bucket | Object # | Title | File | Bytes | Width × Height |
|---|---|---|---|---|---|
| `set-26118` | SK-A-2961 | Venus en Adonis | `set-26118__kmGDy.jpg` | 82861 | 609 × 720 |
| `set-26118` | SK-A-834 | Watermolen bij een beboste heuvel | `set-26118__yaEop.jpg` | 67839 | 720 × 539 |
| `set-26118` | SK-A-378 | Stilleven met dood wild, vruchten en groenten | `set-26118__BPTNf.jpg` | 62771 | 720 × 453 |
| `set-26121` | SK-A-4878 | A Young Woman Warming her Hands over a Brazier: Allegory of  | `set-26121__nuhhV.jpg` | 38556 | 602 × 720 |
| `set-26121` | SK-A-1856 | The Meeting of Granida and Daifilo | `set-26121__hQYKU.jpg` | 70122 | 720 × 557 |
| `set-26121` | SK-A-4013 | Portret Govert van Slingelandt (1623-90), zijn eerste vrouw  | `set-26121__cdqwm.jpg` | 86686 | 621 × 720 |
| `search-Vermeer` | RP-F-00-69 | Joh. Ver. Meer. Delft vom Rotterdamer Kanal gesehen | `search-Vermeer__eenOX.jpg` | 56716 | 720 × 632 |
| `search-Vermeer` | RP-F-00-596 | Fotoreproductie Schilderij Vrouw met waterkan door Johannes  | `search-Vermeer__uMAlX.jpg` | 64387 | 720 × 666 |
| `search-Vermeer` | RP-F-F00107 | The Letter | `search-Vermeer__XzSAF.jpg` | 45599 | 525 × 720 |
| `search-tulip` | RP-T-1967-86 | Blad uit een tulpenboek | `search-tulip__oSnri.jpg` | 45660 | 720 × 521 |
| `search-tulip` | SK-A-2890 | Tulip Fields | `search-tulip__KGCgR.jpg` | 51453 | 720 × 388 |
| `search-tulip` | RP-P-1936-365 | Tulip and Iris | `search-tulip__gcvyL.jpg` | 42687 | 500 × 720 |

Total downloaded: **12 files** in `out/images/`.

