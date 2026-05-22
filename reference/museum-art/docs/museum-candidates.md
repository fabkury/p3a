# Candidate Sources for Museum-Art UBI

*Research conducted 2026-05-07. Endpoints verified against live IIIF services where possible — see "Verification notes" at end.*

## Scope and criteria

The unified browsing interface (UBI) must support three features for every integrated source:

- **List collections** — the source's top-level groupings of artworks.
- **List artworks in a collection, with range-style pagination** — e.g., `artworks 200–249 of collection X`. Range-style requests, not just next-cursor.
- **Keyword search** across artworks.

Hard constraints:

- Source exposes artworks via **IIIF Image API** (v1.1, v2, or v3). IIIF Presentation API (manifests, collections) is strongly preferred since it is the natural carrier for the UBI's "collection" and "artwork listing" concepts.
- **Free** to use. A free API key is acceptable; paid tiers, paywalls, or daily quotas tight enough to break a public-facing UBI are not.
- **Operational** as of 2026-05-07.

Three categories of source qualify and are surveyed below: dedicated art museums, cultural-heritage aggregators, and digital libraries with substantial visual-art holdings.

## Reading the matrix

| Column | Meaning |
|---|---|
| **IIIF Img** | Image API version (v1.1, v2, or v3) |
| **IIIF Pres.** | Presentation API: which manifest version, "no" for image-only, "partial" for inconsistent coverage |
| **Auth** | "none" = anonymous, "key" = free API key required |
| **List/paginate** | offset (range-friendly), cursor (range-emulated only), enum (walk-the-collection only), CSV (bulk harvest), none |
| **Search** | full-text, faceted, IIIF Search, or "none" if site-only |

Tiers:

- **Tier 1** = clean fit for all three UBI features.
- **Tier 2** = at least one feature requires emulation, scraping, or heavy adapter logic.
- **Watch list** = unverified or institutional friction.
- **Excluded** = fails one of the hard constraints.

## Capability matrix

| Source | IIIF Img | IIIF Pres. | Auth | List/paginate | Search | Items w/ images (approx) | Tier |
|---|---|---|---|---|---|---|---|
| SMK (Statens Museum for Kunst, Copenhagen) | v2 | v3 | none | offset | full-text + facets | ~197K | 1 |
| Rijksmuseum (Amsterdam) | v2 | v2/v3 | none | cursor | full-text + facets | ~800K | 1 |
| V&A (Victoria & Albert, London) | v2 | v2 | none | offset (page) | full-text + facets | ~470K | 1 |
| Art Institute of Chicago | v2 | v2 | none (60 rpm) | offset (page ≤ 100) | full-text (10K result cap) | ~120K | 1 |
| Harvard Art Museums | v2 | v2.1 | key (2,500/day) | offset (page) | full-text + facets | ~50K | 1 |
| Wellcome Collection | v2 | v3 | none | offset (page) | full-text + IIIF Search | ~1M | 1 |
| BnF Gallica | v1.1 | v2 | none | offset (SRU `startRecord`) | full-text (SRU) | 10M+ docs | 1 |
| Library of Congress | v2 | v2 | none | offset + page | full-text + facets | ~2M+ visual | 1 |
| J. Paul Getty Museum | v2 | v2 | none | enum only (Activity Streams) | weak (no JSON search API) | ~30K | 2 |
| Smithsonian Open Access | v2 | partial | key | offset (`start`/`rows`) | full-text + Solr-style | art subset of 5.1M | 2 |
| Yale Center for British Art | v2 | v3 | none | LUX paging / OAI-PMH | LUX faceted | ~70K | 2 |
| Princeton University Art Museum | v3 | none (image only) | none | offset (`from`/`size` ≤ 500) | full-text | ~115K | 2 |
| National Gallery of Art (DC) | v2 | v2 (anon 403) | none | bulk CSV harvest | none (UI only) | ~80K | 2 |
| Internet Archive | v3 | v3 | none | rows + cursor | full-text (Lucene) | 5M+ image items | 2 |
| Europeana | v2 | v2 | key | offset (`start`/`rows`) | full-text + facets | image-filtered subset of 50M+ | 2 |
| Digital Bodleian (Oxford) | v2/v3 (negotiated) | v2/v3 | none | walk-the-collection | site only (no JSON search API) | ~800K | 2 |
| Fitzwilliam Museum (Cambridge) | v2 | yes (unverified) | key (60/min) | offset (page) | full-text + facets | ~200K | 2 |
| e-codices (Switzerland) | v2 | v2 | none | walk-the-collection | none (programmatic) | ~700K page images (manuscripts) | 2 |

A wider watch list (Vatican Library, Belvedere, Nationalmuseum Stockholm, Royal Collection Trust, Yale University Art Gallery, Finna.fi, DPLA, Cultural Japan, Stanford SDR) appears below the per-source sections.

---

## Tier 1 — strongest integration fit

These sources support all three UBI features with native, documented endpoints. Adapter work is minimal: parse JSON, map their pagination + search params.

### SMK — Statens Museum for Kunst (Copenhagen)

- **API docs**: <https://api.smk.dk/api/v1/docs/>
- **IIIF Image (v2)**: `https://iip.smk.dk/iiif/jp2/{path}.tif.jp2/{region}/{size}/{rotation}/{quality}.{format}`
- **IIIF Presentation (v3)**: `https://api.smk.dk/api/v1/iiif/manifest?id={objectNumber}` — verified live (`id=KKS5261`)
- **List & paginate**: `/api/v1/art/search?keys=*&offset=0&rows=N` — true offset pagination, range-friendly
- **Search**: full-text via `keys=` plus rich faceting (artist, period, technique)
- **Collections**: a flat per-artwork `collection` field (Royal Print Collection, Royal Coin Cabinet…)
- **Rights**: largely CC0 / public-domain digitized works
- **Why tier 1**: the cleanest fit of any candidate — no key, no quotas, IIIF v3 manifests, real offset pagination, full-text search, ~197K artworks largely public-domain.

### Rijksmuseum (Amsterdam)

- **API docs**: <https://data.rijksmuseum.nl/docs/>
- **IIIF Image (v2)**: served by Micrio — `https://iiif.micr.io/{object-id}/`
- **IIIF Presentation (v2/v3 mixed)**: `https://iiif.micr.io/{object-id}/manifest`
- **List & paginate**: cursor-based via `pageToken` in Linked Art `OrderedCollectionPage`, 100/page; **no offset** — range queries must be emulated client-side via cursor walks plus a local index
- **Search**: faceted via `https://data.rijksmuseum.nl/search/collection`
- **Collections**: Linked Art `HumanMadeObject` records grouped by `memberOfSetId` curated sets
- **Rights**: public-domain works are CC0; otherwise restricted
- **Why tier 1**: ~800K objects in Linked Open Data, anonymous IIIF, well-documented Linked Art / Change Discovery / OAI-PMH pipelines. Cursor-only pagination is the one rough edge — relevant when designing the UBI's pagination contract.

### Victoria & Albert Museum (London)

- **API docs**: <https://developers.vam.ac.uk/>
- **IIIF Image (v2)**: `https://framemark.vam.ac.uk/collections/{id}/{region}/{size}/{rotation}/{quality}.{format}` (JPEG output only)
- **IIIF Presentation (v2)**: `https://iiif.vam.ac.uk/collections/{objectid}/manifest.json`
- **List & paginate**: offset-style via `page` and `page_size` on `https://api.vam.ac.uk/v2/objects/search`
- **Search**: full-text + extensive faceting on the same endpoint
- **Collections**: V&A's six "museums" (V&A South Kensington, V&A East, etc.) act as top-level groupings; below that are departments, materials, places, techniques
- **Rights**: ~470K images reusable under V&A terms; some restricted-but-IIIF-accessible
- **Why tier 1**: ~1.2M catalog records, ~470K with IIIF manifests; clean separation between Image host and manifest host; offset pagination + full-text search natively supported.

### Art Institute of Chicago

- **API docs**: <https://api.artic.edu/docs/>
- **IIIF Image (v2)**: `https://www.artic.edu/iiif/2/{id}/{region}/{size}/{rotation}/{quality}.{format}`
- **IIIF Presentation (v2)**: `https://api.artic.edu/api/v1/artworks/{id}/manifest.json` — verified
- **List & paginate**: page-based (`page`, `limit` ≤ 100); search uses Elasticsearch `from`/`size` with a 10K-record ceiling
- **Search**: `q=` full-text plus Elasticsearch DSL via `query`; faceted filtering
- **Collections**: the richest collection vocabulary in the survey — `departments`, `classifications`, `subjects`, `themes`, `galleries`, `exhibitions`, `artwork-types`. Maps well to a tagged-union "collection" abstraction.
- **Rights**: open-access subset is CC0; IIIF served for public-domain works
- **Why tier 1**: best-documented single museum API. The 10K Elasticsearch result cap is the only sharp edge — beyond that, browse-by-id strategies are needed.

### Harvard Art Museums

- **API docs**: <https://github.com/harvardartmuseums/api-docs>
- **IIIF Image (v2)**: served via Harvard DRS at `https://nrs.harvard.edu/urn-3:HUAM:{id}`
- **IIIF Presentation (v2.1)**: `https://iiif.harvardartmuseums.org/manifests/object/{id}`; collection root at `/collections/top` — both verified
- **List & paginate**: page-based (`page`, `size` ≤ 100); responses include hypermedia next/prev URLs
- **Search**: full-text + faceted across 21+ resource types (Object, Gallery, Classification, Century, Culture, Medium, Technique, Worktype, Place, Period, Color…)
- **Auth**: free API key (form-based registration); IIIF endpoints themselves are anonymous
- **Rights**: mixed; some restricted images remain non-public
- **Why tier 1**: ~250K records, ~50K with images, with the most semantically rich faceting in the survey. The 2,500/day quota is the binding constraint for a public UBI — needs caching.

### Wellcome Collection

- **API docs**: <https://developers.wellcomecollection.org/docs/iiif>
- **IIIF Image (v2)**: `https://iiif.wellcomecollection.org/image/{id}/...`
- **IIIF Presentation (v3)**: `https://iiif.wellcomecollection.org/presentation/{bnumber}` — verified
- **List & paginate**: separate Catalogue API supports `page`+`pageSize` (offset-style)
- **Search**: full-text + faceted via Catalogue API; IIIF Search API also implemented
- **Collections**: top-level IIIF collection at `/presentation/v2/collections` exposes five facets (subject, genre, contributor, digital collection, archives)
- **Rights**: predominantly CC-BY 4.0 / public-domain
- **Why tier 1**: the cleanest IIIF integration of any single institution. Documentation is best-in-class. Strong art holdings (historical medical/scientific prints, paintings, anatomical drawings, photography). Counts as a "library" institutionally but the visual-art subset is large and curated.

### BnF Gallica (Bibliothèque nationale de France)

- **API docs**: <https://api.bnf.fr/api-iiif-de-recuperation-des-images-de-gallica>
- **IIIF Image (v1.1, level 2)**: `https://gallica.bnf.fr/iiif/ark:/12148/{ark}/f{n}/{region}/{size}/{rotation}/{quality}.jpg`
- **IIIF Presentation (v2)**: `https://gallica.bnf.fr/iiif/ark:/12148/{ark}/manifest.json` — verified
- **List & paginate**: SRU API supports `startRecord`/`maximumRecords` — offset/range-friendly
- **Search**: full-text via SRU
- **Collections**: ARK identifiers; collections via the SRU/Gallica search API
- **Rights**: largely public-domain or open licence
- **Why tier 1**: a 10M+ document corpus including the Département des Estampes et de la Photographie (prints, drawings, posters) and rich illuminated manuscripts. The Image API is on the older v1.1 spec — adapter must declare both versions.

### Library of Congress

- **API docs**: <https://www.loc.gov/apis/json-and-yaml/>
- **IIIF Image (v2)**: `https://tile.loc.gov/image-services/iiif/{id}/...`
- **IIIF Presentation (v2)**: manifests embedded in item JSON via `?fo=json`
- **List & paginate**: offset + page (`sp=N&c=N`) on JSON-API
- **Search**: full-text `q=` plus many faceting parameters
- **Collections**: hierarchical, e.g. `/collections/civil-war-maps/`, plus formats and divisions; the World Digital Library was folded into LoC as `/collections/world-digital-library/` in 2021
- **Rights**: mixed; large public-domain set (Prints/Photographs, manuscripts, posters, fine art)
- **Why tier 1**: 30M+ items overall; the Prints/Photographs Online Catalog alone has ~2M+ visual items. Treats art and other media uniformly under one JSON API + IIIF.

---

## Tier 2 — strong but with quirks

These sources support most UBI features natively but require client-side emulation or scraping for at least one. Each entry calls out which.

### J. Paul Getty Museum

- **API docs**: <https://data.getty.edu/museum/collection/docs/>
- **IIIF Image (v2 level 2)**: `https://media.getty.edu/iiif/image/{uuid}`
- **IIIF Presentation (v2)**: `https://media.getty.edu/iiif/manifest/{uuid}` — verified
- **List**: Linked Art (CIDOC-CRM) records over an Activity Streams `OrderedCollection` at `https://data.getty.edu/museum/collection/activity-stream` (~4.5M items, ~45K pages — but most are non-artwork entities; scoping to museum objects required)
- **Paginate**: enum-only (no offset, no cursor) — the UBI must walk the Activity Stream and build a local index to support range pagination
- **Search**: no documented JSON full-text search; `search.getty.edu` is a frontend
- **Why tier 2**: clean IIIF, CC0 open content (~30K open images), but pagination and search both need emulation. A reasonable approach is a periodic cron of the Activity Stream into a local index.

### Smithsonian Open Access (incl. Freer/Sackler, NMAAHC, Cooper Hewitt, NPG, SAAM)

- **API docs**: <https://edan.si.edu/openaccess/docs/>
- **IIIF Image (v2 L2)**: `https://ids.si.edu/ids/iiif/{idsId}/{region}/{size}/{rotation}/{quality}.jpg`. The `{idsId}` is the `idsId` field of any `content.descriptiveNonRepeating.online_media.media[]` entry from a search response — the search payload itself only carries the proprietary delivery-service URL, but the IIIF endpoint resolves for the same id (verified 2026-05-22 with `SAAM-1968.155.176_1`: info.json returns 2354×3000, v2 Level 2)
- **IIIF Presentation (partial)**: per-object manifests exist for IIIF-enabled items but are not consistently surfaced in search-API responses; the UBI must probe per item, or skip Presentation entirely and rely on the derived Image API URL above
- **List & paginate**: `https://api.si.edu/openaccess/api/v1.0/search?start=N&rows=M` — verified; range-friendly
- **Search**: full-text + Solr-style `fqs` filter queries
- **Auth**: free API key from api.data.gov; `DEMO_KEY` works for low-volume testing
- **Collections**: 21 Smithsonian "units" (museums) via `unit_code`; within each unit, departments and topical groupings
- **Why tier 2**: massive scope (~5.1M digital records, mostly non-art) with uneven IIIF Presentation coverage. The free key is api.data.gov-style with default 1,000/hr rate limit.

### Yale Center for British Art

- **API docs**: <https://britishart.yale.edu/collections-data-sharing>
- **IIIF Image (v2 level 2)**: `https://images.collections.yale.edu/iiif/2/{id}`
- **IIIF Presentation (v3)**: `https://manifests.collections.yale.edu/ycba/obj/{id}` — verified
- **List & paginate**: no museum-specific JSON listing endpoint; access via LUX (Yale-wide Linked Art platform at `https://lux.collections.yale.edu`) which paginates Linked Art `OrderedCollectionPage`, **or** OAI-PMH at `https://harvester-bl.britishart.yale.edu/oaicatmuseum/` (LIDO XML, resumption-token only)
- **Search**: LUX faceted full-text across all Yale collections
- **Why tier 2**: rare v3 manifests, ~70K public-domain images with CC0-equivalent rights, and an unusual "discovery via cross-institution platform" architecture. The UBI must either treat LUX as the search backend (which then includes Yale University Art Gallery, Yale Library, Beinecke, etc.) or harvest OAI-PMH.

### Princeton University Art Museum

- **API docs**: <https://github.com/Princeton-University-Art-Museum/puam-api-docs>
- **IIIF Image (v3 level 2)**: `https://media.artmuseum.princeton.edu/iiif/3/collection/{id}` — info.json verified
- **IIIF Presentation**: not provided — image-only IIIF; the UBI must synthesize a manifest client-side from the Image API + metadata, or skip the Presentation layer for this source
- **List & paginate**: offset (`from` + `size` ≤ 500) on `/search`
- **Search**: full-text via `q`, with `type` filter (`artobjects`, `makers`, `packages`, `all`)
- **Collections**: `department` + `classification` on objects; "Packages" = curated thematic groupings (the closest fit to "collection")
- **Why tier 2**: one of the only IIIF Image v3 implementations in the survey, but no Presentation API. This is the most direct test of the UBI's "manifest abstraction" — Princeton supplies images and metadata, the manifest must come from us.

### National Gallery of Art (Washington DC)

- **Open data repo**: <https://github.com/NationalGalleryOfArt/opendata>
- **IIIF Image (v2 L2)**: directly accessible at `https://media.nga.gov/iiif/public/objects/{d1}/{d2}/{d3}/{d4}/{d5}/{id}-primary-0-nativeres.ptif/{region}/{size}/{rotation}/{quality}.{format}`, where `{d1..d5}` are the five digits of the zero-padded object id. Verified 2026-05-22 with object 46126: info.json reports **32266×29579, v2 Level 2** — the highest source resolution of any museum surveyed.
- **IIIF Presentation (v2)**: `https://www.nga.gov/api/v1/iiif/presentation/manifest.json?cultObj:id={id}` — pattern referenced widely (Wikidata, IIIF guides) but **still returns 403 to anonymous fetches** (re-verified 2026-05-22). Practical path: synthesize manifests client-side from per-object metadata in the CSV + the Image API URLs above.
- **List & paginate**: there is no public live JSON search/list API. The de-facto enumeration path is the bulk CSV download from the opendata GitHub repo (~130K artworks, daily refresh). The UBI must ingest the CSV and build a local index.
- **Search**: not a public JSON search API — search lives only in the website UI
- **Why tier 2**: per-object IIIF works (and at outstanding resolution), ~80K open-access CC0 images, but listing and search must both be emulated locally over the CSV dump. Treat as a "bulk-harvest then index" backend rather than a live API.

### Internet Archive

- **API docs**: <https://iiif.archive.org/iiif/documentation>
- **IIIF Image (v3)**: per-canvas, derived from manifest
- **IIIF Presentation (v3)**: `https://iiif.archive.org/iiif/{identifier}/manifest.json`
- **List & paginate**: advanced search supports `rows` + `page` and `cursorMark` (verified: `mediatype:image` returns ~5.5M items)
- **Search**: full-text + Lucene-syntax facets (filter by `licenseurl`, `collection:opensource`, etc. for safe-reuse subsets)
- **Collections**: items belong to one or more "collection" string identifiers; `mediatype:image` filter isolates images
- **Why tier 2**: massive volume but mixed art content — much of IA is books/text. The UBI integration here makes sense only with curated `collection:` filters scoping to art-relevant partner collections.

### Europeana

- **API docs**: <https://pro.europeana.eu/page/apis>
- **IIIF Image (v2, aggregated)**: generated for items with media
- **IIIF Presentation (v2)**: `https://iiif.europeana.eu/presentation/{datasetId}/{localId}/manifest`
- **List & paginate**: Search API supports `start`+`rows` (range-friendly) plus `cursor` for deep paging
- **Search**: full-text + extensive facets (`qf=TYPE:IMAGE`, `qf=REUSABILITY:open`, etc.)
- **Auth**: free API key (account-based registration since May 2025); both Search and Record APIs free
- **Why tier 2**: aggregator across 4,000+ European institutions — covers the long tail of museums (some Prado works, many national libraries) that have no direct API. IIIF manifest quality varies by source institution. Should be wrapped as one UBI source with a clearly marked aggregator semantic.

### Digital Bodleian (Oxford)

- **API docs**: <https://digital.bodleian.ox.ac.uk/developer/iiif/>
- **IIIF Image (v2 + v3 via content negotiation)**: `https://iiif.bodleian.ox.ac.uk/iiif/image/{id}/...`
- **IIIF Presentation (v2 default, v3 via content negotiation)**: `/iiif/manifest/{uuid}.json`; collection endpoints at `/iiif/collection/`
- **List & paginate**: walk-the-collection — no offset endpoint. The UBI must traverse IIIF Collections and emulate range pagination by counting members.
- **Search**: site search; IIIF Search API not universal across items
- **Collections**: explicit IIIF Collections; site organizes by themes (Medieval, Maps, Photographs, Music…)
- **Why tier 2**: 800K+ images, gold-standard manifest hygiene, but pagination is collection-walk-only. Useful as a stress-test for the UBI's "collection traversal" code path.

### Fitzwilliam Museum (Cambridge)

- **API docs**: <https://data.fitzmuseum.cam.ac.uk/api>
- **IIIF Image**: yes (deep-zoom advertised by the API; version likely v2)
- **IIIF Presentation**: yes — manifest URLs surfaced through the data API (e.g., `https://data.fitzmuseum.cam.ac.uk/id/image/iiif/media-{id}`)
- **List & paginate**: JSON-API style (offset + page-size)
- **Search**: full-text + faceted
- **Collections**: Linked Art–based, with departments and object types
- **Auth**: API key required (session cookie or bearer token); 60 req/min standard, 300 req/min for whitelisted IPs (verified 2026-05-22)
- **Rights**: **metadata CC0, but images are CC-BY-NC-ND 4.0** (verified 2026-05-22); 20th-century and in-copyright works are excluded from the API entirely. The ND clause is incompatible with downstream re-encoding/derivative pipelines (a thumbnailer-and-cache deployment, for example).
- **Why tier 2**: ~200K records, Linked Art alignment makes it a code-shareable peer of Rijksmuseum. Direct manifest URL probe still not completed in this research session — *manifest pattern needs verification* before integration. The image-license caveat above is the more serious concern: any caller that caches, re-encodes, or composites Fitzwilliam images is likely outside the licence.

### e-codices (Switzerland)

- **API docs**: <https://www.e-codices.unifr.ch/> (per-collection IIIF endpoints)
- **IIIF Image (v2)**
- **IIIF Presentation (v2)**: `https://www.e-codices.unifr.ch/metadata/iiif/{ms-id}/manifest.json`; top-level collection of collections at `/metadata/iiif/collection.json` — verified (lists 117 sub-collections)
- **List & paginate**: walk-the-collection-tree — no offset API
- **Search**: site-level only; no public search API
- **Collections**: nested IIIF Collections by holding library
- **Rights**: CC BY-NC for most
- **Why tier 2**: one of the first IIIF implementers (2014). Pure walk-the-tree model. Niche (~2,500 manuscripts / ~700K page images) but valuable as a "minimum-IIIF-only" integration target — if the UBI can serve e-codices, it can serve any pure-IIIF source.

---

## Watch list — needs further investigation or verification

- **Vatican Library / DigiVatLib** — strong manuscript collection (~25K items), IIIF v2 verified at `https://digi.vatlib.it/iiif/{shelfmark}/manifest.json`, but no clean cursor/offset listing API and no JSON search; integration would degrade pagination and search. Useful for known-shelfmark fetch.
- **Nationalmuseum (Stockholm)** — ~700K records, large CC0 corpus, IIPImage IIIF server, but documentation is thin and dated (only an Albin Larsson 2020 blog post); manifest URL pattern needs scraping to confirm.
- **Belvedere (Vienna)** — IIIF Presentation present (collection-level endpoint discovered), Open Content Policy with public-domain works free, but no formal developer documentation; per-object manifest URLs likely require scraping the Mirador page.
- **Royal Collection Trust (UK)** — ~300K records, image host at `albert.rct.uk`, but no public REST API and no documented manifest catalog. Tentative; would need direct contact with the institution.
- **Yale University Art Gallery** — sibling of YCBA on the same Yale infrastructure; manifest pattern inferred (`/yuag/obj/{id}`) but not directly verified in this research session.
- **Finna.fi** (National Library of Finland aggregator) — covers ~400 Finnish museums/libraries/archives. Useful as a backstop for Finnish-only institutions like Ateneum that lack their own developer-facing API. Per-museum IIIF coverage varies.
- **DPLA** (Digital Public Library of America) — aggregator over US partner hubs; some items expose `iiifManifest` field but IIIF coverage depends on the contributing hub. Best treated as a fallback alongside direct museum integrations.
- **Cultural Japan** (`https://cultural.jp` / `https://api.cultural.jp/`) — aggregates ~123K Japanese IIIF manifests including ColBase, Tokyo National Museum, and others. Direct OpenAPI probe failed this session; verify before relying on it.
- **Stanford Digital Repository** — IIIF v2/v3 manifests at `https://purl.stanford.edu/{druid}/iiif/manifest`, but listing/search is via SearchWorks (catalog-search interface, not a clean machine API). Useful for known-druid retrieval, not for browsing.

---

## Excluded (and why)

### North American art museums

- **Metropolitan Museum of Art** — public REST API is excellent and CC0, but image delivery is plain CDN URLs (`images.metmuseum.org/CRDImages/...`). No IIIF Image API and no manifests exposed.
- **Cleveland Museum of Art** — Open Access API is CC0 and rich, but image delivery is CDN-only (web/print/full TIFF URLs). No IIIF Image API and no manifests.
- **Walters Art Museum** — v1 API closed in 2023; v2 not online as of 2026.
- **MoMA** — `api.moma.org` is staff/partner-only; no public auth.
- **Brooklyn Museum** — OpenCollection API exists with API-key registration, but no documented IIIF support and the registration endpoint failed verification.
- **Whitney, LACMA, RISD** — public REST APIs exist for some, but none expose IIIF.
- **Indianapolis Museum of Art / Newfields** — IIIF server "in development" per Local Contexts grant (2023–2024), no operational public IIIF endpoint as of 2026. Watch list.
- **National Gallery of Canada** — no public IIIF endpoint or API found.

### European art museums

- **Mauritshuis** — no public developer API; Adlib + Umbraco backend. Reachable only via Europeana.
- **Tate** — no IIIF; metadata-only GitHub dataset abandoned 2014; images explicitly excluded.
- **British Museum** — IIIF tile server exists at `media.britishmuseum.org/iiif/...` but no documented developer API, search, or manifest catalog.
- **National Gallery (London)** — IIIF server exists (`media.ng-london.org.uk`) and a "Simple IIIF Discovery" research project, but the discovery wiki returned 403 and no documented manifest/list/search API for general use.
- **Louvre, Centre Pompidou, Musée d'Orsay, Uffizi, Vatican Museums (paintings, not the Library), Prado, Reina Sofía, Thyssen-Bornemisza, Hermitage, Pushkin, Munch, Albertina, KHM Wien, Pinakothek/Bayerische Staatsgemäldesammlungen** — none exposes a public IIIF endpoint with a documented programmatic listing/search API. Pinakothek images go through Cloudinary, not IIIF. Hermitage also has geopolitical-access concerns post-2022.
- **Kunstmuseum Basel** — uses IIIF internally for its viewer and offers per-object manifest downloads, but the public collection website is an eMuseumPlus legacy parameter UI with no documented search or list API. Borderline; exclude unless willing to scrape.

### Digital libraries / aggregators / non-Western

- **NYPL Digital Collections** — Repo API deprecated August 2026 with no public replacement; do not build new integration on it.
- **British Library** — partial recovery from 2023 cyberattack; ~3K manuscripts back online by late 2024 but full IIIF service still degraded; revisit later in 2026.
- **Trove / National Library of Australia** — March 2025 restrictions explicitly forbid using the API to extract full content.
- **HathiTrust** — primarily text/book corpus; OAuth required; no clean IIIF Presentation API for the full corpus. Out of art scope.
- **National Diet Library (Japan)** — IIIF on individual items but no documented programmatic listing API in English; predominantly text/book corpus. Discovery via Cultural Japan (watch list).
- **Wikimedia Commons** — no official IIIF service; the `wmflabs` zoomviewer proxy is unmaintained.
- **Wikidata** — no native IIIF; only references via `P6108`/`P6109`. Useful as metadata enrichment, not a primary source.
- **World Digital Library** — wdl.org archived 2021; collection lives inside LoC as `/collections/world-digital-library/`.
- **Trinity College Dublin** — IIIF behind a CAPTCHA-protected frontend; no documented public API.
- **Tokyo National Museum / e-Museum / ColBase** — IIIF manifests on individual object pages but no documented public listing/search API. Discovery is via Cultural Japan (watch list).
- **National Palace Museum (Taiwan), National Museum of Korea** — no documented IIIF API.
- **Te Papa Tongarewa (NZ)** — Collections API and IIIF-style image URLs exist, but does not expose standard IIIF Presentation manifests per documentation.

---

## Cross-cutting observations on the design tension

The capability matrix splits cleanly along the central CLAUDE.md tension (`reconciling heterogeneous capabilities`).

### Pagination — three regimes coexist

- *Offset/range native*: SMK, V&A, AIC, Harvard, Wellcome, BnF, LoC, Princeton, Smithsonian, Internet Archive, Europeana, Fitzwilliam.
- *Cursor only*: Rijksmuseum (and Internet Archive's cursorMark mode).
- *Walk-the-collection / enum only*: Getty, Bodleian, e-codices, Vatican Library; NGA-DC is a special case (bulk CSV).

The UBI's pagination contract probably needs to express both regimes — e.g., always-supported range pagination plus an `accuracy` flag indicating whether the answer comes from the upstream live or from an internal index that may lag.

### Search — three regimes

- *Full-text + facets, JSON*: SMK, Rijksmuseum, V&A, AIC, Harvard, Wellcome, BnF SRU, LoC, Princeton, Smithsonian, Europeana, IA.
- *Faceted only via cross-institution layer*: YCBA via LUX.
- *None / site-only*: Getty, NGA-DC, Bodleian, e-codices, Vatican Library.

The "no native search" tier requires a local search index — the same internal index serving emulated pagination can serve search for free. This argues for an internal indexer service from day one, even though some sources don't need it.

### Collections semantics — wide variation

- Department-based: V&A, Princeton (excluded museums Cleveland, Met use this too).
- Multi-vocabulary tagged-union: AIC (departments + categories + galleries + exhibitions + artwork-types), Harvard (21+ vocabularies).
- Linked Art entity-of-type: Getty, Rijksmuseum, YCBA via LUX, Fitzwilliam.
- Curated set / "Packages": Princeton.
- Source unit code: Smithsonian (`unit_code` per museum).
- Hierarchical collections-tree: LoC, Bodleian, e-codices.
- Aggregator dataset: Europeana, DPLA.
- Flat per-object tag: SMK (`collection` field).

A pragmatic UBI shape: `Collection` is an opaque ID + display name + ordered membership; the per-source adapter maps the source's native concept (department, category-term, set, dataset, etc.) onto this shape. The ID space is namespaced per source (`smk:royal-print`, `aic:dept-1`, `harvard:classification-painting`, etc.).

### IIIF version split

- Image v1.1: BnF Gallica (alone — adapter must handle older spec).
- Image v2: most sources.
- Image v3: Princeton, Internet Archive, Bodleian (negotiable).
- Presentation v3: SMK, YCBA, Wellcome, Internet Archive, Bodleian (negotiable).

The IIIF client layer must accept v1.1, v2, and v3 image services and v2/v3 manifests. The IIIF community libraries (`@iiif/parser`, `iiif-prezi3` server-side) handle this if used.

**No Presentation API at all:** Princeton (image-only). The UBI must either *synthesize* a manifest from per-object metadata + image services, or accept "Presentation API: not provided" as a per-source capability flag and surface it in the public API.

### Aggregator vs single-museum semantics

Europeana and DPLA aggregate hundreds of institutions each. If both are integrated alongside individual museums (V&A, Rijksmuseum, etc.), there will be duplicate records. The UBI either deduplicates by source-institution metadata or treats aggregator-only items as a clearly marked "via aggregator" subset.

---

## Open questions

1. **Search emulation tolerance**: for sources with no native JSON search (Getty, NGA-DC, Bodleian, e-codices, Vatican Library), is client-side search emulation over a periodically-harvested local index acceptable for v1, or should they be deprioritized to "browse-only" sources?
2. **Aggregator status**: should Europeana and DPLA be first-class sources alongside individual museums (with deduplication risk) or treated only as fallbacks for museums that lack a direct API?
3. **Manuscript-heavy sources**: e-codices, Vatican Library, BnF Gallica's Estampes-vs-other split. Are illuminated manuscripts and book illustrations in scope as "art", or only painting/print/sculpture/drawing?
4. **Range pagination contract**: should the UBI's public API express range pagination as "always-supported, possibly emulated" (with an `accuracy: live | indexed | stale` hint) or as "native | unsupported" with caller responsibility?
5. **Starter set**: for the first integration milestone, which 2-3 sources should we pick as the proving ground? My suggestion: **SMK** (cleanest fit, all features native), **Art Institute of Chicago** (richest collection vocabulary, well-documented), and **Getty** (forces the emulation paths early — Activity Streams pagination + no native search). This combination forces the UBI to handle native, semi-native, and fully-emulated sources in v1.
6. **Stepping-stone goal**: this project feeds a follow-on that displays artworks at a particular pixel size. IIIF Image API supports arbitrary `{size}` requests, so this is fundamentally fine, but max source resolution varies by museum (Getty publishes >100 megapixel originals; some sources cap at 4K). Should the UBI surface a `max-resolution` capability per artwork, or normalize to a guaranteed-floor (e.g., always at least 2048px on the long side)?

---

## Verification notes

The following endpoints were confirmed live during research on 2026-05-07 (manifest or info.json fetched, parsed, validated):

- Art Institute of Chicago — `/api/v1/artworks/24645/manifest.json` (Presentation v2)
- Harvard Art Museums — `iiif.harvardartmuseums.org/manifests/object/299843` (Presentation v2.1) and collection root `/collections/top`
- J. Paul Getty Museum — `media.getty.edu/iiif/manifest/53be857e-41e8-4198-b45d-2e0f52d3051b` (Presentation v2); Activity Streams at `data.getty.edu/museum/collection/activity-stream`
- Yale Center for British Art — `manifests.collections.yale.edu/ycba/obj/499` (Presentation v3)
- Princeton University Art Museum — `media.artmuseum.princeton.edu/iiif/3/collection/y1975-17/info.json` (Image v3)
- Smithsonian — `api.si.edu/openaccess/api/v1.0/search` (with `DEMO_KEY`)
- SMK — `api.smk.dk/api/v1/iiif/manifest?id=KKS5261` (Presentation v3); image info at `iip.smk.dk/iiif/jp2/qz20sx771_kks5261.tif.jp2/info.json`
- V&A — `iiif.vam.ac.uk/collections/O9138/manifest.json`
- Rijksmuseum — `iiif.micr.io/RFwqO/manifest`
- BnF Gallica — `gallica.bnf.fr/iiif/ark:/12148/bpt6k9907264/manifest.json`
- Wellcome Collection — `iiif.wellcomecollection.org/presentation/b18035723`
- Internet Archive — search confirmed `mediatype:image` returns ~5.5M items
- Vatican Library — `digi.vatlib.it/iiif/MSS_Vat.ar.319/manifest.json`
- e-codices — `e-codices.unifr.ch/metadata/iiif/collection.json` (top-level Collection of 117 sub-collections)

Endpoints that *failed* anonymous fetch in this session but are documented as live (returned 403, likely client-fingerprint policies — re-verify with proper headers): NGA-DC manifest, LoC item JSON, DPLA registration endpoint, Royal Collection Trust image host, Cultural Japan OpenAPI.

Negative verifications (confirmed *no* IIIF): Met Museum (sample object 436535 returns CDN URLs, no IIIF service); Cleveland Museum of Art (sample artwork response has no IIIF service field).

### Follow-up verifications (2026-05-22)

Spot-checks that update Tier 2 entries:

- **NGA-DC Image API** — `media.nga.gov/iiif/public/objects/0/4/6/1/2/6/46126-primary-0-nativeres.ptif/info.json` returned a valid v2 Level 2 info.json (32266×29579, JPEG, native/color/gray, 256-tile pyramid). The image-level IIIF is fully live and exceptionally high resolution; only the per-object Presentation manifest is broken anonymously. NGA Presentation manifest endpoint **re-verified still 403** on the same date.
- **Smithsonian IIIF Image** — `ids.si.edu/ids/iiif/SAAM-1968.155.176_1/info.json` returned a valid v2 Level 2 info.json (2354×3000, JPG). Confirms that the IIIF endpoint resolves directly from the `idsId` carried in search responses, even though the search API itself only exposes the proprietary delivery-service URL. Sample search call `unit_code:SAAM` with `DEMO_KEY` returned items with `content.freetext.objectRights[0].content = "CC0"`.
- **Getty Activity Stream** — re-confirmed live: 4,537,822 items across 45,428 pages (~100 items per page). Same shape as 2026-05-07.
- **Fitzwilliam Museum** — `data.fitzmuseum.cam.ac.uk/api` confirms IIIF deep-zoom support, 60 req/min anonymous rate limit, **CC-BY-NC-ND on images and CC0 on metadata**; 20th-century / in-copyright works excluded. Manifest URL pattern still not directly probed.
