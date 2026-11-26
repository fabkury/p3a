// SPDX-License-Identifier: Apache-2.0
// The playset editor's logic, and its agreement with the firmware: the
// limits are read out of playset.h, so a limit changed there fails here.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import {
    CHOICES, DEFAULT_WEIGHT, DISPLAY_NAME_MAX, IDENTIFIER_MAX, KLIPY_CATEGORIES, MAX_CHANNELS, MAX_WEIGHT, NAME_MAX,
    addProblem, balance, hasOffset, identifierProblem, makeChannel, move, nameProblem, saveProblem, toDevice, typeLabel,
    wholeNumber,
} from '../../components/p3a_net/static/static/playset-model.js';

const header = readFileSync(new URL('../../components/play_scheduler/include/playset.h', import.meta.url), 'utf8');
const define = (name) => Number(new RegExp(`#define ${name}\\s+(\\d+)`).exec(header)[1]);

test('the limits are the firmware’s', () => {
    assert.equal(MAX_CHANNELS, define('PLAYSET_MAX_CHANNELS'));
    assert.equal(NAME_MAX, define('PLAYSET_NAME_LEN') - 1);
    assert.equal(IDENTIFIER_MAX, define('PLAYSET_IDENT_LEN') - 1);
    assert.equal(DISPLAY_NAME_MAX, define('PLAYSET_DISPLAY_LEN') - 1);
    assert.equal(MAX_WEIGHT, define('PLAYSET_MAX_WEIGHT'));
    // Every kind the page offers fits the field it goes into.
    const kindRoom = define('PLAYSET_KIND_LEN') - 1;
    for (const choice of CHOICES) {
        for (const k of choice.kinds) assert.ok(k.kind.length <= kindRoom, k.kind);
    }
    // And every source it offers is one the firmware names.
    const names = /"local"[^;]*/.exec(readFileSync(new URL('../../components/play_scheduler/playset.c', import.meta.url), 'utf8'))[0];
    for (const choice of CHOICES) assert.ok(names.includes(`"${choice.id}"`), choice.id);
});

test('what is offered', () => {
    assert.deepEqual(CHOICES.map((c) => c.id), ['makapix', 'giphy', 'klipy', 'museum', 'pinned', 'local']);
    assert.equal(KLIPY_CATEGORIES.length, 36);
    assert.equal(new Set(KLIPY_CATEGORIES).size, 36);
    assert.equal(hasOffset('makapix'), false);
    assert.equal(hasOffset('local'), false, 'an offset would hide the newest files');
    for (const s of ['giphy', 'klipy', 'museum', 'pinned']) assert.equal(hasOffset(s), true, s);
    assert.equal(typeLabel({ source: 'makapix', kind: 'by_user' }), 'Makapix Club · Artist');
    assert.equal(typeLabel({ source: 'klipy', kind: 'sticker:search' }), 'Klipy · Stickers: Search');
    assert.equal(typeLabel({ source: 'local', kind: '' }), 'Local SD Card');
    assert.equal(typeLabel({ source: 'museum', kind: 'vam:category' }), 'Museum');
    assert.equal(typeLabel({ source: 'new-thing', kind: '' }), 'new-thing');
});

test('an identifier is checked by its source’s rule', () => {
    assert.equal(identifierProblem('makapix', 'all', ''), '');
    assert.equal(identifierProblem('makapix', 'by_user', 'QVJ'), '');
    assert.match(identifierProblem('makapix', 'by_user', ''), /artist ID/);
    assert.match(identifierProblem('makapix', 'reactions', 'a b'), /letters and digits/);
    assert.equal(identifierProblem('makapix', 'hashtag', 'pixel_art-8'), '');
    assert.match(identifierProblem('makapix', 'hashtag', '#art'), /without the #/);
    assert.match(identifierProblem('makapix', 'hashtag', ''), /hashtag/);
    assert.equal(identifierProblem('giphy', 'trending', ''), '');
    assert.match(identifierProblem('giphy', 'search', '  '), /search query/);
    assert.equal(identifierProblem('giphy', 'search', 'pixel cats'), '');
    assert.match(identifierProblem('klipy', 'gif:search', ''), /search query/);
    assert.equal(identifierProblem('klipy', 'sticker:trending', ''), '');
    assert.equal(identifierProblem('klipy', 'gif:category', 'good morning'), '');
    assert.match(identifierProblem('museum', 'vam:category', ''), /museum browser/);
    assert.equal(identifierProblem('museum', 'cma:department', 'Egyptian and Ancient Near Eastern Art'), '');
    assert.equal(identifierProblem('pinned', '', ''), '', 'no list named is the active one');
    assert.equal(identifierProblem('local', '', ''), '');
    assert.match(identifierProblem('giphy', 'search', 'x'.repeat(96)), /too long/);
    assert.match(identifierProblem('giphy', 'search', 'é'.repeat(48)), /too long/, 'counted in bytes');
    assert.equal(identifierProblem('giphy', 'search', 'é'.repeat(47)), '');
});

test('a channel is made with a weight and no offset, and may be refused a place', () => {
    const ch = makeChannel('giphy', 'search', { identifier: ' cats ', display_name: '  My cats ' });
    assert.deepEqual(ch, { source: 'giphy', kind: 'search', identifier: 'cats', display_name: 'My cats', weight: DEFAULT_WEIGHT, offset: 0 });
    assert.equal(makeChannel('local').kind, '');
    assert.equal(makeChannel('giphy', 'trending', { display_name: 'n'.repeat(80) }).display_name.length, DISPLAY_NAME_MAX);
    assert.equal(addProblem([], ch), '');
    assert.match(addProblem([], makeChannel('giphy', 'search')), /search query/);
    assert.match(addProblem([makeChannel('local')], makeChannel('local')), /already added/);
    assert.equal(addProblem([makeChannel('local')], makeChannel('giphy', 'trending')), '');
    const full = Array.from({ length: MAX_CHANNELS }, () => makeChannel('giphy', 'trending'));
    assert.match(addProblem(full, ch), /Maximum 64 channels/);
});

test('a name is a file’s name', () => {
    assert.equal(nameProblem('Evening mix'), '');
    assert.equal(nameProblem('café ☕'), '');
    assert.match(nameProblem(''), /required/);
    assert.match(nameProblem('   '), /required/);
    assert.match(nameProblem('a/b'), /slash/);
    assert.match(nameProblem('a\\b'), /slash/);
    assert.match(nameProblem('two\nlines'), /control/);
    assert.match(nameProblem(' padded'), /space/);
    assert.match(nameProblem('n'.repeat(33)), /at most 32/);
    assert.equal(nameProblem('n'.repeat(32)), '');
    assert.equal(nameProblem('é'.repeat(32)), '', 'characters, not bytes');
});

test('what stops a save is said, first problem first', () => {
    const good = [makeChannel('makapix', 'promoted'), makeChannel('giphy', 'search', { identifier: 'cats' })];
    assert.equal(saveProblem('Mix', good), '');
    assert.match(saveProblem('', good), /Name is required/);
    assert.match(saveProblem('Mix', []), /at least one channel/);
    assert.match(saveProblem('Mix', [makeChannel('giphy', 'search')]), /Giphy: : Please enter a search query|search query/);
    assert.match(saveProblem('Mix', [{ ...good[0], weight: -1 }]), /weight is 0 to 1000000/);
    assert.match(saveProblem('Mix', [{ ...good[0], weight: 1.5 }]), /weight/);
    assert.match(saveProblem('Mix', [{ ...good[0], weight: MAX_WEIGHT + 1 }]), /weight/);
    assert.match(saveProblem('Mix', [{ ...good[0], offset: -3 }]), /offset/);
    assert.match(saveProblem('Mix', Array.from({ length: 65 }, () => good[0])), /Maximum 64/);
    assert.equal(wholeNumber('250', MAX_WEIGHT), 250);
    assert.equal(wholeNumber('', MAX_WEIGHT), 0);
    assert.equal(wholeNumber('-4', MAX_WEIGHT), 0);
    assert.equal(wholeNumber('abc', MAX_WEIGHT), 0);
    assert.equal(wholeNumber('99999999999', MAX_WEIGHT), MAX_WEIGHT);
    assert.equal(wholeNumber('12.9', MAX_WEIGHT), 12);
});

test('the balance bar shows each channel’s share of the turns', () => {
    const three = [{ ...makeChannel('local'), weight: 100 }, { ...makeChannel('giphy', 'trending'), weight: 300 },
        { ...makeChannel('makapix', 'promoted'), weight: 0 }];
    const bar = balance(three);
    assert.deepEqual(bar.map((s) => s.percent), [25, 75, 0]);
    assert.deepEqual(bar.map((s) => s.label), ['SD Card', 'Giphy Trending', 'Promoted Artworks']);
    assert.equal(bar[0].colour, '#667eea');
    // Every weight zero: all play equally.
    const muted = balance(three.map((c) => ({ ...c, weight: 0 })));
    assert.deepEqual(muted.map((s) => s.percent), [33.3, 33.3, 33.3]);
    assert.deepEqual(balance([]), []);
    // The colours go round.
    const many = balance(Array.from({ length: 8 }, () => makeChannel('giphy', 'trending')));
    assert.equal(many[6].colour, many[0].colour);
    assert.equal(many[7].index, 7);
});

test('channels are reordered without losing one', () => {
    const list = ['a', 'b', 'c', 'd'];
    assert.deepEqual(move(list, 0, 2), ['b', 'c', 'a', 'd']);
    assert.deepEqual(move(list, 3, 0), ['d', 'a', 'b', 'c']);
    assert.deepEqual(move(list, 1, 1), list);
    assert.deepEqual(move(list, 9, 0), list);
    assert.deepEqual(move(list, 0, -1), list);
    assert.notEqual(move(list, 1, 1), list, 'always a new array');
});

test('what is sent is what the device’s reader takes', () => {
    const sent = toDevice('Mix', [
        { source: 'makapix', kind: 'by_user', identifier: 'QVJ', display_name: 'Birds', weight: 3, offset: 40, verified: true },
        { source: 'giphy', kind: 'search', identifier: 'cats', display_name: '', weight: 0, offset: 12 },
        { source: 'local' },
    ]);
    assert.deepEqual(sent, { name: 'Mix', channels: [
        { source: 'makapix', kind: 'by_user', identifier: 'QVJ', display_name: 'Birds', weight: 3, offset: 0 },
        { source: 'giphy', kind: 'search', identifier: 'cats', display_name: '', weight: 0, offset: 12 },
        { source: 'local', kind: '', identifier: '', display_name: '', weight: 0, offset: 0 },
    ] });
    // The device refuses keys it does not know: none of the page's own travel.
    for (const ch of sent.channels) {
        assert.deepEqual(Object.keys(ch), ['source', 'kind', 'identifier', 'display_name', 'weight', 'offset']);
    }
});
