# Single-artwork preview for the museum browse modal

- **Status:** Draft (this spec)
- **Date:** 2026-05-12
- **Owner:** pub@kury.dev
- **Scope:** Web-UI only. Replaces the existing 8-thumbnail grid preview in
  the playset-editor's museum browse modal with a single-artwork preview
  navigable via Previous / Next buttons.
- **Related:** `docs/art-institutions/finalized-design.md` §1, §7.1 (which
  this change supersedes for the preview surface).

## 1. Motivation

The current preview step (`webui/museum/browse.js`,
`renderPreviewStep()`) fetches up to 8 artworks for the picked
(museum, axis, term) selection and renders them as a 4×2 grid of
64 × 64 IIIF thumbnails. Field experience surfaced two problems:

1. **Latency.** Fetching 8 items can be visibly slow. For Rijks it is
   worse than the user sees today — the modal currently *skips* image
   resolution for Rijks because each thumbnail requires a 3-hop
   Linked-Art walk, and 8 walks are too expensive. The Rijks preview
   today falls back to a textual card list with no images at all
   (`webui/museum/rijksmuseum.js`, `thumbnailUrl()` returns `null`).
2. **Mobile viewing.** 64 × 64 thumbnails in a 4-column grid inside a
   ≤560 px modal are too small to actually see the artwork. The user
   ends up making the selection blind.

Both issues collapse if the preview shows a single artwork at a time
at a larger size. Loading one image is fast, and at ~400 px the user
can actually evaluate whether this category is what they want.

## 2. Non-goals

- No changes to the device-side refresh, download, or playback paths.
- No changes to channel persistence or REST endpoints.
- No changes to the museum, axis, or term selection steps.
- No on-device caching of preview images.
- No keyword search, "favorite this artwork" controls, or per-artwork
  add (the user is still adding the *channel*, not an artwork).

## 3. UX

The preview step (the 4th screen in the browse flow:
museum → axis → terms → **preview**) is restructured as a single
column:

```
┌──────────────────────────────────┐
│  ← Back              Browse… ×   │  (existing header)
├──────────────────────────────────┤
│  AIC · Departments · Painting    │  (existing crumbs)
├──────────────────────────────────┤
│                                  │
│          [ artwork image ]       │  ~400×400 IIIF rendition
│                                  │
│  The Bedroom                     │  title
│  Vincent van Gogh · 1889         │  artist · date (omitted if empty)
│                                  │
│  [ ← Previous ]  1 / 1234        │  index/total counter
│                  [ Next → ]      │
│                                  │
│  ───────────────────────────     │
│  Painting · 1234 artworks        │  (existing add-row label)
│                            [Add] │  (existing Add button)
└──────────────────────────────────┘
```

Behaviors:

- The image is rendered at the IIIF size `!400,400` (longest-side
  ≤ 400 px). It scales responsively within the modal's
  `max-width: 560px`.
- Caption shows artwork `title` (always), then `artist · date`
  (rendered only when at least one is non-empty).
- Index counter shows `N / total` when `total` is known; falls back to
  `N` otherwise (Rijks reports `total` only after the first
  OrderedCollectionPage is fetched, which it always is).
- Previous is disabled at `index === 0`.
- Next is disabled when:
  - The current item is the last item the museum will yield (we have
    walked off the end of results), **or**
  - For AIC, `index >= 999` — the public-tier `from + size ≤ 1000`
    cap (see `docs/art-institutions/finalized-design.md` §9.1 /
    §15.1) means the 1000th artwork is the highest reachable
    offset.
- The "Add channel" button continues to commit the **channel**
  (museum + axis + term), not the currently-visible artwork. Its
  meaning is unchanged from today.

### 3.1 Loading state

When `index` changes and the next image must be fetched:

- The current image stays visible at `opacity: 0.4`, with a small
  spinner badge overlaid in the corner of the image slot.
- Previous and Next are disabled until the new image's
  `load` or `error` event fires.
- On `load`: opacity returns to 1.0, spinner removed, buttons
  re-enabled (subject to the rules in §3).

This avoids a layout flash and signals progress without resetting
the slot to a blank placeholder.

### 3.2 Per-artwork failure

If a single artwork cannot be displayed (image 404, IIIF rendition
unavailable, or Rijks Linked-Art walk fails), the image slot is
replaced with a placeholder tile:

```
  ┌──────────────────────────┐
  │                          │
  │   ⚠ couldn't load        │
  │     this artwork         │
  │                          │
  └──────────────────────────┘
```

The caption still renders if `title` is known. Previous and Next
remain enabled so the user can move past the failure — one bad entry
must not block the whole flow. A preview-time failure does not affect
what gets persisted on "Add channel": the channel still commits as
(museum, axis, term). The device performs its own resolution and
download independently and applies its own retry / tombstone logic
(§7.3, §11 of finalized-design.md).

## 4. Data flow

### 4.1 State

`renderPreviewStep()` owns a sub-state attached to the modal's existing
`state` object:

```js
state.preview = {
    index:       0,      // 0-based current artwork (into items[])
    items:       [],     // accumulated FILTERED items returned by listArtworks
    total:       null,   // result.total from the first listArtworks() call
    nextOffset:  0,      // offset to pass to the next listArtworks() call
                         //   — advances by PAGE_SIZE per fetch, NOT by items.length
                         //   (AIC and V&A filter out items lacking image_id,
                         //   so items.length can be less than pages fetched ×
                         //   PAGE_SIZE; if we used items.length as the offset,
                         //   the next fetch could land on the same API page).
    cursorEnd:   false,  // true once we know there are no more items to fetch
    loading:     false,  // true during a fetch or image load
    error:       null,   // last fatal fetch error (non-429)
    renderToken: 0,      // monotonic counter for stale-render guard (§4.5)
}
```

A module-level constant `PAGE_SIZE = 20` controls how many items are
fetched per API round-trip. The choice balances first-paint latency
(small page = fast first render) against round-trip frequency
(small page = more Next clicks trigger fetches). 20 is small enough
to be quick on every museum surveyed, large enough that a user who
clicks Next 5–10 times stays within one page.

### 4.2 Initial entry

When the user picks a term and `renderPreviewStep()` runs:

1. Reset `state.preview` to defaults (`nextOffset = 0`).
2. Render the layout shell (image placeholder + caption skeleton +
   nav row + add row) with `loading: true`.
3. Call `adapter.listArtworks(termId, { offset: 0, rows: PAGE_SIZE, axis })`.
4. On success: store `items` and `total`. Set `nextOffset = PAGE_SIZE`.
   Decide initial `cursorEnd` using the §4.3 step-3 rules. Set
   `index = 0`.
5. Resolve `previewUrl` for `items[0]` (see §4.4) and set the image
   `src`. Render caption.
6. On failure: existing 429 cooldown branch unchanged. Other errors
   render the existing "Failed to load preview." status text.

If step 4 returns zero items, the existing "No previewable artwork.
Channel may be empty." message is shown (with the Add button still
present — same as today; the user may still add an empty channel,
though the device will inactivate it on refresh).

### 4.3 Navigation

**Next click**:
1. If `state.preview.index + 1 < items.length`: just bump `index`,
   render image + caption for the new item. No fetch.
2. Else if `cursorEnd === true`: no-op (button should be disabled
   already; this is a defensive check).
3. Else: set `loading = true`, disable buttons, call
   `listArtworks(termId, { offset: nextOffset, rows: PAGE_SIZE, axis })`.
   Append the returned items to `items` (note: may be fewer than
   PAGE_SIZE if the adapter filtered out items lacking an image, or
   if the cursor is exhausted). Set `nextOffset += PAGE_SIZE`.
   Decide end-of-stream:
   - If `total` is known and `nextOffset >= total`: set `cursorEnd = true`.
   - Else if returned slice was empty: set `cursorEnd = true`.
   - Else if museum is AIC and `nextOffset >= 1000`: set `cursorEnd = true`
     (public-tier `from + size ≤ 1000` cap; see §15.1 of
     finalized-design.md).
   Then bump `index` to the first newly-appended item. Render.
   If the fetch returned an empty slice and `items` is still empty,
   we have a dead end — render the existing "No previewable artwork"
   message and disable Next.

**Previous click**:
- Always step 1 (we never evict from `items`). Bump `index` down,
  render.

**Note on AIC's 1000-cap.** Public AIC callers cannot fetch records
past offset 999. The cap is enforced on `nextOffset` (cleaner than
on `index`, which counts filtered items): once `nextOffset >= 1000`,
no further fetches are attempted. The user can still navigate
forward through any items already in `items` array; Next disables
when `index + 1 >= items.length` AND `cursorEnd === true`.

### 4.4 Adapter changes — `previewUrl(item, size)`

A new per-adapter method, returning `Promise<string | null>`:

```js
// In webui/museum/artic.js, vam.js:
async previewUrl(item, size) {
    if (!item || !item.imageId) return null;
    return this.thumbnailUrl(item.imageId, size);
}

// In webui/museum/rijksmuseum.js:
async previewUrl(item, size) {
    if (!item || !item.id) return null;
    if (this._resolvedThumbs.has(item.id)) {
        return this._resolvedThumbs.get(item.id); // may be null on cached failure
    }
    try {
        const micrioId = await this._resolveLinkedArt(item.id); // 3-hop walk
        const url = micrioId
            ? `https://iiif.micr.io/${encodeURIComponent(micrioId)}/full/!${size},${size}/0/default.jpg`
            : null;
        this._resolvedThumbs.set(item.id, url);
        return url;
    } catch (_) {
        this._resolvedThumbs.set(item.id, null); // cache failure
        return null;
    }
}

// _resolveLinkedArt is new. Walks HMO -> VisualItem -> DigitalObject ->
// access_point, parses each JSON-LD response, and returns the micrio
// short id (last path segment of the access_point URL). Throws on any
// hop that fails or returns an unexpected shape. Reference for the
// expected JSON shape: components/art_institution/museums/rijksmuseum.c
// and finalized-design.md §9.2.
```

The cache (`_resolvedThumbs: Map<string, string | null>`) lives on
the adapter instance. Within one modal session, the user who walks
forward and then back never triggers a second walk for the same
artwork. The cache is intentionally not persisted across modal
sessions — `ADAPTERS` are instantiated once per page load
(`webui/museum/index.js`), so a soft refresh of the playset editor
resets it, which is fine.

`thumbnailUrl()` is kept on all adapters. It's still useful for
future surfaces (e.g. a hypothetical now-playing strip), and the
Rijks `thumbnailUrl()` continues to return `null` synchronously
since callers that aren't prepared to `await` shouldn't get an
unresolved promise.

### 4.5 Rendering a preview slot

`renderPreviewSlot(item)`, internal to `browse.js`:

1. Set image `src` to a 1×1 transparent placeholder, opacity 0.4.
2. Render caption block from `item.title`, `item.artist`, `item.date`.
3. `await state.adapter.previewUrl(item, 400)`.
4. If `null`: show the failure placeholder (§3.2), set `loading: false`,
   re-enable buttons (subject to §3 rules). Return.
5. Else: set image `src` to the resolved URL. Attach one-shot `load` /
   `error` handlers that clear `loading`, swap opacity, re-enable
   buttons, or (on `error`) swap in the failure placeholder.

If the user clicks Next while a previous slot is still loading, the
old handlers are detached (or guarded by a stale-token check) so they
don't fight the new render. The simplest way is a monotonically
increasing `renderToken` on `state.preview` — handlers check it before
mutating DOM. This matters because the network is slow and the
image-load can outlast the user's patience.

## 5. Files touched

| File | Change |
|---|---|
| `webui/museum/browse.js` | Rewrite `renderPreviewStep()` (currently lines 299-374) to render the single-image layout and own `state.preview`. Add internal helpers `renderPreviewSlot`, navigation handlers, render-token guard. CSS: remove `.mb-thumbs`, `.mb-textcards`, `.mb-textcard*` rules; add `.mb-preview`, `.mb-preview-img`, `.mb-preview-fail`, `.mb-preview-meta`, `.mb-preview-meta-title`, `.mb-preview-meta-sub`, `.mb-nav`, `.mb-nav button`, `.mb-nav .mb-index`. |
| `webui/museum/artic.js` | Add `async previewUrl(item, size)` returning the IIIF JPEG URL or null. |
| `webui/museum/vam.js` | Same — `async previewUrl(item, size)`. |
| `webui/museum/rijksmuseum.js` | Add `async previewUrl(item, size)` performing the 3-hop Linked-Art walk (`HMO → VisualItem → DigitalObject → access_point`, per `finalized-design.md` §9.2). Add `_resolveLinkedArt(hmoId)` helper. Add `_resolvedThumbs` Map cache on the adapter instance. (The device-side equivalent walk lives in `components/art_institution/museums/rijksmuseum.c`; the browser implementation is independent but must agree on the JSON shape — same Accept header, same hop sequence.) |
| `docs/art-institutions/finalized-design.md` | Update §1 (drop the "64×64 thumbnail previews of the first 8 artworks" line, replace with single-artwork preview) and §7.1 step 5 to describe the new preview shape. Add a §15 entry noting the UX-change rationale. |

No C-side / firmware changes. No new HTTP endpoints. No NVS settings.

## 6. Testing

Manual, matching the existing testing posture for art-institution work.

1. **AIC — page boundary.** Add an AIC Painting channel. From the
   preview, click Next 25 times. Confirm the 20→21 transition
   triggers a fetch (visible in network tab) and that subsequent
   clicks within the new page are instant. Confirm caption and image
   update on every click.
2. **AIC — 1000-cap.** Navigate to index 999 (use a high-count term
   like Painting). Confirm Next disables. Click Previous → enabled.
3. **AIC — small term.** Pick a term with < 5 artworks. Confirm
   counter shows `1 / N`, Next disables at the last item.
4. **V&A.** Add a V&A channel (use `collection` axis). Navigate
   forward/back. Confirm parity with AIC behavior.
5. **Rijks — happy path.** Add a Rijks set channel. Confirm the
   image renders within a few seconds (3-hop walk). Click Next.
   Confirm subsequent walks happen, image swaps.
6. **Rijks — back-and-forth cache.** Walk forward 3 items, then back
   to item 0. Confirm no new walk (cache hit) — image renders
   instantly.
7. **Rijks — walk failure.** Identify an HMO whose walk fails (e.g.
   pull the network mid-walk via DevTools throttling, or pick a
   known-broken set entry). Confirm placeholder renders, Prev/Next
   still work, can move past it.
8. **AIC — 429 cooldown.** Trigger a 429 during navigation (run
   several rapid Next clicks or use a low-budget test account if
   available). Confirm the existing cooldown banner appears.
9. **Mobile viewport.** In Chrome DevTools mobile emulation (iPhone
   12 / 390×844), open the modal, reach the preview step. Confirm
   the image fits, caption is readable, Prev/Next buttons are
   reachable with one thumb, "Add channel" is visible without
   excessive scrolling.
10. **Channel commits correctly.** From the preview at index 5,
    click "Add channel". Confirm the playset gains a channel for
    the picked (museum, axis, term) — not for the specific artwork
    at index 5. The committed spec must be identical to what the
    same selection produced before this change.

## 7. Implementation order

1. `previewUrl` method on AIC and V&A adapters (trivial pass-through).
2. `previewUrl` method on Rijks adapter (cache + walk).
3. Rewrite `renderPreviewStep()` in `browse.js` and replace CSS rules.
4. Manual test pass per §6.
5. Update `docs/art-institutions/finalized-design.md`.
6. Commit.

## 8. Open risks

- **Rijks walk latency on slow networks.** A 3-hop walk on a mobile
  connection can take several seconds. We accept this — it's the
  Linked-Art reality and the cache makes repeat visits free. If
  measurements show the median walk exceeds ~5s, we revisit (e.g.
  parallelize the hops where they don't have data dependencies, or
  add a "skip Rijks images, show captions" preference). Not in scope
  here.
- **Image-load timeouts.** A stalled `<img>` load could leave buttons
  disabled forever. Mitigation: implement a 15 s timeout that
  triggers the same flow as the `error` handler (placeholder + buttons
  re-enabled).
- **Render-token correctness.** If the guard is implemented sloppily,
  fast Next-clicks could leak DOM updates from stale loads. Test #1
  exercises this; if needed, an `AbortController` for the fetch +
  setting `img.src = ''` on tokenization can harden it further.
