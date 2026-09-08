---
marp: true
theme: default
size: 16:9
paginate: true
title: IIIF from the smallest patron
description: p3a at the IIIF Community Call, 2026-10-14 (v2, sparse deck)
footer: "IIIF from the smallest patron · Fabrício Kury · IIIF Community Call, 14 October 2026"
style: |
  section {
    font-family: "Segoe UI", "Helvetica Neue", Arial, sans-serif;
    font-size: 30px;
    padding: 60px 80px;
    background: #fbfaf7;
    color: #1d1d1b;
  }
  section h1 { font-size: 1.6em; color: #1d1d1b; margin-bottom: 0.5em; }
  section h2 { font-size: 1.15em; color: #555; font-weight: 600; margin-bottom: 0.4em; }
  section p, section li { line-height: 1.4; }
  section ul { margin-top: 0.2em; }
  section li { margin-bottom: 0.25em; }
  section code { font-size: 0.85em; background: #eeece6; padding: 0.05em 0.3em; border-radius: 4px; }
  section pre { background: #eeece6; border-radius: 8px; font-size: 0.9em; padding: 0.6em 0.9em; }
  section table { font-size: 0.68em; margin: 0.4em 0; }
  section th { background: #e6e2d8; }
  section td, section th { padding: 0.25em 0.6em; }
  section footer { color: #8a8780; font-size: 0.45em; }
  section::after { color: #8a8780; font-size: 0.5em; }
  section.title { background: #23303a; color: #fbfaf7; }
  section.title h1 { color: #fbfaf7; font-size: 2.0em; margin-bottom: 0.1em; }
  section.title h2 { color: #c8c3b7; font-weight: 400; }
  section.title p { color: #c8c3b7; font-size: 0.75em; }
  section.title footer, section.title::after { display: none; }
  section.dark { background: #23303a; color: #fbfaf7; }
  section.dark h1 { color: #fbfaf7; }
  section.dark p, section.dark li { color: #e6e2d8; }
  section.dark footer, section.dark::after { color: #7d8790; }
  section.dense { font-size: 24px; }
  section.dense table { font-size: 0.7em; }
  section.dense li { margin-bottom: 0.15em; }
  section.quote p.big { font-size: 1.45em; line-height: 1.3; color: #23303a; margin: 0.5em 0 0.6em; }
  .muted { color: #6f6b62; }
  .small { font-size: 0.7em; }
  .cols { display: grid; grid-template-columns: 1fr 1fr; gap: 1.4em; }
  .num { font-size: 2.6em; font-weight: 700; color: #23303a; line-height: 1; display: block; }
  .numlabel { font-size: 0.85em; color: #6f6b62; display: block; margin-bottom: 0.8em; }
---

<!-- _class: title -->
<!-- _paginate: false -->

![bg right:42%](../../../images/photos/p3a-museum-channel-5.jpg)

# IIIF from the smallest patron

## What nine museum APIs look like from a 32 MB art frame

**Fabrício Kury**
Biomedical informatician in New York City. Currently works with Medicare claims data analytics at Sparx, Inc.

IIIF Community Call · 14 October 2026 · github.com/fabkury/p3a

<!--
- [0:00] Thank the host and the Slack member who invited you. One line on how you got here: posted on IIIF-Discuss in August, an invitation followed on Slack.
- Photo: Two Dancers, Edgar Degas, c. 1893-98, Art Institute of Chicago, CC0. Requested from AIC's IIIF endpoint at exactly the panel's resolution, 720 by 720.
- Position yourself: an outsider. You had never heard of IIIF before May 2026. This is a field report from the far edge of the audience, from the smallest regular visitor these endpoints have.
- Say the takeaway once, up front: the Image API kept its promise; discovery above it is the gap. Everything else is evidence for that sentence.
- Gratitude before anything: open-access programs and IIIF endpoints exist because people inside these institutions fought for them. Nothing in the talk is a complaint.
-->

---

# What p3a is

![bg right:40%](../../../images/photos/p3a-with-makapix-club-artwork.jpg)

- Open-hardware desktop art frame, Apache 2.0
- $39.99 board, 4-inch 720 × 720 panel
- 400 MHz microcontroller, 32 MB of RAM
- No operating system, no browser, no cloud
- Pixel art, GIFs, and since May, museums

<!--
- [0:40] Began as a pixel-art player: Makapix Club (a pixel-art community you also run, open source), Giphy and Klipy, files on the SD card, artwork from a URL. Say Makapix in one sentence and move on.
- Hardware: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, $39.99 at the manufacturer as of August 2026; under $70 with a microSD card and a USB-C supply. "Less than most exhibition catalogs."
- ESP32-P4: dual-core 400 MHz RISC-V. 32 MB PSRAM plus about 768 KB of internal SRAM. FreeRTOS only. Hardware JPEG decoder with limits. Wi-Fi 6 via a companion ESP32-C6. Firmware in C on ESP-IDF.
- "Microcontroller" is accurate; that is what the P4 is. Pre-empt pedantry with the spec line.
- The two numbers that matter for the rest of the talk: 32 MB of RAM in total, and 720 pixels on a side.
- No middleman: the firmware talks to the museum's server directly and decodes the JPEG on the chip. No account, no subscription, no server of yours in between. Updates itself over the air; runs on real people's desks.
- Since May 2026: nine museums, seven over the IIIF Image API, two over their own image CDNs.
-->

---

<!-- _class: dark -->

# Ninety seconds of the device

<video src="../demo.mp4" width="100%" controls muted></video>

<!--
- [2:00] Play demo.mp4 from the folder above (fallback: images/videos/art-institution-channels/p3a-museums-2026-05-19.mp4). Narrate live; cues in video-shot-list.md.
- Shot 1-2: "Forty-dollar board, four-inch square panel, no operating system, no browser. Everything on screen right now came over the IIIF Image API from the museum's own server."
- Shot 3: "The device stores no metadata. Tap the info button and your phone asks the museum for title, artist, date."
- Shot 4: "Channels are the museum's own facets. This is a Rijksmuseum curated set; the preview is resolving a Linked Art chain live."
- Shot 5-7: "A minute later it is on the frame."
- Stop the clip at 3:30 regardless of where it is.
-->

---

<!-- _class: dense -->

# Nine museums, the museum's own facets

| Museum | Facets you can pick | Key |
|---|---|---|
| Art Institute of Chicago | Departments, classifications, subjects, themes, galleries, artwork types | none |
| Rijksmuseum | Curated sets (193) | none |
| Victoria and Albert Museum | Collections, categories, venues | none |
| Wellcome Collection | Work types, genres, subjects, contributors | none |
| Statens Museum for Kunst | Collections | none |
| Harvard Art Museums | Classifications, centuries, cultures, periods, places, media, techniques, work types, groups, galleries | free key |
| Smithsonian | Cooper Hewitt, SAAM, NPG, NMAAHC, Hirshhorn, African Art | free key |
| Cleveland Museum of Art | Departments, types (CC0 only) | none |
| Minneapolis Institute of Art | Classifications, departments, countries, styles (public domain only) | none |

<!--
- [3:30] A channel is a list of artworks assumed too large to fetch at once: a (museum, facet, term) triple, for example "Rijksmuseum, curated set, Dutch Paintings of the Seventeenth Century" or "V&A, category, Photographs".
- A playset mixes channels at chosen ratios of airtime. The user assembles it in a small web app the device serves to a phone on the same Wi-Fi.
- Defaults: listing refreshed every 4 days; 1,024 artworks cached per channel on the SD card (max 4,096); 30 seconds per artwork; 64 channels max.
- The channel abstraction predates IIIF by months; it was built for Giphy and Makapix. The museum feature had to fit it.
- Facets are the museum's own vocabulary. p3a invents no taxonomy. Rijksmuseum has one axis (its 193 curated sets); Harvard has ten.
- Seven of nine need no key. Harvard: free key by email. Smithsonian: free api.data.gov key; the shared demo key cannot survive one refresh.
- AIC is listed but currently disabled; you will say why near the end. One sentence now if asked: "its image host challenges non-browser clients since August."
- The device stores no metadata. Title, artist, date come from the browser, on demand, from the museum's own API. Different for every museum; IIIF standardizes none of it.
-->

---

# One URL template

```
{iiif_base}/{identifier}/full/!720,720/0/default.jpg
```

- Image API 2, best fit in 720 × 720, JPEG
- Worked unmodified on seven image stacks
- Fifty-nine lines of shared C

<!--
- [5:00] This is the standards success story. Say it plainly: the people who wrote and implemented the Image API should hear it.
- Once an adapter has produced an image identifier, museums stop being different from one another. The firmware builds this one URL and saves the response to the SD card. That is the entire institution-independent contract.
- The seven stacks: Chicago's in-house server; the Rijksmuseum's images served by Micrio, a commercial deep-zoom vendor; the V&A's framemark host; Wellcome's own service; Copenhagen's IIPImage; Harvard's URN resolver, which 303s to the real image host; the Smithsonian's IDS.
- Seven independently operated image stacks, one line of URL construction. Nothing else in this talk is uniform.
- The shared IIIF helpers used by every adapter are 59 lines of C. Hold that number; it comes back.
- !720,720 never crops: across about 90 reported-versus-delivered comparisons (survey of 2026-08-25), the delivered aspect ratio equalled the master's at every IIIF museum.
-->

---

# Three deliberate simplifications

- **No `info.json`.** `!720,720` honored everywhere; zero misfires
- **JPEG only.** Hardware decoder; progressive falls back to software
- **Stream, never buffer.** 32 KB chunks, two TLS sessions, 16 MiB cap

<!--
- [6:30] Each of these is a choice a browser never has to make.
- No info.json: a capable client negotiates, fetches info.json, learns sizes and features, chooses. p3a never does. The panel is 720 by 720; !720,720 has been universally honored; skipping negotiation halves the request count on a device that pays a TLS round trip per request. Not one misfire since the feature shipped on 2026-05-15. Where advertisement and reality diverged, they diverged the other way; SMK, later.
- JPEG only: museum servers serve JPEG far more reliably than WebP, and the ESP32-P4 has a hardware JPEG decoder with sharp limits. It cannot decode progressive JPEGs at all; ESP-IDF additionally rejects images whose pixel count is not a multiple of 8 (bites artworks smaller than 720 px); a decoded image can exceed what PSRAM holds. Everything the hardware rejects falls back to libjpeg-turbo in software, fenced so a corrupt file cannot crash the device. Spot check in August: five of seven IIIF museums served baseline JPEG at this rendition; Chicago and Harvard served progressive, so every image from those two decodes in software.
- Stream, never buffer: images move to the SD card in 32 KB chunks through a fixed buffer, temp file, rename on completion. Two concurrent TLS sessions device-wide. A 16 MiB cap in case a server ignores the size request and returns the master. "A client with 32 MB of total RAM does not get to just download the image and see."
-->

---

# What a 64-byte budget clarifies

<div class="cols">
<div>

<span class="num">64 B</span><span class="numlabel">per artwork, 48 of them for the image id</span>
<span class="num">33 B</span><span class="numlabel">per channel identifier</span>

</div>
<div>

<span class="num">192 KB – 1 MB</span><span class="numlabel">parse buffer, sized per museum</span>
<span class="num">142 vs 3,700</span><span class="numlabel">bytes per listed artwork, Chicago vs SMK</span>

</div>
</div>

<p class="muted">Everything implicit becomes a buffer size, a request count, or a byte.</p>

<!--
- [7:45] This slide exists so "small client" is concrete for people who think in servers.
- The 64-byte record: a hash, dimensions, a timestamp, one byte of file type, and 48 bytes for the museum's image identifier. Identifiers longer than 48 bytes are silently unusable. That is what excluded the Library of Congress; later.
- Identifier length is an interoperability property. Chicago's image UUIDs (36 chars), Harvard's URNs (about 25), Micrio's ids (about 8), Cleveland accession numbers (up to 14) all fit.
- Identifier stability is one too. The record survives firmware updates, cache rebuilds, re-listings. An identifier that drifts reads as a deletion plus a brand-new artwork: old record evicted, image re-downloaded under the new id, old file sits unreferenced until age-based cleanup collects it.
- The 33-byte channel identifier slot (32 chars plus null): Wellcome terms whose label exceeds 32 characters cannot become channels, because Wellcome filters by label, not by id.
- Parse buffers, allocated for the lifetime of a refresh: Chicago and Rijksmuseum 192 KB, V&A, Wellcome, Cleveland 256 KB, SMK and Minneapolis 512 KB, Smithsonian 1 MB. Verbosity is a tax paid in RAM: invisible on a laptop, on this device it is the difference between a 192 KB and a 512 KB buffer.
- Not all RAM is equal: 768 KB internal versus 32 MB PSRAM. A 1 MB JSON of small nodes would fragment the DMA-capable internal pool the Wi-Fi path needs and panic the chip, so JSON parsing is redirected to PSRAM.
- One byte of state is enough, barely: the file-type byte doubles as the Rijksmuseum resolver's state machine (0xFF not yet resolved, 0xFE given up).
-->

---

# Being a polite client

![bg right:50% fit](../figure-architecture.svg)

- Identify yourself, with an email
- Honor `Retry-After`; remember it; share it
- One rate-limit budget per household
- Pace even when allowed

<!--
- [9:00] A device that lives in living rooms and refreshes museum APIs on a timer had better be a good citizen. Several mechanisms exist purely for politeness.
- Identify yourself: every request carries `p3a/{version} (pub@kury.dev)`. Chicago asks for this explicitly via a custom header; the Smithsonian enforces the spirit of it with a firewall (later).
- Honor Retry-After, remember it, share it: one cooldown slot per museum on the device, RAM-only. Any HTTP 429 engages it: Retry-After honored when present, capped at an hour; 60 seconds otherwise, sized to Chicago's 60-requests-per-minute window. Every layer that dials out checks it: the refresh gate, the adapter, the download loop, the Rijksmuseum resolver. Cooldowns only extend, never shorten.
- One budget per household, the subtle one: the browse UI runs in the user's browser and queries museum APIs directly, so browser and device share one public IP, and museum rate limits are per IP. When the browse UI receives a 429 it reports it to the device (a small REST call), engaging the same cooldown the device's own traffic would; before expensive browse operations the UI polls the device's cooldown table and waits its turn. That is the figure: the dashed box is the household.
- Pace even when allowed: 150 to 200 ms between page fetches during refresh; browse probes capped at six concurrent; two TLS connections device-wide. A full channel refresh costs roughly 41 to 82 API calls against daily quotas of 1,000 to 2,500 where quotas exist, and runs every four days.
- The figure is the article's, updated to nine museums.
-->

---

# How it got built

- **7 May:** a survey. First contact with IIIF
- **11 May:** three museums live, end to end
- **22 May:** seven museums
- **14 Aug:** Chicago's image host starts challenging bots
- **24 Aug:** nine museums; two without IIIF

<!--
- [10:15] Four days from "never heard of IIIF" to three museums live; fifteen days to seven. The speed is the point of the next slide.
- 7 May: the museum-ubi survey. 18 sources in a capability matrix, four tiers; per-museum probe scripts and reports. An LLM-driven survey; this is where IIIF entered the picture.
- 10 to 11 May: design written as five rounds of Q&A. Firmware component built in one day: component skeleton, scheduler wiring, Chicago adapter and the IIIF download path, eviction, REST endpoints for rate-limit sharing, web UI. Chicago, Rijksmuseum, V&A end to end. museum-ubi imported into the repo as reference.
- 12 May: Wellcome, SMK. Gallica deferred (SRU/XML, no XML parser on the device).
- 13 to 15 May: Library of Congress investigated, scaffolded, removed (identifier length). v0.10.0 ships with five museums.
- 18 to 22 May: Harvard, Smithsonian. Seven.
- 7 to 9 August: Presentation API probes for a longer write-up.
- 14 August: Chicago's IIIF host starts answering non-browser clients with a Cloudflare challenge; AIC disabled in firmware, cached art still plays.
- 23 to 24 August: Cleveland and Minneapolis, the first non-IIIF museums. Nine.
- 25 August: first post to IIIF-Discuss, which is why you are here.
-->

---

# A sandbox first, and a disclosure

- museum-ubi: a unified browsing interface, built to de-risk p3a
- Three features per source: list collections, list artworks 200–249, search
- JS adapters became the browse UI; C adapters written from the reports
- p3a is built end to end with LLM assistance
- **An agent scouting for content sources came back with the IIIF Image API**

<!--
- [11:45] museum-ubi: "unified browsing interface". A throwaway project to de-risk the p3a feature before committing firmware work. The question was whether heterogeneous museum APIs could be normalized behind one interface at all.
- Its charter: collapse categories, sections, folders, sets, departments into one concept, collections. Three features every source must support: list collections; list artworks 200 to 249 of collection X (range, not next-cursor); keyword search. "The hard problem was never one integration; it was reconciling heterogeneous capabilities behind one interface." Each source documents what is native, emulated, or unavailable.
- Deliverables: the capability matrix; Python probes and a report per museum; a small hash-routed browser app with one JS adapter per museum and Playwright tests.
- Carried into p3a: the JS adapters became the browse UI (now about 3,000 lines across nine adapters); the C adapters were written fresh from the reports. Keyword search, the third feature, is still deferred; channels are facet-based.
- Its charter also said IIIF Presentation was "strongly preferred, the natural carrier" for collections. That preference did not survive contact. Shortly.
- The disclosure, said straight, once: p3a is developed end to end with LLM assistance, in the Cursor IDE and with Claude Code. The design documents are literally Q&A transcripts. When you wanted more content sources you sent agents to scour the web; one came back with something called the IIIF Image API. You had never heard of it. "This talk exists because a language model introduced me to a library standard."
- Every technical claim in this talk was verified by you against live endpoints, and you take responsibility for them. No affiliation with Anthropic, Cursor, or any museum.
-->

---

# What the client needs from discovery

1. List collections, in the museum's own vocabulary
2. Enumerate one, **with a total, at an arbitrary offset**
3. An image identifier **inline** in every record

And nothing else: the device stores no metadata.

<!--
- [14:00] Long before IIIF entered the picture, the channel design fixed what a discovery layer must provide. Three primitives, for every museum.
- One: list collections, whatever the museum's vocabulary is: departments, classifications, curated sets, administrative units.
- Two: enumerate a collection with a total count at an arbitrary offset. "Artworks 400 to 499 of Paintings", not merely "next page". Counts and offsets matter because a channel can be configured to start deep in a collection, and because a device that caches 1,024 of 735,001 artworks needs to know both numbers.
- Three: yield an image identifier inline. Each listing record must carry the key that unlocks pixels, without a further per-artwork request. The single biggest determinant of client cost.
- No metadata: 48 of 64 bytes go to the image key. Title, artist, date are fetched by the browser on demand from the museum. That austerity is analytically useful: "can a small client browse this museum?" reduces to three yes-or-no questions, which makes the comparison between museums, and between APIs and standards, unusually crisp.
- These are museum-ubi's three features minus keyword search, plus the inline-id requirement the 64-byte record forced.
-->

---

# One template, nine adapters

<div class="cols">
<div>

<span class="num">59</span><span class="numlabel">lines cover everything the standard standardized</span>

</div>
<div>

<span class="num">4,700</span><span class="numlabel">lines are the difference between "has an API" and "has the same API"</span>

</div>
</div>

- Nine adapters, same three primitives, nine search APIs
- Chicago: best documented, still the longest

<!--
- [15:15] Line counts as of September 2026, C source per adapter: Chicago 849, Rijksmuseum 645, Smithsonian 526, Minneapolis 491, Harvard 477, Cleveland 475, Wellcome 438, SMK 406, V&A 399. Shared IIIF helpers used by all: 59. The core around them (dispatch, refresh, download, resolve, rate limit): about 1,000 more.
- A crude but honest proxy for how far each API is from the happy path.
- None of these museums is doing anything wrong. Each API is internally coherent and most are genuinely good. The point is the variance: nine reasonable APIs are still nine APIs, and the client pays for each difference in code, in buffer sizes, and in failure modes that must each be learned once, the hard way.
- Chicago's API is the best documented you found, and the adapter is still the longest; the reason is on the quirks slide.
-->

---

<!-- _class: dense -->

# Could Presentation have been the layer? Adoption

Live probes, 7 and 9 August 2026, Harvard with a valid key.

| Museum | Collection document | Per-artwork manifest |
|---|---|---|
| Chicago | No; `/iiif/2/collection` resolves as an image named "collection" | Yes, P2, 2.5 KB |
| Rijksmuseum | No; Linked Art + ActivityStreams instead | Yes, P3, 1.6 KB, label is a hash |
| V&A | No; root 404 | Yes, P2, 2.9 KB |
| Wellcome | **Yes**, P3 tree; both facet children **503**, both dates | Yes, P3, 107 KB |
| SMK | None advertised | Yes, P3, 3.4 KB, from the search host |
| Harvard | **Root exists**; its one child returns **500**, both dates | Yes, P2, 9.2 KB |
| Smithsonian | Not in the documentation | Undocumented |

Two of seven publish a Collection; both broke where enumeration begins. Manifests presuppose discovery.

<!--
- [16:30] The early museum-ubi notes called Presentation "strongly preferred, the natural carrier". The shipped system uses none of it. Because that decision was made in the fog of development, you re-examined it with fresh probes of all seven IIIF museums on 7 August, failure cases re-verified 9 August, Harvard's with and without a valid key. The answer is a confident no, on three independent grounds; this is the first.
- Chicago: the image server treats "collection" as an image identifier and redirects to an info.json that 404s. Conclusive: nothing mounted there. Per-artwork manifests exist, hanging off the search API's URL space; you need the search API to learn the id first.
- Rijksmuseum: no Collection; discovery is Linked Art with ActivityStreams paging. The Micrio manifest exists (P3, 1.6 KB) but its label is an opaque hash: it serves neither discovery nor display.
- V&A: clean per-object manifests, no collection surface at all.
- Wellcome: the strongest adopter of the seven, a handsome top-level tree that even materializes facet axes (subject, genre, contributor) as sub-collections. Both facet children probed returned 503 on both dates, each response describing itself as "temporarily unavailable". Reproducible across dates. And even when they work, P3 Collections carry no totals and no paging.
- SMK: manifests served by the bespoke API host, keyed by object number obtained from search. No collection advertised.
- Harvard: the textbook case. Ships the advertised Collection root; the single child that would enumerate objects returns 500, identically with and without a key, on both dates. Dead one level down, exactly where discovery would begin.
- Smithsonian: IIIF absent from the documentation entirely.
- Six of seven serve lovely per-artwork manifests, but a manifest is addressed by an object id only the search API can supply: manifests presuppose discovery rather than provide it.
- Be gentle and precise: these are observations on two dates; you will re-probe the week before the call and say what you found. If Wellcome or Harvard staff are on: "I would love to be told this was an outage." The adoption ground is the weakest of the three on purpose; the next slide stands without it.
-->

---

# Specification, and economics

- Presentation 3.0: Collections are ordered trees, no paging, no counts, no offsets
- Content Search: text within one object
- Change Discovery: *build a search engine, then query your copy*
- One search page: 50–100 artworks with image ids, 14–185 KB
- Presentation path: about 1,000 manifest fetches. **50–100× the requests**

<!--
- [18:30] Even under perfect adoption, the current spec cannot express what a browsing client asks.
- Presentation 3.0 scopes itself to what a viewer needs to render an object and says plainly that discovery and harvesting are not what it provides. Collections are ordered trees of references: no paging construct (2.1's paging properties were removed in 3.0), no item counts, no offset access, no facets, no metadata search.
- Presentation 2.1 did have paging (first, next, total, startIndex), but total is optional and navigation is link-following only. Even the friendliest reading gives sequential traversal with an optional count: the Rijksmuseum cursor walk, in other words, with all its costs.
- Content Search searches annotation text within a single object and states that descriptive metadata is out of scope. "Find this word in this digitized book", not "find artworks by department".
- Change Discovery is a harvester's ActivityStreams feed that presumes the client maintains its own index. The IIIF discovery model is: build a search engine, then query your copy. A microcontroller cannot be a harvester. It needs the institution's index, queryable in place, which is exactly what every museum's bespoke search API is.
- Economics, measured the same day. One bespoke search page: Chicago 14 KB for 100 artworks, about 142 bytes each; Rijksmuseum 8 KB for 100 ids only, plus three JSON-LD hops per artwork later; V&A 101 KB for 100; Wellcome 110 KB for 100; SMK 185 KB for 50. One manifest: 1.6 KB at Micrio, 2.5 at Chicago, 2.9 at the V&A, 3.4 at SMK, 9.2 at Harvard, 107 KB at Wellcome. A single Wellcome manifest weighs as much as an entire hundred-work catalogue page.
- For one 1,024-artwork channel: 10 to 20 requests on the bespoke path (140 KB at Chicago, about 2 MB at SMK) versus a tree walk plus about 1,000 manifest fetches: 2.5 MB at Chicago sizes, over 100 MB at Wellcome sizes. A 50 to 100 times request multiplier for a client that pays TLS per request. And at the end of it, still no counts and no offsets.
- Quote the scope statements accurately; do not editorialize beyond "scoped for viewers and harvesters".
-->

---

<!-- _class: quote -->

# The conclusion I did not expect

<p class="big">Bespoke search for discovery and IIIF for pixels is not a betrayal of the standard. It is the division of labor the specifications prescribe.</p>

- The Rijksmuseum's Linked Art walk is the standards path, in production, at a price
- The gap is real and known. Presentation Collections will not close it as they stand
- **This is the claim I would like you to push back on**

<!--
- [20:30] Pause here. This is the ask.
- The Rijksmuseum adapter is the exception that measures the rule: its Linked Art walk is the standards-based discovery path, working in production, at the price of cursor-only access, emulated offsets, and three extra fetches per artwork. Details in two slides.
- The conclusion you did not expect to reach: p3a's split is not a pragmatic betrayal of the standard. It is the division of labor the IIIF specifications themselves prescribe. The gap between "there should be a standard way to browse collections" and "there is one" is real, known to this community, and, as the adoption findings suggest, unlikely to be closed by Presentation Collections as they stand.
- Make the ask explicit: is the conclusion right? Is anything on the roadmap meant to close it: a Discovery working-group effort, a query surface over Collections, a profile for small clients? If there is an active proposal you have not seen, this is where you want to hear about it. You would happily test it against a client this small.
- Secondary, only if it comes up: pointers to prior firmware-level IIIF clients. You searched discuss.iiif.io, the Consortium's news archive, the Code4Lib Journal back catalog, and the open web; found none; asked on the list; no reply. "As far as I can tell; I would welcome correction."
-->

---

<!-- _class: dense -->

# Idiosyncrasies, one line each

<div class="cols">
<div>

- **Chicago:** `from + size` capped at 1,000, undocumented
- **Rijksmuseum:** three JSON-LD hops to reach an image id
- **V&A:** venue counts read 0 with the has-images filter
- **Wellcome:** filters by the label string; no stable id
- **SMK:** `info.json` promises WebP; server says 400

</div>
<div>

- **Harvard:** half of "has image" has no image, without a permission gate
- **Smithsonian:** WAF rejects with **HTTP 200**
- **Cleveland:** no IIIF; fixed 750–1,300 px renditions
- **Minneapolis:** no IIIF; S3 buckets; `[]` past 10,000

</div>
</div>

<!--
- [22:00] Do not read the list. Say: "every one of these was learned the hard way, and none is documented as a failure." Pick two rows, then move to the deep dives.
- Chicago: anonymous callers discover empirically that from + size on search must stay at or under 1,000; paging past about page ten on large facets returns 403; a custom AIC-User-Agent header is requested. The adapter's one creative move: Chicago's search accepts Elasticsearch-style JSON bodies via POST and honors bool + range filters on the numeric artwork id, so for offsets beyond 1,000 the adapter partitions the museum: probes counts with size 0, recursively bisects the id space until every bucket holds at most 1,000 records (up to 64 buckets), then pages within buckets. A $40 device performing adaptive query planning against a museum's search engine, recomputed at each refresh because the collection moves. "I am fond of this code and slightly discontent that it needs to exist."
- Rijksmuseum: no image id in listings; cursor-only paging; set list over OAI-PMH without CORS headers, so the firmware bakes the 193-set list into its flash and serves it to its own configuration UI. The device carries a copy of the Rijksmuseum's table of contents wherever it goes, because of CORS headers.
- V&A: the venue facet returns count 0 when combined with the has-images filter; the browse UI enumerates venues unfiltered, then re-probes each term with the filter, browser side, once per session. Page times size capped at 10,000.
- Wellcome: genres, subjects, contributors filter by the label string itself, no stable id; labels over 32 characters cannot become channels (33-byte slot). Its /images endpoint silently ignores the /works filters.
- SMK: info.json advertises WebP, server returns 400 for it; asking to trim fields returns empty items, so 50 records cost about 205 KB.
- Harvard: without q=imagepermissionlevel:0, 57 of the first 100 "has image" records have no usable image URL (about 26% of the "has image" set is permission-restricted at a layer the flag does not see); the "IIIF server" is a URN resolver that 303s to the real host; 2,500 requests per day on the free key; terms longer than 32 chars dropped (about 22% of periods, 27% of galleries).
- Smithsonian: next slide. Also: usage:CC0 is not indexed for filtering; media is sometimes an object, sometimes an array; 50 records can exceed 512 KB.
- Cleveland: no IIIF, fixed _web.jpg renditions 750 to 1,300 px, median about 300 KB, up to about 1 MB; no facet endpoint, so vocabularies are baked at release from a full-corpus scan; width and height are strings, occasionally empty.
- Minneapolis: no IIIF; pre-rendered S3 buckets at 400, 800, full; a raw Elasticsearch query travels in the URL path; past the 10,000 window the endpoint returns a bare [] or 500; mojibake with unpaired surrogates in the Style facet.
- Cleveland and Minneapolis work on this panel only because their baked sizes happen to land near 720 px. IIIF turns that coincidence into a guarantee. Quiet advocacy from the consumer side.
-->

---

# Rijksmuseum: Linked Art, three hops down

- HumanMadeObject → VisualItem → DigitalObject → image URL
- Each hop a JSON-LD fetch; nothing inline
- Resolved lazily, one artwork per download pass
- Three failures tombstone; a later refresh forgives
- The honest price of the standards path. It works

<!--
- [23:30] The one museum of the nine whose discovery layer is itself standards-based: Linked Art documents traversed as an OrderedCollectionPage stream, 100 per page, opaque base64 pageToken, partOf.totalItems (for example 735,001). Say this with respect: the Rijksmuseum did the standards-based thing, and it works.
- Nothing in the listing carries an image identifier. For every artwork the client walks HumanMadeObject, shows[], VisualItem, digitally_shown_by[], DigitalObject, access_point[], whose URL finally names the Micrio image id. Each hop is a separate JSON-LD fetch: measured 13.5 KB, 1.5 KB, 0.6 KB.
- Three hops per artwork at listing time would be brutal, so the adapter resolves lazily: the refresh stores unresolved records (file-type byte 0xFF); the download loop resolves one artwork per pass, interleaved between image downloads. A walk costs a few hundred milliseconds; draining all would block downloads. A fresh channel takes about 50 minutes to fully populate; the first artworks appear within seconds.
- A record that fails resolution three times is tombstoned (0xFE) and skipped until a future refresh re-lists it, which resets its budget. The merge must never let an unresolved record overwrite a resolved one, or it orphans the file already on disk; tombstones are replaced to grant a fresh budget. Lock discipline: snapshot under lock, drop the lock for the network, re-find by id for writeback.
- Cursor-only pagination means the arbitrary-offset primitive is emulated: read the total from the stream, wrap the offset into range, pay ceil(N/100) throwaway page fetches to reach it.
- After resolution the key stores both the Micrio id and the object id, because there is no public reverse mapping from Micrio id to object, and the info view needs the object.
- One firmware wrinkle: id.rijksmuseum.nl 303s to data.rijksmuseum.nl, and ESP-IDF's HTTP client would not surface the Location header under the pattern used; the fix captures it in an on-header event handler.
- Every cost here is the honest price of the standards-based path. The Micrio manifest exists but its label is an opaque hash.
-->

---

# Two more, in one minute each

<div class="cols">
<div>

**Smithsonian: the WAF that says 200**

- Default User-Agent rejected
- With HTTP 200 and an HTML page
- Symptom: JSON parse error on success
- Fix: identify yourself. One line

</div>
<div>

**SMK: the `info.json` that lies**

- Friendliest API of the nine
- `info.json` advertises WebP
- Server returns 400 for it
- Negotiation would have hurt

</div>
</div>

<!--
- [25:00] Smithsonian: api.si.edu and ids.si.edu sit behind an F5 web application firewall that rejects requests with an empty or default User-Agent. It rejects them with HTTP 200 and an HTML page reading "Request Rejected". For a firmware client the failure signature is a JSON parse error on what claimed to be a successful response. Among the most misleading failure modes in the whole project: every signal along the way insisted the request had succeeded. The fix costs one line: User-Agent p3a/version (pub@kury.dev), exactly what a WAF wants of a well-behaved bot. The lesson is simple in hindsight; the diagnosis was not. Also the largest records of the nine: fifty can exceed half a megabyte, so this adapter alone budgets a 1 MB buffer. The shared demo key cannot survive one refresh; a free api.data.gov key gives 1,000 per hour against about 82 calls per refresh. This is the evidence for "reject loudly" on the checklist.
- SMK: the friendliest API of the nine. True offset pagination, anonymous, clean JSON, image dimensions right in the listing. One perfect irony: its IIPImage server's info.json advertises WebP and returns 400 when asked for it. The one museum where capability negotiation would have changed p3a's behavior is the one where negotiation lies. This is why skipping info.json has been safe: divergence goes the other way. An info.json is a promise, and small clients are the ones that believe it. Also: asking the search API to trim response fields returns empty items, so pages must be swallowed whole, about 4 KB per record, 512 KB of buffer for fifty. Keep the affection: SMK is the API you would point a newcomer at.
-->

---

# The ones that could not ship

- **Library of Congress:** IIIF ids up to 123 characters; 48-byte slot
- **Gallica:** SRU returns XML; the device has no XML parser
- **Getty:** enumeration only, 4.5 M entities, no search
- **Europeana, DPLA:** thumbnails, 400 px at most
- **Smithsonian NMAI, Freer/Sackler:** ARK ids unresolved; zero records

<!--
- [26:30] Framed as constraints of the device meeting properties of the API, never as verdicts on the institution.
- Library of Congress: excellent IIIF service, !720,720 verified, anonymous fetches work with a polite User-Agent. But identifier length is bimodal: 27 to 47 characters in Prints and Photographs, maps, manuscripts; 67 to 123 in newspapers and music. Whether an item fits the 48-byte slot depends on which storage subtree it lives in, not on any facet. And many listings carry only a 150 px thumbnail with no way to tell from the listing whether the IIIF service exists for the item. Scaffolded 13 May, removed 15 May. The clearest case of identifier length being an interoperability property.
- Gallica: catalogue search is SRU returning XML, OAI-PMH-flavored Dublin Core. The device has no XML parser and ESP-IDF ships none. Image API is v1.1. Also 403s default curl fingerprints.
- Getty: enumeration only, via an ActivityStreams OrderedCollection of about 4.5 million mostly non-artwork entities; no documented JSON search; range pagination would need a local index. The harvester model, exactly what a microcontroller cannot do.
- Europeana: the Thumbnail API caps at 400 px, and provider-level image URLs are too heterogeneous for one adapter. DPLA: thumbnails only. You asked on IIIF-Discuss whether anyone knows a reliable path to mid-size renditions across Europeana providers; still asking.
- Smithsonian units: NMAI's ARK identifiers do not resolve at the IDS IIIF endpoint (0 of the probed info.json returned 200); Freer and Sackler have zero Open Access records as of May. Also Yale (discovery only via LUX or OAI-PMH), National Gallery of Art (bulk CSV only). NYPL's API retired on 1 August.
- Also excluded on TLS-fingerprint grounds in the August survey: Finna, Science Museum Group, Brooklyn Museum. An mbedTLS handshake from an ESP32 is exactly the fingerprint these systems flag.
-->

---

<!-- _class: dense -->

# What would help clients smaller than a browser

1. Inline image identifiers in search results
2. True offset pagination, with totals
3. A filter that means "displayable image included"
4. `Retry-After` on every 429
5. Reject loudly: an error, not a 200
6. Advertise only what is served
7. Document the sharp edges

<p class="muted">Every item already exists in at least one of the nine APIs.</p>

<!--
- [27:45] Read the seven fast; the audience can screenshot. Every item already exists in at least one of the nine APIs, so this is curation, not invention.
- One: inline image identifiers in search results. The single biggest determinant of client cost. One field turns N+1 requests into 1.
- Two: true offset pagination with totals. Cursors force sequential walks; caps that appear only empirically force adapters to learn them empirically.
- Three: a filter meaning "displayable image actually included", that composes with other filters and reflects permissions, not just file existence. Harvard's permission gate is the example.
- Four: Retry-After on every 429. Clients that want to comply can only comply with what is stated.
- Five: reject loudly. An HTTP error for a rejected request; a WAF that says 200 turns every downstream parser into a liar.
- Six: advertise only what is served. An info.json is a promise, and small clients are the ones that believe it.
- Seven: document the sharp edges. Result-window caps, key-quota floors, permission gates. Every undocumented limit was learned by trial and error.
- Land on: none of this asks museums to design for microcontrollers. It asks something cheaper: that the properties small clients depend on (short stable ids, lean listings, honest capability advertisements) be recognized as compatibility surface, not implementation detail.
- Two things changing, briefly: museum APIs are mortal (NYPL's Digital Collections API retired in August; smaller ones die quietly; p3a carries a per-source kill switch). And bot defense is now a compatibility issue: since August, Chicago's IIIF host answers non-browser clients with a challenge, the firmware ships with AIC disabled, cached art still plays. One factual sentence, no blame; AIC was the first museum integrated and has the best-documented API. A legitimate small client is collateral unless "identify yourself honestly" remains a sufficient answer. Both trends raise the value of everything IIIF got right: the Image API's uniformity is precisely what makes a nine-museum client maintainable by one person and their language models.
-->

---

<!-- _class: title -->

# Thank you

## Is the discovery-gap conclusion right? Is anything on IIIF's roadmap meant to close it?

**Try it:** Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, $39.99 · fabkury.github.io/p3a/web-flasher · github.com/fabkury/p3a

**Fabrício Kury** · pub@kury.dev · A longer write-up is under review at the Code4Lib Journal · Slides CC BY 4.0

<!--
- [29:00] Try it, thirty seconds: the board plus a microSD card and a USB-C supply; flash from the browser at the web flasher; source, docs, and adapters on GitHub (adapters in components/art_institution/museums/, probes and design documents in docs/). Two museums need a free key (Harvard, Smithsonian); the other seven need nothing. If your institution's API lists artworks with a count at an offset and carries an image id inline, an adapter is a few hundred lines. Talk to me.
- Restate the ask in one sentence and stop talking. Fifteen minutes of questions; prepared answers in qa-prep.md.
- Corrections welcome, especially from the museums named. Everything shown was observed politely from outside.
- If silence: ask whether anyone serves a paged Collection with totals today, in any version.
-->
