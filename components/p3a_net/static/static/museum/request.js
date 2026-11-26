// SPDX-License-Identifier: Apache-2.0
// What every museum adapter needs and none should write for itself: what
// to do when a museum says "slow down", how many requests go out at once,
// the keys the device keeps, and how long a term may be.
//
// The browser asks the museums directly, from the person's own address.
// The device asks them too, on its own schedule, and keeps its own
// account of who told it to wait: the two do not share one (1.x had the
// page report its 429s to the device).

// A channel's identifier, in bytes (playset.h, PLAYSET_IDENT_LEN - 1).
// A term longer than this cannot be made into a channel, so it is not offered.
export const IDENTIFIER_MAX_BYTES = 95;

const encoder = new TextEncoder();

export function fitsIdentifier(text) {
    return encoder.encode(String(text ?? '')).length <= IDENTIFIER_MAX_BYTES;
}

// --- "slow down" ------------------------------------------------------------------

const waitUntil = new Map();   // museum id -> time (ms) before which it should not be asked

// How long a 429 says to wait, in seconds: its Retry-After if it is a
// sensible number, else a minute. Never more than an hour.
export function retryAfterSeconds(headerValue) {
    const n = Number.parseInt(String(headerValue ?? ''), 10);
    return Number.isFinite(n) && n > 0 ? Math.min(n, 3600) : 60;
}

// An adapter calls this with the 429 it got.
export function noteRateLimit(museum, response, now = Date.now()) {
    const seconds = retryAfterSeconds(response?.headers?.get?.('Retry-After'));
    const until = now + seconds * 1000;
    if (until > (waitUntil.get(museum) || 0)) waitUntil.set(museum, until);
    return seconds;
}

// Seconds this museum still asks to be left alone; 0 when it may be asked.
export function cooldownSeconds(museum, now = Date.now()) {
    const until = waitUntil.get(museum) || 0;
    return until > now ? Math.ceil((until - now) / 1000) : 0;
}

// --- many requests, a few at a time -------------------------------------------------

// Like Promise.all(items.map(fn)), with at most `limit` running. A museum
// that is asked 193 things in one instant answers some of them with 429.
export async function mapLimit(items, limit, fn) {
    const out = new Array(items.length);
    let next = 0;
    const worker = async () => {
        while (next < items.length) {
            const i = next++;
            out[i] = await fn(items[i], i);
        }
    };
    await Promise.all(Array.from({ length: Math.max(1, Math.min(limit, items.length)) }, worker));
    return out;
}

// --- the keys the device keeps ------------------------------------------------------

let keys = null;

// The device's key for a museum that needs one ('harvard_api_key',
// 'smithsonian_api_key'), or ''. These two are the only settings the
// device gives out unmasked, because the page must send them (D33).
export async function museumKey(name) {
    if (!keys) {
        keys = fetch('/api/v2/secrets/museum-keys')
            .then((r) => (r.ok ? r.json() : null))
            .then((j) => (j && j.ok && j.data) || {})
            .catch(() => ({}));
    }
    const value = (await keys)[name];
    return typeof value === 'string' ? value.trim() : '';
}

// Which museums this firmware plays, and why one cannot be: the device's
// own table (GET /api/v2/sources/museums). An empty list if it cannot be asked.
export async function deviceMuseums() {
    try {
        const r = await fetch('/api/v2/sources/museums');
        const j = r.ok ? await r.json() : null;
        return (j && j.ok && j.data && j.data.museums) || [];
    } catch (e) {
        return [];
    }
}
