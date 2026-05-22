// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Smithsonian Open Access browse adapter.
//
// Single axis (`unit`). The v1 unit list is hardcoded below (see
// ART_BEARING_UNITS): six art-bearing Smithsonian units whose IIIF coverage
// passed Phase A's ≥80% threshold (see
// reference/museum-art/source/smithsonian/output/report.md §A and
// DEFERRED.md for excluded units and future axis ideas).
//
// BYOK: Smithsonian's API requires an api.data.gov key. The user enters
// one via Settings > Museums (saved as `si_api_key` in NVS). When empty,
// the adapter throws a recognizable error so the modal can show
// "enter your key" instead of a generic failure.
//
// Image flow: search returns `content.descriptiveNonRepeating.online_media
// .media[*].idsId`. The browser stores the raw idsId as `imageId`; the
// thumbnail URL prepends https://ids.si.edu/ids/iiif/ and appends IIIF
// size syntax. The device-side adapter stores the same idsId verbatim in
// iiif_key and builds the same URL.

const SEARCH = 'https://api.si.edu/openaccess/api/v1.0/search';
const IIIF_PREFIX = 'https://ids.si.edu/ids/iiif/';

// v1 wired units, ordered by p3a-relevance: largest + most reliable first.
// Source: reference/museum-art/source/smithsonian/output/report.md.
// To add a unit (e.g. if NMAI or FSG become viable, or to expose NMAH for
// material-culture browsing), append here — the firmware accepts any
// unit_code as term_id, no C-side changes needed.
const ART_BEARING_UNITS = [
    { id: 'CHNDM',  label: 'Cooper Hewitt Design Museum' },
    { id: 'SAAM',   label: 'Smithsonian American Art Museum' },
    { id: 'NPG',    label: 'National Portrait Gallery' },
    { id: 'NMAAHC', label: 'African American History Museum' },
    { id: 'HMSG',   label: 'Hirshhorn Museum' },
    { id: 'NMAfA',  label: 'National Museum of African Art' },
];

const AXES = [
    { name: 'unit', label: 'Museums' },
];

async function loadConfigKey() {
    const r = await fetch('/config', { credentials: 'same-origin' });
    if (!r.ok) throw new Error(`/config returned HTTP ${r.status}`);
    const j = await r.json();
    const cfg = (j && j.ok && j.data) ? j.data : {};
    return typeof cfg.si_api_key === 'string' ? cfg.si_api_key.trim() : '';
}

function makeNoKeyError() {
    const err = new Error('Smithsonian API key not configured');
    err.code = 'SI_NO_KEY';
    err.userMessage = 'Enter your api.data.gov API key in Settings → Museums to browse the Smithsonian. Sign up free at https://api.data.gov/signup/.';
    return err;
}

async function getJsonWithKey(url, apiKey) {
    // Append api_key as the first query param. Callers build URLs without
    // it so this helper centralizes attaching the key.
    const sep = url.includes('?') ? '&' : '?';
    const finalUrl = `${url}${sep}api_key=${encodeURIComponent(apiKey)}`;
    const r = await fetch(finalUrl);
    if (r.status === 401 || r.status === 403) {
        const err = makeNoKeyError();
        err.userMessage = 'api.data.gov rejected the saved API key (' + r.status + '). Update it in Settings → Museums.';
        throw err;
    }
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'si',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`SI 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`SI ${r.status} ${url}`);
    return r.json();
}

// ── Field accessors (records are deeply nested + heterogeneous) ──

function safeGet(obj, ...path) {
    let cur = obj;
    for (const k of path) {
        if (cur == null) return null;
        if (typeof k === 'number') {
            if (!Array.isArray(cur) || k < 0 || k >= cur.length) return null;
            cur = cur[k];
        } else {
            if (typeof cur !== 'object') return null;
            cur = cur[k];
        }
    }
    return cur;
}

function getIdsId(item) {
    // media is sometimes an object, sometimes an array — handle both.
    const media = safeGet(item, 'content', 'descriptiveNonRepeating', 'online_media', 'media');
    if (Array.isArray(media)) {
        for (const m of media) {
            if (m && typeof m === 'object' && typeof m.idsId === 'string' && m.idsId) {
                return m.idsId;
            }
        }
    } else if (media && typeof media === 'object' && typeof media.idsId === 'string' && media.idsId) {
        return media.idsId;
    }
    return null;
}

function getTitle(item) {
    if (typeof item.title === 'string' && item.title.trim()) return item.title.trim();
    const nested = safeGet(item, 'content', 'descriptiveNonRepeating', 'title', 'content');
    if (typeof nested === 'string' && nested.trim()) return nested.trim();
    return '(untitled)';
}

function getArtist(item) {
    const names = safeGet(item, 'content', 'freetext', 'name');
    if (Array.isArray(names)) {
        // Prefer entries with an artist-like label.
        for (const n of names) {
            if (!n || typeof n !== 'object') continue;
            const label = String(n.label || '').toLowerCase();
            const content = n.content;
            if (typeof content === 'string' &&
                (label.includes('artist') || label.includes('maker') ||
                 label.includes('creator') || label.includes('designer') ||
                 label.includes('photographer') || label.includes('manufacturer'))) {
                return content;
            }
        }
        // Fall back to the first named entry of any kind.
        for (const n of names) {
            if (n && typeof n === 'object' && typeof n.content === 'string' && n.content) {
                return n.content;
            }
        }
    }
    const structured = safeGet(item, 'content', 'indexedStructured', 'name');
    if (Array.isArray(structured) && structured.length > 0) {
        const first = structured[0];
        if (typeof first === 'string') return first;
        if (first && typeof first === 'object' && typeof first.content === 'string') return first.content;
    }
    return '';
}

function getDate(item) {
    const dates = safeGet(item, 'content', 'freetext', 'date');
    if (Array.isArray(dates)) {
        for (const d of dates) {
            if (d && typeof d === 'object' && typeof d.content === 'string' && d.content) {
                return d.content;
            }
        }
    }
    const structured = safeGet(item, 'content', 'indexedStructured', 'date');
    if (Array.isArray(structured) && structured.length > 0) {
        return String(structured[0]);
    }
    return '';
}

export class SmithsonianAdapter {
    get id()          { return 'si'; }
    get displayName() { return 'Smithsonian'; }
    get shortName()   { return 'SI'; }
    get axes()        { return AXES.slice(); }

    constructor() {
        // Lazy-loaded API key (Promise<string>, '' if unset).
        this._keyPromise = null;
        // termsByAxis[axis] = [{ id, label, count }] cached per session.
        this._termsByAxis = Object.create(null);
    }

    async _getKey() {
        if (!this._keyPromise) {
            this._keyPromise = loadConfigKey().catch(() => '');
        }
        const key = await this._keyPromise;
        if (!key) throw makeNoKeyError();
        return key;
    }

    async listCollections({ axis = 'unit' } = {}) {
        if (axis !== 'unit') throw new Error(`SI: unknown axis ${axis}`);
        if (this._termsByAxis[axis]) return this._termsByAxis[axis];

        const apiKey = await this._getKey();

        // Probe each unit's count in parallel (rows=0). Six units total ⇒
        // six round-trips, well under any quota and fast enough for the
        // modal to render counts on first open.
        const probes = ART_BEARING_UNITS.map(async (u) => {
            const q = encodeURIComponent(`unit_code:${u.id} AND online_visual_material:true`);
            try {
                const data = await getJsonWithKey(`${SEARCH}?q=${q}&rows=0`, apiKey);
                const count = Number(safeGet(data, 'response', 'rowCount') || 0);
                return { id: u.id, label: u.label, count };
            } catch (e) {
                if (e && e.code === 'SI_NO_KEY') throw e;  // bubble up
                // Other failures: surface the unit with count=0 so the
                // modal still lets the user pick it.
                return { id: u.id, label: u.label, count: 0 };
            }
        });
        const terms = await Promise.all(probes);
        this._termsByAxis[axis] = terms;
        return terms;
    }

    async listArtworks(termId, { offset = 0, rows = 20, axis = 'unit' } = {}) {
        if (axis !== 'unit') throw new Error(`SI: unknown axis ${axis}`);
        const apiKey = await this._getKey();

        const q = encodeURIComponent(`unit_code:${termId} AND online_visual_material:true`);
        const url = `${SEARCH}?q=${q}&start=${offset}&rows=${rows}`;
        const data = await getJsonWithKey(url, apiKey);
        const records = safeGet(data, 'response', 'rows') || [];
        const total = Number(safeGet(data, 'response', 'rowCount') || 0);

        const items = [];
        for (const r of records) {
            const idsId = getIdsId(r);
            if (!idsId) continue;  // no displayable media
            items.push({
                id:      r && r.id != null ? String(r.id) : idsId,
                imageId: idsId,                 // mirrors device-side iiif_key
                title:   getTitle(r),
                artist:  getArtist(r),
                date:    getDate(r),
            });
        }
        return { items, total };
    }

    thumbnailUrl(imageId, size = 64) {
        // imageId is the idsId (e.g. "SAAM-1935.13.211_1"). Build the IIIF
        // URL with size syntax. Smithsonian's IDS server serves jpg.
        return `${IIIF_PREFIX}${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }

    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }

    // Look up title/artist/date for an item already in a channel cache.
    // The device stores the idsId in iiif_key; Smithsonian's full-text
    // search matches the idsId as a literal token in q=. Returns nulls on
    // miss so the panel renders "—" placeholders gracefully.
    async fetchMetadataByIiifKey(iiifKey) {
        if (!iiifKey) return { title: null, artist: null, date: null };
        const apiKey = await this._getKey().catch(() => null);
        if (!apiKey) return { title: null, artist: null, date: null };
        try {
            const q = encodeURIComponent(`"${iiifKey}"`);
            const data = await getJsonWithKey(`${SEARCH}?q=${q}&rows=1`, apiKey);
            const rows = safeGet(data, 'response', 'rows') || [];
            if (rows.length === 0) return { title: null, artist: null, date: null };
            const it = rows[0];
            const t = getTitle(it);
            const a = getArtist(it);
            const dt = getDate(it);
            return {
                title:  (t && t !== '(untitled)') ? t : null,
                artist: a  || null,
                date:   dt || null,
            };
        } catch (_) {
            return { title: null, artist: null, date: null };
        }
    }
}
