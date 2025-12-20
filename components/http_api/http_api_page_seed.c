/**
 * @file http_api_page_seed.c
 * @brief Global seed configuration page
 * 
 * Contains handler for:
 * - GET /seed - Global seed configuration page
 */

#include "http_api_internal.h"

/**
 * GET /seed
 * Returns the global seed configuration page
 */
esp_err_t h_get_seed(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "<title>p3a - Global Seed</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{margin:0;padding:12px 10px 16px;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;flex-direction:column;align-items:center;gap:12px;color:#fff}"
        ".card{width:min(520px,100%);background:rgba(255,255,255,0.12);border-radius:18px;box-shadow:0 4px 12px rgba(0,0,0,0.15);padding:16px}"
        ".row{display:flex;gap:10px;align-items:center;justify-content:space-between;flex-wrap:wrap}"
        "input{width:100%;padding:12px 12px;border-radius:12px;border:none;font-size:1rem}"
        ".btn{background:rgba(255,255,255,0.95);border:none;border-radius:12px;padding:12px 14px;font-size:1rem;color:#667eea;cursor:pointer;box-shadow:0 4px 12px rgba(0,0,0,0.15)}"
        ".btn.secondary{background:rgba(255,255,255,0.25);color:#fff}"
        ".status{display:none;margin-top:10px;padding:10px;border-radius:12px;font-size:0.95rem}"
        ".status.ok{display:block;background:rgba(0,200,0,0.25)}"
        ".status.err{display:block;background:rgba(255,0,0,0.25)}"
        "h1{margin:0 0 8px 0;font-weight:300;letter-spacing:0.08em;text-transform:lowercase}"
        "p{margin:0 0 12px 0;opacity:0.9}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"card\">"
        "  <h1>global seed</h1>"
        "  <p>This seed is persisted and takes effect after reboot.</p>"
        "  <div class=\"row\">"
        "    <div style=\"flex:1;min-width:220px\">"
        "      <label for=\"seed\" style=\"display:block;margin-bottom:6px;opacity:0.9\">Seed (uint32)</label>"
        "      <input id=\"seed\" type=\"number\" min=\"0\" step=\"1\" />"
        "    </div>"
        "    <button class=\"btn\" onclick=\"saveSeed()\">Save</button>"
        "  </div>"
        "  <div class=\"row\" style=\"margin-top:12px\">"
        "    <button class=\"btn secondary\" onclick=\"window.location.href='/'\">Back</button>"
        "    <button class=\"btn\" onclick=\"reboot()\">Reboot</button>"
        "  </div>"
        "  <div id=\"status\" class=\"status\"></div>"
        "</div>"
        "<script>"
        "function setStatus(ok,msg){var s=document.getElementById('status');s.textContent=msg;s.className='status '+(ok?'ok':'err');}"
        "function loadSeed(){var xhr=new XMLHttpRequest();xhr.open('GET','/settings/global_seed',true);xhr.onreadystatechange=function(){"
        " if(xhr.readyState===4){try{var r=JSON.parse(xhr.responseText);if(xhr.status>=200&&xhr.status<300&&r.ok){document.getElementById('seed').value=r.data.global_seed;}"
        " else{setStatus(false,'Failed to load seed');}}catch(e){setStatus(false,'Parse error');}}};xhr.send();}"
        "function saveSeed(){var v=document.getElementById('seed').value;var n=parseInt(v,10);if(isNaN(n)||n<0){setStatus(false,'Invalid seed');return;}"
        " var xhr=new XMLHttpRequest();xhr.open('PUT','/settings/global_seed',true);xhr.setRequestHeader('Content-Type','application/json');"
        " xhr.onreadystatechange=function(){if(xhr.readyState===4){try{var r=JSON.parse(xhr.responseText);if(xhr.status>=200&&xhr.status<300&&r.ok){setStatus(true,'Saved. Reboot to apply.');}"
        " else{setStatus(false,'Save failed');}}catch(e){setStatus(false,'Parse error');}}};"
        " xhr.send(JSON.stringify({global_seed:n}));}"
        "function reboot(){var xhr=new XMLHttpRequest();xhr.open('POST','/action/reboot',true);xhr.setRequestHeader('Content-Type','application/json');xhr.send('{}');setStatus(true,'Reboot queued');}"
        "loadSeed();"
        "</script>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

