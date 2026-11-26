// SPDX-License-Identifier: Apache-2.0
// The update page's reading of the device's answer.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ago, outcome, plainNotes, view, waiting } from '../../components/p3a_net/static/static/update-model.js';

const idle = { running: '2.0.0', state: 'idle', on_trial: false, checked: false, available: false };
const release = { version: '2.0.1', prerelease: false, bytes: 2453520, published: '2027-01-02T03:04:05Z', notes: '## New\n\n- **Fixes**' };

test('before any look, after one that found nothing, and after one that failed', () => {
    let v = view(idle);
    assert.equal(v.headline, 'Not checked yet.');
    assert.deepEqual(v.rows, [['Installed', '2.0.0']]);
    assert.equal(v.install.enabled, false);
    assert.equal(v.install.label, 'Install update');
    assert.equal(v.check.enabled, true);
    assert.equal(v.back, null);
    assert.equal(v.progress, null);
    assert.match(v.lastChecked, /not since the device started/);
    assert.deepEqual(v.messages, []);

    v = view({ ...idle, checked: true, checked_ago_s: 200 });
    assert.equal(v.headline, 'This device is up to date.');
    assert.equal(v.tone, 'ok');
    assert.equal(v.lastChecked, 'Last checked: 3 minutes ago');

    // The newest release is what already runs, or older: shown, not offered.
    v = view({ ...idle, checked: true, checked_ago_s: 5, latest: { ...release, version: '2.0.0' } });
    assert.deepEqual(v.rows[1], ['Newest release', '2.0.0']);
    assert.equal(v.install.enabled, false);
    assert.equal(v.notes, '');

    v = view({ ...idle, checked: true, checked_ago_s: 5, check_error: 'RATE_LIMITED' });
    assert.equal(v.headline, 'Could not look for updates.');
    assert.equal(v.tone, 'error');
    assert.match(v.messages[0].text, /limiting requests/);
    v = view({ ...idle, checked: true, checked_ago_s: 5, check_error: 'ESP_FAIL' });
    assert.match(v.messages[0].text, /could not reach GitHub \(ESP_FAIL\)/);
});

test('a newer release is offered with its size and notes', () => {
    const v = view({ ...idle, checked: true, checked_ago_s: 30, available: true, latest: release });
    assert.equal(v.headline, 'Version 2.0.1 is available.');
    assert.deepEqual(v.rows[1], ['Available', '2.0.1 · 2.3 MB']);
    assert.deepEqual(v.install, { label: 'Install 2.0.1', enabled: true, why: '' });
    assert.equal(v.notes, release.notes);
    assert.equal(v.published, '2027-01-02');
    const pre = view({ ...idle, checked: true, available: true, latest: { ...release, version: '2.1.0-rc.1', prerelease: true } });
    assert.match(pre.rows[1][1], /^2\.1\.0-rc\.1 \(pre-release\)/);
});

test('while the device is busy nothing else can be started', () => {
    let v = view({ ...idle, state: 'checking', checked: true, checked_ago_s: 900, check_error: 'MISSING' });
    assert.equal(v.headline, 'Looking for releases…');
    assert.equal(v.check.enabled, false);
    assert.deepEqual(v.messages, [], 'an old error is not shown while a new look runs');

    v = view({ ...idle, state: 'installing', progress: 42, checked: true, available: true, latest: release, other: '1.9.0' });
    assert.match(v.headline, /Installing 2\.0\.1.*Do not unplug/);
    assert.equal(v.progress, 42);
    assert.equal(v.install.enabled, false);
    assert.equal(v.check.enabled, false);
    assert.deepEqual(v.back, { label: 'Go back to 1.9.0', enabled: false });
    assert.equal(view({ ...idle, state: 'installing', progress: 250, available: true, latest: release }).progress, 100);
    assert.equal(view({ ...idle, state: 'installing', available: true, latest: release }).progress, 0);

    v = view({ ...idle, state: 'restarting', available: true, latest: release });
    assert.equal(v.headline, 'Restarting…');
    assert.equal(v.install.enabled, false);
});

test('what else the device says is said', () => {
    let v = view({ ...idle, on_trial: true, other: '1.9.0', checked: true });
    assert.deepEqual(v.rows, [['Installed', '2.0.0 (on trial)'], ['Also on this device', '1.9.0']]);
    assert.match(v.messages[0].text, /just installed.*ten minutes/);
    assert.deepEqual(v.back, { label: 'Go back to 1.9.0', enabled: true });

    v = view({ ...idle, checked: true, available: true, latest: release, undone: '2.0.1' });
    assert.match(v.messages[0].text, /2\.0\.1 was installed before and the device went back from it: installing it again/);
    assert.equal(v.messages[0].tone, 'warn');
    assert.equal(v.install.enabled, true, 'the owner may try again');
    // After going back, the newer firmware is still in the other slot: switching needs no download.
    v = view({ ...idle, checked: true, available: true, latest: release, other: '2.0.1' });
    assert.deepEqual(v.back, { label: 'Switch to 2.0.1 (already on this device)', enabled: true });
    v = view({ ...idle, checked: true, undone: '2.0.1' });
    assert.match(v.messages[0].text, /went back from it\.$/);

    v = view({ ...idle, checked: true, install_error: { code: 'CORRUPT', message: 'the download does not match the release’s checksum' } });
    assert.match(v.messages[0].text, /last install failed: the download does not match.*kept running 2\.0\.0/);

    v = view({ ...idle, checked: true, available: true, latest: release, blocked: 'CARD_IN_USE' });
    assert.equal(v.install.enabled, false);
    assert.match(v.install.why, /SD card over USB/);
    assert.match(v.messages[0].text, /Unplug it/);
    v = view({ ...idle, checked: true, available: true, latest: release, blocked: 'SOMETHING_NEW' });
    assert.match(v.messages[0].text, /SOMETHING_NEW/);
});

test('after the restart the page knows what became of what it asked for', () => {
    assert.equal(outcome('2.0.1', { running: '2.0.1', on_trial: true }), 'done');
    assert.equal(outcome('2.0.1', { running: '2.0.0', undone: '2.0.1' }), 'undone');
    assert.equal(outcome('2.0.1', { running: '2.0.0' }), 'failed');
    assert.equal(outcome('2.0.0', { running: '2.0.0', other: '2.0.1' }), 'done', 'going back is asked for by version too');
});

test('while the device is away the page waits, and says when to stop waiting', () => {
    assert.deepEqual(waiting(3), { tone: 'info', text: 'The device is restarting…', give_up: false });
    assert.match(waiting(60).text, /Still waiting/);
    assert.equal(waiting(179).give_up, false);
    const late = waiting(180);
    assert.equal(late.give_up, true);
    assert.equal(late.tone, 'error');
    assert.equal(waiting(20, 10).give_up, true);
});

test('times and notes are made readable', () => {
    assert.equal(ago(0), 'just now');
    assert.equal(ago(59), 'just now');
    assert.equal(ago(60), '1 minute ago');
    assert.equal(ago(3599), '59 minutes ago');
    assert.equal(ago(3600), '1 hour ago');
    assert.equal(ago(86400 * 2 + 5), '2 days ago');
    assert.equal(ago(-5), 'just now');
    assert.equal(ago(undefined), 'just now');
    assert.equal(plainNotes('## What’s new\r\n\r\n- **Fixes** the `clock`\n- See [the guide](https://example.com/g)\n\n\n\nMore'),
        'What’s new\n\n- Fixes the clock\n- See the guide (https://example.com/g)\n\nMore');
    // Nothing in the notes becomes markup: the page sets them as text, and this leaves tags as they are.
    assert.equal(plainNotes('<script>alert(1)</script>'), '<script>alert(1)</script>');
    assert.equal(plainNotes('[x](javascript:alert(1))'), '[x](javascript:alert(1))', 'only http links are rewritten');
    assert.equal(plainNotes(null), '');
});
