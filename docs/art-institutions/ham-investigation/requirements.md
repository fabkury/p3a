# Harvard Art Museums — Stage 1 requirements

*Stage 1 of the three-stage HAM evaluation. Stage 2 (`probe_*.py` scripts +
`REPORT.md`) will exercise each item below against the live HAM API. Stage 3
(adapter implementation) only begins once Stage 2 returns go/no-go answers
for every Required item.*

The checklist is extracted from the five live adapters (AIC, Rijks, V&A,
Wellcome, SMK), `docs/art-institutions/finalized-design.md`, and the
existing Stage-2-style reports for offset partitioning and the Library of
Congress investigation.

Tier legend: **R** = required (can't add HAM without it); **P** = preferred
(workaround exists but costs adapter code); **O** = optional (nice-to-have).

## Decisions locked at Stage 1

| Decision | Choice |
|---|---|
| API-key placement | **BYOK (bring-your-own-key)** — *revised post-Stage 2.* Initially set as "browser + device (shared)" with the assumption of a shipped key; revised to BYOK after Stage 2 confirmed HAM's 2 500 req/day-per-key cap would be shared across the installed base. Key is stored in NVS under `ham_api_key`, entered via a new "Museums" section in `webui/settings.html`, and served to the browser at browse time. Mirrors the Giphy model. |
| Axis selection strategy | **Inventory + recommend** — *revised post-Stage 2.* Originally framed as "ship the 1–4 most user-meaningful axes"; revised to hybrid discovery — compile-time display-label map + skip-list, runtime axis health check. See REPORT.md §"Axis strategy". |
| HAM API key used during Stage 2 | `c09e5b21-5ea4-4762-b611-e41d3a2ba07d` (single-user investigation key; not shipped). |

## Phase A — Browse (browser-side, in the playset editor)

| # | Capability | Tier | Stage 2 probe |
|---|---|---|---|
| A1 | Enumerate one or more **facet axes** (e.g. classification, period, century, technique, gallery, culture, place). | R | Inventory HAM's full facet/aggregation surface; recommend 1–4 axes that map well to user intent. |
| A2 | For each candidate axis, list **terms with image-bearing-artwork counts**, sorted "most populous first". | R | Find the term-enumeration endpoint(s); verify counts are obtainable in ≤ 1 request per axis (Wellcome aggregations style) or with a bounded per-term probe (AIC/V&A venue style). |
| A3 | For an (axis, term) pair, **list artworks** with `offset`/`page` + `rows`. | R | GET pages 1–3 and an offset jump; verify ordering is stable across calls. |
| A4 | Filter to **image-bearing records only** (so refresh doesn't store metadata-only entries). | R | Find HAM's equivalent of `images_exist=1` / `has_image:true`. If absent, verify the per-record image field can be tested cheaply during listing. |
| A5 | Per-artwork **caption metadata** (title, creator, date) inline on the listing response. | R | Confirm these fields exist on the listing payload — not just per-artwork detail. |
| A6 | **CORS** headers permit browser-direct calls from a non-museum origin (`p3a.local`). | P | Send a preflight from a browser DevTools console. If blocked, plan to proxy via the device or bake a static snapshot (Rijks pattern). |
| A7 | A **preview image** at ≈ 400×400 reachable from the browser. | R | Verify HAM's IIIF (or thumbnail) URL pattern from a desktop browser. |

## Phase B — Refresh (device-side, periodic)

| # | Capability | Tier | Stage 2 probe |
|---|---|---|---|
| B1 | Same listing endpoint as A3 reachable via **HTTPS from ESP-IDF's `esp_http_client`** with the `esp_crt_bundle` cert store. | R | TLS handshake from Linux with `openssl s_client` against HAM's listing host; cross-check the chain root is in `esp-x509-crt-bundle`. |
| B2 | **Random-access pagination** at offsets up to `channel_offset + ai_cache_size`. With defaults that's `offset + 1024`; with the ceiling (`CHANNEL_CACHE_HARD_CAP = 4096`) it's `offset + 4096`. | R | Walk to offsets 100, 1000, 4096, 10 000. Note where it 403s, errors, or returns empty. Note any cap that depends on auth tier. |
| B3 | **Total record count** in the listing envelope. Used for orphan eviction and `channel_offset % total` modulo wrap. | R | Look for `total`, `record_count`, `info.totalrecords`, etc. |
| B4 | **Rate-limit signaling**: 429 + `Retry-After` (or a documented per-API limit we can hard-code as a cooldown). | R | Burst-probe to provoke. Confirm: (a) the limit's value, (b) the response shape, (c) whether the limit is per-IP or per-API-key. Per-key matters because every shipped p3a uses the same key. |
| B5 | **Stable artwork identifiers** across refreshes — orphan eviction churns the cache otherwise. | P | Capture 50 IDs now; re-fetch one week later or read HAM's docs for ID-stability guarantees. |
| B6 | **JSON** response — cJSON is the only on-device parser. No XML / SRU. | R | Confirm `Content-Type: application/json`. |
| B7 | **Deep-offset workaround** if B2's natural cap is below 4096. AIC needed bool+range POST DSL because public callers cap at `from + size ≤ 1000`. | P | If B2 caps low, look for an escape hatch — partition-by-ID range, filter-by-date, cursor/scroll, etc. |

## Phase C — Download (device-side, per artwork)

| # | Capability | Tier | Stage 2 probe |
|---|---|---|---|
| C1 | **IIIF Image API v2** (or compatible) URL pattern serving JPEG at a requested longest-side cap. p3a uses `…/{key}/full/!720,720/0/default.jpg`. | R | Fetch one image with our exact size syntax; verify `image/jpeg`, file size < 16 MiB, longest side ≈ 720 px. |
| C2 | **Image identifier fits in 47 chars** (the 48-byte `iiif_key` slot, null-terminated). Reference: AIC UUID 36, Rijks 5–12, SMK ~25, Wellcome ~10. This was the binding constraint that killed LoC. | R | Sample 50 IDs from a representative listing; report `max_len` and the distribution. |
| C3 | **Per-image file size ≤ 16 MiB** (`P3A_MAX_ARTWORK_SIZE`). | P | Spot-check large works (paintings, prints) at !720,720; if any exceed, document the workaround (smaller cap, dimension filter). |
| C4 | **TLS chain on the image CDN** covered by `esp_crt_bundle`. The IIIF host may differ from the API host. | R | Same as B1 but against the IIIF host. |
| C5 | **Image identifier discoverable** — either inline on the listing (AIC/V&A/Wellcome/SMK pattern, preferred) or via a documented walk (Rijks Linked-Art 3-hop). | R | If a walk is required: how many hops? > 3 hops × ~300 ms each is a strong reason to defer. |

## Phase D — Engineering, security, operations

| # | Capability | Tier | Stage 2 probe |
|---|---|---|---|
| D1 | **API-key model**: required on every endpoint or only some? Quota per key or per IP? Key revocable without firmware update? | R | Read HAM API docs. Burst the listing endpoint with and without `apikey=…`. |
| D2 | **No per-request CPU-heavy auth** (no HMAC body signing, no JWT minting). A static `?apikey=…` query param or header is fine. | R | Read auth docs. |
| D3 | **Acceptable Use Policy** allows our use case (public artwork display via embedded device). Attribution requirements documented. | R | Read HAM's API ToS. Note any "Image courtesy of …" overlay obligations as design input, not blockers. |
| D4 | **Cipher support** on HAM CDNs doesn't exclude ESP-IDF mbedTLS defaults. | P | If TLS handshakes succeed from desktop but fail from device, the cipher list is the suspect. |

## Patterns that probably apply (heads-up reminders)

These don't need a Stage 2 probe — they're shapes the firmware already
handles. If HAM exhibits one, we won't be surprised:

- **Lazy resolution** (Rijks pattern) — opaque IDs needing a fetch chain to
  discover the image URL. The `resolve_entry` dispatch hook handles it.
  Three failures → tombstone.
- **403 on deep pages** (AIC pattern) — undocumented page cap that
  strict-fails past N. Adapter treats it as partial-success refresh.
- **Polymorphic facet response** (SMK pattern) — defensive parsing of
  multiple shapes is already an established pattern.
- **Aggregation cap** (Wellcome pattern) — capped count of terms per
  facet (Wellcome caps at 20). User picks from the top-N most-populous
  terms.

## Stage 2 deliverables (for symmetry with existing investigations)

Following the convention from `docs/art-institutions/offset-tests/` and
`docs/art-institutions/loc-investigation/`:

```
docs/art-institutions/ham-investigation/
  requirements.md              # this file
  REPORT.md                    # Stage 2 headline findings + go/no-go
  probe_ham_basic.py           # Phase A/B basic surface
  probe_ham_facets.py          # Phase A1/A2 axis inventory
  probe_ham_deep.py            # Phase B2/B7 deep-offset probe
  probe_ham_image.py           # Phase C1/C2/C3 IIIF rendition tests
  output/raw/                  # captured JSON dumps for reproducibility
  output/                      # per-script .md summaries
  .gitignore                   # ignore __pycache__, .venv, etc.
```

## Go / no-go criteria for Stage 3

After Stage 2 finishes, HAM proceeds to Stage 3 (adapter implementation) if
**all R items** in Phases A–D pass, **and** Phase C2 confirms the image
identifier fits in 47 chars for the recommended axes. Failing P items are
documented as adapter quirks; failing R items either block or require a
design exception captured in the report's go/no-go section.
