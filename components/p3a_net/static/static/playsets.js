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
    identifierProblem, makeChannel, move, nameProblem, saveProblem, toDevice, typeLabel, wholeNumber,
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
    dragged: -1,
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
            openEditor(name, playset.channels || []);
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

function openEditor(name, channels) {
    state.editing = name;
    state.channels = channels.map((ch) => ({
        source: ch.source, kind: ch.kind || '', identifier: ch.identifier || '', display_name: ch.display_name || '',
        weight: ch.weight || 0, offset: ch.offset || 0,
    }));
    $('playsetName').value = name || '';
    $('listView').classList.add('hidden');
    $('editorView').classList.remove('hidden');
    renderEditor();
    window.scrollTo(0, 0);
}

function closeEditor() {
    $('editorView').classList.add('hidden');
    $('listView').classList.remove('hidden');
}

function renderBalance() {
    const bar = balance(state.channels);
    $('weightVizContainer').classList.toggle('hidden', bar.length === 0);
    $('weightViz').innerHTML = bar.filter((s) => s.percent >= 5).map((s) =>
        `<div class="weight-bar" style="width:${s.percent}%;background:${s.colour};" title="${esc(s.label)}: ${Math.round(s.percent)}%">`
        + `${s.percent >= 10 ? `${Math.round(s.percent)}%` : ''}</div>`).join('');
}

function renderButtons() {
    const name = $('playsetName').value;
    const named = nameProblem(name);
    const error = $('nameError');
    error.textContent = name === '' ? '' : named;
    error.classList.toggle('hidden', !named || name === '');
    const blocked = saveProblem(name, state.channels) !== '';
    $('saveBtn').disabled = blocked;
    $('saveAndPlayBtn').disabled = blocked;
    $('addChannelBtn').disabled = state.channels.length >= MAX_CHANNELS;
    $('channelCount').textContent = state.channels.length;
}

function channelCard(ch, index) {
    const colour = balance(state.channels)[index].colour;
    const offsetOk = hasOffset(ch.source);
    const last = state.channels.length - 1;
    // What the channel is does not change once added: only its name, weight and offset.
    const what = ch.identifier && ch.source !== 'pinned' ? `<div class="mode-description">${esc(ch.source === 'museum' ? `${ch.kind} · ${ch.identifier}` : ch.identifier)}</div>` : '';
    return `<div class="channel-card" draggable="true" data-index="${index}">`
        + '<div class="channel-header"><div class="channel-header-left">'
        + `<span class="channel-color-swatch" style="background:${colour};" title="Channel color in balance graph"></span>`
        + `<span class="channel-type-badge">${esc(typeLabel(ch))}</span></div>`
        + '<div class="channel-controls">'
        + `<button type="button" class="btn btn-secondary btn-small btn-icon" data-do="up" title="Move up"${index === 0 ? ' disabled' : ''}>↑</button>`
        + `<button type="button" class="btn btn-secondary btn-small btn-icon" data-do="down" title="Move down"${index === last ? ' disabled' : ''}>↓</button>`
        + '<button type="button" class="btn btn-danger btn-small" data-do="remove">Remove</button></div></div>'
        + '<div class="channel-fields">'
        + `<input type="text" class="form-input" data-field="display_name" maxlength="${DISPLAY_NAME_MAX}" placeholder="${esc(channelLabel({ ...ch, display_name: '' }))}" value="${esc(ch.display_name)}">`
        + what
        + '<div class="field-row">'
        + `<div><label class="form-label" title="0 mutes this channel; if every channel is 0, all play equally">Weight (0 = mute)</label>`
        + `<input type="number" class="form-input" data-field="weight" min="0" max="${MAX_WEIGHT}" value="${ch.weight}"></div>`
        + `<div><label class="form-label">${offsetOk ? 'Offset (skip this many)' : 'Offset (not for this type)'}</label>`
        + `<input type="number" class="form-input" data-field="offset" min="0" value="${ch.offset}"${offsetOk ? '' : ' disabled title="Not supported for this channel type"'}></div>`
        + '</div></div></div>';
}

function renderEditor() {
    $('channelList').innerHTML = state.channels.length
        ? state.channels.map(channelCard).join('')
        : '<div class="empty-state"><p>No channels yet</p></div>';
    renderBalance();
    renderButtons();
}

$('playsetName').addEventListener('input', renderButtons);

const channelList = $('channelList');
channelList.addEventListener('click', (e) => {
    const button = e.target.closest('[data-do]');
    const card = e.target.closest('.channel-card');
    if (!button || !card) return;
    const index = Number(card.dataset.index);
    if (button.dataset.do === 'remove') state.channels.splice(index, 1);
    else state.channels = move(state.channels, index, index + (button.dataset.do === 'up' ? -1 : 1));
    renderEditor();
});
channelList.addEventListener('change', (e) => {
    const field = e.target.dataset.field;
    const card = e.target.closest('.channel-card');
    if (!field || !card) return;
    const ch = state.channels[Number(card.dataset.index)];
    if (field === 'display_name') ch.display_name = e.target.value.trim();
    else if (field === 'weight') ch.weight = wholeNumber(e.target.value, MAX_WEIGHT);
    else if (field === 'offset') ch.offset = wholeNumber(e.target.value, 4294967295);
    renderEditor();
});
// Dragging a card onto another puts it there.
channelList.addEventListener('dragstart', (e) => {
    const card = e.target.closest('.channel-card');
    if (!card) return;
    state.dragged = Number(card.dataset.index);
    card.classList.add('dragging');
});
channelList.addEventListener('dragend', () => {
    state.dragged = -1;
    for (const el of channelList.querySelectorAll('.dragging, .drag-over')) el.classList.remove('dragging', 'drag-over');
});
channelList.addEventListener('dragover', (e) => {
    const card = e.target.closest('.channel-card');
    if (!card || state.dragged < 0) return;
    e.preventDefault();
    for (const el of channelList.querySelectorAll('.drag-over')) el.classList.remove('drag-over');
    card.classList.add('drag-over');
});
channelList.addEventListener('drop', (e) => {
    const card = e.target.closest('.channel-card');
    if (!card || state.dragged < 0) return;
    e.preventDefault();
    state.channels = move(state.channels, state.dragged, Number(card.dataset.index));
    state.dragged = -1;
    renderEditor();
});

async function save(andPlay) {
    const name = $('playsetName').value;
    const problem = saveProblem(name, state.channels);
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
        body.innerHTML = '<div class="modal-header"><h3 class="modal-title">Add Channel</h3><button type="button" class="close-btn" data-do="close">&times;</button></div>'
            + '<div class="form-group"><label class="form-label">Channel Type</label><select id="add-source" class="form-input">'
            + CHOICES.map((c) => `<option value="${c.id}">${esc(c.label)}</option>`).join('') + '</select></div>'
            + '<div id="add-fields"></div>'
            + `<div class="form-group"><label class="form-label">Display Name (optional)</label><input type="text" id="add-name" class="form-input" maxlength="${DISPLAY_NAME_MAX}" placeholder="Custom label for this channel"></div>`
            + '<div class="button-group"><button type="button" class="btn btn-secondary" data-do="close">Cancel</button>'
            + '<button type="button" class="btn btn-primary" data-do="add">Add Channel</button></div>';
        const source = () => body.querySelector('#add-source').value;
        const choice = () => CHOICES.find((c) => c.id === source());
        const kind = () => body.querySelector('#add-kind')?.value ?? choice().kinds[0]?.kind ?? '';

        const renderFields = () => {
            const c = choice();
            let html = '';
            if (c.browse) {
                html = '<div class="mode-description">Pick a museum, a section of its collection and look through it; the channel is added from there.</div>'
                    + '<button type="button" class="btn btn-primary" data-do="museum" style="width:100%;margin:8px 0 14px;">Browse museums…</button>';
            } else {
                // The kind chosen before this redraw, if it is one of this source's.
                const was = body.querySelector('#add-kind')?.value;
                const chosen = c.kinds.find((k) => k.kind === was) || c.kinds[0];
                if (c.kinds.length > 1) {
                    html += `<div class="form-group"><label class="form-label">${esc(c.label)} Channel</label><select id="add-kind" class="form-input">`
                        + c.kinds.map((k) => `<option value="${esc(k.kind)}"${k === chosen ? ' selected' : ''}>${esc(k.label)}</option>`).join('') + '</select></div>';
                }
                for (const field of chosen.fields) {
                    if (field === 'sqid') {
                        html += '<div class="form-group"><label class="form-label">Artist ID (sqid)</label><div class="sqid-input-row">'
                            + '<input type="text" id="add-id" class="form-input" maxlength="64" placeholder="artist_sqid">'
                            + '<button type="button" class="btn btn-secondary btn-small" data-do="check">Check</button></div>'
                            + '<div class="check-results" id="add-check">Enter sqid to check.</div></div>';
                    } else if (field === 'hashtag') {
                        html += '<div class="form-group"><label class="form-label">Hashtag (without #)</label><div class="sqid-input-row">'
                            + '<input type="text" id="add-id" class="form-input" maxlength="64" placeholder="pixelart">'
                            + '<button type="button" class="btn btn-secondary btn-small" data-do="check">Check</button></div>'
                            + '<div class="check-results" id="add-check">Enter hashtag to check.</div></div>';
                    } else if (field === 'query') {
                        html += '<div class="form-group"><label class="form-label">Search Query</label><input type="text" id="add-id" class="form-input" maxlength="90" placeholder="cats"></div>';
                    } else if (field === 'category') {
                        html += '<div class="form-group"><label class="form-label">Category</label><select id="add-id" class="form-input">'
                            + KLIPY_CATEGORIES.map((k) => `<option value="${esc(k)}">${esc(k)}</option>`).join('') + '</select></div>';
                    } else if (field === 'list') {
                        html += '<div class="form-group"><label class="form-label">Pin list</label><select id="add-id" class="form-input">'
                            + '<option value="">The active list, whichever it is</option>'
                            + state.pins.map((l) => `<option value="${esc(l.slug)}">${esc(l.name)} (${l.count})</option>`).join('') + '</select></div>';
                    }
                }
                const keyOf = { giphy: 'giphy_api_key', klipy: 'klipy_api_key' }[c.id];
                if (keyOf && settings[keyOf] === '') {
                    html += `<div class="mode-description">${esc(c.label)} needs an API key and none is set: this channel will not play until you add one in <a href="/settings#${c.id}">Settings</a>.</div>`;
                }
                if (c.once) {
                    html += `<div class="mode-description">${state.channels.some((ch) => ch.source === c.id) ? 'SD Card channel already added' : 'Plays local files from the device SD card'}</div>`;
                }
            }
            body.querySelector('#add-fields').innerHTML = html;
        };

        const add = (channel) => {
            const problem = addProblem(state.channels, channel);
            if (problem) {
                toast(problem, true);
                return;
            }
            state.channels.push(channel);
            toast('Channel added');
            close();
            renderEditor();
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
        openEditor(null, []);
    } else if (hash.startsWith('edit=')) {
        const name = hash.slice(5);
        try {
            openEditor(name, (await get(`/api/v2/playsets/${encodeURIComponent(name)}`)).channels || []);
        } catch (e) {
            toast(explain(e), true);
        }
    }
});
