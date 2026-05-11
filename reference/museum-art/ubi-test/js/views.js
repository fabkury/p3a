import { getAdapter, listSources } from './adapters/index.js';

const esc = s => String(s ?? '').replace(/[&<>"']/g,
  c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

const fmtCount = n => (n == null ? '' : Number(n).toLocaleString());

function adapterCrumb(sourceId) {
  const A = getAdapter(sourceId).constructor;
  return `<a href="#/source/${sourceId}">${esc(A.displayName)}</a>`;
}

export async function landing(el) {
  const sources = listSources();
  el.innerHTML = `
    <h1>UBI-test</h1>
    <p>Pick a museum to explore. Each source is queried directly from the browser through a unified adapter interface.</p>
    <div class="cards">
      ${sources.map(s => `
        <a class="card" href="#/source/${s.id}">
          <h2>${esc(s.displayName)}</h2>
          <p>Open</p>
        </a>
      `).join('')}
    </div>
  `;
}

export async function museum(el, sourceId) {
  const A = getAdapter(sourceId).constructor;
  const axes = A.axes;
  el.innerHTML = `
    <p class="crumbs"><a href="#/">Home</a></p>
    <h1>${esc(A.displayName)}</h1>
    ${axes ? `
      <p>This source has ${axes.length} collection-like axes. Pick one before listing:</p>
      <p><label>Axis: <select id="axis">${axes.map(a => `<option value="${a}">${a}</option>`).join('')}</select></label></p>
    ` : ''}
    <p><button id="list-btn">List collections</button></p>
    <div id="result"></div>
  `;
  const btn = document.getElementById('list-btn');
  const result = document.getElementById('result');
  btn.addEventListener('click', async () => {
    btn.disabled = true;
    result.innerHTML = '<p class="loading">Loading collections…</p>';
    try {
      const axis = axes ? document.getElementById('axis').value : undefined;
      const adapter = getAdapter(sourceId);
      const cols = await adapter.listCollections(axis ? { axis } : {});
      if (cols.length === 0) {
        result.innerHTML = '<p>No collections returned.</p>';
        return;
      }
      const axisQS = axis ? `&axis=${encodeURIComponent(axis)}` : '';
      result.innerHTML = `
        <p><strong>${cols.length}</strong> collection(s):</p>
        <ul class="collections">
          ${cols.map(c => `
            <li>
              <a href="#/source/${sourceId}/collections/${encodeURIComponent(c.id)}?offset=0&rows=24${axisQS}">${esc(c.label)}</a>
              ${c.count !== undefined && c.count !== null
                ? ` <span class="count">— ${fmtCount(c.count)} artwork${c.count === 1 ? '' : 's'}</span>`
                : ''}
            </li>
          `).join('')}
        </ul>
      `;
    } catch (e) {
      result.innerHTML = `<div class="error"><strong>Error:</strong> ${esc(e.message)}</div>`;
    } finally {
      btn.disabled = false;
    }
  });
}

export async function collection(el, sourceId, collectionId, query) {
  const adapter = getAdapter(sourceId);
  const A = adapter.constructor;
  const offset = parseInt(query.offset || '0', 10);
  const rows = parseInt(query.rows || '24', 10);
  const axis = query.axis;
  const axisQS = axis ? `&axis=${encodeURIComponent(axis)}` : '';
  const baseHash = `#/source/${sourceId}/collections/${encodeURIComponent(collectionId)}`;

  const headerHtml = `
    <p class="crumbs"><a href="#/">Home</a> › ${adapterCrumb(sourceId)} › ${esc(collectionId)}</p>
    <h1>${esc(collectionId)}</h1>
  `;
  el.innerHTML = headerHtml + '<p class="loading">Loading artworks…</p>';

  try {
    const { items, total } = await adapter.listArtworks(collectionId, { offset, rows, axis });
    const totalStr = total != null ? ` of ${fmtCount(total)}` : '';
    const lastShown = items.length > 0 ? offset + items.length - 1 : offset;
    const prevOffset = Math.max(0, offset - rows);
    const nextOffset = offset + rows;
    const hasNext = items.length === rows && (total == null || nextOffset < total);

    el.innerHTML = headerHtml + `
      <p>Showing items <strong>${offset}</strong>–<strong>${lastShown}</strong>${totalStr}.</p>
      ${items.length === 0 ? '<p>No items at this offset.</p>' : `
        <ol class="artworks" start="${offset + 1}">
          ${items.map(it => `
            <li>
              <a href="#/source/${sourceId}/artwork/${encodeURIComponent(it.id)}">${esc(it.title)}</a>
              ${it.artist ? ` — <span class="artist">${esc(it.artist)}</span>` : ''}
              ${it.date ? ` <span class="date">(${esc(it.date)})</span>` : ''}
            </li>
          `).join('')}
        </ol>
      `}
      <p class="pager">
        ${offset > 0
          ? `<a href="${baseHash}?offset=${prevOffset}&rows=${rows}${axisQS}">← Prev</a>`
          : '<span class="disabled">← Prev</span>'}
        &nbsp;|&nbsp;
        ${hasNext
          ? `<a href="${baseHash}?offset=${nextOffset}&rows=${rows}${axisQS}">Next →</a>`
          : '<span class="disabled">Next →</span>'}
      </p>
    `;
  } catch (e) {
    el.innerHTML = headerHtml + `<div class="error"><strong>Error:</strong> ${esc(e.message)}</div>`;
  }
}

export async function artwork(el, sourceId, artworkId) {
  const adapter = getAdapter(sourceId);
  const A = adapter.constructor;
  const headerHtml = `
    <p class="crumbs"><a href="#/">Home</a> › ${adapterCrumb(sourceId)} › artwork</p>
  `;
  el.innerHTML = headerHtml + '<p class="loading">Loading artwork…</p>';
  try {
    const a = await adapter.getArtwork(artworkId);
    const meta = [a.artist, a.date].filter(Boolean).map(esc).join(' · ');
    el.innerHTML = `
      <p class="crumbs"><a href="#/">Home</a> › ${adapterCrumb(sourceId)} › ${esc(a.title)}</p>
      <h1>${esc(a.title)}</h1>
      ${meta ? `<p class="meta">${meta}</p>` : ''}
      <div class="image">
        <img src="${esc(a.imageUrl)}" alt="${esc(a.title)}">
      </div>
      <p class="iiif-url"><small>IIIF: <a href="${esc(a.imageUrl)}" target="_blank" rel="noopener">${esc(a.imageUrl)}</a></small></p>
    `;
  } catch (e) {
    el.innerHTML = headerHtml + `<div class="error"><strong>Error:</strong> ${esc(e.message)}</div>`;
  }
}
