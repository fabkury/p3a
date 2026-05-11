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

## Round 2 — 2026-05-11 (answers pending)

### 1. Confirm the three "no break" decisions

The whole design rests on these. If you want to override any of them now
(e.g. you'd rather break the playset format so we can add per-channel
filters in the same wave), it's much cheaper to know now than in the
middle of implementation.

- **Recommendation:** keep all three "no break" choices.

### 2. Channel display name format

The 65-char `display_name` slot needs a convention so channels look
consistent in the playset list and on the device's info screen.

- Option A: `"AIC · Departments · Arts of Greece, Rome, and Byzantium"`
- Option B: `"Painting (AIC)"` (museum tag at end, axis dropped)
- Option C: `"AIC / Department / Painting"` (path-like)
- **Recommendation:** **Option A** — three dots-separated parts make the
  hierarchy explicit and read well in the existing playset-item layout.

### 3. First-refresh trigger for institution channels

When a playset that contains an institution channel is activated, do we
kick off a refresh immediately, or wait for the periodic timer (up to
8 hours)?

- **Recommendation:** **immediate** — same as Makapix and Giphy. The
  user just made a choice; they expect to see artwork soon.

### 4. Standalone "Browse Museums" page

Today the only entry into the museum browse flow would be the Add Channel
modal in the playset editor. Some users may want to browse museums
casually without committing to a playset edit.

- **Recommendation:** **not in v1.** Keep the surface area minimal; add
  later if there's real demand. The browse code is reusable, so adding
  `/museum-browse` as a thin wrapper later is cheap.

### 5. Pre-warm of axis lists on first Wi-Fi connect

The browse modal could feel slow the first time it opens (AIC's facet
probes are 30 parallel requests). The device could pre-fetch the axis
term lists and serve them as static JSON.

- **Recommendation:** **no in v1.** Browse is browser-direct and the
  device shouldn't be a cache for browser-facing data. If browse latency
  becomes annoying in field testing, add browser-side localStorage
  caching first (cheaper than a device-side endpoint).

### 6. TLS cert bundle coverage

The C-side downloader uses `esp_crt_bundle`, which we need to confirm
covers `api.artic.edu`, `www.artic.edu`, `iiif.micr.io`, and
`data.rijksmuseum.nl`.

- **Recommendation:** I'll verify this before any C code lands. If any
  CDN isn't covered, document the workaround (explicit cert pinning, or
  `esp_crt_bundle_attach` + custom cert) in the design doc rather than
  discovering it at flash time.

### 7. `info.json`-aware rendition negotiation

Confirm v1 ships only with `…/full/!720,720/0/default.jpg` and the
smarter "smallest discrete size ≥ 720" path is deferred to v1.1 with
measurement.

- **Recommendation:** **defer.** Adding it later is non-breaking.

### 8. Build & test harness

The design doc names `components/art_institution/test/fixtures/` for
captured JSON responses. The browser-side adapters can mirror ubi-test's
Playwright suite. If you'll be running everything by hand, I'll skip
building a CI harness in v1; if you want one, I'll plan for it now
(affects component layout slightly).

- **Recommendation:** **skip CI in v1**, capture fixtures locally so the
  C parsing is testable manually, lean on the existing ubi-test
  Playwright suite for the JS adapters.

### 9. Order of implementation

Several big chunks can land in different orders. Possible sequences:

- **A:** C component first → browser adapters → editor UI → ship
- **B:** Browser adapters + editor UI first (validates UX) → C component → ship
- **C:** AIC end-to-end first (slice through C + JS + UI) → Rijks
  end-to-end → ship
- **Recommendation:** **C** — vertical slicing surfaces architecture
  mistakes earliest. Once AIC works end-to-end, Rijks reuses the same
  scaffolding.
