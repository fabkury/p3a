# giphy-click — design research notes

Context dump for a feature called **giphy-click**, researched but not yet
implemented. Reload this file at the start of a future conversation to pick
up where we left off.

## Feature definition

- When the user swipes up on a Giphy artwork (same gesture currently used to
  submit an emoji reaction on a Makapix Club artwork), register an onclick
  action against the Giphy analytics service for that artwork.
- Instead of the current "swipe-up on a non-Makapix artwork shows the error
  icon" behaviour, show `images/submit_click.png` on successful registration.
- Swipe-up on a Giphy artwork where no analytics payload is available still
  shows the error icon (graceful fallback).
- Swipe-down on a Giphy artwork: unchanged (error icon). Giphy has no
  "revoke" action to register.
- `images/submit_click.png` must be embedded in the firmware the same way the
  other reaction icons are (see `scripts/png2blit.py`, and the existing
  `main/reaction_{submit,revoke,error}_img.{c,h}`).

## User-locked design decisions

1. **No onload pings.** Giphy beta API keys are 100 requests/hour and users
   supply their own keys. A ping per display at the 30 s swap cadence would
   exhaust the quota. Only onclick is registered.
2. **In-memory-only analytics state is acceptable.** Channel refresh runs
   every 1/2/4/8 hours (user may drop 8 h to cap at 4 h), which is frequent
   enough that losing the analytics map across reboots is fine.
3. **Swipe-down on Giphy posts keeps the error icon.**
4. **Missing analytics payload → error icon** (graceful fallback).

## How Giphy's Action Register API works

### Analytics Object shape

Every GIF object returned by most endpoints carries an `analytics` field:

```json
"analytics": {
  "onload":  { "url": "https://giphy-analytics.giphy.com/v2/pingback_simple?analytics_response_payload=<opaque-base64>&action_type=SEEN"  },
  "onclick": { "url": "https://giphy-analytics.giphy.com/v2/pingback_simple?analytics_response_payload=<opaque-base64>&action_type=CLICK" },
  "onsent":  { "url": "https://giphy-analytics.giphy.com/v2/pingback_simple?analytics_response_payload=<opaque-base64>&action_type=SENT"  }
}
```

There is **also** a top-level `analytics_response_payload` field holding the
raw opaque base64 string (same payload embedded in all three URLs).

### Firing an onclick

1. Take `data.analytics.onclick.url` from the API response.
2. Append `&random_id=<rid>&ts=<unix_ms>`.
3. Issue `GET` to the resulting URL. Success = HTTP 200 with
   `{"status":200}` (~14 bytes).

The payload is **opaque and pre-signed server-side** — the client cannot
synthesize it from the GIF id. It must be obtained from an API response.

`random_id` is a per-device pseudonymous id from `/v1/randomid`. p3a already
handles this — see `giphy_fetch_random_id()` in
`components/giphy/giphy_api.c` and the NVS accessor
`config_store_get_giphy_random_id()`.

### Rate limits

- 100 requests/hour on `api.giphy.com` for beta keys (all p3a users).
- **No rate-limit headers** are returned — you only learn you're over quota
  when calls start returning HTTP 429.
- Pingbacks to `giphy-analytics.giphy.com` almost certainly **do not** count
  against the 100/hr quota (separate host, heavy CDN, all Giphy SDKs fire
  them freely). Not formally documented, but empirically safe to assume.

## Relevant existing p3a code

### Giphy component (`components/giphy/`)

- `giphy_types.h` — `giphy_channel_entry_t` is **64 bytes packed** with a
  `_Static_assert`; the 26-byte `reserved` tail has no room for a ~300 B URL.
- `giphy_api.c::parse_gif_object` — parses each GIF from the `data[]` array;
  currently only extracts id/dimensions/timestamp, not analytics.
- `giphy_api.c::giphy_fetch_page` — uses `&fields=id,trending_datetime,...`
  to trim the response; would need `analytics` (or the shorter
  `analytics_response_payload`) added to the field list.
- `giphy_api.c::giphy_fetch_random_id` — already implemented.
- `giphy_refresh.c` — page-by-page fetch + merge + evict loop; owns the
  refresh lifecycle. Where analytics state would be rebuilt if we chose a
  warm-cache variant.

### Touch / reaction path

- `components/p3a_core/p3a_touch_router.c::handle_animation_playback` —
  `P3A_TOUCH_EVENT_SWIPE_UP` currently checks
  `p3a_current_post_get_source() != REACTION_POST_SOURCE_MAKAPIX` and shows
  the error icon. That's exactly the branch giphy-click modifies.
- The same file contains `reaction_mqtt_task` — the template for our
  fire-and-forget background-task pattern (shows optimistic icon, spawns
  task, overrides with error icon on failure).
- `main/display_reaction_overlay.c` — owns the overlay state machine with
  `REACTION_OVERLAY_{IDLE,SUBMIT,REVOKE,ERROR}`. Adding a new state for the
  click icon is the shape of the UI change.
- `components/p3a_core/include/p3a_current_post.h` — the post source enum
  includes `GIPHY = 2` (per the header comment), so
  `p3a_current_post_get_source()` already returns the value we need to
  detect "current post is a Giphy GIF".

### PNG embedding

- `scripts/png2blit.py input.png --name <name> --outdir main` generates
  `main/<name>.c` + `main/<name>.h`.
- Add the `.c` to `main/CMakeLists.txt` (see lines ~19–21 for the existing
  reaction images).
- `images/submit_click.png` and `.xcf` already exist (user committed the
  source art).

## Three design alternatives considered

### (A) In-memory sidecar

PSRAM `UT_hash` keyed by `post_id` → onclick payload/URL, rebuilt during each
refresh, cleared on reboot.

**Pros:** zero cache-format impact (64 B invariant preserved); no SD I/O
change; natural lifecycle match (Giphy entries only enter cache via refresh,
which also knows the payloads); cheap eviction sync; small footprint (~75 KB
at default 256 entries, ~1 MB at max 4096); clean atomic swap on refresh.

**Cons:** post-reboot blind window (swipes show error until next refresh —
user accepted this); memory lifecycle must stay synced with merge/evict
(mitigated by per-refresh arena + pointer swap); opaque payload TTL unknown
(refresh cadence likely shorter than any rotation); adds ~15–20 KB JSON per
page; not inspectable from disk.

### (B) Persisted sidecar

Mirror file on SD next to the channel cache. Rejected — user is fine with
in-memory-only, and this doubles the I/O failure modes.

### (C) Extend the 64 B struct

Requires cache-format migration and breaks makapix interop. Rejected.

### (L1) Pure lazy (no sidecar)

Every swipe-up does:

1. `GET /v1/gifs/<giphy_id>?api_key=<k>&fields=id,analytics&random_id=<rid>`
   → parse `data.analytics.onclick.url`.
2. `GET <onclick_url>&random_id=<rid>&ts=<unix_ms>` → 200.

**Pros:** simplest code (~100 lines vs. ~300+ for A); zero PSRAM footprint;
no refresh integration; solves post-reboot blind window symmetrically (no
state to lose); unchanged for any future code path that puts a GIF on
screen.

**Cons:** every click costs 1 API-key quota unit; harder floor on latency
(extra round-trip before the pingback); every click pays the cost, even
those that could have been cached.

### (L2) Hybrid — warm cache + lazy fallback

Keep the A-style hash, fall through to the L1 path on miss. Cost: union of
A's complexity and L1's code path, for a marginal gain on a case the user
accepts losing.

## API verification tests (executed against user's beta key)

All queries hit real Giphy servers. Key redacted; GID = `Qr04BtCDTF7aM` (a
trending GIF at test time).

| Test | Result |
|------|--------|
| `GET /v1/gifs/trending?limit=1` returns `analytics` with `onload`/`onclick`/`onsent` | ✅ |
| `GET /v1/gifs/<id>` (no field filter) returns `analytics` + top-level `analytics_response_payload` | ✅ |
| `GET /v1/gifs/<id>?fields=id,analytics` trims response to ~1.0 KB, analytics present | ✅ |
| `GET /v1/gifs?ids=<id>&fields=id,analytics` (bulk) returns analytics, ~1.1 KB per GIF | ✅ |
| Real onclick pingback (`onclick.url + &random_id + &ts`) → HTTP 200, body `{"status":200}` | ✅ |
| Pingback URL total length (with `random_id` + `ts` appended) | 310 chars |
| Rate-limit / quota-remaining headers on `api.giphy.com` or `giphy-analytics.giphy.com` | **none** |

Example trimmed response shape (`fields=id,analytics`):

```json
{"data":{
  "id":"Qr04BtCDTF7aM",
  "analytics_response_payload":"<~180-char-base64>",
  "analytics":{
    "onload":{"url":"https://giphy-analytics.giphy.com/v2/pingback_simple?analytics_response_payload=e%3D...&action_type=SEEN"},
    "onclick":{"url":"...&action_type=CLICK"},
    "onsent":{"url":"...&action_type=SENT"}
  }
}}
```

## Current recommendation

**Pure lazy (L1)** is the tentative choice. It is viable (test-verified),
strictly simpler than (A), has zero PSRAM footprint, no refresh-path
changes, and solves the post-reboot case for free. The per-click quota cost
is real but a casual user is nowhere near the 100/hr cap.

Alternative (A) remains the fastest-UX option if we ever want to avoid the
second round-trip; the stored structure would be a per-refresh PSRAM arena
holding `analytics_response_payload` strings (not full URLs), with a
`UT_hash` of `post_id → (offset, len)`.

## Open items for the next session

- User approval on L1 vs. A. (Last message: user was leaning L1; design
  recommendation was L1.)
- Plan the implementation:
  - Add `analytics` (or `analytics_response_payload`) to the `&fields=`
    list in `giphy_fetch_page` **only if** we go with (A). L1 does not
    modify the refresh path at all.
  - Run `scripts/png2blit.py images/submit_click.png --name submit_click_img --outdir main`
    and add the generated `.c` to `main/CMakeLists.txt`.
  - Extend `display_reaction_overlay.c`: new `REACTION_OVERLAY_CLICK`
    state and `reaction_overlay_show_click()` function.
  - In `p3a_touch_router.c::handle_animation_playback`'s `SWIPE_UP` case:
    detect `POST_SOURCE_GIPHY` and branch to the giphy-click handler;
    keep existing error-icon fallback for all failure modes.
  - New short-lived task analogous to `reaction_mqtt_task`, using
    `esp_http_client` with `esp_crt_bundle_attach` (same pattern as
    `giphy_fetch_random_id`). For L1, two sequential `GET`s; for A, one.
  - Confirm `random_id` is available at click time (it's persisted in
    NVS; `config_store_get_giphy_random_id` is the accessor).
- Decide behaviour on HTTP 429 from `api.giphy.com` (quota exhausted):
  error icon is the obvious choice; a soft cool-down could also prevent
  hammering.
