// SPDX-License-Identifier: Apache-2.0
// What every page has: the bottom bar, toasts, and polling that stops while
// nobody is looking. 1.x copied each of these into every page (and the
// playset editor's toast was an empty function).

import { esc } from './lib.js';

const PAGES = [
    { href: '/', icon: '⌂', label: 'Home' },
    { href: '/playsets', icon: '☰', label: 'Playsets' },
    { href: '/settings', icon: '⚙', label: 'Settings' },
    { href: '/update', icon: '⇡', label: 'Update' },
];

// Adds the bottom bar. `active` is the href of the page that calls.
export function nav(active) {
    document.body.classList.add('has-bottom-nav');
    const bar = document.createElement('nav');
    bar.className = 'bottom-nav';
    bar.innerHTML = PAGES.map((p) =>
        `<a href="${p.href}" class="bottom-nav-item${p.href === active ? ' active' : ''}">`
        + `<span class="bottom-nav-icon">${p.icon}</span><span class="bottom-nav-label">${esc(p.label)}</span>`
        + (p.href === '/update' ? '<span class="bottom-nav-badge" id="update-badge"></span>' : '')
        + '</a>').join('');
    document.body.appendChild(bar);
    // A dot on Update when a newer firmware is known. Asked once per page; it costs a few bytes.
    fetch('/api/v2/update').then((r) => r.json()).then((j) => {
        if (j && j.ok && j.data && j.data.available) bar.querySelector('#update-badge')?.classList.add('visible');
    }).catch(() => {});
}

let toastTimer = null;

// One message at a time, at the bottom, for four seconds.
export function toast(message, isError = false) {
    let el = document.getElementById('toast');
    if (!el) {
        el = document.createElement('div');
        el.id = 'toast';
        el.className = 'toast';
        el.setAttribute('role', 'status');
        document.body.appendChild(el);
    }
    el.textContent = message;
    el.className = `toast ${isError ? 'toast-error' : 'toast-success'}`;
    requestAnimationFrame(() => el.classList.add('show'));
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.remove('show'), 4000);
}

// Calls `fn` every `ms` while the page is visible, and at once when it
// becomes visible again. One timer, whatever is asked of it: 1.x's
// "poll soon" stacked a new interval on each call.
//   .soon(delay)  one extra call after `delay` ms, then the rhythm resumes
//   .stop()
export function poller(fn, ms) {
    let timer = null;
    let running = false;
    let stopped = false;
    const run = async () => {
        if (running || stopped) return;
        running = true;
        try {
            await fn();
        } catch (e) {
            // The caller's fn reports its own failures; the rhythm goes on.
        }
        running = false;
        schedule(ms);
    };
    const schedule = (delay) => {
        clearTimeout(timer);
        if (stopped || document.hidden) return;
        timer = setTimeout(run, delay);
    };
    document.addEventListener('visibilitychange', () => {
        if (!document.hidden) schedule(0);
        else clearTimeout(timer);
    });
    schedule(0);
    return {
        soon: (delay = 750) => schedule(delay),
        stop: () => {
            stopped = true;
            clearTimeout(timer);
        },
    };
}

// A dialog over the page. `build(body, close)` fills it. Escape and a click
// outside close it (1.x's dialogs had neither).
export function dialog(build, { className = 'pin-modal', overlayClass = 'pin-modal-overlay' } = {}) {
    const overlay = document.createElement('div');
    overlay.className = `${overlayClass} active`;
    overlay.setAttribute('role', 'dialog');
    overlay.setAttribute('aria-modal', 'true');
    const body = document.createElement('div');
    body.className = className;
    overlay.appendChild(body);
    const close = () => {
        document.removeEventListener('keydown', onKey);
        overlay.remove();
    };
    const onKey = (e) => {
        if (e.key === 'Escape') close();
    };
    overlay.addEventListener('click', (e) => {
        if (e.target === overlay) close();
    });
    document.addEventListener('keydown', onKey);
    document.body.appendChild(overlay);
    build(body, close);
    return close;
}
