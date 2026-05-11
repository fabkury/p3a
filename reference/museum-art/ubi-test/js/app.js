import { parseHash } from './router.js';
import { landing, museum, collection, artwork } from './views.js';

const main = document.getElementById('main');

const esc = s => String(s ?? '').replace(/[&<>"']/g,
  c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

async function render() {
  main.innerHTML = '<p class="loading">Loading…</p>';
  try {
    const { segs, query } = parseHash();
    if (segs.length === 0) {
      await landing(main);
    } else if (segs[0] === 'source' && segs.length === 2) {
      await museum(main, segs[1]);
    } else if (segs[0] === 'source' && segs.length === 4 && segs[2] === 'collections') {
      await collection(main, segs[1], decodeURIComponent(segs[3]), query);
    } else if (segs[0] === 'source' && segs.length === 4 && segs[2] === 'artwork') {
      await artwork(main, segs[1], decodeURIComponent(segs[3]));
    } else {
      main.innerHTML = `<p class="error">Unknown route: ${esc(location.hash)}</p>`;
    }
  } catch (e) {
    main.innerHTML = `<div class="error"><h2>Error</h2><pre>${esc(e.stack || e.message)}</pre></div>`;
  }
}

window.addEventListener('hashchange', render);
window.addEventListener('DOMContentLoaded', render);
render();
