// SMK adapter — translates source/smk/run.py to JS.
//
// Native offset pagination, full-text + facet search, direct IIIF URLs.

import { Adapter } from './base.js';

const SEARCH = 'https://api.smk.dk/api/v1/art/search';

function parseFacetPairs(raw) {
  if (!raw) return [];
  if (Array.isArray(raw)) {
    if (raw.length && typeof raw[0] !== 'object' && raw.length % 2 === 0) {
      const out = [];
      for (let i = 0; i < raw.length; i += 2) out.push([String(raw[i]), Number(raw[i + 1]) || 0]);
      return out;
    }
    if (raw.length && Array.isArray(raw[0])) {
      return raw.map(p => [String(p[0]), Number(p[1]) || 0]);
    }
    if (raw.length && typeof raw[0] === 'object') {
      return raw.map(e => [String(e.name ?? e.value ?? e.key ?? ''), Number(e.count ?? e.doc_count) || 0]);
    }
  }
  if (typeof raw === 'object') {
    return Object.entries(raw).map(([k, v]) => [String(k), Number(v) || 0]);
  }
  return [];
}

function getTitle(item) {
  const t = item.titles;
  if (Array.isArray(t)) {
    for (const e of t) {
      if (e && typeof e === 'object' && e.title) return String(e.title);
      if (typeof e === 'string') return e;
    }
  }
  if (typeof t === 'string') return t;
  return '(untitled)';
}

function getArtist(item) {
  const prod = item.production;
  if (Array.isArray(prod)) {
    for (const p of prod) {
      const c = p?.creator || p?.creator_forename || p?.creator_surname;
      if (c) return String(c);
    }
  }
  return '';
}

function getDate(item) {
  const prod = item.production;
  if (Array.isArray(prod) && prod[0]) {
    const d = prod[0].creation_date_start || prod[0].creation_date_end || '';
    return String(d).slice(0, 4);
  }
  return '';
}

export class SmkAdapter extends Adapter {
  static get id() { return 'smk'; }
  static get displayName() { return 'SMK — Statens Museum for Kunst'; }
  static get axes() { return null; }

  async listCollections() {
    const r = await fetch(`${SEARCH}?keys=*&rows=0&facets=collection`);
    if (!r.ok) throw new Error(`SMK ${r.status}`);
    const d = await r.json();
    const pairs = parseFacetPairs(d?.facets?.collection);
    pairs.sort((a, b) => b[1] - a[1]);
    return pairs.map(([name, count]) => ({ id: name, label: name, count }));
  }

  async listArtworks(collectionId, { offset = 0, rows = 24 } = {}) {
    const filters = `[collection:${collectionId}],[has_image:true]`;
    const url = `${SEARCH}?keys=*&offset=${offset}&rows=${rows}&filters=${encodeURIComponent(filters)}`;
    const r = await fetch(url);
    if (!r.ok) throw new Error(`SMK ${r.status}`);
    const d = await r.json();
    const items = (d.items || []).map(it => ({
      id: it.object_number || it.id,
      title: getTitle(it),
      artist: getArtist(it),
      date: getDate(it),
    }));
    return { items, total: d.found || 0 };
  }

  async getArtwork(id) {
    const filters = `[object_number:${id}]`;
    const url = `${SEARCH}?keys=*&rows=1&filters=${encodeURIComponent(filters)}`;
    const r = await fetch(url);
    if (!r.ok) throw new Error(`SMK ${r.status}`);
    const d = await r.json();
    const it = (d.items || [])[0];
    if (!it) throw new Error(`SMK artwork not found: ${id}`);
    if (!it.image_iiif_id) throw new Error(`SMK artwork has no IIIF image: ${id}`);
    return {
      title: getTitle(it),
      artist: getArtist(it),
      date: getDate(it),
      imageUrl: `${it.image_iiif_id}/full/!720,720/0/default.jpg`,
    };
  }
}
