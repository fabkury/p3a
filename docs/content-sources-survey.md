# Online content sources beyond Makapix, Giphy, Klipy, and the museum seven — survey

- **Status:** Research report / decision support (no code written)
- **Date:** 2026-08-02
- **Method:** Four parallel research passes (GIF/social platforms, pixel-art/retro
  communities, museums/archives/aggregators, ambient/data feeds). Claims were
  verified against official documentation and **live endpoint probes** on
  2026-08-02 — sample images were downloaded and pixel-measured where docs were
  silent. Items that could not be confirmed are marked **UNVERIFIED**.
- **Selection rules (project decisions for this survey):**
  - Rank primarily by **catalog size/freshness** and **implementation effort**.
  - **Device-direct only** — the ESP32 calls the API and downloads the image
    itself; sources needing a companion proxy/transcoder were excluded.
  - All auth models considered; OAuth is flagged as heavy (p3a has none today).
  - Licensing/ToS risk is **flagged, not filtered** — gray sources are included
    with the risk stated.

---

## 1. Executive summary

Roughly 60 sources were assessed. The strongest candidates, by category:

| Pick | Category | Why |
|---|---|---|
| **Cleveland Museum of Art** | Museum #8 | Anonymous JSON with *working* `has_image=1&cc0=1` filters, ~900 px CDN JPEG URL directly in the search response. Simpler than several museums already shipped. 41k CC0 works, growing. |
| **NOAA GOES Earth imagery** | Ambient feed | 678×678 GeoColor JPEG at a **fixed anonymous URL**, refreshed every 10 minutes, US-Gov public domain. No JSON, no key, no parsing — cheaper than anything p3a has built. |
| **Wikimedia Commons** | Mega-catalog | 100M+ files, freshest catalog anywhere, anonymous JSON search, machine-readable per-file licenses. One hard rule: thumbnails now only exist at bucket widths — use **960 px** (720 returns HTTP 400). |
| **Tumblr** | Animated community feed | The only surviving GIF-native art community with an open API. Giphy-style `api_key` query param, fixed 75–1280 px renditions including **real GIFs**, 1,000 req/hr. |
| **Bluesky** | Live community feed | Public AppView is **zero-auth JSON**; posts hand you pre-sized ≤1 MB JPEG CDN URLs. Static images only. |
| **Demozoo** (+ 16colo.rs) | Retro/demoscene | 63k demoscene graphics via anonymous JSON with 3 rendition sizes; 16colo.rs adds ANSI artpacks via a real documented API. Strong on-ethos content. |
| **PICO-8 BBS** | Unique synergy | Carts are tiny `.p8.png` files at stable predictable URLs that feed p3a's built-in PICO-8 player. No API (HTML scrape or curated list); gate to CC-tagged carts. |
| **NASA APOD / xkcd / Dial-a-Moon / NWS radar** | Quick wins | Each is a half-day-class integration with clean or sanctioned licensing. |

**2026 landscape headlines that shaped the ranking:**

1. **The GIF-API market has collapsed onto exactly the two providers p3a
   already has.** Google terminated the Tenor API on 2026-06-30 (all existing
   agreements ended; Discord/WhatsApp/Bluesky migrated, mostly to Klipy).
   Gfycat died in 2023, Imgur stopped issuing API keys ~2024, and Reddit
   403-walled its free `.json` endpoints in May 2026. Giphy + Klipy now *are*
   the surviving market — and animated sources are therefore scarce, which
   raises Tumblr's value (the one strong candidate that adds animated GIFs).
2. **Stock-photo APIs ban this product category.** Unsplash and Pexels both
   have verified ToS clauses prohibiting wallpaper-style applications — an
   ambient photo frame is exactly that. Ruled out despite perfect technical fit.
3. **Museum APIs keep dying.** NYPL's API shut down 2026-08-01 (days before
   this survey), Walters closed 2023, MKG Hamburg's API host no longer
   resolves, Minneapolis's image hosts appeared dead. Aliveness checks
   and per-source kill switches matter. (*Update 2026-08-24: Minneapolis's
   images turned out to live on a different host, `1.api.artsmia.org`,
   with 400/800/full pre-renders — Mia shipped as the ninth museum.*)
4. **Bot-protection is now a first-class device risk.** Several otherwise-good
   sources front their APIs with Cloudflare-style JS challenges or WAFs that
   reject non-browser TLS fingerprints (Finna, Pixilart, Science Museum Group,
   Brooklyn; Reddit enforces via TLS fingerprinting). A compliant or
   browser-ish User-Agent is load-bearing at Wikimedia, LoC, and Gallica. Any
   committed source should get a **real-hardware TLS probe** before design work.

---

## 2. What qualifies a source (constraints recap)

- **Formats:** WebP (anim+static), GIF, PNG, APNG, JPEG, BMP. No AVIF/HEIC/SVG,
  no MP4/WebM — note Mastodon and Reddit convert GIFs to MP4 (`gifv`), which
  silently guts their animated value.
- **Size:** need ≤ ~1–2 MP renditions — either size-parameterized URLs (IIIF,
  `?dimension=`, width buckets) or naturally small images. Full-res museum
  scans without a size param are unusable.
- **JSON only:** the codebase has no XML parser (the documented reason Gallica
  was deferred — `docs/deferred/gallica.md`). SRU/OAI-PMH/RSS-only sources
  inherit that cost.
- **Compact stable IDs:** the 64-byte cache entry (48-char `iiif_key` slot,
  32-char playset identifier) favors sources with short accession numbers or
  numeric IDs. Sources with signed/expiring image URLs (DeviantArt's wixmp
  JWTs, Reddit previews) must store the ID and re-resolve at download time —
  the pattern Klipy already established.
- **Auth ladder:** anonymous > BYOK key (Klipy/HAM/SI pattern) > single-POST
  `client_credentials` OAuth (new but small) > browser/device-flow OAuth
  (heavy, none today).
- **Politeness plumbing exists:** shared rate-limit cooldown, retry-with-backoff,
  SD vault caching, eviction — new sources ride it.

---

## 3. Top picks — deep-dives

### 3.1 Cleveland Museum of Art — the drop-in eighth museum

- **Catalog:** 41,477 works with image + CC0 (live count; up from ~37k at 2019
  launch — still growing).
- **API:** `GET https://openaccess-api.clevelandart.org/api/artworks/?q=…&has_image=1&cc0=1&skip=N&limit=N`
  — **no key**, plain JSON; both filters verified working *server-side* (unlike
  the Met, whose equivalent filters are broken).
- **Images:** no IIIF needed — every result carries stable CDN JPEG URLs
  `https://openaccess-cdn.clevelandart.org/{accession}/{accession}_web.jpg`,
  where `web` ≈ 900 px longest side (sample measured 748×893). `print`
  (3400 px) and `full` (TIFF) exist; ignore them.
- **Rate limits:** none published; no special headers.
- **Licensing:** CC0 metadata *and* images on the flagged subset — the lowest
  risk in the entire survey.
- **Effort: trivial.** Anonymous JSON search + a fixed URL template from the
  accession number (accession numbers are short — fits the 48-char key slot).
  The C adapter is *simpler* than existing museums because there is no IIIF
  URL grammar and no resolver hop. Two independent research passes verified
  the same facts.
- **Verdict: strongest museum add available.**

### 3.2 NOAA GOES Earth imagery — the cheapest integration p3a could ever ship

- **Content:** live full-disk GeoColor (plus CONUS and ~16 band products) from
  GOES-19 (East) and GOES-18 (West). **Updated every 10 minutes** (verified via
  Last-Modified stepping).
- **Access:** no API at all. Fixed anonymous size-named aliases, e.g.
  `https://cdn.star.nesdis.noaa.gov/GOES19/ABI/FD/GEOCOLOR/678x678.jpg`
  (verified 200, ~410 KB) — a near-perfect fit for the 720×720 panel. Also
  `339x339.jpg`, `1808x1808.jpg`; CONUS at `…/CONUS/GEOCOLOR/1250x750.jpg`.
  **Never fetch `latest.jpg`** — it aliases the 10848×10848 original (15.6 MB).
  Timestamped history is available via plain directory listings.
- **Licensing:** US Government work — public domain. No usage policy found
  beyond an "informational purposes" disclaimer; the CDN exists to serve these.
- **Effort: below IIIF — no JSON, no key.** One HTTPS GET per refresh;
  optional `If-Modified-Since`. The real work is a new lightweight **"live
  feed" channel kind**: one image that updates in place rather than a
  catalog to refresh/pick from. That machinery, once built, also serves
  APOD, xkcd, Dial-a-Moon, NWS radar, Himawari, and Wikimedia POTD (§3.7).
- **Companions for an "Earth trilogy":** JMA **Himawari** full disk 601×601
  every 10 min (fixed time-slot filenames, no `latest` alias — needs slot
  arithmetic + 404 fallback; attribution expected) and **EUMETSAT** Meteosat
  via anonymous WMS GetMap (returns exact 720×720 JPEG on request — verify the
  open-data licence text before shipping). NASA **EPIC** is redundant with
  GOES (2-day lag) — skip.
- **Verdict: best effort-to-freshness ratio in the survey.**

### 3.3 Wikimedia Commons — the biggest, freshest catalog

- **Catalog:** 100M+ files, growing daily; the largest and freshest candidate.
- **API:** anonymous action API, verified live:
  `commons.wikimedia.org/w/api.php?action=query&generator=search&gsrsearch=…&gsrnamespace=6&prop=imageinfo&iiprop=url|extmetadata&iiurlwidth=720&format=json`
  returns `thumburl` plus machine-readable license (`LicenseShortName`,
  `Artist`, `UsageTerms`) per file.
- **Images — the critical 2025/26 change:** arbitrary thumbnail widths are
  **gone** for anonymous clients. Allowed buckets:
  20/40/60/120/250/330/500/**960**/1280/1920/3840. A direct `720px-` thumb URL
  returns **HTTP 400**; `iiurlwidth=720` rounds up to the 960 px rendition
  (verified 200). Hardcode 960 and downscale.
- **Rate limits:** 2026 regime — IP-only clients 10 req/min; a compliant
  User-Agent (`p3aBot/1.x (URL; email)`) gets 200 req/min; 429 + Retry-After.
- **Licensing:** overwhelmingly CC-BY/BY-SA/PD with per-file machine-readable
  metadata; surface artist + license for BY works. Risk is **curation**, not
  rights: Commons is unmoderated in breadth and there is no safe-search API —
  channels must be curated search strings or categories.
- **Effort: moderate** — anonymous JSON + fixed-bucket URL builder + custom UA.
  The engineering is easy; picking channel curations that reliably look good
  is the actual work.
- **Verdict: strong; the catalog-scale play.** Start with a few curated
  channels (e.g. Featured pictures / Quality images categories) rather than
  raw search.

### 3.4 Tumblr — the last animated community feed

- **Content:** one of the largest living GIF-art/pixel-art communities; tags
  like `pixel art`, `gif art`, `glitch art` active daily. With Tenor dead,
  this is the **only strong candidate that adds animated GIFs**.
- **API:** `https://api.tumblr.com/v2/…?api_key={consumer key}` — the key is
  used bare, Giphy-style (no OAuth handshake for public reads). Registration
  confirmed open. Relevant reads: tagged-post search (`/v2/tagged?tag=…`) and
  any public blog's photo posts (`/v2/blog/{blog}/posts/photo`). BYOK-able and
  shippable with a shared key. Caveat: the `/tagged` endpoint's current auth
  level was not re-confirmed on the docs page (historically api_key) — spike
  first.
- **Images:** `alt_sizes` arrays at fixed 1280/500/400/250/100/75 px in JPG,
  PNG, or **real GIF** (Tumblr's CDN serves actual `.gif` files — unlike
  Mastodon/Reddit's MP4 conversion). Direct stable `64.media.tumblr.com` URLs.
  Trap: GIF `alt_sizes` below the original are sometimes static thumbnails —
  pick the largest GIF rendition and enforce a byte cap.
- **Rate limits:** 1,000/hr + 5,000/day per key; 300/min per IP. Ample.
- **Licensing/ToS:** API License permits in-app display with attribution;
  long-term SD caching is mild gray (agreement expects deletions respected —
  p3a's eviction churn approximates this). Post-2018 tag feeds are mostly SFW
  but tag hygiene filtering is advisable. Structural risk: Automattic froze
  Tumblr feature development in 2025 — the API is alive but in maintenance
  mode.
- **Effort: trivial-to-moderate** — closest match to the existing Giphy
  pattern. Extra work: legacy "photo" vs NPF post-format parsing, filtering
  non-photo posts, tag-list configuration UI.
- **Verdict: strong — the animated-content play.**

### 3.5 Bluesky — the zero-auth live art feed

- **Content:** fast-growing network with a lively pixel-art scene (`#pixelart`
  hashtag, dedicated custom feeds). Effectively unbounded and user-curatable —
  any custom feed URI can become a channel.
- **API:** `https://public.api.bsky.app/xrpc/…` — official docs confirm the
  public AppView **requires no authentication** and recommends exactly this
  host. `app.bsky.feed.searchPosts?q=%23pixelart`,
  `app.bsky.feed.getFeed?feed=at://…` (custom feeds). Clean JSON. Simpler than
  Giphy — no key management at all.
- **Images:** the post JSON hands you CDN rendition URLs
  (`embed.images[].thumb` ≈ 1000 px class, `.fullsize`); all uploads are
  re-encoded JPEG with a ~1 MB ceiling — naturally inside the RAM budget.
  **Static JPEG only** (JPEG re-encode also softens pixel art); GIFs in posts
  are external embeds (now Klipy URLs — extractable in principle, fiddly).
- **Rate limits:** "generous" for the public AppView; no practical concern.
- **Licensing:** public-by-design content; reading public feeds via the public
  AppView is the sanctioned pattern. Artist copyright posture is the same as
  Makapix/museum channels. Honor the `labels` moderation field.
- **Effort: trivial.** Work is feed/hashtag selection UX and label filtering.
- **Verdict: strong — cheapest live-community integration available.**

### 3.6 Demozoo + 16colo.rs — the retro pair

**Demozoo** (demozoo.org/api/v1):
- 63,467 productions under `supertype=graphics` (verified), continuously
  updated; C64/Amiga/PC compo graphics = premium retro content with natural
  channel granularity (platform/type filters → "C64 graphics", "Amiga pixel
  art").
- Anonymous JSON; list endpoints paginate via `next` URLs; **screenshots
  require one extra request per production** (`/productions/{id}/` →
  `screenshots[]` with `original_url` / `standard_url` 400 px /
  `thumbnail_url` 200 px on media.demozoo.org; verified). Many entries have
  empty screenshots — skip. `standard` is small for the panel; `original` is
  usually ≤1 MP for retro platforms — prefer it with a byte cap.
- No documented limits; daily DB dumps exist, so polite polling expected.
  Artwork rights formally remain with sceners — displaying compo graphics is
  customary in the scene but formally gray.
- **Effort: low-moderate** (2-hop, like the Rijks resolver).

**16colo.rs** (ANSI/ASCII artpack archive):
- Artpacks 1990→2026, still receiving new packs (verified). Real documented
  anonymous JSON API (`api.16colo.rs/v1/…`: packs, years, groups, artists,
  latest releases; pagination 1–500). Pre-rendered PNGs per piece at
  `16colo.rs/pack/{pack}/tn/{file}.png` (+ `/x1/`, `/x2/`).
- **Geometry caveat:** ANSI renders are extremely tall (80 cols × hundreds of
  rows — multi-MP at x1). Use `tn/` renditions or the API's `dimensions` data
  to filter; accept crop/letterbox. Note the bare API root returned 403 to a
  non-browser fetcher while documented endpoints worked — include in the
  hardware TLS probe.
- Artpacks were made for free BBS distribution and the archive redistributes
  them openly (FTP/RSYNC mirrors); low-to-moderate gray with a strong
  community norm of redistribution.
- **Effort: low** (museum pattern minus IIIF sizing; the work is the
  dimension/aspect filter).

**Verdict: both strong; Demozoo first** (bigger catalog, better filtering,
multi-size renditions). Pouet.net's API also works but is strictly dominated
by Demozoo except for its "top of the month" curation angle.

### 3.7 Quick wins (each ≈ half a day on the "live feed" channel kind)

| Source | Access | Image | Licensing | Notes |
|---|---|---|---|---|
| **NASA APOD** | api.nasa.gov key (free, Giphy-style); `?thumbs=true` | `url` field ≈ 960 px (verified today); ignore `hdurl` | Sanctioned API; photos often photographer-copyrighted — show credit | Handle `media_type: video/other` days (thumbnail or skip). 1,000 req/hr keyed. `count=N` random = instant shuffle-archive of ~11,300 entries |
| **xkcd** | `xkcd.com/info.0.json` + `/{n}/info.0.json`, no auth | PNG, modest sizes; rare huge/interactive comics are acceptable misses | **CC BY-NC 2.5 verified** — explicitly permits noncommercial reposting with attribution | 3,279 comics and counting; random-archive channel is trivial |
| **NASA Dial-a-Moon** | `svs.gsfc.nasa.gov/api/dialamoon/{ISO time}`, anonymous JSON | **730×730 JPEG** — almost exactly the panel | NASA SVS public domain | Hourly-accurate moon phase; charming single-subject channel |
| **NWS radar loops** | `radar.weather.gov/ridge/standard/{STATION}_loop.gif`, fixed URL | **Animated GIF 600×550** (verified); CONUS 600×392 | US-Gov public domain | A live *animated* radar loop — p3a plays it natively. US-centric |
| **Wikimedia POTD** | `api.wikimedia.org/feed/v1/wikipedia/en/featured/YYYY/MM/DD`, anonymous | feed hands a 960 px thumb URL | Free-licensed with per-image license + artist in the JSON | One probe saw the `image` key, another didn't — confirm shape before relying on it. Response ~100+ KB; parser must skip to the `image` key |
| **Bing Image of the Day** | `bing.com/HPImageArchive.aspx?format=js…`, anonymous, **unofficial** | suffix-selectable incl. exact `w=720&h=720` (verified) | Gray by definition — no ToS sanctions third-party use; decade-stable in practice | Ship only if the project tolerates an unofficial endpoint |

### 3.8 Special mention: Lexaloffle PICO-8 BBS

Uniquely valuable because carts feed p3a's **built-in PICO-8 player**, not just
the image pipeline:

- Carts are tiny (~10–30 KB) `.p8.png` files at **stable predictable URLs**
  (`lexaloffle.com/bbs/cposts/{2-char}/{slug}.p8.png`, verified), with PNG
  thumbnails at `bbs/thumbs/pico8_{slug}.png`. Thousands of carts, new
  releases daily.
- **No API.** The cart lister returns HTML only; SPLORE's protocol is
  undocumented (community-reverse-engineered), and Lexaloffle has made no
  statement on third-party use. Integration means either HTML-scraping the
  lister (brittle, tolerance unknown) or — the recommended v1 — **shipping a
  curated cart list** (a hand-vetted JSON of CC-tagged carts baked into the
  webui/LittleFS image, refreshed at release time). The BBS has a
  per-cart opt-in CC4-BY-NC-SA tag and a "creative commons" filter tab;
  restricting to CC carts keeps both licensing and optics clean.
- **Effort:** moderate for the curated-list v1 (download+cache is trivial;
  the new part is routing a downloaded cart into the PICO-8 player);
  high/brittle for live scraping.
- **Verdict: strong-if-curated** — no other source exercises the PICO-8
  player.

---

## 4. Full survey tables

Verdicts: **S** strong · **P** possible · **W** weak · **✕** dead/infeasible.

### 4.1 Museums, libraries, archives, aggregators

| Source | V | Open-image catalog | Auth | Image delivery | Notes / risk |
|---|---|---|---|---|---|
| Cleveland (§3.1) | S | 41k CC0, growing | none | `{acc}_web.jpg` ~900 px | Drop-in; filters work server-side |
| Wikimedia Commons (§3.3) | S | 100M+, freshest | none | 960 px bucket only | UA-dependent rate tier; curation is the work |
| DigitaltMuseum (api.dimu.org) | S | ~3.4M openly licensed (live facets) | key **by email** to kulturit.no | `ems.dimu.org/image/{id}?dimension=800x800` (verified) | Nordic aggregator, hundreds of museums; Solr syntax; **Content-Type header lies** (says WebP, bytes were JPEG) — sniff magic bytes; docs Norwegian-only |
| Openverse | S | ~842M CC/PD | none (anon) | API-proxied 600 px WebP thumbs | **20/min + 200/day anon cap** (measured); OAuth2 client-credentials tier optional; per-result ready-made attribution string; provider link-rot mitigated by thumbs |
| Internet Archive | S | 5.7M image items | none | **IIIF Image API 3 level2 verified** | Needs identifier→filename hop (manifest/metadata); curate by collection allowlist (Met mirror alone 141k); uptime history is the risk |
| Library of Congress | S | ~1.6M `online-format:image` | none | ~1024 px `v.jpg` **inline in search JSON** (zero-hop) | 20 req/min JSON limit (1-hr block!); browser-ish UA needed; rights = curated sets. **Already deeply investigated in-repo** — see §7 |
| Nat. Library of Norway (api.nb.no) | S | 667k PD (server-side filter) | none | IIIF templates embedded in responses | Less work than several shipped museums; skews historical photos; ToS UNVERIFIED |
| Te Papa (NZ) | S | ≤300k images | self-serve `x-api-key` (BYOK) | real IIIF 2 level2; image host anonymous (verified) | Per-item rights filter needed (NC variants exist) |
| Finnish National Gallery | S | ~13k pure CC0 | self-serve key (BYOK) | `ImageDownloadLinkForSupportedWidths` map | POST-body search; small catalog is the only knock |
| Yale LUX | S | 17M records | none | IIIF via 2-hop resolver (Rijks-clone) | Tiny payloads; **no verified open-access/rights query filter yet** — the open item |
| The Met | P | ~406k CC0 (UNVERIFIED) | none | **no size params**; `primaryImageSmall` ≈ 600 px | `isPublicDomain`/`hasImages` search filters demonstrably broken (2 req/artwork workaround); 600 px ceiling = 1.2× upscale |
| Europeana | P | millions open | self-serve key (since 2025) | **Thumbnail API caps at 400 px**; provider URLs unusable at scale | Perfect API shape, held back solely by the 400→720 upscale. Validates the design doc's deferral |
| Cooper Hewitt | P | ~215k | anonymous GraphQL (keys optional) | S3 JPEGs, large = 1024 px shortest | New GraphQL API is alive (old REST dead); **overlaps existing `si` source** — dedupe check |
| BnF Gallica | P | huge PD corpus | none | IIIF verified end-to-end (`native.jpg`) | **XML-only SRU search** — the known blocker; deferral stands (§7). Mandatory "Source gallica.bnf.fr / BnF" credit |
| Paris Musées | P | ~260k CC0 HD | free token (BYOK) | **UNVERIFIED whether any sub-3000 px derivative exists** — make-or-break | GraphQL; docs behind bot wall; spike with a real token first |
| Getty | P | ~88k CC0 | none | IIIF 3 level2 with WebP (verified) | **No search API** — SPARQL/JSON-LD only, 2–3 hops; heavy |
| National Gallery of Art (US) | P | ~60k open | n/a | IIIF 2.1 works (`!720,720` verified) | **No search API — CSV dumps only**; needs a shipped seed list; foreign to the pattern |
| Barnes Foundation | P | ~2k+ (static) | none | CloudFront JPEG `_b` = 1024 px | Undocumented ES endpoints (work today, zero stability promise) |
| Auckland Museum | P | 38k no-known-restrictions | none | **440×440 or original, nothing between** | Documented 1,000 req/day; honor `isTaonga`/`isSensitive` flags |
| Finna.fi | P* | millions | n/a | n/a | **Cloudflare JS challenge on every endpoint tested** — disqualifying for ESP32 unless a re-test from another network clears the API host |
| Biodiversity Heritage Library | W | large but text-heavy | free key | full-res or tiny thumb only — no mid rendition | Illustration discovery requires per-item page walking; poor fit |
| DPLA | W | large | key | thumbnail-only, small, no size params | Institutional future uncertain post-2025; validates deferral |
| NYPL | ✕ | — | — | — | **API shut down 2026-08-01**, no replacement planned |
| Walters / MKG Hamburg / Nationalmuseum SE / MoMA | ✕ | — | — | — | APIs closed, dead hosts, or dumps-only without images |
| Minneapolis Inst. of Art | S | ~34.5k PD w/ image | none | `1.api.artsmia.org/{400\|800\|full}/{id}.jpg` (~50 KB @ 800) | **SHIPPED 2026-08-24 as `mia` (ninth museum)** — the 2026-08-02 probe hit a dead legacy host; the S3 bucket host works. ES passthrough search + live aggregations |
| Brooklyn Museum | ✕ | — | — | — | Aggressive bot protection blocks every probe |
| Science Museum Group | ✕ | — | — | — | CC **BY-NC**-SA images + WAF 403s non-browser UAs — two independent red flags |
| Flickr / Flickr Commons | ✕ | — | **new keys require paid Flickr Pro** (corroborated; wording UNVERIFIED) | — | Its CC content is reachable free via Openverse instead |
| National Gallery (London) | ✕ | — | — | — | No open API; beta API "should not be used"; NC-ND |

### 4.2 Pixel art, retro, demoscene

| Source | V | Notes |
|---|---|---|
| PICO-8 BBS (§3.8) | S | Unique PICO-8-player synergy; curated-list v1 recommended |
| 16colo.rs (§3.6) | S | Real anonymous JSON API + PNGs; tall-image geometry |
| Demozoo (§3.6) | S | 63k graphics prods, anonymous JSON, 3 renditions |
| Mastodon `#pixelart` | P | Keyless JSON tag timelines (`/api/v1/timelines/tag/…?only_media=true`) with direct image URLs + `sensitive` flag; **per-instance policy varies** (mastodon.social worked anonymously; mastodon.art rejected); GIFs become MP4 — static only. Strictly dominated by Bluesky on uniformity; cheap follow-on since Pixelfed speaks the same API |
| Internet Archive (retro collections) | P | The IIIF plumbing is a gift; blocked on finding curated, stable collections (`subject:"pixel art"` = only 907 mixed-quality items) |
| Pouet.net | P | Works, but Demozoo dominates; "top of the month" is its one angle |
| DeviantArt | P | **`client_credentials` covers public browse** (verified in auth docs) — cheapest possible OAuth (one token POST + 1-hr refresh). But: wixmp image URLs are JWT-signed and expire (re-resolve at download), docs mandate gzip support, platform in maintenance mode |
| OpenGameArt | W | Ideal CC licensing, wrong shape (spritesheets/ZIPs) + RSS-only (XML) |
| Lospec | W | **No gallery API** (palettes only); rights-reserved art; small team — a permission/partnership email is the respectable route |
| PixelJoint | W | Deepest classic gallery (110k pieces), **no API**, community hostile to rehosting — permission-first only |
| Pixilart | W | Undocumented API behind bot protection (403 to probes) |
| itch.io / Newgrounds | W | APIs exist but exclude content browsing by design |
| pixiv | ✕ | No official API; community access = intercepted mobile OAuth + ToS violation + NSFW risk — untouchable for a shipped device |
| Dwitter / pixelart.club | ✕ | JS-canvas content / apparently dead |

### 4.3 GIF, sticker, meme, social

| Source | V | Notes |
|---|---|---|
| Tumblr (§3.4) | S | The animated-content play |
| Bluesky (§3.5) | S | The zero-auth play; static only |
| Mastodon | P | See §4.2 |
| DeviantArt | P | See §4.2 |
| Stipop | P | 150k+ curated stickers (PNG/GIF), instant free key — a Klipy-pattern clone; smaller catalog, startup-longevity risk |
| Imgflip | W-P | Free no-auth `get_memes` = ~100 static templates; fun novelty, no depth without paying |
| Lemmy | P-lite | Anonymous JSON Reddit-alternative (`/api/v3/post/list?community_name=…`); small communities; endpoint details not verified against official docs |
| Reddit | W | Free `.json` 403-walled May 2026 (TLS-fingerprint enforced); OAuth + manual app approval + MP4-heavy content |
| Imgur | W | New API key registration closed since ~2024; platform declining (UK-blocked 2025) |
| Tenor | ✕ | **API terminated 2026-06-30** |
| Gfycat | ✕ | Dead since 2023 |
| Pinterest / 9GAG / iFunny | ✕ | No public browse API / no API at all |
| Signal sticker packs | ✕ | AES-encrypted blobs; decryption on-device is heavy and clearly outside intended use |

Watch-list: a June 2026 report claims Giphy's free API tier has been tightened
— not verified in depth; worth monitoring for the *existing* integration.

### 4.4 Ambient, photo, data feeds

| Source | V | Notes |
|---|---|---|
| NOAA GOES (§3.2) | S | The standout |
| Wikimedia POTD (§3.7) | S | Museum-grade daily curation, zero auth |
| NASA APOD (§3.7) | S | Highest content appeal per unit effort among keyed sources |
| xkcd (§3.7) | S | CC BY-NC explicitly sanctions this use |
| Bing IOTD (§3.7) | P-S | Perfect fit, unofficial endpoint |
| NASA Image & Video Library | P | Anonymous search over hundreds of thousands of images with `~thumb/~medium/~large/~orig` renditions — themed channels ("nebula", "apollo"); archive not feed; limits unverified |
| JMA Himawari | P | 601×601 every 10 min; slot arithmetic + 404 fallback; attribution |
| NWS radar loops (§3.7) | P | Animated GIF, US-centric, zero cost |
| NASA EPIC | P | 1080×1080 but ~2-day lag — redundant with GOES |
| Dial-a-Moon (§3.7) | P | 730×730, PD, charming |
| EUMETSAT WMS | P | Exact 720×720 on request; licence text unverified |
| Openverse | P | See §4.1 |
| TheCatAPI / Dog CEO | P | Verified working (key optional / none); Giphy-shaped fun channels; ToS not deep-read |
| Pixabay | P-low | Docs 403'd (UNVERIFIED); notably its ToS *mandates* downloading to own storage — caching is required, not forbidden |
| Unsplash / Pexels | ✕ | **Verified ToS prohibition on wallpaper-style apps** (Pexels names wallpaper apps explicitly) + hotlink/tracking requirements incompatible with SD caching |
| NASA Mars Rover Photos | ✕ | **All endpoints 404 today** from two clients; no retirement notice found — re-check later |
| Lorem Picsum | dev-only | Useful test stub for photo-channel plumbing; not a product source |
| GoComics / Comics Kingdom / webcomic RSS | W | Licensed syndicates without APIs; per-comic RSS+HTML parsing (XML) for indies — xkcd is the one comic with a sanctioned JSON API |

---

## 5. Cross-cutting engineering findings

1. **User-Agent is load-bearing.** Wikimedia's anonymous tier is 10 req/min
   vs 200 req/min with a compliant UA; LoC 403s non-browser fetchers on some
   surfaces; Gallica has the same quirk (already documented in-repo). Making
   the UA string a per-source property of the adapter framework is cheap and
   future-proofing.
2. **WAF/bot protection must be probed from real hardware.** Cloudflare-style
   JS challenges or TLS-fingerprint rejection appeared at Finna (all
   endpoints), Pixilart, Science Museum Group, Brooklyn, Reddit, and even the
   bare 16colo.rs API root. An mbedTLS handshake from an ESP32 is exactly the
   fingerprint these systems flag. Any committed source should get an
   on-device HTTPS probe before design work starts.
3. **Don't trust Content-Type.** DigitaltMuseum's image endpoint declared
   `image/webp` while serving JPEG bytes. Sniff magic bytes (the decoder layer
   effectively does, but download-side format checks shouldn't rely on
   headers).
4. **Fixed-bucket thumbnail regimes are spreading.** Wikimedia now hard-rejects
   non-bucket widths (960 px is the p3a bucket of choice). Expect more
   providers to do this; URL builders should treat the size as per-source
   config, not a universal `!720,720`.
5. **Expiring/signed image URLs require the Klipy re-resolve pattern.**
   DeviantArt (wixmp JWTs), Reddit previews, and any CDN-signed source must
   store a compact ID and re-resolve at download time — never persist the URL.
6. **Animated content is scarce now.** Tenor is dead; Mastodon and Reddit
   transcode GIF→MP4. New animated sources reduce to: Tumblr (real GIFs),
   NWS radar loops (animated GIF), Stipop (animated stickers), PICO-8 carts
   (via the player). Weight accordingly if animation matters to the frame.
7. **The no-XML-parser constraint held up well.** Every strong candidate is
   JSON or raw-image; the only XML casualties (Gallica, OpenGameArt, webcomic
   RSS) were already marginal for other reasons.
8. **A "live feed" channel kind is the cheapest new capability with the widest
   payoff.** One fixed-URL image, refetched on a cadence with
   If-Modified-Since, no catalog/vault semantics: it unlocks GOES, Himawari,
   EUMETSAT, APOD, xkcd, Dial-a-Moon, NWS radar, POTD, and Bing IOTD — nine
   sources on one small mechanism.

## 6. Licensing posture summary

- **Cleanest (public domain / CC0):** GOES, NWS, NASA (APOD images sometimes
  photographer-copyrighted — show credit), Dial-a-Moon, Cleveland, Finnish
  National Gallery, NB Norway's PD filter, Openverse (per-result license +
  attribution string).
- **Sanctioned with attribution:** xkcd (CC BY-NC), Wikimedia (per-file
  CC/BY-SA metadata), Tumblr/Bluesky/Mastodon API display use, Te Papa,
  DigitaltMuseum (attribution = owning museum).
- **Community-norm gray:** demoscene screenshots (Demozoo/Pouet), 16colo.rs
  artpacks, PICO-8 carts (mitigate: CC-tagged subset only). Formal rights sit
  with artists; redistribution is the long-standing community practice.
- **Contractually hostile — avoid:** Unsplash/Pexels (wallpaper-app
  prohibitions), pixiv (ToS + intercepted-credential access), scraping
  rights-reserved galleries (PixelJoint, Lospec, Pixilart) — for those,
  a permission-first partnership email is the only respectable route, and
  Lospec's small team might actually say yes.

## 7. Relation to prior in-repo research

- **Library of Congress** — `docs/art-institutions/loc-investigation/REPORT.md`
  (2026-05-13) already established the integration design (48-char `iiif_key`
  prefix filtering, facet syntax quirks, curated-collection axis). This
  survey's fresh probe adds one improvement: the ~1024 px `v.jpg` derivative
  inline in search JSON allows a **zero-hop, non-IIIF** variant that sidesteps
  the low IIIF-surface-rate problem entirely — and confirms the now-documented
  20 req/min JSON limit. LoC remains the most shovel-ready large add.
- **Gallica** — `docs/deferred/gallica.md` deferred it over the XML/SRU
  parser. This survey re-verified the IIIF image path works end-to-end; the
  blocker is unchanged. Deferral stands.
- **Europeana / DPLA** — deferred as aggregators in
  `docs/art-institutions/finalized-design.md` §1. The survey adds the concrete
  reasons the deferral was right: Europeana's thumbnail ceiling is 400 px;
  DPLA is thumbnail-only with an uncertain institutional future. The
  aggregators that *do* clear the bar are DigitaltMuseum, Openverse, and
  Internet Archive.

## 8. Suggested next steps (by appetite, not commitment)

1. **Half-day class:** build the "live feed" channel kind with GOES as the
   proof, then xkcd/APOD/Dial-a-Moon nearly free behind it.
2. **Museum #8:** Cleveland — trivial, CC0, and two independent verification
   passes agree. LoC is the shovel-ready large follow-up.
3. **One community feed:** Tumblr if animation is the priority, Bluesky if
   simplicity is. Both start with an on-device TLS/UA probe.
4. **Retro track:** Demozoo first; 16colo.rs after solving tall-image
   handling; PICO-8 curated cart list when someone wants to exercise the
   PICO-8 player.
5. **Before any design work on a chosen source:** run the real-hardware HTTPS
   probe (TLS fingerprint / WAF / UA behavior) — §5.2 — since that is the one
   failure mode desktop research cannot rule out.
