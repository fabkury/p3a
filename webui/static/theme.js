// p3a Web UI — brand theme switcher.
//
// Four brand themes ship today (see docs/brand-identity/). The active one is
// stored under a `data-theme` attribute on <html>; common.css keys every design
// token off that attribute. Selection is persisted in localStorage. (A future
// version may promote this to an NVS-backed device setting; until then it is
// per-browser.)
//
// This file is loaded SYNCHRONOUSLY in each page's <head> so the saved theme is
// applied before first paint — no flash of the default theme. It renders no UI
// of its own; it exposes `window.p3aTheme` so the Settings → Display tab can
// list the themes and change the active one.
(function () {
    var KEY = 'p3a-theme';
    var DEFAULT = 'spectrum';
    var THEMES = [
        { id: 'spectrum', name: 'Spectrum' },
        { id: 'gallery',  name: 'Gallery' },
        { id: 'console',  name: 'Pixel Console' },
        { id: 'dither',   name: 'Dither Pop' },
        { id: 'blossom',  name: 'Blossom' }
    ];
    // Browser chrome (address bar) colour per theme — keep meta theme-color in sync.
    var THEME_COLOR = {
        spectrum: '#0E0A1A',
        gallery:  '#F4F1EA',
        console:  '#07080A',
        dither:   '#F3EEE3',
        blossom:  '#FBF1F3'
    };

    function isValid(id) {
        for (var i = 0; i < THEMES.length; i++) { if (THEMES[i].id === id) return true; }
        return false;
    }
    function getSaved() {
        var t;
        try { t = localStorage.getItem(KEY); } catch (e) { t = null; }
        return isValid(t) ? t : DEFAULT;
    }
    function apply(id) {
        document.documentElement.setAttribute('data-theme', id);
        var meta = document.querySelector('meta[name="theme-color"]');
        if (meta) meta.setAttribute('content', THEME_COLOR[id] || THEME_COLOR[DEFAULT]);
    }

    function setTheme(id) {
        if (!isValid(id)) return false;
        try { localStorage.setItem(KEY, id); } catch (e) {}
        apply(id);
        return true;
    }

    // ── Apply immediately (head-time, pre-paint) ──
    apply(getSaved());

    // Public API. The theme is changed from exactly one place — the Display tab
    // of the Settings page — which reads THEMES and calls set(). (NVS-ready: a
    // future revision can hydrate the initial value from /config and POST back.)
    window.p3aTheme = {
        THEMES: THEMES,
        get: getSaved,
        set: setTheme
    };
})();
