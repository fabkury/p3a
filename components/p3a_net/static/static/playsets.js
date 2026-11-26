// SPDX-License-Identifier: Apache-2.0
// The playsets page: the saved playsets, and the editor that builds one
// from channels. What a playset must be and what can be added is
// playset-model.js; this file is the page around it.
//
// In 1.x every message of this page went to a function that did nothing
// (audit H11). They are toasts now, like on every other page.

import { del, get, put } from './api.js';
import { channelLabel, esc, explain } from './lib.js';
import {
    CHOICES, DISPLAY_NAME_MAX, KLIPY_CATEGORIES, MAX_CHANNELS, MAX_WEIGHT, addProblem, balance, hasOffset,
    identifierProblem, makeChannel, move, nameProblem, refreshChoices, saveProblem, shareLabel, sizeFrom, toDevice,
    tuningNote, tuningOf, tuningProblem, typeLabel, wholeNumber,
} from './playset-model.js';
import { dialog, nav, toast } from './ui.js';

const $ = (id) => document.getElementById(id);
const state = {
    playsets: [],
    max: 32,
    active: '',
    pins: [],
    editing: null,   // the name of the saved playset being edited; null for a new one
    channels: [],
    open: null,      // the channel whose row is open, if any
    dragged: -1,
    schema: null,    // the device's settings schema and settings: what a channel may set for itself
    settings: {},
};

nav('/playsets');

// --- the list ---------------------------------------------------------------------

async function loadList() {
    try {
        const [list, player] = await Promise.all([get('/api/v2/playsets'), get('/api/v2/player')]);
        state.playsets = list.playsets || [];
        state.max = list.max || 32;
        state.active = player.playset || '';
        renderList(list.unreadable || 0);
    } catch (e) {
        $('playsetList').innerHTML = `<div class="empty-state"><p>${esc(explain(e))}</p></div>`;
    }
}

function renderList(unreadable = 0) {
    const full = state.playsets.length >= state.max;
    const create = $('createNewBtn');
    create.disabled = full;
    create.title = full ? `Maximum of ${state.max} playsets reached` : '';
    if (state.playsets.length === 0) {
        $('playsetList').innerHTML = '<div class="empty-state"><div class="empty-state-icon">\u{1F3B5}</div><p>No playsets yet. Create your first mix!</p></div>';
        return;
    }
    $('playsetList').innerHTML = state.playsets.map((p) => {
        const playing = p.name === state.active;
        return `<div class="playset-item${playing ? ' active-playset' : ''}" data-name="${esc(p.name)}">`
            + `<div class="playset-info"><div class="playset-name">${esc(p.name)}${playing ? ' <span class="now-playing-badge">Now Playing</span>' : ''}</div>`
            + `<div class="playset-meta">${p.channels} channels</div></div>`
            + '<div class="playset-actions">'
            + '<button type="button" class="btn btn-primary btn-small" data-do="play">▶ Play</button>'
            + '<button type="button" class="btn btn-secondary btn-small" data-do="edit">Edit</button>'
            + '<button type="button" class="btn btn-danger btn-small" data-do="delete">Delete</button></div></div>';
    }).join('')
        + (unreadable ? `<div class="empty-state"><p>${unreadable} saved playset(s) could not be read and were left as they are on the card.</p></div>` : '');
}

$('playsetList').addEventListener('click', async (e) => {
    const button = e.target.closest('[data-do]');
    const name = button?.closest('[data-name]')?.dataset.name;
    if (!name) return;
    const path = `/api/v2/playsets/${encodeURIComponent(name)}`;
    try {
        if (button.dataset.do === 'play') {
            button.closest('.playset-item').classList.add('pending');
            await put('/api/v2/player/playset', { saved: name });
            state.active = name;
            toast(`Switching to "${name}"…`);
            renderList();
        } else if (button.dataset.do === 'edit') {
            const playset = await get(path);
            await openEditor(name, playset.channels || []);
        } else if (button.dataset.do === 'delete') {
            if (!window.confirm(`Delete playset "${name}"?`)) return;
            await del(path);
            toast('Playset deleted');
            await loadList();
        }
    } catch (err) {
        button.closest('.playset-item')?.classList.remove('pending');
        toast(explain(err), true);
    }
});

// --- the editor -------------------------------------------------------------------
//
// A playset's channels are a list of rows, one line each: its colour in the
// balance bar, its name, what it is, its share. A row opens in place for
// its fields, one row at a time. (2.0's first editor, like 1.x's, drew
// every channel as a card with every field open: a playset of twenty
// channels was a page of forms.)

// What channels of a source may set for themselves, or null (playset-model.js).
const tuningFor = (source) => (state.schema ? tuningOf(source, state.schema, state.settings) : null);

// The schema and the settings as they are now: the ranges and the "default"
// of a channel's own size and refresh. Without them those two fields are
// not offered, and the rest of the editor works.
async function loadTuning() {
    try {
        const [schema, settings] = await Promise.all([
            fetch('/api/v2/settings/schema').then((r) => r.json()),
            get('/api/v2/settings'),
        ]);
        state.schema = schema;
        state.settings = settings;
    } catch (e) {
        state.schema = null;
        state.settings = {};
    }
}

async function openEditor(name, channels) {
    state.editing = name;
    state.channels = channels.map((ch) => ({
        source: ch.source, kind: ch.kind || '', identifier: ch.identifier || '', display_name: ch.display_name || '',
        weight: ch.weight || 0, offset: ch.offset || 0, cache_size: ch.cache_size || 0, refresh_s: ch.refresh_s || 0,
    }));
    state.open = null;
    $('playsetName').value = name || '';
    $('editorTitle').textContent = name ? 'Edit playset' : 'New playset';
    $('listView').classList.add('hidden');
    $('editorView').classList.remove('hidden');
    await loadTuning();
    renderEditor();
    window.scrollTo(0, 0);
}

function closeEditor() {
    $('editorView').classList.add('hidden');
    $('listView').classList.remove('hidden');
}

// What a row says under the channel's name: what it is, and what it set for itself.
function rowDetail(ch) {
    const parts = [typeLabel(ch)];
    // A channel renamed by its owner still says what it plays.
    if (ch.display_name && ch.identifier && ch.source !== 'pinned') parts.push(ch.identifier);
    const note = tuningNote(ch);
    if (note) parts.push(note);
    return parts.join(' · ');
}

function renderBalance(bar) {
    $('weightVizContainer').classList.toggle('hidden', bar.length < 2);
    // Every channel that plays has its piece, however thin; the wide ones say their share.
    $('weightViz').innerHTML = bar.filter((s) => !s.muted).map((s) =>
        `<button type="button" class="weight-bar" data-index="${s.index}" style="flex:${Math.max(s.percent, 0.5)} 1 0;background:${s.colour};"`
        + ` title="${esc(s.label)}: ${shareLabel(s)}">${s.percent >= 9 ? shareLabel(s) : ''}</button>`).join('');
}

function renderButtons() {
    const name = $('playsetName').value;
    const named = nameProblem(name);
    const error = $('nameError');
    error.textContent = name === '' ? '' : named;
    error.classList.toggle('hidden', !named || name === '');
    const blocked = saveProblem(name, state.channels, tuningFor) !== '';
    $('saveBtn').disabled = blocked;
    $('saveAndPlayBtn').disabled = blocked;
    $('addChannelBtn').disabled = state.channels.length >= MAX_CHANNELS;
    $('channelCount').textContent = state.channels.length;
}

function panel(ch, index) {
    const last = state.channels.length - 1;
    const tuning = tuningFor(ch.source);
    let fields = '<div class="channel-field wide"><label class="form-label">Name</label>'
        + `<input type="text" class="form-input" data-field="display_name" maxlength="${DISPLAY_NAME_MAX}" placeholder="${esc(channelLabel({ ...ch, display_name: '' }))}" value="${esc(ch.display_name)}"></div>`
        + '<div class="channel-field"><label class="form-label">Weight</label>'
        + `<input type="number" class="form-input" data-field="weight" inputmode="numeric" min="0" max="${MAX_WEIGHT}" value="${ch.weight}">`
        + '<div class="field-hint">Its share against the others. 0 mutes it.</div></div>';
    if (hasOffset(ch.source)) {
        fields += '<div class="channel-field"><label class="form-label">Offset</label>'
            + `<input type="number" class="form-input" data-field="offset" inputmode="numeric" min="0" value="${ch.offset}">`
            + '<div class="field-hint">Skips this many from the start.</div></div>';
    }
    if (tuning) {
        const bad = tuningProblem({ ...ch, refresh_s: 0 }, tuning) !== '';
        fields += '<div class="channel-field"><label class="form-label">Keep</label>'
            + `<input type="text" class="form-input${bad ? ' error' : ''}" data-field="cache_size" inputmode="numeric" placeholder="Default (${tuning.size.standard})" value="${Number.isNaN(ch.cache_size) ? '' : ch.cache_size || ''}">`
            + `<div class="field-hint">Artworks, ${tuning.size.min} to ${tuning.size.max}. Empty for the default.</div></div>`
            + '<div class="channel-field"><label class="form-label">Refresh</label>'
            + '<select class="form-input" data-field="refresh_s">'
            + refreshChoices(tuning, ch.refresh_s).map(([v, label]) => `<option value="${v}"${v === (ch.refresh_s || 0) ? ' selected' : ''}>${esc(label)}</option>`).join('')
            + '</select><div class="field-hint">How often it looks for new artworks.</div></div>';
    }
    return `<div class="channel-panel"><div class="channel-fields">${fields}</div>`
        + '<div class="channel-actions">'
        + `<button type="button" class="btn btn-secondary btn-small" data-do="up"${index === 0 ? ' disabled' : ''}>↑ Up</button>`
        + `<button type="button" class="btn btn-secondary btn-small" data-do="down"${index === last ? ' disabled' : ''}>↓ Down</button>`
        + '<span class="channel-actions-gap"></span>'
        + '<button type="button" class="btn btn-danger btn-small" data-do="remove">Remove</button></div></div>';
}

function channelRow(ch, index, segment) {
    const open = state.open === ch;
    return `<div class="channel-row${open ? ' open' : ''}${segment.muted ? ' muted' : ''}" data-index="${index}">`
        + '<div class="channel-row-head" draggable="true">'
        + '<span class="drag-handle" title="Drag to reorder" aria-hidden="true">⠿</span>'
        + `<button type="button" class="channel-toggle" data-do="toggle" aria-expanded="${open}">`
        + `<span class="channel-dot" style="background:${segment.colour};"></span>`
        + `<span class="channel-names"><span class="channel-name">${esc(channelLabel(ch))}</span>`
        + `<span class="channel-sub">${esc(rowDetail(ch))}</span></span>`
        + `<span class="channel-share">${shareLabel(segment)}</span>`
        + '<span class="channel-chevron" aria-hidden="true"></span></button></div>'
        + (open ? panel(ch, index) : '') + '</div>';
}

function renderEditor() {
    const bar = balance(state.channels);
    $('channelList').innerHTML = state.channels.length
        ? state.channels.map((ch, i) => channelRow(ch, i, bar[i])).join('')
        : '<div class="empty-state"><p>No channels yet. Add one to start.</p></div>';
    renderBalance(bar);
    renderButtons();
}

// What changes while a field is being typed in, without redrawing the row
// (which would take the field away from under the keyboard): every row's
// name, detail and share, the balance bar, and whether it can be saved.
function refreshHeads() {
    const bar = balance(state.channels);
    for (const row of $('channelList').querySelectorAll('.channel-row')) {
        const index = Number(row.dataset.index);
        const ch = state.channels[index];
        row.classList.toggle('muted', bar[index].muted);
        row.querySelector('.channel-name').textContent = channelLabel(ch);
        row.querySelector('.channel-sub').textContent = rowDetail(ch);
        row.querySelector('.channel-share').textContent = shareLabel(bar[index]);
    }
    renderBalance(bar);
    renderButtons();
}

function openRow(ch) {
    state.open = ch;
    renderEditor();
    const index = state.channels.indexOf(ch);
    $('channelList').querySelector(`.channel-row[data-index="${index}"]`)?.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
}

$('playsetName').addEventListener('input', renderButtons);

// A piece of the balance bar opens its channel.
$('weightViz').addEventListener('click', (e) => {
    const piece = e.target.closest('[data-index]');
    if (piece) openRow(state.channels[Number(piece.dataset.index)]);
});

const channelList = $('channelList');
channelList.addEventListener('click', (e) => {
    const button = e.target.closest('[data-do]');
    const row = e.target.closest('.channel-row');
    if (!button || !row) return;
    const index = Number(row.dataset.index);
    const what = button.dataset.do;
    if (what === 'toggle') {
        state.open = state.open === state.channels[index] ? null : state.channels[index];
    } else if (what === 'remove') {
        state.channels.splice(index, 1);
        state.open = null;
    } else {
        state.channels = move(state.channels, index, index + (what === 'up' ? -1 : 1));
    }
    renderEditor();
});

function takeField(target) {
    const field = target.dataset.field;
    const row = target.closest('.channel-row');
    if (!field || !row) return false;
    const ch = state.channels[Number(row.dataset.index)];
    if (field === 'display_name') {
        ch.display_name = target.value.trim();
    } else if (field === 'weight') {
        ch.weight = wholeNumber(target.value, MAX_WEIGHT);
    } else if (field === 'offset') {
        ch.offset = wholeNumber(target.value, 4294967295);
    } else if (field === 'cache_size') {
        ch.cache_size = sizeFrom(target.value);
        target.classList.toggle('error', tuningProblem({ ...ch, refresh_s: 0 }, tuningFor(ch.source)) !== '');
    } else if (field === 'refresh_s') {
        ch.refresh_s = Number(target.value) || 0;
    }
    return true;
}
channelList.addEventListener('input', (e) => {
    if (takeField(e.target)) refreshHeads();
});
channelList.addEventListener('change', (e) => {
    if (!takeField(e.target)) return;
    // An emptied or over-large number shows as what it was taken for.
    const ch = state.channels[Number(e.target.closest('.channel-row').dataset.index)];
    if (e.target.dataset.field === 'weight') e.target.value = ch.weight;
    if (e.target.dataset.field === 'offset') e.target.value = ch.offset;
    refreshHeads();
});
// Dragging a row by its head onto another puts it there. (On a touch screen
// there is no dragging: the open row has Up and Down.)
channelList.addEventListener('dragstart', (e) => {
    const row = e.target.closest('.channel-row');
    if (!row || !e.target.closest('.channel-row-head')) return;
    state.dragged = Number(row.dataset.index);
    row.classList.add('dragging');
    if (e.dataTransfer) {
        e.dataTransfer.effectAllowed = 'move';
        e.dataTransfer.setData('text/plain', '');  // Firefox starts no drag without data
    }
});
channelList.addEventListener('dragend', () => {
    state.dragged = -1;
    for (const el of channelList.querySelectorAll('.dragging, .drag-over')) el.classList.remove('dragging', 'drag-over');
});
channelList.addEventListener('dragover', (e) => {
    const row = e.target.closest('.channel-row');
    if (!row || state.dragged < 0) return;
    e.preventDefault();
    for (const el of channelList.querySelectorAll('.drag-over')) el.classList.remove('drag-over');
    row.classList.add('drag-over');
});
channelList.addEventListener('drop', (e) => {
    const row = e.target.closest('.channel-row');
    if (!row || state.dragged < 0) return;
    e.preventDefault();
    state.channels = move(state.channels, state.dragged, Number(row.dataset.index));
    state.dragged = -1;
    renderEditor();
});

async function save(andPlay) {
    const name = $('playsetName').value;
    const problem = saveProblem(name, state.channels, tuningFor);
    if (problem) {
        toast(problem, true);
        return;
    }
    const renamed = state.editing !== null && state.editing !== name;
    const exists = state.playsets.some((p) => p.name === name);
    if (exists && (state.editing === null || renamed)
        && !window.confirm(`A playset named "${name}" already exists. Overwrite it?`)) {
        return;
    }
    const buttons = [$('saveBtn'), $('saveAndPlayBtn')];
    buttons.forEach((b) => { b.disabled = true; });
    try {
        await put(`/api/v2/playsets/${encodeURIComponent(name)}`, toDevice(name, state.channels));
        if (renamed) {
            // The device has no "rename": the new one is saved, then the old one goes.
            await del(`/api/v2/playsets/${encodeURIComponent(state.editing)}`).catch(() => {});
        }
        if (andPlay) {
            await put('/api/v2/player/playset', { saved: name });
            toast('Playset saved! Switching…');
        } else {
            toast('Playset saved successfully!');
        }
        closeEditor();
        await loadList();
    } catch (e) {
        toast(e.code === 'TOO_MANY_PLAYSETS' ? `There are ${state.max} playsets already. Delete one first.`
            : e.code === 'INVALID_PLAYSET' ? e.message : `Failed to save playset: ${explain(e)}`, true);
        renderButtons();
    }
}

$('saveBtn').addEventListener('click', () => save(false));
$('saveAndPlayBtn').addEventListener('click', () => save(true));
$('cancelBtn').addEventListener('click', closeEditor);
$('createNewBtn').addEventListener('click', () => openEditor(null, []));

// --- adding a channel -------------------------------------------------------------

// Asks Makapix Club, from the browser, whether there is such an artist or
// tag (its public pages; no key). Advice only: a channel can be added
// without it.
let checkBlockedUntil = 0;
async function checkMakapix(kind, identifier, box, nameInput) {
    if (Date.now() < checkBlockedUntil) {
        box.textContent = `Rate limited: wait ${Math.ceil((checkBlockedUntil - Date.now()) / 1000)}s.`;
        return;
    }
    const what = kind === 'hashtag' ? 'verify-hashtag' : kind === 'reactions' ? 'verify-reactions' : 'verify-user';
    box.className = 'check-results';
    box.textContent = 'Querying Makapix Club...';
    let response;
    try {
        response = await fetch(`https://makapix.club/api/player/${what}/${encodeURIComponent(identifier)}`);
    } catch (e) {
        box.textContent = 'Network error.';
        return;
    }
    if (response.status === 404) {
        box.textContent = kind === 'hashtag' ? 'Hashtag not found.' : 'User not found.';
        return;
    }
    if (response.status === 429) {
        checkBlockedUntil = Date.now() + 5000;
        box.textContent = 'Rate limited: wait 5s.';
        return;
    }
    if (!response.ok) {
        box.textContent = `Error (HTTP ${response.status}).`;
        return;
    }
    let data;
    try {
        data = await response.json();
    } catch (e) {
        box.textContent = 'Invalid response.';
        return;
    }
    const picture = (url) => (url ? `<img class="avatar" alt="" src="${esc(url.startsWith('/') ? `https://makapix.club${url}` : url)}">` : '');
    const count = (n, capped, noun) => (n === 0 ? `No ${noun} yet` : `${capped ? '100+' : n} ${noun}`);
    box.className = 'check-results has-data';
    let suggested;
    if (kind === 'hashtag') {
        box.innerHTML = `${picture(data.latest_artwork_url)}<div class="user-info">#${esc(data.tag)}<br>${esc(count(data.artwork_count, data.artwork_count_capped, 'artworks'))}</div>`;
        suggested = `#${data.tag}`;
    } else if (kind === 'reactions') {
        box.innerHTML = `${picture(data.latest_artwork_url)}<div class="user-info">${esc(data.handle)}<br>${esc(count(data.artwork_count, data.artwork_count_capped, 'reactions'))}</div>`;
        suggested = `${data.handle}'s reactions`;
    } else {
        box.innerHTML = `${picture(data.avatar_url)}<div class="user-info">${esc(data.handle)}<br>Rep: ${esc(data.reputation)} · Artworks: ${esc(data.artwork_count)}</div>`;
        suggested = data.handle;
    }
    if (nameInput && !nameInput.value) nameInput.value = suggested;
}

async function openAddChannel() {
    try {
        state.pins = (await get('/api/v2/pins')).lists || [];
    } catch (e) {
        state.pins = [];
    }
    // Which sources have their key (a key reads back masked, or empty when there is none).
    let settings = {};
    try {
        settings = await get('/api/v2/settings');
    } catch (e) { /* the note is advice only */ }
    dialog((body, close) => {
        body.innerHTML = '<div class="modal-header"><h3 class="modal-title">Add channel</h3><button type="button" class="close-btn" data-do="close" aria-label="Close">&times;</button></div>'
            + '<div class="form-group"><label class="form-label" for="add-source">From</label><select id="add-source" class="form-input">'
            + CHOICES.map((c) => `<option value="${c.id}">${esc(c.label)}</option>`).join('') + '</select></div>'
            + '<div id="add-fields"></div>'
            + `<div class="form-group" id="add-name-group"><label class="form-label" for="add-name">Name <span class="optional">optional</span></label><input type="text" id="add-name" class="form-input" maxlength="${DISPLAY_NAME_MAX}" placeholder="What to call it"></div>`
            + '<div class="button-group"><button type="button" class="btn btn-secondary" data-do="close">Cancel</button>'
            + '<button type="button" class="btn btn-primary" data-do="add" id="add-confirm">Add channel</button></div>';
        const source = () => body.querySelector('#add-source').value;
        const choice = () => CHOICES.find((c) => c.id === source());
        const kind = () => body.querySelector('#add-kind')?.value ?? choice().kinds[0]?.kind ?? '';

        const renderFields = () => {
            const c = choice();
            let html = '';
            if (c.browse) {
                html = '<div class="form-group"><div class="field-hint">Pick a museum and a section of its collection, look through it, and add the channel from there.</div>'
                    + '<button type="button" class="btn btn-primary add-browse" data-do="museum">Browse museums…</button></div>';
            } else {
                // The kind chosen before this redraw, if it is one of this source's.
                const was = body.querySelector('#add-kind')?.value;
                const chosen = c.kinds.find((k) => k.kind === was) || c.kinds[0];
                if (c.kinds.length > 1) {
                    html += '<div class="form-group"><label class="form-label" for="add-kind">Channel</label><select id="add-kind" class="form-input">'
                        + c.kinds.map((k) => `<option value="${esc(k.kind)}"${k === chosen ? ' selected' : ''}>${esc(k.label)}</option>`).join('') + '</select></div>';
                }
                for (const field of chosen.fields) {
                    if (field === 'sqid') {
                        html += '<div class="form-group"><label class="form-label" for="add-id">Artist ID</label><div class="sqid-input-row">'
                            + '<input type="text" id="add-id" class="form-input" maxlength="64" placeholder="artist_sqid">'
                            + '<button type="button" class="btn btn-secondary btn-small" data-do="check">Check</button></div>'
                            + '<div class="check-results" id="add-check">The short id in the address of the artist’s page at makapix.club.</div></div>';
                    } else if (field === 'hashtag') {
                        html += '<div class="form-group"><label class="form-label" for="add-id">Hashtag</label><div class="sqid-input-row">'
                            + '<input type="text" id="add-id" class="form-input" maxlength="64" placeholder="pixelart">'
                            + '<button type="button" class="btn btn-secondary btn-small" data-do="check">Check</button></div>'
                            + '<div class="check-results" id="add-check">Without the #.</div></div>';
                    } else if (field === 'query') {
                        html += '<div class="form-group"><label class="form-label" for="add-id">Search for</label><input type="text" id="add-id" class="form-input" maxlength="90" placeholder="cats"></div>';
                    } else if (field === 'category') {
                        html += '<div class="form-group"><label class="form-label" for="add-id">Category</label><select id="add-id" class="form-input">'
                            + KLIPY_CATEGORIES.map((k) => `<option value="${esc(k)}">${esc(k)}</option>`).join('') + '</select></div>';
                    } else if (field === 'list') {
                        html += '<div class="form-group"><label class="form-label" for="add-id">Pin list</label><select id="add-id" class="form-input">'
                            + '<option value="">The active list, whichever it is</option>'
                            + state.pins.map((l) => `<option value="${esc(l.slug)}">${esc(l.name)} (${l.count})</option>`).join('') + '</select></div>';
                    }
                }
                const keyOf = { giphy: 'giphy_api_key', klipy: 'klipy_api_key' }[c.id];
                if (keyOf && settings[keyOf] === '') {
                    html += `<div class="add-note warn">${esc(c.label)} needs an API key and none is set: this channel will not play until you add one in <a href="/settings#${c.id}">Settings</a>.</div>`;
                }
                if (c.once) {
                    html += `<div class="add-note">${state.channels.some((ch) => ch.source === c.id) ? 'This playset already has the SD card channel.' : 'Plays the files on the device’s SD card.'}</div>`;
                }
            }
            body.querySelector('#add-fields').innerHTML = html;
            // A museum channel is added from the browser, and the card only once: no button that would refuse.
            const taken = c.once && state.channels.some((ch) => ch.source === c.id);
            body.querySelector('#add-confirm').classList.toggle('hidden', !!c.browse);
            body.querySelector('#add-confirm').disabled = taken;
        };

        const add = (channel) => {
            const problem = addProblem(state.channels, channel);
            if (problem) {
                toast(problem, true);
                return;
            }
            state.channels.push(channel);
            close();
            openRow(channel);  // open, for its weight and the rest
        };

        body.addEventListener('change', (e) => {
            if (e.target.id === 'add-source') {
                body.querySelector('#add-fields').innerHTML = '';
                renderFields();
            } else if (e.target.id === 'add-kind') {
                renderFields();
            }
        });
        body.addEventListener('click', async (e) => {
            const what = e.target.closest('[data-do]')?.dataset.do;
            if (what === 'close') {
                close();
            } else if (what === 'check') {
                const id = body.querySelector('#add-id').value.trim();
                const problem = identifierProblem('makapix', kind(), id);
                if (problem) {
                    body.querySelector('#add-check').textContent = problem;
                    return;
                }
                checkMakapix(kind(), id, body.querySelector('#add-check'), body.querySelector('#add-name'));
            } else if (what === 'museum') {
                try {
                    const { openMuseumBrowse } = await import('./museum/browse.js');
                    openMuseumBrowse({
                        onAdd: (picked) => {
                            const typed = body.querySelector('#add-name').value;
                            add(makeChannel('museum', picked.kind, { identifier: picked.identifier, display_name: typed || picked.display_name }));
                        },
                    });
                } catch (err) {
                    toast('The museum browser could not be loaded.', true);
                }
            } else if (what === 'add') {
                if (choice().browse) {
                    toast('Use the museum browser to pick a term first', true);
                    return;
                }
                const id = body.querySelector('#add-id')?.value ?? '';
                add(makeChannel(source(), kind(), { identifier: id, display_name: body.querySelector('#add-name').value }));
            }
        });
        renderFields();
    }, { className: 'modal', overlayClass: 'modal-overlay' });
}

$('addChannelBtn').addEventListener('click', openAddChannel);

// "/playsets#new" opens the editor on a new playset, "#edit=<name>" on a saved one.
loadList().then(async () => {
    const hash = decodeURIComponent(location.hash.slice(1));
    if (hash === 'new') {
        await openEditor(null, []);
    } else if (hash.startsWith('edit=')) {
        const name = hash.slice(5);
        try {
            await openEditor(name, (await get(`/api/v2/playsets/${encodeURIComponent(name)}`)).channels || []);
        } catch (e) {
            toast(explain(e), true);
        }
    }
});
