# V&A demo run

_Generated: 2026-05-07 14:42:05 Eastern Daylight Time_

This run exercises the four UBI features against the V&A API (`https://api.vam.ac.uk/v2/objects/search`): list collections (across three facet axes), list artworks in a collection (range-style pagination via `page`+`page_size`), keyword search, and IIIF download at 720px on the longest side.

## 1. Collections

V&A doesn't have a single flat *collection* field. Three facet axes qualify as 'collections' under the UBI's abstraction:

- **`collection`** — V&A's curatorial collections (closest analogue to SMK's single `collection` field).
- **`category`** — object-type categories (Prints, Photographs, ...).
- **`venue`** — the six V&A sites (South Kensington, East, Wedgwood, ...).

Top values for each axis (image-bearing records only, except `venue` — see note):

### Axis: `collection`

| ID | Value | Count |
|---|---|---:|
| `THES48595` | Prints, Drawings & Paintings Collection | 310,346 |
| `THES48602` | Theatre and Performance Collection | 81,931 |
| `THES48601` | Textiles and Fashion Collection | 70,944 |
| `THES48596` | East Asia Collection | 64,404 |
| `THES48594` | Ceramics Collection | 55,366 |

### Axis: `category`

| ID | Value | Count |
|---|---|---:|
| `THES48903` | Prints | 104,407 |
| `THES48968` | Designs | 89,034 |
| `THES48966` | Drawings | 82,921 |
| `THES48910` | Photographs | 81,693 |
| `THES48885` | Textiles | 74,579 |

### Axis: `venue`

| ID | Value | Count |
|---|---|---:|
| `VA` | V&A South Kensington | 0 |
| `ES` | V&A East Storehouse | 0 |
| `EM` | V&A East Museum | 0 |
| `WED` | V&A Wedgwood Collection | 0 |
| `YVA` | Young V&A | 0 |

_Note on `venue`_: the V&A search API returns `count=0` for venue terms when `images_exist=1` is also requested (the API doesn't compute that combination). The listing above for `venue` was therefore fetched without the image filter; downloads below re-apply it.

## 2. Artworks in collections (range pagination)

Demonstrating range-style pagination by requesting `offset=200, rows=5` (i.e. artworks 200–204). The V&A API exposes only `page`+`page_size`, so the adapter computes `page = offset/rows + 1 = 41` and `page_size = 5`.

### `collection`: Prints, Drawings & Paintings Collection (`THES48595`)

Total in this bucket (with images): **310,346**.

| Index | System # | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `O1430111` | (Photograph) | (unknown) | — |
| 201 | `O1430109` | (Photograph) | (unknown) | — |
| 202 | `O1430108` | (Photograph) | (unknown) | — |
| 203 | `O1430104` | (Photograph) | (unknown) | — |
| 204 | `O1430091` | (Photograph) | (unknown) | — |

### `category`: Prints (`THES48903`)

Total in this bucket (with images): **104,450**.

| Index | System # | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `O1411335` | Bookplate for Frances Tufnell Tyrell | Nash, Paul | 1910 |
| 201 | `O1411088` | (Endpaper) | Enid Marx | 20th century |
| 202 | `O1411074` | (Endpaper) | Ravilious, Eric | early 20th century |
| 203 | `O1411069` | (Endpaper) | Claud Lovat Fraser | 1911-1921 |
| 204 | `O1410898` | Champagne Charlie | Leybourne, George | ca. 1866 |

### `venue`: V&A South Kensington (`VA`)

Total in this bucket (with images): **738,406**.

| Index | System # | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `O1434987` | (Furnishing fabric) | Pastori e Casanova | c. 1940 |
| 201 | `O1434986` | (Furnishing fabric) | Pastori e Casanova | c. 1940 |
| 202 | `O1434985` | (Furnishing fabric) | Pastori e Casanova | c. 1940 |
| 203 | `O1434984` | (Furnishing fabric) | Pastori e Casanova | c. 1940 |
| 204 | `O1434983` | (Furnishing fabric) | Pastori e Casanova | c. 1940 |

## 3. Keyword search

### Keyword: `landscape`

Total matches (with images): **35,297**. First 5:

| # | System # | Title | Artist | Date |
|---:|---|---|---|---|
| 1 | `O1070459` | Dorset Landscape | Percy Hague Jowett | c.1930 |
| 2 | `O133993` | Landscape with Stream | Goyen, Jan van | 1628 |
| 3 | `O81398` | Landscape near Haarlem | Schelfhout, Andreas | 1839 |
| 4 | `O121179` | Rocky Italian landscape | Castelli, Alessandro | second half of the 19th century |
| 5 | `O126247` | Landscape | Diaz de la Peña, Narcisse Virgile | 1850s |

### Keyword: `horse`

Total matches (with images): **7,832**. First 5:

| # | System # | Title | Artist | Date |
|---:|---|---|---|---|
| 1 | `O1805552` | Horse & Bamboo | Frith, Bob | mid-1980s |
| 2 | `O93494` | Dala horse | Unknown | ca. 1900 |
| 3 | `O138880` | (Rocking horse) | Unknown | ca. 1610 |
| 4 | `O1805549` | The Wish | Frith, Bob | 1987 |
| 5 | `O90345` | (Hobby horse) | Relko Rocking Horses | 1983 |

## 4. Downloads

Per the demo spec, downloads come from **2 collections** and **2 keyword searches**. All images requested via IIIF Image API at `https://framemark.vam.ac.uk/collections/<id>/full/!720,720/0/default.jpg` (fit within 720×720, aspect preserved → longest side is exactly 720). V&A IIIF serves JPEG only.

Files are saved under `output/images/`.

| Group | System # | Title | Artist | File | Dimensions | Bytes | URL |
|---|---|---|---|---|---|---:|---|
| collection [collection]: Prints, Drawings & Paintings Collection | `O1434305` | (Photograph) | (unknown) | `O1434305.jpg` | 720×475 | 82,322 | https://framemark.vam.ac.uk/collections/2018LF6808/full/!720,720/0/default.jpg |
| collection [collection]: Prints, Drawings & Paintings Collection | `O1434204` | (Photograph) | (unknown) | `O1434204.jpg` | 557×720 | 93,769 | https://framemark.vam.ac.uk/collections/2018LH1410/full/!720,720/0/default.jpg |
| collection [collection]: Prints, Drawings & Paintings Collection | `O1434203` | (Photograph) | (unknown) | `O1434203.jpg` | 448×720 | 71,000 | https://framemark.vam.ac.uk/collections/2018LH1332/full/!720,720/0/default.jpg |
| collection [category]: Prints | `O1433404` | Proposed opera house in the Haymarket, London | Emden, Walter | `O1433404.jpg` | 720×495 | 121,743 | https://framemark.vam.ac.uk/collections/2018KT9256/full/!720,720/0/default.jpg |
| collection [category]: Prints | `O1433400` | John Orlando Parry | unknown | `O1433400.jpg` | 514×720 | 80,341 | https://framemark.vam.ac.uk/collections/2018KT9248/full/!720,720/0/default.jpg |
| collection [category]: Prints | `O1432417` | The Stations of the Cross | Villemur | `O1432417.jpg` | 541×720 | 73,882 | https://framemark.vam.ac.uk/collections/2018KR1531/full/!720,720/0/default.jpg |
| search: landscape | `O1070459` | Dorset Landscape | Percy Hague Jowett | `O1070459.jpg` | 516×720 | 67,100 | https://framemark.vam.ac.uk/collections/2017JV2822/full/!720,720/0/default.jpg |
| search: landscape | `O133993` | Landscape with Stream | Goyen, Jan van | `O133993.jpg` | 720×524 | 68,298 | https://framemark.vam.ac.uk/collections/2007BP0756/full/!720,720/0/default.jpg |
| search: landscape | `O81398` | Landscape near Haarlem | Schelfhout, Andreas | `O81398.jpg` | 720×607 | 47,148 | https://framemark.vam.ac.uk/collections/2006AM5687/full/!720,720/0/default.jpg |
| search: horse | `O1805552` | Horse & Bamboo | Frith, Bob | `O1805552.jpg` | 543×720 | 107,401 | https://framemark.vam.ac.uk/collections/2026PM1529/full/!720,720/0/default.jpg |
| search: horse | `O93494` | Dala horse | Unknown | `O93494.jpg` | 720×479 | 41,361 | https://framemark.vam.ac.uk/collections/2006AF4154/full/!720,720/0/default.jpg |
| search: horse | `O138880` | (Rocking horse) | Unknown | `O138880.jpg` | 720×539 | 40,987 | https://framemark.vam.ac.uk/collections/2010ED1443/full/!720,720/0/default.jpg |

## Run summary

- Facet axes surveyed: **3** (`collection`, `category`, `venue`)
- Collections sampled in detail: **3** ([collection] Prints, Drawings & Paintings Collection, [category] Prints, [venue] V&A South Kensington)
- Collections used for downloads: **2** ([collection] Prints, Drawings & Paintings Collection, [category] Prints)
- Keyword searches: **2** (landscape, horse)
- Downloads attempted: **12** (succeeded: **12**, with longest-side==720: **12**)
- Elapsed: **9.3s**
