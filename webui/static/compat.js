(function() {
    var REQUIRED_API = 1;

    function banner(msg, isError) {
        var el = document.getElementById('p3a-compat');
        if (el) el.remove();
        el = document.createElement('div');
        el.id = 'p3a-compat';
        el.style.cssText = 'position:fixed;top:0;left:0;right:0;padding:10px;text-align:center;z-index:9999;' +
            (isError ? 'background:#d32f2f;color:#fff' : 'background:#ff9800;color:#000');
        el.innerHTML = msg + ' <button onclick="this.parentElement.remove()" style="margin-left:12px">Dismiss</button>';
        document.body.prepend(el);
    }

    function check() {
        fetch('/status').then(function(r){return r.json()}).then(function(d) {
            if (!d.ok) return;
            var api = d.data.api_version || 1;
            if (api > REQUIRED_API) {
                banner('&#9888; Web UI is outdated. <a href="/ota" style="color:#fff">Update now</a>', true);
            } else if (api < REQUIRED_API) {
                banner('&#9888; Firmware is outdated. <a href="/ota" style="color:#fff">Update now</a>', true);
            } else {
                var el = document.getElementById('p3a-compat');
                if (el) el.remove();
            }
        }).catch(function(){});
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', check);
    } else {
        check();
    }
})();
