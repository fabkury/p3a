// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// SMK (Statens Museum for Kunst) browse adapter.
//
// One axis (`collection`). The modal auto-advances past the axis step.
// SMK's facets API can return the collection pairs in three shapes
// (alternating array, list of pairs, dict); the adapter handles all
// three to match the Python reference's defensive parsing.
//
// IIIF id is the JP2 filename — everything after `/iiif/jp2/` in
// `image_iiif_id`. The C-side build_iiif_url prepends the standard
// host prefix at refresh / download time.

const SEARCH = 'https://api.smk.dk/api/v1/art/search';
const IIIF_PREFIX = 'https://iip.smk.dk/iiif/jp2';

const MAX_LABEL_CHARS = 32;
const PAGE_SIZE = 100;

async function getJson(url) {
    const r = await fetch(url);
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'smk',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`SMK 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`SMK ${r.status} ${url}`);
    return r.json();
}

function parseFacetPairs(raw) {
    // SMK returns facets in one of three shapes; mirror Python's
    // _parse_facet_pairs (reference/museum-art/source/smk/run.py).
    const out = [];
    if (raw && typeof raw === 'object' && !Array.isArray(raw)) {
        for (const k of Object.keys(raw)) {
            out.push([String(k), Number(raw[k] || 0)]);
        }
        return out;
    }
    if (!Array.isArray(raw) || raw.length === 0) return out;
    const head = raw[0];
    if ((typeof head === 'string' || typeof head === 'number') && raw.length % 2 === 0) {
        for (let i = 0; i < raw.length; i += 2) {
            out.push([String(raw[i]), Number(raw[i + 1] || 0)]);
        }
        return out;
    }
    for (const entry of raw) {
        if (entry && typeof entry === 'object' && !Array.isArray(entry)) {
            const name = entry.name || entry.value || entry.key;
            const count = entry.count || entry.doc_count || 0;
            if (name != null) out.push([String(name), Number(count || 0)]);
        } else if (Array.isArray(entry) && entry.length === 2) {
            out.push([String(entry[0]), Number(entry[1] || 0)]);
        }
    }
    return out;
}

function extractFilename(imageIiifId) {
    if (typeof imageIiifId !== 'string') return null;
    const marker = '/iiif/jp2/';
    const idx = imageIiifId.lastIndexOf(marker);
    if (idx < 0) return null;
    const filename = imageIiifId.slice(idx + marker.length);
    if (!filename) return null;
    return filename;
}

function getTitle(item) {
    const titles = item.titles;
    if (Array.isArray(titles)) {
        for (const t of titles) {
            if (t && typeof t === 'object' && t.title) return String(t.title);
            if (typeof t === 'string' && t) return t;
        }
    } else if (typeof titles === 'string') {
        return titles;
    }
    return '(untitled)';
}

function getArtist(item) {
    const prod = item.production;
    if (Array.isArray(prod)) {
        for (const p of prod) {
            if (p && typeof p === 'object') {
                const c = p.creator || p.creator_forename || p.creator_surname;
                if (c) return String(c);
            }
        }
    }
    return '';
}

function getDate(item) {
    const prod = item.production;
    if (Array.isArray(prod) && prod.length > 0) {
        const p = prod[0];
        if (p) {
            const d = p.creation_date_text || p.creation_date || p.date_of_birth;
            if (d) return String(d);
        }
    }
    return '';
}

export class SmkAdapter {
    get id()          { return 'smk'; }
    get displayName() { return 'Statens Museum for Kunst'; }
    get shortName()   { return 'SMK'; }
    get axes() {
        return [{ name: 'collection', label: 'Collections' }];
    }

    constructor() {
        this._terms = null;  // cached per session
    }

    async listCollections({ axis = 'collection' } = {}) {
        if (axis !== 'collection') throw new Error(`SMK: unknown axis ${axis}`);
        if (this._terms) return this._terms;

        const params = new URLSearchParams({
            keys: '*',
            rows: '0',
            facets: 'collection',
        });
        const data = await getJson(`${SEARCH}?${params}`);
        const raw = (data && data.facets && data.facets.collection) || [];
        const pairs = parseFacetPairs(raw);

        const out = [];
        for (const [name, count] of pairs) {
            if (!name) continue;
            if (count <= 0) continue;
            if (name.length > MAX_LABEL_CHARS) continue;  // identifier[33] gate
            out.push({ id: name, label: name, count });
        }
        out.sort((a, b) => b.count - a.count);
        this._terms = out;
        return out;
    }

    async listArtworks(termId, { offset = 0, rows = PAGE_SIZE, axis = 'collection' } = {}) {
        if (axis !== 'collection') throw new Error(`SMK: unknown axis ${axis}`);
        const params = new URLSearchParams({
            keys: '*',
            offset: String(offset),
            rows: String(rows),
            filters: `[collection:${termId}],[has_image:true]`,
        });
        const d = await getJson(`${SEARCH}?${params}`);
        const records = Array.isArray(d.items) ? d.items : [];
        const items = [];
        for (const r of records) {
            const filename = extractFilename(r && r.image_iiif_id);
            if (!filename) continue;
            items.push({
                id: String(r.object_number || r.id || ''),
                imageId: filename,
                title:  getTitle(r),
                artist: getArtist(r),
                date:   getDate(r),
            });
        }
        const total = Number(d.found || 0);
        return { items, total };
    }

    thumbnailUrl(imageId, size = 64) {
        return `${IIIF_PREFIX}/${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }

    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }

    // Fetch the artwork's title given the device's iiif_key (the JP2
    // filename, e.g. "bc386p50w_kksgb22235.tif.jp2"). The filename is
    // unique within SMK; a free-text `keys` search returns the matching
    // record as the only hit.
    async fetchTitleByIiifKey(iiifKey) {
        if (!iiifKey) return null;
        const params = new URLSearchParams({
            keys: String(iiifKey),
            rows: '1',
        });
        const d = await getJson(`${SEARCH}?${params}`);
        const recs = (d && Array.isArray(d.items)) ? d.items : [];
        if (recs.length === 0) return null;
        return getTitle(recs[0]);
    }
}
