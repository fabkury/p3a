// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Library of Congress browse adapter.
//
// Single axis (`format`). Three hardcoded terms: photo/print/drawing,
// manuscript/mixed material, 3d object. Term counts are populated by
// a per-term `c=1` probe on first axis open.
//
// LoC's listing returns a IIIF URL only on a subset of results
// (~8% for photo/print/drawing). The adapter filters results client-
// side to those with a tile.loc.gov/image-services/iiif/ URL whose
// extracted id is < 48 chars — matching the C-side refresh filter
// so what the user previews matches what the device will actually
// store.
//
// The 500-page cap (matching the C-side refresh) makes listArtworks
// safe to call without offset bounds.

const SEARCH = 'https://www.loc.gov/search/';
const IIIF_HOST = 'https://tile.loc.gov/image-services/iiif';

const TERMS = [
    { id: 'photo, print, drawing',       label: 'Photo, Print, Drawing' },
    { id: 'manuscript/mixed material',   label: 'Manuscript/Mixed Material' },
    { id: '3d object',                   label: '3D Object' },
];

const MAX_IIIF_KEY_LEN = 48;

async function getJson(url) {
    const r = await fetch(url);
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'loc',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`LoC 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`LoC ${r.status} ${url}`);
    return r.json();
}

function buildSearchUrl(termId, { c, sp }) {
    const params = new URLSearchParams({
        fo: 'json',
        c: String(c),
        sp: String(sp),
        fa: `original-format:${termId}`,
    });
    return `${SEARCH}?${params}`;
}

function extractIiifId(imageUrl) {
    if (typeof imageUrl !== 'string') return null;
    const prefix = `${IIIF_HOST}/`;
    if (!imageUrl.startsWith(prefix)) return null;
    const rest = imageUrl.slice(prefix.length);
    const slash = rest.indexOf('/');
    const id = slash >= 0 ? rest.slice(0, slash) : rest;
    if (!id || id.length >= MAX_IIIF_KEY_LEN) return null;
    return id;
}

function pickIiifId(result) {
    const images = Array.isArray(result.image_url) ? result.image_url : [];
    for (const u of images) {
        const id = extractIiifId(u);
        if (id) return id;
    }
    const resources = Array.isArray(result.resources) ? result.resources : [];
    for (const res of resources) {
        if (res && typeof res === 'object') {
            const id = extractIiifId(res.image);
            if (id) return id;
        }
    }
    return null;
}

function getTitle(result) {
    return result && result.title ? String(result.title) : '(untitled)';
}

function getDate(result) {
    return result && result.date ? String(result.date) : '';
}

function getArtist(result) {
    const contribs = result && result.contributor;
    if (Array.isArray(contribs) && contribs.length > 0) {
        return String(contribs[0]);
    }
    return '';
}

export class LocAdapter {
    get id()          { return 'loc'; }
    get displayName() { return 'Library of Congress'; }
    get shortName()   { return 'LoC'; }
    get axes() {
        return [{ name: 'format', label: 'Formats' }];
    }

    constructor() {
        this._terms = null;  // cached per session
    }

    async listCollections({ axis = 'format' } = {}) {
        if (axis !== 'format') throw new Error(`LoC: unknown axis ${axis}`);
        if (this._terms) return this._terms;

        // Probe each term in parallel for a count (concurrency = 3 since
        // the list is fixed and tiny).
        const probes = TERMS.map(async (t) => {
            try {
                const j = await getJson(buildSearchUrl(t.id, { c: 1, sp: 1 }));
                const total = (j && j.pagination && j.pagination.total) | 0;
                return { id: t.id, label: t.label, count: total };
            } catch (_) {
                return { id: t.id, label: t.label, count: 0 };
            }
        });
        const out = await Promise.all(probes);
        // Keep the configured order — Photos first, then Manuscripts,
        // then 3D. (Counts are informational, not ranking signal.)
        this._terms = out;
        return out;
    }

    async listArtworks(termId, { offset = 0, rows = 20, axis = 'format' } = {}) {
        if (axis !== 'format') throw new Error(`LoC: unknown axis ${axis}`);
        // LoC uses sp (1-based page) + c (per-page). Translate offset/rows.
        // We request c = max(rows, 100) so a single fetch typically yields
        // enough IIIF-bearing items for the preview after filtering. The
        // browse modal calls back for more when its buffer drains.
        const c = Math.max(rows, 100);
        const sp = Math.floor(offset / c) + 1;
        const url = buildSearchUrl(termId, { c, sp });
        const d = await getJson(url);
        const results = Array.isArray(d.results) ? d.results : [];
        const items = [];
        for (const r of results) {
            const iiifId = pickIiifId(r);
            if (!iiifId) continue;
            items.push({
                id: iiifId,
                imageId: iiifId,
                title: getTitle(r),
                artist: getArtist(r),
                date: getDate(r),
            });
            if (items.length >= rows) break;
        }
        const total = (d && d.pagination && d.pagination.total) | 0;
        return { items, total };
    }

    thumbnailUrl(imageId, size = 64) {
        return `${IIIF_HOST}/${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }

    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }
}
