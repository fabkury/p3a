# Web UI and HTTP API

Part of the [2.0 architecture review](README.md). Covers the HTTP server,
the REST surface, the web UI's structure, and everything that exists because
the UI lives in its own partition with its own version (the 2.0 change A).
Constraints for 2.0: vanilla HTML/JS, no build step, served from the device;
PICO-8 removed.

Line numbers refer to `main` at `1692d43a`.

## 1. Routing: six registrations and a `strcmp` chain

Only six `httpd_uri_t` are registered (`/status`, `/static/*`, `/museum/*`,
`/pico8/*`, `/pico_stream`, `/upload`). Everything else goes to four
catch-all method routers (`components/http_api/http_api.c:866-906`) that
dispatch by `strcmp`/`strncmp` (`http_api.c:508-752`), with two sub-routers
for pages and OTA and a third level for pin lists. `max_uri_handlers = 13`
(`:844`) is cosmetic.

The surface has grown in rings. Settings live under `/config`, `/settings/*`,
`/rotation` and `/api/device-name`; the UI has already voted by using
`/config` for nearly everything (9 references) and `/settings/refresh_override`
once. Playback selection has five entry points: `POST /channel`,
`POST /playset/{name}`, `POST /playsets/{name}?activate`,
`GET /playsets/{name}?activate=true` (a GET with a side effect) and
`POST /api/pin-lists/{slug}/play`. `/api/` is a half-adopted namespace:
`pin-lists` and `museum` are under it, older resources are at the root.
`POST /rotation` toggles rather than sets (`README.md:146`).

Nine registered endpoints are not called by the web UI: `/api/ui-config`,
`/api/network-status`, `/api/state`, `/api/storage`, `/channels/stats`,
`GET /channel` (marked deprecated at `http_api_rest_actions.c:104`),
`POST /channel`, `/settings/dwell_time`, `/ota/webui/status` (a "legacy
alias", `http_api_ota.c:286`, used only by the surrogate page). Two exist for
`host/jitter-lab` scripts only.

**Fresh design.** A static route table `{method, path, handler, flags}` with
one matcher; `/api/v2/` as the only prefix; one activation verb
(`POST /playsets/{name}` with an `activate` flag); settings only through
`/config`. Delete the nine dead endpoints and the PICO-8 routes.

**Migration cost.** Low. The handlers are fine; it is the plumbing and the
paths. Third-party callers are the README and HOW-TO curl examples plus the
jitter lab, all under the project's control.

**Verdict.** Would not build it this way. Fix in 2.0; it is a clean break
anyway.

## 2. The response envelope is a convention, not a function

`http_api_utils.c` has the right helpers (`send_json`, `send_json_error` with
the canonical `{"ok":false,"error":..,"code":..}`, `send_json_root`,
`recv_json_object`). Errors are consistent: 175 call sites, 84 string codes,
always with an HTTP status. Success is not: 30 handlers go through
`send_json_root`, 48 send hand-written literal JSON, and about 14 of those
skip the `data` wrapper (`http_api_rest_status.c:816` puts `device_name` at
the root; `http_api_rest_pinned.c:534` puts `slug` at the root;
`http_api_pages.c:200` puts `message` at the root). The routers and
`serve_file` answer 404/414/500 as plain text. This is why `compat.js:22`
needs `d.data.api_version || 1`.

The one place that does more than the minimum is `/playsets/active`, which
computes a weak ETag and answers 304 (`http_api_rest_playsets.c:47,428-497`)
for the 4 s poll in `index.html`.

**Fresh design.** `send_ok(req, cJSON *data)` is the only success path and
adds `ok:true` and `data` itself. Every handler returns a `cJSON *` or an
error enum; the envelope is applied in one place. ETag on every GET of a
resource.

**Migration cost.** Low, mechanical, 48 sites.

## 3. Slow work on the httpd task

`httpd_config_t` (`http_api.c:838-849`): core 0, 8 KB stack, 12 sockets,
`SO_LINGER{1,0}` to avoid TIME_WAIT exhausting lwIP's 32 sockets. The
`api_worker` task (`:87-146`) carries five enum commands and returns 202
without a result channel. Everything else runs inline in the httpd task:

- `POST /ota/install` sends 202, sleeps 500 ms, then runs
  `esp_https_ota_begin/perform` to completion on the httpd task
  (`http_api_ota.c:240-250`, `ota_manager_install.c:222-248`). The server is
  frozen for other clients during a firmware download.
- `POST /upload` streams up to 16 MiB to the SD card in the handler
  (`http_api_upload.c:124-363`). This one is defensible.
- `GET /status` and `/api/storage` call `esp_vfs_fat_info`
  (`storage_eviction.c:165`), a FAT free-cluster walk that can take seconds
  on a large card, on every status poll.
- `POST /erase` blocks 1.2 s then `esp_restart()`.

The 300-line Makapix MQTT command dispatcher (`http_api.c:173-480`) lives in
the HTTP component because it shares `enqueue_cmd`.

**Fresh design.** The job runner from the firmware page. Handlers that do
work longer than a few ms enqueue a `{fn, arg}` and return 202 with a job id;
`GET /api/v2/jobs/{id}` reports progress. The free-space figure is cached by
the storage layer and refreshed after every write burst, never computed in a
request. A `player_commands` module that both HTTP and MQTT call makes the two
transports obviously equivalent and testable without a socket.

**Migration cost.** Low once the job runner exists.

**Verdict.** Would not build it this way. Fix in 2.0.

## 4. Dual OTA: what exists only because the UI is a separate partition

Firmware OTA (`github_ota.c`, `ota_manager.c`, `ota_manager_install.c`) is
straightforward: list releases, compare `tag_name`, fetch `p3a.bin` and its
`.sha256`, install with `esp_https_ota`. Web UI OTA (`ota_manager_webui.c`,
859 lines) fetches `manifest.json`, compares an `X.Y` version against
`/webui/version.txt` on LittleFS, downloads the whole 4 MB `storage.bin` into
PSRAM, SHA-256s it, sets NVS `webui_invalid`, unmounts, erases and rewrites
the partition, verifies by read-back, remounts. A health check
(`p3a_board_fs.c:140-153`) decides whether to serve the real UI or a 6 KB
embedded "surrogate" recovery page (`surrogate_ui.h`) that polls
`/ota/webui/status` and POSTs `/ota/webui/repair`. `compat.js` shows an
"outdated" banner when `/status` `api_version` differs from the compiled
`P3A_API_VERSION`.

Everything below exists only because of that split:

| Artifact | Location | Approx. lines |
|---|---|---|
| Web UI OTA engine | `ota_manager_webui.c` | 859 |
| `manifest.json` fetch and X.Y compare | `github_ota.c:414-713` | ~300 |
| Web UI OTA types and API | `ota_manager.h:40-87, 215-320` | ~150 |
| Web UI check in the OTA task | `ota_manager.c:203-258, 506-539` | ~45 |
| `/ota/webui/*` handlers | `http_api_ota.c:48-103, 280-375, 402-428` | ~130 |
| Surrogate UI | `surrogate_ui.h` | 127 |
| Health check and surrogate switch | `p3a_board_fs.c:60-155`, `http_api_pages.c:28-74` | ~140 |
| LittleFS mount | `p3a_board_fs.c:20-57`, `main/p3a_main.c:559-565` | ~45 |
| VFS `serve_file` with `.gz` probing (never finds any) | `http_api_pages.c:62-158` | ~95 |
| Captive-portal reads from `/webui/setup/*` | `wifi_captive_portal.c:99-330, 457-473` | ~180 |
| CMake: `version.txt`, `metadata.json`, `compat.js`, LittleFS image | `CMakeLists.txt:104-140` | ~40 |
| CMake release script: `storage.bin`, `manifest.json` webui block | `CMakeLists.txt:179, 372, 406-440` | ~40 |
| `compat.js.in`, generated `compat.js`, `metadata.json`, `version.txt` | `webui/` | 44 + generated |
| Web UI half of `ota.html` | `webui/ota.html` | ~120 |
| Kconfig `OTA_WEBUI_ENABLE`, `storage` partition, `joltwallet/littlefs` dependency | | |
| Two release assets (`storage.bin`, `storage.bin.sha256`) and the `webui` block of `manifest.json` | release contract | |
| Two of three version numbers (`WEBUI_VERSION`, `P3A_API_VERSION`) and their rules in AGENTS.md and six docs pages | | |

About 2,100 to 2,300 lines of C, 250 lines of CMake and JS, one partition,
one managed dependency, two release assets, four generated files, and two
entries in `docs/known-issues.md`. `P3A_API_VERSION` was bumped four times
in 2026 and never since May; `WEBUI_VERSION` was bumped 46 times. The banner
mechanism's one recorded catch was a bug in its own plumbing (`compat.js`
drifted at 1 while the API was 4).

**Verdict.** Change A is correct and the review found nothing that argues for
keeping the split. The UI and the firmware are one program; shipping them as
two artifacts bought independent UI releases at the price of a second OTA
engine, a recovery mode, a compatibility banner and a class of field failures.

## 5. Embedding feasibility

Measured on a scratchpad copy of `webui/` without `pico8/` (which is 717 KB
by itself):

| | Raw | gzip -9 |
|---|---|---|
| Total, 41 files | 823,560 B | 402,212 B |
| HTML, JS, CSS, JSON only | ~535,000 | ~140,000 |
| PWA icons (`icon-512*.png`, `icon-192*.png`, `apple-touch-icon.png`, `favicon.png`) | ~274,000 | ~255,000 |
| `index.html` | 136,203 | 31,328 |
| `playset-editor.html` | 84,505 | 13,751 |
| `settings.html` | 78,651 | 16,366 |
| `museum/*.js` plus two JSON | ~127,000 | ~39,000 |

Two thirds of the compressed payload is PWA icons that gzip does not shrink;
shrinking the 512 px icons gets the embedded UI to about 150 KB. The project
uses `EMBED_FILES` nowhere today; its existing "embedding" is hand-converted
hex arrays (`main/*_img.c`, 337 KB of C source). `p3a.bin` for v1.2.4 is
2,461,648 B in an 8 MB slot. Embedding fits with over 5 MB of headroom.

**Fresh design.** A build step gzips `webui/` into `${CMAKE_BINARY_DIR}` and
`EMBED_FILES` the results. The server serves from a static table `{path, mime,
ptr, len}` with `Content-Encoding: gzip` and a strong `ETag` derived from the
firmware version, so browsers cache across reboots and invalidate on update.
No VFS, no LittleFS, no health check, no surrogate. The captive portal serves
its four pages from the same table, so a damaged flash is no longer a UI
failure mode (today a device with a damaged LittleFS and no Wi-Fi shows "UI
files not found. Please reflash the device." at the one moment the user
cannot reach the repair page, `wifi_captive_portal.c:212-218`).

The freed 4 MB `storage` partition is a 2.0 decision: enlarge the app slots,
keep it as spare, or repurpose it as a small data partition. Changing
`partitions.csv` is already a full-reflash event, which 2.0 is.

**Migration cost.** Low. Two days of CMake and a static-file handler, then a
deletion pass over the table in section 4.

## 6. The web UI itself

Four pages, 7,451 lines, roughly 65 percent inline JavaScript:

| Page | Total | Script | Style | Markup |
|---|---|---|---|---|
| `index.html` | 3,156 | 2,266 | 740 | 150 |
| `playset-editor.html` | 2,072 | 1,391 | 559 | 122 |
| `settings.html` | 1,674 | 1,008 | 136 | 530 |
| `ota.html` | 549 | 321 | 155 | 73 |

Shared JS is `compat.js` and `theme.js`. There is no shared request helper:
`index.html:1893` has `sendJSON()` plus 11 raw `XMLHttpRequest` uses,
`settings.html:706` has `mergeConfig()` plus 9 XHR, the editor 8 XHR,
`ota.html` 5 `fetch`; 42 `fetch(` and 28 XHR sites in total, about 40
distinct device paths. Each of six museum adapters defines its own
`getJson()`.

Validation ranges are hard-coded on both sides and already disagree: Klipy
cache size `32..2500` in `config_store_klipy.c:73` and `settings.html:446,1134`;
dwell time capped at 86,400,000 ms in `config_store_settings.c:273` and at
100,000 via `/settings/dwell_time` (`http_api_rest_settings.c:293`); the UI
default of 30000 at `settings.html:1641` is independent of the C default. The
UI maps none of the 84 error codes; it shows the `error` text verbatim.

The browser museum adapters (`webui/museum/*.js`, 9 adapters) and the device
adapters (`components/art_institution/museums/*.c`, 4,765 lines) are parallel
implementations of the same APIs. Duplicated: every API base and IIIF host,
the axis maps (`artic.js:54-58` vs `artic.c:70-83`; `mia.js:40` vs `mia.c:80`;
HAM, SI, Wellcome likewise), and the museum id list, which `museum/index.js:9-12`
says must match `art_institution.c` by hand.

**Fresh design, within the no-build-step constraint.** One `static/api.js`
(`api.get/post/put/del` returning the envelope and throwing on `!ok`) used by
every page. `/api/v2/init` ships the numeric limits and defaults so the UI
never hard-codes a range. A `museums.json` that the browser loads directly and
the build compiles into a C table, so a drift between the two adapter sets is
a build error. The browser keeps talking to museum APIs directly; given the
Cloudflare TLS-fingerprint problem in `docs/known-issues.md` that split is the
right one.

Splitting the three big pages' inline scripts into `static/*.js` files is
worth doing for the embedded build (they gzip and cache separately) and costs
nothing in tooling.

**Migration cost.** Low to medium, mostly in `index.html`.

## 7. Versioning after A

With the UI embedded, `WEBUI_VERSION`, `compat.js`, `metadata.json`,
`version.txt` and `requires_api_version` are dead. `api_version` in `/status`
can stay as a courtesy to third-party clients, but `firmware_version` carries
the same information; one version number is the right count. The Makapix MQTT
commands are versioned by the MQTT protocol document, not by the HTTP API.

## 8. Auth and CORS

None today, and none recommended for 2.0. The device is LAN-only and
mDNS-addressed, the UI is same-origin, and the curl examples and lab tooling
depend on the open API. It should be stated in `HOW-TO-USE.md` that the API is
unauthenticated on the LAN; today it is unstated.

## 9. What is good and should be kept

- `send_json_error` with HTTP status plus string code is the right error
  shape and already consistent.
- The ETag/304 pattern on `/playsets/active`.
- The `/api/init` single boot payload for the UI.
- `SO_LINGER` and `lru_purge_enable` on the server; the socket budget is
  understood.
- The web flasher reads `flash_args`, so dropping `storage.bin` needs no
  flasher change.
- The browser-side museum browse with the device-side refresh is the right
  division of labour; only the duplicated tables need a single source.
