// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file surrogate_ui.h
 * @brief Hardcoded fallback HTML for when web UI partition is unavailable
 *
 * This surrogate UI is served when the LittleFS partition is corrupted,
 * missing files, or marked invalid. It provides basic device info and
 * a repair button to trigger web UI OTA recovery.
 */

#pragma once

/**
 * Surrogate UI HTML - served when web UI partition is unhealthy
 *
 * Features:
 * - Clean, minimal design that works without external resources
 * - Shows device info (firmware version, MAC address)
 * - Large "Repair Web UI" button to trigger OTA recovery
 * - Auto-refresh status while repair is in progress
 * - Links to GitHub for manual recovery instructions
 */
static const char SURROGATE_UI_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>p3a - Web UI Recovery</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
"background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"
"min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}\n"
".card{background:#fff;border-radius:16px;padding:32px;max-width:420px;width:100%;"
"box-shadow:0 8px 32px rgba(0,0,0,0.2);text-align:center}\n"
"h1{color:#333;font-size:1.5rem;margin-bottom:8px}\n"
".warning{background:#fff3cd;border:1px solid #ffc107;border-radius:8px;padding:16px;"
"margin:16px 0;color:#856404;font-size:0.9rem}\n"
".note{background:#e3f2fd;border:1px solid #90caf9;border-radius:8px;padding:16px;"
"margin:16px 0;color:#1565c0;font-size:0.85rem;text-align:left}\n"
".note a{color:#1565c0;font-weight:500}\n"
".info{background:#f8f9fa;border-radius:8px;padding:12px;margin:16px 0;"
"font-size:0.85rem;color:#666;text-align:left}\n"
".info-row{display:flex;justify-content:space-between;padding:4px 0}\n"
".info-label{color:#888}\n"
".info-value{color:#333;font-weight:500;font-family:monospace}\n"
".btn{display:block;width:100%;padding:16px;border:none;border-radius:12px;"
"font-size:1rem;font-weight:600;cursor:pointer;margin:8px 0;transition:all 0.2s}\n"
".btn-primary{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"
"color:#fff;box-shadow:0 4px 12px rgba(102,126,234,0.4)}\n"
".btn-primary:hover{transform:translateY(-1px);box-shadow:0 6px 16px rgba(102,126,234,0.5)}\n"
".btn-primary:active{transform:translateY(0)}\n"
".btn-primary:disabled{background:#ccc;color:#888;cursor:not-allowed;box-shadow:none;transform:none}\n"
".status{padding:12px;border-radius:8px;margin:16px 0;font-size:0.9rem}\n"
".status-info{background:#e3f2fd;color:#1565c0}\n"
".status-success{background:#e8f5e9;color:#2e7d32}\n"
".status-error{background:#ffebee;color:#c62828}\n"
".link{color:#667eea;text-decoration:none;font-size:0.85rem}\n"
".link:hover{text-decoration:underline}\n"
".spinner{display:inline-block;width:16px;height:16px;border:2px solid #fff;"
"border-radius:50%;border-top-color:transparent;animation:spin 1s linear infinite;"
"margin-right:8px;vertical-align:middle}\n"
"@keyframes spin{to{transform:rotate(360deg)}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"card\">\n"
"<h1>Web UI Recovery</h1>\n"
"<div class=\"warning\">\n"
"The web interface is unavailable or corrupted.<br>\n"
"Use the button below to repair it.\n"
"</div>\n"
"<div class=\"note\">\n"
"One common reason for this error is when your p3a version requires a full flash. "
"Please try a full flash using the <a href=\"https://fabkury.github.io/p3a/web-flasher/\" target=\"_blank\">web flasher</a>, "
"or see more options at the <a href=\"https://github.com/fabkury/p3a\" target=\"_blank\">GitHub repository</a>.\n"
"</div>\n"
"<div class=\"info\" id=\"device-info\">\n"
"<div class=\"info-row\"><span class=\"info-label\">Firmware</span><span class=\"info-value\" id=\"fw-ver\">Loading...</span></div>\n"
"<div class=\"info-row\"><span class=\"info-label\">Web UI</span><span class=\"info-value\" id=\"webui-ver\">Unavailable</span></div>\n"
"</div>\n"
"<div class=\"status\" id=\"status\" style=\"display:none\"></div>\n"
"<button class=\"btn btn-primary\" id=\"repair-btn\" onclick=\"repair()\">\n"
"Repair Web UI\n"
"</button>\n"
"<p style=\"margin-top:16px\">\n"
"<a class=\"link\" href=\"https://github.com/fabkury/p3a\" target=\"_blank\">\n"
"Manual recovery instructions\n"
"</a>\n"
"</p>\n"
"</div>\n"
"<script>\n"
"var repairing=false,pollId=null;\n"
"function showStatus(msg,type){\n"
"var el=document.getElementById('status');\n"
"el.textContent=msg;el.className='status status-'+type;el.style.display='block';\n"
"}\n"
"function fetchStatus(){\n"
"fetch('/ota/webui/status').then(function(r){return r.json();}).then(function(d){\n"
"if(d.ok&&d.data){\n"
"document.getElementById('webui-ver').textContent=d.data.current_version||'Unavailable';\n"
"if(repairing&&d.data.partition_valid&&!d.data.needs_recovery){\n"
"showStatus('Repair complete! Reloading...','success');\n"
"setTimeout(function(){location.reload();},2000);\n"
"repairing=false;if(pollId){clearInterval(pollId);pollId=null;}\n"
"}\n"
"}}).catch(function(){});\n"
"fetch('/ota/status').then(function(r){return r.json();}).then(function(d){\n"
"if(d.ok&&d.data){document.getElementById('fw-ver').textContent=d.data.current_version||'Unknown';}\n"
"}).catch(function(){});\n"
"}\n"
"function repair(){\n"
"var btn=document.getElementById('repair-btn');\n"
"btn.disabled=true;btn.innerHTML='<span class=\"spinner\"></span>Repairing...';\n"
"showStatus('Starting repair...','info');repairing=true;\n"
"fetch('/ota/webui/repair',{method:'POST'}).then(function(r){return r.json();}).then(function(d){\n"
"if(d.ok){showStatus('Downloading web UI update...','info');pollId=setInterval(fetchStatus,2000);}\n"
"else{showStatus(d.error||'Repair failed','error');btn.disabled=false;btn.textContent='Repair Web UI';repairing=false;}\n"
"}).catch(function(e){showStatus('Network error: '+e,'error');btn.disabled=false;btn.textContent='Repair Web UI';repairing=false;});\n"
"}\n"
"fetchStatus();\n"
"</script>\n"
"</body>\n"
"</html>";
