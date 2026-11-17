// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Harvard Art Museums browse adapter — hybrid discovery model.
//
// Unlike the other adapters, HAM ships no API key. The user enters one
// via Settings > Museums (saved as `ham_api_key` in NVS); the adapter
// reads it lazily from /config on first use. If the key is empty, the
// adapter throws a recognizable error so the modal can show "enter your
// key" instead of a generic failure.
//
// HAM's API surface is unusually uniform across facets: endpoint name ==
// filter-param name == term resource path. So instead of hardcoding a
// curated subset of axes, the adapter declares a display-label map +
// skip-list at compile time and discovers terms / counts at runtime from
// whatever HAM actually returns. The label-length filter (drop labels >
// 32 chars, the playset identifier-slot ceiling) is applied uniformly.
//
// Image flow: listing returns `primaryimageurl` of the form
// `https://nrs.harvard.edu/urn-3:HUAM:NNNN_dynmc`. The browser uses it
// as-is with IIIF size syntax appended; the NRS host 303-redirects to
// the IDS image server transparently. The device-side adapter strips
// the host prefix to fit the URN in the 48-byte iiif_key slot.
//
// See docs/art-institutions/ham-investigation/REPORT.md for the
// investigation and finalized-design.md §9 for the per-museum spec.

const API_ROOT = 'https://api.harvardartmuseums.org';
const NRS_PREFIX = 'https://nrs.harvard.edu/';
const PAGE_SIZE = 100;
const MAX_LABEL_CHARS = 32;  // playset identifier[33] slot

// Compile-time axis surface. Order matches the curated browse order in
// REPORT.md §"Axes (filterable, in browse order)". The adapter discovers
// terms within each axis at runtime; an axis with zero usable terms is
// hidden by the modal automatically (renderTermsStep shows "no terms").
const AXES = [
    { name: 'classification', label: 'Classifications' },
    { name: 'century',        label: 'Centuries'       },
    { name: 'culture',        label: 'Cultures'        },
    { name: 'period',         label: 'Periods'         },
    { name: 'place',          label: 'Places'          },
    { name: 'medium',         label: 'Media'           },
    { name: 'technique',      label: 'Techniques'      },
    { name: 'worktype',       label: 'Work types'      },
    { name: 'group',          label: 'Groups'          },
    { name: 'gallery',        label: 'Galleries'       },
];

// Skip-list (Stage 2 findings):
//   - color  : /color vocabulary returns CSS color names but the color
//              filter on /object is inert (returns 0 records for every
//              value). Filter pathway is broken; the data lives only as
//              per-object `colors[]` arrays.
//   - person : 42 549 terms. Browse modal can't reasonably enumerate a
//              flat 42 k picker; deferred to a future keyword-search
//              feature.

async function loadConfigKey() {
    const r = await fetch('/config', { credentials: 'same-origin' });
    if (!r.ok) throw new Error(`/config returned HTTP ${r.status}`);
    const j = await r.json();
    const cfg = (j && j.ok && j.data) ? j.data : {};
    return typeof cfg.ham_api_key === 'string' ? cfg.ham_api_key.trim() : '';
}

function makeNoKeyError() {
    const err = new Error('HAM API key not configured');
    err.code = 'HAM_NO_KEY';
    err.userMessage = 'Enter your Harvard Art Museums API key in Settings → Museums to browse this museum.';
    return err;
}

async function getJsonWithKey(url, apiKey) {
    // Append apikey as the first query param. The adapter callers build
    // URLs without it so this helper centralizes attaching the key.
    const sep = url.includes('?') ? '&' : '?';
    const finalUrl = `${url}${sep}apikey=${encodeURIComponent(apiKey)}`;
    const r = await fetch(finalUrl);
    if (r.status === 401) {
        const err = makeNoKeyError();
        err.userMessage = 'Harvard Art Museums rejected the saved API key (401). Update it in Settings → Museums.';
        throw err;
    }
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'ham',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`HAM 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`HAM ${r.status} ${url}`);
    const j = await r.json();
    // HAM answers a not-found object id or a malformed query with HTTP 200 and
    // a body of {"error": "..."} — it does NOT use a 4xx status. Treat a
    // top-level `error` as a hard failure so callers reject (and the info panel
    // logs + shows "—") instead of silently parsing all-null metadata out of an
    // error envelope. Listing/search responses carry {info, records} and never
    // a top-level `error`, so this only fires on genuine failures.
    if (j && typeof j === 'object' && j.error) {
        throw new Error(`HAM API error: ${j.error} (${url})`);
    }
    return j;
}

function getPeopleDisplay(rec) {
    const ppl = rec && rec.people;
    if (Array.isArray(ppl) && ppl.length > 0) {
        const first = ppl[0];
        if (first && first.displayname) return String(first.displayname);
    }
    return '';
}

function extractUrn(primaryUrl) {
    if (typeof primaryUrl !== 'string') return null;
    if (!primaryUrl.startsWith(NRS_PREFIX)) return null;
    return primaryUrl.substring(NRS_PREFIX.length);
}

export class HamAdapter {
    get id()          { return 'ham'; }
    get displayName() { return 'Harvard Art Museums'; }
    get shortName()   { return 'HAM'; }
    get axes()        { return AXES.slice(); }

    constructor() {
        // Lazy-loaded API key (a Promise<string>, '' if unset).
        this._keyPromise = null;
        // termsByAxis: axisName -> [{ id, label, count }] cached per session.
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

    async listCollections({ axis = 'classification' } = {}) {
        const ax = AXES.find(a => a.name === axis);
        if (!ax) throw new Error(`HAM: unknown axis ${axis}`);
        if (this._termsByAxis[axis]) return this._termsByAxis[axis];

        const apiKey = await this._getKey();

        // Sort by objectcount desc so the most populous terms come back on
        // page 1. For axes whose vocabulary doesn't surface objectcount
        // (worktype, group), the sort is a no-op and the terms come out in
        // the API's natural order; the alphabetic re-sort below fixes that.
        const url = `${API_ROOT}/${encodeURIComponent(axis)}`
            + `?size=100&page=1&sort=objectcount&sortorder=desc`;
        const data = await getJsonWithKey(url, apiKey);
        const records = Array.isArray(data && data.records) ? data.records : [];

        // Two ordering rules, decided by whether objectcount is populated on
        // the first record. HAM's `worktype` and `group` vocabularies have
        // `objectcount: 0` on every term, so we fall back to alphabetical
        // ordering (the filter itself still works — just no popularity hint).
        const sample = records[0];
        const hasCounts = sample && typeof sample.objectcount === 'number' && sample.objectcount > 0;

        const terms = [];
        for (const r of records) {
            const name = typeof r.name === 'string' ? r.name : null;
            if (!name) continue;
            if (name.length > MAX_LABEL_CHARS) continue;  // identifier slot
            const count = typeof r.objectcount === 'number' ? r.objectcount : 0;
            if (hasCounts && count <= 0) continue;
            // Term-id field varies by axis (`classificationid`, `galleryid`,
            // `periodid`, etc.) but the generic `id` is always present too.
            const id = r.id != null ? String(r.id) : null;
            if (!id) continue;
            terms.push({ id, label: name, count });
        }

        if (hasCounts) {
            terms.sort((a, b) => b.count - a.count);
        } else {
            terms.sort((a, b) => a.label.localeCompare(b.label));
        }

        this._termsByAxis[axis] = terms;
        return terms;
    }

    async listArtworks(termId, { offset = 0, rows = 20, axis = 'classification' } = {}) {
        if (!AXES.find(a => a.name === axis)) throw new Error(`HAM: unknown axis ${axis}`);

        const apiKey = await this._getKey();

        // HAM uses page+size only (no offset). Translate (offset, rows) by
        // requesting the slice that contains `offset`. Since `rows` (20) is
        // smaller than PAGE_SIZE (100), one page request covers `rows`
        // entries starting at the within-page offset. For larger row
        // requests we'd need to fetch multiple pages; not needed for the
        // browse modal today.
        const page = Math.floor(offset / PAGE_SIZE) + 1;
        const skip = offset % PAGE_SIZE;

        // `q=imagepermissionlevel:0` is required to suppress permission-
        // restricted records (Q1 of the investigation REPORT.md).
        const params = new URLSearchParams({
            size: String(PAGE_SIZE),
            page: String(page),
            hasimage: '1',
            sort: 'id',
            sortorder: 'asc',
            q: 'imagepermissionlevel:0',
            fields: 'id,objectnumber,title,dated,people,primaryimageurl',
            [axis]: String(termId),
        });

        const data = await getJsonWithKey(`${API_ROOT}/object?${params}`, apiKey);
        const records = Array.isArray(data && data.records) ? data.records : [];
        const total = (data && data.info && Number(data.info.totalrecords)) || 0;

        const items = [];
        for (let i = skip; i < records.length && items.length < rows; i++) {
            const r = records[i];
            if (!r) continue;
            const primary = typeof r.primaryimageurl === 'string' ? r.primaryimageurl : '';
            const urn = extractUrn(primary);
            if (!urn) continue;  // permission-restricted or non-NRS host
            items.push({
                id:      r.id != null ? String(r.id) : (r.objectnumber || ''),
                imageId: urn,             // URN portion, mirrors device-side iiif_key
                title:   r.title || '(untitled)',
                artist:  getPeopleDisplay(r),
                date:    r.dated || '',
            });
        }
        return { items, total };
    }

    thumbnailUrl(imageId, size = 64) {
        // imageId is the URN portion (set in listArtworks). Build the NRS
        // URL with IIIF size syntax; the host transparently redirects to
        // ids.lib.harvard.edu and preserves the path.
        return `${NRS_PREFIX}${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }

    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }

    // Fetch title + artist + date given the device's iiif_key (the URN
    // portion of primaryimageurl, e.g. "urn-3:HUAM:79762_dynmc",
    // "urn-3:HUAM:INV056029_dynmc", or occasionally "urn-3:HUAM:802142" with
    // no suffix). The token between "HUAM:" and the optional "_dynmc" is HAM's
    // `renditionnumber` — NOT the object id (object 1429's rendition is 79762),
    // and it can be non-numeric. So we recover the object by searching the
    // image sub-field instead of hitting /object/{id}: a rendition number used
    // as an object id 200s with {"error":"Not found"}, which getJsonWithKey now
    // rejects. `q=images.renditionnumber:{rendition}` returns the one matching
    // object (verified against the live API for both numeric and INV-prefixed
    // renditions). `images.imageid` would work equally well as a fallback.
    //
    // Requires a configured API key — propagates makeNoKeyError() when
    // absent so the panel shows "—" without exposing a stack trace.
    async fetchMetadataByIiifKey(iiifKey) {
        if (!iiifKey) return { title: null, artist: null, date: null };
        // Capture the rendition token: everything between "urn-3:HUAM:" and the
        // optional "_dynmc" suffix. Non-greedy so "_dynmc" is stripped when
        // present and the whole tail is kept when it's absent.
        const m = /^urn-3:HUAM:(.+?)(?:_dynmc)?$/.exec(iiifKey);
        if (!m) return { title: null, artist: null, date: null };
        const rendition = m[1];

        const apiKey = await this._getKey();
        const sp = new URLSearchParams({
            q: `images.renditionnumber:${rendition}`,
            size: '1',
            fields: 'title,people,dated',
        });
        const data = await getJsonWithKey(`${API_ROOT}/object?${sp}`, apiKey);
        const records = Array.isArray(data && data.records) ? data.records : [];
        if (records.length === 0) return { title: null, artist: null, date: null };
        const rec = records[0];
        const artist = getPeopleDisplay(rec);  // '' when missing
        return {
            title:  rec.title ? String(rec.title) : null,
            artist: artist    || null,
            date:   rec.dated ? String(rec.dated) : null,
        };
    }
}
