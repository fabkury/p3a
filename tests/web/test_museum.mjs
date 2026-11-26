// SPDX-License-Identifier: Apache-2.0
// The museum browser against the firmware: every museum the page offers
// is one the device plays, under the same id and with the same axes
// (a channel's kind is "{id}:{axis}"). 1.x kept the two lists by hand.
// And the shared request helper.
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { test } from 'node:test';

import { offered } from '../../components/p3a_net/static/static/museum/browse.js';
import { getAdapter, listAdapters } from '../../components/p3a_net/static/static/museum/index.js';
import {
    IDENTIFIER_MAX_BYTES, cooldownSeconds, fitsIdentifier, mapLimit, noteRateLimit, retryAfterSeconds,
} from '../../components/p3a_net/static/static/museum/request.js';

const here = (path) => new URL(`../../${path}`, import.meta.url);

// The firmware's table: one museum_<id>.c each, with `.id = "..."` and `.axes = "a b c"`.
function firmwareMuseums() {
    const dir = here('components/source_museum/');
    const out = new Map();
    for (const name of readdirSync(dir)) {
        if (!/^museum_\w+\.c$/.test(name)) continue;
        const text = readFileSync(new URL(name, dir), 'utf8');
        const id = /\.id = "([^"]+)"/.exec(text);
        const axes = /\.axes = "([^"]+)"/.exec(text);
        if (id && axes) out.set(id[1], axes[1].split(' '));
    }
    return out;
}

test('the page’s museums are the firmware’s, id for id and axis for axis', () => {
    const firmware = firmwareMuseums();
    const adapters = listAdapters();
    assert.ok(firmware.size >= 9);
    assert.deepEqual(adapters.map((a) => a.id).sort(), [...firmware.keys()].sort());
    for (const adapter of adapters) {
        // An adapter without axes makes channels of the one axis "set".
        const axes = adapter.axes && adapter.axes.length ? adapter.axes.map((a) => a.name) : ['set'];
        assert.deepEqual(axes, firmware.get(adapter.id), adapter.id);
        assert.ok(adapter.displayName && adapter.shortName, adapter.id);
        for (const method of ['listCollections', 'listArtworks', 'thumbnailUrl']) {
            assert.equal(typeof adapter[method], 'function', `${adapter.id}.${method}`);
        }
        assert.equal(getAdapter(adapter.id), adapter);
    }
    assert.equal(getAdapter('louvre'), null);
});

test('a term is a channel’s identifier: the limit is the firmware’s', () => {
    const header = readFileSync(here('components/play_scheduler/include/playset.h'), 'utf8');
    assert.equal(IDENTIFIER_MAX_BYTES, Number(/#define PLAYSET_IDENT_LEN\s+(\d+)/.exec(header)[1]) - 1);
    assert.equal(fitsIdentifier('x'.repeat(95)), true);
    assert.equal(fitsIdentifier('x'.repeat(96)), false);
    assert.equal(fitsIdentifier('é'.repeat(48)), false, 'bytes, not characters');
    assert.equal(fitsIdentifier(''), true);
});

test('the files the adapters load are there, and their terms fit', () => {
    const cma = JSON.parse(readFileSync(here('components/p3a_net/static/static/museum/cma-terms.json'), 'utf8'));
    assert.deepEqual(Object.keys(cma.axes).sort(), ['department', 'type']);
    let longest = 0;
    for (const terms of Object.values(cma.axes)) {
        assert.ok(terms.length > 0);
        for (const t of terms) {
            assert.ok(fitsIdentifier(t.id), t.id);
            assert.equal(t.query, undefined, 'a term’s id is its full name: nothing to expand');
            longest = Math.max(longest, t.id.length);
        }
    }
    assert.ok(longest > 32, 'the names 1.x had to cut are whole');
    const rijks = JSON.parse(readFileSync(here('components/p3a_net/static/static/museum/rijks-sets.json'), 'utf8'));
    assert.ok(rijks.length > 100);
    for (const s of rijks) assert.ok(s.spec && s.name && fitsIdentifier(s.spec), JSON.stringify(s));
});

test('a museum that says "slow down" is left alone for as long as it asked', () => {
    assert.equal(retryAfterSeconds('120'), 120);
    assert.equal(retryAfterSeconds(null), 60);
    assert.equal(retryAfterSeconds('soon'), 60);
    assert.equal(retryAfterSeconds('0'), 60);
    assert.equal(retryAfterSeconds('999999'), 3600);
    const answer = (value) => ({ headers: { get: (name) => (name === 'Retry-After' ? value : null) } });
    assert.equal(cooldownSeconds('test-a', 1000), 0);
    assert.equal(noteRateLimit('test-a', answer('30'), 1000), 30);
    assert.equal(cooldownSeconds('test-a', 1000), 30);
    assert.equal(cooldownSeconds('test-a', 20500), 11);
    assert.equal(cooldownSeconds('test-a', 31000), 0);
    assert.equal(cooldownSeconds('test-b', 1000), 0, 'each museum has its own');
    // A shorter wait does not cut a longer one short.
    noteRateLimit('test-a', answer('100'), 40000);
    noteRateLimit('test-a', answer('5'), 41000);
    assert.equal(cooldownSeconds('test-a', 41000), 99);
    assert.equal(noteRateLimit('test-c', {}, 0), 60, 'an answer without headers');
});

test('many requests go out a few at a time, and come back in order', async () => {
    let running = 0, most = 0;
    const out = await mapLimit([5, 1, 4, 2, 3, 6, 0], 3, async (n, i) => {
        most = Math.max(most, ++running);
        await new Promise((r) => setTimeout(r, n));
        running--;
        return `${i}:${n}`;
    });
    assert.deepEqual(out, ['0:5', '1:1', '2:4', '3:2', '4:3', '5:6', '6:0']);
    assert.equal(most, 3);
    assert.deepEqual(await mapLimit([], 4, async () => 1), []);
    assert.deepEqual(await mapLimit([1, 2], 0, async (n) => n * 2), [2, 4]);
});

test('only what the device plays is offered, and what it cannot play now says why', () => {
    const adapters = [{ id: 'vam' }, { id: 'artic' }, { id: 'gone' }];
    const known = [{ id: 'vam' }, { id: 'artic', unavailable: 'its image server refuses clients that are not web browsers' }, { id: 'new' }];
    const rows = offered(adapters, known);
    assert.deepEqual(rows.map((r) => r.adapter.id), ['vam', 'artic']);
    assert.equal(rows[0].blocked, '');
    assert.match(rows[1].blocked, /refuses clients/);
    // The device could not be asked: everything is offered.
    assert.equal(offered(adapters, []).length, 3);
    assert.equal(offered(adapters, null).every((r) => r.blocked === ''), true);
});
