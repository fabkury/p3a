# Part 1 — Intro, Web Flasher, Windows Flasher

Covers `docs/flash-p3a.md` lines 1–38:
- Title and intro paragraph (lines 1–5)
- Option 1: Web Flasher (lines 7–23)
- Option 2: p3a Flasher (Windows) (lines 25–38)

## Verification findings

### Intro paragraph (lines 1–5)

1. **Statement (line 3):** "all future updates are installed wirelessly via `http://p3a.local/ota`."
   - **Status:** CORRECT.
   - **Evidence:**
     - `/ota` GET handler is registered and serves `webui/ota.html` — `components/http_api/http_api_ota.c:383-385` (`if (strcmp(uri, "/ota") == 0) { return h_get_ota_page(req); }`). Sibling routes `/ota/status`, `/ota/check`, `/ota/install`, `/ota/rollback` confirm a working OTA UI.
     - The `p3a.local` mDNS hostname is started in both STA and AP modes — `components/wifi_manager/app_wifi.c:450, 468` and `components/wifi_manager/wifi_captive_portal.c:670, 739`.
     - The same wording appears verbatim in `CMakeLists.txt:259` and is consistent with `README.md:53, 104` and `docs/HOW-TO-USE.md:121, 129`.
   - **Proposed fix:** none.

### Option 1: Web Flasher (lines 7–23)

2. **Statement (line 13):** A hosted web flasher exists at `https://fabkury.github.io/p3a/web-flasher/`.
   - **Status:** UNVERIFIABLE from the codebase — *and likely INCORRECT.*
   - **Evidence:** No `web-flasher/` directory exists anywhere in the repo (verified via `Glob` for `web-flasher/**` and `**/web-flasher*` — no results). There is no GitHub Pages source, no `index.html`, no ESP Web Tools manifest committed for a hosted flasher. The repo does ship per-version `manifest.json` files under `release/v*/manifest.json`, but those describe the firmware/web-UI for OTA (`api_version`, `firmware`, `webui`) — they are not ESP Web Tools manifests and would not power a WebSerial flasher. Whether a Pages site has been published from a different branch/repo cannot be confirmed without a network fetch, but nothing in this repo backs the claim.
   - **Proposed fix:** Either (a) add the web-flasher source under `web-flasher/` (with `index.html` + an ESP Web Tools `manifest.json` listing chip `ESP32-P4` and per-version builds) and configure GitHub Pages, or (b) remove Option 1 from the doc until such a tool is published. If a Pages site genuinely exists outside this repo, reference its actual source location in a comment so future audits can verify it.

3. **Statement (line 11):** "Chrome 89+ or Edge 89+ (Firefox and Safari don't support WebSerial)".
   - **Status:** UNVERIFIABLE from the codebase (browser-capability claim, not code-driven).
   - **Evidence:** The WebSerial API requirement is a property of the browser, not of this firmware. The codebase contains no WebSerial-using page to validate the version threshold against. The statement is consistent with public Chrome/Edge documentation (WebSerial shipped in Chromium 89), but please double-check independently.
   - **Proposed fix:** none from a code-audit standpoint; depends on resolution of finding 2.

4. **Statement (line 18):** "Wait ~2 minutes".
   - **Status:** UNVERIFIABLE.
   - **Evidence:** No timing data is recorded in the repo. The same figure is used in `flasher/README.md:19` for the standalone flasher, suggesting it is a rough estimate carried over rather than a measurement.
   - **Proposed fix:** none, but consider a less precise hedge (e.g., "a couple of minutes") if no measurement backs the figure.

5. **Statement (line 21):** "The web flasher downloads firmware directly from GitHub and flashes it to your device — works on Windows, macOS, and Linux."
   - **Status:** UNVERIFIABLE / depends on finding 2. There is no web-flasher source in this repo to inspect for its download behavior or platform coverage.
   - **Proposed fix:** resolve along with finding 2.

### Option 2: p3a Flasher (Windows) (lines 25–38)

6. **Statement (line 29):** "Download `p3a-flasher.exe` from the [releases folder](https://github.com/fabkury/p3a/tree/main/release/)".
   - **Status:** PARTIALLY INCORRECT.
   - **Evidence:**
     - `release/` does exist and is checked into the tree, with per-version subdirectories (`release/v0.7.6-dev/` … `release/v0.9.0/`). The path itself resolves.
     - However, `p3a-flasher.exe` is *only* present in older versions: `release/v0.7.6-dev/`, `v0.7.7-dev/`, `v0.7.9-dev/`, `v0.8.0-dev/`, `v0.8.1-dev/`, `v0.8.2-dev/`, `v0.8.3-dev/`, `v0.8.4-dev/`, `v0.8.5-dev/`. It is **missing** from the most recent releases — `release/v0.8.6-dev/`, `v0.8.7-dev/`, `v0.8.8/`, `v0.9.0/` (verified via `Glob release/v0.x.x/*`). A user landing on the `/tree/main/release/` page and opening the latest version folder will not find the exe and will be confused.
     - The flasher's own README points elsewhere: `flasher/README.md:15` directs users to "the [Releases page](https://github.com/fabkury/p3a/releases)" (i.e. GitHub Releases artifacts), not to the in-tree `release/` directory.
   - **Proposed fix:** Change the link to the GitHub Releases page to match `flasher/README.md`:
     > `Download **p3a-flasher.exe** from the latest [GitHub Release](https://github.com/fabkury/p3a/releases/latest).`
     If the in-tree `release/` directory is intentionally the distribution channel, fix the build so every published version contains `p3a-flasher.exe` (currently the latest four don't), and recommend the user pick the latest version subfolder rather than the parent `release/` listing.

7. **Statement (line 27 / line 36):** Standalone Windows app, "no installation, configuration or Internet connection needed."
   - **Status:** CORRECT.
   - **Evidence:** `flasher/p3a_flasher.py` + `flasher/build_flasher.py` produce a PyInstaller-bundled exe with embedded firmware (`flasher/README.md:7-11, 22-30`: "Embedded firmware — Includes the firmware version it was built with", "No installation — Single portable executable"). The CMake integration at build time copies the produced `p3a-flasher.exe` into `release/v{VERSION}/`. With the firmware embedded, no Internet is required for the default flow.
   - **Proposed fix:** none.

8. **Statement (line 31):** "Run `p3a-flasher.exe`" then click **Flash Device**, device auto-detected.
   - **Status:** CORRECT.
   - **Evidence:** `flasher/README.md:9` ("Auto-detect devices — Automatically finds connected ESP32-P4") and the matching end-user steps at `flasher/README.md:13-20` mirror the doc's sequence.
   - **Proposed fix:** none.

9. **Statement (line 33):** "Wait ~2 minutes".
   - **Status:** UNVERIFIABLE (same as finding 4). Matches `flasher/README.md:19`.
   - **Proposed fix:** none.

### Summary

| # | Statement | Status |
|---|-----------|--------|
| 1 | OTA at `http://p3a.local/ota` | CORRECT |
| 2 | Hosted web flasher at `fabkury.github.io/p3a/web-flasher/` | UNVERIFIABLE — likely INCORRECT (no source in repo) |
| 3 | Chrome/Edge 89+ WebSerial requirement | UNVERIFIABLE (out-of-scope browser claim) |
| 4 | Web flasher takes ~2 minutes | UNVERIFIABLE |
| 5 | Web flasher downloads firmware from GitHub, cross-platform | UNVERIFIABLE (depends on #2) |
| 6 | `p3a-flasher.exe` at `github.com/fabkury/p3a/tree/main/release/` | PARTIALLY INCORRECT — exe missing from latest 4 versions; flasher README points to Releases page |
| 7 | Standalone, offline, no install | CORRECT |
| 8 | Auto-detects device, single click | CORRECT |
| 9 | Windows flasher takes ~2 minutes | UNVERIFIABLE |

The two action items most worth resolving are **#2** (either ship the web flasher or remove Option 1) and **#6** (point at GitHub Releases, or restore `p3a-flasher.exe` to every in-tree release subfolder).
