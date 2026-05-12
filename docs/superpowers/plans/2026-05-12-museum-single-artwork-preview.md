# Single-artwork preview for museum browse modal — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 8-thumbnail grid preview in the playset-editor's museum browse modal with a single-artwork preview navigable via Previous / Next buttons.

**Architecture:** Web-UI only. `renderPreviewStep()` in `webui/museum/browse.js` is rewritten to render one artwork at a time, fetching additional pages on demand via `adapter.listArtworks({offset, rows})` and resolving per-artwork preview URLs via a new `adapter.previewUrl(item, size)` method that each adapter implements. AIC / V&A return the IIIF URL synchronously; Rijks performs a 3-hop Linked-Art walk (HMO → VisualItem → DigitalObject → access_point) with per-adapter caching.

**Tech Stack:** Vanilla ES modules, no build step; fetch API; IIIF Image API v2 size syntax. Tests are manual (this project has no JS test framework; the existing pattern is browser-based verification on the running device).

**Spec:** `docs/superpowers/specs/2026-05-12-museum-single-artwork-preview-design.md`

---

## Testing posture

This codebase has no JS test framework. The existing `webui/museum/*` files were verified manually via the device's served UI. Plan tasks therefore use **manual verification steps** in place of unit-test commands. Each verification step describes exactly what to do in the browser (DevTools open) and what to observe. The user is responsible for builds and on-device testing per `CLAUDE.md`.

When a code change has no behavior change yet (e.g. adding an unused adapter method), the verification step is "no behavioral change — confirm the page still loads with no console errors."

---

## File map

| File | Action | Responsibility |
|---|---|---|
| `webui/museum/artic.js` | Modify | Add `async previewUrl(item, size)` returning the IIIF JPEG URL for `item.imageId`, or null. |
| `webui/museum/vam.js` | Modify | Same shape as AIC. |
| `webui/museum/rijksmuseum.js` | Modify | Add `async previewUrl(item, size)` performing the 3-hop Linked-Art walk. Add `_resolveLinkedArt(hmoUrl)` helper. Add `_resolvedThumbs` Map for per-session caching. |
| `webui/museum/browse.js` | Modify | Rewrite `renderPreviewStep()`. Replace `.mb-thumbs` / `.mb-textcards` CSS with single-preview CSS. Add `PAGE_SIZE` constant, `state.preview` substate, navigation handlers, render-token guard. |
| `docs/art-institutions/finalized-design.md` | Modify | §1 (drop "64×64 thumbnail previews of the first 8 artworks" wording, replace with single-artwork preview), §7.1 step 5, new §15 entry noting the UX-change rationale. |

No new files. No C-side changes. No new HTTP endpoints.

---

## Task 1: Add `previewUrl()` to AIC and V&A adapters

Trivial pass-through — these museums return `imageId` inline from `listArtworks`, so `previewUrl` just calls the existing `thumbnailUrl()` (which is already the IIIF builder, parameterized by size).

**Files:**
- Modify: `webui/museum/artic.js` (around line 178, before the closing brace of the class — after `thumbnailUrl()`)
- Modify: `webui/museum/vam.js` (around line 161, same position)

- [ ] **Step 1: Add `previewUrl` to `webui/museum/artic.js`**

Insert this method on the `ArticAdapter` class, immediately after the existing `thumbnailUrl(imageId, size = 64)` method:

```js
    // Single-artwork preview URL. AIC has the IIIF id inline in
    // listArtworks output, so this is a synchronous thumbnailUrl with a
    // larger size parameter — same JPEG, different rendition.
    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }
```

- [ ] **Step 2: Add `previewUrl` to `webui/museum/vam.js`**

Insert this method on the `VamAdapter` class, immediately after the existing `thumbnailUrl(imageId, size = 64)` method:

```js
    // Single-artwork preview URL. V&A returns _primaryImageId inline,
    // so this is a synchronous thumbnailUrl with a larger size.
    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }
```

- [ ] **Step 3: Manual verification (no behavior change yet)**

Reload the playset editor in a browser pointed at the device. Open the museum browse modal. Walk to the preview step for an AIC channel and for a V&A channel. The existing 8-thumbnail grid should still render exactly as before (no caller invokes `previewUrl` yet). No console errors.

Expected: identical UX to pre-change.

- [ ] **Step 4: Commit**

```powershell
git add webui/museum/artic.js webui/museum/vam.js
git commit -m "webui/museum: add previewUrl() to AIC and V&A adapters"
```

---

## Task 2: Add `previewUrl()` and Linked-Art walk to Rijks adapter

The Rijks adapter currently returns `null` from `thumbnailUrl()` because the IIIF id isn't in `listArtworks` output — it requires the 3-hop Linked-Art walk. This task adds the walk plus a session-scoped cache.

**Files:**
- Modify: `webui/museum/rijksmuseum.js`

Walk shape (mirrors `components/art_institution/museums/rijksmuseum.c` `art_institution_rijks_resolve_entry()`):

```
Hop 1: HMO URL                      → JSON-LD object with `shows: [{id, type}, ...]`
Hop 2: each shows[i].id (VisualItem) → JSON-LD with `digitally_shown_by: [{id, type}, ...]`
Hop 3: each digitally_shown_by[j].id (DigitalObject) → JSON-LD with `access_point: [{id, type}, ...]`

Look in access_point for an `id` starting with `https://iiif.micr.io/`.
The micrio short id is everything between that prefix and the next slash (or end of URL).
```

- [ ] **Step 1: Update the file header comment**

In `webui/museum/rijksmuseum.js`, replace the existing thumbnail-explanation block (lines 14-20) with updated wording. Find:

```js
// Per-artwork thumbnails are intentionally NOT pre-resolved here. The
// IIIF id is reachable only via a 3-hop Linked-Art walk
// (HMO -> VisualItem -> DigitalObject -> access_point), which would
// cost 8 * 3 = 24 HTTP requests to populate the modal's preview strip.
// listArtworks() instead returns items without imageId; the modal
// falls back to a textual card layout. The device does the walk lazily
// at download time (components/art_institution/museums/rijksmuseum.c).
```

Replace with:

```js
// Per-artwork IIIF resolution requires a 3-hop Linked-Art walk:
//   HMO -> VisualItem -> DigitalObject -> access_point
// listArtworks() returns items without imageId so it doesn't pay 8x3 =
// 24 requests up front. previewUrl() does the walk lazily for the one
// currently-displayed item in the preview modal and caches the result
// per adapter instance. The device does the same walk independently at
// download time (components/art_institution/museums/rijksmuseum.c).
```

- [ ] **Step 2: Add the micrio prefix constant**

Find the existing constants near the top of the file:

```js
const BASE = 'https://data.rijksmuseum.nl';
const ID_PREFIX = 'https://id.rijksmuseum.nl/';
// The bundled sets file is served by the device's HTTP server at the
// path below (the route mirrors /pico8/* and /static/*).
const SETS_URL = '/museum/rijks-sets.json';
```

Add immediately after:

```js
const MICRIO_PREFIX = 'https://iiif.micr.io/';
```

- [ ] **Step 3: Add `_resolvedThumbs` cache to the constructor**

Find the existing constructor:

```js
    constructor() {
        this._sets = null;
        this._counts = Object.create(null);
        // setSpec -> { items: [...], cursors: [url, ...], next: url|null }
        this._cursors = Object.create(null);
    }
```

Add one line at the end:

```js
    constructor() {
        this._sets = null;
        this._counts = Object.create(null);
        // setSpec -> { items: [...], cursors: [url, ...], next: url|null }
        this._cursors = Object.create(null);
        // HMO URL -> resolved micrio short id, or null on cached failure.
        // Scoped to one adapter instance (= one page load).
        this._resolvedThumbs = new Map();
    }
```

- [ ] **Step 4: Add `_resolveLinkedArt(hmoUrl)` and `previewUrl(item, size)` methods**

Insert these two methods immediately after the existing `thumbnailUrl(_imageId, _size)` method (currently the last method in the class, around line 175). The class's closing brace is right after `thumbnailUrl`:

```js
    // 3-hop Linked-Art walk. Returns the micrio short id on success, or
    // null if no micrio access_point is reachable. Throws on network
    // errors at hop 1 (the failure is treated as transient and not
    // cached). Hops 2 and 3 are best-effort — a failure on any VisualItem
    // or DigitalObject just continues to the next sibling.
    async _resolveLinkedArt(hmoUrl) {
        const hmo = await fetchJsonLd(hmoUrl);
        const shows = (hmo && Array.isArray(hmo.shows)) ? hmo.shows : [];
        for (const s of shows) {
            if (!s || typeof s.id !== 'string' || !s.id) continue;
            let visual;
            try { visual = await fetchJsonLd(s.id); }
            catch (_) { continue; }
            const digi = (visual && Array.isArray(visual.digitally_shown_by))
                ? visual.digitally_shown_by : [];
            for (const d of digi) {
                if (!d || typeof d.id !== 'string' || !d.id) continue;
                let digital;
                try { digital = await fetchJsonLd(d.id); }
                catch (_) { continue; }
                const ap = (digital && Array.isArray(digital.access_point))
                    ? digital.access_point : [];
                for (const a of ap) {
                    if (!a || typeof a.id !== 'string') continue;
                    if (!a.id.startsWith(MICRIO_PREFIX)) continue;
                    const rest = a.id.slice(MICRIO_PREFIX.length);
                    const slash = rest.indexOf('/');
                    const key = slash >= 0 ? rest.slice(0, slash) : rest;
                    if (key) return key;
                }
            }
        }
        return null;
    }

    // Single-artwork preview URL. Walks Linked Art to resolve the IIIF
    // id, caches the result (success or failure) per item.id. Returns
    // a string URL or null.
    async previewUrl(item, size = 400) {
        if (!item || !item.id) return null;
        if (this._resolvedThumbs.has(item.id)) {
            const cached = this._resolvedThumbs.get(item.id);
            return cached
                ? `${MICRIO_PREFIX}${encodeURIComponent(cached)}/full/!${size},${size}/0/default.jpg`
                : null;
        }
        let micrioId;
        try {
            micrioId = await this._resolveLinkedArt(item.id);
        } catch (_) {
            // Hop-1 failure: don't cache (could be transient network).
            return null;
        }
        this._resolvedThumbs.set(item.id, micrioId || null);
        if (!micrioId) return null;
        return `${MICRIO_PREFIX}${encodeURIComponent(micrioId)}/full/!${size},${size}/0/default.jpg`;
    }
```

(`thumbnailUrl()` keeps returning `null` — unchanged.)

- [ ] **Step 5: Manual verification — Linked-Art walk works**

Reload the playset editor in a browser pointed at the device. Open DevTools (Network tab). Run this in the Console:

```js
const m = (await import('/museum/index.js'));
const r = m.getAdapter('rijks');
// Pick any HMO URL from the orderedItems output. Easiest: open the
// modal, pick a Rijks set, watch listArtworks() emit items in the
// console (or use one from the network response). Example HMO URL:
const hmo = 'https://id.rijksmuseum.nl/200108720';
console.log(await r.previewUrl({ id: hmo }, 400));
```

Expected: a URL like `https://iiif.micr.io/{key}/full/!400,400/0/default.jpg`. Open that URL in a new tab — confirm an image loads. Network panel should show 3 hops (HMO → VisualItem → DigitalObject) plus the eventual image fetch.

Run the call a second time with the same HMO — it should return instantly (cache hit, no new network requests).

Try one bad HMO (e.g. mangle the id): should return null or throw, and not crash anything.

- [ ] **Step 6: Commit**

```powershell
git add webui/museum/rijksmuseum.js
git commit -m "webui/museum/rijks: add previewUrl() with lazy Linked-Art walk"
```

---

## Task 3: Rewrite `renderPreviewStep()` and update CSS

Replace the 8-thumbnail grid with the single-artwork preview. This is the big task — the modal's preview state, navigation handlers, render-token guard, and CSS all live here.

**File:**
- Modify: `webui/museum/browse.js`

- [ ] **Step 1: Replace the preview-related CSS rules**

Find this block in the `CSS` template-literal constant near the top of the file:

```js
.mb-thumbs { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; margin: 8px 0 12px; }
.mb-thumb { width: 100%; aspect-ratio: 1 / 1; background: #0f172a; border-radius: 6px;
    object-fit: cover; display: block; }
.mb-textcards { display: flex; flex-direction: column; gap: 6px; margin: 8px 0 12px; }
.mb-textcard { background: rgba(255,255,255,0.04); border-radius: 6px; padding: 8px 10px;
    font-size: 0.82rem; line-height: 1.3; }
.mb-textcard .mb-tc-title { color: #f3f4f6; font-weight: 500; }
.mb-textcard .mb-tc-meta  { color: #94a3b8; font-size: 0.78rem; margin-top: 2px; }
```

Replace it with:

```js
.mb-preview { display: flex; flex-direction: column; align-items: stretch; margin: 8px 0 12px; }
.mb-preview-slot { position: relative; width: 100%; aspect-ratio: 1 / 1; background: #0f172a;
    border-radius: 6px; overflow: hidden; display: flex; align-items: center; justify-content: center; }
.mb-preview-img { width: 100%; height: 100%; object-fit: contain; display: block;
    transition: opacity 120ms linear; }
.mb-preview-img.loading { opacity: 0.4; }
.mb-preview-spin { position: absolute; bottom: 8px; right: 8px; width: 18px; height: 18px;
    border-radius: 50%; border: 2px solid rgba(255,255,255,0.2); border-top-color: #cbd5e1;
    animation: mb-spin 0.9s linear infinite; }
.mb-preview-spin.hidden { display: none; }
@keyframes mb-spin { to { transform: rotate(360deg); } }
.mb-preview-fail { color: #fca5a5; font-size: 0.85rem; text-align: center; padding: 12px; }
.mb-preview-meta { margin-top: 10px; }
.mb-preview-meta .mb-meta-title { color: #f3f4f6; font-weight: 500; font-size: 0.92rem; }
.mb-preview-meta .mb-meta-sub   { color: #94a3b8; font-size: 0.8rem; margin-top: 2px; }
.mb-nav { display: flex; align-items: center; justify-content: space-between; gap: 10px;
    margin-top: 10px; }
.mb-nav button { padding: 6px 14px; border-radius: 8px; border: 0; cursor: pointer;
    background: rgba(255,255,255,0.08); color: #f3f4f6; font-weight: 500; }
.mb-nav button[disabled] { opacity: 0.4; cursor: not-allowed; }
.mb-nav .mb-counter { color: #94a3b8; font-variant-numeric: tabular-nums; font-size: 0.85rem; }
```

- [ ] **Step 2: Add `PAGE_SIZE` constant**

Find the existing module-level constants near the top:

```js
const STYLE_ID = 'museum-browse-style';
```

Add immediately before:

```js
// PAGE_SIZE — items per listArtworks() round-trip during preview
// navigation. See spec §4.1 for the trade-off; 20 is the chosen value.
const PAGE_SIZE = 20;
```

- [ ] **Step 3: Initialize `state.preview` in the `state` literal**

Find the `state` declaration at the top of `openMuseumBrowse()` (around line 119):

```js
    let state = {
        step: 'museum',         // 'museum' | 'axis' | 'terms' | 'preview'
        adapter: null,          // museum adapter once chosen
        axis: null,             // {name, label} once chosen (or null if axes empty)
        term: null,             // {id, label, count}
        thumbs: [],             // imageId[] for preview
        cooldownTick: null,     // interval id for the cooldown countdown
    };
```

Replace it with:

```js
    let state = {
        step: 'museum',         // 'museum' | 'axis' | 'terms' | 'preview'
        adapter: null,          // museum adapter once chosen
        axis: null,             // {name, label} once chosen (or null if axes empty)
        term: null,             // {id, label, count}
        cooldownTick: null,     // interval id for the cooldown countdown
        preview: null,          // preview sub-state, populated by renderPreviewStep
    };
```

(The `thumbs` field is dead — the old preview's strip data — and is removed.)

- [ ] **Step 4: Replace `renderPreviewStep()` and add helpers**

Find the existing `renderPreviewStep()` function (lines 299-374, the `async function renderPreviewStep() { ... }` block). Replace **the entire function** with the new implementation below, plus the four new helpers immediately after it. (The `confirmAdd()` function that follows stays unchanged.)

```js
    function computeCursorEnd(prev, gotItemsCount) {
        // After a fetch returns gotItemsCount items, decide if this is
        // the end of the stream. See spec §4.3.
        if (gotItemsCount === 0) return true;
        if (typeof prev.total === 'number' && prev.nextOffset >= prev.total) return true;
        // AIC public-tier cap: `from + size <= 1000` (see finalized-design.md §9.1).
        if (state.adapter && state.adapter.id === 'artic' && prev.nextOffset >= 1000) {
            return true;
        }
        return false;
    }

    async function renderPreviewStep() {
        state.step = 'preview';
        state.preview = {
            index: 0,
            items: [],
            total: null,
            nextOffset: 0,
            cursorEnd: false,
            loading: true,
            error: null,
            renderToken: 0,
        };
        title.textContent = `${state.adapter.displayName}: preview`;
        crumbs.textContent = state.axis
            ? `${state.adapter.displayName} · ${state.axis.label} · ${state.term.label}`
            : `${state.adapter.displayName} · ${state.term.label}`;
        back.disabled = false;
        back.onclick = () => renderTermsStep();
        clear(body);
        body.appendChild(el('div', { class: 'mb-status', text: 'Loading…' }));

        let result;
        try {
            const arg = state.axis
                ? { offset: 0, rows: PAGE_SIZE, axis: state.axis.name }
                : { offset: 0, rows: PAGE_SIZE };
            result = await state.adapter.listArtworks(state.term.id, arg);
        } catch (err) {
            clear(body);
            if (err && err.status === 429) {
                const remain = await fetchCooldown(state.adapter.id);
                renderCooldown(remain || 60);
                return;
            }
            body.appendChild(el('div', { class: 'mb-status error', text: 'Failed to load preview.' }));
            // Still surface the Add button below so the user can commit
            // the channel even if the preview load failed (the device
            // will refresh independently).
            body.appendChild(buildAddRow());
            return;
        }

        const prev = state.preview;
        prev.items = result.items || [];
        prev.total = (typeof result.total === 'number') ? result.total : null;
        prev.nextOffset = PAGE_SIZE;
        prev.cursorEnd = computeCursorEnd(prev, prev.items.length);

        clear(body);
        if (prev.items.length === 0) {
            body.appendChild(el('div', {
                class: 'mb-status',
                text: 'No previewable artwork. Channel may be empty.',
            }));
            body.appendChild(buildAddRow());
            return;
        }

        const shell = buildPreviewShell();
        body.appendChild(shell.root);
        body.appendChild(buildAddRow());
        prev.loading = false;
        await renderPreviewSlot(shell);
    }

    function buildPreviewShell() {
        // Construct shell first, populate fields as we go, then wire up
        // events and assemble parent containers. Click handlers reference
        // `shell` so the same object can pass through to onPrev/onNext.
        const shell = {};
        shell.img      = el('img', { class: 'mb-preview-img', alt: '' });
        shell.spinner  = el('div', { class: 'mb-preview-spin hidden' });
        shell.failTile = el('div', { class: 'mb-preview-fail hidden',
            text: '⚠ couldn’t load this artwork' });
        shell.metaTitle = el('div', { class: 'mb-meta-title' });
        shell.metaSub   = el('div', { class: 'mb-meta-sub' });
        shell.prevBtn   = el('button', { text: '← Previous' });
        shell.nextBtn   = el('button', { text: 'Next →' });
        shell.counter   = el('span', { class: 'mb-counter' });

        shell.prevBtn.addEventListener('click', () => onPrev(shell));
        shell.nextBtn.addEventListener('click', () => onNext(shell));

        const slot = el('div', { class: 'mb-preview-slot' },
            [shell.img, shell.spinner, shell.failTile]);
        const meta = el('div', { class: 'mb-preview-meta' },
            [shell.metaTitle, shell.metaSub]);
        const nav  = el('div', { class: 'mb-nav' },
            [shell.prevBtn, shell.counter, shell.nextBtn]);
        shell.root = el('div', { class: 'mb-preview' }, [slot, meta, nav]);
        return shell;
    }

    function buildAddRow() {
        const addRow = el('div', { class: 'mb-add-row' });
        addRow.appendChild(el('span', {
            class: 'mb-add-label',
            text: `${state.term.label} · ${state.term.count} artwork${state.term.count === 1 ? '' : 's'}`,
        }));
        const addBtn = el('button', { class: 'mb-add', text: 'Add channel' });
        addBtn.addEventListener('click', confirmAdd);
        addRow.appendChild(addBtn);
        return addRow;
    }

    function updateNavState(shell) {
        const prev = state.preview;
        const atStart = prev.index <= 0;
        const atEnd   = (prev.index + 1 >= prev.items.length) && prev.cursorEnd;
        shell.prevBtn.disabled = atStart || prev.loading;
        shell.nextBtn.disabled = atEnd   || prev.loading;
        // Counter: "N / total" if total known, else just "N".
        const human = prev.index + 1;
        shell.counter.textContent = (typeof prev.total === 'number')
            ? `${human} / ${prev.total}`
            : String(human);
    }

    async function renderPreviewSlot(shell) {
        const prev = state.preview;
        const token = (prev.renderToken = (prev.renderToken + 1) | 0);
        const item = prev.items[prev.index];

        // Caption renders immediately from listArtworks metadata.
        shell.metaTitle.textContent = item.title || '(untitled)';
        const sub = [item.artist, item.date].filter(Boolean).join(' · ');
        shell.metaSub.textContent = sub;
        shell.metaSub.style.display = sub ? '' : 'none';

        // Loading state: dim current image, show spinner, hide fail tile.
        prev.loading = true;
        shell.img.classList.add('loading');
        shell.spinner.classList.remove('hidden');
        shell.failTile.classList.add('hidden');
        updateNavState(shell);

        let url = null;
        try {
            url = await state.adapter.previewUrl(item, 400);
        } catch (_) { url = null; }

        if (token !== prev.renderToken) return; // user clicked again; abandon

        if (!url) {
            shell.img.removeAttribute('src');
            shell.img.classList.remove('loading');
            shell.spinner.classList.add('hidden');
            shell.failTile.classList.remove('hidden');
            prev.loading = false;
            updateNavState(shell);
            return;
        }

        // Attach one-shot load/error handlers; check token to bail if a
        // newer navigation has started.
        const onLoad = () => {
            if (token !== prev.renderToken) return;
            shell.img.classList.remove('loading');
            shell.spinner.classList.add('hidden');
            shell.failTile.classList.add('hidden');
            prev.loading = false;
            updateNavState(shell);
        };
        const onError = () => {
            if (token !== prev.renderToken) return;
            shell.img.removeAttribute('src');
            shell.img.classList.remove('loading');
            shell.spinner.classList.add('hidden');
            shell.failTile.classList.remove('hidden');
            prev.loading = false;
            updateNavState(shell);
        };
        shell.img.onload  = onLoad;
        shell.img.onerror = onError;
        shell.img.src     = url;
    }

    function onPrev(shell) {
        const prev = state.preview;
        if (prev.loading) return;
        if (prev.index <= 0) return;
        prev.index -= 1;
        renderPreviewSlot(shell);
    }

    async function onNext(shell) {
        const prev = state.preview;
        if (prev.loading) return;
        if (prev.index + 1 < prev.items.length) {
            prev.index += 1;
            renderPreviewSlot(shell);
            return;
        }
        if (prev.cursorEnd) return;

        // Need a fetch. Disable buttons immediately.
        prev.loading = true;
        updateNavState(shell);
        shell.spinner.classList.remove('hidden');

        let result;
        try {
            const arg = state.axis
                ? { offset: prev.nextOffset, rows: PAGE_SIZE, axis: state.axis.name }
                : { offset: prev.nextOffset, rows: PAGE_SIZE };
            result = await state.adapter.listArtworks(state.term.id, arg);
        } catch (err) {
            shell.spinner.classList.add('hidden');
            prev.loading = false;
            if (err && err.status === 429) {
                const remain = await fetchCooldown(state.adapter.id);
                renderCooldown(remain || 60);
                return;
            }
            // Soft failure: stay on the current item, mark cursor end so
            // we don't loop. User can still retry by clicking Next again
            // if cursorEnd were not set — but we set it to avoid hammering
            // a broken endpoint.
            prev.cursorEnd = true;
            updateNavState(shell);
            return;
        }

        const newItems = (result && Array.isArray(result.items)) ? result.items : [];
        prev.items.push(...newItems);
        prev.nextOffset += PAGE_SIZE;
        if (prev.total == null && result && typeof result.total === 'number') {
            prev.total = result.total;
        }
        prev.cursorEnd = computeCursorEnd(prev, newItems.length);

        if (newItems.length === 0) {
            // We bumped past the actual end. Nothing new to show.
            shell.spinner.classList.add('hidden');
            prev.loading = false;
            updateNavState(shell);
            return;
        }

        prev.index += 1;
        renderPreviewSlot(shell);
    }
```

- [ ] **Step 5: Manual verification — AIC walk**

Reload the playset editor. Open browser DevTools (Network tab).

Open the museum modal, pick AIC → axis: artwork-types → term: "Painting" (or any term with many artworks). Confirm:

- A single image appears at ~400 px, fitting within the modal.
- Caption below shows title, then artist · date (when present).
- Counter shows `1 / N` where N is the term's total.
- "← Previous" is disabled (greyed-out).
- "Next →" is enabled.
- Below the nav row, the existing Add row still shows "Painting · N artworks" and the "Add channel" button.

Click "Next →" 19 times. Image and caption should swap on each click; no new network request to `api.artic.edu` until the 20th click. On the 20th click (index 0 → 19 needs no fetch; 19 → 20 needs the next page), a single `GET /artworks/search?...&page=2&...` should appear in the Network panel. Image swaps to item 20.

Click "← Previous" 5 times. Should return to item 15 instantly with no network fetches.

- [ ] **Step 6: Manual verification — AIC 1000-cap**

Continue with the same AIC term (pick one with > 1000 artworks like "Painting"). Click "Next →" repeatedly. Watch the `nextOffset` reach 1000 (you can inspect `state.preview` via DevTools by setting a breakpoint, or just count clicks: 50 page fetches = 1000 records since PAGE_SIZE=20).

Expected: Once `nextOffset >= 1000`, `cursorEnd` is set. After navigating through all locally-fetched items, "Next →" disables. "← Previous" continues to work. AIC's 1001+ records remain inaccessible — matches the device-side behavior.

- [ ] **Step 7: Manual verification — Rijks Linked-Art walk + cache**

Open the museum modal, pick Rijksmuseum (no axis step — Rijks is axis-less) → any set with > 5 items.

Expected on entering preview:
- "Loading…" briefly, then the image slot fills with a real artwork (3-hop walk completes).
- Caption shows the title (artist/date often available for Rijks).
- Counter shows `1 / N`.

Click "Next →". Image should dim, spinner shows, new image swaps in after the walk for item 2.

Click "← Previous". Item 1 should return INSTANTLY — no network requests for the Linked-Art walk (cache hit).

Click "Next →" twice to reach item 3. Then "← Previous", "Next →" — should be cached round-trip from items 2 and 3.

- [ ] **Step 8: Manual verification — failure tile**

Force a Linked-Art walk failure. Easiest method: in DevTools Network panel, right-click → Block request URL, and block `https://data.rijksmuseum.nl/*` (or use the offline checkbox momentarily). Navigate to a fresh Rijks item.

Expected: failure tile appears in the image slot ("⚠ couldn't load this artwork"). Caption still renders (because metadata was already in `listArtworks` output). "← Previous" and "Next →" remain enabled — user can move past the failure.

Re-enable the network and navigate forward to a fresh item — should walk and render normally.

- [ ] **Step 9: Manual verification — V&A**

Open the museum modal, pick V&A → axis: collection → any term. Expected: same flow as AIC — image renders, prev/next work, caption populated.

- [ ] **Step 10: Manual verification — small term (boundary)**

Find a term with < 5 artworks (try AIC's `subjects` axis, deep terms). Expected: counter shows `1 / N` where N is small, Next disables at index N-1 (cursorEnd = true once total is reached).

- [ ] **Step 11: Manual verification — mobile viewport**

In Chrome DevTools, toggle device emulation (iPhone 12 / 390×844). Open the museum modal and walk to the preview step.

Expected: image fits the modal width, caption readable, both nav buttons reachable with one thumb, "Add channel" button visible without excessive scrolling. The modal is `max-width: 560px` but on a 390 px viewport the modal will be roughly viewport-width; the image's `aspect-ratio: 1/1` keeps it square within available width.

- [ ] **Step 12: Manual verification — Add still commits the channel**

From the preview at index 5 (or any non-zero index), click "Add channel". Confirm that the playset gains a channel for `(museum, axis, term)` — not for any specific artwork. Inspect the playset (e.g. via `/playset-editor` or by GETting `/playsets/<name>`): the channel spec should be identical to what the same selection produced before this change (`display_name` formula unchanged, `type:"institution"`, `name:"{museum}:{axis}"`, `identifier:"{term_id}"`).

- [ ] **Step 13: Commit**

```powershell
git add webui/museum/browse.js
git commit -m "webui/museum: single-artwork preview with prev/next navigation"
```

---

## Task 4: Update `finalized-design.md`

Reflect the UX change in the source-of-truth design doc.

**File:**
- Modify: `docs/art-institutions/finalized-design.md`

- [ ] **Step 1: Update §1 Scope**

Find this line in the "In v1" list (under "## 1. Scope"):

```
- 64×64 thumbnail previews of the first 8 artworks per term in the browse
  UI.
```

Replace with:

```
- Single-artwork preview in the browse UI, navigable via Previous / Next
  buttons. Per-artwork preview URLs are resolved on demand: AIC and V&A
  use the inline image id from the listing response; Rijks performs a
  3-hop Linked-Art walk lazily, one artwork at a time.
```

- [ ] **Step 2: Update §7.1 step 5**

Find this step in the "### 7.1 Browse → save" numbered list:

```
5. Browser calls `listArtworks(termId, {offset:0, rows:8})` and renders
   thumbnails at 64×64 via IIIF. Strip shows whatever count is
   available (≤8), thumbnails only, no captions. Optional hover
   tooltips on desktop browsers.
```

Replace with:

```
5. Browser calls `listArtworks(termId, {offset:0, rows:20})` and renders
   a single-artwork preview with Previous / Next navigation. The preview
   image is rendered at IIIF `!400,400`. Caption shows title, artist,
   and date. Additional pages are fetched lazily on Next when the local
   buffer is exhausted. AIC's `from + size ≤ 1000` cap (§9.1, §15.1) is
   enforced on the browser side so Next disables at the 1000th record.
```

- [ ] **Step 3: Add a §15 entry documenting the rationale**

Find the end of the "## 15. Field-observed fixes" section (after §15.5). Add a new subsection:

```
### 15.6 Browse preview UX: 8-thumbnail grid → single-artwork preview

The original design (§7.1, M1) used an 8-thumbnail 4×2 grid at 64×64.
Two field-observed issues drove the redesign:

1. Latency. Fetching 8 artworks visibly stalls the preview, especially
   for Rijks — its IIIF resolution requires a 3-hop Linked-Art walk
   per artwork, so populating an 8-tile grid would cost 24 extra HTTP
   requests. The original Rijks implementation worked around this by
   skipping image previews entirely and rendering a textual card list,
   which made Rijks's preview qualitatively different from AIC and V&A.
2. Mobile readability. 64×64 thumbnails in a 4-column grid inside a
   ≤560 px modal are too small to evaluate the artwork.

The replacement shows one artwork at a time at IIIF `!400,400`, with
Previous / Next navigation. Per-artwork preview URLs are resolved
lazily — AIC and V&A use the inline `image_id` from the listing
response (synchronous); Rijks performs the 3-hop walk on demand and
caches the resolved micrio id per adapter instance. The Add button
still commits the channel (museum, axis, term), not the visible
artwork. See
`docs/superpowers/specs/2026-05-12-museum-single-artwork-preview-design.md`.
```

- [ ] **Step 4: Manual verification — doc renders**

Open `docs/art-institutions/finalized-design.md` in any markdown viewer (GitHub-style preview, VS Code preview, etc.) and confirm:
- §1's bullet list still parses cleanly.
- §7.1 step 5 reads naturally.
- §15.6 appears at the bottom of the field-fixes list.

- [ ] **Step 5: Commit**

```powershell
git add docs/art-institutions/finalized-design.md
git commit -m "docs: museum preview is single-artwork w/ prev-next, not 8-grid"
```

---

## Task 5: Final integration check

Sanity gate before declaring the feature done.

- [ ] **Step 1: Full end-to-end flow**

In a fresh browser tab (clear cache), open the playset editor on the device. Walk the full museum browse flow for each museum:

1. AIC → axis: departments → term: any → preview navigates → Add → confirm channel appears in the playset editor's channel list.
2. V&A → axis: collection → term: any → preview navigates → Add → confirm.
3. Rijks (no axis) → set: any → preview navigates → Add → confirm.

For each, confirm:
- Preview image loads.
- Prev/Next behave correctly at boundaries.
- Add commits the right channel spec (the channel appears in the playset with `type:"institution"`).

- [ ] **Step 2: Cooldown re-entry**

Trigger an AIC 429 (rapid Next clicks; or visit a high-fanout AIC term repeatedly across separate modal opens). When the cooldown banner appears in the modal, confirm the countdown ticks down and the user is sent back to onPickMuseum after it expires — i.e. the existing rate-limit branch (`fetchCooldown` + `renderCooldown`) still fires correctly from the preview step's catch block.

- [ ] **Step 3: Open the existing playset list with Rijks channels**

If the device already has Rijks channels saved (from prior M2 testing), open the playset editor and confirm those channels still render correctly in the channel list — no regression in unrelated code.

- [ ] **Step 4: Done**

If all steps above pass, the feature is complete. No further commit needed (everything was committed task-by-task).

---

## Self-review notes

- Spec §2 non-goals respected: no device changes, no REST changes, no on-device cache, no per-artwork add, no search.
- Spec §3 UX matches Task 3 Step 4's render: image slot, caption, prev/next, counter, Add row preserved.
- Spec §3.1 loading state matches the `loading` CSS class + spinner toggle in `renderPreviewSlot`.
- Spec §3.2 failure tile matches `mb-preview-fail` + reveal logic.
- Spec §4.1 state shape (`nextOffset`, `renderToken`) used in Task 3 Step 4.
- Spec §4.3 navigation rules and AIC 1000-cap matched in `computeCursorEnd` and `onNext`.
- Spec §4.4 `previewUrl` shape matched in Tasks 1 and 2.
- Spec §4.5 render-token guard matched in `renderPreviewSlot`.
- Spec §5 files-touched table matches Tasks 1-4.
- Spec §6 testing items mapped to Task 3 Steps 5-12 and Task 5.
- Spec §7 implementation order matches Tasks 1-4.
- Spec §8 risks: image-load timeout NOT addressed in the plan; this is acceptable for v1 — if a user reports a hung image, we add a 15s `setTimeout` that calls the `error` path. Flagged for future iteration; not a blocker.
