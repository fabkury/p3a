// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Museum browse modal — the four-step flow specified in
// docs/art-institutions/finalized-design.md §7.1:
//   museum -> axis -> term-with-count -> 8-thumbnail strip -> Add.
//
// The modal owns its own DOM and CSS injection. Callers invoke
// openMuseumBrowse({ onAdd, onCancel }); the modal mounts itself,
// runs the flow, and calls back exactly once with the result.

import { listAdapters, getAdapter } from './index.js';

// PAGE_SIZE — items per listArtworks() round-trip during preview
// navigation. See spec §4.1 for the trade-off; 20 is the chosen value.
const PAGE_SIZE = 20;

const STYLE_ID = 'museum-browse-style';
const CSS = `
.mb-overlay { position: fixed; inset: 0; background: rgba(15,20,30,0.78); display: flex;
    align-items: center; justify-content: center; padding: 16px; z-index: 9999; }
.mb-overlay.hidden { display: none; }
.mb-modal { background: #1f2233; color: #f3f4f6; border-radius: 12px; width: 100%;
    max-width: 560px; max-height: 90vh; display: flex; flex-direction: column;
    box-shadow: 0 20px 60px rgba(0,0,0,0.5); }
.mb-header { padding: 14px 16px; border-bottom: 1px solid rgba(255,255,255,0.08);
    display: flex; align-items: center; gap: 8px; }
.mb-header h3 { margin: 0; font-size: 1rem; flex: 1; font-weight: 600; }
.mb-back { background: transparent; border: 0; color: #cbd5e1; cursor: pointer; font-size: 1rem; padding: 4px 8px; }
.mb-back[disabled] { visibility: hidden; }
.mb-close { background: transparent; border: 0; color: #cbd5e1; cursor: pointer; font-size: 1.4rem; padding: 0 4px; }
.mb-crumbs { padding: 6px 16px 0; font-size: 0.78rem; color: #94a3b8; min-height: 1.4em; }
.mb-body { padding: 12px 16px 16px; overflow: auto; flex: 1; min-height: 200px; }
.mb-list { list-style: none; margin: 0; padding: 0; }
.mb-item { display: flex; align-items: center; justify-content: space-between; gap: 10px;
    padding: 10px 12px; border-radius: 8px; cursor: pointer; user-select: none;
    background: rgba(255,255,255,0.04); margin-bottom: 6px; }
.mb-item:hover { background: rgba(99,102,241,0.18); }
.mb-item .mb-count { color: #94a3b8; font-variant-numeric: tabular-nums; font-size: 0.85rem; }
.mb-status { padding: 16px 12px; color: #cbd5e1; font-size: 0.9rem; text-align: center; }
.mb-status.error { color: #fca5a5; }
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
.mb-add-row { display: flex; align-items: center; justify-content: space-between; gap: 10px;
    padding-top: 8px; border-top: 1px solid rgba(255,255,255,0.08); margin-top: 8px; }
.mb-add-row .mb-add-label { font-size: 0.85rem; color: #cbd5e1; }
.mb-add-row button { padding: 8px 16px; border-radius: 8px; border: 0; cursor: pointer; font-weight: 600; }
.mb-add-row .mb-add { background: #6366f1; color: white; }
.mb-add-row .mb-add[disabled] { opacity: 0.55; cursor: not-allowed; }
.mb-rl-banner { background: rgba(220,38,38,0.15); border: 1px solid rgba(220,38,38,0.4);
    color: #fecaca; padding: 10px 12px; border-radius: 8px; margin-bottom: 10px; font-size: 0.85rem; }
`;

function ensureStyle() {
    if (document.getElementById(STYLE_ID)) return;
    const style = document.createElement('style');
    style.id = STYLE_ID;
    style.textContent = CSS;
    document.head.appendChild(style);
}

function el(tag, attrs, children) {
    const node = document.createElement(tag);
    if (attrs) {
        for (const k of Object.keys(attrs)) {
            if (k === 'class') node.className = attrs[k];
            else if (k === 'text') node.textContent = attrs[k];
            else if (k.startsWith('on') && typeof attrs[k] === 'function') {
                node.addEventListener(k.slice(2), attrs[k]);
            } else if (attrs[k] === false || attrs[k] === null || attrs[k] === undefined) {
                /* skip */
            } else {
                node.setAttribute(k, attrs[k] === true ? '' : String(attrs[k]));
            }
        }
    }
    if (children) {
        for (const child of children) {
            if (child == null) continue;
            node.appendChild(typeof child === 'string' ? document.createTextNode(child) : child);
        }
    }
    return node;
}

function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }

async function fetchCooldown(museumId) {
    try {
        const r = await fetch('/api/museum/rate-limits');
        if (!r.ok) return 0;
        const j = await r.json();
        return (j && j[museumId] && j[museumId].remaining_sec) | 0;
    } catch (_) {
        return 0;
    }
}

function truncateForDisplayName(s, max) {
    if (s.length <= max) return s;
    return s.slice(0, Math.max(1, max - 1)) + '…';
}

// Display name format: "{museum_short} · {Term label}" (axis omitted —
// it's an organizational facet, not user-meaningful in the channel row),
// truncated at 64 chars with an ellipsis at the term tail.
function composeDisplayName(adapter, termLabel) {
    const prefix = `${adapter.shortName} · `;
    const budget = 64 - prefix.length;
    if (budget <= 1) return truncateForDisplayName(prefix + termLabel, 64);
    return prefix + truncateForDisplayName(termLabel, budget);
}

export function openMuseumBrowse({ onAdd, onCancel } = {}) {
    ensureStyle();

    let state = {
        step: 'museum',         // 'museum' | 'axis' | 'terms' | 'preview'
        adapter: null,          // museum adapter once chosen
        axis: null,             // {name, label} once chosen (or null if axes empty)
        term: null,             // {id, label, count}
        cooldownTick: null,     // interval id for the cooldown countdown
        preview: null,          // preview sub-state, populated by renderPreviewStep
    };

    const overlay = el('div', { class: 'mb-overlay' });
    const modal   = el('div', { class: 'mb-modal' });
    const header  = el('div', { class: 'mb-header' });
    const back    = el('button', { class: 'mb-back', text: '←', title: 'Back' });
    const title   = el('h3',   { text: 'Browse museums' });
    const close   = el('button', { class: 'mb-close', text: '×', title: 'Close' });
    const crumbs  = el('div', { class: 'mb-crumbs' });
    const body    = el('div', { class: 'mb-body' });

    header.appendChild(back);
    header.appendChild(title);
    header.appendChild(close);
    modal.appendChild(header);
    modal.appendChild(crumbs);
    modal.appendChild(body);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);

    function close_modal(cancelled) {
        if (state.cooldownTick) { clearInterval(state.cooldownTick); state.cooldownTick = null; }
        if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
        if (cancelled && typeof onCancel === 'function') onCancel();
    }
    close.addEventListener('click', () => close_modal(true));
    overlay.addEventListener('click', (e) => { if (e.target === overlay) close_modal(true); });

    function renderMuseumStep() {
        state.step = 'museum';
        state.adapter = null;
        state.axis = null;
        state.term = null;
        title.textContent = 'Pick a museum';
        crumbs.textContent = '';
        back.disabled = true;
        clear(body);

        const list = el('ul', { class: 'mb-list' });
        const adapters = listAdapters();
        if (adapters.length === 0) {
            body.appendChild(el('div', { class: 'mb-status', text: 'No museums available.' }));
            return;
        }
        for (const a of adapters) {
            const row = el('li', { class: 'mb-item' }, [
                el('span', { text: a.displayName }),
                el('span', { class: 'mb-count', text: a.shortName }),
            ]);
            row.addEventListener('click', () => onPickMuseum(a));
            list.appendChild(row);
        }
        body.appendChild(list);
    }

    async function onPickMuseum(adapter) {
        state.adapter = adapter;
        // Cooldown gate before any expensive per-axis probe.
        const cooldown = await fetchCooldown(adapter.id);
        if (cooldown > 0) {
            renderCooldown(cooldown);
            return;
        }
        if (!adapter.axes || adapter.axes.length === 0) {
            // Axis-less museum (M2 Rijksmuseum). Jump straight to the term
            // list. Pass axis=null down through the flow.
            state.axis = null;
            renderTermsStep();
        } else {
            renderAxisStep();
        }
    }

    function renderAxisStep() {
        state.step = 'axis';
        title.textContent = `${state.adapter.displayName}: pick a section`;
        crumbs.textContent = state.adapter.displayName;
        back.disabled = false;
        back.onclick = () => renderMuseumStep();
        clear(body);

        const list = el('ul', { class: 'mb-list' });
        for (const ax of state.adapter.axes) {
            const row = el('li', { class: 'mb-item' }, [ el('span', { text: ax.label }) ]);
            row.addEventListener('click', () => {
                state.axis = ax;
                renderTermsStep();
            });
            list.appendChild(row);
        }
        body.appendChild(list);
    }

    function renderCooldown(initialSec) {
        title.textContent = `${state.adapter.displayName}: rate limited`;
        crumbs.textContent = state.adapter.displayName;
        back.disabled = false;
        back.onclick = () => renderMuseumStep();
        clear(body);

        const remaining = el('div', { class: 'mb-rl-banner' });
        body.appendChild(remaining);
        let secs = initialSec;
        function paint() {
            remaining.textContent = `Rate-limited — try again in ${secs}s. AIC caps usage at 60 requests/minute per IP.`;
        }
        paint();
        if (state.cooldownTick) clearInterval(state.cooldownTick);
        state.cooldownTick = setInterval(() => {
            secs--;
            if (secs <= 0) {
                clearInterval(state.cooldownTick);
                state.cooldownTick = null;
                onPickMuseum(state.adapter);  // retry
            } else {
                paint();
            }
        }, 1000);
    }

    async function renderTermsStep() {
        state.step = 'terms';
        const axisLabel = state.axis ? state.axis.label : 'Sets';
        title.textContent = `${state.adapter.displayName}: ${axisLabel}`;
        crumbs.textContent = state.axis
            ? `${state.adapter.displayName} · ${state.axis.label}`
            : state.adapter.displayName;
        back.disabled = false;
        back.onclick = () => {
            state.term = null;
            if (state.adapter.axes && state.adapter.axes.length > 0) renderAxisStep();
            else renderMuseumStep();
        };
        clear(body);
        body.appendChild(el('div', { class: 'mb-status', text: 'Loading terms…' }));

        let terms;
        try {
            const arg = state.axis ? { axis: state.axis.name } : {};
            terms = await state.adapter.listCollections(arg);
        } catch (err) {
            clear(body);
            if (err && err.status === 429) {
                const remain = await fetchCooldown(state.adapter.id);
                renderCooldown(remain || 60);
                return;
            }
            body.appendChild(el('div', { class: 'mb-status error', text: 'Failed to load terms.' }));
            return;
        }

        clear(body);
        if (!terms || terms.length === 0) {
            body.appendChild(el('div', { class: 'mb-status', text: 'No terms with artwork available.' }));
            return;
        }
        const list = el('ul', { class: 'mb-list' });
        for (const t of terms) {
            const row = el('li', { class: 'mb-item' }, [
                el('span', { text: t.label }),
                el('span', { class: 'mb-count', text: String(t.count) }),
            ]);
            row.addEventListener('click', () => onPickTerm(t));
            list.appendChild(row);
        }
        body.appendChild(list);
    }

    async function onPickTerm(term) {
        state.term = term;
        renderPreviewStep();
    }

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

    function confirmAdd() {
        if (!state.adapter || !state.term) return;
        const axisName = state.axis ? state.axis.name : 'set';
        const spec = {
            type: 'institution',
            name: `${state.adapter.id}:${axisName}`,
            identifier: state.term.id,
            display_name: composeDisplayName(state.adapter, state.term.label),
            weight: 100,
        };
        close_modal(false);
        if (typeof onAdd === 'function') onAdd(spec);
    }

    renderMuseumStep();
}
