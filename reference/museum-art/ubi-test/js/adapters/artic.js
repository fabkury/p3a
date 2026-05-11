// Art Institute of Chicago adapter — translates source/artic/run.py to JS.
//
// AIC has no flat "collection" concept; it exposes seven facet vocabularies.
// Six are filterable on the search endpoint (departments, classifications,
// subjects, themes, galleries, artwork-types) and one is list-only
// (exhibitions). The sub-selector exposes only the six filterable axes —
// exhibitions is intentionally hidden because clicking through to artwork
// listing isn't supported by AIC's search API for it.
//
// Per AIC's docs, every request carries an identifying AIC-User-Agent
// header. CORS preflight on the endpoint allows it.

import { Adapter } from './base.js';

const API = 'https://api.artic.edu/api/v1';
const SEARCH = `${API}/artworks/search`;
const IIIF_HOST = 'https://www.artic.edu/iiif/2';

const HEADERS = {
  'AIC-User-Agent': 'museum-art-ubi-test/0.1 (pub@kury.dev)',
};

const ARTWORK_FIELDS = 'id,title,image_id,artist_title,date_display';
const TERMS_PER_AXIS = 30;

const AXES = [
  { name: 'departments',     endpoint: `${API}/departments`,           extraParams: {}, filterField: 'department_id' },
  { name: 'classifications', endpoint: `${API}/category-terms/search`, extraParams: { 'query[term][subtype]': 'classification' }, filterField: 'classification_id' },
  { name: 'subjects',        endpoint: `${API}/category-terms/search`, extraParams: { 'query[term][subtype]': 'subject' },         filterField: 'subject_id' },
  { name: 'themes',          endpoint: `${API}/category-terms/search`, extraParams: { 'query[term][subtype]': 'theme' },           filterField: 'category_ids' },
  { name: 'galleries',       endpoint: `${API}/galleries`,             extraParams: {}, filterField: 'gallery_id' },
  { name: 'artwork-types',   endpoint: `${API}/artwork-types`,         extraParams: {}, filterField: 'artwork_type_id' },
];

const AXIS_BY_NAME = Object.fromEntries(AXES.map(a => [a.name, a]));

async function getJson(url) {
  const r = await fetch(url, { headers: HEADERS });
  if (!r.ok) throw new Error(`AIC ${r.status} ${url}`);
  return r.json();
}

function buildSearchParams(filterField, filterValue, page, limit) {
  const p = new URLSearchParams({
    page: String(page),
    limit: String(limit),
    fields: ARTWORK_FIELDS,
  });
  p.set(`query[term][${filterField}]`, String(filterValue));
  return p;
}

export class ArticAdapter extends Adapter {
  static get id() { return 'artic'; }
  static get displayName() { return 'Art Institute of Chicago'; }
  static get axes() { return AXES.map(a => a.name); }

  constructor() {
    super();
    // termsByAxis: axisName -> [{ id, label, count }]
    // Cached per-instance so re-listing in the same session is instant.
    this._termsByAxis = {};
  }

  async listCollections({ axis = 'departments' } = {}) {
    if (this._termsByAxis[axis]) return this._termsByAxis[axis];
    const ax = AXIS_BY_NAME[axis];
    if (!ax) throw new Error(`AIC: unknown axis ${axis}`);

    // 1) Pull up to TERMS_PER_AXIS terms in storage order.
    const params = new URLSearchParams({ limit: String(TERMS_PER_AXIS), page: '1' });
    for (const [k, v] of Object.entries(ax.extraParams)) params.set(k, v);
    const listing = await getJson(`${ax.endpoint}?${params}`);
    const terms = (listing.data || []).map(t => ({
      id: String(t.id),
      label: String(t.title || t.id),
    }));

    // 2) Probe artwork counts in parallel.  AIC's term endpoints don't
    //    expose counts directly, so we hit the search endpoint with
    //    limit=1 per term and read pagination.total.
    const counts = await Promise.all(terms.map(async t => {
      try {
        const sp = buildSearchParams(ax.filterField, t.id, 1, 1);
        const d = await getJson(`${SEARCH}?${sp}`);
        return Number(d?.pagination?.total) || 0;
      } catch {
        return 0;
      }
    }));
    const out = terms
      .map((t, i) => ({ ...t, count: counts[i] }))
      .filter(t => t.count > 0)
      .sort((a, b) => b.count - a.count);
    this._termsByAxis[axis] = out;
    return out;
  }

  async listArtworks(termId, { offset = 0, rows = 24, axis = 'departments' } = {}) {
    const ax = AXIS_BY_NAME[axis];
    if (!ax) throw new Error(`AIC: unknown axis ${axis}`);
    const page = Math.floor(offset / rows) + 1;
    const sp = buildSearchParams(ax.filterField, termId, page, rows);
    const d = await getJson(`${SEARCH}?${sp}`);
    const items = (d.data || []).map(it => ({
      id: String(it.id),
      title: it.title || '(untitled)',
      artist: it.artist_title || '',
      date: it.date_display || '',
    }));
    return { items, total: Number(d?.pagination?.total) || 0 };
  }

  async getArtwork(id) {
    const url = `${API}/artworks/${encodeURIComponent(id)}?fields=${encodeURIComponent(ARTWORK_FIELDS)}`;
    const d = await getJson(url);
    const rec = d?.data || {};
    if (!rec.image_id) throw new Error(`AIC artwork has no image_id: ${id}`);
    return {
      title: rec.title || '(untitled)',
      artist: rec.artist_title || '',
      date: rec.date_display || '',
      imageUrl: `${IIIF_HOST}/${rec.image_id}/full/!720,720/0/default.jpg`,
    };
  }
}
