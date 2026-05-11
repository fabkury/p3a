// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Rijksmuseum browse adapter — ported from
// reference/museum-art/ubi-test/js/adapters/rijksmuseum.js.
//
// Rijks is axis-less: the only organizational facet is the curated
// "Set" list. The OAI-PMH endpoint at data.rijksmuseum.nl/oai doesn't
// expose CORS headers, so the sets list is pre-baked into
// /museum/rijks-sets.json by the device's LittleFS image; this adapter
// fetches that local copy and uses the Linked-Art search endpoint for
// per-set artwork listings.
//
// Per-artwork thumbnails are intentionally NOT pre-resolved here. The
// IIIF id is reachable only via a 3-hop Linked-Art walk
// (HMO -> VisualItem -> DigitalObject -> access_point), which would
// cost 8 * 3 = 24 HTTP requests to populate the modal's preview strip.
// listArtworks() instead returns items without imageId; the modal
// falls back to a textual card layout. The device does the walk lazily
// at download time (components/art_institution/museums/rijksmuseum.c).

const BASE = 'https://data.rijksmuseum.nl';
const ID_PREFIX = 'https://id.rijksmuseum.nl/';
// The bundled sets file is served by the device's HTTP server at the
// path below (the route mirrors /pico8/* and /static/*).
const SETS_URL = '/museum/rijks-sets.json';

async function fetchJsonLd(url) {
    const r = await fetch(url, { headers: { 'Accept': 'application/ld+json' } });
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'rijks',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`Rijks 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`Rijks ${r.status} ${url}`);
    return r.json();
}

function getTitle(hmo) {
    for (const n of (hmo && hmo.identified_by) || []) {
        if (n && n.type === 'Name' && n.content) return String(n.content);
    }
    return '(untitled)';
}

function getDate(hmo) {
    const ts = hmo && hmo.produced_by && hmo.produced_by.timespan;
    if (ts && Array.isArray(ts.identified_by)) {
        for (const n of ts.identified_by) {
            if (n && n.type === 'Name' && n.content) return String(n.content);
        }
    }
    return '';
}

function getArtist(hmo) {
    const carriers = hmo && hmo.produced_by && hmo.produced_by.carried_out_by;
    if (Array.isArray(carriers)) {
        for (const c of carriers) {
            if (c && c._label) return String(c._label);
        }
    }
    return '';
}

export class RijksmuseumAdapter {
    get id()          { return 'rijks'; }
    get displayName() { return 'Rijksmuseum'; }
    get shortName()   { return 'Rijks'; }
    // Axis-less: the modal skips the axis step and goes straight to the
    // term (set) list.
    get axes()        { return []; }

    constructor() {
        this._sets = null;
        this._counts = Object.create(null);
        // setSpec -> { items: [...], cursors: [url, ...], next: url|null }
        this._cursors = Object.create(null);
    }

    async _loadSets() {
        if (this._sets) return this._sets;
        const r = await fetch(SETS_URL, { cache: 'no-store' });
        if (!r.ok) throw new Error(`Failed to load rijks-sets.json (${r.status})`);
        this._sets = await r.json();
        return this._sets;
    }

    _setUrl(setSpec) {
        const memberId = `${ID_PREFIX}${setSpec}`;
        return `${BASE}/search/collection?memberOfSetId=${encodeURIComponent(memberId)}&imageAvailable=true`;
    }

    async listCollections() {
        const sets = await this._loadSets();
        // Probe partOf.totalItems for each set in parallel. Each request
        // is small (just the first page of a Linked-Art collection).
        // Concurrency is bounded by the browser's per-host connection
        // pool — no explicit cap needed for the Rijks endpoint, which is
        // light and doesn't enforce 60-req/min the way AIC does.
        const counts = await Promise.all(sets.map(async (s) => {
            if (this._counts[s.spec] !== undefined) return this._counts[s.spec];
            try {
                const d = await fetchJsonLd(this._setUrl(s.spec));
                const total = (d && d.partOf && typeof d.partOf.totalItems === 'number')
                    ? d.partOf.totalItems
                    : null;
                this._counts[s.spec] = total;
                return total;
            } catch (_) {
                this._counts[s.spec] = null;
                return null;
            }
        }));
        return sets
            .map((s, i) => ({ id: s.spec, label: s.name, count: counts[i] == null ? 0 : counts[i] }))
            .filter(t => t.count > 0)
            .sort((a, b) => b.count - a.count);
    }

    async listArtworks(setSpec, { offset = 0, rows = 8 } = {}) {
        if (!this._cursors[setSpec]) {
            this._cursors[setSpec] = { items: [], cursors: [this._setUrl(setSpec)] };
        }
        const cache = this._cursors[setSpec];

        // Walk forward until we have offset+rows items, or the next
        // pointer is null (end of collection).
        while (cache.items.length < offset + rows) {
            const url = cache.cursors[cache.cursors.length - 1];
            if (!url) break;
            const d = await fetchJsonLd(url);
            const items = d && Array.isArray(d.orderedItems) ? d.orderedItems : [];
            cache.items.push(...items);
            if (this._counts[setSpec] == null && d && d.partOf && typeof d.partOf.totalItems === 'number') {
                this._counts[setSpec] = d.partOf.totalItems;
            }
            const nx = (d && d.next && d.next.id) || null;
            cache.cursors[cache.cursors.length - 1] = url;
            cache.cursors.push(nx);
        }

        const slice = cache.items.slice(offset, offset + rows);
        // Hydrate metadata for the visible window in parallel. The 3-hop
        // walk for thumbnails is deliberately skipped — see file header.
        const hmos = await Promise.all(slice.map(it =>
            fetchJsonLd(it.id).catch(() => null)
        ));
        const out = slice.map((it, i) => {
            const hmo = hmos[i];
            return {
                id: it.id,                  // full HMO URL — passed as identifier to device
                imageId: null,              // intentionally absent; modal will render textual card
                title:  hmo ? getTitle(hmo)  : '(metadata unavailable)',
                artist: hmo ? getArtist(hmo) : '',
                date:   hmo ? getDate(hmo)   : '',
            };
        });
        return { items: out, total: this._counts[setSpec] };
    }

    // No usable thumbnail URL without the Linked-Art walk; signal "none"
    // so the modal renders a placeholder rather than a broken image.
    thumbnailUrl(_imageId, _size) {
        return null;
    }
}
