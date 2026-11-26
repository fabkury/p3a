// SPDX-License-Identifier: Apache-2.0
// What the pages work out from what the device says: names, labels, what is
// wrong and what to tell the person. No DOM and no requests here, so that
// node can test it (tools/webui/test_lib.mjs).

export function esc(text) {
    return String(text ?? '').replace(/[&<>"']/g, (c) =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

export function formatBytes(bytes) {
    const n = Number(bytes) || 0;
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    if (n < 1024 * 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MB`;
    return `${(n / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

// "in 5m", "in 2h", "now": how long until something, from seconds.
export function formatIn(seconds) {
    const s = Number(seconds);
    if (!Number.isFinite(s) || s < 0) return '';
    if (s < 5) return 'now';
    if (s < 90) return `in ${Math.round(s)}s`;
    if (s < 5400) return `in ${Math.round(s / 60)}m`;
    if (s < 129600) return `in ${Math.round(s / 3600)}h`;
    return `in ${Math.round(s / 86400)}d`;
}

export function formatUptime(seconds) {
    const s = Math.max(0, Math.floor(Number(seconds) || 0));
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    return d > 0 ? `${d}d ${h}h ${m}m` : h > 0 ? `${h}h ${m}m` : `${m}m`;
}

// --- sources and channels -----------------------------------------------------

export const SOURCE_NAMES = {
    local: 'SD Card', makapix: 'Makapix Club', giphy: 'Giphy', klipy: 'Klipy', museum: 'Museum', pinned: 'Pins',
};

// Sources whose pictures are photographs and paintings: shown smoothly. The
// rest is pixel art, shown with sharp pixels, as the device does.
export function isSmooth(source) {
    return source === 'giphy' || source === 'klipy' || source === 'museum';
}

const MAKAPIX_KINDS = {
    all: 'All Artworks', promoted: 'Promoted Artworks', user: 'My Artworks',
};

// What to call a channel when its playset gives it no name.
export function channelLabel(ch) {
    if (ch.display_name) return ch.display_name;
    const id = ch.identifier || '';
    switch (ch.source) {
        case 'local': return 'SD Card';
        case 'makapix':
            if (ch.kind === 'by_user') return `Artist: ${id || 'unknown'}`;
            if (ch.kind === 'reactions') return `Reactions: ${id || 'unknown'}`;
            if (ch.kind === 'hashtag') return `#${id || 'tag'}`;
            return MAKAPIX_KINDS[ch.kind] || 'Makapix Club';
        case 'giphy':
            return ch.kind === 'search' ? `Giphy: ${id}` : 'Giphy Trending';
        case 'klipy': {
            const [product, mode] = String(ch.kind || '').includes(':') ? ch.kind.split(':') : ['gif', ch.kind];
            const what = product === 'sticker' ? 'Klipy Stickers' : 'Klipy';
            return mode === 'trending' || !id ? `${what} Trending` : `${what}: ${id}`;
        }
        case 'museum': {
            const museum = String(ch.kind || '').split(':')[0];
            return id ? `${museum.toUpperCase()} · ${id}` : museum.toUpperCase() || 'Museum';
        }
        case 'pinned': return 'Pins';
        default: return ch.source || 'Channel';
    }
}

// One letter for the source and two for the kind, as the now-playing rows show them.
export function channelBadges(ch) {
    const letter = { local: 'S', makapix: 'M', giphy: 'G', klipy: 'K', museum: 'A', pinned: 'P' }[ch.source] || '?';
    const kind = String(ch.kind || '');
    let sub;
    if (ch.source === 'museum') sub = kind.split(':')[0].slice(0, 2);
    else if (ch.source === 'klipy') sub = (kind.split(':')[1] || kind).slice(0, 2);
    else if (ch.source === 'local') sub = 'sd';
    else if (ch.source === 'pinned') sub = 'pn';
    else sub = { all: 'al', promoted: 'pr', by_user: 'us', hashtag: 'ht', reactions: 're', trending: 'tr', search: 'se' }[kind]
        || kind.slice(0, 2);
    return [letter, (sub || '--').toUpperCase()];
}

// What stands in a channel's way, from its last listing's error. `null`
// when nothing does.
//   kind: 'needs-key' | 'registration' | 'cooldown' | 'auth' | 'unavailable' | 'failed'
export function channelProblem(ch) {
    if (!ch.error) return null;
    switch (ch.error) {
        case 'NO_KEY': return { kind: ch.source === 'makapix' ? 'registration' : 'needs-key' };
        case 'RATE_LIMITED': return { kind: 'cooldown' };
        case 'AUTH': return { kind: 'auth' };
        case 'ESP_ERR_NOT_SUPPORTED': return { kind: ch.source === 'museum' ? 'unavailable' : 'failed' };
        default: return { kind: 'failed' };
    }
}

// The banners the home page shows, from the player's channels and the
// device's status. Each: {id, tone: 'danger' | 'warn' | 'info', title, text, href?}.
// One banner per source and kind of problem, whatever the number of channels.
export function banners(player, status, update) {
    const out = [];
    if (update?.available && update.latest) {
        out.push({
            id: 'update', tone: 'info', title: '\u21E1 Update Available', href: '/update',
            text: `Version ${update.latest.version} of the firmware is available. Tap here to see it. Nothing is installed until you say so.`,
        });
    }
    const storage = status?.storage;
    if (storage && storage.health && storage.health !== 'ok') {
        out.push({
            id: 'sd-failed', tone: 'danger', title: '⚠ SD Card Failure',
            text: 'The SD card is not working. Saving and downloads are disabled; playback may continue from what is '
                + 'already loaded. Power off, replace or reformat the card, and restart the device. To reformat it, '
                + 'long-press the screen and use the Format option.',
        });
    } else if (storage && storage.repairs > 0) {
        out.push({
            id: 'sd-repaired', tone: 'warn', title: '⚠ SD Card Corruption Repaired',
            text: `p3a found damaged folders on the SD card and repaired them (${storage.repairs} since power-on). `
                + 'Artwork is downloaded again as needed. A card that needs repairing is losing data: back it up and '
                + 'consider replacing it.',
        });
    }
    const seen = new Set();
    for (const ch of player?.channels || []) {
        const problem = channelProblem(ch);
        if (!problem) continue;
        const key = `${ch.source}:${problem.kind}`;
        if (seen.has(key)) continue;
        seen.add(key);
        const name = SOURCE_NAMES[ch.source] || ch.source;
        const settings = `/settings#${ch.source}`;
        if (problem.kind === 'registration') {
            out.push({
                id: key, tone: 'danger', title: '👑 Makapix Registration Needed', href: '/settings#makapix',
                text: 'This playset has Makapix Club channels that need a linked account, so they show no artwork. '
                    + 'Tap here to register this device in Settings → Makapix.',
            });
        } else if (problem.kind === 'needs-key') {
            out.push({
                id: key, tone: 'warn', title: `🔑 ${name} API Key Needed`, href: settings,
                text: `This playset has ${name} channels but no API key is set, so they cannot refresh. `
                    + 'Tap here to add a key in Settings.',
            });
        } else if (problem.kind === 'auth') {
            out.push(ch.source === 'makapix' ? {
                id: key, tone: 'danger', title: '⚠ Makapix Registration Not Accepted', href: '/settings#makapix',
                text: 'Makapix Club does not accept this device’s registration. Tap here to register it again.',
            } : {
                id: key, tone: 'danger', title: `⚠ Invalid ${name} API Key`, href: settings,
                text: `${name} rejected the API key. Its channels keep playing what they have but will not refresh. `
                    + 'Tap here to update the key in Settings.',
            });
        } else if (problem.kind === 'cooldown') {
            out.push({
                id: key, tone: 'warn', title: `⏳ ${name} Rate Limited`,
                text: `${name} asked the device to slow down. It waits before refreshing those channels.`,
            });
        } else if (problem.kind === 'unavailable') {
            out.push({
                id: key, tone: 'info', title: '⛔ Museum Temporarily Unavailable',
                text: 'A museum in this playset blocks direct access from apps and devices, so no new artwork can be '
                    + 'fetched from it. Art already downloaded keeps playing.',
            });
        }
        // 'failed' (the network, a server having a bad moment) is shown on the channel's row only.
    }
    return out;
}

// --- the pill bar ---------------------------------------------------------------

// The playsets the home page always offers. `needs`: 'registration' when
// the channel lists nothing without one.
export const BUILTINS = [
    { id: 'builtin:makapix-all', label: 'All Makapix', needs: 'registration',
      playset: { name: '', channels: [{ source: 'makapix', kind: 'all' }] } },
    { id: 'builtin:makapix-promoted', label: 'Promoted',
      playset: { name: '', channels: [{ source: 'makapix', kind: 'promoted' }] } },
    { id: 'builtin:local', label: 'SD Card',
      playset: { name: '', channels: [{ source: 'local' }] } },
    { id: 'builtin:giphy-trending', label: 'Giphy',
      playset: { name: '', channels: [{ source: 'giphy', kind: 'trending' }] } },
];

// Which pill stands for what is playing: 'playset:<name>', 'pins:<slug>' or
// a built-in's id; '' when none does. `activeSlug` is the active pin list's
// (a pin channel with no identifier plays that one).
export function activePill(player, activeSlug) {
    if (!player) return '';
    if (player.playset) return `playset:${player.playset}`;
    const channels = player.channels || [];
    if (channels.length !== 1) return '';
    const ch = channels[0];
    if (ch.source === 'pinned') {
        const slug = ch.identifier || activeSlug || '';
        return slug ? `pins:${slug}` : '';
    }
    for (const b of BUILTINS) {
        const want = b.playset.channels[0];
        if (want.source === ch.source && (want.kind || '') === (ch.kind || '') && !ch.identifier && !ch.offset) {
            return b.id;
        }
    }
    return '';
}

// The name over the now-playing card.
export function playingName(player, pinLists) {
    if (!player) return '';
    if (player.playset) return player.playset;
    const channels = player.channels || [];
    if (channels.length === 1) {
        const ch = channels[0];
        if (ch.source === 'pinned') {
            const list = (pinLists || []).find((l) => (ch.identifier ? l.slug === ch.identifier : l.active));
            return list ? list.name : 'Pins';
        }
        return channelLabel(ch);
    }
    return channels.length ? `${channels.length} channels` : '';
}

// --- uploads --------------------------------------------------------------------

export const UPLOAD_MAX_BYTES = 16 * 1024 * 1024;        // what the device takes
export const JPEG_SOURCE_MAX_BYTES = 256 * 1024 * 1024;  // a photograph is shrunk in the browser first
const PICTURE_ENDINGS = ['.webp', '.gif', '.png', '.apng', '.jpg', '.jpeg', '.bmp'];

export function isJpeg(name, type) {
    return /\.jpe?g$/i.test(name || '') || type === 'image/jpeg';
}

// Why a file cannot be uploaded, or '' when it can.
export function uploadProblem(name, size, type) {
    const lower = String(name || '').toLowerCase();
    if (!PICTURE_ENDINGS.some((e) => lower.endsWith(e))) {
        return 'Not a picture p3a plays (WebP, GIF, PNG, JPEG or BMP).';
    }
    const limit = isJpeg(name, type) ? JPEG_SOURCE_MAX_BYTES : UPLOAD_MAX_BYTES;
    if (size > limit) return `File too large! Maximum size is ${limit / (1024 * 1024)}MiB.`;
    if (/[\\/:*?"<>|]/.test(name)) return 'The name has characters a file on the card cannot have.';
    if (new TextEncoder().encode(name).length > 119) return 'The name is too long (119 bytes at most).';
    return '';
}

// The size a photograph is shrunk to so that it fits the screen; null when
// it already does.
export function fitWithin(width, height, max) {
    if (width <= max && height <= max) return null;
    const scale = Math.min(max / width, max / height);
    return { width: Math.max(1, Math.round(width * scale)), height: Math.max(1, Math.round(height * scale)) };
}

// "photo.jpeg" to "photo.png".
export function withExtension(name, ext) {
    const dot = name.lastIndexOf('.');
    return (dot > 0 ? name.slice(0, dot) : name) + ext;
}

// --- messages -------------------------------------------------------------------

// What to say when the device refuses something, from the error's code;
// the device's own words when the code is not one the page has better ones for.
export function explain(error) {
    switch (error?.code) {
        case 'PAUSED': return 'The player is paused. Resume it first.';
        case 'NOTHING_TO_PLAY': return 'There is nothing to play yet.';
        case 'CARD_IN_USE': return 'The SD card is in use by a computer over USB.';
        case 'OFFLINE': return 'The device has no network connection.';
        case 'NOTHING_SHOWN': return 'No artwork is on the screen.';
        case 'CANNOT_BE_PINNED': return 'Your own files are kept already and cannot be pinned.';
        case 'LIST_IS_FULL': return 'That pin list is full.';
        case 'LIST_IS_ACTIVE': return 'The active list cannot be deleted. Make another one active first.';
        case 'TOO_MANY_LISTS': return 'There are too many pin lists. Delete one first.';
        case 'CARD_FULL': return 'The SD card is full.';
        case 'TOO_LARGE': return 'The file is too large.';
        case 'TIMEOUT': return 'The device did not answer.';
        case 'NETWORK': return 'The device could not be reached.';
        default: return error?.message || 'Something went wrong.';
    }
}
