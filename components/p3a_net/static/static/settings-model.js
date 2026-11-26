// SPDX-License-Identifier: Apache-2.0
// The settings page's logic without its DOM: which control a setting gets,
// what a typed value must be, and where each setting sits on the page.
// Tested with node (tests/web/test_settings_model.mjs).
//
// The device's schema (GET /api/v2/settings/schema) is the source of truth
// for what settings exist, their types and their limits: nothing here
// repeats a range. This file adds only what a schema has no business
// knowing: the words of help, friendlier names for some values, and the
// page's layout.

export const SECRET_MASK = '********';  // how the device shows a key that is set

const HOURS = (h) => [h * 3600, h === 1 ? '1 hour' : `${h} hours`];
const DAYS = (d) => [d * 86400, d === 1 ? '1 day' : `${d} days`];
const RATINGS = [['g', 'G - General Audiences'], ['pg', 'PG - Parental Guidance'], ['pg-13', 'PG-13'], ['r', 'R - Restricted']];

// Per setting: help text, a unit, named options, a slider.
export const PRESENTATION = {
    brightness: { range: true, unit: '%' },
    rotation: { unit: '°' },
    background_color: { help: 'Background color for transparent artwork' },
    intro_animation: {
        dynamic: 'intro-animations', emptyLabel: 'Random (per boot)',
        help: 'Set "Random" to have the device pick a different one each time, or pick one to play it on every boot.',
    },
    intro_duration_ms: { range: true, step: 100, unit: ' ms' },
    indicator_size: {
        unit: 'px',
        help: 'The processing indicator is a small triangle in the bottom-right corner when an artwork swap is requested: '
            + 'blue while working, red if it fails.',
    },
    playback_max_speed: { help: "Ignores each frame's intended duration and plays as fast as the device can." },
    dwell_s: { unit: 'sec (0 = never)' },
    pick_mode: { options: [['random', 'Random'], ['recency', 'In the channel’s own order']] },
    channel_select: {
        options: [['swrr', 'SWRR'], ['stochastic', 'Stochastic']],
        help: 'How the next channel is chosen when a playset has more than one. SWRR (Smooth Weighted Round Robin) '
            + 'rotates channels in a fixed order that exactly matches their weights. Stochastic picks randomly with '
            + 'the same weights but nudges any channel that has fallen behind.',
    },
    aspect_max_tenths: { help: '40 is four times as wide as tall (or as tall as wide).' },
    timezone: {
        zones: true,
        help: 'Pick a region, or type a POSIX time zone for any other.',
    },
    device_name: {
        help: 'Device names are useful if you have more than one p3a. Lowercase letters, numbers and hyphens only. '
            + 'Leave empty to use the default.',
    },
    sd_root: {
        placeholder: '/p3a',
        help: 'Folder on the SD card. Changing it does not move or delete any files.',
    },
    makapix_refresh_s: { options: [HOURS(2), HOURS(4), HOURS(8), HOURS(24)] },
    makapix_cache_size: { unit: 'artworks', help: 'Takes effect at the next channel refresh.' },
    giphy_api_key: {
        help: 'Giphy channels do not work without a key. You can create one for free at developers.giphy.com. '
            + 'To remove a stored key, clear the field and save. Free keys are limited to 100 requests per hour; '
            + 'each request returns up to 50 artworks.',
    },
    giphy_rating: { options: RATINGS },
    giphy_country: {
        placeholder: 'e.g. US',
        help: 'Optional 2-letter country code (ISO 3166-1) to influence regional trending content. Leave empty for global results.',
    },
    giphy_refresh_s: { options: [HOURS(1), HOURS(2), HOURS(4), HOURS(8)] },
    giphy_cache_size: { unit: 'GIFs', help: 'Free Giphy keys are limited to 500 items per query.' },
    klipy_api_key: {
        help: 'Klipy channels do not work without a key. Get a free key at partner.klipy.com. To remove a stored key, '
            + 'clear the field and save. Test keys are limited to 100 requests per hour.',
    },
    klipy_rating: { options: RATINGS },
    klipy_format: {
        options: [['gif', 'GIF'], ['webp', 'WebP (smaller)']],
        help: 'Klipy serves both GIF and WebP; choose WebP for smaller downloads.',
    },
    klipy_refresh_s: { options: [HOURS(1), HOURS(2), HOURS(4), HOURS(8)] },
    klipy_cache_size: { unit: 'items', help: 'A full refresh of N items costs about N/50 requests.' },
    museum_refresh_s: { options: [DAYS(1), DAYS(2), DAYS(4), DAYS(8)] },
    museum_cache_size: { options: [32, 64, 128, 256, 512, 1024, 2048, 4096].map((n) => [n, String(n)]) },
    harvard_api_key: {
        help: 'Harvard Art Museums channels need a key. Request a free one at harvardartmuseums.org/collections/api. '
            + 'It allows 2,500 requests per day.',
    },
    smithsonian_api_key: {
        help: 'Smithsonian channels need an api.data.gov key. Get a free one at api.data.gov/signup. Do not use '
            + 'DEMO_KEY: it is capped at about 30 requests per hour.',
    },
};

// The page: tabs, their sections, and each section's settings or widget.
// A setting the schema has and this table does not is added to the tab of
// its group, under "More", so that a new setting shows without a change here.
export const TABS = [
    { id: 'display', label: 'Display', groups: ['display', 'clock', 'playback'], sections: [
        { title: 'Theme', widget: 'theme' },
        { title: 'Brightness', keys: ['brightness'] },
        { title: 'Screen Rotation (Clockwise)', keys: ['rotation'] },
        { title: 'Background Color', keys: ['background_color'] },
        { title: 'Boot Logo Animation', keys: ['intro_animation', 'intro_duration_ms'] },
        { title: 'Clock', keys: ['clock_enabled', 'clock_24h', 'timezone', 'clock_position', 'clock_size', 'clock_color',
            'clock_outline_color', 'clock_outline'] },
        { title: 'Playback', keys: ['dwell_s', 'pick_mode', 'channel_select', 'playback_max_speed', 'aspect_filter',
            'aspect_max_tenths'] },
        { title: 'Advanced', keys: ['source_credit', 'indicator_size', 'show_fps'] },
    ] },
    { id: 'network', label: 'Network', groups: ['network'], sections: [
        { title: 'Connection', widget: 'connection' },
        { title: 'Device Name', keys: ['device_name'] },
        { widget: 'erase-wifi' },
    ] },
    { id: 'giphy', label: 'Giphy', groups: ['giphy'], sections: [
        { title: 'API', keys: ['giphy_api_key'] },
        { title: 'Analytics ID', widget: 'giphy-id' },
        { title: 'Content', keys: ['giphy_rating', 'giphy_country'] },
        { title: 'Advanced', keys: ['giphy_refresh_s', 'giphy_cache_size'] },
    ] },
    { id: 'klipy', label: 'Klipy', groups: ['klipy'], sections: [
        { title: 'API', keys: ['klipy_api_key'] },
        { title: 'Content', keys: ['klipy_rating', 'klipy_format'] },
        { title: 'Advanced', keys: ['klipy_refresh_s', 'klipy_cache_size'] },
    ] },
    { id: 'museum', label: 'Museum', groups: ['museum'], sections: [
        { title: 'Museums', widget: 'museums', keys: ['museum_refresh_s', 'museum_cache_size'] },
        { title: 'Harvard Art Museums (API key)', keys: ['harvard_api_key'] },
        { title: 'Smithsonian (API key)', keys: ['smithsonian_api_key'] },
    ] },
    { id: 'storage', label: 'Storage', groups: ['storage'], sections: [
        { title: 'SD Card Storage', widget: 'storage', keys: ['sd_root'] },
    ] },
    { id: 'makapix', label: 'Makapix', groups: ['makapix'], sections: [
        { title: 'Makapix Club', widget: 'makapix' },
        { title: 'Advanced', keys: ['makapix_refresh_s', 'makapix_cache_size'] },
    ] },
    { id: 'system', label: 'System', groups: [], sections: [
        { title: 'Firmware', widget: 'firmware' },
        { title: 'Actions', widget: 'actions' },
        { title: 'About p3a', widget: 'about' },
        { title: 'About this device', widget: 'device' },
    ] },
];

// The tabs with every visible setting of the schema placed: those the
// table names where it names them, the rest under "More" in the tab of
// their group. A key the table names and the schema lacks is dropped.
export function layout(schema) {
    const known = new Map((schema?.settings || []).filter((s) => !s.advanced).map((s) => [s.key, s]));
    const placed = new Set();
    const tabs = TABS.map((tab) => ({
        ...tab,
        sections: tab.sections.map((section) => {
            const keys = (section.keys || []).filter((k) => known.has(k));
            keys.forEach((k) => placed.add(k));
            return { ...section, keys };
        }),
    }));
    for (const tab of tabs) {
        const more = [...known.values()].filter((s) => !placed.has(s.key) && tab.groups.includes(s.group)).map((s) => s.key);
        if (more.length) {
            more.forEach((k) => placed.add(k));
            tab.sections.push({ title: 'More', keys: more });
        }
    }
    // A group no tab claims: nothing is hidden for want of a place.
    const lost = [...known.values()].filter((s) => !placed.has(s.key)).map((s) => s.key);
    if (lost.length) tabs[tabs.length - 1].sections.push({ title: 'Other settings', keys: lost });
    return tabs;
}

// Which control draws a setting.
//   toggle | select | choices | range | number | color | secret | zone | text
// `options` (for select and choices) are [value, label] pairs; `instant`
// controls save when changed, the others have a Save button.
export function controlFor(entry, extra = {}) {
    const look = PRESENTATION[entry.key] || {};
    const base = { key: entry.key, label: entry.label, help: look.help || '', unit: look.unit || '', reboot: !!entry.reboot };
    switch (entry.type) {
        case 'bool':
            return { ...base, kind: 'toggle', instant: true };
        case 'enum': {
            const named = new Map(look.options || []);
            return { ...base, kind: 'select', instant: true, options: entry.values.map((v) => [v, named.get(v) || v]) };
        }
        case 'color':
            return { ...base, kind: 'color', instant: true };
        case 'int':
            if (entry.choices) {
                return { ...base, kind: 'choices', instant: true, options: entry.choices.map((v) => [v, `${v}${look.unit || ''}`]) };
            }
            if (look.options) {
                // Only what the device would take: the schema's range decides.
                const options = look.options.filter(([v]) => v >= entry.min && v <= entry.max);
                return { ...base, kind: 'select', instant: true, numeric: true, options };
            }
            if (look.range) {
                return { ...base, kind: 'range', min: entry.min, max: entry.max, step: look.step || 1 };
            }
            return { ...base, kind: 'number', min: entry.min, max: entry.max };
        case 'string':
            if (entry.secret) return { ...base, kind: 'secret', maxLength: entry.max_len };
            if (look.dynamic) {
                const names = extra[look.dynamic] || [];
                return { ...base, kind: 'select', instant: true, options: [['', look.emptyLabel || '(none)'], ...names.map((n) => [n, n])] };
            }
            if (look.zones) return { ...base, kind: 'zone', maxLength: entry.max_len };
            return { ...base, kind: 'text', maxLength: entry.max_len, placeholder: look.placeholder || '' };
        default:
            return { ...base, kind: 'text', maxLength: entry.max_len || 64 };
    }
}

// What the control of a select shows for a stored value that is not among
// its options (a refresh interval set through the API): the value itself.
export function withCurrent(options, value) {
    return options.some(([v]) => v === value) ? options : [...options, [value, String(value)]];
}

// Checks what a person typed against the schema entry. Returns
// {value} (what to send) or {error} (what to say). The device checks again:
// this is so that the message comes at once and in the page's words.
export function validate(entry, raw) {
    switch (entry.type) {
        case 'bool':
            return { value: !!raw };
        case 'int': {
            const text = String(raw).trim();
            if (!/^-?\d+$/.test(text)) return { error: `${entry.label}: enter a whole number` };
            const n = Number(text);
            if (entry.choices) {
                return entry.choices.includes(n) ? { value: n } : { error: `${entry.label}: must be one of ${entry.choices.join(', ')}` };
            }
            if (n < entry.min || n > entry.max) return { error: `${entry.label}: must be ${entry.min} to ${entry.max}` };
            return { value: n };
        }
        case 'enum':
            return entry.values.includes(raw) ? { value: raw } : { error: `${entry.label}: not one of the choices` };
        case 'color':
            return /^#[0-9a-fA-F]{6}$/.test(raw) ? { value: raw.toLowerCase() } : { error: `${entry.label}: not a color` };
        case 'string': {
            const text = entry.secret ? String(raw).trim() : String(raw);
            if ([...text].length > entry.max_len) return { error: `${entry.label}: at most ${entry.max_len} characters` };
            if (entry.check === 'device_name' && text !== '' && !/^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.test(text)) {
                return { error: 'Use only lowercase letters, numbers and hyphens, with no hyphen first or last.' };
            }
            if (entry.check === 'country' && text !== '' && !/^[A-Za-z]{2}$/.test(text)) {
                return { error: 'Country code must be 2 letters' };
            }
            if (entry.check === 'sd_root' && text !== '') {
                if (!text.startsWith('/')) return { error: 'Path must start with /' };
                if (text === '/') return { error: 'Path cannot be just / - name a folder' };
                if (text.includes('..')) return { error: 'Path cannot contain ..' };
            }
            return { value: entry.check === 'country' ? text.toUpperCase() : text };
        }
        default:
            return { value: raw };
    }
}

// A key as the page shows it: whether one is stored, never what it is.
export function secretState(value) {
    return value === SECRET_MASK ? 'set' : value ? 'set' : 'none';
}

// Time zones a person can pick by name. The device takes a POSIX TZ string
// (it has no zone database); this is the list of the common ones. Anything
// else can be typed.
export const ZONES = [
    ['UTC0', 'UTC'],
    ['HST10', 'Honolulu'],
    ['AKST9AKDT,M3.2.0,M11.1.0', 'Anchorage'],
    ['PST8PDT,M3.2.0,M11.1.0', 'Los Angeles, Vancouver'],
    ['MST7MDT,M3.2.0,M11.1.0', 'Denver, Edmonton'],
    ['MST7', 'Phoenix'],
    ['CST6CDT,M3.2.0,M11.1.0', 'Chicago, Winnipeg'],
    ['CST6', 'Mexico City'],
    ['EST5EDT,M3.2.0,M11.1.0', 'New York, Toronto'],
    ['<-05>5', 'Bogotá, Lima'],
    ['AST4ADT,M3.2.0,M11.1.0', 'Halifax'],
    ['<-04>4<-03>,M9.1.6/24,M4.1.6/24', 'Santiago'],
    ['<-03>3', 'São Paulo, Buenos Aires'],
    ['GMT0BST,M3.5.0/1,M10.5.0', 'London, Dublin, Lisbon'],
    ['CET-1CEST,M3.5.0,M10.5.0/3', 'Paris, Berlin, Madrid, Rome, Amsterdam'],
    ['EET-2EEST,M3.5.0/3,M10.5.0/4', 'Athens, Helsinki, Kyiv'],
    ['SAST-2', 'Johannesburg'],
    ['EET-2EEST,M4.5.5/0,M10.5.4/24', 'Cairo'],
    ['<+03>-3', 'Istanbul, Riyadh, Nairobi'],
    ['MSK-3', 'Moscow'],
    ['<+04>-4', 'Dubai'],
    ['PKT-5', 'Karachi'],
    ['IST-5:30', 'India'],
    ['<+06>-6', 'Dhaka'],
    ['<+07>-7', 'Bangkok, Jakarta, Hanoi'],
    ['CST-8', 'Beijing, Hong Kong, Taipei'],
    ['<+08>-8', 'Singapore, Manila, Kuala Lumpur'],
    ['AWST-8', 'Perth'],
    ['JST-9', 'Tokyo'],
    ['KST-9', 'Seoul'],
    ['ACST-9:30ACDT,M10.1.0,M4.1.0/3', 'Adelaide'],
    ['AEST-10', 'Brisbane'],
    ['AEST-10AEDT,M10.1.0,M4.1.0/3', 'Sydney, Melbourne'],
    ['NZST-12NZDT,M9.5.0,M4.1.0/3', 'Auckland'],
];
