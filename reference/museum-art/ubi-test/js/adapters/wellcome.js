// Wellcome Collection adapter — translates source/wellcome/run.py to JS.
//
// Wellcome's catalogue mixes books, manuscripts, archives, and images. The
// `items.locations.locationType=iiif-image` filter restricts results to
// works that have a IIIF Image service. Four facet axes (workType, genres,
// subjects, contributors) qualify as "collections" and are exposed via a
// sub-selector. All four are filterable directly via query parameters.

import { Adapter } from './base.js';

const API = 'https://api.wellcomecollection.org/catalogue/v2';
const WORKS = `${API}/works`;
const IIIF_FILTER = { 'items.locations.locationType': 'iiif-image' };
const RESULT_INCLUDES = 'items,contributors,production,subjects,genres';

const AXES = [
  { name: 'workType',     agg: 'workType',                 filter: 'workType',                 keyField: 'id' },
  { name: 'genres',       agg: 'genres.label',             filter: 'genres.label',             keyField: 'label' },
  { name: 'subjects',     agg: 'subjects.label',           filter: 'subjects.label',           keyField: 'label' },
  { name: 'contributors', agg: 'contributors.agent.label', filter: 'contributors.agent.label', keyField: 'label' },
];
const AXIS_BY_NAME = Object.fromEntries(AXES.map(a => [a.name, a]));
const INFO_JSON_RE = /^(https:\/\/iiif\.wellcomecollection\.org\/image\/[^/]+)\/info\.json$/;

async function getJson(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`Wellcome ${r.status} ${url}`);
  return r.json();
}

function iiifBaseForWork(work) {
  for (const item of (work?.items || [])) {
    for (const loc of (item?.locations || [])) {
      if (loc?.locationType?.id !== 'iiif-image') continue;
      const url = loc?.url || '';
      const m = INFO_JSON_RE.exec(url);
      if (m) return m[1];
      if (url.endsWith('/info.json')) return url.slice(0, -'/info.json'.length);
      if (url) return url.replace(/\/$/, '');
    }
  }
  return null;
}

function getTitle(w)  { return w?.title ? String(w.title) : '(untitled)'; }
function getArtist(w) {
  const a = (w?.contributors || [])[0]?.agent;
  return a?.label ? String(a.label) : '';
}
function getDate(w) {
  const d = (w?.production || [])[0]?.dates?.[0];
  return d?.label ? String(d.label) : '';
}

export class WellcomeAdapter extends Adapter {
  static get id() { return 'wellcome'; }
  static get displayName() { return 'Wellcome Collection'; }
  static get axes() { return AXES.map(a => a.name); }

  constructor() {
    super();
    // Aggregations come back for all four axes in one request; cache the
    // whole response keyed implicitly (only one variant — image-bearing).
    this._allAggs = null;
  }

  async _loadAggs() {
    if (this._allAggs) return this._allAggs;
    const params = new URLSearchParams({
      pageSize: '1',
      aggregations: AXES.map(a => a.agg).join(','),
      ...IIIF_FILTER,
    });
    const d = await getJson(`${WORKS}?${params}`);
    this._allAggs = d?.aggregations || {};
    return this._allAggs;
  }

  async listCollections({ axis = 'workType' } = {}) {
    const ax = AXIS_BY_NAME[axis];
    if (!ax) throw new Error(`Wellcome: unknown axis ${axis}`);
    const aggs = await this._loadAggs();
    const buckets = aggs?.[ax.agg]?.buckets || [];
    return buckets
      .map(b => {
        const data = b?.data || {};
        const id = data.id ?? data.label;
        const label = data.label ?? data.id;
        if (!id || !label) return null;
        return {
          id: String(ax.keyField === 'id' ? data.id : data.label),
          label: String(label),
          count: Number(b?.count) || 0,
        };
      })
      .filter(Boolean)
      .sort((a, b) => b.count - a.count);
  }

  async listArtworks(termId, { offset = 0, rows = 24, axis = 'workType' } = {}) {
    const ax = AXIS_BY_NAME[axis];
    if (!ax) throw new Error(`Wellcome: unknown axis ${axis}`);
    const page = Math.floor(offset / rows) + 1;
    const params = new URLSearchParams({
      page: String(page),
      pageSize: String(rows),
      include: RESULT_INCLUDES,
      ...IIIF_FILTER,
    });
    params.set(ax.filter, termId);
    const d = await getJson(`${WORKS}?${params}`);
    const items = (d.results || []).map(w => ({
      id: String(w.id),
      title: getTitle(w),
      artist: getArtist(w),
      date: getDate(w),
    }));
    return { items, total: Number(d?.totalResults) || 0 };
  }

  async getArtwork(id) {
    const url = `${WORKS}/${encodeURIComponent(id)}?include=${encodeURIComponent('items,contributors,production')}`;
    const w = await getJson(url);
    const base = iiifBaseForWork(w);
    if (!base) throw new Error(`Wellcome work has no iiif-image location: ${id}`);
    return {
      title: getTitle(w),
      artist: getArtist(w),
      date: getDate(w),
      imageUrl: `${base}/full/!720,720/0/default.jpg`,
    };
  }
}
