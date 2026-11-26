// SPDX-License-Identifier: Apache-2.0
// The update page's logic without its DOM: what the device's answer
// (GET /api/v2/update) means for a person, and what the page should say
// while the device restarts into another firmware. Tested with node
// (tests/web/test_update_model.mjs).

import { formatBytes } from './lib.js';

// "3 minutes ago" for a number of seconds.
export function ago(seconds) {
    const s = Math.max(0, Math.floor(Number(seconds) || 0));
    if (s < 60) return 'just now';
    const units = [[86400, 'day'], [3600, 'hour'], [60, 'minute']];
    for (const [size, name] of units) {
        if (s >= size) {
            const n = Math.floor(s / size);
            return `${n} ${name}${n === 1 ? '' : 's'} ago`;
        }
    }
    return 'just now';
}

const CHECK_ERRORS = {
    RATE_LIMITED: 'GitHub is limiting requests from this network. Try again in an hour.',
    AUTH: 'GitHub is limiting requests from this network. Try again in an hour.',
    MISSING: 'The list of releases was not found.',
    CORRUPT: 'GitHub’s answer could not be read.',
    ESP_ERR_NO_MEM: 'The device is short of memory right now. Try again in a moment.',
};

const BLOCKED = {
    CARD_IN_USE: 'A computer has the SD card over USB. Unplug it to install an update.',
};

// What the page shows for an answer of the device.
//   status      one of 'idle', 'checking', 'installing', 'restarting'
//   headline    one line saying where things stand
//   tone        'ok' | 'info' | 'warn' | 'error': how the headline is dressed
//   rows        [label, value] pairs for the versions table
//   notes       the release's text, or ''
//   progress    0..100 while installing, else null
//   install     {label, enabled, why}: the install button
//   check       {enabled}
//   back        {label, enabled} or null: going back to the other firmware
//   messages    further lines, each {tone, text}
export function view(u) {
    const latest = u.latest || null;
    const busy = u.state !== 'idle';
    const rows = [['Installed', u.running + (u.on_trial ? ' (on trial)' : '')]];
    if (latest) {
        rows.push([u.available ? 'Available' : 'Newest release',
            `${latest.version}${latest.prerelease ? ' (pre-release)' : ''}`
            + (u.available && latest.bytes ? ` · ${formatBytes(latest.bytes)}` : '')]);
    }
    if (u.other) rows.push(['Also on this device', u.other]);

    let headline, tone = 'ok';
    if (u.state === 'installing') {
        headline = `Installing ${latest ? latest.version : 'the update'}… Do not unplug the device.`;
        tone = 'info';
    } else if (u.state === 'restarting') {
        headline = 'Restarting…';
        tone = 'info';
    } else if (u.state === 'checking') {
        headline = 'Looking for releases…';
        tone = 'info';
    } else if (u.available) {
        headline = `Version ${latest.version} is available.`;
        tone = 'info';
    } else if (u.checked && u.check_error) {
        headline = 'Could not look for updates.';
        tone = 'error';
    } else if (u.checked) {
        headline = 'This device is up to date.';
    } else {
        headline = 'Not checked yet.';
    }

    const messages = [];
    if (u.checked && u.check_error && u.state !== 'checking') {
        messages.push({ tone: 'error', text: CHECK_ERRORS[u.check_error] || `The device could not reach GitHub (${u.check_error}).` });
    }
    if (u.install_error && u.state === 'idle') {
        messages.push({ tone: 'error', text: `The last install failed: ${u.install_error.message}. The device kept running ${u.running}.` });
    }
    if (u.on_trial) {
        messages.push({ tone: 'info', text: 'This firmware was just installed. It is kept once the device is back on the network; '
            + 'if it cannot get there within ten minutes, the device goes back by itself.' });
    }
    if (u.undone) {
        messages.push({ tone: 'warn', text: `Version ${u.undone} was installed before and the device went back from it`
            + `${u.available && latest.version === u.undone ? ': installing it again may end the same way' : ''}.` });
    }
    if (u.blocked) {
        messages.push({ tone: 'warn', text: BLOCKED[u.blocked] || `An update cannot be installed now (${u.blocked}).` });
    }

    let why = '';
    if (!u.available) why = 'Nothing newer to install';
    else if (busy) why = 'Busy';
    else if (u.blocked) why = BLOCKED[u.blocked] || u.blocked;
    return {
        status: u.state,
        headline,
        tone,
        rows,
        notes: u.available && latest ? latest.notes || '' : '',
        published: latest && latest.published ? latest.published.slice(0, 10) : '',
        progress: u.state === 'installing' ? Math.max(0, Math.min(100, Number(u.progress) || 0)) : null,
        install: { label: u.available ? `Install ${latest.version}` : 'Install update', enabled: why === '', why },
        check: { enabled: !busy },
        // The other slot usually holds the firmware from before; after going back it holds the newer one.
        back: u.other ? {
            label: u.available && latest.version === u.other ? `Switch to ${u.other} (already on this device)` : `Go back to ${u.other}`,
            enabled: !busy,
        } : null,
        lastChecked: u.checked ? `Last checked: ${ago(u.checked_ago_s)}` : 'Last checked: not since the device started',
        messages,
    };
}

// The page asked for `target` (a version) to become the running firmware,
// by an install or by going back, and the device has restarted and
// answers again with `u`. What happened?
//   'done'    it runs
//   'undone'  the device tried it and went back
//   'failed'  it runs what it ran before, with a reason or without
export function outcome(target, u) {
    if (u.running === target) return 'done';
    if (u.undone === target) return 'undone';
    return 'failed';
}

// What to say while the device does not answer: it is restarting, and a
// restart into another firmware takes about half a minute. After `limit`
// seconds something is wrong.
export function waiting(elapsedSeconds, limit = 180) {
    if (elapsedSeconds >= limit) {
        return { tone: 'error', text: 'The device has not come back. Check that it is powered and on the network, then reload this page.', give_up: true };
    }
    if (elapsedSeconds >= 45) {
        return { tone: 'info', text: 'Still waiting for the device to come back…', give_up: false };
    }
    return { tone: 'info', text: 'The device is restarting…', give_up: false };
}

// The release's notes are Markdown from GitHub. The page shows them as
// text, with the few marks that read badly as text taken out. Never HTML.
export function plainNotes(markdown) {
    return String(markdown || '')
        .replace(/\r\n/g, '\n')
        .replace(/^#{1,6}\s*/gm, '')
        .replace(/\*\*([^*\n]+)\*\*/g, '$1')
        .replace(/`([^`\n]+)`/g, '$1')
        .replace(/\[([^\]\n]+)\]\((https?:\/\/[^)\s]+)\)/g, '$1 ($2)')
        .replace(/\n{3,}/g, '\n\n')
        .trim();
}
