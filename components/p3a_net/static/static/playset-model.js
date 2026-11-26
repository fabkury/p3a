// SPDX-License-Identifier: Apache-2.0
// The playset editor's logic without its DOM: what channels can be made,
// what a playset must be to be saved, and how the balance bar is drawn.
// Tested with node (tests/web/test_playset_model.mjs).
//
// A playset, as the device takes it (playset.h):
//   {"name": "...", "channels": [{"source", "kind", "identifier", "display_name", "weight", "offset",
//                                  "cache_size", "refresh_s"}]}
//
// `cache_size` and `refresh_s` are a channel's own, in this playset: how
// many artworks it holds and how often it is listed again. Zero is "what
// the source's settings say". What a source allows is the range of its own
// two settings, read from the device's schema (tuningOf): nothing here
// states one.

import { channelLabel } from './lib.js';
import { PRESENTATION } from './settings-model.js';

export const MAX_CHANNELS = 64;
export const NAME_MAX = 32;
export const DISPLAY_NAME_MAX = 63;
export const IDENTIFIER_MAX = 95;   // bytes
export const MAX_WEIGHT = 1000000;
export const DEFAULT_WEIGHT = 100;

// Klipy's categories are fixed: a category is a search for its name.
export const KLIPY_CATEGORIES = [
    'hello', 'lol', 'love', 'happy birthday', 'thank you', 'excited', 'yes', 'no', 'sorry', 'happy', 'sad', 'thumbs up',
    'scared', 'bye', 'confused', 'angry', 'hungry', 'kiss', 'please', 'surprised', 'nervous', 'embarrassed', 'ok',
    'congratulations', 'hugs', 'good luck', 'dance', 'clapping', 'bored', 'wink', 'smile', 'aww', 'high five',
    'good morning', 'youre welcome', 'good night',
];

// What the Add Channel dialog offers. `fields` says what each choice asks for:
//   'sqid'      a Makapix user's short id
//   'hashtag'   a tag without '#'
//   'query'     words to search for
//   'category'  one of KLIPY_CATEGORIES
//   'list'      one of the device's pin lists (empty: the active one)
export const CHOICES = [
    { id: 'makapix', label: 'Makapix Club', kinds: [
        { kind: 'all', label: 'All Artworks', fields: [] },
        { kind: 'promoted', label: 'Promoted Artworks', fields: [] },
        { kind: 'user', label: 'My Artworks', fields: [] },
        { kind: 'by_user', label: 'Artist', fields: ['sqid'] },
        { kind: 'reactions', label: 'Artist Reactions', fields: ['sqid'] },
        { kind: 'hashtag', label: 'Hashtag', fields: ['hashtag'] },
    ] },
    { id: 'giphy', label: 'Giphy', kinds: [
        { kind: 'trending', label: 'Trending', fields: [] },
        { kind: 'search', label: 'Search', fields: ['query'] },
    ] },
    { id: 'klipy', label: 'Klipy', kinds: [
        { kind: 'gif:trending', label: 'GIFs: Trending', fields: [] },
        { kind: 'gif:search', label: 'GIFs: Search', fields: ['query'] },
        { kind: 'gif:category', label: 'GIFs: Category', fields: ['category'] },
        { kind: 'sticker:trending', label: 'Stickers: Trending', fields: [] },
        { kind: 'sticker:search', label: 'Stickers: Search', fields: ['query'] },
        { kind: 'sticker:category', label: 'Stickers: Category', fields: ['category'] },
    ] },
    { id: 'museum', label: 'Museum', browse: true, kinds: [] },
    { id: 'pinned', label: 'Pin List', kinds: [{ kind: '', label: 'Pin list', fields: ['list'] }] },
    { id: 'local', label: 'Local SD Card', once: true, kinds: [{ kind: '', label: 'SD Card', fields: [] }] },
];

// Sources whose channels start at `offset` in their listing. Makapix's do
// not, and the SD card's does not either: its listing is newest first, and
// an offset would hide the files just copied or uploaded.
export function hasOffset(source) {
    return source === 'giphy' || source === 'klipy' || source === 'museum' || source === 'pinned';
}

const bytes = (text) => new TextEncoder().encode(String(text ?? '')).length;

// Why a channel's identifier will not do, or '' when it will. The rules
// are the device's (each source's own check), said in the page's words.
export function identifierProblem(source, kind, identifier) {
    const id = String(identifier ?? '');
    if (bytes(id) > IDENTIFIER_MAX) return 'That is too long.';
    if (source === 'makapix') {
        if (kind === 'by_user' || kind === 'reactions') {
            if (!id) return 'Please enter an artist ID';
            if (!/^[A-Za-z0-9_-]{1,64}$/.test(id)) return 'An artist ID is letters and digits only.';
        } else if (kind === 'hashtag') {
            if (!id) return 'Please enter a hashtag';
            if (!/^[A-Za-z0-9_-]{1,64}$/.test(id)) return 'A hashtag is letters, digits, "_" and "-", without the #.';
        }
    } else if (source === 'giphy' && kind === 'search') {
        if (!id.trim()) return 'Please enter a search query';
    } else if (source === 'klipy') {
        const mode = String(kind).split(':')[1];
        if ((mode === 'search' || mode === 'category') && !id.trim()) return 'Please enter a search query';
    } else if (source === 'museum') {
        if (!id) return 'Use the museum browser to pick a term first';
    }
    return '';
}

// A channel as the device takes it. `fields`: {identifier, display_name}.
export function makeChannel(source, kind, fields = {}) {
    return {
        source,
        kind: kind || '',
        identifier: String(fields.identifier ?? '').trim(),
        display_name: String(fields.display_name ?? '').trim().slice(0, DISPLAY_NAME_MAX),
        weight: DEFAULT_WEIGHT,
        offset: 0,
        cache_size: 0,
        refresh_s: 0,
    };
}

// --- a channel's own size and refresh interval ----------------------------------------

// What a channel of this source may set for itself, from the device's
// schema (GET /api/v2/settings/schema) and its settings as they are now:
//   {size: {min, max, standard}, refresh: {min, max, standard, options}}
// `standard` is what the channel gets when it sets nothing; `options` are
// the intervals the settings page offers for this source. null for a source
// with no such settings (the card, a pin list).
export function tuningOf(source, schema, settings) {
    const entry = (key) => (schema?.settings || []).find((s) => s.key === key);
    const size = entry(`${source}_cache_size`);
    const refresh = entry(`${source}_refresh_s`);
    if (!size || !refresh) return null;
    const now = (e) => (Number.isInteger(settings?.[e.key]) ? settings[e.key] : e.default);
    const offered = (PRESENTATION[refresh.key]?.options || []).filter(([v]) => v >= refresh.min && v <= refresh.max);
    return {
        size: { min: size.min, max: size.max, standard: now(size) },
        refresh: { min: refresh.min, max: refresh.max, standard: now(refresh), options: offered },
    };
}

// "2 hours", "1 day", "90 minutes": an interval in words.
export function formatDuration(seconds) {
    const s = Number(seconds) || 0;
    const say = (n, unit) => `${n} ${unit}${n === 1 ? '' : 's'}`;
    if (s >= 86400 && s % 86400 === 0) return say(s / 86400, 'day');
    if (s >= 3600 && s % 3600 === 0) return say(s / 3600, 'hour');
    if (s >= 60 && s % 60 === 0) return say(s / 60, 'minute');
    return say(s, 'second');
}

// The refresh control's choices, [seconds, label] pairs: the source's
// setting first (0), then the intervals the settings page offers, and the
// channel's own value when it is none of those (set through the API).
export function refreshChoices(tuning, value) {
    const choices = [[0, `Default (${formatDuration(tuning.refresh.standard)})`],
        ...tuning.refresh.options.map(([v]) => [v, formatDuration(v)])];
    if (value && !choices.some(([v]) => v === value)) choices.push([value, formatDuration(value)]);
    return choices;
}

// What a person typed for a size: '' is 0 (the source's setting), a whole
// number is itself, anything else is NaN (and stops the save).
export function sizeFrom(text) {
    const t = String(text ?? '').trim();
    if (t === '') return 0;
    return /^\d+$/.test(t) ? Number(t) : Number.NaN;
}

// Why a channel's own size or interval will not do, or ''. `tuning` is
// tuningOf() for its source; with none, the channel must set neither.
export function tuningProblem(ch, tuning) {
    const size = ch.cache_size ?? 0;
    const refresh = ch.refresh_s ?? 0;
    if (!tuning) return size || refresh ? 'this kind of channel has no size or refresh of its own' : '';
    if (size !== 0 && !(Number.isInteger(size) && size >= tuning.size.min && size <= tuning.size.max)) {
        return `keeps ${tuning.size.min} to ${tuning.size.max} artworks`;
    }
    if (refresh !== 0 && !(Number.isInteger(refresh) && refresh >= tuning.refresh.min && refresh <= tuning.refresh.max)) {
        return `refreshes every ${formatDuration(tuning.refresh.min)} to ${formatDuration(tuning.refresh.max)}`;
    }
    return '';
}

// What a channel's row says of what it set for itself: "keeps 100 · every 2 hours", or ''.
export function tuningNote(ch) {
    const parts = [];
    if (ch.cache_size) parts.push(`keeps ${ch.cache_size}`);
    if (ch.refresh_s) parts.push(`every ${formatDuration(ch.refresh_s)}`);
    return parts.join(' · ');
}

// Why a channel cannot be added to these, or ''.
export function addProblem(channels, channel) {
    if (channels.length >= MAX_CHANNELS) return `Maximum ${MAX_CHANNELS} channels allowed`;
    if (channel.source === 'local' && channels.some((c) => c.source === 'local')) return 'SD Card channel already added';
    return identifierProblem(channel.source, channel.kind, channel.identifier);
}

// Why a name will not do, or ''. `taken`: the names of the saved playsets;
// `editing`: the name of the one being edited (it may keep its own).
export function nameProblem(name) {
    const text = String(name ?? '');
    if (!text.trim()) return 'Name is required';
    if ([...text].length > NAME_MAX) return `Name is at most ${NAME_MAX} characters`;
    // The device refuses a slash and control characters: the name is a file's.
    if (/[\u0000-\u001f\u007f/\\]/.test(text)) return 'Name must not contain a slash or control characters';
    if (text !== text.trim()) return 'Name must not start or end with a space';
    return '';
}

// Everything that stops a save, first problem first; '' when it can go.
// `tuningFor(source)` gives tuningOf() for a source; without it (the schema
// could not be read) sizes and intervals are left to the device to check.
export function saveProblem(name, channels, tuningFor = null) {
    const named = nameProblem(name);
    if (named) return named;
    if (channels.length === 0) return 'Add at least one channel';
    if (channels.length > MAX_CHANNELS) return `Maximum ${MAX_CHANNELS} channels allowed`;
    for (const ch of channels) {
        const problem = identifierProblem(ch.source, ch.kind, ch.identifier);
        if (problem) return `${channelLabel(ch)}: ${problem}`;
        if (!Number.isInteger(ch.weight) || ch.weight < 0 || ch.weight > MAX_WEIGHT) {
            return `${channelLabel(ch)}: weight is 0 to ${MAX_WEIGHT}`;
        }
        if (!Number.isInteger(ch.offset) || ch.offset < 0 || ch.offset > 4294967295) {
            return `${channelLabel(ch)}: offset is not a number`;
        }
        if (tuningFor) {
            const tuned = tuningProblem(ch, tuningFor(ch.source));
            if (tuned) return `${channelLabel(ch)}: ${tuned}`;
        }
    }
    return '';
}

// A number field's text as a whole number within [0, max]; what is not
// one is 0 (an emptied field mutes the channel, as 1.x did).
export function wholeNumber(text, max) {
    const n = Number.parseInt(String(text), 10);
    return Number.isFinite(n) && n > 0 ? Math.min(n, max) : 0;
}

// A channel's colour in the balance bar and on its row. Hues a golden angle
// apart: 64 channels in a row and no two neighbours look alike. (1.x and
// 2.0's first editor cycled six colours.)
export function colourOf(index) {
    return `hsl(${Math.round((258 + index * 137.508) % 360)} 68% 62%)`;
}

// The balance bar: each channel's share of the turns. If every weight is
// zero all play equally; otherwise a channel of weight zero has no share.
// Segments: {label, percent, colour, index, muted}. `muted`: this channel
// never plays as things are.
export function balance(channels) {
    const total = channels.reduce((n, ch) => n + (ch.weight || 0), 0);
    return channels.map((ch, index) => {
        const share = total === 0 ? 1 / channels.length : (ch.weight || 0) / total;
        return {
            index,
            label: channelLabel(ch),
            percent: Math.round(share * 1000) / 10,
            colour: colourOf(index),
            muted: share === 0,
        };
    });
}

// A share in a row's words: "40%", "<1%", "muted".
export function shareLabel(segment) {
    if (segment.muted) return 'muted';
    return segment.percent < 1 ? '<1%' : `${Math.round(segment.percent)}%`;
}

// Moves the channel at `from` to `to`; a new array.
export function move(channels, from, to) {
    if (from === to || from < 0 || to < 0 || from >= channels.length || to >= channels.length) return channels.slice();
    const out = channels.slice();
    const [taken] = out.splice(from, 1);
    out.splice(to, 0, taken);
    return out;
}

// What to send: only what the device's reader takes, numbers as numbers.
export function toDevice(name, channels) {
    return {
        name,
        channels: channels.map((ch) => ({
            source: ch.source,
            kind: ch.kind || '',
            identifier: ch.identifier || '',
            display_name: ch.display_name || '',
            weight: ch.weight || 0,
            offset: hasOffset(ch.source) ? ch.offset || 0 : 0,
            cache_size: ch.cache_size || 0,
            refresh_s: ch.refresh_s || 0,
        })),
    };
}

// The type badge on a channel's card.
export function typeLabel(ch) {
    const choice = CHOICES.find((c) => c.id === ch.source);
    const kind = choice?.kinds.find((k) => k.kind === (ch.kind || ''));
    if (ch.source === 'makapix' || ch.source === 'giphy' || ch.source === 'klipy') {
        return kind ? `${choice.label} · ${kind.label}` : choice?.label || ch.source;
    }
    return choice?.label || ch.source;
}
