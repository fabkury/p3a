# Playset Editor - Pre-Implementation Questions

## 1. API Endpoint Mismatch

The mock-up calls these endpoints:

| Operation | Mock-up uses | Actual backend |
|-----------|-------------|----------------|
| List all | `GET /playsets` | `GET /playsets` (matches) |
| Load one | `GET /playset/{name}` | `GET /playsets/{name}` |
| Save | `PUT /playset/{name}` | `POST /playsets/{name}` |
| Delete | `DELETE /playset/{name}` | `DELETE /playsets/{name}` |
| Activate | `POST /playset/{name}` | `POST /playset/{name}` (legacy) or `POST /playsets/{name}` with `"activate": true` |

**Question:** Should the editor simply target the real endpoints (`/playsets/` plural, POST instead of PUT), or do you also want changes on the backend side? The real API also supports `"activate": true` in the POST body, so "Save & Play" can be a single request instead of the mock-up's two-step (PUT then POST).

Answer: Adapt the mock-up to the real endpoints. The codebase (the backend) is generally the source of truth, the mock-up should be the one that gets adapted.


## 2. Page URL and Routing

Existing pages are routed in `http_api_pages.c:245` (`http_api_pages_route_get`), each mapped to a LittleFS file:

| URL | File |
|-----|------|
| `/` | `/spiffs/index.html` |
| `/settings` | `/spiffs/settings.html` |
| `/giphy` | `/spiffs/giphy.html` |
| `/config/network` | `/spiffs/config/network.html` |

**Question:** What URL should the playset editor live at?

Answer: `/playset-editor` - simple, unambiguous



## 3. Navigation from Main Page

`index.html` currently has channel buttons that call `playPlayset(name)` to activate hardcoded playsets like `channel_recent`, `channel_promoted`, `channel_sdcard`. There's no visible link to the settings or giphy pages from the first ~80 lines of markup (they may be accessed directly by URL).

**Question:** Should the playset editor be linked from `index.html`? If so, where (e.g., a new button in the controls area, a nav bar, a gear icon)?

Answer: Yes, it should be linked from `index.html`. There should be another button at the bottom, just like "Settings", "Network", etc.


## 4. XHR vs Fetch

The existing pages (`index.html`, `settings.html`) use `XMLHttpRequest`. The mock-up uses `fetch()`. Both work on modern browsers.

**Question:** Should the production page use `fetch()` (as in the mock-up) or `XMLHttpRequest` (matching the existing pages)? `fetch` is cleaner and more modern. The ESP32's HTTP server doesn't care either way.

Answer: Use `XMLHttpRequest`.


## 5. Giphy Channel Name Encoding

The mock-up stores Giphy channels with `name: "trending"` or `name: "search_cats"`. The backend's `playset_json.c` just stores whatever string is in the `name` field of each channel entry (max 32 chars).

**Question:** Is the `trending` / `search_{query}` convention correct, or should the name field encode differently (e.g., `giphy_trending`, `giphy_search_cats`)? The play scheduler needs to interpret these names when it encounters a `PS_CHANNEL_TYPE_GIPHY` channel.

Answer: Thanks for the question. The names should follow the convention you suggested: `giphy_trending`, `giphy_search_cats`, etc. Furthermore, those names need to be sanitized into filename-safe strings, because they end up being used as file names for the channel caches.



## 6. Named Channel Options

The mock-up offers two named channels: "All Recent Artworks" (`all`) and "Promoted Artworks" (`promoted`).

**Question:** Are these the only two named channels, or should the editor also support others? Is there a way to dynamically fetch available named channels from the backend, or are they hardcoded?

Answer: For now, those are the only two available named channels. In the future that may be more.


## 7. Active Playset Indicator

The mock-up's list view shows each playset's name, channel count, balance mode, and selection mode, but doesn't indicate which playset (if any) is currently active.

**Question:** Should the list view show which playset is currently playing? The backend exposes this via `GET /channel` (returns `playset` field in the response).

Answer: Thanks for the question. Yes, the list view should show which playset is currently playing. However, the /channel endpoint will eventually deprecated. For now it should keep all its functionalities, but another, playset-centric endpoint should be used to retrieve the currently active playset. At some point in the future, /channel should be removed (please make note of that in the comments in the code).


## 8. "Default" or Protected Playsets

**Question:** Is there a concept of a built-in/default playset that shouldn't be deletable or renamable? Or can the user freely create, edit, and delete all playsets?

Answer: Thanks for the question. Yes, we need to implement a hardcoded list of playset names that are not available nor deletable. For now, that list will only include one item: "followed_artists", which is a playset used also by Makapix Club. In the future there may be other reserved playsets that can't be deleted or overwritten.


## 9. Maximum Playsets

The backend's `playset_store_list()` allocates room for 32 entries.

**Question:** Is 32 the hard cap on total saved playsets? Should the editor enforce this limit and disable "Create New" when 32 are stored?

Answer: Yes, that is the hard cap. The editor should enforce this limit.


## 10. LittleFS Size Budget

The LittleFS partition is 4 MB and already holds `index.html`, `settings.html`, `giphy.html`, `config/network.html`, PICO-8 assets, etc.

**Question:** Is space a concern? The mock-up HTML is ~40 KB uncompressed (~8 KB gzipped). Should I gzip-compress the production file, or is raw HTML fine? (The `serve_file` function already supports `.gz` transparently.)

Answer: Space is not a concern, but efficiency is. If the unzipping is going to be done server-side, then don't use it. If the server transfers a gzipped asset that the client browser unzips, then use it.


## 11. Duplicate Playset Names

The mock-up makes the name field read-only when editing an existing playset but free-text when creating.

**Question:** If a user creates a playset with a name that already exists, should we overwrite silently, warn and confirm, or reject? The backend's `playset_store_save()` overwrites without complaint.

Answer: Warn and confirm.


## 12. compat.js

All existing pages include `<script src="/static/compat.js"></script>`, which checks the firmware API version and shows a warning banner if the web UI and firmware are out of sync.

**Question:** The playset editor should include this too, correct?

Answer: Correct.


## 13. Scope of This Task

The mock-up is a complete, self-contained HTML file with inline CSS and JS. The production page needs:

1. **The HTML/CSS/JS file** placed in `webui/` (source for LittleFS)
2. **A route added** in `http_api_pages.c` to serve it
3. **(Optional)** A link from `index.html`
4. **(Optional)** Backend changes if any API gaps are found

**Question:** Is that the right scope? Anything else in or out?

Answer: That seems correct to me.