# Wellcome Collection demo run

_Generated: 2026-05-07 15:59:44 Eastern Daylight Time_

This run exercises the four UBI features against the Wellcome Catalogue API (`https://api.wellcomecollection.org/catalogue/v2/works`): list collections (across four facet axes), list artworks in a collection (range-style pagination via `page`+`pageSize`), keyword search, and IIIF download at 720px on the longest side. All listings are filtered to image-bearing works via `items.locations.locationType=iiif-image`.

## 1. Collections

Wellcome's catalogue is a unified library/archive index. Four facet axes qualify as 'collections' under the UBI's abstraction; all are filterable via direct query parameters:

- **`workType`** — coarse format (Pictures, Digital Images, ...).
- **`genres`** — fine-grained genres (Engraving, Lithography, ...).
- **`subjects`** — what's depicted (Botany, Human anatomy, ...).
- **`contributors`** — creator agents.

All counts below are computed by the Wellcome API's `aggregations=...` parameter, scoped to image-bearing works.

### Axis: `workType`

_Filter parameter_: `workType`.

| Filter value | Label | Count |
|---|---|---:|
| `k` | Pictures | 48,208 |
| `q` | Digital Images | 25,221 |
| `h` | Archives and manuscripts | 9,290 |
| `r` | 3-D Objects | 27 |
| `hdig` | Born-digital archives | 1 |

### Axis: `genres`

_Filter parameter_: `genres.label`.

| Filter value | Label | Count |
|---|---|---:|
| `Engraving and Engravings` | Engraving and Engravings | 10,967 |
| `Etching` | Etching | 7,689 |
| `Portrait prints` | Portrait prints | 7,534 |
| `Lithography` | Lithography | 5,353 |
| `Photograph` | Photograph | 4,833 |

### Axis: `subjects`

_Filter parameter_: `subjects.label`.

| Filter value | Label | Count |
|---|---|---:|
| `Botany` | Botany | 2,056 |
| `Plants` | Plants | 1,843 |
| `Human anatomy` | Human anatomy | 1,584 |
| `Hospitals` | Hospitals | 1,391 |
| `Physicians` | Physicians | 1,172 |

### Axis: `contributors`

_Filter parameter_: `contributors.agent.label`.

| Filter value | Label | Count |
|---|---|---:|
| `ROYAL VETERINARY COLLEGE` | ROYAL VETERINARY COLLEGE | 1,012 |
| `John Thomson` | John Thomson | 599 |
| `Jacques Renaud Benard` | Jacques Renaud Benard | 477 |
| `David Gregory & Debbie Marshall` | David Gregory & Debbie Marshall | 398 |
| `Dr Henry Oakeley` | Dr Henry Oakeley | 366 |

## 2. Artworks in collections (range pagination)

Demonstrating range-style pagination by requesting `offset=200, rows=5` (i.e. works 200–204). Wellcome exposes only `page`+`pageSize`, so the adapter computes `page = offset/pageSize + 1 = 41` and `pageSize = 5`.

### `workType`: Pictures

Total in this bucket: **48,208** (filter: `workType=k`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `a4tzhgfs` | Francis Shillington-Scales. Photograph by J. Palmer Clarke. | (unknown) | — |
| 201 | `a4u7kgru` | Wounded foreigners outside the city walls being picked up by French soldiers and taken to a guarded fortress. Coloured lithograph by G. E... | Hippolyte Lecomte | 1820 |
| 202 | `a4v5w3ax` | Skeleton and écorché figure holding placard featuring male and female figures: half-title page to 'Trattato di anatomia pittorica'. Litho... | Squanquerillo, Costantino. | 1839 |
| 203 | `a4vd4muq` | Pan-Gu, the primordial giant of Chinese myth, holding the symbol of yin and yang. Woodcut, 1850/1900 (?). | (unknown) | 1850-1900 |
| 204 | `a4vdpbvn` | A man in his office is confronted by an innkeepr who presents him with a wine bill, in the presence of other men. Wood engraving. | (unknown) | 1800-1899 |

### `genres`: Engraving and Engravings

Total in this bucket: **10,967** (filter: `genres.label=Engraving and Engravings`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `ad9m3nmb` | Anatomy, medicine and botany; top, kidneys; centre left, bee; centre right, hornet; bottom, Mecca balsam (Commiphora opobalsamum). Colour... | (unknown) | [between 1834 and 1837] |
| 201 | `adbn2hc2` | A saddle, especially made for ladies to ride side-saddle. Engraving. | (unknown) | — |
| 202 | `adc6dvfu` | Sir Francis Drake. Line engraving, 1688. | (unknown) | — |
| 203 | `adddbpp2` | Giovanni Pico della Mirandola [Johannes Picus Mirandulanus]. Line engraving by F.B. Lorieux after L. Beaudouin after a painting attribute... | Lorieux, F. B. | 1808 |
| 204 | `ade5bab6` | Common figwort (Scrophularia nodosa): flowering stem, root and floral segments. Coloured engraving after J. Sowerby, 1805. | James Sowerby | 1 January 1805 |

### `subjects`: Botany

Total in this bucket: **2,056** (filter: `subjects.label=Botany`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `c92pj768` | Ammi (Trachyspermum ammi (L.) Sprague): flowering stem with separate root, flower, fruit and seed. Coloured engraving after F. von Scheid... | Scheidl, Franz Anton von, 1731-1801. | [1772] |
| 201 | `c9724cmb` | Four plant stems with catkins, all from types of willow (Salix species). Chromolithograph by W. Dickes & co., c. 1855. | W. Dickes & Co. | [1855] |
| 202 | `c9smrg48` | Botanic Garden, Oxford: panoramic view of the greenhouses with a small ornamental detail of the gates and plans. Line engraving by J. Ske... | Green, Benjamin, 1739-1798. | 1 August 1820 |
| 203 | `ca5uwea7` | Myrrhis annua: flowering and fruiting stems with separate flower and fruit. Coloured etching by M. Bouchard, 1778. | Bouchard, Magdalena. | [1778] |
| 204 | `caadakyb` | Common or scarlet pimpernel (Anagallis arvensis f. caerulea (Schreber) Baumg.): flowering and fruiting stems with root and separate flora... | Bouchard, Magdalena. | [1774] |

### `contributors`: ROYAL VETERINARY COLLEGE

Total in this bucket: **1,012** (filter: `contributors.agent.label=ROYAL VETERINARY COLLEGE`).

| Index | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 200 | `eq895eau` | A bridle and horse's tongue being held. | ROYAL VETERINARY COLLEGE | — |
| 201 | `eqff8g5j` | Swollen neck in terrapin - vitamin deficient | ROYAL VETERINARY COLLEGE | — |
| 202 | `etfcjdzr` | Dissection: caudal view of perineal region | ROYAL VETERINARY COLLEGE | — |
| 203 | `eu6twsjx` | Section tibial head - copper deficient lamb | ROYAL VETERINARY COLLEGE | — |
| 204 | `eugg6mm3` | Milking cows: double-chambered teat dip cup | ROYAL VETERINARY COLLEGE | — |

## 3. Keyword search

### Keyword: `landscape`

Total matches (with images): **1,414**. First 5:

| # | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 1 | `ughvrc82` | Landscape with large tree. | William Alfred Delamotte | — |
| 2 | `u2r3haxn` | Landscape at Rohtasgarh, Bihar. Coloured aquatint by Thomas Daniell, 1795. | Thomas Daniell | July 1795 |
| 3 | `mr759sh8` | Landscape with fort at Latifpur, Bihar. Coloured etching by William Hodges, 1785. | William Hodges | 7 October 1785 |
| 4 | `ep64mz52` | Landscape view of Bagnères de Bigorre with the Pyrénees in the background. Coloured chromolithograph. | (unknown) | — |
| 5 | `m4mup5en` | Landscape with the fort at Rohtasgarh, Bihar. Coloured aquatint by Thomas Daniell, 1796. | Thomas Daniell | September 1796 |

### Keyword: `horse`

Total matches (with images): **1,623**. First 5:

| # | ID | Title | Artist | Date |
|---:|---|---|---|---|
| 1 | `v4t9q43f` | Horse restrained in horse-box for injection | ROYAL VETERINARY COLLEGE | — |
| 2 | `wb4aqmdf` | Horse doctor giving medicine to a horse, German, 18th century | (unknown) | — |
| 3 | `c3xsp8nx` | Horse foetuses: five figures showing the foetus of a horse during the gestation period, with dissections of its abdomen and stomach demon... | Benjamin Herring II | [1860?] |
| 4 | `c45w8r3m` | Horse handler holding both twitch & lead | ROYAL VETERINARY COLLEGE | — |
| 5 | `dteyrm23` | Horse examination: skin pinch test | ROYAL VETERINARY COLLEGE | — |

## 4. Downloads

Per the demo spec, downloads come from **2 collections** and **2 keyword searches**. All images requested via the Wellcome IIIF Image API at `https://iiif.wellcomecollection.org/image/<vid>/full/!720,720/0/default.jpg` (fit within 720×720, aspect preserved → longest side is exactly 720). The IIIF base URL is recovered from each work's `items[].locations[]` where `locationType.id == 'iiif-image'`.

Files are saved under `output/images/`.

| Group | ID | Title | Artist | File | Dimensions | Bytes | URL |
|---|---|---|---|---|---|---:|---|
| [workType] Pictures | `a22jbu3k` | Queen's College, Oxford: its buildings in the sixteenth century, before their replacement by neoclassical buildings. Wood engraving by J.... | John Jackson | `a22jbu3k.jpg` | 720×598 | 213,223 | https://iiif.wellcomecollection.org/image/V0014156/full/!720,720/0/default.jpg |
| [workType] Pictures | `a22r6556` | Adam Sedgwick. Photograph. | (unknown) | `a22r6556.jpg` | 477×720 | 140,494 | https://iiif.wellcomecollection.org/image/V0027136ER/full/!720,720/0/default.jpg |
| [workType] Pictures | `a22snhqe` | A dragonfly on a lotus flower (Nelumbo species) held above the water. Watercolour. | (unknown) | `a22snhqe.jpg` | 720×632 | 166,743 | https://iiif.wellcomecollection.org/image/V0043838/full/!720,720/0/default.jpg |
| [genres] Engraving and Engravings | `a26dxq5n` | The Kent and Canterbury Hospital, Canterbury. Line engraving by Lester, 1810, after R. Dighton. | Robert Dighton | `a26dxq5n.jpg` | 720×418 | 109,462 | https://iiif.wellcomecollection.org/image/L0007221/full/!720,720/0/default.jpg |
| [genres] Engraving and Engravings | `a2722mkn` | Conrad Gesner. Line engraving, 1666. | (unknown) | `a2722mkn.jpg` | 598×720 | 223,864 | https://iiif.wellcomecollection.org/image/V0002231/full/!720,720/0/default.jpg |
| [genres] Engraving and Engravings | `a28g3jmr` | Saint Thomas of Villanova performs a posthumous miracle: he cures a woman who was about to die. Engraving. | (unknown) | `a28g3jmr.jpg` | 637×720 | 244,010 | https://iiif.wellcomecollection.org/image/V0048151/full/!720,720/0/default.jpg |
| search: landscape | `ughvrc82` | Landscape with large tree. | William Alfred Delamotte | `ughvrc82.jpg` | 720×539 | 236,947 | https://iiif.wellcomecollection.org/image/L0018497/full/!720,720/0/default.jpg |
| search: landscape | `u2r3haxn` | Landscape at Rohtasgarh, Bihar. Coloured aquatint by Thomas Daniell, 1795. | Thomas Daniell | `u2r3haxn.jpg` | 720×546 | 161,574 | https://iiif.wellcomecollection.org/image/V0050464/full/!720,720/0/default.jpg |
| search: landscape | `mr759sh8` | Landscape with fort at Latifpur, Bihar. Coloured etching by William Hodges, 1785. | William Hodges | `mr759sh8.jpg` | 720×500 | 126,682 | https://iiif.wellcomecollection.org/image/V0050412/full/!720,720/0/default.jpg |
| search: horse | `v4t9q43f` | Horse restrained in horse-box for injection | ROYAL VETERINARY COLLEGE | `v4t9q43f.jpg` | 720×471 | 88,108 | https://iiif.wellcomecollection.org/image/A0000654/full/!720,720/0/default.jpg |
| search: horse | `wb4aqmdf` | Horse doctor giving medicine to a horse, German, 18th century | (unknown) | `wb4aqmdf.jpg` | 720×482 | 189,077 | https://iiif.wellcomecollection.org/image/L0019360/full/!720,720/0/default.jpg |
| search: horse | `c3xsp8nx` | Horse foetuses: five figures showing the foetus of a horse during the gestation period, with dissections of its abdomen and stomach demon... | Benjamin Herring II | `c3xsp8nx.jpg` | 720×544 | 136,045 | https://iiif.wellcomecollection.org/image/V0016907/full/!720,720/0/default.jpg |

## Run summary

- Facet axes surveyed: **4** (`workType`, `genres`, `subjects`, `contributors`)
- Collections sampled in detail: **4** ([workType] Pictures, [genres] Engraving and Engravings, [subjects] Botany, [contributors] ROYAL VETERINARY COLLEGE)
- Collections used for downloads: **2** ([workType] Pictures, [genres] Engraving and Engravings)
- Keyword searches: **2** (landscape, horse)
- Downloads attempted: **12** (succeeded: **12**, with longest-side==720: **12**)
- Elapsed: **10.0s**
