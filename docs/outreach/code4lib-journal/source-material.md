# Source material — Code4Lib Journal article

Consolidated 2026-08-07 from: `docs/art-institutions/finalized-design.md`,
a full code sweep of `components/art_institution/` (+ decode/HTTP paths),
`docs/content-sources-survey.md`, the parked listserv draft
(`../code4lib-post.md`), and the Journal's guidelines. This file is input
for the article, not the article. file:line references are as of 2026-08-07.

## 1. The device, in one paragraph

p3a: open-hardware desktop art frame. Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
(~$48 retail): ESP32-P4 MCU (silicon rev v1.0), 720×720 4" IPS touchscreen,
32 MB PSRAM, hardware JPEG codec, SD card, ESP32-C6 Wi-Fi co-processor over
SDIO. Firmware in C on ESP-IDF v5.5 / FreeRTOS. Apache 2.0,
github.com/fabkury/p3a. Plays WebP/GIF/PNG/APNG/JPEG/BMP from Giphy, Klipy,
Makapix Club, local SD — and seven museums over IIIF ("Museums" feature,
codebase `art_institution`).

## 2. The seven museums

| id | Museum | IIIF prefix | Auth | Resolution quirk |
|---|---|---|---|---|
| `artic` | Art Institute of Chicago | `www.artic.edu/iiif/2/` | none (AIC-User-Agent header) | image_id inline |
| `rijks` | Rijksmuseum | `iiif.micr.io/` | none | 3-hop Linked Art walk |
| `vam` | Victoria and Albert Museum | `framemark.vam.ac.uk/collections/` | none | `_primaryImageId` inline |
| `wellcome` | Wellcome Collection | `iiif.wellcomecollection.org/image/` | none | vid from `locations[]` where `locationType.id=="iiif-image"` |
| `smk` | Statens Museum for Kunst | `iip.smk.dk/iiif/jp2/` | none | JP2 filename after last `/iiif/jp2/` |
| `ham` | Harvard Art Museums | `nrs.harvard.edu/` (URN resolver!) | BYOK `apikey` | NRS URN → 303 → ids.lib.harvard.edu |
| `si` | Smithsonian | `ids.si.edu/ids/iiif/` | BYOK api.data.gov key | idsId inline; F5 WAF needs real User-Agent |

All requests: `{prefix}{key}/full/!720,720/0/default.jpg` — IIIF Image API
v2, bang-size, JPEG. One URL template covers all seven; everything else
about them differs.

Adapter sizes (lines of C): artic 839, rijksmuseum 645, smithsonian 526,
ham 477, wellcome 438, smk 406, vam 399, common 59. Good proxy for "how
far each museum's API is from the happy path."

## 3. Architecture facts

- Per-museum adapter = compile-time struct row: `id`, `display`, enum,
  `refresh_channel()`, `build_iiif_url()`, optional `resolve_entry()`
  (only Rijks), optional `api_key_missing()` (only HAM/SI)
  (`art_institution.h:43-106`, `art_institution.c:26-90`). Enum
  append-only: ordinals index the rate-limit table across upgrades.
- Browser/device split: browse-time term enumeration + preview runs in
  the user's browser (JS adapters mirror the C table 1:1,
  `webui/museum/index.js`); firmware owns persistence, refresh,
  download, playback. Mirrors how the browser already talks to
  makapix.club directly.
- Channel = (museum, axis, term) triple, e.g. `artic:departments` +
  `PC-4`, stored in the playset binary format (33-byte identifier slot —
  this small slot has real consequences, see Wellcome below).
- 64-byte packed cache entry `institution_channel_entry_t`, sharing the
  slot layout with Makapix/Giphy entries; field order keeps natural
  alignment under `__attribute__((packed))`; `_Static_assert(sizeof==64)`.
  48-byte `iiif_key` — entries with longer keys are silently skipped.
  `extension` byte doubles as state machine: 0-3 = format, 0xFF =
  unresolved (Rijks), 0xFE = tombstone.
- Vault: `/sdcard/p3a/museum/{museum_id}/{d0}/{d1}/{key}.{ext}`,
  FNV-1a-64 sharding, 64×64 dirs. Per-museum, shared across channels:
  a painting in four AIC facets is stored once (avoids 2-5× duplicate
  bandwidth). Dedup key: salted DJB2 of `"{museum}:{iiif_key}"` (salt
  spells "AINS" in ASCII).
- No new GC: orphan eviction deliberately does NOT unlink museum vault
  files (cross-channel overlap is common, unlike Giphy); age-based
  storage eviction reclaims them under disk pressure.
- Settings: `ai_refresh_sec` default 4 days (1-8 d), `ai_cache_size`
  default 1024 (32-4096).

## 4. Per-museum war stories (the article's meat)

**AIC — the 1000-record wall and Elasticsearch bisection.** Public
callers hit `from + size ≤ 1000` on `/artworks/search` (docs imply
10 000). Also: HTTP 403 on `?page=N` past ~page 10 for very large facets.
Fix #1: treat 403/401 after ≥1 merged page as partial success (skip
orphan eviction, persist `last_refresh`, return OK). Fix #2 (the
standout): for offset playback the adapter recursively bisects the
artwork-ID space with POST `bool`+`range` Elasticsearch DSL until every
bucket holds ≤1000 records — max 64 buckets, ID space to 1 M (IDs
cluster <330 k as of 2026), count-only probes with `"size":0`
(`artic.c:461-839`). An embedded device doing adaptive query planning
against a museum's search engine.

**Rijksmuseum — Linked Art on a microcontroller.** No inline image URL.
Chain per artwork: HMO → `shows[]` → VisualItem → `digitally_shown_by[]`
→ DigitalObject → `access_point[]` → micrio URL; each hop a JSON-LD GET.
`id.rijksmuseum.nl` 303s to `data.rijksmuseum.nl` and ESP-IDF's HTTP
client does not auto-follow under the open/fetch_headers/read pattern —
on IDF 5.5.2 `disable_auto_redirect` doesn't stop `fetch_headers` from
consuming `Location` before user code reads it; workaround: capture it
in an `HTTP_EVENT_ON_HEADER` handler (`finalized-design.md` §15.2).
Refresh stores unresolved HMO ids (`extension=0xFF`); the download loop
resolves lazily, one entry per pass (a walk costs a few hundred ms —
draining all would block downloads); 3 consecutive failures → tombstone
(0xFE); a later refresh re-adds with fresh budget. Lock discipline:
snapshot under lock, drop lock for network, re-find by post_id for
writeback (a concurrent refresh may have moved/evicted the entry).
Merge resurrection guard: an incoming unresolved entry never overwrites
a resolved one (would orphan the on-disk file), but tombstones ARE
replaced to grant a fresh budget. post_id hashes the HMO URL (stable),
not the micrio id (unknown until resolved). After resolution the key
stores `"{micrio}|{hmo_int}"` because there is no public reverse micrio→HMO
mapping and the title view needs the HMO. Browse sets come from OAI-PMH
with no CORS headers → firmware bakes `rijks-sets.json` (~193 sets) into
its own flash and serves it to the browser itself.

**Smithsonian — the WAF that says 200.** api.si.edu + ids.si.edu sit
behind an F5 BIG-IP ASM WAF that rejects default/empty User-Agent with
HTTP **200** + "Request Rejected" HTML — symptom is a JSON parse error,
not an HTTP error. Fix: `User-Agent: p3a/{version} (pub@kury.dev)`.
Also: `usage:CC0` is not Solr-indexed (ANDing it → 0 hits); `media` is
sometimes object, sometimes array; SAAM pages exceeded 512 KB at
rows=100 → rows=50 + a 1 MB response buffer; DEMO_KEY (~30 req/h) can't
survive a real refresh → forced free registration; registered quota
1000 req/h vs ~82 calls for a full 4096-entry refresh.

**Harvard — permission gates and a URN resolver as an image server.**
`q=imagepermissionlevel:0` is mandatory: without it ~half of `hasimage=1`
records have no displayable URL. The "IIIF base" is nrs.harvard.edu — a
URN resolver, not an image server; IIIF path syntax is appended to the
URN and a 303 lands on ids.lib.harvard.edu (handled by the generic
redirect shim, no resolver walk needed). 2500 req/day BYOK quota vs ~41
calls per full refresh. Terms >32 chars dropped at enumeration (33-byte
playset slot) — affects ~22 % of periods, ~27 % of galleries.

**Wellcome — filtering by label because there's no id.** For
genres/subjects/contributors the filter value IS the label string
(no stable id exposed) → labels >32 chars can't become channels.

**SMK — info.json that lies.** Backend advertises WebP in info.json but
returns 400 on `.webp` requests (kept JPEG). `fields` param returns
empty items when used → responses can't be trimmed request-side;
metadata-rich pages ~4 KB/record → 512 KB buffer, rows=50.

**V&A — trust but verify.** `images_exist=1` requested, missing ids
still skipped defensively ("the V&A API isn't perfectly consistent").
Venue facet returns count=0 when combined with `images_exist=1` →
enumerate without the flag, re-probe each term with it. Offset capped
at 9900 (page×size ≤ 10 000 cap).

## 5. Rate limiting (the "polite client" story)

- Per-museum cooldown table: `int64_t[7]` of absolute esp_timer µs —
  lock-free reads; RAM-only. Only-extend semantics (a longer deadline
  wins). Default 60 s (sized to AIC's 60 req/min window); `Retry-After`
  honored, parsed centrally, capped 3600 s.
- Checked at four layers: refresh gate, adapter entry, download entry,
  resolver.
- Browser↔device budget sharing: AIC's cap is per-IP, and browser +
  device share the IP. Browse-modal 429s are POSTed to the device
  (`/api/museum/rate-limits/report-429`), device cooldowns are polled by
  the UI before expensive probes. Unknown museum ids silently ignored
  (don't leak the museum list to opportunistic POSTs).
- Politeness delay 150-200 ms between pages; AIC browse probes capped at
  concurrency 6; process-wide max 2 concurrent TLS sessions.
- Being a good API citizen as a design feature: identifying User-Agent
  with contact email on every request; refresh default every 4 days.
- Honest gap (lessons-learned): the designed "≥3 connection failures in
  30 s → defensive cooldown" trigger was never implemented.

## 6. Embedded constraints worth explaining to a library-tech audience

- Response buffers sized per museum: AIC/Rijks 192 KB, V&A/Wellcome
  256 KB, SMK 512 KB, SI 1 MB — all PSRAM-first. cJSON allocations
  redirected to PSRAM: museum JSON (up to 1 MB, many small nodes) would
  otherwise fragment the DMA-capable internal pool the Wi-Fi SDIO path
  needs, panicking the chip.
- Images stream to SD in 32 KB chunks, temp-then-rename, 16 MiB cap —
  never fully buffered.
- JPEG decode: P4's hardware JPEG engine first (100 ms timeout, RGB888,
  16-px MCU padding); libjpeg-turbo software fallback for progressive
  JPEGs + files tripping IDF's SOF gate — with jerror `exit()` replaced
  by setjmp/longjmp (a corrupt JPEG must not reboot the device) and a
  progress monitor yielding every 200 ms to dodge the 15 s watchdog.
  So "hardware JPEG codec" needs the caveat: HW first, SW fallback.
- IDF bug workaround: `jpeg_release_codec_handle` NULL-derefs on
  partially-constructed decoder engines → linker `--wrap` shim
  (`idf_jpeg_release_null_fix.c`).
- No `info.json` negotiation at all — `!720,720` was universally
  supported across all seven servers; panel is 720×720 so requests are
  exactly panel-sized. (Deliberate simplification; deferred forever
  unless needed.)
- 720 hardcoded at the call site (`download_manager.c:530`), not in
  adapters.

## 7. Numbers table (for the article's fact box)

- Board: ~$48 retail. MCU: ESP32-P4 dual-core RISC-V @ 400 MHz. RAM:
  32 MB PSRAM + ~768 KB internal. Display: 720×720 4" IPS.
- Firmware C, ESP-IDF v5.5. Component: ~3700 lines of C across 7
  adapters + shared core.
- Cache: default 1024 entries/channel (max 4096); refresh default every
  4 days; page sizes 50-100; ~41-82 API calls per full refresh.
- HTTP: 15 s timeout (20 s SI), 3 attempts, backoff {0,1,3} s, max 2
  TLS sessions, 5 redirects max.

## 8. Positioning / prior art

- C4LJ IIIF articles to date are all server-side: tiling at scale
  (#14933), HTJ2K vs JPEG2000 (#17596), IIIF by the Numbers (#15217),
  annotation pipelines, migration stories. No IIIF-client article, no
  embedded/firmware article. The lane: "what your IIIF endpoint looks
  like from a 400 MHz microcontroller" — the consuming edge as a lens
  on how institutions actually serve images.
- Prior-art claim discipline (inherited from the parked listserv draft):
  do not assert "first firmware IIIF client"; searched discuss.iiif.io +
  IIIF Consortium news without finding one; phrase as "as far as I can
  tell" or invite correction.
- Angle candidates: (a) implementation report proper; (b) "seven
  museums, seven APIs" — the IIIF Image API is the only uniform layer,
  discovery/search above it is bespoke per institution; (c) constraints
  as clarity — what a 64-byte cache entry and a 33-byte identifier slot
  force you to decide about metadata.

## 9. Future work / ecosystem observations (from 2026-08 survey)

- Museum APIs keep dying: NYPL API shut down 2026-08-01, Walters 2023,
  MKG Hamburg host gone, Minneapolis image hosts dead. Aliveness checks
  and per-source kill switches are a real design concern.
- Bot protection is now a first-class device risk: WAFs/Cloudflare
  challenges reject non-browser TLS fingerprints (Finna, Science Museum
  Group, Brooklyn); compliant User-Agent load-bearing at Wikimedia,
  LoC, Gallica.
- Next museum candidate: Cleveland Museum of Art (anonymous JSON,
  working CC0+has-image filters, ~900 px CDN URL inline — "simpler than
  several museums already shipped").
- Deferred: Europeana/DPLA aggregators, Gallica (SRU/XML — no XML
  parser on device), keyword search, `info.json` negotiation.

## 10. Journal fit notes

- 1500-5000 words; clarity over formality; headings; code "where and
  only where useful"; images separate PNGs ≤500 px inline; CC-BY; CSE
  refs; abstract + 1-2 sentence bio + disclosure statement required.
- The parked listserv draft's four "questions for the community" can
  become the article's discussion/future-work section.
