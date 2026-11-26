// SPDX-License-Identifier: Apache-2.0
// The update page: what runs, what is on offer, installing it, going back.
// What the device's answer means is update-model.js; this file is the page
// around it, and the wait while the device restarts into another firmware.

import { get, post } from './api.js';
import { esc, explain } from './lib.js';
import { outcome, plainNotes, view, waiting } from './update-model.js';
import { nav, poller, toast } from './ui.js';

const $ = (id) => document.getElementById(id);

// When the page asked for another firmware: which version, since when, and
// whether the device has stopped answering yet (it restarts after the job).
let asked = null;

nav('/update');

function note(tone, text) {
    return `<div class="update-note ${tone}">${esc(text)}</div>`;
}

function draw(u) {
    const v = view(u);
    $('versions').innerHTML = v.rows.map(([label, value], i) =>
        `<div class="version-row"><span class="version-label">${esc(label)}</span>`
        + `<span class="version-value${i === 1 && u.available ? ' update-available' : ''}">${esc(value)}</span></div>`).join('');
    const headline = $('headline');
    headline.className = `update-note ${v.tone === 'ok' ? 'info' : v.tone}`;
    headline.textContent = v.headline;
    $('progress').style.display = v.progress === null ? 'none' : 'block';
    if (v.progress !== null) {
        $('progress-fill').style.width = `${v.progress}%`;
        $('progress-text').textContent = `${v.progress}%`;
    }
    $('messages').innerHTML = v.messages.map((m) => note(m.tone, m.text)).join('');
    const notes = $('release-notes');
    notes.style.display = v.notes ? 'block' : 'none';
    notes.textContent = v.notes ? `${v.published ? `Released ${v.published}\n\n` : ''}${plainNotes(v.notes)}` : '';
    const install = $('install-btn');
    install.textContent = v.install.label;
    install.disabled = !v.install.enabled;
    install.title = v.install.why;
    const back = $('back-btn');
    back.style.display = v.back ? '' : 'none';
    if (v.back) {
        back.textContent = v.back.label;
        back.disabled = !v.back.enabled;
        back.dataset.version = u.other;
    }
    $('check-btn').disabled = !v.check.enabled;
    $('last-checked').textContent = v.lastChecked;
    document.getElementById('update-badge')?.classList.toggle('visible', !!u.available);
}

function drawAway(elapsed) {
    const w = waiting(elapsed);
    const headline = $('headline');
    headline.className = `update-note ${w.tone}`;
    headline.textContent = w.text;
    for (const id of ['install-btn', 'back-btn', 'check-btn']) $(id).disabled = true;
    $('progress').style.display = 'none';
    return w.give_up;
}

// The device answers again after the page asked for another firmware.
function settle(u) {
    const what = outcome(asked.version, u);
    if (what === 'done') {
        toast(`Now running ${u.running}`);
    } else if (what === 'undone') {
        toast(`${asked.version} did not start properly. The device went back to ${u.running}.`, true);
    } else if (!u.install_error) {
        toast(`The device is still running ${u.running}.`, true);
    }
    asked = null;
}

const status = poller(async () => {
    let u;
    try {
        u = await get('/api/v2/update', { timeout: 4000 });
    } catch (e) {
        if (asked) {
            asked.away = true;
            if (drawAway((Date.now() - asked.since) / 1000)) asked = null;
        } else {
            const headline = $('headline');
            headline.className = 'update-note error';
            headline.textContent = `The device does not answer (${explain(e)}).`;
        }
        return;
    }
    // Settled once the device has been away and is back, or the job ended without a restart.
    if (asked && (asked.away || (u.state === 'idle' && Date.now() - asked.since > 4000))) settle(u);
    draw(u);
}, 1500);

async function start(path, version, question) {
    if (!window.confirm(question)) return;
    try {
        await post(path);
        asked = { version, since: Date.now(), away: false };
        status.soon(0);
    } catch (e) {
        toast(explain(e), true);
    }
}

$('install-btn').addEventListener('click', async () => {
    const u = await get('/api/v2/update').catch(() => null);
    if (!u || !u.available) return;
    start('/api/v2/update/install', u.latest.version,
        `Install ${u.latest.version}? The device restarts when it is done. Do not unplug it meanwhile.`);
});

$('back-btn').addEventListener('click', () => {
    const version = $('back-btn').dataset.version;
    start('/api/v2/update/rollback', version, `Go back to ${version}? The device restarts.`);
});

$('check-btn').addEventListener('click', async () => {
    try {
        await post('/api/v2/update/check');
        status.soon(0);
    } catch (e) {
        toast(explain(e), true);
    }
});

// What this device is: its board does not change, so it is asked once.
get('/api/v2/status').then((s) => {
    const row = (label, value) => (value ? `<div class="version-row"><span class="version-label">${esc(label)}</span>`
        + `<span class="version-value">${esc(value)}</span></div>` : '');
    const c = s.coprocessor || {};
    $('device').innerHTML = row('Firmware built', s.built) + row('ESP-IDF', s.idf)
        + row('Chip', s.chip ? `${{ esp32p4: 'ESP32-P4' }[s.chip.model] || s.chip.model} rev ${s.chip.revision}` : '')
        + row('Wi-Fi co-processor', c.running ? `ESP32-C6, firmware ${c.running}` : 'ESP32-C6, not answering')
        + (c.locked ? note('warn', `The co-processor's firmware could not be updated to ${c.embedded}: it is locked at ${c.running}. Wi-Fi works; see the documentation.`) : '');
}).catch(() => {});
