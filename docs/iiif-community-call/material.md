# Material for the IIIF Community Call talk

Consolidated 2026-09-08 from the Code4Lib Journal draft
(`docs/outreach/code4lib-journal/article.md`, its `source-material.md`
and `presentation-api-research.md`), `docs/art-institutions/finalized-design.md`,
the museum-ubi reference project (`reference/museum-art/`), the
IIIF-Discuss email (`docs/outreach/iiif-discuss-thread.md`), the
content-sources survey (`docs/content-sources-survey.md`), git history,
and the interview of 2026-09-08. This is input for the talk, not the
talk. Where a number was measured, the measurement date is given so it
can be re-run before 2026-10-14.

Contents

1. Framing
2. What p3a is
3. How p3a uses IIIF
4. How it was built
5. What the client needs from a discovery layer
6. Could IIIF Presentation have been the discovery layer?
7. Idiosyncrasies, museum by museum
8. Museums that could not ship
9. Being a polite client
10. What would help small clients
11. Ecosystem observations
12. Numbers fact box
13. Assets
14. Things to verify before the talk
15. Source pointers

---

## 1. Framing

**Title.** IIIF from the smallest patron. Subtitle: what nine museum
APIs look like from a 32 MB art frame.

**Thesis, one sentence.** The IIIF Image API kept its promise: one URL
template served pixels from seven independently operated image stacks.
Above the pixels, finding artworks, browsing collections, and paginating
required a bespoke adapter per museum, because the standard IIIF offers
there is scoped for viewers and harvesters, not for small clients asking
contained questions.

**Vantage point.** An outsider. Fab had never heard of IIIF before May
2026. p3a is not a museum product, a startup product, or a research
prototype; it is a working consumer-grade object that exists because of
the open-access decisions museums made over the last decade, and it is
the smallest regular visitor those endpoints have.

**Register.** Gratitude first, always. Every quirk is an observed
behavior, reported politely from outside, with the institution named.
None of the museums is doing anything wrong; the point is variance.
Article line worth reusing: "Nine reasonable APIs are still nine APIs."

**The ask.** Feedback on the discovery-gap analysis (section 6). Is the
conclusion right? Is anything on IIIF's roadmap meant to close it?
Secondary, only if it comes up: pointers to prior firmware-level IIIF
clients (the "first, as far as I can tell" claim is deliberately
falsifiable).

**Time split.** What p3a is + video 0:00-6:00. How p3a uses IIIF
6:00-11:00. How it was built 11:00-16:00. Discovery gap 16:00-23:00.
Idiosyncrasies 23:00-28:00. Close 28:00-30:00.

**Speaker bio (title slide).** Fabrício Kury is a biomedical
informatician in New York City. He currently works with Medicare claims
data analytics at Sparx, Inc.

## 2. What p3a is

- Open-hardware desktop art frame. Firmware in C on ESP-IDF v5.5 and
  FreeRTOS. Apache 2.0. https://github.com/fabkury/p3a. Web flasher:
  https://fabkury.github.io/p3a/web-flasher/. Makezine wrote it up:
  https://makezine.com/projects/desktop-pixel-art-player-p3a/.
- Hardware: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, $39.99 at the
  manufacturer's site as of August 2026. Add a microSD card and a USB-C
  supply: under $70 all-in (article says "under $60" in one place and
  "under $70" in another; use $70 to be safe, or re-check).
- ESP32-P4: dual-core 400 MHz RISC-V. 32 MB PSRAM plus about 768 KB of
  internal SRAM. No operating system beyond FreeRTOS. No browser stack.
  Hardware JPEG decoder with sharp limits (section 3). Wi-Fi 6 via a
  companion ESP32-C6 over SDIO. 4-inch 720x720 24-bit IPS capacitive
  touchscreen. 32 MB flash.
- Began as a pixel-art player. Sources: Makapix Club (a pixel-art social
  network Fab also runs, open source), Giphy and Klipy (GIFs and
  stickers), local files on the SD card, artwork from a URL, and
  museums. Plays WebP, GIF, PNG, APNG, JPEG, BMP. Also streams PICO-8
  games from a browser-hosted emulator.
- Updates itself over the air from GitHub Releases; runs on real users'
  devices. Current firmware 1.2.1 (2026-08-31), web UI 2.23.

**Channels and playsets.** p3a's abstraction for a source of artworks is
the channel: a list assumed to be too large to fetch at once. A museum
channel is a (museum, axis, term) triple, for example "Rijksmuseum,
curated set, Dutch Paintings of the Seventeenth Century" or "V&A,
category, Photographs". A playset mixes channels at chosen ratios of
airtime. The user assembles channels in a small web app the device
itself serves to a phone or laptop on the same Wi-Fi.

**Defaults.** Listing refreshed every 4 days (range 1-8). Up to 1,024
artworks cached per channel (range 32-4,096). 30 seconds per artwork.
Hard limits by design: 64 channels, 4,096 artworks each.

**Museums shipping today (firmware 1.2.1).** Nine. Seven over the IIIF
Image API; two (Cleveland, Minneapolis) over their own fixed-rendition
CDNs. AIC currently disabled (section 11).

| Museum | id | Facets the user can pick | Key needed |
|---|---|---|---|
| Art Institute of Chicago | `artic` | Departments, Classifications, Subjects, Themes, Galleries, Artwork types | no (disabled since 2026-08-14) |
| Rijksmuseum | `rijks` | Curated sets (193) | no |
| Victoria and Albert Museum | `vam` | Collections, Categories, Venues | no |
| Wellcome Collection | `wellcome` | Work types, Genres, Subjects, Contributors | no |
| Statens Museum for Kunst | `smk` | Collections | no |
| Harvard Art Museums | `ham` | Classifications, Centuries, Cultures, Periods, Places, Media, Techniques, Work types, Groups, Galleries | free key |
| Smithsonian | `si` | Museums: Cooper Hewitt, SAAM, NPG, NMAAHC, Hirshhorn, African Art | free api.data.gov key |
| Cleveland Museum of Art | `cma` | Departments, Types (CC0 works only) | no |
| Minneapolis Institute of Art | `mia` | Classifications, Departments, Countries, Styles (public domain only) | no |

**Artwork info.** The device stores no metadata. When the viewer taps
the info button in the web app, the browser queries the originating
museum's API for title, artist, and date. Different for every museum;
IIIF standardizes none of it.

**Reception so far (for context, not for the slides):** r/MuseumPros
post of 2026-06-07 drew 200+ upvotes and about 100 shares. The IIIF
audience should not hear Reddit numbers.

## 3. How p3a uses IIIF

### 3.1 The uniform layer

Once an adapter has produced an image identifier, museums stop being
different. The firmware builds

```
{iiif_base}/{identifier}/full/!720,720/0/default.jpg
```

IIIF Image API version 2, bang-size syntax requesting a best fit within
720x720, rotation zero, JPEG. It worked unmodified on all seven IIIF
services:

| Museum | Image stack | IIIF prefix |
|---|---|---|
| AIC | in-house server | `www.artic.edu/iiif/2/` |
| Rijksmuseum | Micrio (commercial deep-zoom vendor) | `iiif.micr.io/` |
| V&A | framemark host | `framemark.vam.ac.uk/collections/` |
| Wellcome | Wellcome's own | `iiif.wellcomecollection.org/image/` |
| SMK | IIPImage | `iip.smk.dk/iiif/jp2/` |
| Harvard | NRS URN resolver, 303 to `ids.lib.harvard.edu` | `nrs.harvard.edu/` |
| Smithsonian | IDS | `ids.si.edu/ids/iiif/` |

Seven independently operated image stacks, one line of URL
construction. The shared IIIF helpers used by all adapters are 59 lines
of C. This is the standards success story and the people who wrote and
implemented the Image API should hear it.

`!720,720` never crops: across about 90 reported-versus-delivered
comparisons (2026-08-25 survey), the delivered aspect ratio equalled the
master's for every IIIF museum.

### 3.2 Three deliberate simplifications

**No `info.json`.** A capable client negotiates: fetch `info.json`,
learn sizes and features, choose. p3a never does. The panel is 720x720,
`!720,720` has been universally honored, and skipping negotiation
halves the request count. In the months since the feature shipped
(2026-05-15) the shortcut has not misfired once. Where advertisement and
reality diverge, they diverge the other way (SMK, section 7).

**JPEG only.** Museum IIIF servers serve JPEG far more reliably than
WebP, and the ESP32-P4 has a hardware JPEG decoder with sharp limits: it
cannot decode progressive (SOF2) JPEGs at all; ESP-IDF v5.5.1
additionally rejects images whose pixel count is not a multiple of 8
(bites artworks smaller than 720 px); a decoded image can exceed what
PSRAM will hold. Everything the hardware rejects falls back to a
software decode via libjpeg-turbo, fenced so a corrupt file cannot crash
the device (jerror `exit()` replaced by setjmp/longjmp; a progress
monitor yields every 200 ms to dodge the 15 s watchdog). Spot check
2026-08: five of the seven IIIF museums served baseline JPEG at this
rendition; AIC and Harvard served progressive, so every image from
those two decodes in software.

**Stream, never buffer.** Images move to the SD card in 32 KB chunks
through a fixed-size buffer, written to a temporary file and renamed on
completion. Device-wide budget: two concurrent TLS sessions. A 16 MiB
cap guards against a server that ignores the size request and returns a
full-resolution master. A client with 32 MB of total RAM does not get to
"just download the image and see".

### 3.3 The constraints, concretely (constraints slide)

- **64-byte artwork record:** a hash, dimensions, a timestamp, one byte
  for file type, and 48 bytes for the museum's image identifier.
  `_Static_assert(sizeof == 64)`. Identifiers longer than 48 bytes are
  silently unusable (this is what excluded the Library of Congress,
  section 8).
- **33-byte channel identifier slot** in the playset format (32 chars
  plus null). Wellcome terms whose label exceeds 32 characters cannot
  become channels, because Wellcome filters by label, not by id.
- **One byte of state.** The file-type byte moonlights as the
  Rijksmuseum resolver's state machine: 0xFF means "not yet resolved",
  0xFE means "given up".
- **Parse buffers sized per museum,** allocated for the lifetime of a
  refresh: AIC and Rijks 192 KB, V&A and Wellcome 256 KB, CMA 256 KB,
  SMK and Mia 512 KB, Smithsonian 1 MB. All PSRAM. cJSON allocations are
  redirected to PSRAM because a 1 MB JSON of small nodes would fragment
  the DMA-capable internal pool the Wi-Fi SDIO path needs and panic the
  chip.
- **Not all RAM is equal:** 768 KB internal SRAM versus 32 MB PSRAM;
  some work can only happen in internal SRAM.
- **HTTP:** 15 s timeout (20 s for the Smithsonian), 3 attempts, backoff
  0/1/3 s, 5 redirects max, two TLS sessions device-wide.
- Article line: "everything implicit becomes a buffer size, a request
  count, or a byte."

### 3.4 Identifier properties as interoperability properties

- **Length.** AIC image UUIDs (36 chars), Harvard URNs (about 25),
  Micrio ids (about 8), Cleveland accession numbers (up to 14),
  Minneapolis numeric ids (up to 7 digits) fit in 48 bytes. LoC's IIIF
  ids run 27-47 chars in the image-rich subtrees and 67-123 chars in
  newspapers and music.
- **Stability.** The record survives firmware updates, cache rebuilds,
  and re-listings. An identifier that drifts reads as a deletion plus a
  new artwork: the old record is evicted, the image is re-downloaded
  under the new id, the old file sits unreferenced until age-based
  cleanup collects it.
- **Verbosity is a tax paid in RAM.** AIC lists at about 142 bytes per
  artwork; SMK at about 3,700. Invisible on a laptop; on the device it
  is the difference between a 192 KB and a 512 KB buffer.

## 4. How it was built

### 4.1 Timeline (from git; museum-ubi files were imported without history)

| Date | Event |
|---|---|
| 2025-11-06 | p3a first commit (pixel-art player). |
| 2026-05-07 | museum-ubi candidate survey: 18 sources in a capability matrix, four tiers; per-museum Python probe scripts and reports (AIC, Rijks, V&A, Wellcome, SMK, Gallica; Smithsonian later). The survey was LLM-driven; this is where IIIF entered the picture. |
| 2026-05-10 | First draft of the art-institution channels design. |
| 2026-05-11 | Design finalized through five Q&A rounds (transcript kept). Firmware stages 1-6 in one day: component skeleton, scheduler wiring, AIC adapter + IIIF download path, eviction, REST endpoints for rate-limit sharing, web UI. AIC (M1), Rijksmuseum (M2), V&A (M3) end-to-end. museum-ubi imported into the repo as `reference/museum-art/`. |
| 2026-05-12 | Wellcome (M4) and SMK (M5). Gallica deferred (M6): SRU/XML, no XML parser on the device. Single-artwork preview with prev/next in the browse modal. |
| 2026-05-13 | Library of Congress investigated and scaffolded. |
| 2026-05-15 | LoC removed (identifier length). **v0.10.0 ships museums** (five). |
| 2026-05-18 | Harvard Art Museums (BYOK). |
| 2026-05-19 | First demo clip shot (the fallback video). |
| 2026-05-22 | Smithsonian Open Access (BYOK). Seven museums. |
| 2026-05-26 | v0.10.1. |
| 2026-06-29 | v1.0.0. |
| 2026-08-07/09 | Presentation API probes for the article. |
| 2026-08-10 | Code4Lib Journal article submitted. |
| 2026-08-14 | AIC disabled: its IIIF host began challenging non-browser clients (v1.1.2). |
| 2026-08-23 | Cleveland Museum of Art: first non-IIIF museum. |
| 2026-08-24 | Minneapolis Institute of Art: second non-IIIF museum. v1.2.0. Fab joins IIIF-Discuss. |
| 2026-08-25 | IIIF-Discuss announcement posted. |
| 2026-08-31 | v1.2.1 (current). |
| 2026-09 | Slack invitation to the community call. |

Museums went from "never heard of IIIF" to three museums end-to-end in
four days, and to seven in fifteen.

### 4.2 museum-ubi: the sandbox

- **Purpose:** a sandbox to de-risk the p3a feature. The question was
  whether heterogeneous museum APIs could be normalized behind one
  interface before committing firmware work.
- **Name:** museum-ubi, "unified browsing interface". Its charter
  (`reference/museum-art/CLAUDE.md`): collapse categories, sections,
  folders, groups, catalogs, and collections into one concept called
  collections; three features every source must support: list
  collections; list artworks in a collection with range-style
  pagination ("artworks 200-249 of collection X", not just next-cursor);
  keyword search. "The hard problem of this project is reconciling
  heterogeneous capabilities behind one interface, not building any
  single integration."
- **Original hard constraint:** the source must expose artworks via the
  IIIF Image API, and "IIIF Presentation API is strongly preferred since
  it is the natural carrier for the UBI's collection and artwork-listing
  concepts." That preference did not survive contact (section 6). The
  IIIF-only rule was retired in August 2026 when Cleveland shipped with
  fixed CDN renditions.
- **What it contained:** the candidate matrix (18 sources: SMK, Rijks,
  V&A, AIC, Harvard, Wellcome, Gallica, LoC in tier 1; Getty,
  Smithsonian, Yale, Princeton, NGA, Internet Archive, Europeana,
  Bodleian, Fitzwilliam, e-codices in tier 2; a watch list of nine
  more). Per-source Python probe scripts (`source/*/run.py`) that
  exercised each API and wrote a report with a feature-support table
  (native / emulated / unavailable). `ubi-test`: a hash-routed
  single-page app (landing, museum, collection, artwork views) with a
  JS adapter per museum (674 lines across five adapters plus a base)
  and Playwright specs per museum.
- **What carried into p3a:** the JS adapters were ported to the browse
  UI (`webui/museum/*.js`, now 3,060 lines across nine adapters plus
  the browse modal). The C adapters were written fresh from the
  per-museum reports and probe outputs. The design doc says the
  browser owns "the API-quirk handling that ubi-test already validates".
- **Keyword search,** one of the three museum-ubi features, is still
  deferred in p3a. Channels are facet-based.

### 4.3 The AI slide

- p3a is developed end-to-end with LLM assistance: first a variety of
  models (mostly Anthropic's) inside the Cursor IDE, later Claude Code
  running the newest Opus-class model available at the time. The design
  documents are literally Q&A transcripts between Fab and the model.
- When Fab wanted new content sources, he sent LLM agents to scour the
  web for candidates. One came back with something called the IIIF
  Image API. He had never heard of it. Article line: "It is not an
  exaggeration to say that this article exists because a language model
  introduced me to a library standard." For the talk: "This talk exists
  because a language model introduced me to a library standard."
- The Code4Lib article and its endpoint probes were likewise produced
  with AI assistance and fully revised by Fab, who verified the claims.
  No affiliation with Anthropic, Cursor, or any museum.

### 4.4 The browser/device split

- The browse UI runs in the user's browser on the LAN and queries
  museum APIs directly (CORS permitting). The device owns persistence,
  periodic refresh, image download, playback, eviction. This mirrors
  how the browser already talks to makapix.club directly for
  verification while the firmware handles delivery.
- The JS adapters mirror the C dispatch table one to one. Per-museum
  adapter in C is a compile-time struct row: id, display name, enum,
  `refresh_channel()`, `build_iiif_url()`, optional `resolve_entry()`
  (Rijks only), optional `api_key_missing()` (Harvard, Smithsonian),
  `unavailable_reason` (AIC today).
- Consequence worth telling: the Rijksmuseum's curated set list is
  published over OAI-PMH without CORS headers, so a browser cannot
  fetch it. The firmware bakes the 193-set list into its flash
  filesystem and serves it to its own configuration UI. The device
  carries a copy of the Rijksmuseum's table of contents wherever it
  goes, because of CORS headers. Cleveland's term vocabulary is baked
  the same way (no facet-enumeration endpoint; a full-corpus scan at
  release time).
- Vault: `/sdcard/p3a/museum/{museum_id}/`, hash-sharded, shared across
  channels of the same museum, so a painting that appears in four AIC
  facets is stored once.

## 5. What the client needs from a discovery layer

Long before IIIF entered the picture, the channel design fixed what the
discovery layer must provide. Three primitives, for every museum:

1. **List collections:** whatever the museum's vocabulary of groupings
   is: departments, classifications, curated sets, administrative units.
2. **Enumerate a collection, with a total count, at an arbitrary
   offset:** "artworks 400-499 of Paintings", not merely "next page".
   Counts and offsets matter because a channel can be configured to
   start deep in a collection, and because a device that caches 1,024
   of 735,001 artworks needs to know both numbers.
3. **Yield an image identifier inline:** each listing record must carry
   the key that unlocks pixels, without a further per-artwork request.

No metadata is stored on the device (48 of 64 bytes go to the image
key), which makes the comparison between museums and standards unusually
crisp: "can a small client browse this museum?" reduces to the three
primitives.

**Adapter line counts (C, as of 2026-09-08):**

| Adapter | Lines |
|---|---|
| Art Institute of Chicago | 849 |
| Rijksmuseum | 645 |
| Smithsonian | 526 |
| Minneapolis Institute of Art | 491 |
| Harvard Art Museums | 477 |
| Cleveland Museum of Art | 475 |
| Wellcome Collection | 438 |
| Statens Museum for Kunst | 406 |
| Victoria and Albert Museum | 399 |
| Shared IIIF helpers, used by all | 59 |

Fifty-nine shared lines cover everything the standard standardized. The
other 4,700 are the difference between "has an API" and "has the same
API". The core component around the adapters is another 994 lines
(dispatch, refresh, download, resolve, rate limit).

## 6. Could IIIF Presentation have been the discovery layer?

The early museum-ubi notes called Presentation "strongly preferred, the
natural carrier". The shipped system uses none of it. Re-examined for
the article with live probes of all seven museums on 2026-08-07, failure
cases re-verified 2026-08-09 (Harvard with a valid API key). The answer
is a confident no on three independent grounds.

### 6.1 Adoption

| Museum | Collection document | Per-artwork manifest | Enumerate with counts at offset |
|---|---|---|---|
| AIC | No. `/iiif/2/collection` is parsed by the image server as an image named "collection" and redirects to an `info.json` that 404s | Yes (P2, 2,494 B), addressed by an id only the search API supplies | No |
| Rijksmuseum | No (Linked Art / ActivityStreams instead) | Yes (Micrio, P3, 1,599 B, label is an opaque hash, no metadata) | Counts yes, offset no, and via AS2, not Presentation |
| V&A | No (root 404) | Yes (P2, 2,903 B) | No |
| Wellcome | Yes, P3 tree with facet axes as sub-collections; both facet children probed returned 503 on both dates ("temporarily unavailable") | Yes (P3, 106,762 B, 36 canvases) | No (tree only) |
| SMK | None advertised | Yes (P3, 3,414 B), served by the bespoke API host | No |
| Harvard | Root exists (P2, one child "Objects"); that child returns 500, identically with and without a key, on both dates | Yes (P2, 9,209 B) | No, broken one level down |
| Smithsonian | Not documented at all | Undocumented | No |

Of seven, exactly two publish a machine-discoverable Collection, and
both broke at precisely the level where enumeration would begin. Six of
seven serve per-artwork manifests, but a manifest is addressed by an
object id only the search API can supply: manifests presuppose
discovery rather than provide it.

### 6.2 Specification

- Presentation 3.0 scopes itself to what a viewer needs to render an
  object and says plainly that discovery and harvesting are not what it
  provides.
- Collections are ordered trees of references: no paging construct
  (2.1's paging properties were removed in 3.0), no item counts, no
  offset access, no facets, no metadata search.
- Presentation 2.1.1 did have paging (`first`, `next`, `total`,
  `startIndex`), but `total` is optional and navigation is
  link-following only. Even the friendliest reading gives sequential
  traversal with an optional count: the Rijksmuseum cursor walk, with
  all its costs.
- Search is delegated to the Content Search API, which searches
  annotation text within a single object and states that descriptive
  metadata is out of scope. "Find this word in this digitized book",
  not "find artworks by department".
- Cross-collection discovery is delegated to the Change Discovery API,
  a harvester's ActivityStreams feed that presumes the client maintains
  its own index. The IIIF discovery model is: build a search engine,
  then query your copy. A microcontroller cannot be a harvester; it
  needs the institution's index, queryable in place, which is exactly
  what every museum's bespoke search API is.

### 6.3 Economics (measured 2026-08-07)

One bespoke search page, 50-100 artworks with inline image identifiers:

| Museum | Bytes | Artworks | Bytes per artwork |
|---|---|---|---|
| AIC | 14,231 | 100 | ~142 |
| Rijksmuseum | 7,774 | 100 | ~78 (ids only; plus 3 JSON-LD hops, ~15.6 KB per artwork at download time) |
| V&A | 100,595 | 100 | ~1,006 |
| Wellcome | 110,403 | 100 | ~1,104 |
| SMK | 184,739 | 50 | ~3,695 |

One representative manifest: Rijks (Micrio) 1,599 B; AIC 2,494 B; V&A
2,903 B; SMK 3,414 B; Harvard 9,209 B; Wellcome 106,762 B. A single
Wellcome manifest weighs as much as an entire 100-work catalogue page.

For a standard 1,024-artwork channel: the bespoke path costs 10-20 HTTPS
requests (140 KB at AIC, ~2 MB at SMK). A Presentation-only path costs
the Collection walk plus about 1,000 manifest GETs: ~2.5 MB at AIC-sized
manifests, over 100 MB at Wellcome-sized ones. A 50-100x request
multiplier for a client that pays a TLS round trip per request, and at
the end the client still has no counts and no offsets.

Probe commands to re-run the week before the talk (curl, with an
identifying User-Agent):

```
https://iiif.harvardartmuseums.org/collections/top
https://iiif.harvardartmuseums.org/collections/object
https://iiif.wellcomecollection.org/presentation/collections
https://iiif.wellcomecollection.org/presentation/collections/genres
https://www.artic.edu/iiif/2/collection
```

### 6.4 Conclusion

The Rijksmuseum adapter is the exception that measures the rule: its
Linked Art walk is the standards-based discovery path, working in
production, at the price of cursor-only access, offset emulation, and
three extra fetches per artwork.

The conclusion Fab did not expect: p3a's split (bespoke search API for
discovery, IIIF for pixels) is not a pragmatic betrayal of the standard.
It is the division of labor the IIIF specifications themselves
prescribe. The gap between "there should be a standard way to browse
collections" and "there is one" is real, known to the community, and
unlikely to be closed by Presentation Collections as they stand. This
is the claim the audience is invited to push back on.

## 7. Idiosyncrasies, museum by museum

### 7.1 One line each (table slide)

| Museum | Quirk |
|---|---|
| AIC | Anonymous `from + size` on search must stay at or under 1,000 (undocumented); HTTP 403 past about page ten on large facets; asks for a custom `AIC-User-Agent` header; since 2026-08 the image host challenges non-browser clients |
| Rijksmuseum | No image id in listings: three JSON-LD hops per artwork; cursor-only pagination; set list over OAI-PMH without CORS; `id.rijksmuseum.nl` 303s and the HTTP client would not surface the Location header |
| V&A | Venue facet returns `count = 0` when combined with `images_exist=1`; page x size capped at 10,000; occasional records flagged with images but missing the id |
| Wellcome | Genres, subjects, and contributors filter by the label string itself (no stable id); `/images` endpoint silently ignores `/works` filters |
| SMK | `info.json` advertises WebP, server returns 400 for it; `fields=` trimming returns empty items, so 50 records cost ~205 KB; facets endpoint returns collection pairs in three different shapes |
| Harvard | Without `q=imagepermissionlevel:0`, 57 of the first 100 "has image" records have no usable image URL; the "IIIF server" is a URN resolver that 303s to the real host; 2,500 requests/day key |
| Smithsonian | WAF rejects a default User-Agent with HTTP 200 and an HTML "Request Rejected" page; `usage: CC0` is not indexed for filtering; `media` is sometimes an object, sometimes an array; 50 records can exceed 512 KB; DEMO_KEY too small for one refresh |
| Cleveland | No IIIF: fixed `_web.jpg` renditions, 750-1,300 px, median ~300 KB, up to ~1 MB; no facet endpoint (`/api/departments/` 404s) so vocabularies are baked; `width`/`height` are strings, sometimes empty |
| Minneapolis | No IIIF: pre-rendered S3 buckets at 400, 800, full; raw Elasticsearch passthrough in the URL path; `from + size` window of 10,000 beyond which the endpoint returns a bare `[]` or 500; mojibake with unpaired surrogates in the Style facet |

### 7.2 Deep dive: Rijksmuseum, Linked Art three hops down

- The one museum whose discovery layer is itself standards-based:
  Linked Art documents traversed as an `OrderedCollectionPage` stream
  (`data.rijksmuseum.nl/search/collection?memberOfSetId=...&imageAvailable=true`,
  100 per page, opaque base64 `pageToken`, `partOf.totalItems`, for
  example 735,001).
- Nothing in the listing carries an image identifier. For every artwork
  the client walks HumanMadeObject, `shows[]`, VisualItem,
  `digitally_shown_by[]`, DigitalObject, `access_point[]`, whose URL
  finally names the Micrio image id. Each hop is a separate JSON-LD
  fetch (measured: 13.5 KB, 1.5 KB, 0.6 KB).
- Three hops per artwork at listing time would be brutal, so the adapter
  resolves lazily: refresh stores unresolved records (file-type byte =
  0xFF), and the download loop resolves one artwork per pass,
  interleaved between image downloads (a walk costs a few hundred ms;
  draining all would block downloads). A record that fails three times
  is tombstoned (0xFE) and skipped until a future refresh re-lists it,
  which resets the budget. A fresh Rijks channel takes about 50 minutes
  to fully populate; the first artworks appear within seconds.
- Merge rule: an incoming unresolved record never overwrites a resolved
  one (that would orphan the file already on disk); tombstones are
  replaced to grant a fresh budget. Lock discipline: snapshot under
  lock, drop the lock for the network, re-find by id for writeback,
  because a concurrent refresh may have moved or evicted the entry.
- Arbitrary offset is emulated: read the total from the stream, wrap
  the offset into range, pay ceil(N/100) throwaway page fetches.
- After resolution the key stores `{micrio}|{hmo}` because there is no
  public reverse mapping from Micrio id to object, and the info view
  needs the object.
- ESP-IDF's HTTP client would not surface the 303 Location header under
  the open/fetch-headers/read pattern; the fix captures it in an
  on-header event handler.
- Every cost here is the honest price of the standards-based path. The
  Micrio manifest exists (P3, 1,599 B) but its label is an opaque hash:
  it serves neither discovery nor display.

### 7.3 Deep dive: Smithsonian, the WAF that says 200

- `api.si.edu` and `ids.si.edu` sit behind an F5 BIG-IP web application
  firewall that rejects requests with an empty or default User-Agent.
  It rejects them with HTTP 200 and an HTML page reading "Request
  Rejected".
- For a firmware client the failure signature is a JSON parse error on
  what claimed to be a successful response. Among the most misleading
  failure modes in the project: every signal along the way insisted the
  request had succeeded.
- The fix costs one line: identify yourself. p3a sends
  `User-Agent: p3a/{version} (pub@kury.dev)`, which is exactly what a
  WAF wants of a well-behaved bot.
- Also: Smithsonian records are the largest of the nine (nested EDAN
  metadata; 50 records can exceed half a megabyte), so this adapter
  alone budgets a 1 MB response buffer. `usage: CC0` is not indexed for
  filtering (AND-ing it matches nothing). `DEMO_KEY` (~30 requests/hour
  per IP) cannot survive one refresh; a free api.data.gov key gives
  1,000/hour against ~82 calls for a full refresh.
- Units shipped: Cooper Hewitt (54,608 online items, ~90% with IIIF),
  SAAM (12,989), NPG (14,641), NMAAHC (4,647), Hirshhorn (449), African
  Art (113). NMAI excluded: its ARK identifiers do not resolve at the
  IDS IIIF endpoint (0% of probed `info.json` returned 200). Freer and
  Sackler: zero records in Open Access as of 2026-05-22.

### 7.4 Deep dive: SMK, the info.json that lies

- SMK is the friendliest API of the nine: true offset pagination,
  anonymous, clean JSON, dimensions in the listing.
- Its IIPImage server's `info.json` advertises WebP and returns HTTP 400
  when asked for it. The one museum where capability negotiation would
  have changed p3a's behavior is the one where negotiation lies. This
  is the evidence for "advertise only what is served" and the reason
  skipping `info.json` has been safe: divergence goes the other way.
- Its search API returns empty items if asked to trim response fields,
  so metadata-rich pages must be swallowed whole: about 4 KB per record,
  ~205 KB for 50, hence a 512 KB buffer.
- Image identifier is the JP2 filename after the last `/iiif/jp2/` in
  `image_iiif_id`; some records carry only a UUID thumbnail and are
  skipped.

### 7.5 Other stories, in reserve

- **AIC's thousand-record wall and the bisection.** Anonymous callers
  find empirically that `from + size` must stay at or under 1,000, and
  paging past about page ten on large facets returns 403. AIC's search
  accepts Elasticsearch-style JSON bodies via POST, and `bool` + `range`
  filters on the numeric artwork id are honored. So for offsets beyond
  1,000 the adapter partitions the museum: probes counts with
  `size: 0`, recursively bisects the id space until every bucket holds
  at most 1,000 records (up to 64 buckets, id space to 1 M), then pages
  within buckets. A $40 device performing adaptive query planning
  against a museum's search engine, recomputed at each refresh because
  the collection moves. Article line: "I am fond of this code and
  slightly discontent that it needs to exist." (Decision: no
  pseudo-code on slides; tell it verbally if there is time, or in Q&A.)
- **Harvard's permission gate.** Without `q=imagepermissionlevel:0`,
  57 of the first 100 `hasimage=1` records have no `primaryimageurl`;
  with it, 0 of 100 (total 226,327 vs 167,189: about 26% of "has image"
  records are permission-restricted at a layer the flag does not see).
  Harvard's image server is a URN resolver: the IIIF path is appended
  to a URN and a 303 lands on the real host. Terms longer than 32 chars
  are dropped: about 22% of periods, 27% of galleries.
- **Wellcome filters by label.** For genres, subjects, and contributors
  the filter value is the label string; no stable id. Labels over 32
  characters cannot become channels (33-byte slot). Deferred design to
  lift it: `docs/deferred/wellcome-long-labels.md`.
- **V&A's count = 0.** The venue facet returns zero counts when
  combined with the has-images filter. The browse UI enumerates venues
  unfiltered, then re-probes each term with the filter (browser side,
  at most once per session, six requests at a time).
- **Cleveland and Minneapolis, coincidence into guarantee.** Both
  publish fixed renditions instead of IIIF. They work on this panel only
  because their baked sizes happen to land near 720 px (Cleveland
  750-1,300 px; Mia's 800 bucket, ~50 KB, the lightest delivery of any
  museum). IIIF turns that coincidence into a guarantee. Quiet advocacy
  for IIIF from the consumer side.
- **Mia's metadata/file divergence.** Its dims describe the original
  scan; the 800 bucket was rendered from a differently cropped source
  in 2 of 30 sampled records. The one metadata/file divergence found,
  and it is not IIIF.

## 8. Museums that could not ship

| Source | Why not (as of the date given) |
|---|---|
| Library of Congress (2026-05-13/15) | Anonymous fetches work with a polite User-Agent; `!720,720` verified. But IIIF identifier length is bimodal: 27-47 chars in Prints and Photographs, maps, manuscripts; 67-123 chars in newspapers and music. Whether an item fits the 48-byte slot depends on which storage subtree it lives in, not on any facet. And listings often carry only a 150 px thumbnail, with no way to tell from the listing whether the IIIF service exists for the item. Facet filter syntax is silently dropped unless field names use dashes and values are the lowercase display title verbatim. Scaffolded 2026-05-13, removed 2026-05-15. |
| BnF Gallica (2026-05-12) | Catalogue API is SRU returning XML (OAI-PMH-flavored Dublin Core). The device has no XML parser and ESP-IDF ships none. Image API is v1.1. Also 403s default `requests`/`curl` fingerprints. Deferred: `docs/deferred/gallica.md`. |
| J. Paul Getty Museum | Enumeration only via an ActivityStreams `OrderedCollection` (~4.5 M items, ~45 K pages, mostly non-artwork entities); no documented JSON search; range pagination would need a local index. The harvester model, exactly what a microcontroller cannot do. |
| Europeana | Thumbnail API caps at 400 px; provider-level image URLs are too heterogeneous for one adapter. Free key. |
| DPLA | Thumbnails only. |
| Yale Center for British Art | Discovery only through LUX (Yale-wide Linked Art platform, OrderedCollectionPage paging) or OAI-PMH (LIDO XML, resumption tokens). |
| Princeton University Art Museum | Image API v3 only, no Presentation; offset pagination `from + size` at or under 500. Not integrated yet, no blocker found. |
| National Gallery of Art (DC) | Bulk CSV harvest only; Presentation anonymous 403. |
| Smithsonian NMAI, Freer/Sackler | NMAI: ARK identifiers do not resolve at the IDS IIIF endpoint. FSG: zero Open Access records as of 2026-05-22. |
| NYPL Digital Collections | API retired 2026-08-01. |

Also excluded on TLS-fingerprint or challenge grounds during the
2026-08 content-sources survey: Finna, Science Museum Group, Brooklyn
Museum (Cloudflare-style challenges reject an mbedTLS handshake from an
ESP32).

## 9. Being a polite client

- **Identify yourself.** Every request carries
  `p3a/{version} (pub@kury.dev)`. AIC asks for this explicitly via a
  custom header; the Smithsonian enforces the spirit of it with a
  firewall.
- **Honor `Retry-After`, remember it, share it.** Each museum has a
  cooldown slot on the device (RAM-only, lock-free reads, absolute
  timestamps). Any HTTP 429 engages it: `Retry-After` honored when
  present (capped at an hour), 60 seconds otherwise (sized to AIC's
  60-requests-per-minute window). Every layer that dials out (refresh
  gate, adapter entry, download entry, the Rijks resolver) checks it.
  Cooldowns only extend, never shorten.
- **One budget per household.** The browse UI runs in the user's
  browser and queries museum APIs directly, so browser and device share
  one public IP, and museum rate limits are per IP. When the browse UI
  receives a 429 it reports it to the device
  (`POST /api/museum/rate-limits/report-429`), engaging the same
  cooldown; before expensive browse operations the UI polls the
  device's cooldown table and waits its turn. Unknown museum ids in
  that POST are silently ignored so the endpoint does not leak the
  museum list.
- **Pace even when allowed.** 150-200 ms between page fetches; browse
  probes capped at six concurrent; two TLS connections device-wide. A
  full channel refresh costs roughly 41-82 API calls against daily
  quotas of 1,000-2,500 where quotas exist, every four days.
- Honest gap: the designed "three connection failures in 30 s engages a
  defensive cooldown" trigger was never implemented.
- Figure: `docs/outreach/code4lib-journal/figure-2-architecture.svg`
  (browser, device, museum APIs, shared cooldown table).

## 10. What would help small clients

Distilled from nine integrations. Every item already exists in at least
one of the APIs, so this is curation, not invention:

1. **Inline image identifiers in search results.** The single biggest
   determinant of client cost. One field turns N+1 requests into 1.
2. **True offset pagination with totals.** Cursors force sequential
   walks; caps that appear only empirically force adapters to learn
   them empirically.
3. **A filter meaning "displayable image actually included"** that
   composes with other filters and reflects permissions, not just file
   existence.
4. **`Retry-After` on every 429.** Clients that want to comply can only
   comply with what is stated.
5. **Reject loudly.** An HTTP error for a rejected request; a WAF that
   says 200 turns every downstream parser into a liar.
6. **Advertise only what is served.** An `info.json` is a promise, and
   small clients are the ones that believe it.
7. **Document the sharp edges.** Result-window caps, key-quota floors,
   permission gates. Every undocumented limit was learned by trial and
   error.

Article framing: none of this asks museums to design for
microcontrollers. It asks something cheaper: that the properties small
clients depend on (short stable ids, lean listings, honest capability
advertisements) be recognized as compatibility surface, not
implementation detail.

## 11. Ecosystem observations

- **Museum APIs are mortal.** NYPL's Digital Collections API retired
  2026-08-01; Walters (2023), MKG Hamburg's host, Minneapolis's old
  image hosts all gone. p3a carries per-source kill switches
  (`unavailable_reason`).
- **Bot defense is a first-class compatibility issue.** WAFs and
  fingerprint-based challenges front several otherwise-open cultural
  APIs. An mbedTLS handshake from an ESP32 is exactly the fingerprint
  these systems flag. A legitimate small client is collateral unless
  "identify yourself honestly" remains a sufficient answer.
- **AIC, specifically.** AIC was the first museum integrated and the
  best-documented API found. Since 2026-08-14 its IIIF host
  (`www.artic.edu/iiif/2`) answers non-browser clients with a Cloudflare
  managed challenge (HTTP 403, `Cross-Origin-Resource-Policy:
  same-origin`, so even cross-origin `<img>` loads fail); the metadata
  API at `api.artic.edu` is unaffected. The firmware marks AIC
  unavailable (refresh and downloads skipped, cached art keeps playing,
  badge and banner in the web UI). The same host had a Cloudflare
  challenge episode in December 2025 during a DoS
  (art-institute-of-chicago/data-aggregator#151, resolved then). On
  2026-08-25 a desktop client fetched 10 sampled renditions without a
  challenge; the block is client-fingerprint based and the device is the
  fingerprint that gets refused. For the talk: one factual sentence, no
  blame; re-test on hardware the week before.
- **Fixed-bucket rendition regimes are spreading** (Wikimedia now
  rejects non-bucket widths; Cleveland and Mia are bucket-only). IIIF's
  size parameter is the thing that makes a 720 px panel not depend on
  luck.
- **Prior art.** Searching discuss.iiif.io, the IIIF Consortium's news
  archive, the Code4Lib Journal back catalog, and the open web found no
  prior IIIF Image API client implemented in microcontroller firmware.
  Stated as "as far as I can tell; I would welcome correction." The
  IIIF-Discuss post asked; no reply.

## 12. Numbers fact box

| Item | Value |
|---|---|
| Board | Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, $39.99 (Aug 2026) |
| MCU | ESP32-P4, dual-core RISC-V, 400 MHz |
| RAM | 32 MB PSRAM + ~768 KB internal SRAM |
| Display | 720x720, 4-inch IPS, 24-bit, capacitive touch |
| Firmware | C on ESP-IDF v5.5, FreeRTOS, Apache 2.0, v1.2.1 |
| Museums | 9 shipped; 7 over IIIF Image API v2; 2 fixed-rendition CDNs; AIC dormant |
| Adapter code | ~4,700 lines of C across 9 adapters; 59 shared IIIF lines; 994 lines of core; ~3,060 lines of browser-side JS |
| Artwork record | 64 bytes; 48 for the image id |
| Channel identifier slot | 33 bytes |
| Cache | 1,024 artworks per channel by default, max 4,096; 64 channels max |
| Refresh | every 4 days by default; 41-82 API calls per full refresh |
| Rendition | `full/!720,720/0/default.jpg`, JPEG, no `info.json` |
| Parse buffers | 192 KB (AIC, Rijks) to 1 MB (Smithsonian) |
| TLS | 2 concurrent sessions device-wide; 16 MiB download cap; 32 KB streaming chunks |
| Search page cost | 14-185 KB for 50-100 artworks; 78-3,695 B per artwork |
| Manifest cost | 1.6-107 KB each |
| Presentation-only channel | 50-100x the requests of the bespoke path |
| Rijks set list | 193 curated sets, baked into flash |
| Rijks full populate | ~50 minutes for a fresh channel |
| Cooldown | 60 s default; `Retry-After` capped at 3,600 s |
| From "never heard of IIIF" to 3 museums live | 4 days (2026-05-07 to 05-11) |
| To 7 museums | 15 days (05-22) |

## 13. Assets

| Asset | Path | Use |
|---|---|---|
| Hero photo, Degas *Two Dancers* (AIC 45243, c. 1893-98, CC0) on the device | `images/photos/p3a-museum-channel-5.jpg` | title slide |
| Four more museum-channel photos | `images/photos/p3a-museum-channel-{1..4}.jpg` | what-p3a-is slide, backgrounds |
| Pixel-art and Giphy photos | `images/photos/p3a-{1..7}.jpg`, `p3a-*-giphy.jpg`, `p3a-with-makapix-club-artwork.jpg` | identity slide |
| Board diagram | `images/hardware/ESP32-P4-WIFI6-Touch-LCD-4B-details-size.jpg` | hardware slide |
| Buttons, ports, gestures | `images/hardware/p3a-buttons-ports-gestures.webp` | optional |
| Architecture figure (browser, device, museums, shared cooldown) | `docs/outreach/code4lib-journal/figure-2-architecture.svg` (PNG next to it) | polite-client slide |
| Web UI screenshot | `images/screenshots/p3a-web-ui-2.20.png` | optional; the browse modal has no screenshot yet, capture one from p3a-fab.local |
| Fallback demo clip (five-museum era) | `images/videos/art-institution-channels/p3a-museums-2026-05-19.mp4` (8.9 MB) | fallback only |
| Logo | `images/brand/p3a-alpha-x2-128p.png` | footer |

## 14. Things to verify before the talk

- AIC block status on hardware (section 11).
- Harvard `collections/object` still 500; Wellcome facet children still
  503 (section 6.3 commands). If either healed, the adoption slide
  changes wording, not conclusion (the specification and economics
  grounds stand alone).
- Board price on waveshare.com.
- Adapter line counts if any adapter changed (`wc -l components/art_institution/museums/*.c`).
- Whether the IIIF-Discuss post got any late replies.
- Whether Code4Lib has moved (status as of 2026-08-24: on hold for new
  issues, submission in the queue).

## 15. Source pointers

| Topic | Where |
|---|---|
| Article draft (thesis, stories, checklist, numbers) | `docs/outreach/code4lib-journal/article.md` |
| Presentation API probes | `docs/outreach/code4lib-journal/presentation-api-research.md` |
| Consolidated technical facts | `docs/outreach/code4lib-journal/source-material.md` |
| Museum design, per-museum specs, rendition survey, field fixes | `docs/art-institutions/finalized-design.md` |
| museum-ubi charter and candidate matrix | `reference/museum-art/CLAUDE.md`, `reference/museum-art/docs/museum-candidates.md` |
| Per-museum research reports | `reference/museum-art/source/*/output/report.md` (Rijks: `out/REPORT.md`) |
| LoC and Harvard investigations | `docs/art-institutions/loc-investigation/REPORT.md`, `docs/art-institutions/ham-investigation/REPORT.md` |
| Smithsonian units and exclusions | `reference/museum-art/source/smithsonian/DEFERRED.md` |
| Gallica, Wellcome long labels | `docs/deferred/gallica.md`, `docs/deferred/wellcome-long-labels.md` |
| Content-sources survey (bot defense, dying APIs, buckets) | `docs/content-sources-survey.md` |
| IIIF-Discuss email as sent | `docs/outreach/iiif-discuss-thread.md` |
| User-facing museum behavior | `docs/HOW-TO-USE.md`, "Museum Channels" |
| Adapters | `components/art_institution/museums/*.c`, `webui/museum/*.js` |
| Demo video brief (outreach version) | `docs/outreach/museum-mode-video-brief.md` |
