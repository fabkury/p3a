// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Art Institute of Chicago browse adapter — ports
// reference/museum-art/ubi-test/js/adapters/artic.js to a plain
// browser-side ES module loaded by the playset editor's museum browse
// modal.
//
// AIC has no flat "collection" concept; it exposes six filterable facet
// vocabularies (departments / classifications / subjects / themes /
// galleries / artwork-types). The exhibitions facet is list-only on the
// artwork side — a saved channel would always come back empty — so it is
// intentionally hidden.
//
// Every request carries an AIC-User-Agent header per AIC's API terms.
// CORS preflight on the API allows it.

const API = 'https://api.artic.edu/api/v1';
const SEARCH = `${API}/artworks/search`;
const IIIF_HOST = 'https://www.artic.edu/iiif/2';

const HEADERS = {
    'AIC-User-Agent': 'p3a-museum-browse/1 (pub@kury.dev)',
};

const ARTWORK_FIELDS = 'id,title,image_id,artist_title,date_display';
const TERMS_PER_AXIS = 30;
// AIC's per-IP cap is 60 req/min. We can fan out the count probes a bit,
// but six in flight at a time keeps us well clear of the limit even when
// other paths (the device's refresh, the user opening another tab) are
// also burning budget.
const COUNT_PROBE_CONCURRENCY = 6;

const AXES = [
    { name: 'departments',     endpoint: `${API}/departments`,            extraParams: {},                                                  filterField: 'department_id' },
    { name: 'classifications', endpoint: `${API}/category-terms/search`,  extraParams: { 'query[term][subtype]': 'classification' },        filterField: 'classification_id' },
    { name: 'subjects',        endpoint: `${API}/category-terms/search`,  extraParams: { 'query[term][subtype]': 'subject' },               filterField: 'subject_id' },
    { name: 'themes',          endpoint: `${API}/category-terms/search`,  extraParams: { 'query[term][subtype]': 'theme' },                 filterField: 'category_ids' },
    { name: 'galleries',       endpoint: `${API}/galleries`,              extraParams: {},                                                  filterField: 'gallery_id' },
    { name: 'artwork-types',   endpoint: `${API}/artwork-types`,          extraParams: {},                                                  filterField: 'artwork_type_id' },
];

const AXIS_BY_NAME = Object.fromEntries(AXES.map(a => [a.name, a]));

// AIC's department / classification labels are wordy; the editor surfaces
// each axis in a dropdown with a friendlier label.
const AXIS_DISPLAY_LABELS = {
    'departments':     'Departments',
    'classifications': 'Classifications',
    'subjects':        'Subjects',
    'themes':          'Themes',
    'galleries':       'Galleries',
    'artwork-types':   'Artwork types',
};

async function getJson(url) {
    const r = await fetch(url, { headers: HEADERS });
    if (r.status === 429) {
        // Tell the device about the cooldown so its own refresh path
        // pauses too. Best-effort: ignore failures.
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'artic',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`AIC 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`AIC ${r.status} ${url}`);
    return r.json();
}

function buildSearchParams(filterField, filterValue, page, limit) {
    const p = new URLSearchParams({
        page: String(page),
        limit: String(limit),
        fields: ARTWORK_FIELDS,
    });
    p.set(`query[term][${filterField}]`, String(filterValue));
    return p;
}

// Simple bounded concurrency runner — keeps the term-count probe under
// AIC's per-IP cap.
async function mapWithConcurrency(items, limit, mapper) {
    const out = new Array(items.length);
    let cursor = 0;
    async function worker() {
        while (true) {
            const i = cursor++;
            if (i >= items.length) return;
            try {
                out[i] = await mapper(items[i], i);
            } catch (_) {
                out[i] = undefined;
            }
        }
    }
    const workers = [];
    for (let w = 0; w < Math.min(limit, items.length); w++) workers.push(worker());
    await Promise.all(workers);
    return out;
}

export class ArticAdapter {
    get id()          { return 'artic'; }
    get displayName() { return 'Art Institute of Chicago'; }
    get shortName()   { return 'AIC'; }
    get axes()        { return AXES.map(a => ({ name: a.name, label: AXIS_DISPLAY_LABELS[a.name] || a.name })); }

    constructor() {
        // termsByAxis: axisName -> [{ id, label, count }] cached per session.
        this._termsByAxis = Object.create(null);
    }

    async listCollections({ axis = 'departments' } = {}) {
        if (this._termsByAxis[axis]) return this._termsByAxis[axis];
        const ax = AXIS_BY_NAME[axis];
        if (!ax) throw new Error(`AIC: unknown axis ${axis}`);

        // 1) Pull TERMS_PER_AXIS terms in storage order.
        const params = new URLSearchParams({ limit: String(TERMS_PER_AXIS), page: '1' });
        for (const [k, v] of Object.entries(ax.extraParams)) params.set(k, v);
        const listing = await getJson(`${ax.endpoint}?${params}`);
        const terms = (listing.data || []).map(t => ({
            id: String(t.id),
            label: String(t.title || t.id),
        }));

        // 2) Probe artwork counts with bounded concurrency. AIC's term
        // endpoints don't expose counts directly, so we hit /search with
        // limit=1 per term and read pagination.total.
        const counts = await mapWithConcurrency(terms, COUNT_PROBE_CONCURRENCY, async (t) => {
            const sp = buildSearchParams(ax.filterField, t.id, 1, 1);
            const d = await getJson(`${SEARCH}?${sp}`);
            return Number(d && d.pagination && d.pagination.total) || 0;
        });

        const out = terms
            .map((t, i) => Object.assign({}, t, { count: counts[i] || 0 }))
            .filter(t => t.count > 0)
            .sort((a, b) => b.count - a.count);

        this._termsByAxis[axis] = out;
        return out;
    }

    async listArtworks(termId, { offset = 0, rows = 8, axis = 'departments' } = {}) {
        const ax = AXIS_BY_NAME[axis];
        if (!ax) throw new Error(`AIC: unknown axis ${axis}`);
        const page = Math.floor(offset / rows) + 1;
        const sp = buildSearchParams(ax.filterField, termId, page, rows);
        const d = await getJson(`${SEARCH}?${sp}`);
        const items = (d.data || [])
            .filter(it => it.image_id)
            .map(it => ({
                id: String(it.id),
                imageId: String(it.image_id),
                title: it.title || '(untitled)',
                artist: it.artist_title || '',
                date: it.date_display || '',
            }));
        return { items, total: Number(d && d.pagination && d.pagination.total) || 0 };
    }

    // 64×64 thumbnail (matches design §1's spec). IIIF v2 size syntax
    // !w,h means "fit within w×h, preserve aspect ratio".
    thumbnailUrl(imageId, size = 64) {
        return `${IIIF_HOST}/${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }
}
