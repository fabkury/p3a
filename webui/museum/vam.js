// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Victoria and Albert Museum browse adapter — ported from
// reference/museum-art/ubi-test/js/adapters/vam.js.
//
// V&A has no flat "collection" field. The UBI surfaces three facet axes
// (collection, category, venue) and the modal renders an axis-picker
// step for the user.
//
// Listing endpoint returns the IIIF identifier (_primaryImageId) inline
// for every record, so the modal can build thumbnails immediately —
// no equivalent of Rijks's three-hop Linked-Art walk is needed.

const SEARCH = 'https://api.vam.ac.uk/v2/objects/search';
const IIIF_HOST = 'https://framemark.vam.ac.uk/collections';

const FACET_PARAM = {
    collection: 'id_collection',
    category:   'id_category',
    venue:      'id_venue',
};

const AXIS_DISPLAY_LABELS = {
    collection: 'Collections',
    category:   'Categories',
    venue:      'Venues',
};

const CLUSTER_SIZE = 50;
const COUNT_PROBE_CONCURRENCY = 6;

async function getJson(url) {
    const r = await fetch(url);
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'vam',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`V&A 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`V&A ${r.status} ${url}`);
    return r.json();
}

function getTitle(item) {
    return item._primaryTitle || (item.objectType ? `(${item.objectType})` : '(untitled)');
}
function getArtist(item) {
    const m = item._primaryMaker;
    if (m && typeof m === 'object' && m.name) return String(m.name);
    return '';
}
function getDate(item) {
    return item._primaryDate ? String(item._primaryDate) : '';
}

export class VamAdapter {
    get id()          { return 'vam'; }
    get displayName() { return 'Victoria and Albert Museum'; }
    get shortName()   { return 'V&A'; }
    get axes() {
        return [
            { name: 'collection', label: AXIS_DISPLAY_LABELS.collection },
            { name: 'category',   label: AXIS_DISPLAY_LABELS.category   },
            { name: 'venue',      label: AXIS_DISPLAY_LABELS.venue      },
        ];
    }

    constructor() {
        // termsByAxis: axisName -> [{ id, label, count }] cached per session.
        this._termsByAxis = Object.create(null);
    }

    async listCollections({ axis = 'collection' } = {}) {
        if (!FACET_PARAM[axis]) throw new Error(`V&A: unknown axis ${axis}`);
        if (this._termsByAxis[axis]) return this._termsByAxis[axis];

        // The V&A search API returns count=0 for venue terms when combined
        // with images_exist=1 (the API doesn't compute that combination),
        // so we re-probe venue without the filter purely for facet
        // enumeration. Actual artwork listings inside a venue facet still
        // apply images_exist=1.
        const params = new URLSearchParams({
            page: '1',
            page_size: '1',
            cluster: 'true',
            cluster_size: String(CLUSTER_SIZE),
        });
        if (axis !== 'venue') params.set('images_exist', '1');

        const d = await getJson(`${SEARCH}?${params}`);
        const rawTerms = (d && d.clusters && d.clusters[axis] && d.clusters[axis].terms) || [];

        let terms = rawTerms
            .filter(t => t && t.id)
            .map(t => ({ id: String(t.id), label: String(t.value || t.id), count: Number(t.count || 0) }));

        // For venue, the count above was probed without images_exist; the
        // user wants to see how many image-bearing artworks each venue
        // has, so re-probe per term. Bounded-concurrency to stay polite.
        if (axis === 'venue' && terms.length > 0) {
            terms = await mapWithConcurrency(terms, COUNT_PROBE_CONCURRENCY, async (t) => {
                try {
                    const probe = new URLSearchParams({
                        page: '1',
                        page_size: '1',
                        images_exist: '1',
                        [FACET_PARAM[axis]]: t.id,
                    });
                    const dd = await getJson(`${SEARCH}?${probe}`);
                    const total = (dd && dd.info && dd.info.record_count) | 0;
                    return Object.assign({}, t, { count: total });
                } catch (_) {
                    return Object.assign({}, t, { count: 0 });
                }
            });
        }

        const out = terms.filter(t => t.count > 0).sort((a, b) => b.count - a.count);
        this._termsByAxis[axis] = out;
        return out;
    }

    async listArtworks(termId, { offset = 0, rows = 8, axis = 'collection' } = {}) {
        if (!FACET_PARAM[axis]) throw new Error(`V&A: unknown axis ${axis}`);
        // V&A only supports page+page_size; (offset, rows) -> (page, page_size).
        const page = Math.floor(offset / rows) + 1;
        const params = new URLSearchParams({
            page: String(page),
            page_size: String(rows),
            images_exist: '1',
            [FACET_PARAM[axis]]: termId,
        });
        const d = await getJson(`${SEARCH}?${params}`);
        const items = (d.records || [])
            .filter(it => it && it._primaryImageId)
            .map(it => ({
                id: String(it.systemNumber || ''),
                imageId: String(it._primaryImageId),
                title:  getTitle(it),
                artist: getArtist(it),
                date:   getDate(it),
            }));
        const total = (d && d.info && d.info.record_count) | 0;
        return { items, total };
    }

    thumbnailUrl(imageId, size = 64) {
        return `${IIIF_HOST}/${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }
}

// Bounded concurrency runner — same shape as artic.js's helper.
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
