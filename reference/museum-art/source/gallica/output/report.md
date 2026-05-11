# BnF Gallica demo run

_Generated: 2026-05-07 16:18:51 Eastern Daylight Time_

This run exercises the four UBI features against BnF Gallica: list collections (`dc.type` enum), list artworks in a collection (range-style pagination via `startRecord`+`maximumRecords` — SRU is 1-based), keyword search (`dc.title all`), and IIIF Image v1.1 download at 720px on the longest side.

All requests go through `https://gallica.bnf.fr/SRU`. Gallica returns HTTP 403 to default `requests` / `curl` fingerprints, so the script sends a browser-like User-Agent.

## 1. Collections

Gallica is a digital library, not a museum, so its top-level grouping is the **`dc.type`** enum (Dublin Core type vocabulary). We use the six values that map cleanly to IIIF documents. SRU doesn't expose facet counts directly, so each row here is the result of a 1-record `numberOfRecords` probe.

| `dc.type` value | Label | Count |
|---|---|---:|
| `image` | Images (prints, drawings, photographs) | 829,630 |
| `manuscrit` | Manuscripts | 192,797 |
| `carte` | Maps | 90,378 |
| `monographie` | Monographs (books) | 927,699 |
| `periodique` | Periodicals | 0 |
| `partition` | Sheet music | 79,342 |

## 2. Artworks in collections (range pagination)

Demonstrating range-style pagination by requesting `offset=200, rows=5` (i.e. records 200–204). SRU is 1-based, so the adapter computes `startRecord = offset+1 = 201` and `maximumRecords = 5`.

### `monographie`: Monographs (books)

Total in this bucket: **927,699** (filter: `dc.type all "monographie"`).

| Index | ARK | Title | Creator | Date |
|---:|---|---|---|---|
| 200 | `12148/bpt6k6586987h` | Les tragédies de Robert Garnier,... | Garnier, Robert (1545?-1590). Auteur du texte | 1588 |
| 201 | `12148/bpt6k55000859` | Expériences et découvertes d'un physicien de douze ans / par Victor Clairville | Clairville, Victor. Auteur du texte | 1887 |
| 202 | `12148/bpt6k6315076w` | Étude critique sur quelques points de la physiologie du sommeil, par J.-B. Langlet | Langlet, Jean-Baptiste (1841-1927). Auteur du texte | 1872 |
| 203 | `12148/bpt6k9794179z` | Histoire contemporaine de 1789 à 1848 (4e édition...) / par M. Gustave Hubault,... | Hubault, Gustave (1825-1898). Auteur du texte | 1880 |
| 204 | `12148/bpt6k62609829` | Précis historique des faits authentiques, relatifs à la journée du 18 fructidor, recueillis par le citoyen Veyrat nommé à la place d'insp... | Veyrat, Pierre-Hugues (1756-1839). Auteur du texte | 1798 |

### `image`: Images (prints, drawings, photographs)

Total in this bucket: **829,630** (filter: `dc.type all "image"`).

| Index | ARK | Title | Creator | Date |
|---:|---|---|---|---|
| 200 | `12148/btv1b8409526t` | Habit de Folie : [estampe] | Joullain, François (1697-1778). Graveur | — |
| 201 | `12148/btv1b7740541b` | Mantes. [Vue du Pont-Neuf] : [dessin] / A.te Guillaumot | Guillaumot, Auguste-Alexandre (1815-1892). Dessinateur | 1855 |
| 202 | `12148/btv1b84278936` | La fille de Jephté / F. Francken II ? | (unknown) | 1945-1985 |
| 203 | `12148/btv1b8430273g` | La danse / Flipart ; [d'après] Longhi | (unknown) | 1945-1985 |
| 204 | `12148/btv1b7200650b` | Plans de projets pour un palais à l'encre sans estre lavez [second projet pour le château de Schleissheim; rez-de-chaussée] : [dessin] | (unknown) | 1715 |

### `manuscrit`: Manuscripts

Total in this bucket: **192,797** (filter: `dc.type all "manuscrit"`).

| Index | ARK | Title | Creator | Date |
|---:|---|---|---|---|
| 200 | `12148/btv1b53080734c` | [2 comptes signés, 1792] (manuscrit autographe) | (unknown) | 1792 |
| 201 | `12148/btv1b8419577d` | [1 lettre de P. de Simon à Charles Nuitter, 30 août 1882] (manuscrit autographe) | (unknown) | 1882 |
| 202 | `12148/btv1b53092688v` | [Lettre autographe signée de Rose Caron, cantatrice, à Alphonse Duvernoy (?) (sans lieu ni date)] (manuscrit autographe) | Caron, Rose (1857-1930). Auteur de lettres | — |
| 203 | `12148/btv1b53104836t` | [Lettre de Pierre Marie François de Salles Baillot à Monsieur Mialle, 27 Octobre 1817] (manuscrit autographe) | Baillot, Pierre (1771-1842). Auteur de lettres | 1817 |
| 204 | `12148/btv1b53051102c` | [Lettre de Mario Versepuy à Raoul Blondel, 28 novembre 1921] (manuscrit autographe) | Versepuy, Mario (1882-1972). Auteur de lettres | 1921 |

### `carte`: Maps

Total in this bucket: **90,378** (filter: `dc.type all "carte"`).

| Index | ARK | Title | Creator | Date |
|---:|---|---|---|---|
| 200 | `12148/btv1b10100642r` | 14 Bechiktache Mouradié : Plan cadastral d'assurances / J. Pervititch | Pervititch, Jacques (188.-19..). Fonction indéterminée | 1922 |
| 201 | `12148/btv1b101005715` | 57 : Constantinople vol. III Kadi-Keui / Chas. E. Goad | Goad, Charles E. Fonction indéterminée | 1906 |
| 202 | `12148/btv1b101009027` | Alep | (unknown) | 19.. |
| 203 | `12148/btv1b101017913` | 60 : Constantinople vol. III Kadi-Keui / Chas. E. Goad | Goad, Charles E. Fonction indéterminée | 1906 |
| 204 | `12148/btv1b53147945f` | Tabulation of the principal characteristics of lights and of abbreviations... / International hydrograhic bureau | Organisation hydrographique internationale. Bureau hydrographique international. Éditeur scientifique | 1931 |

### `partition`: Sheet music

Total in this bucket: **79,342** (filter: `dc.type all "partition"`).

| Index | ARK | Title | Creator | Date |
|---:|---|---|---|---|
| 200 | `12148/btv1b52504291n` | Six sonates pour le clavecin / Rigel | (unknown) | — |
| 201 | `12148/bpt6k315567g` | La Chanson des quatre cloches : [4 mélodies : pour chant et piano : op. 194]. 4, Cloches de Noël (Ed. originale en clé de fa) / Marc Delm... | Delmas, Marc (1885-1931). Compositeur | — |
| 202 | `12148/btv1b9062522w` | Trois Sonates pour clavecin ou forte piano... Oeuvre 38e. [P XII 35-37] | Kozeluch, Leopold (1747-1818). Compositeur | 1793 |
| 203 | `12148/bpt6k11615196` | Sonnet d'Arvers et d'art vert. Musique de Mme A. Stratt. Paroles de M. Goudetsky | Stratt, A. (Mme). Compositeur | 1910 |
| 204 | `12148/btv1b90787961` | Deux Sonates pour le clavecin avec accompagnement de violon... Oeuvre II | Schobert, Winceslas (1733-1767). Compositeur | 1770 |

## 3. Keyword search

Search uses `dc.title all "<kw>"`. (`gallica.contenu` and `bib.any` are advertised in some third-party docs but BnF's SRU rejects them with `There are no translation for the following key`.)

### Keyword: `landscape`

Total matches: **71**. First 5:

| # | ARK | Title | Creator | Date |
|---:|---|---|---|---|
| 1 | `12148/bpt6k15234964` | Le Landscape français, France | Sainte-Beuve, Charles-Augustin (1804-1869). Auteur du texte | 1834 |
| 2 | `12148/bpt6k105825m` | Le landscape français. Italie | (unknown) | 1833 |
| 3 | `12148/btv1b532461549` | [Paysage ovale] : [estampe] / I♀V | Maître I♀V (15..-15..). Graveur | 15.. |
| 4 | `12148/btv1b532302583` | [Vue de forêt] : [estampe] / [anonyme] | (unknown) | 15.. |
| 5 | `12148/btv1b8431524b` | Landscape with a procession crossing a bridge / Claude Gellée | (unknown) | 1945-1985 |

### Keyword: `horse`

Total matches: **215**. First 5:

| # | ARK | Title | Creator | Date |
|---:|---|---|---|---|
| 1 | `12148/btv1b10023573f` | [Horse Market] : [estampe] / Pascin | Pascin, Jules (1885-1930). Graveur | 1916 |
| 2 | `12148/btv1b100289549` | An Arabian Horse : [estampe] / J. [sic] Gericault inv.t | Géricault, Théodore (1791-1824). Lithographe | 1821 |
| 3 | `12148/btv1b9004630r` | Shell : for the utlost horse power : [affiche] / [Jean d'Ylen] | Ylen, Jean d' (1886-1938). Illustrateur | 1926 |
| 4 | `12148/btv1b53207718t` | White Horse Aigle [chef Indien] : [photographie de presse] / [Agence Rol] | Agence Rol. Agence photographique (commanditaire) | 1929 |
| 5 | `12148/btv1b53009945m` | Mameluck bridant son Cheval : [estampe] | Levachez. Graveur | 1803 |

## 4. Downloads

Per the demo spec, downloads come from **2 collections** and **2 keyword searches**. All images requested via the IIIF Image API v1.1 at `https://gallica.bnf.fr/iiif/ark:/<ark>/f1/full/!720,720/0/native.jpg` (fit within 720×720, aspect preserved → longest side is exactly 720). Multi-folio Gallica documents (manuscripts, books, periodicals) collapse to **folio 1** for the demo — typically the title page or cover.

Files are saved under `output/images/`.

| Group | ARK | Title | Creator | File | Dimensions | Bytes | URL |
|---|---|---|---|---|---|---:|---|
| [monographie] Monographs (books) | `12148/bpt6k6382082m` | M. T. Ciceronis Brutus, sive de Claris oratoribus. Accedit libellus de Optimo genere oratorum. Recensuit L. Quicherat | Cicéron (0106-0043 av. J.-C.). Auteur du texte | `12148_bpt6k6382082m.jpg` | 458×720 | 32,868 | https://gallica.bnf.fr/iiif/ark:/12148/bpt6k6382082m/f1/full/!720,720/0/native.jpg |
| [monographie] Monographs (books) | `12148/bpt6k6547512m` | M. d'Alais, curé de Paray-le-Monial, sa vie et ses derniers moments : nécrologie / par M. F. Cucherat,... | Cucherat, François (1812-1887). Auteur du texte | `12148_bpt6k6547512m.jpg` | 475×720 | 45,649 | https://gallica.bnf.fr/iiif/ark:/12148/bpt6k6547512m/f1/full/!720,720/0/native.jpg |
| [monographie] Monographs (books) | `12148/bpt6k5667889s` | Chronologie de la maison de Lastic / (par P. de Chazelles) | Chazelles, Pierre-Léon Bérard de (1804-1876). Auteur du texte | `12148_bpt6k5667889s.jpg` | 466×720 | 2,276 | https://gallica.bnf.fr/iiif/ark:/12148/bpt6k5667889s/f1/full/!720,720/0/native.jpg |
| [image] Images (prints, drawings, photographs) | `12148/btv1b7700420q` | Ecu d'or aux croissants | Loosen, Christiaene (1922-2001). Autorité émettrice de monnaie | `12148_btv1b7700420q.jpg` | 720×338 | 42,482 | https://gallica.bnf.fr/iiif/ark:/12148/btv1b7700420q/f1/full/!720,720/0/native.jpg |
| [image] Images (prints, drawings, photographs) | `12148/btv1b8430435g` | Le duo / Juditg Leyster | (unknown) | `12148_btv1b8430435g.jpg` | 556×720 | 64,074 | https://gallica.bnf.fr/iiif/ark:/12148/btv1b8430435g/f1/full/!720,720/0/native.jpg |
| [image] Images (prints, drawings, photographs) | `12148/btv1b8427588v` | Le Parnasse / [d'après] Raphaël ; [gravé] M. A. Raimondi | (unknown) | `12148_btv1b8427588v.jpg` | 720×539 | 95,520 | https://gallica.bnf.fr/iiif/ark:/12148/btv1b8427588v/f1/full/!720,720/0/native.jpg |
| search: landscape | `12148/bpt6k15234964` | Le Landscape français, France | Sainte-Beuve, Charles-Augustin (1804-1869). Auteur du texte | `12148_bpt6k15234964.jpg` | 468×720 | 57,035 | https://gallica.bnf.fr/iiif/ark:/12148/bpt6k15234964/f1/full/!720,720/0/native.jpg |
| search: landscape | `12148/bpt6k105825m` | Le landscape français. Italie | (unknown) | `12148_bpt6k105825m.jpg` | 507×720 | 4,065 | https://gallica.bnf.fr/iiif/ark:/12148/bpt6k105825m/f1/full/!720,720/0/native.jpg |
| search: landscape | `12148/btv1b532461549` | [Paysage ovale] : [estampe] / I♀V | Maître I♀V (15..-15..). Graveur | `12148_btv1b532461549.jpg` | 720×456 | 69,185 | https://gallica.bnf.fr/iiif/ark:/12148/btv1b532461549/f1/full/!720,720/0/native.jpg |
| search: horse | `12148/btv1b10023573f` | [Horse Market] : [estampe] / Pascin | Pascin, Jules (1885-1930). Graveur | `12148_btv1b10023573f.jpg` | 720×489 | 59,885 | https://gallica.bnf.fr/iiif/ark:/12148/btv1b10023573f/f1/full/!720,720/0/native.jpg |
| search: horse | `12148/btv1b100289549` | An Arabian Horse : [estampe] / J. [sic] Gericault inv.t | Géricault, Théodore (1791-1824). Lithographe | `12148_btv1b100289549.jpg` | 720×546 | 38,512 | https://gallica.bnf.fr/iiif/ark:/12148/btv1b100289549/f1/full/!720,720/0/native.jpg |
| search: horse | `12148/btv1b9004630r` | Shell : for the utlost horse power : [affiche] / [Jean d'Ylen] | Ylen, Jean d' (1886-1938). Illustrateur | `12148_btv1b9004630r.jpg` | 720×535 | 58,881 | https://gallica.bnf.fr/iiif/ark:/12148/btv1b9004630r/f1/full/!720,720/0/native.jpg |

## Run summary

- Collections probed: **6** (`image`, `manuscrit`, `carte`, `monographie`, `periodique`, `partition`)
- Collections used for downloads: **2** ([monographie] Monographs (books), [image] Images (prints, drawings, photographs))
- Keyword searches: **2** (landscape, horse)
- Downloads attempted: **12** (succeeded: **12**, with longest-side==720: **12**)
- Elapsed: **25.6s**
