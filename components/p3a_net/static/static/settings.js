// SPDX-License-Identifier: Apache-2.0
// The settings page. Every setting is drawn from the device's schema
// (settings-model.js says with which control and where): the page holds no
// range, no default and no list of settings of its own, which in 1.x had
// drifted from the firmware's. Beside the settings are a few read-outs and
// actions (the network, Makapix, storage, the system).

import { del, get, job, patch, post } from './api.js';
import { esc, explain, formatBytes, formatUptime } from './lib.js';
import { SECRET_MASK, ZONES, controlFor, layout, validate, withCurrent } from './settings-model.js';
import { nav, poller, toast } from './ui.js';

const $ = (id) => document.getElementById(id);
const state = { schema: null, values: {}, status: null, makapix: null, animations: [], tab: 'display' };
const entries = new Map();

nav('/settings');

// --- saving -----------------------------------------------------------------------

async function save(key, raw, { quiet = false } = {}) {
    const entry = entries.get(key);
    const checked = validate(entry, raw);
    if (checked.error) {
        toast(checked.error, true);
        return false;
    }
    try {
        const answer = await patch('/api/v2/settings', { [key]: checked.value });
        state.values[key] = entry.secret && checked.value !== '' ? SECRET_MASK : checked.value;
        if (!quiet) toast(`${entry.label}: saved`);
        if (answer.reboot_needed) $('reboot-notice').style.display = '';
        return true;
    } catch (e) {
        toast(e.code === 'INVALID_SETTING' ? e.message : explain(e), true);
        return false;
    }
}

// --- drawing a setting ------------------------------------------------------------

function settingHtml(key) {
    const entry = entries.get(key);
    const c = controlFor(entry, { 'intro-animations': state.animations });
    const value = state.values[key];
    const id = `set-${key}`;
    const hint = c.help ? `<div class="setting-hint">${esc(c.help)}</div>` : '';
    const reboot = c.reboot ? '<div class="setting-hint">Takes effect after a restart.</div>' : '';
    // A long label takes a line of its own, with its control under it.
    const label = `<span class="setting-label${c.label.length > 22 ? ' long' : ''}">${esc(c.label)}</span>`;
    const saveButton = `<button type="button" class="btn" data-save="${esc(key)}">Save</button>`;
    switch (c.kind) {
        case 'toggle':
            return `<div class="toggle-row"><span class="toggle-label">${esc(c.label)}</span>`
                + `<label class="toggle-switch"><input type="checkbox" id="${id}" data-instant="${esc(key)}"${value ? ' checked' : ''}>`
                + `<span class="toggle-slider"></span></label></div>${hint}`;
        case 'select': {
            const options = withCurrent(c.options, value);
            return `<div class="setting-row">${label}<select class="setting-select" id="${id}" data-instant="${esc(key)}"`
                + `${c.numeric ? ' data-numeric="1"' : ''}>`
                + options.map(([v, text]) => `<option value="${esc(v)}"${v === value ? ' selected' : ''}>${esc(text)}</option>`).join('')
                + `</select>${hint}${reboot}</div>`;
        }
        case 'choices':
            return `<div class="rot-row" id="${id}">` + c.options.map(([v, text]) =>
                `<button type="button" class="rot-btn${v === value ? ' active' : ''}" data-choice="${esc(key)}" data-value="${esc(v)}">${esc(text)}</button>`).join('')
                + `</div>${hint}`;
        case 'range':
            return `<div class="brightness-row"><input type="range" id="${id}" min="${c.min}" max="${c.max}" step="${c.step}" value="${esc(value)}" data-range="${esc(key)}">`
                + `<span class="brightness-value" id="${id}-value">${esc(value)}${esc(c.unit)}</span>${saveButton}</div>${hint}`;
        case 'number':
            return `<div class="setting-row">${label}<input type="number" class="setting-input" id="${id}" min="${c.min}" max="${c.max}" value="${esc(value)}" style="width:110px;">`
                + (c.unit ? `<span class="setting-hint" style="width:auto;">${esc(c.unit)}</span>` : '') + saveButton + hint + reboot + '</div>';
        case 'color':
            return `<div class="bg-row">${label}<input type="color" id="${id}" value="${esc(value)}" data-instant="${esc(key)}" class="bg-preview" style="padding:0;"></div>${hint}`;
        case 'secret': {
            const set = value === SECRET_MASK;
            return `<div class="info-row"><span class="info-label">Status</span><span class="status-badge ${set ? 'status-connected' : 'status-warn'}" id="${id}-badge">`
                + `${set ? 'Key set' : 'No key: channels won’t refresh'}</span></div>`
                + `<div class="setting-row">${label}<input type="password" class="setting-input" id="${id}" maxlength="${c.maxLength}" autocomplete="off"`
                + ` placeholder="${set ? 'A key is stored. Type a new one to replace it' : 'Paste the key'}" style="flex:1;min-width:0;">${saveButton}`
                + (set ? `<button type="button" class="btn btn-danger" data-clear="${esc(key)}">Remove</button>` : '') + hint + '</div>';
        }
        case 'zone': {
            const known = ZONES.some(([posix]) => posix === value);
            return `<div class="setting-row"><span class="setting-label">Time zone</span><select class="setting-select" id="${id}-pick" data-zone="${esc(key)}">`
                + ZONES.map(([posix, name]) => `<option value="${esc(posix)}"${posix === value ? ' selected' : ''}>${esc(name)}</option>`).join('')
                + `<option value=""${known ? '' : ' selected'}>Other…</option></select></div>`
                + `<div class="setting-row" id="${id}-custom"${known ? ' style="display:none;"' : ''}><span class="setting-label">POSIX time zone</span>`
                + `<input type="text" class="setting-input" id="${id}" maxlength="${c.maxLength}" value="${esc(value)}" style="flex:1;min-width:0;">${saveButton}</div>${hint}`;
        }
        default:
            return `<div class="setting-row">${label}<input type="text" class="setting-input" id="${id}" maxlength="${c.maxLength}" value="${esc(value)}"`
                + ` placeholder="${esc(c.placeholder || '')}" style="flex:1;min-width:0;">${saveButton}${hint}${reboot}</div>`;
    }
}

// --- the widgets: what is on the page beside the settings --------------------------

const row = (label, value) => (value === undefined || value === null || value === '' ? ''
    : `<div class="info-row"><span class="info-label">${esc(label)}</span><span class="info-value">${esc(value)}</span></div>`);

const widgets = {
    theme() {
        const themes = window.p3aTheme?.THEMES || [];
        const current = window.p3aTheme?.get();
        return `<div class="setting-row"><span class="setting-label">Color theme</span><select class="setting-select" id="theme-select">`
            + themes.map((t) => `<option value="${esc(t.id)}"${t.id === current ? ' selected' : ''}>${esc(t.name)}</option>`).join('')
            + '</select><div class="setting-hint">Sets the look of this web interface. The choice is saved in this browser.</div></div>';
    },
    connection() {
        const n = state.status?.network;
        if (!n) return '<div class="info-row"><span class="info-label">Status</span><span class="status-badge status-neutral">Loading...</span></div>';
        const up = n.state === 'up';
        return `<div class="info-row"><span class="info-label">Status</span><span class="status-badge ${up ? 'status-connected' : 'status-disconnected'}">${up ? 'Connected' : 'Disconnected'}</span></div>`
            + row('Network (SSID)', n.ssid) + row('IP Address', n.ip) + row('Hostname', n.hostname ? `${n.hostname}.local` : '')
            + row('Signal Strength (RSSI)', n.rssi ? `${n.rssi} dBm` : '');
    },
    'erase-wifi'() {
        return '<button type="button" class="btn danger-btn" id="erase-wifi">Erase Wi-Fi &amp; Reboot</button>'
            + '<p class="warning">Device will restart in configuration mode</p>';
    },
    'giphy-id'() {
        return '<div class="setting-hint" style="margin-bottom:8px;">Giphy gives each device an anonymous ID and uses it to personalize trending results. Resetting it gives you a fresh identity.</div>'
            + '<button type="button" class="btn danger-btn" id="giphy-reset-id">Reset Random ID</button>';
    },
    museums() {
        return '<div class="setting-hint" style="margin-bottom:10px;">Channels that play artwork from the Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection, '
            + 'the Statens Museum for Kunst, Harvard Art Museums, the Smithsonian, the Cleveland Museum of Art and the Minneapolis Institute of Art. '
            + 'Build a museum channel from Playsets &gt; Add Channel &gt; Museum.</div>';
    },
    storage() {
        const s = state.status?.storage;
        if (!s) return '';
        if (s.health !== 'ok') return '<div class="info-row"><span class="info-label">SD card</span><span class="status-badge status-disconnected">Not working</span></div>';
        const total = s.total_mb * 1024 * 1024, free = s.free_mb * 1024 * 1024;
        const used = total > 0 ? Math.round(((total - free) / total) * 100) : 0;
        return `<div class="info-row"><span class="info-label">${esc(formatBytes(free))} free of ${esc(formatBytes(total))}</span><span class="info-value">${100 - used}% free</span></div>`
            + `<div class="progress-bar" style="height:6px;"><div class="progress-fill" style="width:${used}%;"></div></div>`
            + row('Folder', s.root)
            + '<p class="warning">You can reach the card from a computer over the USB-C port next to the buttons.</p>';
    },
    makapix() {
        const m = state.makapix;
        if (!m) return '<div class="info-row"><span class="info-label">Status</span><span class="status-badge status-neutral">Loading...</span></div>';
        const r = m.registration || {};
        let badge, detail = '', buttons = '';
        if (r.phase === 'showing_code') {
            const minutes = Math.floor(r.seconds_left / 60), seconds = String(r.seconds_left % 60).padStart(2, '0');
            badge = ['status-warn', 'Waiting for the code'];
            detail = '<div style="text-align:center;margin:12px 0;"><div class="setting-hint" style="width:auto;">Sign in at makapix.club and enter this code:</div>'
                + `<div style="font-family:var(--font-mono);font-size:2.2rem;letter-spacing:0.2em;margin:8px 0;">${esc(r.code)}</div>`
                + `<div class="setting-hint" style="width:auto;">Good for ${minutes}:${seconds}. It is on the device’s screen too.</div></div>`;
            buttons = '<button type="button" class="btn btn-full" data-makapix="cancel">Cancel</button>';
        } else if (r.phase === 'asking') {
            badge = ['status-warn', 'Asking for a code…'];
        } else if (!m.registered) {
            badge = ['status-neutral', 'Not registered'];
            detail = `<div class="setting-hint">${state.status?.network?.state === 'up' ? 'Link this device to your Makapix Club account to play your feeds and control it from the site.' : 'Connect to Wi-Fi to register.'}</div>`;
            buttons = '<button type="button" class="btn btn-full" data-makapix="register">Register this device</button>';
        } else {
            const links = {
                connected: ['status-connected', 'Connected'], connecting: ['status-warn', 'Connecting…'],
                waiting: ['status-warn', 'Not connected'], invalid: ['status-disconnected', 'Registration invalid'],
            };
            badge = links[m.link] || ['status-neutral', m.link];
            if (m.link === 'invalid') {
                detail = '<div class="setting-hint">Makapix Club does not accept this device’s certificates. Register it again.</div>';
            } else if (m.link === 'waiting') {
                detail = '<div class="setting-hint">Reconnecting automatically…</div>';
            }
            if (m.certificate_ends) {
                const ends = new Date(m.certificate_ends * 1000);
                const days = Math.round((ends - Date.now()) / 86400000);
                detail += `<div class="setting-hint">Certificate valid until ${ends.toISOString().slice(0, 10)} (${days} days)`
                    + `${m.certificate_renewal === 'renewed' ? ', renewed' : ''}. The device renews it by itself.</div>`;
            }
            buttons = `<button type="button" class="btn btn-full" data-makapix="register" style="margin-bottom:8px;">${m.link === 'invalid' ? 'Register again' : 'Register again (another account)'}</button>`
                + '<button type="button" class="btn danger-btn" data-makapix="unregister">Remove Makapix Club registration</button>'
                + '<div class="setting-hint">Removing it deletes the stored credentials. The player stays listed on the site until it is removed there.</div>';
        }
        const last = r.phase === 'idle' && r.last_result && !['registered', 'unregistered', 'cancelled'].includes(r.last_result)
            ? `<div class="setting-hint">Last attempt: ${esc(r.last_result)}</div>` : '';
        return `<div class="info-row"><span class="info-label">Status</span><span class="status-badge ${badge[0]}">${esc(badge[1])}</span></div>${detail}${last}${buttons}`;
    },
    firmware() {
        const s = state.status;
        if (!s) return '';
        return row('Version', s.version) + row('IDF', s.idf) + row('Build date', s.built) + row('Uptime', formatUptime(s.uptime_s))
            + row('Device time', s.time?.set ? s.time.now : 'not set yet');
    },
    actions() {
        const dump = state.status?.coredump;
        const crash = dump?.present
            ? '<div class="setting-hint" style="margin:12px 0 6px;">The device kept a record of its last crash'
                + `${dump.task ? ` (in ${esc(dump.task)})` : ''}. It helps find the fault. It is a copy of the device’s memory and may hold `
                + 'your Wi-Fi password and API keys: share it only with someone you trust.</div>'
                + '<div class="setting-row"><a class="btn" href="/api/v2/system/coredump" download>Download crash record</a>'
                + '<button type="button" class="btn btn-danger" id="erase-coredump">Erase it</button></div>'
            : '';
        return '<button type="button" class="btn danger-btn" id="reboot-btn">Reboot</button>' + crash
            + '<div class="setting-hint" style="margin:14px 0 6px;">A factory reset erases every setting, the Wi-Fi network and the Makapix registration. '
            + 'Files on the SD card are kept.</div>'
            + '<button type="button" class="btn danger-btn" id="factory-reset">Factory reset</button>';
    },
    about() {
        // The font's licence (CC BY 4.0) asks that its author be named where the software is presented.
        return row('Author', 'Fabrício Kury') + row('On-screen font', 'Everyday Ample by VEXED (CC BY 4.0)')
            + '<div class="info-row"><span class="info-label">Homepage</span><span class="info-value"><a href="https://github.com/fabkury/p3a" target="_blank" rel="noopener">github.com/fabkury/p3a</a></span></div>';
    },
    device() {
        const s = state.status;
        if (!s) return '';
        const r = s.recovery_restarts || {};
        return row('Free memory', s.memory ? `${formatBytes(s.memory.free)} (lowest ${formatBytes(s.memory.min_free)})` : '')
            + row('Wi-Fi crash reboots', String(r.wifi ?? 0)) + row('Touch crash reboots', String(r.touch ?? 0))
            + row('Link crash reboots', String(r.link ?? 0))
            + '<div class="setting-hint">A crash reboot happens when the Wi-Fi co-processor, the touch controller or their link stops responding and the device restarts to recover.</div>';
    },
};

function redraw(widget) {
    for (const el of document.querySelectorAll(`[data-widget="${widget}"]`)) el.innerHTML = widgets[widget]();
}

// --- the page ---------------------------------------------------------------------

function draw() {
    const tabs = layout(state.schema);
    $('tab-bar').innerHTML = tabs.map((t) =>
        `<button type="button" class="tab-item${t.id === state.tab ? ' active' : ''}" data-tab="${t.id}">${esc(t.label)}</button>`).join('');
    $('tabs').innerHTML = tabs.map((t) =>
        `<div class="tab-content${t.id === state.tab ? ' active' : ''}" id="tab-${t.id}">` + t.sections.map((s) => {
            const body = (s.widget ? `<div data-widget="${s.widget}">${widgets[s.widget]()}</div>` : '') + s.keys.map(settingHtml).join('');
            return body ? `<div class="section">${s.title ? `<h3>${esc(s.title)}</h3>` : ''}${body}</div>` : '';
        }).join('') + '</div>').join('');
}

function showTab(id) {
    if (!layout(state.schema).some((t) => t.id === id)) return;
    state.tab = id;
    for (const el of document.querySelectorAll('.tab-item')) el.classList.toggle('active', el.dataset.tab === id);
    for (const el of document.querySelectorAll('.tab-content')) el.classList.toggle('active', el.id === `tab-${id}`);
    history.replaceState(null, '', `#${id}`);
    if (id === 'makapix') makapixPoll.soon(0);
}

async function load() {
    const [schema, values, animations] = await Promise.all([
        fetch('/api/v2/settings/schema').then((r) => r.json()),
        get('/api/v2/settings'),
        get('/api/v2/ui/intro-animations').catch(() => ({ animations: [] })),
    ]);
    state.schema = schema;
    state.values = values;
    state.animations = animations.animations || [];
    for (const s of schema.settings) entries.set(s.key, s);
    const wanted = location.hash.slice(1);
    if (layout(schema).some((t) => t.id === wanted)) state.tab = wanted;
    draw();
}

const loaded = load().catch((e) => {
    $('tabs').innerHTML = `<div class="section"><p class="warning">The settings could not be loaded: ${esc(explain(e))}</p></div>`;
});

// The device's status: at load and every ten seconds.
poller(async () => {
    state.status = await get('/api/v2/status');
    await loaded;
    if (state.status.reboot_needed) $('reboot-notice').style.display = '';
    for (const w of ['connection', 'storage', 'firmware', 'device']) redraw(w);
    if (!document.querySelector('#reboot-btn.confirm')) redraw('actions');
}, 10000);

// Makapix: every two seconds while its tab is open (a code's countdown runs).
const makapixPoll = poller(async () => {
    await loaded;
    if (state.tab !== 'makapix' && state.makapix) return;
    state.makapix = await get('/api/v2/makapix');
    redraw('makapix');
}, 2000);

// --- what the controls do ---------------------------------------------------------

async function restart() {
    try {
        await post('/api/v2/system/reboot');
        toast('Restarting…');
        $('reboot-notice').style.display = 'none';
    } catch (e) {
        toast(explain(e), true);
    }
}

document.addEventListener('click', async (e) => {
    const t = e.target;
    const tab = t.closest('.tab-item');
    if (tab) {
        showTab(tab.dataset.tab);
        return;
    }
    if (t.dataset.save) {
        const key = t.dataset.save;
        const input = $(`set-${key}`);
        const entry = entries.get(key);
        if (entry.secret && input.value.trim() === '') {
            toast('Type the key first. To remove a stored key, use Remove.', true);
            return;
        }
        t.disabled = true;
        if (await save(key, input.value) && entry.secret) {
            input.value = '';
            draw();  // the badge and the Remove button
        }
        t.disabled = false;
    } else if (t.dataset.clear) {
        if (window.confirm('Remove the stored key?') && await save(t.dataset.clear, '')) draw();
    } else if (t.dataset.choice) {
        const key = t.dataset.choice;
        const group = t.parentElement;
        for (const b of group.children) b.disabled = true;
        if (await save(key, t.dataset.value)) {
            for (const b of group.children) b.classList.toggle('active', b === t);
        }
        for (const b of group.children) b.disabled = false;
    } else if (t.dataset.makapix) {
        try {
            if (t.dataset.makapix === 'register') {
                await post('/api/v2/makapix/registration');
            } else if (t.dataset.makapix === 'cancel') {
                await del('/api/v2/makapix/registration');
            } else if (window.confirm('Remove the Makapix Club registration? You can register again later.')) {
                await del('/api/v2/makapix');
                toast('Makapix Club registration removed');
            }
            makapixPoll.soon(300);
        } catch (err) {
            toast(explain(err), true);
        }
    } else if (t.id === 'reboot-now') {
        restart();
    } else if (t.id === 'reboot-btn') {
        // Two presses within two seconds: no dialog, and no accident.
        if (t.classList.contains('confirm')) {
            t.classList.remove('confirm');
            t.textContent = 'Reboot';
            restart();
        } else {
            t.classList.add('confirm');
            t.textContent = 'Click again to confirm';
            setTimeout(() => {
                t.classList.remove('confirm');
                t.textContent = 'Reboot';
            }, 2000);
        }
    } else if (t.id === 'factory-reset') {
        if (window.prompt('This erases every setting, the Wi-Fi network and the Makapix registration.\nType RESET to go ahead.') !== 'RESET') return;
        try {
            const accepted = await post('/api/v2/system/factory-reset', { confirm: 'factory-reset' });
            toast('Erasing… the device restarts into Wi-Fi setup.');
            job(accepted.job).catch(() => {});  // the device restarts: the answer may never come
        } catch (err) {
            toast(explain(err), true);
        }
    } else if (t.id === 'erase-wifi') {
        if (!window.confirm('Erase Wi-Fi credentials? Device will reboot into setup mode.')) return;
        try {
            await del('/api/v2/wifi');
            toast('Wi-Fi credentials erased. Device will reboot.');
        } catch (err) {
            toast('Failed to erase credentials', true);
        }
    } else if (t.id === 'giphy-reset-id') {
        if (!window.confirm('Reset Giphy Random ID? A new one will be obtained on next refresh.')) return;
        try {
            await post('/api/v2/sources/giphy/reset-id');
            toast('Random ID reset');
        } catch (err) {
            toast(explain(err), true);
        }
    } else if (t.id === 'erase-coredump') {
        if (!window.confirm('Erase the crash record?')) return;
        try {
            await del('/api/v2/system/coredump');
            state.status.coredump = { present: false };
            redraw('actions');
        } catch (err) {
            toast(explain(err), true);
        }
    }
});

document.addEventListener('change', async (e) => {
    const t = e.target;
    if (t.id === 'theme-select') {
        window.p3aTheme.set(t.value);
        toast(`Theme: ${t.selectedOptions[0].textContent}`);
    } else if (t.dataset.instant) {
        const key = t.dataset.instant;
        const before = state.values[key];
        const raw = t.type === 'checkbox' ? t.checked : t.dataset.numeric ? Number(t.value) : t.value;
        if (!(await save(key, raw))) {
            // Back to what the device has.
            if (t.type === 'checkbox') t.checked = !!before;
            else t.value = before;
        }
    } else if (t.dataset.zone) {
        const key = t.dataset.zone;
        const custom = $(`set-${key}-custom`);
        if (t.value === '') {
            custom.style.display = '';  // typed by hand, then Save
        } else {
            custom.style.display = 'none';
            $(`set-${key}`).value = t.value;
            await save(key, t.value);
        }
    }
});

document.addEventListener('input', (e) => {
    const t = e.target;
    if (t.dataset.range) {
        const c = controlFor(entries.get(t.dataset.range));
        $(`${t.id}-value`).textContent = `${t.value}${c.unit}`;
    }
});

window.addEventListener('hashchange', () => showTab(location.hash.slice(1)));
