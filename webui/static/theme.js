// p3a Web UI — brand theme switcher.
//
// Four brand themes ship today (see docs/brand-identity/). The active one is
// stored under a `data-theme` attribute on <html>; common.css keys every design
// token off that attribute. Selection is persisted in localStorage. (A future
// version may promote this to an NVS-backed device setting; until then it is
// per-browser.)
//
// This file is loaded SYNCHRONOUSLY in each page's <head> so the saved theme is
// applied before first paint — no flash of the default theme. The picker UI is
// a small "Theme" <select> injected just above the bottom navigation on every
// page.
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

    // ── Apply immediately (head-time, pre-paint) ──
    apply(getSaved());

    // ── Inject the picker once the DOM is ready ──
    function injectPicker() {
        if (document.getElementById('p3a-theme-picker')) return;

        var wrap = document.createElement('div');
        wrap.className = 'theme-picker';
        wrap.id = 'p3a-theme-picker';

        var label = document.createElement('label');
        label.setAttribute('for', 'p3a-theme-select');
        label.textContent = 'Theme';

        var sel = document.createElement('select');
        sel.className = 'theme-select';
        sel.id = 'p3a-theme-select';
        var current = getSaved();
        for (var i = 0; i < THEMES.length; i++) {
            var opt = document.createElement('option');
            opt.value = THEMES[i].id;
            opt.textContent = THEMES[i].name;
            if (THEMES[i].id === current) opt.selected = true;
            sel.appendChild(opt);
        }
        sel.addEventListener('change', function () {
            var id = sel.value;
            if (!isValid(id)) return;
            try { localStorage.setItem(KEY, id); } catch (e) {}
            apply(id);
        });

        wrap.appendChild(label);
        wrap.appendChild(sel);

        // Place just above the fixed bottom navigation if present, otherwise at
        // the end of the page body. Inserting before the (position:fixed) nav
        // keeps the picker as the last in-flow block, i.e. visually at the bottom.
        var nav = document.querySelector('.bottom-nav');
        if (nav && nav.parentNode) { nav.parentNode.insertBefore(wrap, nav); }
        else { document.body.appendChild(wrap); }
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', injectPicker);
    } else {
        injectPicker();
    }
})();
