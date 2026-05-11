# Art Institution Channels — Question rounds

This file captures the back-and-forth that shapes
[design.md](./design.md). Each round lists the question, the recommendation,
and (when given) the user's answer. Decisions ratified here flow into
design.md's decision log.

## Round 1 — 2026-05-10

### 1. Naming and namespacing

What term do we use throughout the codebase, the playset format, and the UI
label? "Art institution", "museum", "institution"?

- **Recommendation:** Code uses `art_institution` (component name, channel
  type), public/UI labels use **"Museums"** because that's the lay term. The
  playset channel-type string is `"institution"` (short, type-stable).
  Inside the codebase, we'll have e.g. `PS_CHANNEL_TYPE_INSTITUTION`,
  `art_institution_refresh_channel(...)`.
- **Answer:** Recommendation accepted.

### 2. Channel-spec encoding (the most consequential decision)

A museum channel needs to identify (museum, axis, term-id) — e.g. AIC ×
`departments` × `PC-4`, or Rijks × `set` × `26118`. We have only
`name[33]` + `identifier[33]` to work with, and the file format is
append-only.

- **Recommendation:** Add **one** new channel type
  `PS_CHANNEL_TYPE_INSTITUTION = 7` (next ordinal). Encode:
  - `name = "{museum_id}:{axis}"` — e.g. `"artic:departments"`,
    `"rijks:set"` (≤33 chars cleanly).
  - `identifier = "{term_id}"` — e.g. `"PC-4"`, `"26118"`.
  - This keeps the binary format unchanged (no new fields), keeps the
    `channel_id` SHA scheme correct, and lets the device-side dispatcher
    route on the `museum_id` prefix in `name`.
- **Answer:** "Given the magnitude of this feature, I am willing to
  implement a breaking change for this feature. Please tell me the pros and
  cons of refactoring the channel binary format."
- **Follow-up recommendation (delivered in Round 1.5):** Do not refactor.
  Pros (variable-length fields, future filters, single migration) are real
  but every Tier-1 museum surveyed fits the existing 33+33 slots, future
  search fits too (`name="artic:search"`, `identifier="<query>"`), and a
  break invalidates user playsets and doubles the test surface for every
  existing channel type. Full pros/cons in design.md §4.1.

### 3. Where per-museum logic lives (and the browser↔device split)

- **Browser side (browse phase):** mirror the ubi-test adapter pattern —
  one JS adapter per museum, embedded into the `webui/` assets and called
  directly by the playset-editor browser code. (This matches the existing
  makapix-direct precedent.)
- **Device side (refresh + download):** a new component `art_institution`
  with a per-museum dispatch table and one adapter file per museum
  (`art_inst_artic.c`, `art_inst_rijks.c`). Each adapter implements
  `refresh(spec, cache)` and
  `build_iiif_url(entry, longest_side, out, len)`.
- **Recommendation:** confirm this two-sided split.
- **Answer:** Recommendation accepted.

### 4. CORS and per-API browser auth

AIC requires the `AIC-User-Agent` header; Rijks needs an
`Accept: application/ld+json` header. Both work cross-origin per the
ubi-test verification. But: any device used in a corporate/captive network
may have CORS-altering proxies, and AIC is rate-limited to 60 req/min per
IP.

- **Recommendation:** Browser-direct in v1 (no device proxy). Keep
  adapters defensive: if a fetch fails, the browse UI surfaces a clear
  error rather than collapsing silently. Add a simple debounce on the AIC
  term-counts probe (it does up to 30 parallel requests in ubi-test — we
  should serialize or limit concurrency to ≤ ~6 to stay polite).
- **Answer:** Recommendation accepted.

### 5. Vault layout for museum-downloaded files

Where do the downloaded `.jpg`/`.webp` files for museum artworks live?
Reuse `vault/`, or get a new top-level path like `giphy/`?

- **Recommendation:** New path
  `/sdcard/p3a/museum/{museum_id}/{sha[0]}/{sha[1]}/{sha[2]}/{key}.{ext}`.
  The per-museum subdirectory makes it easy to wipe one museum's cache
  without touching others, mirrors the `giphy/` separation, and keeps the
  Makapix vault scheme's Linked-Open-Data semantics intact. The `key` for
  AIC is the `image_id` UUID (36 chars); for Rijks, the micrio short-id.
- **Answer:** "This is actually a highly delicate topic because it
  determines file ownership on the SD card across playsets. Please give me
  the pros and cons and a recommendation about: appending the channel ID
  to the artwork key, so that each channel always owns its artworks
  exclusively (channel delete -> unlink() all its artworks from SD card)."
- **Follow-up recommendation (delivered in Round 1.5):** Per-museum vault
  shared across channels (NOT per-channel ownership). Per-channel
  ownership pays a 2–5× duplication tax on overlapping AIC facets (e.g. a
  Picasso painting can sit in 4 channels: department, artwork-type, theme,
  subject). Channel deletion leaves orphans until eviction or a future
  mark-and-sweep GC. Full table in design.md §4.3.

### 6. Cache entry shape (64 bytes per entry)

A museum entry needs at minimum: `post_id` (int32, hashed from museum's
id), `kind`, `extension`, `created_at`, plus a key/identifier sufficient
to reconstruct the IIIF URL at download time without re-querying the
museum.

- **Recommendation:** Reuse the existing 64-byte slot via a new
  `museum_channel_entry_t` overlay (analogous to
  `giphy_channel_entry_t`). Store the IIIF base/key in the entry (≤24
  chars covers Rijks; AIC's 36-char UUID is the tight case — fits if we
  drop dimensions or compress). Add `PS_ENTRY_FORMAT_MUSEUM` enum value.
- **Answer:** "This sounds like we need to consider a refactor. Tell me
  how an ideal new cache binary format would look like here. Given the
  magnitude of this feature, I am willing to consider a breaking change."
- **Follow-up recommendation (delivered in Round 1.5):** Sibling layout
  `institution_channel_entry_t` fits in 64 bytes with a 48-byte
  `iiif_key` slot — covers AIC's 36-char UUIDs, SMK's 25-char JP2 paths,
  and Rijks's short micrio IDs. No cache format break required. Full
  pros/cons + final layout in design.md §4.2.

### 7. Browse UI shape and preview budget

The brief says "preview the first few items at 64×64". Where in the
playset-editor flow does this live, and how many thumbnails per term?

- **Recommendation:**
  - Replace the current "Add Channel" modal's flat type dropdown with a
    multi-step picker for type=Museum: museum → axis (if applicable) →
    term list with counts → preview strip → confirm.
  - Preview **8 thumbnails per term**, lazy-loaded only when a term is
    expanded (not all terms eagerly). Each is a direct IIIF call:
    `…/full/!64,64/0/default.jpg`.
  - "Picked" terms become a saved channel via the existing
    `POST /playsets/{name}` shape — no new server endpoint needed.
- **Answer:** Recommendation accepted.

### 8. Device-side IIIF rendition size

At playback time, what longest-side do we request from the IIIF server?
The screen is 720×720 24-bit.

- **Recommendation:** `…/full/!720,720/0/default.jpg` — same as ubi-test
  reports use for downloads. JPEG (not WebP); museum IIIF servers
  reliably serve JPEG, less reliably WebP. Asset type `ASSET_TYPE_JPEG`.
- **Answer:** "If it is possible to request the IIIF endpoint to send
  'smallest size that is bigger than 720x720 pixels', switch to that
  size. If not, keep the '!720,720' of ubi-test. If this is
  server-dependent, implement the latter as a fallback path to the
  former."
- **Follow-up:** v1 ships `!720,720` only. There is no IIIF parameter for
  "smallest discrete size ≥ N"; the closest mechanism is the `info.json`
  `sizes` array, which is unevenly populated and costs an extra round
  trip per artwork. The smarter negotiator is deferred to v1.1 with a
  measurement-driven decision per museum. See design.md §10.

### 9. Refresh cadence and cap

- **Recommendation:** Same hourly schedule the existing dispatcher uses
  for makapix/giphy. Cap at `CHANNEL_CACHE_DEFAULT_MAX_ENTRIES` (2048).
  Document this clearly in the channel-add UI ("up to 2048 artworks").
- **Answer:** "Art institutions have fundamentally different refresh
  cadences, so they deserve their own first-class, user-configurable,
  NVS-persisted, web UI-editable runtime setting: art institution
  channel refresh cadence (options: 8 hours, 1 day, 2 days, 4 days),
  and art institution channel cache size (options: 32 to 4096)."
- **Implementation:** Two new global NVS keys, `ai_refresh_sec` and
  `ai_cache_size`, surfaced in `webui/settings.html` under a new
  "Museums" section. Defaults: 86400 (1 day) and 256 entries. Allowed
  cache sizes: 32, 64, 128, 256, 512, 1024, 2048, 4096. See design.md §8.

### 10. Search vs browse-only in the v1 milestone

- **Recommendation:** **Browse-only in v1.** Defer keyword search.
- **Answer:** "We are not going to implement search for now, but we must
  be cognizant that this feature is desired in the future. This should
  be taken into consideration when considering whether to refactor any
  part of the codebase now, including breaking changes such as binary
  file format changes."
- **Implication:** the chosen channel-spec encoding (`name="artic:search"`,
  `identifier="<query>"`) does support future search without a format
  break. Tracked in design.md §13 (Future work).

### 11. Exhibitions axis (AIC) and any other "list-only" axes

AIC's `exhibitions` axis is list-only — it lists exhibitions but
search-by-exhibition is unsupported, so a saved channel for it would be
empty.

- **Recommendation:** Hide list-only axes from the browse UI entirely
  (mirror ubi-test's choice). The 6 filterable AIC axes plus Rijks's set
  list are the v1 surface.
- **Answer:** Recommendation accepted.

### 12. What you want me to deliver after this round

- **Recommendation:** A written design plan, then code.
- **Answer:** "Please do:
  - 12.a. Give me your written, detailed design plan. Place it into a
    new folder inside the docs/ folder. We are going to use that design
    plan as our live, evolving source of truth from now on.
  - 12.b. Commit what we've accomplished so far.
  - 12.c. Then ask me a new round of questions."
- **Delivery:** design.md added to `docs/art-institutions/`, committed as
  the live source of truth.

## Round 2 — 2026-05-11

### 1. Confirm the three "no break" decisions

The whole design rests on these. If you want to override any of them now
(e.g. you'd rather break the playset format so we can add per-channel
filters in the same wave), it's much cheaper to know now than in the
middle of implementation.

- **Recommendation:** keep all three "no break" choices.
- **Answer:** Recommendation accepted.

### 2. Channel display name format

The 65-char `display_name` slot needs a convention so channels look
consistent in the playset list and on the device's info screen.

- Option A: `"AIC · Departments · Arts of Greece, Rome, and Byzantium"`
- Option B: `"Painting (AIC)"` (museum tag at end, axis dropped)
- Option C: `"AIC / Department / Painting"` (path-like)
- **Recommendation:** **Option A** — three dots-separated parts make the
  hierarchy explicit and read well in the existing playset-item layout.
- **Answer:** Recommendation accepted. Rijks (axis-less) collapses to
  `"Rijks · {Set name}"`. Tail-ellipsis when the assembled string exceeds
  64 chars. See design.md §4.1.

### 3. First-refresh trigger for institution channels

When a playset that contains an institution channel is activated, do we
kick off a refresh immediately, or wait for the periodic timer (up to
8 hours)?

- **Recommendation:** **immediate** — same as Makapix and Giphy. The
  user just made a choice; they expect to see artwork soon.
- **Answer:** Immediate. See design.md §7.2 for the mechanism
  (`last_refresh = 0` on persist).

### 4. Standalone "Browse Museums" page

Today the only entry into the museum browse flow would be the Add Channel
modal in the playset editor. Some users may want to browse museums
casually without committing to a playset edit.

- **Recommendation:** **not in v1.** Keep the surface area minimal; add
  later if there's real demand. The browse code is reusable, so adding
  `/museum-browse` as a thin wrapper later is cheap.
- **Answer:** Not in v1.

### 5. Pre-warm of axis lists on first Wi-Fi connect

The browse modal could feel slow the first time it opens (AIC's facet
probes are 30 parallel requests). The device could pre-fetch the axis
term lists and serve them as static JSON.

- **Recommendation:** **no in v1.** Browse is browser-direct and the
  device shouldn't be a cache for browser-facing data. If browse latency
  becomes annoying in field testing, add browser-side localStorage
  caching first (cheaper than a device-side endpoint).
- **Answer:** Not in v1. **However**, the user flagged a Rijksmuseum
  quirk to verify. Verified: Rijks's OAI-PMH `ListSets` endpoint
  (`https://data.rijksmuseum.nl/oai?verb=ListSets`) does **not** return
  CORS headers, so a browser cannot fetch it directly. The only viable
  approach is to ship the set list as a baked LittleFS asset that the
  firmware itself serves at `/webui/museum/rijks-sets.json`. This is
  exactly what ubi-test does (see `reference/museum-art/ubi-test/tools/`
  `build_rijks_sets.py`). The design doc was updated to make this
  prominent (design.md §9.2 and decision log).

### 6. TLS cert bundle coverage

The C-side downloader uses `esp_crt_bundle`, which we need to confirm
covers `api.artic.edu`, `www.artic.edu`, `iiif.micr.io`, and
`data.rijksmuseum.nl`.

- **Recommendation:** Verify this before any C code lands. If any CDN
  isn't covered, document the workaround (explicit cert pinning, or
  `esp_crt_bundle_attach` + custom cert) in the design doc rather than
  discovering it at flash time.
- **Answer:** Recommendation accepted. Verification is the pre-merge
  gate documented in design.md §12.3.

### 7. `info.json`-aware rendition negotiation

Confirm v1 ships only with `…/full/!720,720/0/default.jpg` and the
smarter "smallest discrete size ≥ 720" path is deferred to v1.1 with
measurement.

- **Recommendation:** **defer.** Adding it later is non-breaking.
- **Answer:** Deferred.

### 8. Build & test harness

The design doc names `components/art_institution/test/fixtures/` for
captured JSON responses. The browser-side adapters can mirror ubi-test's
Playwright suite. If you'll be running everything by hand, I'll skip
building a CI harness in v1; if you want one, I'll plan for it now
(affects component layout slightly).

- **Recommendation:** **skip CI in v1**, capture fixtures locally so the
  C parsing is testable manually, lean on the existing ubi-test
  Playwright suite for the JS adapters.
- **Answer:** Skip CI. The user runs testing manually.

### 9. Order of implementation

Several big chunks can land in different orders. Possible sequences:

- **A:** C component first → browser adapters → editor UI → ship
- **B:** Browser adapters + editor UI first (validates UX) → C component → ship
- **C:** AIC end-to-end first (slice through C + JS + UI) → Rijks
  end-to-end → ship
- **Recommendation:** **C** — vertical slicing surfaces architecture
  mistakes earliest. Once AIC works end-to-end, Rijks reuses the same
  scaffolding.
- **Answer:** Option C. Milestones M1 (AIC), M2 (Rijks), M3 (polish)
  in design.md §15.

## Round 3 — 2026-05-11

These came out of folding the Round 2 answers in. They are smaller in
scope than Round 1/2 — mostly tightening behavior at boundaries — but
several are gating for M1.

### 1. Cache eviction policy when a channel exceeds `ai_cache_size`

When a refresh produces more candidates than `ai_cache_size`, which
entries get evicted? Today Makapix/Giphy use a FIFO-ish "oldest first"
trim after merge. Museums have a different access pattern: AIC term
listings are paginated by an internal Elasticsearch order that may
shift between refreshes (relevance / recency rank), and Rijks set
listings are roughly date-of-acquisition-ordered.

Options:

- **A:** Trim oldest by `created_at` (museum-provided timestamp). Stable
  across refreshes, but biases away from new acquisitions.
- **B:** Trim by insertion order into the cache (LRU-ish FIFO). Matches
  existing patterns; gives newly-listed artworks priority on next refresh.
- **C:** Trim by `post_id` hash (effectively random but deterministic).
  No bias; surprising at debug time.

**Recommendation:** **B** — match what we already do, accept the bias
toward "what the museum showed us most recently". Add a note in the
decision log so a future v2 can revisit if users complain.

**Answer:** B.

### 2. Default `ai_cache_size`

The default is currently 256. Museums can have very large sets (AIC
"Painting" has ~12,000 records; Rijks's largest sets are 5,000+).

- **A:** Keep 256 (one screen of channel content, 99th-percentile users
  probably won't notice the cap).
- **B:** Raise to 1024 (more variety, ~3 MB more SD usage per channel at
  ~3 KB/artwork JPEG-720 — small).
- **C:** Raise to 2048 to match the existing
  `CHANNEL_CACHE_DEFAULT_MAX_ENTRIES`.

**Recommendation:** **B (1024)**. The whole point of museum channels is
breadth-of-collection; 256 is too small to show off the catalog. 2048 is
generous but the marginal artwork-per-MB ratio drops quickly past 1024
because of the long tail of artworks the picker rarely reaches.

**Answer:** 1024.

### 3. Eager vs lazy image download

When a refresh adds N new entries to `Ci`, do we download all N JPEGs
right away, or only download an artwork the first time the picker
chooses it?

- Makapix: eager (download pipeline keeps the vault prefilled).
- Giphy: lazy (downloads on first play).

The Giphy pattern saves bandwidth on entries the picker never reaches.
Museum cache sizes (256–4096) are larger than typical Giphy caches, so
the gap matters more.

**Recommendation:** **lazy** (like Giphy). With `ai_cache_size = 1024`
the eager strategy would download ~3 MB × 1024 = ~3 GB per channel on
first refresh, which is unfriendly on a metered or slow connection.
Lazy keeps the SD usage proportional to actual playback.

**Answer:** "I don't understand your question. My understanding is this:
as long as you're playing a playset containing channel C, if C has items
in Ci but not in LAi then the download manager should be downloading
those files one at a time."

**Interpretation:** the user's model is **continuous serialized fill**,
not eager-parallel and not lazy-on-pick. As long as the active playset
contains an institution channel that has `Ci \ LAi ≠ ∅`, the download
manager continuously downloads one artwork at a time (oldest-not-in-LAi
first) until `Ci ⊆ LAi`, then idles until refresh. This is what design.md
§7.3 now describes. It also implies that the IIIF download path is not
parallelized — single-flight downloads, which conveniently helps with
AIC's per-IP rate limit as well.

(My original A/B/C framing was confusing because "eager" and "lazy"
gloss over the serialization axis. The chosen model is a third thing.
The above interpretation is what I'll proceed with — flag if it's
wrong.)

### 4. Rijks "unresolvable" retry policy

The Rijks lazy-IIIF flow (3-hop Linked Art walk at first-download time)
can fail for a specific HMO. If it fails, do we:

- **A:** Retry forever on each download attempt (current design).
- **B:** Mark the entry as permanently unresolvable after N attempts
  (need a sentinel — a second extension-byte value, e.g. `0xFE` =
  "tombstone").
- **C:** Drop the entry from the cache after N attempts.

**Recommendation:** **B with N = 3**. Drop-from-cache (C) is loud but
the next refresh will re-add it. Retry-forever (A) wastes bandwidth on
artworks Rijks has actually removed.

**Answer:** B with N = 3. See design.md §7.3 + §11 for the tombstone
encoding (`extension = 0xFE`). A subsequent refresh re-creates the
entry with a fresh budget if the HMO still appears in the set
listing.

### 5. Refresh staggering / device-side rate limiting

If a user activates a playset with, say, 4 AIC channels and 2 Rijks
channels at once, the refresh dispatcher could fire 6 refreshes at
roughly the same moment. AIC's per-IP cap of 60 req/min could be hit
during the initial walk (each channel walks up to 41 pages of 100
artworks → 41 requests per channel × 4 channels = 164 requests in
short succession).

Options:

- **A:** No staggering. Trust per-museum retry/cooldown for 429s
  (existing pattern). Worst case: a few channels stall on first refresh
  and recover on the next dispatcher tick.
- **B:** Serialize per-museum (at most one in-flight refresh per
  museum). Simple to implement, slows initial fill but bounded.
- **C:** Serialize globally for institution channels (at most one
  in-flight institution refresh at a time). Simplest; slowest.

**Recommendation:** **B** — per-museum serialization. AIC is the only
museum where this realistically matters; Rijks's listing is light and
its real cost is the per-artwork lazy walk (which is download-path,
not refresh-path). Keeps initial-fill latency reasonable for users who
mix museums.

**Answer:** "B — AIC's 60-req/min cap is the one that bites. API
rate-limiting needs to be a 'first class concept' in the codebase.
Look at how Giphy handles its limited API key: the web UI is fully
cognizant of the limit, and the device is able to navigate around it
efficiently. The same level of polish needs to be present for art
institution APIs."

**Action taken:** design.md §11.1 added — a first-class rate-limit
infrastructure modeled on Giphy's
(`giphy_set_rate_limited` / `giphy_is_rate_limited` /
`giphy_rate_limit_remaining_seconds`), generalized to a per-museum
cooldown table. Includes:

- Public API in `art_institution.h` (`art_institution_set_rate_limited`,
  etc.).
- Refresh + download paths both gate on cooldown.
- Per-museum serialization (this question's literal answer).
- `Retry-After` header honored when present; per-museum default
  fallback otherwise.
- New REST endpoints `GET /api/v1/museum/rate-limits` and `POST
  /api/v1/museum/rate-limits/report-429` so browser-side fetches share
  the same cooldown state with device-side fetches (AIC's limit is
  per-IP — they share the budget).
- Browse modal reads cooldown state and renders a countdown when in
  cooldown; reports its own 429s back to the device.
- Settings page hint mirrors the Giphy section's tone.

A follow-up open question (Round 4 Q1) asks whether anything in this
infrastructure proposal needs more elaboration before M1.

### 6. Storing artist / title / date for on-device display

The 64-byte cache entry has no room for human-readable metadata. The
existing device has no "now showing" overlay either, so today it
wouldn't be used. But the channel info screen could plausibly show
"Picasso, Self Portrait, 1907" instead of just a thumbnail.

- **A:** Don't store. Stays bandwidth-cheap; UI is icon-only.
- **B:** Store in a sidecar file per channel (`metadata.bin` alongside
  `entries.bin`). Some IO cost on refresh.
- **C:** Skip in v1, design the sidecar shape now so it can be added
  non-breakingly later.

**Recommendation:** **C** — define the file layout in the design doc as
deferred work, but don't build it. Concretely: a parallel binary file
keyed by `post_id` containing variable-length UTF-8 records.

**Answer:** "For v1, option A." No metadata storage, no reserved
layout. The decision keeps v1 lean; if v1.1 wants a "now showing"
overlay, the sidecar can be added then with a then-fresh design pass.

### 7. Channel-add modal: minimum thumbnail strip semantics

Round 1 confirmed "8 thumbnails per term". Clarifications still open:

- Is the strip purely advisory (user has already committed to a term
  by the time they click "Add") or is it the actual confirmation step
  (i.e., the user can back out after seeing the previews)?
- What happens if fewer than 8 artworks exist in a term? Just show
  what's there?
- Do we show artwork titles / artists in the strip, or thumbnails
  only?

**Recommendation:** strip is confirmation step (user clicks "Add"
beneath the strip), shows whatever count is available (≤8), thumbnails
only, no captions. Tooltips on hover for desktop browsers if cheap.

**Answer:** Confirmed.

### 8. Settings UI placement

`webui/settings.html` would gain a "Museums" section. Where?

- **A:** Alphabetical (after "Giphy", before "Network").
- **B:** Grouped by content source (Makapix, Giphy, Museums together).
- **C:** New section at the bottom of the page (less disruption).

**Recommendation:** **B** — grouping by content source is the user's
mental model.

**Answer:** B.

### 9. `webui/museum/` module loading

The design has `webui/museum/*.js` loaded as ES modules
(`<script type="module">`) from `playset-editor.html`. Modern browsers
support this natively. The existing `webui/compat.js` shim exists for
older / restricted browsers — I want to make sure the playset editor
doesn't accidentally end up rendered in a context where ES modules
fail.

**Recommendation:** ES modules are fine — the playset editor is only
served to LAN-connected modern browsers (phone, laptop), never via
the boot-time captive portal.

**Answer:** "I don't understand what you are asking here, please
explain."

**Re-asked:** This is really a "no surprises at flash time" check, not
a design decision. Concretely, the question is:

> Is it true that when the user opens the playset editor in their
> browser, they're always reaching it via the LAN HTTP server on the
> normally-booted device — not via the captive portal that runs
> during Wi-Fi setup? If yes, ES modules work for everyone and we're
> done. If the playset editor is ever served from the captive portal
> (which I would not expect, but the codebase does have a `compat.js`
> shim, so I want to confirm), then we need to think about whether
> the museum browse modal can load there.

A grep of the captive-portal serving code on my side can answer this
without bothering the user. Pulled out of Round 3 — moved to a
**self-check** I'll do before M1 starts, not a question for the user.
If it surfaces something surprising I'll come back with a real
question.

### 10. Anything I missed?

Open-ended catch-all. After Round 3 lands, the next step is starting
M1 (AIC C component + browser adapter + editor wiring + soak test).
If anything in §1–§16 of design.md reads wrong on a re-read with fresh
eyes, this is the cheapest moment to flag it.

**Answer:** "I have no comments about this."

## Round 4 — 2026-05-11

Three questions left, picked to close out the design phase before code
lands.

### 1. Concrete bar for "first-class rate-limit handling"

design.md §11.1 proposes:

- Per-museum cooldown table with `art_institution_set_rate_limited` /
  `..._is_rate_limited` / `..._remaining_seconds`, mirroring Giphy.
- `Retry-After` honored when present; per-museum default fallback
  otherwise.
- New `GET /api/v1/museum/rate-limits` and `POST
  /api/v1/museum/rate-limits/report-429` endpoints so the browser and
  device share AIC's per-IP budget.
- Browse modal renders a countdown when in cooldown.
- Settings hint documents AIC's 60-req/min limit with the math.

Does this match the bar you set when you said "the same level of
polish as Giphy"? Specifically:

- (a) Is the `GET`/`POST` browser↔device cooldown-sharing pair
  necessary, or is browser-only handling enough (the device just
  observes its own 429s)? The sharing is the part that's
  *more* than what Giphy does — Giphy's key is server-side only.
- (b) Should the device's cooldown state survive reboot (persisted
  to NVS), or is Giphy's RAM-only behavior the right reference? AIC
  60s windows make reboot-clear acceptable; longer cooldowns might
  not.
- (c) Anything else you've seen go wrong with rate-limit polish that
  I should design defensively against?

**Recommendation:** keep all three (browser↔device sharing,
RAM-only state, no extra defensive surface beyond what's in §11.1).
But this is exactly the kind of thing where the user's intuition is
load-bearing.

**Answer:**
- (a) Yes, share.
- (b) RAM-only.
- (c) "Be meticulous about the channel list, in the current playset
  info, on the landing page of the web UI. Like with Giphy channels,
  an institution channel should appear as 'API rate limited' only if
  it is stale (i.e. needs refresh) AND the API has reached its rate
  limit."

**Action taken (c):** design.md §11.1 now spells out the landing-page
badge rule. The badge renders **only when both** `now ≥ last_refresh +
ai_refresh_sec` and `art_institution_rate_limit_remaining(museum) > 0`.
Fresh channels never show the badge even if the API is throttled (they
have content to play); non-throttled stale channels never show it
either (the next dispatcher tick will fix them). This mirrors Giphy's
landing-page behavior.

### 2. Vault eviction policy on SD-space pressure

design.md §4.3 picks per-museum-shared vault storage and notes:
"orphans remain until eviction or a future mark-and-sweep pass." But
*when* does eviction trigger, and *which files go first*?

Today the codebase manages vault size via:

- The cache's `ai_cache_size` cap (so `Ci` doesn't grow unboundedly).
- Implicit assumption: SD is large enough.

Museum channels at 1024 entries × ~3 MB per JPEG-720 ≈ 3 GB per
channel. If a user has 3 museum channels + Makapix + Giphy, that's
realistic SD pressure on smaller cards (Waveshare boards typically
ship with 8 GB).

Options:

- **A:** Do nothing in v1. If the SD fills up, downloads start
  failing and the picker uses what's there. Document the math in
  the settings hint.
- **B:** Maintain a configurable free-space floor (e.g. 500 MB).
  When crossed, evict museum vault files LRU (by file `atime`) until
  the floor is restored. Eviction only touches `/sdcard/p3a/museum/`,
  never Makapix vault or Giphy cache (which already self-manage).
- **C:** Per-museum cache size is *also* a vault size cap — but
  this would require tracking per-museum disk usage, which adds
  bookkeeping.

**Recommendation:** **A in v1.** Museum vault eviction belongs in
the same v1.1 pass as `info.json` rendition negotiation — both want
real on-device measurement before designing. v1 fails loudly (logs +
inert downloads) if the SD fills, which is debuggable. **B** is the
target for v1.1 once we know typical user patterns.

**Answer:** "You have highlighted an important problem in our design.
We cannot afford to just leave orphan files like that. Please propose
ways to avoid leaving orphan files. What if we deleted all artworks of
a given channel when its playset gets deleted?"

**Proposal (now in design.md §4.4):** **cross-channel mark-and-sweep
vault GC in v1.**

The user's literal proposal — "delete all artworks of a channel when
its playset gets deleted" — doesn't work as stated because the vault
is per-museum and shared across channels: a Picasso painting can sit
in 4 AIC channels (Department × Artwork-Type × Subject × Theme), each
potentially in different playsets. Unlinking on playset delete would
nuke files other playsets still need.

The right mechanism is **mark-and-sweep**, in two phases:

1. **Phase 1 — Orphan channel cache GC.** After a playset save or
   delete, walk all playsets on disk, collect the set of channel_ids
   they reference. Delete cache files whose channel_id isn't
   referenced anywhere.
2. **Phase 2 — Orphan vault file GC, per museum.** For each museum,
   walk the surviving cache files, build the union of `iiif_key`s in
   their `Ci`. Unlink vault files outside the union.

Triggered by:

| Event | Phase 1 | Phase 2 |
|---|---|---|
| Playset save (channel removed) | yes | yes |
| Playset delete | yes | yes |
| Refresh-driven FIFO trim past `ai_cache_size` | no | yes |
| Boot-time sanity sweep | yes | yes |

Runs async on a low-priority task. Coalescing flag bundles
back-to-back triggers into one sweep. Logs counts of cache files and
vault files removed, matching the existing intra-channel evict log
line.

Reference counting was considered and rejected: persisted refcount
files are hard to keep consistent across crashes, and the recovery
path ends up doing mark-and-sweep anyway. Per-channel vault ownership
(append channel_id to vault path) was rejected back in Round 1 §4.3
because it pays a 2–5× duplication tax on overlapping AIC facets.

**Round 5 follow-up:** confirm this proposal matches your intent
before M1 code lands. Specifically: are the four trigger points the
right set, or would you prefer to add a periodic safety-net pass too?

### 3. M1 entry conditions / commit cadence

Two sub-questions about how M1 should run as a working process,
not a design decision:

- (a) Should the current `docs/art-institutions/` updates (Rounds
  1–3 closed, Round 4 open, design.md v0.3) be committed *before*
  any code starts, so the design is reviewable in isolation? Or
  bundle the design updates with the first M1 commit?
- (b) Inside M1 itself, the work splits naturally into ~6 chunks
  (component scaffold, playset wiring, AIC adapter C, AIC adapter
  JS, browse modal, settings page). Should these land as one M1 PR
  reviewed at the end, or as a stack of small PRs that each pass
  build/manual test?

**Recommendation for (a):** commit the design updates now, separately.
The design has shifted enough since the last commit
(`bc944362 Add Q&A transcript`) that pinning it lets me reference a
known-good baseline in commit messages.

**Recommendation for (b):** one M1 PR. The chunks are tightly
coupled — a half-implemented C side without a UI is unusable to
flash-test, and a UI side without the C side has nothing to talk
to. One PR also matches how the rest of this repo has been shipping
recent features.

**Answer:**
- (a) "commit docs now, so design has a clean baseline I can reference
  in code commit messages."
- (b) "one M1 PR. Half-implemented C with no UI is untestable, and
  vice versa."

## Round 5 — 2026-05-11 (single follow-up triggered by Round 4 Q2)

The Round 4 Q2 answer asked me to propose orphan-prevention schemes;
I did that inline above and added §4.4 to design.md. Round 5 is a
single confirmation question.

### 1. Does the proposed vault GC scheme match your intent?

Concretely (full text in design.md §4.4 and questions.md Round 4 Q2):

- **Phase 1** removes orphan channel cache files; **Phase 2** removes
  orphan vault files per museum.
- Runs async on a low-priority task with a coalescing flag.
- Triggers: playset save, playset delete, refresh-driven FIFO trim
  past `ai_cache_size`, boot-time sanity sweep.
- No periodic safety-net pass in v1 (added as future work; trigger
  set should cover the actual orphan-creating events).

**Things to confirm or override:**

- (a) Is the four-trigger set complete? Anything I missed?
- (b) Should there also be a periodic safety-net pass in v1 (e.g. every
  24 hours), or is that paranoia until we see evidence of leaks?
- (c) The proposal explicitly *doesn't* implement reference counting
  or per-channel vault ownership. Both were considered and rejected
  for the reasons in §4.4. Any objection to those rejections?

**Recommendation:** accept the scheme as proposed. Add a periodic
safety-net pass in v1.1 if field testing surfaces leaks.

If the answer is "yes, as proposed", I'll consider the design phase
closed and proceed with the docs commit + M1.

**Answer:** "I don't like the idea of 4.4 Cross-channel vault GC. It
seems brittle and costly from a computational perspective. Let's do
this:
- when refreshing a channel, intra-channel orphaned artworks get
  evicted, just like `channel_cache_evict_orphans_makapix`,
  `giphy_evict_orphans` do.
- when deleting a playset, no artwork eviction takes place. Files
  are left orphaned.
- If storage becomes limited, the existing eviction mechanism (evict
  artworks not used within the last X days) should be triggered to
  release SD storage space."

**Action taken:** the prototype §4.4 (cross-channel mark-and-sweep) was
**rejected and replaced**. The replacement §4.4 leans entirely on two
existing codebase mechanisms — no new GC code:

1. **Refresh-time intra-channel orphan eviction** — the museum adapter
   calls the same eviction the Makapix and Giphy adapters already do
   (`channel_cache_evict_orphans_makapix` / `giphy_evict_orphans`)
   at the end of a full-refresh walk.
2. **Age-based storage eviction** — the existing
   `components/storage_eviction/` already walks Makapix vault and
   Giphy cache when SD free space drops below
   `CONFIG_STORAGE_EVICTION_TARGET_MIB`, with multi-pass age
   halving. Extension: add `sd_path_get_museum()` and one extra
   `evict_from_base_dir()` call in `evict_old_files()`. The
   recursive walker is base-dir-agnostic; the museum subdirectory
   layout matches vault and giphy.

Channel cache file orphans (cache files for channels no longer
referenced by any playset) are already handled by
`channel_eviction_check_and_run()`, which deletes stale channel
`.cache` files protected by active-playset reference. Once a playset
stops referencing a channel, its cache ages out via that path.

The trade is: orphan vault files can persist on SD between the time
they become unreferenced and the next eviction pass. That's fine —
the files cost nothing while SD has headroom, and when pressure hits,
age-based eviction picks oldest/least-recently-used (which is what
orphans tend to be) automatically.

Design phase **closed**. Docs commit + M1 implementation can proceed.
