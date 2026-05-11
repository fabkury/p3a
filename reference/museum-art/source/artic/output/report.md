# AIC demo run

_Generated: 2026-05-07 15:47:29 Eastern Daylight Time_

This run exercises the four UBI features against the AIC API (`https://api.artic.edu/api/v1/artworks/search`): list collections (across **seven** facet axes), list artworks in a collection (range-style pagination via `page`+`limit`), keyword search, and IIIF download at 720px on the longest side.

## 1. Collections

AIC has no single `collection` field. Seven orthogonal vocabularies qualify as 'collections' under the UBI's abstraction. Six of them support artwork-level filtering via the search API's structured `query[term][...]`; **`exhibitions`** is list-only (the artwork side stores `exhibition_history` as free text, not a structured ID).

### Axis: `departments`

_Artwork-level filterable_: **yes** (via `query[term][department_id]`).

| ID | Title |
|---|---|
| `PC-4` | Arts of Greece, Rome, and Byzantium |
| `PC-838` | Modern and Contemporary Art |
| `PC-1` | Arts of Africa |
| `PC-826` | AIC Archives |
| `PC-824` | Ryerson and Burnham Libraries Special Collections |

### Axis: `classifications`

_Artwork-level filterable_: **yes** (via `query[term][classification_id]`).

| ID | Title |
|---|---|
| `TM-11301` | industrial design |
| `TM-11302` | Magazine |
| `TM-11303` | Rendering, hand |
| `TM-11304` | Rendering, computer |
| `TM-11305` | fashion design |

### Axis: `subjects`

_Artwork-level filterable_: **yes** (via `query[term][subject_id]`).

| ID | Title |
|---|---|
| `TM-11203` | swags |
| `TM-11204` | fruit |
| `TM-11205` | trees |
| `TM-11206` | foliage |
| `TM-11210` | tassels |

### Axis: `themes`

_Artwork-level filterable_: **yes** (via `query[term][category_ids]`).

| ID | Title |
|---|---|
| `PC-109` | Art Institute Icons |
| `PC-110` | Silk Road |
| `PC-111` | Recent Acquisitions |
| `PC-142` | African American artists |
| `PC-153` | Portraits |

### Axis: `galleries`

_Artwork-level filterable_: **yes** (via `query[term][gallery_id]`).

| ID | Title |
|---|---|
| `2147480173` | Gallery 109 |
| `2147477236` | Gallery 161 |
| `25238` | Bluhm Family Terrace |
| `2707` | Gallery 57 |
| `2706` | Gallery 58 |

### Axis: `exhibitions`

_Artwork-level filterable_: **no (list-only)**.

| ID | Title |
|---|---|
| `7966` | The Ancient Americas: Art from Sacred Landscapes |
| `10195` | G234 Rotation - May 2023 |
| `10090` | G232 Rotation - January 2023 |
| `10052` | G233A Rotation - January 2023 |
| `10051` | G233 Rotation - July 2022/January 2023 |

### Axis: `artwork-types`

_Artwork-level filterable_: **yes** (via `query[term][artwork_type_id]`).

| ID | Title |
|---|---|
| `49` | TBM Equipment |
| `1` | Painting |
| `23` | Vessel |
| `22` | Basketry |
| `21` | Miniature room |

## 2. Artworks in collections (range pagination)

Demonstrating range-style pagination by requesting `offset=200, rows=5` (i.e. artworks 200–204). The AIC API exposes only `page`+`limit`, so the adapter computes `page = offset/limit + 1 = 41` and `limit = 5`.

_AIC's search caps Elasticsearch results at 10,000 records — beyond that, range queries fail and a browse-by-id strategy is needed. The demo offsets stay well below the cap._

### `departments`: Arts of Greece, Rome, and Byzantium (`PC-4`)

Total in this bucket: **2,195** (filter: `department_id=PC-4`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `9709` | Diobol (Coin) Depicting an Ear of Grain | Ancient Greek | 500-473 BCE |
| 201 | `87663` | Phiale (Shallow Bowl for Pouring Ritual Libations) | Ancient Greek | 300-250 BCE |
| 202 | `28503` | Aryballos (Container for Oil) | Ancient Greek | 625-600 BCE |
| 203 | `4424` | Drachm (Coin) Depicting a Gorgon | Ancient Greek | early 5th century BCE |
| 204 | `10337` | Coin Depicting a Bundle of Twigs | Judean | Hasmonean Dynasty (136–135 BCE), reign of Simon Macccabeus |

### `classifications`: industrial design (`TM-11301`)

Total in this bucket: **65** (filter: `classification_id=TM-11301`). (term has only 65 artworks — falling back to `offset=0` instead of 200)

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 0 | `242628` | Tube Frame Furniture, Fleetwood Furniture Company, Tables and Chairs Design Drawing | Henry P. Glass | 01/02/1955 |
| 1 | `242588` | Upright Piano Designs, Presentation Design Drawing | Henry P. Glass | 1990 |
| 2 | `242765` | Patent Drawing for Baby Doll Toy, Sketch | Meyer/Glass Design | 1967–1988 |
| 3 | `213631` | Fleetwood Furniture Company, Swingline Juvenile Furniture, Toy Chest, Presentation Drawing | Henry P. Glass | c. 1952 |
| 4 | `190645` | American Way Outdoor Dining Furniture for Molla Inc., Wrought Iron Table With Wood Top, Multiple Views | Henry P. Glass | 1940 |

### `subjects`: foliage (`TM-11206`)

Total in this bucket: **71** (filter: `subject_id=TM-11206`). (term has only 71 artworks — falling back to `offset=0` instead of 200)

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 0 | `1875` | Fragment | (unknown) | 1450-1500 |
| 1 | `109644` | Panel (From a Settee) | (unknown) | 1745/55 |
| 2 | `135610` | Unfinished Panel | (unknown) | 1500-1525 |
| 3 | `39146` | Windrush | William Morris | Design 1883, made 1917–25 |
| 4 | `199663` | Aba (Dress for Child or Young Woman) | (unknown) | 19th or early 20th century |

### `themes`: African American artists (`PC-142`)

Total in this bucket: **913** (filter: `category_ids=PC-142`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `142595` | Mama Wata | Radcliffe Bailey | 1995 |
| 201 | `147196` | Hip Hop, Galveston, Texas | Earlie Hudnall, Jr. | 1996 |
| 202 | `55722` | Back Where I, Were Born. 2/20-1888 AD; | Joseph Yoakum | 1965 |
| 203 | `228612` | The Worker | Charles White | 1938/57 |
| 204 | `73906` | Man in Window, New York | Roy DeCarava | 1978 |

### `galleries`: Gallery 58 (`2706`)

Total in this bucket: **44** (filter: `gallery_id=2706`). (term has only 44 artworks — falling back to `offset=0` instead of 200)

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 0 | `2257` | Fragment | Wari | About 600-800 CE |
| 1 | `2061` | Fragment Displaying Stepped-Fret Motif | Nasca | About 200-500 |
| 2 | `13694` | Necklace with a Compartment for Magical Texts | (unknown) | Ottoman dynasty (1299–1923), 18th/19th century |
| 3 | `3000` | Fragment of a Decorative Border | Nasca | 100 BCE-200 CE |
| 4 | `4024` | Tiraz Fragment | (unknown) | Fatimid period (969-1171), 11th century |

### `artwork-types`: Painting (`1`)

Total in this bucket: **3,795** (filter: `artwork_type_id=1`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `9010` | L'Estaque | Émilie Charmy | c. 1910 |
| 201 | `111610` | The Garden of Palazzo Contarini dal Zaffo | Francesco Guardi | Late 1770s |
| 202 | `160222` | Albino | Marlene Dumas | 1986 |
| 203 | `183277` | Landscape | Li Huayi | 2002 |
| 204 | `185222` | Group Pilgrimage to the Jizo Nun | Ike Taiga | 1755/65 |

## 3. Keyword search

### Keyword: `landscape`

Total matches: **4,482**. First 5:

| # | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 1 | `28560` | The Bedroom | Vincent van Gogh | 1889 |
| 2 | `137125` | Many Mansions | Kerry James Marshall | 1994 |
| 3 | `20684` | Paris Street; Rainy Day | Gustave Caillebotte | 1877 |
| 4 | `27992` | A Sunday on La Grande Jatte — 1884 | Georges Seurat | 1884–86, border added 1888–89 |
| 5 | `86385` | City Landscape | Joan Mitchell | 1955 |

### Keyword: `horse`

Total matches: **1,150**. First 5:

| # | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 1 | `151424` | Inventions of the Monsters | Salvador Dalí | 1937 |
| 2 | `15468` | Saint George and the Dragon | Bernat Martorell | 1434–35 |
| 3 | `16146` | Equestrienne (At the Cirque Fernando) | Henri de Toulouse-Lautrec | 1887–88 |
| 4 | `97910` | The Advance-Guard, or The Military Sacrifice (The Ambush) | Frederic Remington | 1890 |
| 5 | `57051` | The Fountains | Hubert Robert | 1787–88 |

## 4. Downloads

Per the demo spec, downloads come from **2 collections** and **2 keyword searches**. All images requested via IIIF Image API at `https://www.artic.edu/iiif/2/<image_id>/full/!720,720/0/default.jpg` (fit within 720×720, aspect preserved → longest side is exactly 720).

Files are saved under `output/images/`.

| Group | ID | Title | Artist | File | Dimensions | Bytes | URL |
|---|---|---|---|---|---|---:|---|
| [departments] Arts of Greece, Rome, and Byzantium | `159136` | Portrait Bust of a Woman | Ancient Roman | `159136.jpg` | 540×720 | 66,499 | https://www.artic.edu/iiif/2/34133244-99a5-246f-c72f-76f32e55d253/full/!720,720/0/default.jpg |
| [departments] Arts of Greece, Rome, and Byzantium | `246` | Oinochoe (Pitcher) | Mattinata Painter | `246.jpg` | 494×720 | 79,023 | https://www.artic.edu/iiif/2/44abac03-4d36-aac6-f44e-ffe1d57dfb03/full/!720,720/0/default.jpg |
| [departments] Arts of Greece, Rome, and Byzantium | `185` | Kylix (Drinking Cup) | Penthesilea Painter | `185.jpg` | 720×551 | 116,902 | https://www.artic.edu/iiif/2/9312601d-c20c-289e-c026-eafc676c5116/full/!720,720/0/default.jpg |
| [classifications] industrial design | `242628` | Tube Frame Furniture, Fleetwood Furniture Company, Tables and Chairs Design Drawing | Henry P. Glass | `242628.jpg` | 720×574 | 75,513 | https://www.artic.edu/iiif/2/b1a31707-4b98-9ac4-01f4-3d56824abc8f/full/!720,720/0/default.jpg |
| [classifications] industrial design | `242588` | Upright Piano Designs, Presentation Design Drawing | Henry P. Glass | `242588.jpg` | 564×720 | 189,806 | https://www.artic.edu/iiif/2/c9696f89-aaa5-0c4a-95e3-ce0e4583ac3c/full/!720,720/0/default.jpg |
| [classifications] industrial design | `242765` | Patent Drawing for Baby Doll Toy, Sketch | Meyer/Glass Design | `242765.jpg` | 446×720 | 73,791 | https://www.artic.edu/iiif/2/256d1d80-a6e0-b8d8-c12d-4adb602fc020/full/!720,720/0/default.jpg |
| search: landscape | `28560` | The Bedroom | Vincent van Gogh | `28560.jpg` | 720×564 | 168,754 | https://www.artic.edu/iiif/2/6644829f-f292-c5c4-a73c-0356a6fdbf0d/full/!720,720/0/default.jpg |
| search: landscape | `137125` | Many Mansions | Kerry James Marshall | `137125.jpg` | 720×614 | 200,435 | https://www.artic.edu/iiif/2/d94d0e3d-5d89-ce07-ee0f-7fa6d8def8ab/full/!720,720/0/default.jpg |
| search: landscape | `20684` | Paris Street; Rainy Day | Gustave Caillebotte | `20684.jpg` | 720×559 | 130,532 | https://www.artic.edu/iiif/2/f8fd76e9-c396-5678-36ed-6a348c904d27/full/!720,720/0/default.jpg |
| search: horse | `151424` | Inventions of the Monsters | Salvador Dalí | `151424.jpg` | 720×466 | 89,559 | https://www.artic.edu/iiif/2/be9551d4-860f-37a0-1408-086617f1824e/full/!720,720/0/default.jpg |
| search: horse | `15468` | Saint George and the Dragon | Bernat Martorell | `15468.jpg` | 449×720 | 152,320 | https://www.artic.edu/iiif/2/8a0e4ac9-43ea-bc3e-884b-ee27f8a10501/full/!720,720/0/default.jpg |
| search: horse | `16146` | Equestrienne (At the Cirque Fernando) | Henri de Toulouse-Lautrec | `16146.jpg` | 720×453 | 112,751 | https://www.artic.edu/iiif/2/65db9e21-83c3-1cc6-7240-1e1996d87f52/full/!720,720/0/default.jpg |

## Run summary

- Facet axes surveyed: **7** (`departments`, `classifications`, `subjects`, `themes`, `galleries`, `exhibitions`, `artwork-types`)
- Filterable axes: **6** of 7
- Collections sampled in detail: **6** ([departments] Arts of Greece, Rome, and Byzantium, [classifications] industrial design, [subjects] foliage, [themes] African American artists, [galleries] Gallery 58, [artwork-types] Painting)
- Collections used for downloads: **2** ([departments] Arts of Greece, Rome, and Byzantium, [classifications] industrial design)
- Keyword searches: **2** (landscape, horse)
- Downloads attempted: **12** (succeeded: **12**, with longest-side==720: **12**)
- Elapsed: **7.8s**
