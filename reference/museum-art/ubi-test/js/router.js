// Hash router. Parses location.hash into { segs, query }.

export function parseHash() {
  const h = location.hash.replace(/^#/, '') || '/';
  const [path, queryStr = ''] = h.split('?');
  const segs = path.split('/').filter(Boolean);
  const query = Object.fromEntries(new URLSearchParams(queryStr));
  return { segs, query };
}

export function hash(parts, query) {
  const path = parts.map(p => encodeURIComponent(p)).join('/');
  const qs = query ? '?' + new URLSearchParams(query).toString() : '';
  return `#/${path}${qs}`;
}
