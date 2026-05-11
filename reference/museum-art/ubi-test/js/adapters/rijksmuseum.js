// Rijksmuseum adapter — translates source/rijksmuseum/explore.py to JS.
//
// Sets list is pre-baked into data/rijks-sets.json (the OAI endpoint blocks CORS).
// Range pagination is emulated over cursor-only OrderedCollectionPage walks.
// IIIF URL is discovered via the 3-hop Linked Art chain
// HMO -> VisualItem -> DigitalObject -> access_point.

import { Adapter } from './base.js';

const BASE = 'https://data.rijksmuseum.nl';
const ID_PREFIX = 'https://id.rijksmuseum.nl/';
const MICRIO_RE = /^(https:\/\/iiif\.micr\.io\/[^/]+)/;

async function fetchJsonLd(url) {
  const r = await fetch(url, { headers: { 'Accept': 'application/ld+json' } });
  if (!r.ok) throw new Error(`Rijks ${r.status} ${url}`);
  return r.json();
}

function getTitle(hmo) {
  for (const n of (hmo?.identified_by || [])) {
    if (n.type === 'Name' && n.content) return String(n.content);
  }
  return '(untitled)';
}

function getDate(hmo) {
  const ts = hmo?.produced_by?.timespan;
  if (ts?.identified_by) {
    for (const n of ts.identified_by) {
      if (n.type === 'Name' && n.content) return String(n.content);
    }
  }
  return '';
}

function getArtist(hmo) {
  const carriers = hmo?.produced_by?.carried_out_by;
  if (Array.isArray(carriers)) {
    for (const c of carriers) {
      if (c?._label) return String(c._label);
    }
  }
  return '';
}

export class RijksmuseumAdapter extends Adapter {
  static get id() { return 'rijksmuseum'; }
  static get displayName() { return 'Rijksmuseum'; }
  static get axes() { return null; }

  constructor() {
    super();
    this._sets = null;          // [{ spec, name }]
    this._counts = {};          // setSpec -> totalItems (or null on failure)
    this._cursors = {};         // setSpec -> [pageUrl, pageUrl, ..., null?]
    this._items = {};           // setSpec -> [item, ...] accumulated as walked
  }

  async _loadSets() {
    if (this._sets) return this._sets;
    const r = await fetch('data/rijks-sets.json');
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
    // Fetch totalItems for each set in parallel via the search endpoint.
    // Each request is cheap (just a partOf.totalItems lookup on first page).
    const counts = await Promise.all(sets.map(async s => {
      if (this._counts[s.spec] !== undefined) return this._counts[s.spec];
      try {
        const d = await fetchJsonLd(this._setUrl(s.spec));
        const total = d?.partOf?.totalItems ?? null;
        this._counts[s.spec] = total;
        return total;
      } catch {
        this._counts[s.spec] = null;
        return null;
      }
    }));
    return sets
      .map((s, i) => ({ id: s.spec, label: s.name, count: counts[i] ?? undefined }))
      .filter(c => c.count === undefined || c.count > 0)
      .sort((a, b) => (b.count || 0) - (a.count || 0));
  }

  async listArtworks(setSpec, { offset = 0, rows = 24 } = {}) {
    if (!this._cursors[setSpec]) {
      this._cursors[setSpec] = [this._setUrl(setSpec)];
      this._items[setSpec] = [];
    }
    const cursors = this._cursors[setSpec];
    const items = this._items[setSpec];

    // Walk forward until we have offset+rows items, or the next pointer is null.
    while (items.length < offset + rows) {
      const url = cursors[cursors.length - 1];
      if (!url) break;  // exhausted
      const d = await fetchJsonLd(url);
      items.push(...(d.orderedItems || []));
      if (this._counts[setSpec] === undefined && d?.partOf?.totalItems !== undefined) {
        this._counts[setSpec] = d.partOf.totalItems;
      }
      const nx = d?.next?.id || null;
      cursors[cursors.length - 1] = url;  // already there
      cursors.push(nx);
    }

    const slice = items.slice(offset, offset + rows);
    // Hydrate metadata for the visible page in parallel.
    const hmos = await Promise.all(slice.map(it =>
      fetchJsonLd(it.id).catch(() => null)
    ));
    const out = slice.map((it, i) => {
      const hmo = hmos[i];
      return {
        id: it.id,                      // full URL; URL-encoded by caller in hash
        title: hmo ? getTitle(hmo) : '(metadata unavailable)',
        artist: hmo ? getArtist(hmo) : '',
        date: hmo ? getDate(hmo) : '',
      };
    });
    return { items: out, total: this._counts[setSpec] ?? null };
  }

  async getArtwork(hmoId) {
    const hmo = await fetchJsonLd(hmoId);
    let imageUrl = null;
    for (const showRef of (hmo.shows || [])) {
      let visual;
      try { visual = await fetchJsonLd(showRef.id); }
      catch { continue; }
      for (const doRef of (visual.digitally_shown_by || [])) {
        let digital;
        try { digital = await fetchJsonLd(doRef.id); }
        catch { continue; }
        for (const ap of (digital.access_point || [])) {
          const m = MICRIO_RE.exec(ap?.id || '');
          if (m) { imageUrl = `${m[1]}/full/!720,720/0/default.jpg`; break; }
        }
        if (imageUrl) break;
      }
      if (imageUrl) break;
    }
    if (!imageUrl) throw new Error('IIIF image URL not found in Linked Art chain');
    return {
      title: getTitle(hmo),
      artist: getArtist(hmo),
      date: getDate(hmo),
      imageUrl,
    };
  }
}
