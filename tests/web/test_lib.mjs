// SPDX-License-Identifier: Apache-2.0
// The pages' shared logic (components/p3a_net/static/static/lib.js).
//   node --test tests/web
import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
    BUILTINS, UPLOAD_MAX_BYTES, activePill, banners, channelBadges, channelLabel, channelProblem, esc, explain,
    fitWithin, formatBytes, formatIn, formatUptime, isJpeg, isSmooth, playingName, uploadProblem, withExtension,
} from '../../components/p3a_net/static/static/lib.js';

test('text is made safe for markup', () => {
    assert.equal(esc('<img src=x onerror="a&b">'), '&lt;img src=x onerror=&quot;a&amp;b&quot;&gt;');
    assert.equal(esc("it's"), 'it&#39;s');
    assert.equal(esc(null), '');
    assert.equal(esc(undefined), '');
    assert.equal(esc(42), '42');
});

test('sizes and times read as a person would say them', () => {
    assert.equal(formatBytes(0), '0 B');
    assert.equal(formatBytes(1023), '1023 B');
    assert.equal(formatBytes(1536), '1.5 KB');
    assert.equal(formatBytes(5 * 1024 * 1024), '5.0 MB');
    assert.equal(formatBytes(29.11 * 1024 ** 3), '29.11 GB');
    assert.equal(formatIn(-1), '');
    assert.equal(formatIn(0), 'now');
    assert.equal(formatIn(45), 'in 45s');
    assert.equal(formatIn(600), 'in 10m');
    assert.equal(formatIn(14400), 'in 4h');
    assert.equal(formatIn(345600), 'in 4d');
    assert.equal(formatIn('soon'), '');
    assert.equal(formatUptime(59), '0m');
    assert.equal(formatUptime(3725), '1h 2m');
    assert.equal(formatUptime(200000), '2d 7h 33m');
});

test('a channel is named from its playset, or from what it is', () => {
    assert.equal(channelLabel({ source: 'giphy', kind: 'search', identifier: 'cats', display_name: 'Cats!' }), 'Cats!');
    assert.equal(channelLabel({ source: 'local' }), 'SD Card');
    assert.equal(channelLabel({ source: 'makapix', kind: 'all' }), 'All Artworks');
    assert.equal(channelLabel({ source: 'makapix', kind: 'promoted' }), 'Promoted Artworks');
    assert.equal(channelLabel({ source: 'makapix', kind: 'by_user', identifier: 'QVJ' }), 'Artist: QVJ');
    assert.equal(channelLabel({ source: 'makapix', kind: 'reactions', identifier: '' }), 'Reactions: unknown');
    assert.equal(channelLabel({ source: 'makapix', kind: 'hashtag', identifier: 'cats' }), '#cats');
    assert.equal(channelLabel({ source: 'giphy', kind: 'trending' }), 'Giphy Trending');
    assert.equal(channelLabel({ source: 'giphy', kind: 'search', identifier: 'pixel cats' }), 'Giphy: pixel cats');
    assert.equal(channelLabel({ source: 'klipy', kind: 'gif:trending' }), 'Klipy Trending');
    assert.equal(channelLabel({ source: 'klipy', kind: 'sticker:search', identifier: 'hello' }), 'Klipy Stickers: hello');
    assert.equal(channelLabel({ source: 'klipy', kind: 'trending' }), 'Klipy Trending');
    assert.equal(channelLabel({ source: 'museum', kind: 'cma:department', identifier: 'Japanese Art' }), 'CMA · Japanese Art');
    assert.equal(channelLabel({ source: 'pinned', identifier: '0badf00d' }), 'Pins');
    assert.equal(channelLabel({ source: 'something-new' }), 'something-new');
    assert.deepEqual(channelBadges({ source: 'makapix', kind: 'promoted' }), ['M', 'PR']);
    assert.deepEqual(channelBadges({ source: 'museum', kind: 'vam:category' }), ['A', 'VA']);
    assert.deepEqual(channelBadges({ source: 'klipy', kind: 'sticker:search' }), ['K', 'SE']);
    assert.deepEqual(channelBadges({ source: 'local' }), ['S', 'SD']);
    assert.deepEqual(channelBadges({ source: 'x', kind: '' }), ['?', '--']);
});

test('photographs are shown smoothly and pixel art sharply', () => {
    assert.equal(isSmooth('museum'), true);
    assert.equal(isSmooth('giphy'), true);
    assert.equal(isSmooth('klipy'), true);
    assert.equal(isSmooth('makapix'), false);
    assert.equal(isSmooth('local'), false);
});

test("a channel's error says what stands in its way", () => {
    assert.equal(channelProblem({ source: 'giphy' }), null);
    assert.deepEqual(channelProblem({ source: 'giphy', error: 'NO_KEY' }), { kind: 'needs-key' });
    assert.deepEqual(channelProblem({ source: 'makapix', error: 'NO_KEY' }), { kind: 'registration' });
    assert.deepEqual(channelProblem({ source: 'klipy', error: 'RATE_LIMITED' }), { kind: 'cooldown' });
    assert.deepEqual(channelProblem({ source: 'giphy', error: 'AUTH' }), { kind: 'auth' });
    assert.deepEqual(channelProblem({ source: 'museum', error: 'ESP_ERR_NOT_SUPPORTED' }), { kind: 'unavailable' });
    assert.deepEqual(channelProblem({ source: 'makapix', error: 'ESP_ERR_NOT_SUPPORTED' }), { kind: 'failed' });
    assert.deepEqual(channelProblem({ source: 'giphy', error: 'ESP_FAIL' }), { kind: 'failed' });
});

test('banners: one per source and problem, the card first', () => {
    assert.deepEqual(banners(null, null), []);
    assert.deepEqual(banners({ channels: [{ source: 'local' }] }, { storage: { health: 'ok', repairs: 0 } }), []);
    const failed = banners({ channels: [] }, { storage: { health: 'failed', repairs: 2 } });
    assert.deepEqual(failed.map((b) => [b.id, b.tone]), [['sd-failed', 'danger']]);
    const repaired = banners({ channels: [] }, { storage: { health: 'ok', repairs: 3 } });
    assert.equal(repaired[0].id, 'sd-repaired');
    assert.match(repaired[0].text, /\(3 since power-on\)/);
    const many = banners({
        channels: [
            { source: 'giphy', error: 'NO_KEY' }, { source: 'giphy', error: 'NO_KEY' },
            { source: 'klipy', error: 'AUTH' }, { source: 'makapix', error: 'NO_KEY' },
            { source: 'museum', error: 'RATE_LIMITED' }, { source: 'museum', error: 'ESP_ERR_NOT_SUPPORTED' },
            { source: 'giphy', error: 'ESP_FAIL' }, { source: 'makapix', error: 'AUTH' },
        ],
    }, null);
    assert.deepEqual(many.map((b) => b.id),
        ['giphy:needs-key', 'klipy:auth', 'makapix:registration', 'museum:cooldown', 'museum:unavailable', 'makapix:auth']);
    assert.equal(many[0].href, '/settings#giphy');
    assert.match(many[0].title, /Giphy API Key Needed/);
    assert.equal(many[2].href, '/settings#makapix');
    assert.equal(many[3].href, undefined);
    assert.match(many[5].title, /Registration Not Accepted/);
    for (const b of many) assert.ok(['danger', 'warn', 'info'].includes(b.tone) && b.title && b.text);
});

test('the pill that stands for what is playing', () => {
    assert.equal(activePill(null), '');
    assert.equal(activePill({ playset: 'Evening', channels: [{ source: 'local' }] }), 'playset:Evening');
    assert.equal(activePill({ playset: '', channels: [{ source: 'local', kind: '' }] }), 'builtin:local');
    assert.equal(activePill({ playset: '', channels: [{ source: 'makapix', kind: 'promoted', identifier: '' }] }),
        'builtin:makapix-promoted');
    assert.equal(activePill({ playset: '', channels: [{ source: 'makapix', kind: 'all' }] }), 'builtin:makapix-all');
    assert.equal(activePill({ playset: '', channels: [{ source: 'giphy', kind: 'trending' }] }), 'builtin:giphy-trending');
    // A search is not the Giphy pill, and two channels are no pill.
    assert.equal(activePill({ playset: '', channels: [{ source: 'giphy', kind: 'search', identifier: 'cats' }] }), '');
    assert.equal(activePill({ playset: '', channels: [{ source: 'local' }, { source: 'giphy', kind: 'trending' }] }), '');
    assert.equal(activePill({ playset: '', channels: [{ source: 'pinned', identifier: '0badf00d' }] }), 'pins:0badf00d');
    assert.equal(activePill({ playset: '', channels: [{ source: 'pinned', identifier: '' }] }, 'cafe0001'), 'pins:cafe0001');
    assert.equal(activePill({ playset: '', channels: [{ source: 'pinned' }] }), '');
    assert.equal(activePill({ playset: '', channels: [] }), '');
    // Every built-in is one channel the device can be handed as it is.
    for (const b of BUILTINS) {
        assert.equal(b.playset.channels.length, 1);
        assert.equal(activePill({ playset: '', channels: b.playset.channels }), b.id);
    }
});

test('the name over the now-playing card', () => {
    const pins = [{ slug: 'aaaa0001', name: 'My Pins', active: true }, { slug: 'bbbb0002', name: 'Cats', active: false }];
    assert.equal(playingName({ playset: 'Evening', channels: [] }), 'Evening');
    assert.equal(playingName({ playset: '', channels: [{ source: 'makapix', kind: 'promoted' }] }), 'Promoted Artworks');
    assert.equal(playingName({ playset: '', channels: [{ source: 'pinned', identifier: 'bbbb0002' }] }, pins), 'Cats');
    assert.equal(playingName({ playset: '', channels: [{ source: 'pinned', identifier: '' }] }, pins), 'My Pins');
    assert.equal(playingName({ playset: '', channels: [{ source: 'pinned', identifier: 'gone' }] }, pins), 'Pins');
    assert.equal(playingName({ playset: '', channels: [{ source: 'local' }, { source: 'giphy' }] }), '2 channels');
    assert.equal(playingName({ playset: '', channels: [] }), '');
    assert.equal(playingName(null), '');
});

test('what may be uploaded', () => {
    assert.equal(uploadProblem('cat.gif', 1000, 'image/gif'), '');
    assert.equal(uploadProblem('CAT.WEBP', UPLOAD_MAX_BYTES, ''), '');
    assert.match(uploadProblem('cat.gif', UPLOAD_MAX_BYTES + 1, 'image/gif'), /Maximum size is 16MiB/);
    // A photograph may be far larger: it is shrunk before it is sent.
    assert.equal(uploadProblem('photo.JPG', 80 * 1024 * 1024, 'image/jpeg'), '');
    assert.match(uploadProblem('photo.jpg', 300 * 1024 * 1024, 'image/jpeg'), /256MiB/);
    assert.match(uploadProblem('notes.txt', 10, 'text/plain'), /Not a picture/);
    assert.match(uploadProblem('cat', 10, 'image/gif'), /Not a picture/);
    assert.match(uploadProblem('a:b.gif', 10, 'image/gif'), /cannot have/);
    assert.match(uploadProblem(`${'n'.repeat(120)}.gif`, 10, 'image/gif'), /too long/);
    assert.match(uploadProblem(`${'é'.repeat(58)}.gif`, 10, 'image/gif'), /too long/);  // 120 bytes
    assert.equal(uploadProblem(`${'é'.repeat(57)}.gif`, 10, 'image/gif'), '');
    assert.equal(isJpeg('a.jpeg', ''), true);
    assert.equal(isJpeg('a.png', 'image/jpeg'), true);
    assert.equal(isJpeg('a.png', 'image/png'), false);
});

test('a photograph is shrunk to fit the screen, keeping its shape', () => {
    assert.equal(fitWithin(720, 720, 720), null);
    assert.equal(fitWithin(100, 50, 720), null);
    assert.deepEqual(fitWithin(1440, 720, 720), { width: 720, height: 360 });
    assert.deepEqual(fitWithin(3000, 4000, 720), { width: 540, height: 720 });
    assert.deepEqual(fitWithin(10000, 3, 720), { width: 720, height: 1 });
    assert.equal(withExtension('photo.jpeg', '.png'), 'photo.png');
    assert.equal(withExtension('my.holiday.JPG', '.png'), 'my.holiday.png');
    assert.equal(withExtension('photo', '.png'), 'photo.png');
});

test("a refusal is explained by its code, else in the device's words", () => {
    assert.match(explain({ code: 'PAUSED', message: 'x' }), /paused/);
    assert.match(explain({ code: 'CANNOT_BE_PINNED', message: 'x' }), /cannot be pinned/);
    assert.equal(explain({ code: 'SOMETHING_NEW', message: 'the device said this' }), 'the device said this');
    assert.equal(explain({}), 'Something went wrong.');
    assert.equal(explain(null), 'Something went wrong.');
});
