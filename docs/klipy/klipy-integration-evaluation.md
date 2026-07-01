# Klipy as a p3a data source — feasibility & integration evaluation

**Status:** Evaluation / decision-support (no code written)
**Date:** 2026-07-01
**Verification:** All claims below were checked against the **live Klipy API** on 2026-07-01 using a
temporary **test key** (100 req/hr tier). The key is intentionally **not** stored in this repo.
**Scope of the question:** Can [Klipy](https://klipy.com/) be integrated into p3a as one more artwork
source (like Makapix, Museums, Giphy)? What would it look like, what gaps exist, and what extra
features could p3a adopt?

---

## 1. Verdict (executive summary)

**Yes — Klipy is a clean, low-risk addition, and it is architecturally a near-mirror of the existing
Giphy component.** It exposes the same primitives p3a's Giphy integration already relies on
(API-key auth, a trending feed, search, paginated JSON, per-size renditions, optional analytics
pings), so it drops into the same six seams: `components/klipy/`, a new play-scheduler channel type,
the SD vault, `config_store`, `http_api`, and a Web-UI settings tab.

Three findings stand out from the live probe:

1. **It is *simpler* than Giphy in Giphy's single worst area.** Klipy returns an explicit
   `url` + `width` + `height` + `size` for **every** (size-tier × format) cell. There is **no**
   CDN-filename guessing and no need for the per-entry rendition override (`reserved[0] ∈ {0,1,2}`,
   the `downsized_medium` vs `giphy.gif` passthrough logic, the `.404` markers) that the
   Giphy integration carries. That entire bug class disappears.
2. **The one real design difference — opaque CDN URLs — has a clean solution.** Klipy download URLs
   are random per-format tokens (`…/aFVSwkzt.mp4`), not reconstructable from the item, and ~80 chars
   (won't fit the 64-byte channel entry). **But** each item has a compact **numeric `id`**
   (`uint64`), and `GET gifs/{id}` resolves identically to `GET gifs/{slug}`. So p3a stores the
   8-byte id and re-resolves at download time. (Verified live.)
3. **The strongest reason to do it is not "more GIFs."** Over the existing Giphy channel, another GIF
   feed is marginal. The compelling additions are **Stickers** (transparent *animated* WebP — a
   content type p3a's decoder already supports but Giphy never gave us) and **provider resilience**
   (a fallback for the Giphy 401/429 pain already engineered around).

**Rough effort:** ~2–3 weeks for trending+search GIF parity (down from a naive Giphy-parity estimate
because the rendition complexity is removed), +~3–5 days for Stickers, +~1–2 days for a Categories
browse UI.

**Recommendation:** Worth building **if** the goal is Stickers and/or a Giphy fallback. If the goal is
just more GIF volume, shelve it behind the demo-video / adoption work.

---

## 2. What was verified live (evidence)

| # | Claim | Method | Result |
|---|-------|--------|--------|
| 1 | Key auth works; key goes in the URL **path** | `GET …/{key}/gifs/trending` | `200`, `result:true` |
| 2 | Rate-limit headers are exposed | response headers | `x-ratelimit-limit: 100`, `x-ratelimit-remaining: 99` (**test key**) |
| 3 | Media schema `file.{hd\|md\|sm\|xs}.{gif\|webp\|mp4}.{url,width,height,size}` | trending item | exactly as predicted |
| 4 | WebP renditions are **animated + alpha** | downloaded `hd.webp`, parsed RIFF/VP8X | `VP8X anim=True alpha=True`, `content-type image/webp` |
| 5 | GIF renditions are real GIF89a | downloaded `hd.gif` | `GIF89a 420x479`, `content-type image/gif` |
| 6 | **Tier name ≠ resolution ladder** | item `hello-july-july-1` | `hd.webp`=498², **`md.webp`=640²** (md larger than hd) |
| 7 | Deep pagination ceiling ≫ Giphy's 499 | `per_page=50`, pages 1→100 | pages **1–50 full (50 each, has_next=true)**; page 100 empty → **≥2,500 items reachable** |
| 8 | `id` is a compact numeric; `gifs/{id}` resolves | `GET gifs/6587795917955715` | `200`, same item as `gifs/{slug}` |
| 9 | Invalid API key → **HTTP 404** (not 401/403) | mangled key | `404 {"result":false,"errors":{"message":["The provided API key is invalid: …"]}}` |
| 10 | Bad slug → HTTP 404, same error envelope | `GET gifs/<bogus>` | `404 {"result":false,"errors":{"message":["404 Not Found"]}}` |
| 11 | Ads are opt-in (absent without ad params) | trending without ad-* params | no `type:"ad"` items returned |
| 12 | Stickers: `type:"sticker"`, webp+gif, **no mp4**, transparent animated | `GET stickers/trending` + byte check | `VP8X anim=True alpha=True` on both samples |
| 13 | Clips: flat `file:{mp4,gif,webp}` + `file_meta`, 320×180 gif/webp | `GET clips/trending` | landscape video moments; decodable but off-ethos |
| 14 | Categories richer than the SDK DTO suggested | `GET gifs/categories` | `{locale, categories:[{category,query,preview_url}]}`, ~35 entries |
| 15 | `blur_preview` is a decodable tiny JPEG data-URI | trending item | `data:image/jpeg;base64,…` (~827 chars) |

---

## 3. The Klipy API — reference

### 3.1 Access model
- **Base URL:** `https://api.klipy.com/api/v1/{API_KEY}/…` — the API key is a **path segment**
  (contrast Giphy/Tenor's `?api_key=`).
- **Keys:** issued at `partner.klipy.com`. **Test key = 100 req/hr** (confirmed via
  `x-ratelimit-limit: 100`); **production key = unlimited** per Klipy's docs.
- **Rate-limit headers:** `x-ratelimit-limit`, `x-ratelimit-remaining` are returned on success (no
  `-reset` header was observed). p3a can read `x-ratelimit-remaining` to **self-throttle** — a
  capability the Giphy integration lacks.
- **ToS:** Klipy's Tenor-migration guide requires visible **"KLIPY" branding/attribution**.
- **CDN:** media served from `static.klipy.com`, path shape
  `…/ii/{32-hex}/{xx}/{yy}/{8-char-token}.{ext}` — the token is **random per format** and **not**
  derivable from the item id/slug.

### 3.2 Endpoints (GIF product; Stickers/Clips/Memes share the shape with a different prefix)

| Method | Path | Purpose |
|---|---|---|
| GET | `gifs/trending?page=&per_page=` | Trending feed |
| GET | `gifs/search?q=&page=&per_page=` | Search |
| GET | `gifs/{id-or-slug}` | Resolve a single item (returns full `file` object) |
| GET | `gifs/categories` | Curated category list |
| GET | `gifs/recent/{customer_id}` | Per-user recents (personalization/ads) |
| POST | `gifs/view/{slug}`  body `{customer_id}` | View tracking (analytics) |
| POST | `gifs/share/{slug}` body `{customer_id}` | Share tracking |
| POST | `gifs/report/{slug}` | Content report |
| DELETE | `gifs/recent/{customer_id}?slug=` | Remove from recents |

**Params:** `q`, `page`, `per_page` (default 24, **min 8, max 50**), `locale`, and `rating`
(`g`/`pg`/`pg-13`/`r`). Pagination is **page-number + `has_next`** (no cursor).

**Products:** `gifs`, `stickers`, `clips`, `memes`, plus an `ads` product. Only **gifs** and
**stickers** are a natural fit for p3a (see §6).

### 3.3 Response envelope
```json
{ "result": true,
  "data": {
    "data": [ /* items */ ],
    "current_page": 1,
    "per_page": 24,
    "has_next": true,
    "meta": { "item_min_width": 80, "ad_max_resize_percent": 10 } } }
```

### 3.4 Item / media schema (GIF & Sticker)
```json
{ "id": 6587795917955715,          // compact numeric — resolvable via gifs/{id}
  "slug": "hello-july-july-1",
  "title": "…",
  "type": "gif",                    // "gif" | "sticker" | "clip" | "ad"
  "tags": [],                       // present but often empty
  "blur_preview": "data:image/jpeg;base64,…",   // decodable tiny JPEG placeholder
  "file": {
    "hd": { "webp": {"url","width","height","size"},
            "gif":  {"url","width","height","size"},
            "mp4":  {"url","width","height","size"} },   // stickers: no mp4
    "md": { … }, "sm": { … }, "xs": { … } } }
```
Single-item resolve additionally returns `total_shares`.

**Important:** the four tiers are **not** a strict resolution ladder — for `hello-july-july-1`,
`md.webp` (640²) is larger than `hd.webp` (498²). **Select renditions by the explicit
`width`/`height` fields, never by tier name.**

Observed sizes for `hello-july-july-1` (bytes):

| tier | webp | gif | mp4 |
|---|---|---|---|
| hd | 498² / 144 KB | 498² / **1.3 MB** | 640² / 436 KB |
| md | 640² / 413 KB | 640² / 716 KB | 640² / 436 KB |
| sm | 220² / 79 KB | 220² / 100 KB | 320² / 108 KB |
| xs | 90² / 20 KB | 90² / 17 KB | 150² / 34 KB |

Note the GIF payloads are up to ~10× the WebP for the same frame — **p3a should prefer `webp`**.

### 3.5 Clip schema (different — flat)
```json
{ "type": "clip", "slug": "good-morning-57", "url": "https://klipy.com/…",
  "file": { "mp4":"…","gif":"…","webp":"…" },                 // flat URL strings, no tiers
  "file_meta": { "mp4": {"width":854,"height":480,"size":…},
                 "gif": {"width":320,"height":180,"size":…},
                 "webp":{"width":320,"height":180,"size":…} } }
```

### 3.6 Error envelope
```json
{ "result": false, "errors": { "message": ["…"] } }
```
- **Invalid API key → HTTP 404** (not 401/403), message `"The provided API key is invalid: […]"`.
- Bad slug → HTTP 404, message `"404 Not Found"`.
- ⚠️ The invalid-key error body **echoes the API key** — p3a must **not** log full error bodies.

---

## 4. How it maps onto p3a

### 4.1 The seam-by-seam mirror (model on `components/giphy/`)

| Seam | Giphy today | Klipy equivalent | Cost |
|---|---|---|---|
| HTTP client | `http_fetch` + cJSON | identical | reuse |
| Component | `giphy_api/cache/download/refresh.c` (~1.6k LOC) | `klipy_*` twins | new, mechanical |
| Channel type | `PS_CHANNEL_TYPE_GIPHY`; ids `giphy_trending`, `giphy_search_<q>` | `PS_CHANNEL_TYPE_KLIPY`; `klipy_trending`, `klipy_search_<q>`, `klipy_category_<c>`, `klipy_stickers` | small |
| Refresh dispatch | `play_scheduler_refresh.c` branch → `giphy_refresh_channel_*()` | add branch → `klipy_refresh_channel_*()` | small |
| Vault | `/sdcard/p3a/giphy/{d0}/{d1}/{id}.{ext}` (FNV-1a → 2×6-bit shards) | `/sdcard/p3a/klipy/…` — **reuse `sd_path_build_sharded()` verbatim** | reuse |
| Config / NVS | `giphy_api_key`, `giphy_cache_size`, `giphy_refresh_interval`, `giphy_rating`, … | `klipy_*` twins (fewer knobs) | small |
| HTTP API | `/config`, `/status` carry `giphy_*`; `channel_stats.giphy_trending` | extend with `klipy_*` | small |
| Web UI | `settings.html#giphy` | `#klipy` tab | small |
| Rate-limit / error latches | 429→10-min cooldown; 401/403→1-hr auth latch; no-key latch; TTL'd on-screen messages | **reuse the machinery**, with the auth-detection change in §4.3 | reuse + 1 tweak |
| Post-id hashing | DJB2 salt `"GIPH"` | DJB2 salt `"KLIP"` | trivial |

### 4.2 The opaque-URL problem and its solution
Giphy's entry model stores a short `giphy_id` and **reconstructs** the CDN URL at play time. Klipy's
CDN URLs are random per-format tokens (§3.1) that cannot be reconstructed, and are ~80 chars — they
do not fit the 64-byte `*_channel_entry_t`. Two clean strategies:

- **(A) Lazy download + re-resolve by id (recommended; mirrors Giphy's lazy model).**
  Store the **8-byte `uint64 id`** in the entry. At first play, `GET gifs/{id}` → read the chosen
  rendition URL → download → cache to the sharded path. Cost: **one lightweight resolve per artwork's
  first play** (then it's on SD forever). Verified: `gifs/{id}` resolves identically to `gifs/{slug}`.
- **(B) Eager download during refresh (no re-resolve ever).**
  The page response already contains the URLs, so download all N files at refresh time. Avoids the
  per-play resolve at the cost of heavier SD/bandwidth per refresh. Fine because refreshes are
  infrequent, but diverges from Giphy's lazy model.

Recommend **(A)** — smallest deviation from the existing flow, trivial 8-byte entry, keeps
on-demand downloads.

### 4.3 What is genuinely *new* work vs Giphy
1. **Auth-failure detection differs.** Klipy returns **HTTP 404** for a bad key, not 401/403. The
   Giphy auth-latch keys off 401/403, so for Klipy p3a must inspect the JSON error envelope
   (`result:false` + `errors.message` containing `"API key is invalid"`) to engage the auth latch.
   Do **not** treat every 404 as auth failure (bad slugs also 404) — match the message.
2. **Id-based re-resolve step** (§4.2, strategy A) — a small new call in the download path.
3. **Rate-limit-header self-throttle (bonus, optional).** Read `x-ratelimit-remaining` and defer
   refresh when low — cheaper than reacting to 429s after the fact.
4. **Don't log full error bodies** (they echo the key).

### 4.4 Rendition selection (per §3.4)
Choose by explicit dimensions, not tier name: prefer the smallest rendition whose `width`/`height`
is ≥ the 720² display (or the largest available if none reach it), format **`webp` first, `gif`
fallback**. In practice `hd`/`md` are the candidates; `md` is sometimes the largest. This replaces
the Giphy `prefer_downsized` heuristic with a straightforward dimension pick.

---

## 5. Gaps & caveats (after live verification)

Most pre-verification gaps are now **closed** (pagination depth, schema, decodability, id-resolve).
What remains:

| Severity | Item | Detail / mitigation |
|---|---|---|
| **Medium** | Auth failure = HTTP 404 | Must parse the error envelope, not the status code (§4.3). |
| **Medium** | Attribution on an ambient display | Klipy ToS wants visible "KLIPY" branding; how that applies to a physical art frame is ambiguous (same ambiguity as Giphy). Clarify with Klipy before shipping. |
| Low | Error bodies echo the API key | Never log full error bodies; log status + a redacted summary. |
| Low | `rating` filter strength unproven | `rating=g/r` are accepted (HTTP 200) but filtering wasn't deep-tested. Default to `g` and confirm on real content before trusting for safety. |
| Low | Production 429 shape unseen | On the test key we saw only the ratelimit headers, never tripped a 429. The Giphy strategy (ignore `Retry-After`, fixed cooldown) ports directly; confirm the 429 body on a production key. |
| Low | No top-tier > `md`/`hd` | Content tops out around 480–640 px. Fine for a 720² display that already upscales Giphy's 200 px renditions; no full-res escape hatch. |
| Info | Clips are 320×180 landscape, licensing-heavy | Decodable but off-ethos (movie/TV moments). Hold (§6). |

---

## 6. Extra features vs Giphy that p3a could adopt

1. **★ Stickers — the standout.** Same item shape as GIFs (`type:"sticker"`), returned by
   `stickers/trending` / `stickers/search`. Byte-verified as **animated + alpha WebP** (VP8X), plus
   a transparent GIF fallback, at pixel-art-friendly sizes (90²–498²). p3a's `animation_decoder`
   already handles animated WebP/APNG with transparency, so once GIF playback works, a **sticker
   channel is nearly free** and delivers a **content type Giphy never gave us** — transparent
   animations to composite over black or over the current artwork. (Design choice: what to composite
   the alpha against.)
2. **Categories browse.** `gifs/categories` returns ~35 curated `{category, query, preview_url}`
   entries (e.g. *love, dance, thumbs up, good morning*), each with a preview GIF URL. This can drive
   curated browse channels (`klipy_category_dance`) with thumbnails — nicer than Giphy's
   trending+search-only UX on a couch-facing device.
3. **`blur_preview` instant placeholder.** Each item carries a tiny base64 **JPEG** data-URI. Since
   p3a has a JPEG decoder, it could show an instant low-res preview while the full file downloads,
   replacing the "Downloading…" text. Pure polish.
4. **Rate-limit-header-aware throttling** (§4.3.3) — a robustness win over Giphy.
5. **Clips / Memes** — Clips decodable (gif/webp 320×180) but landscape + licensing-heavy → hold.
   Memes are static images (JPEG/PNG) → low priority.
6. **Localization** (`locale`) — Klipy leans into localized content; mirror the existing
   `giphy_country_code` precedent.

---

## 7. Effort & recommendation

**Effort** (assumes the Giphy component as the template):

| Deliverable | Estimate |
|---|---|
| Trending + search GIF parity (component, channel type, vault, config, http_api, webui, error/rate handling) | ~2–3 weeks |
| Stickers channel (+ alpha-compositing decision) | +3–5 days |
| Categories browse UI | +1–2 days |

The estimate is *below* a naive "Giphy-parity" number because removing the rendition/CDN-guessing
complexity offsets the new id-resolve + 404-auth-detection work.

**Recommendation.** Klipy is a clean, low-risk build that is technically *simpler* than Giphy in its
worst spot. The case to do it rests on **Stickers** (net-new transparent-animation content) and
**provider resilience** (a Giphy fallback), not on adding another GIF feed. If neither is a current
priority, defer it behind the demo-video / adoption work.

---

## 8. Open decision / next steps

The single question that determines scope: **what's driving the interest — a Giphy
alternative/fallback, the Stickers content type, or just exploring feasibility?**

- **Stickers / fallback →** scope an implementation plan against the seams in §4 (GIF-first, or
  GIF+Stickers).
- **Just exploring →** this document is the answer; no further action needed until it's prioritized.

Remaining pre-build verifications (cheap, do at plan time): confirm `rating=g` filtering on real
content, and capture a production-key 429 body.

---

## Appendix A — raw probe evidence (2026-07-01)

Trending item (`per_page=2`), first result:
```
slug='hello-july-july-1' type='gif' id=6587795917955715
  hd.webp 498x498 143790B   md.webp 640x640 413412B   sm.webp 220x220 79436B   xs.webp 90x90 20290B
  hd.gif  498x498 1305911B  md.gif  640x640 715665B
  hd.mp4  640x640 435860B
  blur_preview: data:image/jpeg;base64,/9j//gART…  (len 827)
```
Pagination probe (`per_page=50`): `pages 1–50 → 50 items each, has_next=true`; `page 100 → 0 items,
has_next=false` ⇒ **≥2,500 reachable items**.

Decodability (downloaded, header-parsed):
```
regular hd.webp  -> VP8X ext anim=True alpha=True   content-type image/webp
sticker hd.webp  -> VP8X ext anim=True alpha=True   content-type image/webp   (both samples)
sticker hd.gif   -> GIF89a 420x479                  content-type image/gif
```
Error shapes:
```
bad slug -> 404  {"result":false,"errors":{"message":["404 Not Found"]}}
bad key  -> 404  {"result":false,"errors":{"message":["The provided API key is invalid: […]"]}}
```

## Appendix B — reference sources
- Klipy docs: <https://docs.klipy.com/> (getting-started, gifs-api, clips-api) — *bot-protected;
  not fetchable by simple HTTP clients.*
- Official demo apps (authoritative response schema): `github.com/KLIPY-com/klipy-android-demo-app`,
  `…/klipy-ios-demo-app`.
- Overview: `github.com/KLIPY-com/Klipy-GIF-API`, Tenor→Klipy migration guide.
- Internal p3a template: `components/giphy/` and the memory note `giphy-rendition-bug`.
