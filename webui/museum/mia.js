// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Minneapolis Institute of Art (Mia) browse adapter — live-aggregation
// model.
//
// Mia's search API is a raw Elasticsearch passthrough
// (search.artsmia.org/{ES query}?size=&from=), and — first among the
// shipped museums — its facet vocabularies are enumerated LIVE from the
// API's aggregations: one cached size=0&aggs=all call returns capitalized
// groups (Classification, Department, Country, Style, ...) with per-term
// counts already scoped to the public-domain-with-image filter. No baked
// term list, no vocabulary endpoints.
//
// Images are pre-rendered S3 buckets at https://1.api.artsmia.org/
// {400|800|full}/{id}.jpg — previews use 400 (~20 KB), the device
// downloads 800. previewUrl() therefore ignores arbitrary sizes.
//
// Term identifiers are the facet values themselves ("Paintings",
// "Asian Art"), embedded in an ES phrase query. Buckets are dropped at
// enumeration when the key: exceeds 32 UTF-8 BYTES (the playset
// identifier slot is bytes, and Mia has multi-byte keys like "Wiener
// Werkstätte"), contains '"' or '\' (would break the phrase query), or
// is not well-formed Unicode (Mia's Style facet contains mojibake with
// unpaired surrogates, which encodeURIComponent throws on). The C
// adapter re-validates the quote/backslash rule (dual gate).
//
// The ES from+size window is 10 000; past it the endpoint returns a
// bare [] instead of the envelope. The modal never pages that deep in
// practice (PAGE_SIZE 20), but listArtworks guards the shape anyway.
//
// See reference/museum-art/source/mia/output/report.md for the API
// investigation and finalized-design.md §9.7 for the per-museum spec.

const API_ROOT = 'https://search.artsmia.org/';
const IMG_ROOT = 'https://1.api.artsmia.org';
const SCOPE = 'rights_type:"Public Domain" AND image:valid';
const MAX_ID_BYTES = 32;  // playset identifier[33] slot (bytes, not chars)

const AXES = [
    { name: 'classification', label: 'Classifications', agg: 'Classification' },
    { name: 'department',     label: 'Departments',     agg: 'Department'     },
    { name: 'country',        label: 'Countries',       agg: 'Country'        },
    { name: 'style',          label: 'Styles',          agg: 'Style'          },
];

const utf8len = (s) => new TextEncoder().encode(s).length;

function isUsableTermKey(key) {
    if (typeof key !== 'string' || !key) return false;
    if (key.includes('"') || key.includes('\\')) return false;  // phrase-query breakers
    // Mia's data contains mojibake with unpaired surrogates; those keys
    // can't round-trip through URLs (encodeURIComponent throws).
    if (typeof key.isWellFormed === 'function') {
        if (!key.isWellFormed()) return false;
    } else {
        try { encodeURIComponent(key); } catch (_) { return false; }
    }
    if (utf8len(key) > MAX_ID_BYTES) return false;  // identifier slot
    return true;
}

async function getJson(url) {
    const r = await fetch(url);
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'mia',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`Mia 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`Mia ${r.status} ${url}`);
    return r.json();
}

// Envelope accessors tolerating the bare-[] past-window response.
function getHits(data) {
    const hits = data && data.hits && Array.isArray(data.hits.hits)
        ? data.hits.hits : [];
    return hits;
}

function getTotal(data) {
    const t = data && data.hits && data.hits.total;
    if (t && typeof t === 'object' && typeof t.value === 'number') return t.value;
    if (typeof t === 'number') return t;
    return 0;
}

export class MiaAdapter {
    get id()          { return 'mia'; }
    get displayName() { return 'Minneapolis Institute of Art'; }
    get shortName()   { return 'Mia'; }
    get axes()        { return AXES.map(a => ({ name: a.name, label: a.label })); }

    constructor() {
        // One aggregation response covers all four axes; cached per session.
        this._aggsPromise = null;
    }

    async _loadAggs() {
        if (!this._aggsPromise) {
            const url = `${API_ROOT}${encodeURIComponent(SCOPE)}?size=0&aggs=all`;
            this._aggsPromise = getJson(url).then(d => (d && d.aggregations) || {});
        }
        return this._aggsPromise;
    }

    async listCollections({ axis = 'classification' } = {}) {
        const ax = AXES.find(a => a.name === axis);
        if (!ax) throw new Error(`Mia: unknown axis ${axis}`);

        const aggs = await this._loadAggs();
        const group = aggs[ax.agg];
        const buckets = group && Array.isArray(group.buckets) ? group.buckets : [];

        const terms = [];
        for (const b of buckets) {
            const key = b && b.key;
            const count = typeof b.doc_count === 'number' ? b.doc_count : 0;
            if (count <= 0) continue;
            if (!isUsableTermKey(key)) continue;
            terms.push({ id: key, label: key, count });
        }
        // ES returns buckets count-desc already; re-sort defensively.
        terms.sort((a, b) => b.count - a.count);
        return terms;
    }

    async listArtworks(termId, { offset = 0, rows = 20, axis = 'classification' } = {}) {
        const ax = AXES.find(a => a.name === axis);
        if (!ax) throw new Error(`Mia: unknown axis ${axis}`);

        const query = `${ax.name}:"${termId}" AND ${SCOPE}`;
        const url = `${API_ROOT}${encodeURIComponent(query)}?size=${rows}&from=${offset}`;
        const data = await getJson(url);

        const items = [];
        for (const hit of getHits(data)) {
            const s = hit && hit._source;
            if (!s || typeof s.id !== 'number') continue;
            if (s.image !== 'valid') continue;
            items.push({
                id:      String(s.id),
                imageId: String(s.id),   // numeric object id, mirrors device iiif_key
                title:   s.title || '(untitled)',
                artist:  s.artist || '',
                date:    s.dated || '',
            });
        }
        return { items, total: getTotal(data) };
    }

    thumbnailUrl(imageId, size = 64) {
        // Only 400/800/full pre-renders exist.
        const bucket = size <= 400 ? 400 : 800;
        return `${IMG_ROOT}/${bucket}/${encodeURIComponent(imageId)}.jpg`;
    }

    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }

    // Fetch title + artist + date given the device's iiif_key (the
    // numeric object id) via an id: query on the same search endpoint.
    async fetchMetadataByIiifKey(iiifKey) {
        if (!iiifKey || !/^\d+$/.test(String(iiifKey))) {
            return { title: null, artist: null, date: null };
        }
        const url = `${API_ROOT}${encodeURIComponent(`id:${iiifKey}`)}?size=1`;
        const data = await getJson(url);
        const hits = getHits(data);
        if (hits.length === 0) return { title: null, artist: null, date: null };
        const s = hits[0]._source || {};
        return {
            title:  s.title && s.title !== '(untitled)' ? String(s.title) : null,
            artist: s.artist ? String(s.artist) : null,
            date:   s.dated ? String(s.dated) : null,
        };
    }
}
