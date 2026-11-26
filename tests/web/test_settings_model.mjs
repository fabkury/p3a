// SPDX-License-Identifier: Apache-2.0
// The settings page's logic, against the device's real schema
// (components/p3a_settings/settings.json): a setting the page names that
// the firmware lacks, or an option the firmware would refuse, fails here.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import {
    PRESENTATION, SECRET_MASK, TABS, ZONES, controlFor, layout, secretState, validate, withCurrent,
} from '../../components/p3a_net/static/static/settings-model.js';

const schema = JSON.parse(readFileSync(new URL('../../components/p3a_settings/settings.json', import.meta.url), 'utf8'));
const byKey = new Map(schema.settings.map((s) => [s.key, s]));
const entry = (key) => byKey.get(key);

test('everything the page names is a setting the firmware has', () => {
    for (const key of Object.keys(PRESENTATION)) assert.ok(byKey.has(key), `PRESENTATION names ${key}`);
    for (const tab of TABS) {
        for (const section of tab.sections) {
            for (const key of section.keys || []) assert.ok(byKey.has(key), `tab ${tab.id} names ${key}`);
        }
    }
});

test('every visible setting has exactly one place on the page', () => {
    const tabs = layout(schema);
    const placed = tabs.flatMap((t) => t.sections.flatMap((s) => s.keys));
    const visible = schema.settings.filter((s) => !s.advanced).map((s) => s.key);
    assert.deepEqual([...placed].sort(), [...visible].sort());
    assert.equal(new Set(placed).size, placed.length, 'none twice');
    // Advanced settings stay off the page.
    for (const s of schema.settings.filter((x) => x.advanced)) assert.ok(!placed.includes(s.key), s.key);
    assert.deepEqual(tabs.map((t) => t.id), ['display', 'network', 'giphy', 'klipy', 'museum', 'storage', 'makapix', 'system']);
});

test('a setting the page does not know yet still shows, in the tab of its group', () => {
    const grown = { settings: [...schema.settings,
        { key: 'giphy_new_thing', type: 'bool', default: false, group: 'giphy', label: 'A new thing' },
        { key: 'sound_volume', type: 'int', default: 5, min: 0, max: 10, group: 'sound', label: 'Volume' },
        { key: 'hidden_thing', type: 'bool', default: false, group: 'giphy', label: 'Hidden', advanced: true }] };
    const tabs = layout(grown);
    const giphy = tabs.find((t) => t.id === 'giphy');
    assert.deepEqual(giphy.sections.at(-1), { title: 'More', keys: ['giphy_new_thing'] });
    assert.deepEqual(tabs.at(-1).sections.at(-1), { title: 'Other settings', keys: ['sound_volume'] });
    // And one the firmware no longer has is dropped, not drawn empty.
    const shrunk = { settings: schema.settings.filter((s) => s.key !== 'brightness') };
    const display = layout(shrunk).find((t) => t.id === 'display');
    assert.deepEqual(display.sections.find((s) => s.title === 'Brightness').keys, []);
    assert.deepEqual(layout(null).flatMap((t) => t.sections.flatMap((s) => s.keys)), []);
});

test('each kind of setting gets its control', () => {
    assert.deepEqual(controlFor(entry('show_fps')).kind, 'toggle');
    const rating = controlFor(entry('giphy_rating'));
    assert.equal(rating.kind, 'select');
    assert.deepEqual(rating.options.map(([v]) => v), ['g', 'pg', 'pg-13', 'r']);
    assert.equal(rating.options[2][1], 'PG-13');
    const position = controlFor(entry('clock_position'));
    assert.deepEqual(position.options[0], ['top-left', 'top-left'], 'a value with no nicer name shows as itself');
    const rotation = controlFor(entry('rotation'));
    assert.equal(rotation.kind, 'choices');
    assert.deepEqual(rotation.options, [[0, '0°'], [90, '90°'], [180, '180°'], [270, '270°']]);
    const brightness = controlFor(entry('brightness'));
    assert.deepEqual([brightness.kind, brightness.min, brightness.max, brightness.step], ['range', 0, 100, 1]);
    const dwell = controlFor(entry('dwell_s'));
    assert.deepEqual([dwell.kind, dwell.min, dwell.max], ['number', 0, 86400]);
    assert.equal(controlFor(entry('background_color')).kind, 'color');
    assert.equal(controlFor(entry('giphy_api_key')).kind, 'secret');
    assert.equal(controlFor(entry('timezone')).kind, 'zone');
    const name = controlFor(entry('device_name'));
    assert.deepEqual([name.kind, name.reboot, name.maxLength], ['text', true, 16]);
    const animation = controlFor(entry('intro_animation'), { 'intro-animations': ['iris-wipe', 'typewriter'] });
    assert.deepEqual(animation.options, [['', 'Random (per boot)'], ['iris-wipe', 'iris-wipe'], ['typewriter', 'typewriter']]);
    assert.equal(animation.instant, true);
    // Instant controls save when changed; the others have a Save button.
    for (const s of schema.settings) {
        const c = controlFor(s);
        assert.equal(!!c.instant, ['toggle', 'select', 'choices', 'color'].includes(c.kind), s.key);
        assert.equal(c.label, s.label);
    }
});

test('a named option the device would refuse is never offered', () => {
    for (const s of schema.settings) {
        const c = controlFor(s);
        if (c.kind !== 'select' || !c.numeric) continue;
        assert.ok(c.options.length >= 2, `${s.key} offers a choice`);
        for (const [value] of c.options) {
            assert.ok(Number.isInteger(value) && value >= s.min && value <= s.max, `${s.key}: ${value}`);
        }
        assert.ok(c.options.some(([v]) => v === s.default), `${s.key}: the default is among the options`);
    }
    // An option outside the schema's range is left out.
    const narrow = { ...entry('giphy_refresh_s'), max: 7200 };
    assert.deepEqual(controlFor(narrow).options.map(([v]) => v), [3600, 7200]);
    // A stored value that is not an option (set through the API) is shown as itself.
    assert.deepEqual(withCurrent([[3600, '1 hour']], 5000), [[3600, '1 hour'], [5000, '5000']]);
    assert.deepEqual(withCurrent([[3600, '1 hour']], 3600), [[3600, '1 hour']]);
});

test('every default in the schema passes the page’s own check', () => {
    for (const s of schema.settings) {
        const checked = validate(s, s.default);
        assert.equal(checked.error, undefined, `${s.key}: ${checked.error}`);
    }
});

test('what a person types is checked before it is sent', () => {
    assert.deepEqual(validate(entry('dwell_s'), ' 45 '), { value: 45 });
    assert.match(validate(entry('dwell_s'), '86401').error, /must be 0 to 86400/);
    assert.match(validate(entry('dwell_s'), '-1').error, /must be 0 to 86400/);
    assert.match(validate(entry('dwell_s'), '4.5').error, /whole number/);
    assert.match(validate(entry('dwell_s'), '').error, /whole number/);
    assert.match(validate(entry('dwell_s'), '1e3').error, /whole number/);
    assert.deepEqual(validate(entry('rotation'), '270'), { value: 270 });
    assert.match(validate(entry('rotation'), '45').error, /one of 0, 90, 180, 270/);
    assert.deepEqual(validate(entry('pick_mode'), 'recency'), { value: 'recency' });
    assert.ok(validate(entry('pick_mode'), 'sideways').error);
    assert.deepEqual(validate(entry('background_color'), '#AABBCC'), { value: '#aabbcc' });
    assert.ok(validate(entry('background_color'), 'red').error);
    assert.deepEqual(validate(entry('show_fps'), 1), { value: true });

    assert.deepEqual(validate(entry('device_name'), ''), { value: '' });
    assert.deepEqual(validate(entry('device_name'), 'living-room-2'), { value: 'living-room-2' });
    for (const bad of ['Bedroom', '-bed', 'bed-', 'bed room', 'béd']) {
        assert.ok(validate(entry('device_name'), bad).error, bad);
    }
    assert.match(validate(entry('device_name'), 'a'.repeat(17)).error, /at most 16/);
    assert.deepEqual(validate(entry('giphy_country'), 'br'), { value: 'BR' });
    assert.deepEqual(validate(entry('giphy_country'), ''), { value: '' });
    assert.match(validate(entry('giphy_country'), 'B').error, /2 letters/);
    assert.match(validate(entry('giphy_country'), 'B1').error, /2 letters/);
    assert.deepEqual(validate(entry('sd_root'), ''), { value: '' });
    assert.deepEqual(validate(entry('sd_root'), '/art'), { value: '/art' });
    assert.match(validate(entry('sd_root'), 'art').error, /start with/);
    assert.match(validate(entry('sd_root'), '/').error, /just \//);
    assert.match(validate(entry('sd_root'), '/a/../b').error, /\.\./);
    // A key is trimmed (it was pasted); other text is kept as typed.
    assert.deepEqual(validate(entry('giphy_api_key'), '  abc123\n'), { value: 'abc123' });
    assert.match(validate(entry('giphy_api_key'), 'k'.repeat(65)).error, /at most 64/);
    // Length is counted in characters, as the device counts it.
    assert.deepEqual(validate(entry('giphy_country'), 'DE').value, 'DE');
});

test('a key is shown as set or not, never as itself', () => {
    assert.equal(secretState(SECRET_MASK), 'set');
    assert.equal(secretState(''), 'none');
    assert.equal(secretState(undefined), 'none');
});

test('the time zones offered are strings the device can take', () => {
    const tz = entry('timezone');
    const seen = new Set();
    for (const [posix, label] of ZONES) {
        assert.ok(label.length > 0);
        assert.ok(posix.length <= tz.max_len, `${label}: ${posix} is longer than the setting holds`);
        // Name, offset, and optionally a second name with two rules.
        assert.match(posix, /^(<[+-]\d\d(:\d\d)?>|[A-Z]{3,5})-?\d{1,2}(:\d\d)?((<[+-]\d\d>|[A-Z]{3,5})(,M\d{1,2}\.\d\.\d(\/\d{1,2})?){2})?$/, label);
        assert.ok(!seen.has(posix), `${posix} twice`);
        seen.add(posix);
    }
    assert.ok(seen.has(tz.default), 'the default zone is on the list');
});
