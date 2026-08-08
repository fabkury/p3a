# Could IIIF Presentation API have served as p3a's artwork-discovery layer?

Research memo for the Code4Lib Journal article. **Probe date: 2026-08-07.** All probes
were live public GETs (curl, UA `p3a-research/1.0 (pub@kury.dev)`), 2–6 requests per
museum, sequential. Byte sizes are exact `size_download` values from that date.
Anything not directly observed is marked **UNVERIFIED**.

**Research question.** p3a's shipped architecture uses each museum's bespoke search API
for discovery (enumerate a filtered collection with total counts at arbitrary offset,
100-ish records per page with inline image identifiers) and the IIIF *Image* API only
for pixels. Could IIIF *Presentation* (Collections + Manifests) have replaced the
bespoke discovery layer?

**Short answer: no** — it fails independently at three levels: adoption (only 2 of 7
museums publish any collection-level Presentation document, and both broke at the
enumeration level when probed), specification (Presentation 3.0 has no paging, no
counts, no offsets, no facets, and declares discovery out of scope), and client cost
(replacing one ~14–185 KB search page with 50–100 manifest fetches is a 50–100×
request multiplier on a microcontroller). Details below.

---

## 1. What the discovery layer must actually do (from the shipped code)

From `components/art_institution/` (adapters in `museums/*.c`):

1. **Faceted browse**: list a museum's axes and terms (AIC: departments,
   classifications, subjects, themes, galleries, artwork-types — `artic.c`
   `AIC_AXES[]`) so the web UI can offer channels.
2. **Filtered enumeration with counts**: "all artworks *with images* in term T",
   with a total, so the device can show progress and modulo-wrap user-chosen
   channel offsets back into range.
3. **Arbitrary-offset access**: the per-channel offset feature starts a channel at
   artwork N. Shipped mappings: AIC/V&A/Wellcome/HAM `page = offset/100 + 1`
   (V&A caps at page x page_size <= 10 000), SMK a literal `offset=` parameter,
   Rijksmuseum **cursor-only** (see below).
4. **Inline image identifiers**: each listing record must carry the IIIF image id so
   one page of 100 records yields 100 downloadable image URLs with no further
   metadata requests.
5. **Bounded parse cost**: response buffers are fixed (192 KB for AIC/Rijks pages),
   parsed with cJSON in PSRAM on an ESP32-P4.

---

## 2. Per-museum probe results

Legend for the four questions: **(a)** machine-discoverable IIIF Collection document;
**(b)** per-artwork manifest; **(c)** could Presentation documents alone enumerate
"all artworks in collection X, with totals, at arbitrary offset"; **(d)** IIIF
Content Search advertised.

| Museum | (a) Collection doc | (b) Per-artwork manifest | (c) Enumerate + counts + offset | (d) Content Search |
|---|---|---|---|---|
| Art Institute of Chicago | **No** | **Yes** (P2, 2 494 B) | No | No |
| Rijksmuseum | **No** (Linked Art/AS2 instead) | **Yes** (Micrio, P3, 1 599 B, no metadata) | Counts yes / offset no — and via AS2, not Presentation | No |
| Victoria & Albert | **No** (root 404) | **Yes** (P2, 2 903 B) | No | No |
| Wellcome Collection | **Yes** (P3 tree) — children 503 on probe date | **Yes** (P3, 106 762 B) | No (tree only, no counts/offset) | **Yes** (SearchService1, per-work text only) |
| SMK | **No** (none advertised) | **Yes** (P3, 3 414 B) | No | No |
| Harvard Art Museums | **Partial** — root exists, enumeration level returns 500 | **Yes** (P2, 9 209 B) | No (broken one level down) | No |
| Smithsonian | **No** (IIIF absent from docs) | **UNVERIFIED** (undocumented) | No | No |

### 2.1 Art Institute of Chicago

| Probe (2026-08-07) | Result |
|---|---|
| `https://www.artic.edu/iiif/2/collection` | **302 → `/iiif/2/collection/info.json`** |
| `https://www.artic.edu/iiif/2/collection/info.json` | **404** |
| `https://api.artic.edu/api/v1/artworks/111628/manifest.json` | **200**, Presentation **2** `sc:Manifest`, **2 494 B** |
| `https://api.artic.edu/api/v1/artworks?page=1&limit=100&fields=id,title,image_id,artist_title` | **200**, **14 231 B**, `pagination.total = 132 681` |

The server treats `collection` as an *Image API identifier* and redirects to its
(nonexistent) `info.json` — conclusive evidence there is no Presentation Collection
mounted there. (A 2026-05 probe of the same URL and `/iiif/3/collection` returned
403; the behavior changed but the answer did not.) Per-artwork manifests exist but
hang off the bespoke API's URL space (`/api/v1/artworks/{id}/manifest.json`): you
need the search API to learn the id before you can fetch the manifest.

### 2.2 Rijksmuseum

| Probe | Result |
|---|---|
| `https://data.rijksmuseum.nl/search/collection?imageAvailable=true` | **200**, 7 774 B, `type: OrderedCollectionPage`, `partOf.totalItems = 735 001`, `next` = opaque base64 `pageToken` cursor |
| `https://id.rijksmuseum.nl/2001` (HMO, 3-hop resolve, hop 1) | 200, 13 532 B |
| VisualItem hop 2 | 200, 1 475 B |
| DigitalObject hop 3 → `access_point: https://iiif.micr.io/KMFvF/full/max/0/default.jpg` | 200, 631 B |
| `https://iiif.micr.io/KMFvF/manifest` | **200**, P3 Manifest, **1 599 B** — but `label` is an opaque hash (`"269ac2cfc50c4a…"`), no title/artist |

Rijksmuseum is the one museum whose discovery layer *is* standards-based — but the
standard is Linked Art with ActivityStreams-style paging, not IIIF Presentation. It
demonstrates exactly what the standards path costs: the cursor is opaque, so there is
**no random access** — p3a's channel-offset feature works there only via a
modulo-wrap using `partOf.totalItems` and walking pages (`rijksmuseum.c` header
comment: cursor walk over OrderedCollectionPage, three JSON-LD hops
HMO → VisualItem → DigitalObject per artwork to find the image URL, run lazily at
download time with a tombstone scheme for failures). The Micrio manifest exists but
is a viewer-support stub carrying no descriptive metadata, so it can serve neither
discovery nor display.

### 2.3 Victoria and Albert Museum

| Probe | Result |
|---|---|
| `https://api.vam.ac.uk/v2/objects/search?page=1&page_size=100&images_exist=1` | **200**, **100 595 B**, `record_count = 741 731`, records carry `_images._iiif_image_base_url` inline |
| `https://iiif.vam.ac.uk/collections/O507242/manifest.json` | **200**, Presentation **2** `sc:Manifest`, **2 903 B** |
| `https://iiif.vam.ac.uk/collections` | **404** |

Clean per-object manifests, no collection-level Presentation surface at all. The
object id (`O507242`) that names the manifest comes from the search API.

### 2.4 Wellcome Collection

| Probe | Result |
|---|---|
| `https://iiif.wellcomecollection.org/presentation/collections` | **200**, **1 095 B**, Presentation **3** Collection with child Collections: *Works by subject*, *Works by genre*, *Works by contributor*, *digitalcollections* |
| `https://iiif.wellcomecollection.org/presentation/collections/genres` | **503** |
| `https://iiif.wellcomecollection.org/presentation/collections/digitalcollections` | **503** |
| `https://iiif.wellcomecollection.org/presentation/b18035723` | **200**, P3 Manifest, **106 762 B** (36 canvases), advertises `SearchService1` |
| `https://api.wellcomecollection.org/catalogue/v2/works?page=1&pageSize=100&items.locations.locationType=iiif-image` | **200**, **110 403 B**, `totalResults = 82 390`, `totalPages = 824` |

Wellcome is the strongest Presentation adopter of the seven — the only one with a
top-level machine-discoverable Collection tree, which even materializes facet axes
(subject/genre/contributor) as sub-collections. Yet **both facet children probed
returned 503 on 2026-08-07** (cause UNVERIFIED — possibly a transient backend
outage, possibly the cost of serving an entire facet axis as one document). Even
when they work, P3 Collections carry no totals and no paging (§3), so the tree
answers "what exists" but not "how many" or "give me items 400–499". Its Content
Search service searches digitized text *within one work* — catalogue discovery is
explicitly the Catalogue API's job. And a single Wellcome manifest (106 KB) is as
large as an entire 100-work catalogue page (110 KB).

### 2.5 Statens Museum for Kunst

| Probe | Result |
|---|---|
| `https://api.smk.dk/api/v1/iiif/manifest?id=KKS5261` | **200**, Presentation **3** Manifest, **3 414 B** |
| `https://api.smk.dk/api/v1/art/search?keys=*&offset=0&rows=50&filters=[has_image:true]` | **200**, **184 739 B**, `found = 54 393` |

Manifests exist (served *by the bespoke API host*, keyed by object number obtained
from search). No IIIF Collection document advertised anywhere (none found;
absence of an unadvertised one is UNVERIFIED). Note SMK search supports true
`offset=` random access — the friendliest pagination of the seven.

### 2.6 Harvard Art Museums

| Probe | Result |
|---|---|
| `https://iiif.harvardartmuseums.org/collections/top` | **200**, **632 B**, P2 `sc:Collection` → one member: `/collections/object` ("Objects") |
| `https://iiif.harvardartmuseums.org/collections/object` | **500** |
| `https://iiif.harvardartmuseums.org/collections/object?page=1` | **500** |
| `https://iiif.harvardartmuseums.org/manifests/object/299843` | **200**, P2 `sc:Manifest`, **9 209 B**, no search service |

The textbook case: HAM ships the advertised IIIF Collection *root*, but the single
child that would actually enumerate objects has been returning **500** — the tree is
dead one level down, precisely at the point where discovery would begin. Live
discovery requires the key-gated `api.harvardartmuseums.org` search API (not probed;
key-gated), which is what p3a uses.

### 2.7 Smithsonian

No live probes requiring a key were made. Documentation checks (2026-08-07):

- `https://edan.si.edu/openaccess/docs/` and the Smithsonian/OpenAccess GitHub
  README: **no mention of IIIF at all** — no Presentation, no manifests, no
  Collections. Documented access is the key-gated `api.si.edu/openaccess` search
  API plus IDS image delivery.
- `ids.si.edu` serves IIIF **Image** API endpoints (`/ids/iiif/{id}` — the base p3a
  builds URLs on, per `museums/smithsonian.c`), but no manifest or collection
  service is documented. Existence of any SI Presentation surface for these
  records: **UNVERIFIED, and undocumented**.

---

## 3. Spec-level analysis: what Presentation guarantees vs what discovery needs

Consulted 2026-08-07: iiif.io Presentation 3.0, Presentation 2.1.1, Content
Search 2.0.

**Presentation 3.0** scopes itself to supplying what a viewer needs to render a
compound object; §1.1 states outright that metadata for harvesting, discovery, and
search-engine indexing is *not* what the API provides. Structurally:

- **Collections** (§5.1) are ordered trees of references — `items` arrays of child
  Collections/Manifests. Ordering is significant; that is the only traversal
  contract.
- **No paging.** The 2.1 paging properties were removed in 3.0. There is no
  `first`/`next`/`last`, no page construct at all for Collections. A large
  collection is either one enormous document or an ad-hoc tree of sub-collections
  (Wellcome's approach).
- **No counts.** No property gives the total number of items in a Collection.
- **No query mechanism.** No filtering, no offsets, no facets. Search is delegated
  to the separate Content Search API.

**Presentation 2.1.1** *did* have paging (§3.5: `first`, `last`, `next`, `prev`,
`total`, `startIndex` on Collections and AnnotationLists) — but: `total` is
**optional** ("may use"), and navigation is **link-following only**; there is no
way to request page N or offset K directly. Even the most Presentation-friendly
reading of 2.1 gives ordered sequential traversal with an optional count — the
Rijksmuseum cursor walk, essentially, with all the costs p3a's `rijksmuseum.c`
already documents (offset access requires walking; totals require trusting an
optional field).

**Content Search 2.0** does not close the gap: its stated scope is annotation
content *within a single IIIF resource*, and it says explicitly that searching
metadata or other descriptive properties is not in scope. It is "find this word in
this digitized book", not "find artworks by department". No facets, no collection
enumeration.

(The IIIF answer to cross-collection discovery is the separate **Change Discovery
API** — an ActivityStreams feed intended for *harvesters* that build their own
search index. That presumes a crawler and an index; a microcontroller with 32 MB
of PSRAM and no persistent query engine is the client that model excludes by
design. None of the seven museums advertises a Change Discovery feed for artworks
that we found; UNVERIFIED beyond absence from their docs.)

So even under **perfect adoption**, Presentation-only discovery reduces to: fetch a
Collection tree, walk it in order, fetch every Manifest to learn anything about the
object it references (a Collection item carries roughly id + label). "All artworks
with images in department X, total count, starting at offset 400" is not
expressible in Presentation 3.0 at all, and only approximable in 2.1 by a full
sequential walk.

## 4. Client cost: measured sizes (2026-08-07)

One bespoke search page, 50–100 artworks **with inline image identifiers**:

| Museum | Page probe | Bytes | Artworks | B/artwork |
|---|---|---|---|---|
| AIC | `/artworks?limit=100&fields=id,title,image_id,artist_title` | 14 231 | 100 | ~142 |
| Rijks | `search/collection` OrderedCollectionPage | 7 774 | 100 | ~78 (ids only — **no** image ids; +3 JSON-LD hops ≈ 15.6 KB per artwork at download time) |
| V&A | `objects/search?page_size=100` | 100 595 | 100 | ~1 006 |
| Wellcome | `works?pageSize=100` | 110 403 | 100 | ~1 104 |
| SMK | `art/search?rows=50` | 184 739 | 50 | ~3 695 |

One representative manifest:

| Museum | Manifest | Bytes | Presentation |
|---|---|---|---|
| Rijks (Micrio) | `iiif.micr.io/KMFvF/manifest` | 1 599 | 3 (no metadata) |
| AIC | `artworks/111628/manifest.json` | 2 494 | 2 |
| V&A | `collections/O507242/manifest.json` | 2 903 | 2 |
| SMK | `iiif/manifest?id=KKS5261` | 3 414 | 3 |
| HAM | `manifests/object/299843` | 9 209 | 2 |
| Wellcome | `presentation/b18035723` | 106 762 | 3 |

To build one 1 000-artwork channel: the bespoke path costs **10–20 HTTPS requests**
(140 KB at AIC, ~2 MB at SMK). A Presentation-only path costs the Collection
walk **plus ~1 000 manifest GETs** — ~2.5 MB at AIC-sized manifests, over
**100 MB** at Wellcome-sized ones — i.e. a **50–100× request multiplier**, on a
device where every request is a TLS round-trip on an ESP32-C6 co-processor link
and JSON parsing happens in fixed 192 KB buffers. And after all of it the client
still has no counts and no offsets. The manifest is also the wrong granularity in
the other direction: it describes canvases for a viewer, not the one field p3a
needs (an Image API base URL), which the bespoke pages deliver inline at
~142 B/artwork (AIC).

## 5. Conclusion

IIIF Presentation could not have served as p3a's artwork-discovery layer, and the
reasons stack rather than compete. **Empirically** (probed 2026-08-07): of the seven
museums, only Wellcome and Harvard publish a machine-discoverable IIIF Collection
document at all, and both failed at exactly the enumeration level when probed —
Harvard's `/collections/object` returns 500 beneath a working root, and both of
Wellcome's probed facet sub-collections returned 503 beneath a working root; AIC and
V&A serve per-object manifests but no collection surface (AIC's `/iiif/2/collection`
resolves as a nonexistent *image*); SMK's manifests are an endpoint *of the bespoke
API*; the Smithsonian documents no Presentation surface whatsoever. Six of seven do
publish per-artwork manifests — but a manifest is addressable only by an object id
that the bespoke search API must supply first, so manifests presuppose the very
discovery layer in question. **Structurally**, the spec itself forecloses the idea:
Presentation 3.0 defines Collections as ordered reference trees with no paging
mechanism, no item counts, no offset access, no facets, and no metadata search — by
its own scope statement it exists to drive viewers, with discovery delegated to
Content Search (annotation text within a single object, metadata explicitly out of
scope) and to the harvester-oriented Change Discovery API, which presumes an
external index a microcontroller cannot host. What p3a's channels require —
"artworks with images in department X, with a total, at offset N" — is not
expressible in Presentation 3.0 and only approximable in 2.1 by sequential
link-walking with an optional `total`. **Economically**, measured on the same day,
one bespoke search page delivers 50–100 artworks with inline image identifiers in
14–185 KB, while the Presentation equivalent is 50–100 manifest fetches
(2.5–107 KB each) after a tree walk — a 50–100× request multiplier for a TLS-per-
request embedded client. The one standards-based discovery path p3a does ship —
Rijksmuseum's Linked Art OrderedCollectionPage walk — works, and is the exception
that measures the rule: cursor-only traversal forced a modulo-wrap workaround for
offsets, and image URLs cost three extra JSON-LD hops per artwork. The shipped
architecture — bespoke search for discovery, IIIF Image API for pixels — is not a
shortcut around the standard; it is the division of labor the IIIF specifications
themselves prescribe.
