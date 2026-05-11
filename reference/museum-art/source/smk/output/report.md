# SMK demo run

_Generated: 2026-05-07 12:25:10 Eastern Daylight Time_

This run exercises the four UBI features against the SMK API: list collections, list artworks in a collection (range pagination), keyword search, and IIIF download at 720px on the longest side.

## 1. Collections

| Collection | Count |
|---|---:|
| Gammel bestand | 13,354 |
| Niels Larsen Stevns samling | 2,363 |
| Richard Mortensens samling | 1,980 |
| Originalgrafik | 1,411 |
| Spenglers fortegnelse | 811 |
| Stroes fortegnelse | 455 |
| Herbert Melbyes samling | 252 |
| Blochs fortegnelse | 172 |
| (blank) | 24 |

## 2. Artworks in collections (range pagination)

Demonstrating range-style pagination by requesting `offset=200, rows=5` (i.e. artworks 200-204).

### Gammel bestand

Total in collection (with images): **9,121**.

| Index | Object # | Title | Artist |
|---:|---|---|---|
| 200 | `KKSgb21518` | Bakkus som vinens gud | Matham, Jacob |
| 201 | `KKSgb21517` | Merkur overdrager Bakkus til nymferne fra Nysa-bjerget | Serwouters, Pieter |
| 202 | `KKSgb21516` | Bakkus' fødsel | Matham, Jacob |
| 203 | `KKSgb21512` | Venus og Amor sover og bliver iagttaget af satyrer | Matham, Jacob |
| 204 | `KKSgb21514` | Et barn, der spiller tamburin | Matham, Jacob |

### Niels Larsen Stevns samling

Total in collection (with images): **1,883**.

| Index | Object # | Title | Artist |
|---:|---|---|---|
| 200 | `KKSnls491/59` | Notater vedrørende Skipper Klementes opstand og slaget ved Svendstrup mose. Afskrift efter Vendsyssel Aarbøger 1925 | Larsen Stevns, Niels |
| 201 | `KKSnls491/58` | Notater vedrørende Skipper Klementes opstand og slaget ved Svendstrup mose. Afskrift efter Vendsyssel Aarbøger 1925 | Larsen Stevns, Niels |
| 202 | `KKSnls491/57` | Notater vedrørende Skipper Klementes opstand og slaget ved Svendstrup mose. Afskrift efter Vendsyssel Aarbøger 1925 | Larsen Stevns, Niels |
| 203 | `KKSnls491/56` | Notater vedrørende Skipper Klementes opstand og slaget ved Svendstrup mose. Afskrift efter Vendsyssel Aarbøger 1925 | Larsen Stevns, Niels |
| 204 | `KKSnls491/55` | Notater vedrørende Skipper Klementes opstand og slaget ved Svendstrup mose. Afskrift efter Vendsyssel Aarbøger 1925 | Larsen Stevns, Niels |

## 3. Keyword search

### Keyword: `landscape`

Total matches (with images): **18,743**. First 5:

| # | Object # | Title | Artist | Collection |
|---:|---|---|---|---|
| 1 | `KKS2020-29` | Landscape | Lemmerz, Christian | — |
| 2 | `KKS2026-3` | Landskab | Johnson, William H. | — |
| 3 | `KKS2004-95` | Valley Landscape | Marin, John | — |
| 4 | `KKS2002-25` | Landscape, Pt. Reyes | Eastman, Mari | — |
| 5 | `KKS1986-100` | "Painting with Italian landscape" | Wörsel, Troels | — |

### Keyword: `horse`

Total matches (with images): **78**. First 5:

| # | Object # | Title | Artist | Collection |
|---:|---|---|---|---|
| 1 | `KMS7988/4` | Jason's Horse | Bonde, Peter | — |
| 2 | `KKSgb12522` | Horsens | Lange, Søren L. | — |
| 3 | `KKS2719` | Horsens Fjord | Hou, Axel | — |
| 4 | `KKSgb12513` | Bygholm ved Horsens | Lange, Søren L. | — |
| 5 | `KKSgb10447` | Horsens | Lode, Alexia de | — |

## 4. Downloads

All images requested via IIIF Image API with size `!720,720` (fit within a 720x720 box, aspect ratio preserved -> longest side is exactly 720).

Files are saved under `output/images/`.

| Group | Object # | Title | Artist | File | Dimensions | Bytes | URL |
|---|---|---|---|---|---|---:|---|
| collection: Gammel bestand | `KKSgb22235` | Andromeda | Swanenburgh, Willem van | `KKSgb22235.jpg` | 559×720 | 466,897 | https://iip.smk.dk/iiif/jp2/bc386p50w_kksgb22235.tif.jp2/full/!720,720/0/default.jpg |
| collection: Gammel bestand | `KKSgb1404` | Deborah | Saenredam, Jan | `KKSgb1404.jpg` | 525×720 | 331,232 | https://iip.smk.dk/iiif/jp2/4f16c739h_kksgb1404.tif.jp2/full/!720,720/0/default.jpg |
| collection: Gammel bestand | `KKSgb22234` | Bakkus serverer for et ældre par. Vinter | Saenredam, Jan | `KKSgb22234.jpg` | 516×720 | 389,694 | https://iip.smk.dk/iiif/jp2/9g54xn814_kksgb22234.tif.jp2/full/!720,720/0/default.jpg |
| collection: Niels Larsen Stevns samling | `KKSnls320` | Den helbredte, knælende mod jorden | Larsen Stevns, Niels | `KKSnls320.jpg` | 720×498 | 297,091 | https://iip.smk.dk/iiif/jp2/js956j509_kksnls320.tif.reconstructed.tif.jp2/full/!720,720/0/default.jpg |
| search: landscape | `KKS2020-29` | Landscape | Lemmerz, Christian | `KKS2020-29.jpg` | 720×600 | 223,806 | https://iip.smk.dk/iiif/jp2/mg74qq501_KKS2020-29.TIF.jp2/full/!720,720/0/default.jpg |
| search: landscape | `KKS2026-3` | Landskab | Johnson, William H. | `KKS2026-3.jpg` | 720×502 | 345,785 | https://iip.smk.dk/iiif/jp2/6d570238v_kks2026_3.tif.jp2/full/!720,720/0/default.jpg |
| search: landscape | `KKS2004-95` | Valley Landscape | Marin, John | `KKS2004-95.jpg` | 720×480 | 281,745 | https://iip.smk.dk/iiif/jp2/6t053g530_KKS2004-95.TIF.jp2/full/!720,720/0/default.jpg |
| search: horse | `KKSgb12522` | Horsens | Lange, Søren L. | `KKSgb12522.jpg` | 720×577 | 252,281 | https://iip.smk.dk/iiif/jp2/fn107176m_kksgb12522.tif.reconstructed.tif.jp2/full/!720,720/0/default.jpg |
| search: horse | `KKS2719` | Horsens Fjord | Hou, Axel | `KKS2719.jpg` | 720×565 | 229,684 | https://iip.smk.dk/iiif/jp2/pn89d893s_kks2719.tif.reconstructed.tif.jp2/full/!720,720/0/default.jpg |
| search: horse | `KKSgb12513` | Bygholm ved Horsens | Lange, Søren L. | `KKSgb12513.jpg` | 720×547 | 245,211 | https://iip.smk.dk/iiif/jp2/0z708z95c_kksgb12513.tif.reconstructed.tif.jp2/full/!720,720/0/default.jpg |

## Run summary

- Collections discovered: **9**
- Collections sampled in detail: **2** (Gammel bestand, Niels Larsen Stevns samling)
- Keyword searches: **2** (landscape, horse)
- Downloads attempted: **10** (succeeded: **10**, with longest-side==720: **10**)
- Elapsed: **7.8s**
