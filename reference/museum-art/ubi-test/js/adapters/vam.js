// V&A adapter — translates source/vam/run.py to JS.
//
// V&A has no flat "collection" concept; the UBI exposes three axes
// (collection, category, venue) and the V&A page renders a sub-selector
// to pick which one drives listCollections.

import { Adapter } from './base.js';

const SEARCH = 'https://api.vam.ac.uk/v2/objects/search';
const OBJECT = 'https://api.vam.ac.uk/v2/object';  // detail endpoint: /v2/object/{systemNumber}
const IIIF_HOST = 'https://framemark.vam.ac.uk/collections';

const FACET_PARAM = {
  collection: 'id_collection',
  category: 'id_category',
  venue: 'id_venue',
};

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

export class VamAdapter extends Adapter {
  static get id() { return 'vam'; }
  static get displayName() { return 'V&A — Victoria and Albert Museum'; }
  static get axes() { return ['collection', 'category', 'venue']; }

  async listCollections({ axis = 'collection' } = {}) {
    if (!FACET_PARAM[axis]) throw new Error(`Unknown axis: ${axis}`);
    // venue counts return 0 when combined with images_exist=1 — re-probe
    // without the filter purely for facet enumeration.
    const params = new URLSearchParams({
      page: '1',
      page_size: '1',
      cluster: 'true',
      cluster_size: '50',
    });
    if (axis !== 'venue') params.set('images_exist', '1');
    const r = await fetch(`${SEARCH}?${params}`);
    if (!r.ok) throw new Error(`V&A ${r.status}`);
    const d = await r.json();
    const terms = d?.clusters?.[axis]?.terms || [];
    return terms
      .filter(t => t && t.id)
      .map(t => ({ id: String(t.id), label: String(t.value), count: Number(t.count || 0) }))
      .sort((a, b) => b.count - a.count);
  }

  async listArtworks(collectionId, { offset = 0, rows = 24, axis = 'collection' } = {}) {
    const param = FACET_PARAM[axis];
    if (!param) throw new Error(`Unknown axis: ${axis}`);
    // V&A only supports page+page_size; (offset, rows) -> (page, page_size).
    const page = Math.floor(offset / rows) + 1;
    const params = new URLSearchParams({
      page: String(page),
      page_size: String(rows),
      images_exist: '1',
      [param]: collectionId,
    });
    const r = await fetch(`${SEARCH}?${params}`);
    if (!r.ok) throw new Error(`V&A ${r.status}`);
    const d = await r.json();
    const items = (d.records || []).map(it => ({
      id: it.systemNumber,
      title: getTitle(it),
      artist: getArtist(it),
      date: getDate(it),
    }));
    return { items, total: d?.info?.record_count || 0 };
  }

  async getArtwork(systemNumber) {
    // The detail endpoint uses different field shapes from the search results.
    const r = await fetch(`${OBJECT}/${encodeURIComponent(systemNumber)}`);
    if (!r.ok) throw new Error(`V&A ${r.status} for ${systemNumber}`);
    const d = await r.json();
    const rec = d.record || d;
    const imageId = (rec.images && rec.images[0]) || null;
    if (!imageId) throw new Error(`V&A artwork has no images: ${systemNumber}`);
    const titleEntry = (rec.titles || []).find(t => t && t.title) || null;
    const title = titleEntry ? String(titleEntry.title) : (rec.objectType ? `(${rec.objectType})` : '(untitled)');
    const makerSrc = rec.artistMakerPerson || rec.artistMakerPeople || rec.artistMakerOrganisations;
    const maker = Array.isArray(makerSrc) ? makerSrc[0] : makerSrc;
    const artist = maker?.name?.text ? String(maker.name.text) : '';
    const dateEntry = (rec.productionDates || [])[0];
    const date = dateEntry?.date?.text ? String(dateEntry.date.text) : '';
    return {
      title,
      artist,
      date,
      imageUrl: `${IIIF_HOST}/${imageId}/full/!720,720/0/default.jpg`,
    };
  }
}
