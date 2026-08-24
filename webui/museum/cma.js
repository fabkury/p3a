// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Cleveland Museum of Art browse adapter — baked-terms model.
//
// CMA is the first non-IIIF museum: its Open Access API is anonymous
// (no key) and every search record carries fixed CDN rendition URLs.
// The mid-size "web" rendition follows a stable template derivable from
// the accession number, so previewUrl() ignores the requested size and
// returns that template directly (~750-1300 px, median ~300 KB — the
// only rendition below print/full, so previews are heavier than the
// IIIF museums' !400,400 requests).
//
// CMA has no facet-enumeration endpoint, so the department/type term
// lists are baked into /museum/cma-terms.json at release time by
// scripts/build_cma_terms.py (rijks-sets.json pattern). The API's
// department=/type= filters demand EXACT full names; two department
// names exceed the 32-char playset identifier slot, so their baked
// entries store a truncated `id` plus a `query` field with the full
// name. This adapter expands id -> query before every API call; the
// device-side mirror is CMA_TERM_EXPANSION in
// components/art_institution/museums/cma.c.
//
// Unlike ham/smk/wellcome, no MAX_LABEL_CHARS gate is applied here:
// baked ids are <= 32 chars by construction, and gating on the (full)
// label would wrongly drop the two truncated departments.
//
// See reference/museum-art/source/cma/output/report.md for the API
// investigation and finalized-design.md §9.6 for the per-museum spec.

const API_ROOT = 'https://openaccess-api.clevelandart.org/api/artworks/';
const CDN_ROOT = 'https://openaccess-cdn.clevelandart.org';
const TERMS_URL = '/museum/cma-terms.json';

const AXES = [
    { name: 'department', label: 'Departments' },
    { name: 'type',       label: 'Types'       },
];

async function getJson(url) {
    const r = await fetch(url);
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'cma',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`CMA 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`CMA ${r.status} ${url}`);
    return r.json();
}

function webUrl(accession) {
    const a = encodeURIComponent(accession);
    return `${CDN_ROOT}/${a}/${a}_web.jpg`;
}

function getCreatorDisplay(rec) {
    const creators = rec && rec.creators;
    if (Array.isArray(creators) && creators.length > 0) {
        const first = creators[0];
        if (first && first.description) return String(first.description);
    }
    return '';
}

export class CmaAdapter {
    get id()          { return 'cma'; }
    get displayName() { return 'Cleveland Museum of Art'; }
    get shortName()   { return 'CMA'; }
    get axes()        { return AXES.slice(); }

    constructor() {
        // Lazy-loaded baked terms (a Promise for the parsed JSON).
        this._termsPromise = null;
        // id -> exact API query value (differs only for truncated ids).
        this._queryById = new Map();
    }

    async _loadTerms() {
        if (!this._termsPromise) {
            this._termsPromise = fetch(TERMS_URL, { cache: 'no-store' })
                .then(r => {
                    if (!r.ok) throw new Error(`CMA terms fetch failed: HTTP ${r.status}`);
                    return r.json();
                })
                .then(j => {
                    const axes = (j && j.axes) || {};
                    for (const axis of Object.keys(axes)) {
                        for (const t of axes[axis]) {
                            this._queryById.set(t.id, t.query || t.id);
                        }
                    }
                    return axes;
                });
        }
        return this._termsPromise;
    }

    _expandTerm(termId) {
        return this._queryById.get(termId) || termId;
    }

    async listCollections({ axis = 'department' } = {}) {
        const ax = AXES.find(a => a.name === axis);
        if (!ax) throw new Error(`CMA: unknown axis ${axis}`);
        const axes = await this._loadTerms();
        const terms = Array.isArray(axes[axis]) ? axes[axis] : [];
        // Already sorted by count desc at bake time. Counts are as of the
        // bake date; listArtworks' live info.total is the authoritative
        // number once a term is opened.
        return terms.map(t => ({ id: t.id, label: t.label, count: t.count }));
    }

    async listArtworks(termId, { offset = 0, rows = 20, axis = 'department' } = {}) {
        if (!AXES.find(a => a.name === axis)) throw new Error(`CMA: unknown axis ${axis}`);
        await this._loadTerms();

        // Native skip/limit pagination — deep offsets verified working.
        const params = new URLSearchParams({
            cc0: '1',
            has_image: '1',
            skip: String(offset),
            limit: String(rows),
            [axis]: this._expandTerm(termId),
            fields: 'accession_number,title,creation_date,creators,images',
        });

        const data = await getJson(`${API_ROOT}?${params}`);
        const records = Array.isArray(data && data.data) ? data.data : [];
        const total = (data && data.info && Number(data.info.total)) || 0;

        const items = [];
        for (const r of records) {
            if (!r) continue;
            const acc = typeof r.accession_number === 'string' ? r.accession_number : '';
            const web = r.images && r.images.web;
            // Require the live web URL: the device rebuilds the same
            // template from the accession, so a record without it would
            // 404 at download time (~2% carry empty image metadata).
            if (!acc || !web || typeof web.url !== 'string' || !web.url) continue;
            items.push({
                id:      acc,
                imageId: acc,             // accession, mirrors device-side iiif_key
                title:   r.title || '(untitled)',
                artist:  getCreatorDisplay(r),
                date:    r.creation_date || '',
            });
        }
        return { items, total };
    }

    thumbnailUrl(imageId, size = 64) {
        // Fixed rendition — `size` is unavoidably ignored (no smaller
        // derivative exists below web).
        void size;
        return webUrl(imageId);
    }

    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }

    // Fetch title + artist + date given the device's iiif_key (the
    // accession number). CMA's single-record endpoint accepts the
    // accession directly: GET /api/artworks/{accession} -> { data: {...} }.
    async fetchMetadataByIiifKey(iiifKey) {
        if (!iiifKey) return { title: null, artist: null, date: null };
        const data = await getJson(`${API_ROOT}${encodeURIComponent(iiifKey)}`);
        const rec = data && data.data;
        if (!rec || typeof rec !== 'object') {
            return { title: null, artist: null, date: null };
        }
        const artist = getCreatorDisplay(rec);  // '' when missing
        return {
            title:  rec.title ? String(rec.title) : null,
            artist: artist    || null,
            date:   rec.creation_date ? String(rec.creation_date) : null,
        };
    }
}
