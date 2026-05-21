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

// Bool+range partitioning constants. AIC's public search endpoint hard-caps
// `from + size ≤ 1000` (see docs/art-institutions/offset-tests/REPORT.md
// §1.3-§1.6). For offsets beyond that we walk POST DSL buckets carved over
// the artwork-ID space, mirroring components/art_institution/museums/artic.c
// (aic_discover_buckets / aic_refresh_partitioned).
const PARTITION_MAX_ID  = 1_000_000;
const PARTITION_CAP     = 1000;
const PARTITION_MIN_RNG = 100;
const PROBE_DELAY_MS    = 150;
const POST_HEADERS = Object.assign({}, HEADERS, {
    'Content-Type': 'application/json',
    'Accept':       'application/json',
});

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

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function postJson(url, body) {
    const r = await fetch(url, {
        method: 'POST', headers: POST_HEADERS, body: JSON.stringify(body),
    });
    if (r.status === 429) {
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
        const err = new Error(`AIC 429 POST ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`AIC ${r.status} POST ${url}`);
    return r.json();
}

// POST DSL body for AIC's /artworks/search. `size === 0` is a count-only
// probe used by bucket discovery; otherwise we ask for the artwork fields
// the preview pane renders.
function buildBoolRangeBody(filterField, termId, lo, hi, from, size) {
    const wantFields = size > 0
        ? ['id', 'title', 'image_id', 'artist_title', 'date_display']
        : ['id'];
    return {
        query: { bool: {
            must:   [{ term:  { [filterField]: Number(termId) } }],
            filter: [{ range: { id: { gte: lo, lt: hi } } }],
        }},
        sort: [{ id: 'asc' }],
        from, size,
        fields: wantFields,
    };
}

function parseItems(data) {
    return (data || [])
        .filter(it => it.image_id)
        .map(it => ({
            id: String(it.id),
            imageId: String(it.image_id),
            title: it.title || '(untitled)',
            artist: it.artist_title || '',
            date: it.date_display || '',
        }));
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
        // buckets: axisName -> termId -> [{ lo, hi, count }, ...] cached per
        // session. Populated lazily on first deep (offset > 1000) jump.
        this._buckets = Object.create(null);
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

    async listArtworks(termId, { offset = 0, rows = 8, axis = 'departments', onProgress = null } = {}) {
        const ax = AXIS_BY_NAME[axis];
        if (!ax) throw new Error(`AIC: unknown axis ${axis}`);

        // Within the public-tier cap → existing GET path. Cheap, no probe.
        if (offset + rows <= PARTITION_CAP) {
            const page = Math.floor(offset / rows) + 1;
            const sp = buildSearchParams(ax.filterField, termId, page, rows);
            const d = await getJson(`${SEARCH}?${sp}`);
            return {
                items: parseItems(d && d.data),
                total: Number(d && d.pagination && d.pagination.total) || 0,
            };
        }
        // Beyond the cap → POST DSL bool+range partitioning.
        return this._fetchAtOffsetDeep(ax.filterField, termId, axis, offset, rows, onProgress);
    }

    // Recursively split [lo, hi) until every sub-range has count ≤ 1000.
    // Empty ranges are omitted. Calls onProgress with a short status string
    // as buckets accumulate so the modal can update its hint line.
    async _discoverBuckets(filterField, termId, lo, hi, out, onProgress) {
        if (lo >= hi) return;
        const body = buildBoolRangeBody(filterField, termId, lo, hi, 0, 0);
        const d = await postJson(SEARCH, body);
        const total = (d && d.pagination && Number(d.pagination.total)) || 0;
        if (total === 0) return;
        if (total <= PARTITION_CAP || (hi - lo) <= PARTITION_MIN_RNG) {
            out.push({ lo, hi, count: total });
            if (onProgress) {
                const sum = out.reduce((s, b) => s + b.count, 0);
                onProgress(`Probing artwork-ID layout… ${out.length} buckets, ${sum} works mapped.`);
            }
            return;
        }
        // Politeness vs the 60 req/min cap.
        await sleep(PROBE_DELAY_MS);
        const mid = lo + Math.floor((hi - lo) / 2);
        await this._discoverBuckets(filterField, termId, lo, mid, out, onProgress);
        await this._discoverBuckets(filterField, termId, mid, hi, out, onProgress);
    }

    // Walk buckets to find the page containing `offset`, fetch one window.
    async _fetchAtOffsetDeep(filterField, termId, axis, offset, rows, onProgress) {
        let buckets = (this._buckets[axis] && this._buckets[axis][termId]) || null;
        if (!buckets) {
            if (onProgress) onProgress('Probing artwork-ID layout… (first deep jump in this term)');
            const acc = [];
            await this._discoverBuckets(filterField, termId, 0, PARTITION_MAX_ID, acc, onProgress);
            buckets = acc;
            if (!this._buckets[axis]) this._buckets[axis] = Object.create(null);
            this._buckets[axis][termId] = buckets;
        }
        const totalRecords = buckets.reduce((s, b) => s + b.count, 0);
        if (totalRecords === 0) return { items: [], total: 0 };

        let walked = 0;
        for (const b of buckets) {
            if (walked + b.count <= offset) { walked += b.count; continue; }
            const withinBucket = offset - walked;
            // Per-query cap: from + size ≤ 1000.
            const sizeReq = Math.min(rows, b.count - withinBucket, PARTITION_CAP - withinBucket);
            if (sizeReq <= 0) return { items: [], total: totalRecords };
            if (onProgress) onProgress(`Fetching artwork ${offset + 1} of ${totalRecords}…`);
            const body = buildBoolRangeBody(filterField, termId, b.lo, b.hi, withinBucket, sizeReq);
            const d = await postJson(SEARCH, body);
            return { items: parseItems(d && d.data), total: totalRecords };
        }
        return { items: [], total: totalRecords };
    }

    // 64×64 thumbnail (matches design §1's spec). IIIF v2 size syntax
    // !w,h means "fit within w×h, preserve aspect ratio".
    thumbnailUrl(imageId, size = 64) {
        return `${IIIF_HOST}/${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }

    // Single-artwork preview URL. AIC has the IIIF id inline in
    // listArtworks output, so this is a synchronous thumbnailUrl with a
    // larger size parameter — same JPEG, different rendition.
    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }

    // Fetch title + artist + date given only the IIIF image_id (the
    // iiif_key the device stores). AIC's listing endpoint returns all
    // three inline, so a single search-by-image_id call is enough. The
    // image_id is a UUID and globally unique within AIC.
    async fetchMetadataByIiifKey(iiifKey) {
        if (!iiifKey) return { title: null, artist: null, date: null };
        const sp = new URLSearchParams({
            limit: '1',
            fields: 'id,title,artist_title,date_display',
        });
        sp.set('query[term][image_id]', String(iiifKey));
        const d = await getJson(`${SEARCH}?${sp}`);
        const items = (d && Array.isArray(d.data)) ? d.data : [];
        if (items.length === 0) return { title: null, artist: null, date: null };
        const it = items[0];
        return {
            title:  it.title         ? String(it.title)         : null,
            artist: it.artist_title  ? String(it.artist_title)  : null,
            date:   it.date_display  ? String(it.date_display)  : null,
        };
    }
}
