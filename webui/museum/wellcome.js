// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Wellcome Collection browse adapter.
//
// 4 facet axes (workType, genres, subjects, contributors). Term
// enumeration is a single aggregations request scoped to image-bearing
// works; no per-term count probe like AIC or V&A's venue axis needs.
//
// For non-workType axes Wellcome doesn't expose a stable short id, so
// the term label IS the filter value. We filter out terms whose label
// exceeds 32 chars to fit the playset identifier[33] slot (decision
// captured in docs/deferred/wellcome-long-labels.md).
//
// IIIF id (vid) lives nested under items[].locations[] where
// locationType.id == 'iiif-image'; refresh stores the vid as the
// iiif_key and the C-side build_iiif_url prepends the standard host.

const WORKS = 'https://api.wellcomecollection.org/catalogue/v2/works';
const IIIF_HOST = 'https://iiif.wellcomecollection.org/image';

const AXES = [
    { name: 'workType',     label: 'Work types',   filter: 'workType',                  agg: 'workType',                 keyField: 'id'    },
    { name: 'genres',       label: 'Genres',       filter: 'genres.label',              agg: 'genres.label',             keyField: 'label' },
    { name: 'subjects',     label: 'Subjects',     filter: 'subjects.label',            agg: 'subjects.label',           keyField: 'label' },
    { name: 'contributors', label: 'Contributors', filter: 'contributors.agent.label',  agg: 'contributors.agent.label', keyField: 'label' },
];

const AGG_BUCKET_SIZE = 100;   // (100) suffix on aggregations=
const MAX_LABEL_CHARS = 32;    // playset identifier[33] = 32 chars + null
const PAGE_SIZE = 100;         // matches C-side refresh

const IIIF_URL_RE = /^https:\/\/iiif\.wellcomecollection\.org\/image\/([^/]+)(?:\/info\.json)?$/;

function findAxis(name) {
    for (let i = 0; i < AXES.length; i++) {
        if (AXES[i].name === name) return AXES[i];
    }
    return null;
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
                    museum: 'wellcome',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`Wellcome 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`Wellcome ${r.status} ${url}`);
    return r.json();
}

function getTitle(work) {
    if (typeof work.title === 'string' && work.title) {
        // Wellcome titles can be paragraph-length; trim for preview captions.
        return work.title.length > 140 ? work.title.slice(0, 137) + '…' : work.title;
    }
    return '(untitled)';
}

function getArtist(work) {
    const contribs = work.contributors;
    if (Array.isArray(contribs) && contribs.length > 0) {
        const first = contribs[0];
        const agent = first && first.agent;
        if (agent && agent.label) return String(agent.label);
    }
    return '';
}

function getDate(work) {
    const prod = work.production;
    if (Array.isArray(prod) && prod.length > 0) {
        const first = prod[0];
        const dates = first && first.dates;
        if (Array.isArray(dates) && dates.length > 0 && dates[0].label) {
            return String(dates[0].label);
        }
    }
    return '';
}

function extractVid(work) {
    if (!work || !Array.isArray(work.items)) return null;
    for (const item of work.items) {
        if (!item || !Array.isArray(item.locations)) continue;
        for (const loc of item.locations) {
            const ltype = loc && loc.locationType && loc.locationType.id;
            if (ltype !== 'iiif-image') continue;
            const url = String(loc.url || '');
            const m = IIIF_URL_RE.exec(url);
            if (m) return m[1];
            // Fallback: strip trailing /info.json if present.
            if (url.startsWith(`${IIIF_HOST}/`)) {
                const tail = url.slice(`${IIIF_HOST}/`.length);
                const slash = tail.indexOf('/');
                return slash >= 0 ? tail.slice(0, slash) : tail;
            }
        }
    }
    return null;
}

export class WellcomeAdapter {
    get id()          { return 'wellcome'; }
    get displayName() { return 'Wellcome Collection'; }
    get shortName()   { return 'Wellcome'; }
    get axes() {
        return AXES.map(a => ({ name: a.name, label: a.label }));
    }

    constructor() {
        // termsByAxis: axisName -> [{ id, label, count }] cached per session.
        this._termsByAxis = Object.create(null);
        this._aggCache = null;  // raw aggregations response, lazily loaded
    }

    async _loadAggregations() {
        if (this._aggCache) return this._aggCache;
        const aggList = AXES.map(a => `${a.agg}(${AGG_BUCKET_SIZE})`).join(',');
        const url = `${WORKS}?pageSize=1`
            + `&items.locations.locationType=iiif-image`
            + `&aggregations=${encodeURIComponent(aggList)}`;
        this._aggCache = getJson(url);
        return this._aggCache;
    }

    async listCollections({ axis = 'workType' } = {}) {
        const axisDef = findAxis(axis);
        if (!axisDef) throw new Error(`Wellcome: unknown axis ${axis}`);
        if (this._termsByAxis[axis]) return this._termsByAxis[axis];

        const data = await this._loadAggregations();
        const aggs = (data && data.aggregations) || {};
        const block = aggs[axisDef.agg] || {};
        const buckets = Array.isArray(block.buckets) ? block.buckets : [];

        const out = [];
        for (const b of buckets) {
            const d = (b && b.data) || {};
            const id = d[axisDef.keyField] != null ? String(d[axisDef.keyField]) : null;
            const label = d.label != null ? String(d.label) : id;
            const count = Number((b && b.count) || 0);
            if (!id || !label) continue;
            if (count <= 0) continue;
            if (label.length > MAX_LABEL_CHARS) continue;  // identifier[33] gate
            out.push({ id, label, count });
        }
        out.sort((a, b) => b.count - a.count);
        this._termsByAxis[axis] = out;
        return out;
    }

    async listArtworks(termId, { offset = 0, rows = PAGE_SIZE, axis = 'workType' } = {}) {
        const axisDef = findAxis(axis);
        if (!axisDef) throw new Error(`Wellcome: unknown axis ${axis}`);
        const page = Math.floor(offset / rows) + 1;
        const params = new URLSearchParams({
            page: String(page),
            pageSize: String(rows),
            'items.locations.locationType': 'iiif-image',
            include: 'items',
            [axisDef.filter]: termId,
        });
        const d = await getJson(`${WORKS}?${params}`);
        const results = Array.isArray(d.results) ? d.results : [];
        const items = [];
        for (const w of results) {
            const vid = extractVid(w);
            if (!vid) continue;
            items.push({
                id: String(w.id || ''),
                imageId: vid,
                title:  getTitle(w),
                artist: getArtist(w),
                date:   getDate(w),
            });
        }
        const total = Number(d.totalResults || 0);
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
