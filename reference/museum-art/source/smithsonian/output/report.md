# Smithsonian Open Access demo run

_Generated: 2026-05-22 15:06:25 Eastern Daylight Time_

_API key in use: `(user-provided key, redacted)`_

This run exercises the four UBI features against the Smithsonian Open Access API (`https://api.si.edu/openaccess/api/v1.0/search`): list collections (the 21 Smithsonian units), list artworks in a unit (range pagination via `start`+`rows`), keyword search, and IIIF download at 720px on the longest side. Plus four Smithsonian-specific probes the firmware adapter needs answered: per-unit IIIF coverage, field-shape audit, rate-limit behavior, and a filter A/B for the production query.

## 1. Collections (Smithsonian units)

Smithsonian has 21 administrative "units" (sub-museums and archives). Each item carries a `unit_code` field, so units are the natural top-level grouping. Below is item count per unit, both unrestricted and restricted to `online_visual_material:true` (which is what the firmware adapter will query).

### Art-bearing units (p3a integration candidates)

| Unit code | Name | Total items | Online visual material |
|---|---|---:|---:|
| `SAAM` | Smithsonian American Art Museum | 13,779 | 12,989 |
| `NPG` | National Portrait Gallery | 15,475 | 14,641 |
| `CHNDM` | Cooper Hewitt, Smithsonian Design Museum | 58,178 | 54,608 |
| `NMAI` | National Museum of the American Indian | 237,742 | 180 |
| `FSG` | Freer Gallery of Art + Sackler (National Museum of Asian Art) | 0 | 0 |
| `NMAfA` | National Museum of African Art | 113 | 113 |
| `HMSG` | Hirshhorn Museum and Sculpture Garden | 449 | 449 |
| `NMAAHC` | National Museum of African American History and Culture | 22,784 | 4,647 |

### Other units (for comparison; non-art, mostly excluded from integration)

| Unit code | Name | Total items | Online visual material |
|---|---|---:|---:|
| `NMNH` | National Museum of Natural History | 0 | 0 |
| `NMAH` | National Museum of American History | 1,318,661 | 10,907 |
| `NASM` | National Air and Space Museum | 1,015 | 992 |
| `NPM` | National Postal Museum | 11,475 | 8,576 |
| `ACM` | Anacostia Community Museum | 250 | 247 |
| `NZP` | Smithsonian's National Zoo & Conservation Biology Institute | 1,061 | 1,061 |
| `SIA` | Smithsonian Institution Archives | 35,525 | 5,477 |
| `AAA` | Archives of American Art | 0 | 0 |
| `SIL` | Smithsonian Libraries | 938,151 | 13,567 |
| `NAA` | National Anthropological Archives | 0 | 0 |
| `EEPA` | Eliot Elisofon Photographic Archives | 0 | 0 |
| `HSFA` | Human Studies Film Archives | 0 | 0 |
| `HAC` | Smithsonian Gardens | 430 | 430 |
| `FBR` | Smithsonian Field Book Project | 1,517 | 37 |
| `CFCHFOLKLIFE` | Ralph Rinzler Folklife Archives and Collections | 17,544 | 0 |

## 2. Artworks in units (range pagination)

Demonstrating range-style pagination by requesting `start=200, rows=5` (i.e. items 200–204) inside each art-bearing unit.

### `SAAM` — Smithsonian American Art Museum

Total in unit (with online media): **12,989** (filter: `unit_code:SAAM AND online_visual_material:true`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `ld1-1643381040022-1643381055518-1` | Cat from a Ball Toss Game | Unidentified | ca. 1920 |
| 201 | `ld1-1643381040022-1643381068964-1` | Untitled (Blocks and Bars) | Unidentified | 1930s - 1940s |
| 202 | `ld1-1643381040022-1643381068947-0` | Untitled (Strips) | Unidentified | 1930s |
| 203 | `ld1-1643381040022-1643381055546-1` | Dalmatian | Unidentified | 20th century |
| 204 | `ld1-1643381040022-1643381068950-0` | Untitled (Cross) | Unidentified | 1930s - 1940s |

### `NPG` — National Portrait Gallery

Total in unit (with online media): **14,641** (filter: `unit_code:NPG AND online_visual_material:true`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `ld1-1643399756728-1643399811074-0` | Clara Barton | John Sartain, 24 Oct 1808 - 25 Oct 1897 | 1882 |
| 201 | `ld1-1643399756728-1643399820995-1` | Thomas Badaraque | Charles Balthazar Julien Févret de Saint-Mémin, 12 Mar 1770 - 23 Jun 1852 | 1800 |
| 202 | `ld1-1643399756728-1643399811217-1` | Won't You Let Me Build a Nest For You | Unidentified Artist | 1909 |
| 203 | `ld1-1643399756728-1643399813245-0` | Napoleon Sarony | Napoleon Sarony, 9 Mar 1821 - 9 Nov 1896 | c. 1860-65 |
| 204 | `ld1-1643399756728-1643399814032-1` | Tony Pastor | José María Mora, c. 1847 - 18 Oct 1926 | c. 1880-85 |

### `CHNDM` — Cooper Hewitt, Smithsonian Design Museum

Total in unit (with online media): **54,608** (filter: `unit_code:CHNDM AND online_visual_material:true`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `ld1-1643399887910-1643399896948-1` | Print | (unknown) | — |
| 201 | `ld1-1643399887910-1643399897825-0` | Print | (unknown) | — |
| 202 | `ld1-1643399887910-1643399898136-2` | Print | (unknown) | — |
| 203 | `ld1-1643399887910-1643399897748-1` | Print | (unknown) | — |
| 204 | `ld1-1643399887910-1643399896907-1` | Print | (unknown) | — |

### `NMAI` — National Museum of the American Indian

Total in unit (with online media): **180** (filter: `unit_code:NMAI AND online_visual_material:true`). (unit has only 180 online items — falling back to `start=0`)

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 0 | `ld1-1643403783737-1643403814187-0` | Clamp for making wampum beads | Non-Indian | 1870-1890 |
| 1 | `ld1-1643403783737-1643403843967-0` | Trade token | Non-Indian; probably used by the Diné (Navajo) | — |
| 2 | `ld1-1643403783737-1643403798447-0` | Treaty of Greenville medal | Non-Indian | 1795 |
| 3 | `ld1-1643403783737-1643403849406-0` | Trade token | Non-Indian; probably used by the Diné (Navajo) | — |
| 4 | `ld1-1643403783737-1643403831022-1` | Shell | Non-Indian (archaeological) | 1800–1900 |

### `FSG` — Freer Gallery of Art + Sackler (National Museum of Asian Art)

Total in unit (with online media): **0** (filter: `unit_code:FSG AND online_visual_material:true`). (unit has only 0 online items — falling back to `start=0`)

_No items returned._

### `NMAfA` — National Museum of African Art

Total in unit (with online media): **113** (filter: `unit_code:NMAfA AND online_visual_material:true`). (unit has only 113 online items — falling back to `start=0`)

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 0 | `ld1-1643621402936-1643621404199-1` | Divination tapper | Yoruba artist | 19th century |
| 1 | `ld1-1643621402936-1643621406363-1` | Weight | Akan artist | 15th-early 18th century |
| 2 | `ld1-1643621402936-1643621403494-0` | Icon | Ethiopian Orthodox | 15th-16th century |
| 3 | `ld1-1643621402936-1643621405878-1` | Ring | Inland Niger Delta Style | 13th-14th century |
| 4 | `ld1-1643621402936-1643621406015-1` | Bracelet | Kongo artist | 17th-18th century |

### `HMSG` — Hirshhorn Museum and Sculpture Garden

Total in unit (with online media): **449** (filter: `unit_code:HMSG AND online_visual_material:true`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `ld1-1643399857816-1643399859774-0` | Houses | Louis Eilshemius, American, b. Arlington, New Jersey, 1864–1941 | 1877-1878 |
| 201 | `ld1-1643399857816-1643399859703-1` | Marey Wheel Photographs of George Reynolds | Thomas Eakins, American, b. Philadelphia, Pennsylvania, 1844–1916 | 1884 |
| 202 | `ld1-1643399857816-1643399859699-1` | Marey Wheel Photographs of Unidentified Model | Thomas Eakins, American, b. Philadelphia, Pennsylvania, 1844–1916 | (1884) |
| 203 | `ld1-1643399857816-1643399859734-1` | Jennie Dean Kershaw (Mrs. Samuel Murray) | Thomas Eakins, American, b. Philadelphia, Pennsylvania, 1844–1916 | (c. 1897) |
| 204 | `ld1-1643399857816-1643399861041-0` | Wet Night, Gramercy Park (After Rain; Nocturne) | Ernest Lawson, American, b. Halifax, Nova Scotia 1873–1939 | (1907) |

### `NMAAHC` — National Museum of African American History and Culture

Total in unit (with online media): **4,647** (filter: `unit_code:NMAAHC AND online_visual_material:true`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `ld1-1696063492069-1696063494600-0` | Tintype of a man in bow tie and waistcoat | Unidentified | ca. 1870 |
| 201 | `ld1-1759311857404-1759311859985-1` | Tan T-strap heeled shoes with cut outs | Selby Shoe Co., American, founded 1928 | 1960s-1970s |
| 202 | `ld1-1643398738600-1643398748619-1` | Afro hair comb with black fist design | Eden Enterprise, Inc., American, founded 2002 | 2002-2014 |
| 203 | `ld1-1727784523374-1727784527849-0` | Carte-de-visite of A. G. Cowery | J. H. Emerson, American, 1827 - 1895 | late 1860s-1870s |
| 204 | `ld1-1643398738600-1643398742770-0` | Ambrotype of a young woman holding two white children | Unidentified | ca. 1855 |

## 3. Keyword search

Smithsonian's search uses Solr-style `q=`; the query string AND-concatenates the keyword with the `online_visual_material:true` filter.

### Keyword: `landscape`

Total matches: **9,336**. First 5:

| # | ID | Title | Artist | Unit |
|---:|---|---|---|---|
| 1 | `ld1-1643399887910-1643399894180-1` | Vision of the Cross | Frederic Edwin Church, American, 1826–1900 | `CHNDM` |
| 2 | `ld1-1643399887910-1643399932313-0` | Two Evening Landscapes | Leon Dabo, American, 1868–1960 | `CHNDM` |
| 3 | `ld1-1643399887910-1643399893977-0` | Vision of the Cross | Frederic Edwin Church, American, 1826–1900 | `CHNDM` |
| 4 | `ld1-1643399887910-1643399894837-1` | Landscape Sketches with a House, Ecuador | Frederic Edwin Church, American, 1826–1900 | `CHNDM` |
| 5 | `ld1-1643399887910-1643399893991-1` | Cloudy Landscape | Frederic Edwin Church, American, 1826–1900 | `CHNDM` |

### Keyword: `horse`

Total matches: **4,836**. First 5:

| # | ID | Title | Artist | Unit |
|---:|---|---|---|---|
| 1 | `ld1-1646149545906-1646150241675-1` | Sport 1905 / bearbeitet von Fedor Freund | Freund, Fedor | `SIL` |
| 2 | `ld1-1643406112513-1643406116424-0` | Florida Horse Conch, Florida Horse Conch, Florida Horse Conch, Florida Horse Conch shell | Invertebrate Zoology | `NMNHEDUCATION` |
| 3 | `ld1-1643399887910-1643399894059-1` | Horse Sketches | Frederic Edwin Church, American, 1826–1900 | `CHNDM` |
| 4 | `ld1-1643399756728-1643399816958-1` | William Cody, American Horse, Young Man Afraid of His Horses and Kicking Bear | John Grabill, active 1886 - 1894 | `NPG` |
| 5 | `ld1-1643406112513-1643406113594-1` | Florida Horse Conch, Florida Horse Conch, Florida Horse Conch, Florida Horse Conch shell | Invertebrate Zoology | `NMNHEDUCATION` |

## 4. Downloads

At least **3 images per art-bearing unit** are saved to `output/images/`, plus 3 per keyword. All images requested via IIIF Image API at `https://ids.si.edu/ids/iiif/<idsId>/full/!720,720/0/default.jpg` (fit within 720×720, aspect preserved → longest side is exactly 720).

Filename convention: `{unit_code}_{idsId}.jpg` for unit downloads, `search_{kw}_{idsId}.jpg` for keyword downloads. The user reviews these files visually at the Phase A checkpoint.

| Group | ID | Title | Artist | Rights | File | Dimensions | Bytes | URL |
|---|---|---|---|---|---|---|---:|---|
| unit: SAAM | `ld1-1643381040022-1643381050525-0` | The Old Orchard | Arthur A. Marschner, born Detroit, MI 1884-died Detroit, MI 1950 | CC0 | `—` | ConnectionError: ('Connection aborted.', RemoteDisconnected('Remote end closed connection without response')) | — | — |
| unit: SAAM | `ld1-1643381040022-1643381052268-0` | The Broyling of Their Fish over the Flame of Fier | Spencer Nichols, born Washington, DC 1875-died Kent, CT 1950 | CC0 | `SAAM_SAAM-1985.66.403415_1.jpg` | 583×720 | 145,334 | https://ids.si.edu/ids/iiif/SAAM-1985.66.403415_1/full/!720,720/0/default.jpg |
| unit: SAAM | `ld1-1643381040022-1643381043647-1` | Louisa Catherine Adams Clement | Mary Louisa Adams Clement, born Newbury, MA 1882-died Warrenton, VA 1950 | CC0 | `—` | ConnectionError: ('Connection aborted.', RemoteDisconnected('Remote end closed connection without response')) | — | — |
| unit: NPG | `ld1-1643399756728-1643399813211-0` | Coonley family | J. F. Coonley, 1832 - 1915 | CC0 | `NPG_NPG-A7000100C.jpg` | 439×720 | 127,630 | https://ids.si.edu/ids/iiif/NPG-A7000100C/full/!720,720/0/default.jpg |
| unit: NPG | `ld1-1643399756728-1643399817474-0` | Sun Bird | Max Rosenthal, 23 Nov 1833 - 8 Aug 1918 | CC0 | `NPG_NPG-S_NPG_2001_58_6_ext.jpg` | 473×720 | 179,099 | https://ids.si.edu/ids/iiif/NPG-S_NPG_2001_58_6_ext/full/!720,720/0/default.jpg |
| unit: NPG | `ld1-1643399756728-1643399821305-1` | Unidentified Woman | Charles Balthazar Julien Févret de Saint-Mémin, 12 Mar 1770 - 23 Jun 1852 | CC0 | `NPG_NPG-S_NPG_74_39_16_48Unidentified-000001.jpg` | 629×720 | 172,440 | https://ids.si.edu/ids/iiif/NPG-S_NPG_74_39_16_48Unidentified-000001/full/!720,720/0/default.jpg |
| unit: CHNDM | `ld1-1643399887910-1643399976048-1` | Mug | (unknown) | CC0 | `—` | ConnectionError: ('Connection aborted.', RemoteDisconnected('Remote end closed connection without response')) | — | — |
| unit: CHNDM | `ld1-1643399887910-1643399899840-2` | Print | (unknown) | CC0 | `CHNDM_CHSDM-4A88303D79B62-000001.jpg` | 527×720 | 93,224 | https://ids.si.edu/ids/iiif/CHSDM-4A88303D79B62-000001/full/!720,720/0/default.jpg |
| unit: CHNDM | `ld1-1643399887910-1643399900495-0` | Bound print | Germain Boffrand, French, 1667–1754 | CC0 | `—` | ConnectionError: ('Connection aborted.', RemoteDisconnected('Remote end closed connection without response')) | — | — |
| unit: NMAI | `ld1-1643403783737-1643403814187-0` | Clamp for making wampum beads | Non-Indian | CC0 | `—` | HTTPError: 404 Client Error: Not Found for url: https://ids.si.edu/ids/iiif/ark:/65665/om836d8871ba50c4b23b51e80da791a2d31/full/!720,720/0/default.jpg | — | — |
| unit: NMAI | `ld1-1643403783737-1643403843967-0` | Trade token | Non-Indian; probably used by the Diné (Navajo) | CC0 | `—` | HTTPError: 404 Client Error: Not Found for url: https://ids.si.edu/ids/iiif/ark:/65665/om83f55fc32883e4455b38dc5023b14fbf5/full/!720,720/0/default.jpg | — | — |
| unit: NMAI | `ld1-1643403783737-1643403798447-0` | Treaty of Greenville medal | Non-Indian | CC0 | `—` | HTTPError: 404 Client Error: Not Found for url: https://ids.si.edu/ids/iiif/ark:/65665/om8827adcc47b2a418094be5e779c7b47ab/full/!720,720/0/default.jpg | — | — |
| unit: NMAfA | `ld1-1643621402936-1643621404199-1` | Divination tapper | Yoruba artist | CC0 | `NMAfA_NMAfA-D20170104-000001.jpg` | 323×720 | 62,776 | https://ids.si.edu/ids/iiif/NMAfA-D20170104-000001/full/!720,720/0/default.jpg |
| unit: NMAfA | `ld1-1643621402936-1643621406363-1` | Weight | Akan artist | CC0 | `NMAfA_NMAfA-5FAD762F6D892_03.jpg` | 720×587 | 102,520 | https://ids.si.edu/ids/iiif/NMAfA-5FAD762F6D892_03/full/!720,720/0/default.jpg |
| unit: NMAfA | `ld1-1643621402936-1643621403494-0` | Icon | Ethiopian Orthodox | CC0 | `NMAfA_NMAfA-D20120036-000002.jpg` | 720×568 | 635,592 | https://ids.si.edu/ids/iiif/NMAfA-D20120036-000002/full/!720,720/0/default.jpg |
| unit: HMSG | `ld1-1643399857816-1643399859640-1` | The Rustic Couple (The Peasant and His Wife) | Albrecht Dürer, German, b. Nuremberg, 1471–1528 | CC0 | `HMSG_HMSG-86.1496-000001.jpg` | 520×720 | 233,715 | https://ids.si.edu/ids/iiif/HMSG-86.1496-000001/full/!720,720/0/default.jpg |
| unit: HMSG | `ld1-1643399857816-1643399859687-0` | Elizabeth Macdowell | Thomas Eakins, American, b. Philadelphia, Pennsylvania, 1844–1916 | CC0 | `HMSG_HMSG-HMSG-83.10.jpg` | 720×584 | 166,692 | https://ids.si.edu/ids/iiif/HMSG-HMSG-83.10/full/!720,720/0/default.jpg |
| unit: HMSG | `ld1-1643399857816-1643399861691-0` | Eakins Hand | Samuel Murray, American, b. Philadelphia, Pennsylvania, 1869–1941 | CC0 | `—` | ConnectionError: ('Connection aborted.', RemoteDisconnected('Remote end closed connection without response')) | — | — |
| unit: NMAAHC | `ld1-1768644485034-1768644488010-1` | Cream pencil skirt with pleats | Unidentified | No known copyright restrictions | `NMAAHC_NMAAHC-2007_3_922_001.jpg` | 593×720 | 147,353 | https://ids.si.edu/ids/iiif/NMAAHC-2007_3_922_001/full/!720,720/0/default.jpg |
| unit: NMAAHC | `ld1-1643398738600-1643398743723-0` | Memorandum of enslaved men and women hired or not hired for 1794 | Rouzee Family, American | Public domain | `NMAAHC_NMAAHC-2011_104_11_001.jpg` | 389×720 | 96,095 | https://ids.si.edu/ids/iiif/NMAAHC-2011_104_11_001/full/!720,720/0/default.jpg |
| unit: NMAAHC | `ld1-1643398738600-1643398750564-0` | Flyer advertising film viewings for Katatura and Namibia- A Trust Betrayed | University Christian Foundation of N.Y.U., American | No Known Copyright Restrictions | `NMAAHC_NMAAHC-2015_97_27_31_001.jpg` | 562×720 | 162,602 | https://ids.si.edu/ids/iiif/NMAAHC-2015_97_27_31_001/full/!720,720/0/default.jpg |
| search: landscape | `ld1-1643399887910-1643399894180-1` | Vision of the Cross | Frederic Edwin Church, American, 1826–1900 | CC0 | `search_landscape_CHSDM-1917-4-254-aMattFlynn.jpg` | 720×485 | 142,923 | https://ids.si.edu/ids/iiif/CHSDM-1917-4-254-aMattFlynn/full/!720,720/0/default.jpg |
| search: landscape | `ld1-1643399887910-1643399932313-0` | Two Evening Landscapes | Leon Dabo, American, 1868–1960 | CC0 | `search_landscape_CHSDM-883CDC79207B2-000001.jpg` | 433×720 | 148,301 | https://ids.si.edu/ids/iiif/CHSDM-883CDC79207B2-000001/full/!720,720/0/default.jpg |
| search: landscape | `ld1-1643399887910-1643399893977-0` | Vision of the Cross | Frederic Edwin Church, American, 1826–1900 | CC0 | `—` | ConnectionError: ('Connection aborted.', RemoteDisconnected('Remote end closed connection without response')) | — | — |
| search: horse | `ld1-1643406112513-1643406116424-0` | Florida Horse Conch, Florida Horse Conch, Florida Horse Conch, Florida Horse Conch shell | Invertebrate Zoology | CC0 | `—` | ConnectionError: ('Connection aborted.', RemoteDisconnected('Remote end closed connection without response')) | — | — |
| search: horse | `ld1-1643399887910-1643399894059-1` | Horse Sketches | Frederic Edwin Church, American, 1826–1900 | CC0 | `search_horse_CHSDM-22527_02-000001.jpg` | 720×463 | 81,885 | https://ids.si.edu/ids/iiif/CHSDM-22527_02-000001/full/!720,720/0/default.jpg |
| search: horse | `ld1-1643399756728-1643399816958-1` | William Cody, American Horse, Young Man Afraid of His Horses and Kicking Bear | John Grabill, active 1886 - 1894 | CC0 | `search_horse_NPG-NPG_2010_5Cody1L_int.jpg` | 720×577 | 189,680 | https://ids.si.edu/ids/iiif/NPG-NPG_2010_5Cody1L_int/full/!720,720/0/default.jpg |

## A. Per-unit IIIF coverage

For each art-bearing unit, sample the first 50 hits of `unit_code:{X} AND online_visual_material:true` and report:

- **idsId present**: fraction of hits whose JSON contains a usable `idsId`.
- **info.json OK**: of the first 10 with `idsId`, fraction whose `info.json` returns HTTP 200 (confirming IIIF actually serves the image).

Units with **< 80% on both** are weak integration candidates.

| Unit | Sampled | idsId present | info.json OK (of probed) |
|---|---:|---:|---:|
| `SAAM` | 50 | 50/50 (100%) | 8/10 (80%) |
| `NPG` | 50 | 50/50 (100%) | 8/10 (80%) |
| `CHNDM` | 50 | 50/50 (100%) | 9/10 (90%) |
| `NMAI` | 50 | 50/50 (100%) | 0/10 (0%) |
| `FSG` | 0 | 0/0 (0%) | 0/0 (0%) |
| `NMAfA` | 50 | 50/50 (100%) | 9/10 (90%) |
| `HMSG` | 50 | 50/50 (100%) | 9/10 (90%) |
| `NMAAHC` | 50 | 50/50 (100%) | 9/10 (90%) |

## B. Field-shape audit

Concrete JSON paths the firmware C parser will hardcode. Sample taken from the first SAAM record with `idsId`. If the paths below change in the future, the C adapter must be updated correspondingly.

| Field | JSON path | Sample value |
|---|---|---|
| Object ID | `id` | `ld1-1643381040022-1643381050525-0` |
| Title | `title` | The Old Orchard |
| Artist | `content.freetext.name[*].content` (where label ∈ artist/maker/creator) | Arthur A. Marschner, born Detroit, MI 1884-died Detroit, MI 1950 |
| Date | `content.freetext.date[*].content` | 1915 |
| Rights | `content.freetext.objectRights[*].content` | CC0 |
| Unit code | `content.descriptiveNonRepeating.unit_code` | `SAAM` |
| IIIF ID | `content.descriptiveNonRepeating.online_media.media[*].idsId` | `SAAM-1935.13.211_1` |

Constructed IIIF URL example: `https://ids.si.edu/ids/iiif/SAAM-1935.13.211_1/full/!720,720/0/default.jpg`

Constructed info.json example: `https://ids.si.edu/ids/iiif/SAAM-1935.13.211_1/info.json`

Note: the `online_media.media` field can be either a single object or a list of objects depending on how many media files the record has. The C parser must handle both shapes (mirror what `get_ids_id()` in this script does).

## C. Rate-limit behavior

Sends 30 rapid `rows=0` requests against the search endpoint and records status codes + any `Retry-After` headers. Captures the actual 429 wire format so the firmware adapter knows what to parse.

API key in use for this probe: `(user-provided key, redacted)`. The api.data.gov default for DEMO_KEY is 30 requests/hour/IP (intentionally low); a registered key bumps to 1,000/hour by default.

| Request # | Status | Retry-After |
|---:|---:|---|
| 1 | 200 | — |
| 2 | 200 | — |
| 3 | 200 | — |
| 4 | 200 | — |
| 5 | 200 | — |
| 6 | 200 | — |
| 7 | 200 | — |
| 8 | 200 | — |
| 9 | 200 | — |
| 10 | 200 | — |
| 11 | 200 | — |
| 12 | 200 | — |
| 13 | 200 | — |
| 14 | 200 | — |
| 15 | 200 | — |
| 16 | 200 | — |
| 17 | 200 | — |
| 18 | 200 | — |
| 19 | 200 | — |
| 20 | 200 | — |
| 21 | 200 | — |
| 22 | 200 | — |
| 23 | 200 | — |
| 24 | 200 | — |
| 25 | 200 | — |
| 26 | 200 | — |
| 27 | 200 | — |
| 28 | 200 | — |
| 29 | 200 | — |
| 30 | 200 | — |

No 429 within 30 requests — firmware will still need defensive 429 handling (it's normal for a real BYOK key under heavy refresh load).

## D. Filter A/B — selecting the production query

Counts for one art-bearing unit (SAAM) under three filter strategies. Picks the most restrictive filter that still yields enough content per unit for a healthy channel.

| Filter | SAAM count |
|---|---:|
| `unit_code:SAAM` | 13,779 |
| `unit_code:SAAM AND online_visual_material:true` | 12,989 |
| `unit_code:SAAM AND online_visual_material:true AND usage:CC0` | 0 |

**Recommended production filter** is the strictest one that retains enough content (threshold: ~1,000 items per unit for a healthy channel). Firmware default: `unit_code:{X} AND online_visual_material:true` unless probe D shows that the explicit `AND usage:CC0` clause yields comparable counts (in which case prefer it for rights clarity).

## E. Deep pagination probe

Tests `start` values from 0 up to 10,000 on `unit_code:CHNDM AND online_visual_material:true` (CHNDM has the most online visual items among art units — 54K+, so it's the only one big enough to exercise deep pagination). Confirms whether the API caps deep offsets the way AIC does at 10K.

| start | rowCount | rows returned | first ID |
|---:|---:|---:|---|
| 0 | 54,608 | 3 | `ld1-1643399887910-1643399976048-1` |
| 100 | 54,608 | 3 | `ld1-1643399887910-1643399895606-0` |
| 500 | 54,608 | 3 | `ld1-1643399887910-1643399900332-0` |
| 1,000 | 54,608 | 3 | `ld1-1643399887910-1643399977817-1` |
| 10,000 | 54,608 | 3 | `ld1-1643399887910-1643399891595-1` |

## Run summary

- Art-bearing units surveyed: **8**
- Other units surveyed (comparison): **15**
- Keyword searches: **2** (landscape, horse)
- Downloads attempted: **27** (succeeded: **17**, with longest-side==720: **17**, CC0-tagged: **14**)
- Coverage rows recorded: **8**
- Rate-limit burst: **30** requests, throttle not observed
- Elapsed: **66.1s**
