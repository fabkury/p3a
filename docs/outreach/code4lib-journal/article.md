# Writing an IIIF Client on a $40 Microcontroller

*Fabrício Kury* — DRAFT for Code4Lib Journal submission. Not submitted.
Revised 2026-08-10 after author review (tracked changes + 9 comments
merged). Word count ≈ 4,500 (journal guidance: 1,500–5,000).

**Abstract.** p3a is an open-source desktop art frame built on a $39.99
ESP32-P4 development board with a 4-inch 720×720 touchscreen. Since May
2026 its firmware has spoken the IIIF Image API natively, pulling
artwork from seven institutions: the Art Institute of Chicago, the
Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection,
the Statens Museum for Kunst, the Harvard Art Museums, and the
Smithsonian. This article reports what the IIIF ecosystem looks like
from an unusually small client. The finding is an asymmetry: the IIIF
Image API delivered perfectly on its interoperability promise — one URL
template serves pixels from all seven institutions — while everything
above it (finding artworks, browsing collections, paginating results)
required seven bespoke adapters against seven very different search
APIs. I describe both layers, measure whether the IIIF Presentation API
could have closed the gap (it could not, for reasons that are partly
adoption and partly the specification itself), and end with a short
list of inexpensive things an institution can do to make its collection
usable by clients much smaller than a browser.

---

## 1. A very small patron

Seven museums will serve their collections, image by image, to a
computer that costs less than most exhibition catalogs. I want to say
at the outset that this is wonderful, and nothing in this article
should be read as a complaint. Open-access programs and IIIF endpoints
exist because people inside these institutions fought for them; what
follows is a field report on what those endpoints look like from the
far edge of their audience, with affection, from their smallest
regular visitor.

The visitor is p3a, an open-hardware desktop art frame [1]. The
hardware is a Waveshare ESP32-P4 development board called
ESP32-P4-WIFI6-Touch-LCD-4B, which retails at the manufacturer's
website for $39.99 as of August 2026. Add a modest microSD card, USB-C
power source and cable, and the whole build should stay under $70. The
ESP32-P4 is a dual-core 400 MHz RISC-V microcontroller. It has no
operating system beyond FreeRTOS and no browser stack. It enjoys 32 MB
of PSRAM plus about 768 KB of internal SRAM, a limited hardware JPEG
decoder (§3), and Wi-Fi version 6 via a companion ESP32-C6 chip. The
board carries a 4-inch, 24-bit, 720×720 IPS capacitive touchscreen.
The firmware is C on ESP-IDF. p3a began as a pixel-art player and
plays animations from Giphy and Klipy, artworks from Makapix Club (an
open-source pixel-art community I also run), and files from its SD
card. The museum feature grew out of a search for more things worth
putting on a good square screen that sits next to you on your desk or
shelf.

I should disclose the method of that search, because it is part of the
story: p3a is developed end-to-end with LLM assistance: first with a
variety of models (but mostly Anthropic's) inside the Cursor IDE,
later with Claude Code generally running the newest Opus-class model
available at the time. When I wanted new content sources, I sent LLM
agents to scour the web for candidates. One of them came back with
something called the IIIF Image API. I had never heard of it. It is
not an exaggeration to say that this article exists because a language
model introduced me to a library standard.

What made IIIF remarkable, from where I sat, is the premise of this
article. Integrating one museum's images usually means integrating
with one museum's image infrastructure; IIIF meant that "integrate
seven museums' images" was *one* engineering task. But only for the
images. Above the pixels — finding artworks, browsing collections,
paginating — no such layer exists in practice, and each museum cost a
separate adapter. One URL template, seven discovery layers. The rest
of this article walks the two halves of that sentence.

**Figure 1.** p3a displaying *Two Dancers* (Edgar Degas, c. 1893–98,
pastel and charcoal; collection of the Art Institute of Chicago, CC0).
The image on screen was requested from AIC's IIIF endpoint at exactly
the panel's resolution. *(File: `images/photos/p3a-museum-channel-5.jpg`;
photo by the author.)*

## 2. What the client needed

p3a's abstraction for a source of artworks is the *channel*. For
example, a channel can be a saved selection like "Art Institute of
Chicago → Departments → Modern and Contemporary Art" or "Rijksmuseum →
the curated set *Paintings*". A channel for p3a is a list of artworks
assumed to be very large, too large to obtain all at once. A user
assembles channels in a small web app the device itself serves to a
phone or laptop on the same wi-fi network. p3a can mix and match
channels at different ratios of airtime. The firmware then keeps each
channel stocked and periodically refreshes its listing every four days
by default. As soon as listings have been obtained, p3a begins
downloading images to a cache on the microSD card (1,024 artworks per
channel by default) and rotates them on screen. Each artwork is
displayed for 30 seconds by default. All these defaults can be
configured.

Long before IIIF entered the picture, this design fixed what the
discovery layer must provide. Three primitives, for every museum:

1. **List collections**: whatever the museum's own vocabulary of
   groupings is: departments, classifications, curated sets,
   administrative units.
2. **Enumerate a collection, with a total count, at an arbitrary
   offset**: "artworks 400–499 of *Paintings*", not merely "next
   page". Counts and offsets matter because a channel can be
   configured to start deep in a collection, and because a device that
   caches 1,024 of 735,001 artworks needs to know both numbers.
3. **Yield an image identifier inline**: each listing record must
   carry the key that unlocks pixels, without requiring a further
   per-artwork request.

What about artwork metadata — titles, artists, dates? The device
stores none of it: its per-artwork record is 64 bytes, of which 48
hold the image key, and it displays images full-bleed. Metadata is
fetched on demand instead — when a viewer taps the info button in the
web app while an artwork is on screen, the *browser*, not the device,
queries the originating museum's API for that artwork's details. That
lookup is, once again, different for every museum; IIIF standardizes
none of it. For the device-side survey that follows, though, the
austerity is analytically useful — it reduces "can a small client
browse this museum?" to the three primitives above, making comparisons
between museums, and between APIs and standards, unusually crisp.

## 3. The uniform layer

Once an adapter has produced an image identifier, museums stop being
different from one another. The firmware builds

`{iiif_base}/{identifier}/full/!720,720/0/default.jpg`

and saves the response to the SD card. That is the entire
institution-independent contract: IIIF Image API version 2, the
bang-size syntax requesting a best-fit within 720×720, rotation zero,
JPEG [2]. It worked, unmodified, on all seven services: Chicago's
in-house server, the Rijksmuseum's images served by Micrio (a
commercial deep-zoom vendor), London's framemark host, Copenhagen's
IIPImage installation, Harvard's delivery service, the Smithsonian's
IDS. Seven independently operated image stacks; one line of URL
construction. Whatever else this article observes, that is a standards
success story and the people who wrote and implemented the Image API
deserve to hear it.

Three deliberate simplifications kept the uniform layer uniform:

**No `info.json`.** A capable IIIF client negotiates: fetch the
image's `info.json`, learn the sizes and features on offer, choose.
p3a never does. The panel is 720×720, `!720,720` has been found to be
universally honored, and skipping negotiation halves the request
count. In the three months since the feature shipped this shortcut has
not misfired once. Where advertisement and reality diverge, they
diverge in the *other* direction (see SMK, below).

**JPEG only.** Museum IIIF servers serve JPEG far more reliably than
WebP, and the ESP32-P4 has a hardware JPEG decoder — with sharp
limits. It cannot decode progressive (SOF2) JPEGs at all; ESP-IDF
v5.5.1 additionally rejects any image whose pixel count is not a
multiple of 8 (which mostly bites artworks smaller than 720 px, since
a 720-pixel side always passes); and a decoded image can simply
exceed what PSRAM will hold. Everything the hardware rejects falls
back to a software decode via libjpeg-turbo, fenced so that a corrupt
file cannot crash the device. How the split falls depends on the
museum: in spot checks for this article, five of the seven served
baseline JPEG at this rendition — hardware territory — while the Art
Institute of Chicago and Harvard served progressive, so every image
from those two museums decodes in software.

**Stream, never buffer.** Images move to the SD card in 32 KB chunks
through a fixed-size buffer, written to a temporary file and renamed
on completion. The device-wide budget is two concurrent TLS sessions;
a 16 MiB cap guards against a server that ignores the size request
and returns a full-resolution master, or any otherwise runaway
response. A client with 32 MB of total RAM does not get to "just
download the image and see".

## 4. The bespoke layer: seven discovery adapters

Above the Image API, each museum is its own scenario. The plainest
evidence is the current line count of p3a's per-museum adapters, each
implementing the same three primitives against a different search API:

| Adapter (C source) | Lines |
|---|---|
| Art Institute of Chicago | 839 |
| Rijksmuseum | 645 |
| Smithsonian | 526 |
| Harvard Art Museums | 477 |
| Wellcome Collection | 438 |
| Statens Museum for Kunst | 406 |
| Victoria and Albert Museum | 399 |
| *Shared IIIF helpers, used by all seven* | *59* |

Fifty-nine shared lines cover everything the standard standardized.
The other ~3,700 are the difference between "has an API" and "has the
same API". What follows are the exhibits I would show a visitor: each
one a real behavior a client must handle, none of them documented
failures, all of them observed politely from outside.

### 4.1 Art Institute of Chicago: the thousand-record wall

AIC's is the best-documented museum API I found, and the adapter is
still the longest. Two behaviors of its Elasticsearch-backed search
explain most of it. First, anonymous callers discover (empirically,
not from the documentation) that `from + size` on `/artworks/search`
must stay at or under 1,000; deeper GET pagination is refused. Second,
paging through very large facets (say, *Painting*) starts returning
HTTP 403 past roughly page ten, independent of the first limit.
Reaching *past* that wall took the adapter's one genuinely creative
move. AIC's search accepts Elasticsearch-style JSON query bodies via
POST, and `bool` + `range` filters on the numeric artwork id are
honored for anonymous callers. So when a channel needs artworks
beyond offset 1,000, the adapter partitions the museum: it probes
record counts (a `size: 0` query costs almost nothing) and
recursively bisects the artwork-id space until every bucket holds at
most 1,000 records, then pages within buckets, all below the cap.

```text
partition(lo, hi):
    n = count(artworks with lo ≤ id < hi)     # "size: 0" probe
    if n ≤ 1000:        emit bucket [lo, hi)   # fits under the cap
    else if hi−lo ≤ minimum span: emit anyway  # give up narrowing
    else:               partition(lo, mid); partition(mid, hi)
```

A $40 device, in other words, performs adaptive query planning against
a museum's search engine: around sixty-four buckets, recomputed at
each refresh because the collection moves underneath it. I am fond of
this code and slightly discontent that it needs to exist.

### 4.2 Rijksmuseum: Linked Art, three hops down

The Rijksmuseum is the one museum of the seven whose discovery layer
is itself standards-based: Linked Art documents, traversed as an
`OrderedCollectionPage` stream [3]. It is also the adapter where p3a
performs semantic-web resource traversal in firmware, because nothing
in the listing carries an image identifier. For every artwork the
client walks: HumanMadeObject → `shows[]` → VisualItem →
`digitally_shown_by[]` → DigitalObject → `access_point[]`, whose URL
finally names the Micrio image id. Each hop is a separate JSON-LD
fetch.

Three hops per artwork at listing time would be brutal, so the
adapter resolves lazily: the refresh stores unresolved records, and
the download loop resolves one artwork per pass, interleaved between
image downloads. A record that fails resolution three times is
tombstoned and skipped until a future refresh re-lists the artwork,
which resets its budget. The cursor-only pagination also means the
"arbitrary offset" primitive is emulated: the client reads the total
from the stream's metadata, wraps the offset into range, and pays
⌈N/100⌉ throwaway page fetches to reach it. Every cost in this
paragraph is the honest price of the standards-based path, and
section 5 returns to it.

One quirk deserves its own sentence: the museum's curated set list —
the entry point for browsing — is published over OAI-PMH without CORS
headers, so a browser cannot fetch it; p3a therefore ships the set
list *baked into the firmware's flash filesystem* and serves it to
its own configuration UI. The device carries a small copy of the
Rijksmuseum's table of contents wherever it goes, because of CORS
headers.

### 4.3 Smithsonian: the WAF that says 200

The Smithsonian's search API and image service both sit behind a web
application firewall that rejects requests bearing an empty or
default User-Agent. It rejects them with HTTP **200** and an HTML
page reading "Request Rejected". For a firmware client, the failure
signature is therefore not an HTTP error but a JSON parse error on
what claimed to be a successful response — among the most misleading
failure modes encountered in this project. The fix costs one line:
identify yourself — p3a sends `p3a/{version} (pub@kury.dev)` — which
is exactly what a WAF wants of a well-behaved bot. The lesson is
simple in hindsight; the diagnosis was not, because every signal
along the way insisted the request had succeeded.

Smithsonian records are also the largest of the seven — a nested
metadata format in which fifty records can exceed half a megabyte —
so this adapter alone budgets a full 1 MB response buffer, and its
rights field turned out not to be indexed for filtering (a query
AND-ing `usage: CC0` silently matches nothing). And access requires a
free api.data.gov key: the shared `DEMO_KEY` is rate-capped too low
to survive a single channel refresh, a fact the settings UI explains
to users.

### 4.4 Harvard, Wellcome, SMK, V&A: a quirk apiece

**Harvard Art Museums** taught the value of reading query parameters:
without `q=imagepermissionlevel:0`, roughly half of the records
flagged "has image" arrive without a usable image URL (measured while
verifying this article: 57 of the first 100 such records; with the
gate, none); the missing half is permission-restricted at a layer the
`hasimage` flag does not see. Harvard's image "server", meanwhile, is
a URN resolver: the stored identifier is a URN, the IIIF path is
appended to it, and a 303 redirect lands on the actual image host.

**Wellcome Collection** exposes stable ids for some facet axes and,
for others (genres, subjects, contributors), filters by the *label
string itself*. p3a stores channel identifiers in a 33-byte slot, so
Wellcome terms with labels longer than 32 characters simply cannot
become channels.

**Statens Museum for Kunst** is the friendliest API of the seven —
true offset pagination, anonymous, clean JSON, with one perfect
irony: its image server's `info.json` *advertises* WebP and returns
400 when asked for it. The one museum where capability negotiation
would have changed p3a's behavior is the one where negotiation lies.
Its search API also returns empty results if asked to trim response
fields, so its metadata-rich pages must be swallowed whole (512 KB of
buffer for fifty artworks).

**The Victoria and Albert Museum** is the shortest adapter and mostly
just works, but computes `count = 0` for one facet type whenever the
has-images filter is applied, so the browse UI enumerates that facet
unfiltered and then re-probes each term with the filter to recover
honest counts — a browser-side, browse-time-only probe (it runs when
the user opens the venue axis in the browse modal, at most once per
session, six requests at a time across a few dozen terms; the
device's refresh path never does this).

None of these institutions is doing anything wrong, exactly. Each
API is internally coherent and most are genuinely good. The point of
the catalog is the *variance*: seven reasonable APIs are still seven
APIs, and the client pays for each difference in code, in buffer
sizes sized to the most verbose page (192 KB to 1 MB, per museum),
and in failure modes that must each be learned once, the hard way.

## 5. Could IIIF Presentation have been the missing layer?

A fair question at this point: IIIF has a second major specification,
the Presentation API, whose Collections explicitly model "groups of
things" [4]. My own early survey notes called it "strongly preferred…
the natural carrier" for collection browsing. The shipped system uses
none of it. Because the decision was made in the fog of development
(with, candidly, an LLM's hands on many of the levers), I re-examined
it for this article with fresh probes of all seven institutions
(2026-08-07; the failure cases re-verified 2026-08-09, Harvard's with
a valid API key). The answer is a confident no, on three independent
grounds.

**Adoption.** Of the seven museums, exactly two publish a
machine-discoverable IIIF Collection document, and both broke at
precisely the level where enumeration would begin. Harvard advertises
a Presentation collection root whose single child, *Objects*, returns
HTTP 500 — identically with and without a valid API key, on probes
two days apart. Wellcome — the strongest adopter, with a handsome
top-level tree that even materializes facet axes as sub-collections —
returned 503 for both facet children on both probe dates, each
response describing itself as "temporarily unavailable". Chicago's
`/iiif/2/collection` URL is parsed by its image server as an image
named "collection" (redirecting to an `info.json` that does not
exist); the V&A's manifest host has no collection surface; SMK
advertises none; the Smithsonian's documentation does not mention
Presentation at all. Six of seven do serve lovely per-artwork
*manifests*, but a manifest is addressed by an object id that only
the search API can supply, so manifests presuppose discovery rather
than provide it.

**Specification.** Even under perfect adoption, the current spec
cannot express what a browsing client asks. Presentation 3.0 scopes
itself to what a viewer needs to render an object, and says plainly
that discovery and harvesting are not what it provides: its
Collections are ordered trees of references, with no paging
construct (version 2.1's paging properties were removed), no item
counts, no offset access, no facets, and no metadata search — search
is delegated to the Content Search API, which searches annotation
text *within* a single object, and cross-collection discovery to the
Change Discovery API, a harvester's feed that presumes the client
maintains its own index. That last assumption deserves underlining:
the IIIF discovery model is *build a search engine, then query your
copy*. A microcontroller cannot be a harvester; it needs the
institution's index, queryable in place, which is exactly what every
museum's bespoke search API is.

**Economics.** Measured the same day: one bespoke search page
delivers 50–100 artworks, with image identifiers inline, in 14–185 KB
— Chicago manages about 142 bytes per artwork. The Presentation
equivalent is a tree walk plus one manifest per artwork (2.5 KB at
Chicago, 107 KB at Wellcome, where a single manifest weighs as much
as an entire hundred-work catalog page). For a standard 1,024-artwork
channel that is ten-to-twenty requests versus a thousand — a 50–100×
multiplier for a client that pays TLS per request — and at the end of
it the client still has no counts and no offsets.

The Rijksmuseum adapter, fittingly, is the exception that measures
the rule: its Linked Art walk *is* the standards-based discovery
path, working in production — at the price of cursor-only access,
offset emulation, and three extra fetches per artwork. The conclusion
I did not expect to reach: p3a's split (bespoke search API for
discovery, IIIF for pixels) is not a pragmatic betrayal of the
standard. It is the division of labor the IIIF specifications
themselves prescribe. The gap between "there should be a standard
way to browse collections" and "there is one" is real, known to the
IIIF community, and, as the adoption findings above suggest, unlikely
to be closed by Presentation Collections as they stand.

## 6. Being a polite client

A device that lives in living rooms and refreshes museum APIs on a
timer had better be a good citizen, and several of p3a's mechanisms
exist purely for politeness. They double as a working example of what
"a well-behaved small client" can mean, so I list them.

**Identify yourself.** Every request carries an identifying
User-Agent with a contact email (Chicago asks for this explicitly via
a custom header; the Smithsonian, as noted, enforces the spirit of it
with a firewall).

**Honor `Retry-After`, remember it, share it.** Each museum has a
cooldown slot on the device; any HTTP 429 engages it — honoring
`Retry-After` when present (capped at an hour), defaulting to sixty
seconds otherwise — and every layer that issues requests (refresh,
downloads, the Rijksmuseum resolver) checks it before dialing out.
Cooldowns only extend, never shorten.

**One budget per household.** The subtle one: p3a's browse UI runs in
the user's browser and queries museum APIs directly, so browser and
device share one public IP and museum rate limits are per-IP. The
two halves therefore share the budget explicitly: when the browse UI
receives a 429 from a museum, it reports it to the device
(`POST /api/museum/rate-limits/report-429`), engaging the same
cooldown the device's own traffic would; before expensive browse
operations, the UI polls the device's cooldown table and waits its
turn.

**Pace even when allowed.** Page fetches during refresh sleep 150–200
milliseconds between requests; browse-time probes cap their
concurrency; the whole device allows itself two TLS connections at
the same time. A full channel refresh costs roughly 41–82 API calls
against daily quotas of 1,000–2,500 where quotas exist, and runs
every four days.

**Figure 2.** Discovery, delivery, and the shared rate-limit budget:
the browse UI (in the user's browser) queries museum search APIs
directly and reports any 429 to the device; the device refreshes
listings, downloads images over the IIIF Image API, and holds the
per-museum cooldown table both halves consult. *(File:
`figure-2-architecture.svg` in this folder; to be rasterized to PNG
≤500 px for submission.)*

## 7. What a 64-byte budget clarifies

p3a stores each artwork as a 64-byte record: a hash, dimensions, a
timestamp, one byte for file type, and 48 bytes for the museum's
image identifier. This budget directly translates into how many
artworks the device can rotate simultaneously (by design, p3a is
limited to playing 64 channels with 4,096 artworks each). There is
something clarifying about a budget this tight: it converts vague API
preferences into sharp, testable requirements, and I offer the
conversions as data points from the small end of the client spectrum:

- **Identifier length is an interoperability property.** Chicago's
  image UUIDs (36 characters), Harvard's URNs (about 25), Micrio's
  short ids (about 8) all fit in 48 bytes; anything longer is
  silently unusable. When Wellcome filters by label instead of id,
  the limit bites visibly (§4.4).
- **Identifier *stability* is one too.** The record survives
  firmware updates, cache rebuilds, and re-listings; an identifier
  that drifts reads as a deletion plus a brand-new artwork — the old
  record is evicted, the image is re-downloaded under the new id,
  and the old file sits unreferenced on the card until age-based
  cleanup collects it.
- **One byte of state is enough — barely.** The file-type byte
  moonlights as the Rijksmuseum resolver's state machine: two
  reserved values mean "not yet resolved" and "given up". When a
  re-listing offers an unresolved record where a resolved one
  already sits, the merge must decline the downgrade or it would
  orphan the very file it already downloaded. Every embedded byte
  ends up doing two jobs.
- **Verbosity is a tax paid in RAM.** The spread between Chicago's
  142 bytes per listed artwork and SMK's ~3,700 is, on a laptop,
  invisible; on this device it is the difference between a 192 KB
  and a 512 KB parse buffer, allocated for the lifetime of a
  refresh. Furthermore, not all RAM in the ESP32-P4 device is equal,
  and at times parts of the computations can only be done using
  internal SRAM (768 KB) as opposed to PSRAM (32 MB).

None of this asks museums to design for microcontrollers. It asks
something cheaper: that the properties small clients depend on —
short stable ids, lean listings, honest capability advertisements —
be recognized as compatibility surface, not implementation detail.

## 8. Discussion: what would help, and what is changing

Distilled from seven integrations, a short generic checklist. Every
item on it already exists in at least one of the seven APIs, so this
is curation not invention:

1. **Inline image identifiers in search results.** The single
   biggest determinant of client cost. One field turns N+1 requests
   into 1.
2. **True offset pagination with totals.** Cursors force sequential
   walks; caps that appear only empirically force adapters to learn
   them empirically.
3. **A filter meaning "displayable image actually included",** that
   composes with other filters and reflects permissions, not just
   file existence.
4. **`Retry-After` on every 429.** Clients that want to comply can
   only comply with what is stated.
5. **Reject loudly.** An HTTP error for a rejected request; a WAF
   that says 200 turns every downstream parser into a liar.
6. **Advertise only what is served.** An `info.json` is a promise,
   and small clients are the ones that believe it.
7. **Document the sharp edges.** Result-window caps, key-quota
   floors, permission gates. Every undocumented limit in this
   article was learned by trial and error.

Two broader observations from the 2026 landscape, briefly. First,
museum APIs are mortal: in the months around this writing, NYPL's
Digital Collections API was retired and several smaller museum APIs
quietly died; any client that embeds institutions must plan for their
endpoints' absence (p3a carries per-source kill switches). Second,
bot defense is becoming a first-class compatibility issue: WAFs and
fingerprint-based challenges now front several otherwise-open
cultural APIs, and a legitimate small client is collateral unless
"identify yourself honestly" remains a sufficient answer. Both trends
raise the value of everything IIIF got right. The Image API's
uniformity is precisely what makes a seven-museum client maintainable
by one person and their language models.

As far as I have been able to determine — searching discuss.iiif.io,
the IIIF Consortium's news archive, this Journal's back catalog, and
the open web — no prior IIIF Image API client implemented in
microcontroller firmware has been described. I would welcome
correction.

## 9. Conclusion

The IIIF Image API kept a promise: seven institutions, seven image
stacks, one line of client code. The discovery layer above it remains
a per-museum negotiation, not because museums ignored a standard, but
because the standard IIIF offers there is scoped for viewers and
harvesters, not for small clients asking contained questions ("what's
in this particular collection? how many? give me items 400–499"). A
$40 microcontroller turns out to be a usefully honest instrument for
measuring that gap: everything implicit becomes a buffer size, a
request count, or a byte.

The frame on my desk was showing a Degas pastel it fetched from
Chicago this morning, sized to the pixel, over an API someone
maintains so that anyone, apparently including a microcontroller, can
ask. Next on its roadmap is the Cleveland Museum of Art, whose API
ranks, by measures developed here, friendlier than some already
shipped. The queue of museums worth thanking is not getting shorter.

## Acknowledgments and disclosure

p3a is open source (Apache 2.0) at https://github.com/fabkury/p3a;
the seven adapters described here are in `components/art_institution/`.
The project — and this article's endpoint probes of 2026-08-07/09 —
were produced with substantial AI assistance: development in the
Cursor IDE (using a variety of models, including Anthropic's) and
with Claude Code (primarily the newest Opus-class model available at
the time). This article was likewise drafted with AI assistance and
fully revised by the author, who verified the technical claims and
takes sole responsibility for the content. The author has no
financial interest in any product, service, or institution discussed;
p3a and Makapix Club are noncommercial open-source projects; the
author has no affiliation with Anthropic, Cursor, or any museum
named.

## About the author

Fabrício Kury (fab@kury.dev, github.com/fabkury) is a biomedical
informatician in New York City. He currently works with Medicare
claims data analytics at Sparx, Inc.

## References

*(CSE style to be finalized at submission; URLs verified 2026-08-08.)*

[1] Kury F. p3a: Wi-Fi pixel art player. https://github.com/fabkury/p3a

[2] Appleby M, Crane T, Sanderson R, Stroop J, Warner S. IIIF Image
API 3.0 (and 2.1). IIIF Consortium. https://iiif.io/api/image/

[3] Rijksmuseum. Collection data & APIs. https://data.rijksmuseum.nl/

[4] Appleby M, Crane T, Sanderson R, Stroop J, Warner S. IIIF
Presentation API 3.0. IIIF Consortium.
https://iiif.io/api/presentation/3.0/

[5] Art Institute of Chicago. API documentation.
https://api.artic.edu/docs/

[6] Victoria and Albert Museum. Collections API.
https://developers.vam.ac.uk/

[7] Wellcome Collection. Catalogue API.
https://developers.wellcomecollection.org/

[8] Statens Museum for Kunst. SMK API. https://api.smk.dk/api/v1/docs/

[9] Harvard Art Museums. API documentation.
https://github.com/harvardartmuseums/api-docs

[10] Smithsonian Institution. Open Access API.
https://edan.si.edu/openaccess/docs/

[11] IIIF Consortium. Change Discovery API 1.0.
https://iiif.io/api/discovery/1.0/
