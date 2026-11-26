// SPDX-License-Identifier: Apache-2.0
// The home page: what is playing, the controls, the playsets and pin lists
// to choose from, and sending a picture to the device.
//
// Everything shown comes from the device: GET /api/v2/player every four
// seconds while the page is visible, the playsets and pin lists at load and
// when they change. The picture itself is the device's own copy
// (current.file): no key and no third party is involved in showing it.

import { del, get, job, patch, post, put, upload } from './api.js';
import {
    BUILTINS, SOURCE_NAMES, UPLOAD_MAX_BYTES, activePill, banners, channelBadges, channelLabel, channelProblem, esc,
    explain, fitWithin, formatBytes, formatIn, isJpeg, isSmooth, playingName, uploadProblem, withExtension,
} from './lib.js';
import { dialog, nav, poller, toast } from './ui.js';

const $ = (id) => document.getElementById(id);
const SCREEN = 720;  // the panel: a photograph larger than this is shrunk before it is sent
const ICONS = {
    liked: '/static/reactions/submit_emoji_reaction.png',
    unliked: '/static/reactions/revoke_emoji_reaction.png',
    noted: '/static/reactions/submit_click.png',
    pin: '/static/reactions/pin.png',
    unpin: '/static/reactions/unpin.png',
    error: '/static/reactions/reaction-error.png',
};

const state = {
    player: null,
    status: null,
    makapix: null,
    playsets: [],
    pins: [],
    pinsRevision: -1,
    liked: new Map(),   // what this page did to an artwork: the device does not say whether it is liked
    pendingPill: '',
    infoFor: '',
};

nav('/');

// --- the artwork ------------------------------------------------------------------

// What identifies the artwork on the screen, for "did it change?".
function identity(current) {
    if (!current) return '';
    return `${current.source}:${current.name ?? current.id ?? current.post_id ?? ''}`;
}

function renderArtwork() {
    const current = state.player?.current;
    const img = $('artwork-img');
    if (!current || !current.file) {
        img.style.display = 'none';
        img.removeAttribute('src');
        img.dataset.file = '';
    } else if (img.dataset.file !== current.file) {
        // Only when it changed: an animation is not started over by every poll.
        img.dataset.file = current.file;
        img.classList.toggle('pixelated', !isSmooth(current.source));
        img.src = current.file;
        img.style.display = '';
    }
    const likable = current && (current.source === 'makapix' || current.source === 'giphy');
    const react = $('reaction-btn');
    react.style.visibility = likable ? 'visible' : 'hidden';
    if (likable) {
        react.textContent = current.source === 'giphy' ? '\u{1F446}' : '\u{1F44D}';
        react.classList.toggle('active', current.source === 'makapix' && state.liked.get(identity(current)) === true);
    }
    const pinnable = current && current.source !== 'local';
    const pin = $('pin-btn');
    pin.style.visibility = pinnable ? 'visible' : 'hidden';
    pin.classList.toggle('active', !!current?.pinned);
    $('info-btn').style.visibility = current ? 'visible' : 'hidden';
    if (state.infoFor && state.infoFor !== identity(current)) {
        $('artwork-info-area').hidden = true;  // it described another artwork
        state.infoFor = '';
    }
    const paused = !!state.player?.paused;
    $('pause-btn').textContent = paused ? 'Resume' : 'Pause';
}

const overlayTimers = {};

function showOverlay(which, icon) {
    const img = $(which === 'pin' ? 'pin-overlay-img' : 'reaction-overlay-img');
    img.src = ICONS[icon];
    img.hidden = false;
    clearTimeout(overlayTimers[which]);
    overlayTimers[which] = setTimeout(() => {
        img.hidden = true;
    }, 4000);
}

function pulse(button) {
    button.classList.remove('pulse');
    void button.offsetWidth;  // so that the animation starts again
    button.classList.add('pulse');
}

// --- now playing ------------------------------------------------------------------

function renderNowPlaying() {
    const player = state.player;
    const card = $('now-playing');
    if (!player || !player.channels || player.channels.length === 0) {
        card.style.display = 'none';
        return;
    }
    card.style.display = '';
    $('now-playing-name').textContent = playingName(player, state.pins);
    const records = player.channels.reduce((n, ch) => n + (ch.records || 0), 0);
    const available = player.channels.reduce((n, ch) => n + (ch.available || 0), 0);
    $('now-playing-stats').textContent = `${player.channels.length} ch · ${available}/${records} artworks`;
    const shuffle = $('shuffle-btn');
    if (!shuffle.dataset.busy) {
        shuffle.classList.toggle('active', !!player.shuffle);
        shuffle.setAttribute('aria-pressed', player.shuffle ? 'true' : 'false');
        $('shuffle-state').textContent = player.shuffle ? 'on' : 'off';
    }
    // Rows are rebuilt: there are few, and nothing in them holds state.
    $('now-playing-channels').innerHTML = player.channels.map((ch) => {
        const [letter, sub] = channelBadges(ch);
        const problem = channelProblem(ch);
        const cls = ch.state === 'listing' ? 'refreshing'
            : !problem ? ''
            : problem.kind === 'cooldown' ? 'cooldown'
            : problem.kind === 'unavailable' ? 'unavailable'
            : problem.kind === 'failed' ? '' : 'needs-key';
        const when = ch.state === 'listing' ? 'listing…'
            : ch.state === 'failed' && !problem ? 'failed'
            : ch.refresh_in_s >= 0 ? formatIn(ch.refresh_in_s) : '';
        const waiting = ch.to_download > 0 ? ` (+${ch.to_download})` : '';
        return `<div class="ps-ch-row ${cls}" title="${esc(ch.error || '')}">`
            + `<span class="ps-badge ps-badge-type">${esc(letter)}</span>`
            + `<span class="ps-badge ps-badge-sub">${esc(sub)}</span>`
            + `<span class="ps-ch-name">${esc(channelLabel(ch))}</span>`
            + (ch.identifier && ch.display_name ? `<span class="ps-ch-id">${esc(ch.identifier)}</span>` : '')
            + `<span class="ps-ch-count">${ch.available || 0}/${ch.records || 0}${waiting}</span>`
            + `<span class="ps-ch-time">${esc(when)}</span></div>`;
    }).join('');
}

function renderBanners() {
    const tones = {
        danger: 'background:var(--c-danger);color:var(--c-on-danger);',
        warn: '',
        info: 'background:var(--c-info-soft-bg);color:var(--c-info-soft-fg);',
    };
    $('banners').innerHTML = banners(state.player, state.status, state.update).map((b) =>
        `<div class="cooldown-banner" style="display:block;${tones[b.tone]}${b.href ? 'cursor:pointer;' : ''}"`
        + `${b.href ? ` data-href="${esc(b.href)}"` : ''}><h4>${esc(b.title)}</h4><p>${esc(b.text)}</p></div>`).join('');
}

$('banners').addEventListener('click', (e) => {
    const banner = e.target.closest('[data-href]');
    if (banner) window.location.href = banner.dataset.href;
});

// --- the pill bar -----------------------------------------------------------------

function pills() {
    const registered = !!state.makapix?.registered;
    const out = [];
    for (const p of state.playsets) {
        out.push({ id: `playset:${p.name}`, kind: 'user', label: p.name });
    }
    for (const l of state.pins) {
        out.push({ id: `pins:${l.slug}`, kind: 'pin', label: `${l.active ? '★ ' : ''}${l.name} (${l.count})`, slug: l.slug });
    }
    for (const b of BUILTINS) {
        out.push({ id: b.id, kind: 'builtin', label: b.label, disabled: b.needs === 'registration' && !registered });
    }
    out.push({ id: 'manage-pins', kind: 'utility', label: 'Manage pins' });
    return out;
}

function renderPills() {
    const all = pills();
    const active = activePill(state.player, state.pins.find((l) => l.active)?.slug);
    if (state.pendingPill && state.pendingPill === active) state.pendingPill = '';
    const half = Math.ceil(all.length / 2);
    const row = (items) => `<div class="pill-row">${items.map((p) =>
        `<button type="button" class="pill-item kind-${p.kind}${p.id === active ? ' active' : ''}`
        + `${p.id === state.pendingPill ? ' pending' : ''}" data-pill="${esc(p.id)}"`
        + `${p.disabled ? ' disabled title="Register this device with Makapix Club first"' : ''}>${esc(p.label)}</button>`).join('')}</div>`;
    $('pill-bar').innerHTML = row(all.slice(0, half)) + row(all.slice(half));
}

async function choosePill(id) {
    if (id === 'manage-pins') {
        openManagePins();
        return;
    }
    let playset;
    let label;
    if (id.startsWith('playset:')) {
        label = id.slice(8);
        playset = { saved: label };
    } else if (id.startsWith('pins:')) {
        const slug = id.slice(5);
        label = state.pins.find((l) => l.slug === slug)?.name || 'Pins';
        playset = { name: '', channels: [{ source: 'pinned', identifier: slug }] };
    } else {
        const builtin = BUILTINS.find((b) => b.id === id);
        if (!builtin) return;
        label = builtin.label;
        playset = builtin.playset;
    }
    state.pendingPill = id;
    renderPills();
    try {
        await put('/api/v2/player/playset', playset);
        toast(`Switching to ${label}…`);
        poll.soon(400);
    } catch (e) {
        state.pendingPill = '';
        renderPills();
        toast(explain(e), true);
    }
}

// A tap plays; a long press or a right click on a pin list opens it.
let pressTimer = null;
let pressed = false;
const pillBar = $('pill-bar');
pillBar.addEventListener('click', (e) => {
    const pill = e.target.closest('[data-pill]');
    if (!pill || pill.disabled) return;
    if (pressed) {
        pressed = false;  // the click that ends a long press
        return;
    }
    choosePill(pill.dataset.pill);
});
pillBar.addEventListener('contextmenu', (e) => {
    const pill = e.target.closest('[data-pill^="pins:"]');
    if (pill) {
        e.preventDefault();
        openBrowsePins(pill.dataset.pill.slice(5));
    }
});
pillBar.addEventListener('touchstart', (e) => {
    const pill = e.target.closest('[data-pill^="pins:"]');
    if (!pill) return;
    pressed = false;
    pressTimer = setTimeout(() => {
        pressed = true;
        openBrowsePins(pill.dataset.pill.slice(5));
    }, 500);
}, { passive: true });
for (const type of ['touchmove', 'touchend', 'touchcancel']) {
    pillBar.addEventListener(type, () => clearTimeout(pressTimer), { passive: true });
}

// --- polling ----------------------------------------------------------------------

async function loadLists() {
    const [playsets, pins] = await Promise.all([get('/api/v2/playsets'), get('/api/v2/pins')]);
    state.playsets = playsets.playsets || [];
    state.pins = pins.lists || [];
}

async function tick() {
    const player = await get('/api/v2/player', { timeout: 5000 });
    state.player = player;
    if (player.pins_revision !== state.pinsRevision) {
        state.pinsRevision = player.pins_revision;
        await loadLists();
    }
    renderArtwork();
    renderNowPlaying();
    renderPills();
    renderBanners();
}

const poll = poller(tick, 4000);

// Things that change seldom: once at load, and every minute.
async function slowTick() {
    const [status, makapix, update] = await Promise.all([get('/api/v2/status'), get('/api/v2/makapix'),
        get('/api/v2/update').catch(() => null)]);
    state.status = status;
    state.makapix = makapix;
    state.update = update;
    renderPills();
    renderBanners();
}
poller(slowTick, 60000);

// --- the controls -----------------------------------------------------------------

async function step(verb) {
    const buttons = [$('back-btn'), $('next-btn')];
    buttons.forEach((b) => { b.disabled = true; });
    try {
        await post(`/api/v2/player/${verb}`);
        poll.soon();
    } catch (e) {
        toast(explain(e), true);
    } finally {
        buttons.forEach((b) => { b.disabled = false; });
    }
}
$('back-btn').addEventListener('click', () => step('back'));
$('next-btn').addEventListener('click', () => step('next'));

$('pause-btn').addEventListener('click', async () => {
    const button = $('pause-btn');
    const pause = !state.player?.paused;
    button.disabled = true;
    try {
        await post(`/api/v2/player/${pause ? 'pause' : 'resume'}`);
        if (state.player) state.player.paused = pause;
        renderArtwork();
        toast(pause ? 'Paused' : 'Resumed');
        poll.soon();
    } catch (e) {
        toast(explain(e), true);
    } finally {
        button.disabled = false;
    }
});

$('reaction-btn').addEventListener('click', async () => {
    const current = state.player?.current;
    if (!current) return;
    const button = $('reaction-btn');
    const key = identity(current);
    // A Makapix like is taken back by a second press; a Giphy click is not.
    const undo = current.source === 'makapix' && state.liked.get(key) === true;
    pulse(button);
    button.disabled = true;
    try {
        const answer = await post(`/api/v2/player/${undo ? 'unlike' : 'like'}`);
        if (answer.reaction === 'none') {
            showOverlay('reaction', 'error');
            toast(current.source === 'makapix' ? 'Register this device with Makapix Club to like artworks.'
                : 'That did not work just now.', true);
        } else {
            showOverlay('reaction', answer.reaction);
            if (current.source === 'makapix') state.liked.set(key, answer.reaction === 'liked');
            renderArtwork();
        }
    } catch (e) {
        showOverlay('reaction', 'error');
        toast(explain(e), true);
    } finally {
        button.disabled = false;
    }
});

$('pin-btn').addEventListener('click', async () => {
    const current = state.player?.current;
    if (!current) return;
    const button = $('pin-btn');
    const unpin = !!current.pinned;
    pulse(button);
    button.disabled = true;
    try {
        await post(`/api/v2/pins/active/${unpin ? 'unpin' : 'pin'}`, unpin ? {} : undefined);
        current.pinned = !unpin;
        showOverlay('pin', unpin ? 'unpin' : 'pin');
        renderArtwork();
        poll.soon();
    } catch (e) {
        showOverlay('pin', 'error');
        toast(explain(e), true);
    } finally {
        button.disabled = false;
    }
});

// What the device knows of an artwork, and for Makapix art what the site
// says of it (asked of the site by the browser: it needs no key).
$('info-btn').addEventListener('click', async () => {
    const current = state.player?.current;
    const panel = $('artwork-info-area');
    if (!current) return;
    if (!panel.hidden && state.infoFor === identity(current)) {
        panel.hidden = true;  // a second press closes it
        state.infoFor = '';
        return;
    }
    const key = identity(current);
    state.infoFor = key;
    const set = (id, text) => { $(id).textContent = text || '—'; };
    const size = current.width ? `, ${current.width}×${current.height}` : '';
    set('artwork-info-title', current.name || '');
    set('artwork-info-artist', '');
    set('artwork-info-date', '');
    set('artwork-info-source', `${SOURCE_NAMES[current.source] || current.source}${current.format ? ` (${current.format}${size})` : ''}`);
    panel.hidden = false;
    const spinner = $('artwork-info-spinner');
    if (current.source === 'museum' && current.museum && current.id) {
        // The museum is asked what this is, by the browser, through the
        // same adapter the museum browser uses (loaded only now).
        spinner.classList.add('loading');
        try {
            const { getAdapter } = await import('./museum/index.js');
            const adapter = getAdapter(current.museum);
            if (adapter && state.infoFor === key) {
                set('artwork-info-source', `${adapter.displayName}${current.format ? ` (${current.format}${size})` : ''}`);
            }
            const about = adapter?.fetchMetadataByIiifKey ? await adapter.fetchMetadataByIiifKey(current.id) : null;
            if (about && state.infoFor === key) {
                set('artwork-info-title', about.title);
                set('artwork-info-artist', about.artist);
                set('artwork-info-date', about.date);
            }
        } catch (e) {
            // The museum could not be asked: what the device knows stays.
        }
        spinner.classList.remove('loading');
        return;
    }
    if (current.source !== 'makapix' || !current.storage_key) return;
    spinner.classList.add('loading');
    try {
        const response = await fetch(`https://makapix.club/api/player/post/${encodeURIComponent(current.storage_key)}`,
            { headers: { Accept: 'application/json' } });
        const post_ = response.ok ? await response.json() : null;
        if (post_ && state.infoFor === key) {
            set('artwork-info-title', post_.title);
            set('artwork-info-artist', post_.owner?.handle ? `@${post_.owner.handle}` : '');
            set('artwork-info-date', String(post_.created_at || '').slice(0, 10));
        }
    } catch (e) {
        // The site could not be asked: what the device knows stays.
    }
    spinner.classList.remove('loading');
});

$('now-playing-header').addEventListener('click', () => $('now-playing').classList.toggle('collapsed'));

$('shuffle-btn').addEventListener('click', async (e) => {
    e.stopPropagation();
    const button = $('shuffle-btn');
    const on = !state.player?.shuffle;
    button.dataset.busy = '1';
    button.classList.toggle('active', on);
    $('shuffle-state').textContent = on ? 'on' : 'off';
    try {
        await patch('/api/v2/settings', { pick_mode: on ? 'random' : 'recency' });
        if (state.player) state.player.shuffle = on;
    } catch (err) {
        toast('Failed to update Shuffle', true);
    }
    delete button.dataset.busy;
    renderNowPlaying();
});

// --- sending a picture ------------------------------------------------------------

$('upload-header').addEventListener('click', () => $('upload-section').classList.toggle('collapsed'));

const fileInput = $('file-input');
const dropZone = $('drop-zone');
dropZone.addEventListener('click', (e) => {
    if (e.target !== fileInput) fileInput.click();
});
for (const type of ['dragenter', 'dragover']) {
    dropZone.addEventListener(type, (e) => {
        e.preventDefault();
        dropZone.classList.add('drag-over');
    });
}
for (const type of ['dragleave', 'drop']) {
    dropZone.addEventListener(type, (e) => {
        e.preventDefault();
        dropZone.classList.remove('drag-over');
    });
}
dropZone.addEventListener('drop', (e) => {
    if (e.dataTransfer?.files?.length) {
        fileInput.files = e.dataTransfer.files;
        fileInput.dispatchEvent(new Event('change'));
    }
});

fileInput.addEventListener('change', () => {
    const file = fileInput.files[0];
    const info = $('file-info');
    const button = $('upload-btn');
    info.textContent = '';
    info.classList.remove('has-file');
    if (!file) return;
    const problem = uploadProblem(file.name, file.size, file.type);
    if (problem) {
        toast(problem, true);
        button.disabled = true;
        fileInput.value = '';
        return;
    }
    button.disabled = false;
    const name = document.createElement('strong');
    name.textContent = file.name;  // as text: a file's name is not markup
    info.append(name, document.createElement('br'), `${formatBytes(file.size)} • ${file.type || 'unknown'}`);
    info.classList.add('has-file');
});

// A photograph larger than the screen is drawn at the screen's size and
// sent as PNG: the device would only scale it down, after a long download
// and a slow decode.
async function shrinkIfLarge(file) {
    if (!isJpeg(file.name, file.type)) return { blob: file, name: file.name };
    const bitmap = await createImageBitmap(file);
    const fit = fitWithin(bitmap.width, bitmap.height, SCREEN);
    if (!fit) {
        bitmap.close();
        return { blob: file, name: file.name };
    }
    const canvas = document.createElement('canvas');
    canvas.width = fit.width;
    canvas.height = fit.height;
    canvas.getContext('2d').drawImage(bitmap, 0, 0, fit.width, fit.height);
    bitmap.close();
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/png'));
    if (!blob) throw new Error('the browser could not convert it');
    return { blob, name: withExtension(file.name, '.png') };
}

$('upload-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const file = fileInput.files[0];
    if (!file) {
        toast('Please select a file', true);
        return;
    }
    const button = $('upload-btn');
    const progress = $('upload-progress');
    const fill = $('progress-fill');
    button.disabled = true;
    let ready;
    try {
        ready = await shrinkIfLarge(file);
    } catch (err) {
        toast(`Error processing image: ${err.message}`, true);
        button.disabled = false;
        return;
    }
    if (ready.blob.size > UPLOAD_MAX_BYTES) {
        toast(`File too large! Maximum size is ${UPLOAD_MAX_BYTES / (1024 * 1024)}MiB.`, true);
        button.disabled = false;
        return;
    }
    fill.style.width = '0%';
    progress.classList.add('active');
    try {
        await upload(`/api/v2/files?name=${encodeURIComponent(ready.name)}`, ready.blob, (percent) => {
            fill.style.width = `${percent}%`;
        });
        toast('Upload successful!');
        fileInput.value = '';
        $('file-info').classList.remove('has-file');
        poll.soon();
    } catch (err) {
        toast(`Upload failed: ${explain(err)}`, true);
    }
    progress.classList.remove('active');
    button.disabled = false;
});

async function showUrl() {
    const input = $('show-url-input');
    const url = input.value.trim();
    if (!url) {
        toast('Please enter a URL', true);
        return;
    }
    const button = $('show-url-btn');
    button.disabled = true;
    try {
        const accepted = await post('/api/v2/player/show', { url });
        toast('Download started');
        input.value = '';
        await job(accepted.job);
        poll.soon(200);
    } catch (e) {
        toast(e.code === 'INVALID_URL' ? 'That is not an http:// or https:// address.' : `Failed: ${explain(e)}`, true);
    }
    button.disabled = false;
}
$('show-url-btn').addEventListener('click', showUrl);
$('show-url-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') showUrl();
});

// --- pin lists --------------------------------------------------------------------

async function refreshPins() {
    state.pins = (await get('/api/v2/pins')).lists || [];
    renderPills();
}

function openManagePins() {
    dialog((body, close) => {
        const render = () => {
            body.innerHTML = '<h3>Pin lists</h3>' + state.pins.map((l) =>
                `<div class="pin-modal-row" data-slug="${esc(l.slug)}">`
                + `<div class="pin-modal-row-header"><span class="pin-modal-row-name">${esc(l.name)}</span>`
                + `<span class="pin-modal-row-meta">(${l.count})</span>`
                + (l.active ? '<span class="pin-modal-active-badge">active</span>' : '') + '</div>'
                + '<div class="pin-modal-row-actions">'
                + '<button type="button" data-do="rename">Rename</button>'
                + `<button type="button" data-do="active"${l.active ? ' disabled' : ''}>Set active</button>`
                + '<button type="button" data-do="browse">Browse</button>'
                + `<button type="button" class="danger" data-do="delete"${l.active ? ' disabled' : ''}>Delete</button>`
                + '</div></div>').join('')
                + '<div class="pin-modal-create"><input type="text" id="new-pin-list" placeholder="New list name" maxlength="64">'
                + '<button type="button" data-do="create">Create</button></div>'
                + '<div class="pin-modal-actions"><button type="button" class="close-btn" data-do="close">Close</button></div>';
        };
        const act = async (what, slug) => {
            const list = state.pins.find((l) => l.slug === slug);
            try {
                if (what === 'close') {
                    close();
                    return;
                }
                if (what === 'browse') {
                    close();
                    openBrowsePins(slug);
                    return;
                }
                if (what === 'create') {
                    const name = body.querySelector('#new-pin-list').value.trim();
                    if (!name) return;
                    await post('/api/v2/pins', { name });
                } else if (what === 'rename') {
                    const name = (window.prompt(`Rename "${list.name}" to:`, list.name) || '').trim();
                    if (!name || name === list.name) return;
                    await put(`/api/v2/pins/${slug}`, { name });
                } else if (what === 'active') {
                    await put(`/api/v2/pins/${slug}`, { active: true });
                } else if (what === 'delete') {
                    if (!window.confirm(`Delete list "${list.name}" (${list.count} pins)? This cannot be undone.`)) return;
                    await del(`/api/v2/pins/${slug}`);
                }
                await refreshPins();
                render();
            } catch (e) {
                toast(explain(e), true);
            }
        };
        body.addEventListener('click', (e) => {
            const button = e.target.closest('[data-do]');
            if (button) act(button.dataset.do, button.closest('[data-slug]')?.dataset.slug);
        });
        body.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && e.target.id === 'new-pin-list') act('create');
        });
        render();
    });
}

function openBrowsePins(slug) {
    const list = state.pins.find((l) => l.slug === slug);
    if (!list) return;
    dialog((body, close) => {
        const PAGE = 20;
        let loaded = 0;
        let total = list.count;
        let loading = false;
        body.innerHTML = `<h3>${esc(list.name)}</h3><div class="pin-browse-strip" id="pin-strip"></div>`
            + '<div class="pin-browse-footer"><span id="pin-range"></span></div>'
            + '<div class="pin-modal-actions"><button type="button" class="close-btn" id="pin-close">Close</button></div>';
        const strip = body.querySelector('#pin-strip');
        const range = body.querySelector('#pin-range');
        const label = () => {
            range.textContent = total === 0 ? 'No pins yet' : `Showing 1–${loaded} of ${total}`;
        };
        const more = async () => {
            if (loading || loaded >= total) return;
            loading = true;
            try {
                const page = await get(`/api/v2/pins/${slug}?offset=${loaded}&limit=${PAGE}`);
                total = page.count;
                for (const pin of page.pins) {
                    const card = document.createElement('div');
                    card.className = 'pin-browse-card';
                    card.dataset.position = pin.position;
                    card.dataset.key = pin.key;
                    const what = pin.id ?? pin.post_id ?? pin.name ?? '';
                    card.innerHTML = (pin.file ? `<img loading="lazy" alt="" class="${isSmooth(pin.source) ? '' : 'pixelated'}" src="${esc(pin.file)}">` : '')
                        + `<div class="meta">${esc(SOURCE_NAMES[pin.source] || pin.source)} — ${esc(what)}</div>`
                        + '<div class="actions"><button type="button" class="danger">Unpin</button></div>';
                    strip.appendChild(card);
                }
                loaded += page.pins.length;
                if (page.pins.length === 0) total = loaded;
            } catch (e) {
                toast(explain(e), true);
                total = loaded;
            }
            loading = false;
            label();
            // Until the strip can be scrolled there is nothing to trigger the next page.
            if (loaded < total && strip.scrollWidth <= strip.clientWidth) more();
        };
        strip.addEventListener('scroll', () => {
            if (strip.scrollLeft + strip.clientWidth >= strip.scrollWidth - 200) more();
        });
        strip.addEventListener('click', async (e) => {
            const button = e.target.closest('button.danger');
            const card = button?.closest('.pin-browse-card');
            if (!card || !window.confirm('Unpin this artwork?')) return;
            try {
                await post(`/api/v2/pins/${slug}/unpin`, { position: Number(card.dataset.position), key: Number(card.dataset.key) });
            } catch (err) {
                if (err.code !== 'LIST_CHANGED') {
                    toast(explain(err), true);
                    return;
                }
            }
            // Positions after it have moved: the strip is read again from the start.
            strip.innerHTML = '';
            loaded = 0;
            await refreshPins();
            total = state.pins.find((l) => l.slug === slug)?.count ?? 0;
            label();
            more();
        });
        body.querySelector('#pin-close').addEventListener('click', close);
        label();
        more();
    });
}
