# Museum Channels — Wellcome + SMK Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Wellcome Collection and SMK as the fourth and fifth museum channels in p3a, end-to-end (browse, persist, refresh, download, play), purely additive over the existing AIC/Rijks/V&A infrastructure.

**Architecture:** Two near-clones of the existing V&A C adapter under `components/art_institution/museums/`, registered through the existing dispatch table; two near-clones of `webui/museum/vam.js` registered in the existing `webui/museum/index.js`; both museums use inline IIIF resolution (no lazy walk like Rijks). Format-neutral — reuses `PS_CHANNEL_TYPE_INSTITUTION = 7`, the 64-byte cache entry layout, the vault layout, the rate-limit table, and the browse modal verbatim.

**Tech Stack:** C (ESP-IDF v5.5.x, FreeRTOS, esp_http_client, cJSON, mbedtls SHA256), vanilla ES modules for the web UI, no new dependencies. Builds are user-run on Windows PowerShell with ESP-IDF activated; testing is manual per the project's existing convention.

**Source of truth:** [`docs/superpowers/specs/2026-05-12-museum-wellcome-smk-design.md`](../specs/2026-05-12-museum-wellcome-smk-design.md).

---

## Prerequisites for the implementer

Before starting Task 1, read these files in order to internalize the pattern:

1. **`docs/art-institutions/finalized-design.md`** — the parent design that this plan extends. Especially §3 (architecture), §4 (data model), §7 (lifecycle), §11.1 (rate limits).
2. **`components/art_institution/museums/vam.c`** — the C adapter you'll be cloning twice. Note: HTTP retry loop, page merge under `channel_cache_lifecycle_lock()`, `Si` hash for orphan eviction, partial-with-content treatment, `last_refresh` persistence gated on `sntp_sync_is_synchronized()`.
3. **`components/art_institution/art_institution.c`** — the dispatch table you'll be appending to.
4. **`components/art_institution/include/art_institution_types.h`** — the `museum_id_t` enum (note the sentinel is `ART_INSTITUTION_NUM_MUSEUMS`, not `_COUNT`) and the 64-byte `institution_channel_entry_t` layout.
5. **`webui/museum/vam.js`** — the JS adapter you'll be cloning twice.
6. **`webui/museum/browse.js`** — the browse modal; you won't edit it, but understand how it consumes adapters via the `listAdapters()` registry.
7. **`reference/museum-art/source/wellcome/run.py`** and **`reference/museum-art/source/smk/run.py`** — the Python reference implementations that ground the API shape decisions.

**Build instructions** (from project CLAUDE.md — user-run only, **never execute as the agent**):

```powershell
$env:PYTHONUTF8="1"
# One-time per session, choose one:
# C:\Users\Fab\esp\v5.5.1\esp-idf\export.ps1
# C:\Espressif\Initialize-Idf.ps1 -IdfId esp-idf-b29c58f93b4ca0f49cdfc4c3ef43b562
idf.py build           # full build
idf.py flash monitor   # flash + serial console
```

When a task says "ask the user to build/flash", surface the command and wait for the user's go-ahead — do not invoke it yourself.

---

# Phase 1 — M4: Wellcome end-to-end

## Task 1: Wellcome scaffolding (enum, prototypes, CMakeLists, dispatch entry)

**Files:**
- Modify: `components/art_institution/include/art_institution_types.h` (around line 37 — the enum)
- Modify: `components/art_institution/art_institution_internal.h` (after the V&A block at the end)
- Modify: `components/art_institution/CMakeLists.txt` (line 11 — SRCS list)
- Modify: `components/art_institution/art_institution.c` (after the V&A entry in `ART_INSTITUTION_MUSEUMS[]`)

- [ ] **Step 1: Append `ART_INSTITUTION_MUSEUM_WELLCOME` to the enum**

Edit `components/art_institution/include/art_institution_types.h`. The existing enum body is:

```c
typedef enum {
    ART_INSTITUTION_MUSEUM_ARTIC = 0,
    ART_INSTITUTION_MUSEUM_RIJKS = 1,
    ART_INSTITUTION_MUSEUM_VAM   = 2,
    ART_INSTITUTION_NUM_MUSEUMS  // sentinel; keep last. ...
} museum_id_t;
```

Replace with:

```c
typedef enum {
    ART_INSTITUTION_MUSEUM_ARTIC    = 0,
    ART_INSTITUTION_MUSEUM_RIJKS    = 1,
    ART_INSTITUTION_MUSEUM_VAM      = 2,
    ART_INSTITUTION_MUSEUM_WELLCOME = 3,
    ART_INSTITUTION_NUM_MUSEUMS  // sentinel; keep last. ...
} museum_id_t;
```

(Keep the existing trailing comment intact — only add the new value and re-align the `=`.)

- [ ] **Step 2: Append Wellcome internal prototypes**

Edit `components/art_institution/art_institution_internal.h`. After the V&A block at the end of the file (before the closing `extern "C"` brace), append:

```c
// ============================================================================
// Wellcome adapter entry points (defined in museums/wellcome.c)
// ============================================================================

esp_err_t art_institution_wellcome_refresh_channel(const char *channel_id,
                                                   const char *axis,
                                                   const char *term_id);

esp_err_t art_institution_wellcome_build_iiif_url(const institution_channel_entry_t *entry,
                                                  int longest_side,
                                                  char *out, size_t len);
```

- [ ] **Step 3: Add `museums/wellcome.c` to the build**

Edit `components/art_institution/CMakeLists.txt`. The current SRCS list ends with `"museums/vam.c"`; add `"museums/wellcome.c"` on the next line:

```cmake
        "museums/vam.c"
        "museums/wellcome.c"
```

- [ ] **Step 4: Append Wellcome to the dispatch table**

Edit `components/art_institution/art_institution.c`. The current `ART_INSTITUTION_MUSEUMS[]` array ends with the V&A entry's closing brace (around line 51 — `},`). After that V&A entry and before the closing `};`, insert:

```c
    {
        .id              = "wellcome",
        .display         = "Wellcome Collection",
        .museum_enum     = ART_INSTITUTION_MUSEUM_WELLCOME,
        .refresh_channel = art_institution_wellcome_refresh_channel,
        .build_iiif_url  = art_institution_wellcome_build_iiif_url,
        .resolve_entry   = NULL,  // Wellcome returns the IIIF id inline; no walk.
    },
```

- [ ] **Step 5: Verify the file compiles (no `museums/wellcome.c` body yet — expect linker errors)**

Ask the user to run `idf.py build` once. The expected outcome is a **linker failure** with two unresolved symbols (`art_institution_wellcome_refresh_channel` and `art_institution_wellcome_build_iiif_url`). This proves the scaffolding is wired correctly. If the **header/enum** changes don't compile (compile-time error rather than link-time), fix those errors before proceeding.

- [ ] **Step 6: Do not commit yet**

Task 2 produces the missing symbols; commit after that.

---

## Task 2: Wellcome C adapter (`museums/wellcome.c`)

**Files:**
- Create: `components/art_institution/museums/wellcome.c`

This file mirrors `museums/vam.c` structure exactly — same HTTP retry loop, same page-merge under `channel_cache_lifecycle_lock()`, same `Si` hash + orphan-eviction pattern, same partial-with-content treatment. The Wellcome-specific differences:

- 4 axes (vs V&A's 3) with different filter parameters.
- Results array is `results` (vs V&A's `records`).
- The IIIF id (`vid`) is nested under `items[].locations[]` (vs V&A's inline `_primaryImageId`) — needs a small walker.
- Pagination metadata: `totalResults` field (vs V&A's `info.record_count`).
- 256 KB response buffer (vs V&A's 192 KB — Wellcome's `include=items` response is richer).

Read `museums/vam.c` once before writing this file so you can spot what changed and what is verbatim.

- [ ] **Step 1: Create the file with the complete contents**

Create `components/art_institution/museums/wellcome.c`:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/wellcome.c
 * @brief Wellcome Collection adapter — refresh + IIIF URL build.
 *
 * Wellcome's catalogue API is structurally simple: each result includes
 * `items[].locations[]` where `locationType.id == "iiif-image"` carries
 * the IIIF service URL. Refresh stores entries fully resolved with
 * `extension = 3` — no lazy walk like Rijks needs.
 *
 * Four facet axes (workType, genres, subjects, contributors). For
 * workType, the filter value is the term `id` (e.g. `k`); for the others,
 * the filter value is the term `label` itself (Wellcome doesn't expose a
 * stable short id for those axes). The browse modal hides labels
 * exceeding 32 chars so the playset identifier[33] slot fits.
 *
 * Reference: https://developers.wellcomecollection.org/api and
 * reference/museum-art/source/wellcome/run.py
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "art_institution_types.h"
#include "channel_cache.h"
#include "channel_metadata.h"
#include "config_store.h"
#include "psram_alloc.h"
#include "sd_path.h"
#include "sntp_sync.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ai_wellcome";

#define WELLCOME_API_BASE           "https://api.wellcomecollection.org/catalogue/v2"
#define WELLCOME_IIIF_BASE          "https://iiif.wellcomecollection.org/image/"
#define WELLCOME_PAGE_LIMIT         100
#define WELLCOME_RESPONSE_BUF_SIZE  (256 * 1024)
#define WELLCOME_FETCH_MAX_ATTEMPTS 3

static const uint32_t s_fetch_backoff_ms[WELLCOME_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

extern void download_manager_rescan(void);

// ----- Axis -> filter-param map -------------------------------------------

typedef struct {
    const char *axis;          // adapter axis name
    const char *filter_param;  // Wellcome query parameter
} wellcome_axis_map_t;

static const wellcome_axis_map_t WELLCOME_AXES[] = {
    { "workType",     "workType"                  },
    { "genres",       "genres.label"              },
    { "subjects",     "subjects.label"            },
    { "contributors", "contributors.agent.label"  },
};

static const char *wellcome_filter_param_for_axis(const char *axis)
{
    if (!axis) return NULL;
    for (size_t i = 0; i < sizeof(WELLCOME_AXES) / sizeof(WELLCOME_AXES[0]); i++) {
        if (strcmp(WELLCOME_AXES[i].axis, axis) == 0) return WELLCOME_AXES[i].filter_param;
    }
    return NULL;
}

// ----- IIIF URL -----------------------------------------------------------

esp_err_t art_institution_wellcome_build_iiif_url(const institution_channel_entry_t *entry,
                                                  int longest_side,
                                                  char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, WELLCOME_IIIF_BASE "%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- Helpers ------------------------------------------------------------

static int drain_body(esp_http_client_handle_t client, char *buf, size_t buf_size)
{
    int total = 0;
    bool read_err = false;
    while (total < (int)buf_size - 1) {
        int n = esp_http_client_read(client, buf + total, buf_size - 1 - total);
        if (n < 0) { read_err = true; break; }
        if (n == 0) break;
        total += n;
    }
    return read_err ? -1 : total;
}

static uint32_t parse_retry_after(const char *value)
{
    if (!value) return 0;
    while (*value == ' ') value++;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || v <= 0) return 0;
    if (v > 3600) v = 3600;
    return (uint32_t)v;
}

/**
 * Walk a Wellcome result's items[].locations[] looking for an
 * iiif-image location, and capture the vid from the URL.
 *
 * Expected URL shape: https://iiif.wellcomecollection.org/image/{vid}[/info.json]
 *
 * Returns true and writes the vid into out on success.
 */
static bool extract_wellcome_vid(const cJSON *work, char *out, size_t out_len)
{
    if (!work || !out || out_len == 0) return false;
    const cJSON *items = cJSON_GetObjectItem(work, "items");
    if (!cJSON_IsArray(items)) return false;
    int items_n = cJSON_GetArraySize(items);
    static const char prefix[] = "https://iiif.wellcomecollection.org/image/";
    const size_t prefix_len = sizeof(prefix) - 1;

    for (int i = 0; i < items_n; i++) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) continue;
        const cJSON *locations = cJSON_GetObjectItem(item, "locations");
        if (!cJSON_IsArray(locations)) continue;
        int locs_n = cJSON_GetArraySize(locations);
        for (int j = 0; j < locs_n; j++) {
            const cJSON *loc = cJSON_GetArrayItem(locations, j);
            if (!cJSON_IsObject(loc)) continue;
            const cJSON *ltype = cJSON_GetObjectItem(loc, "locationType");
            if (!cJSON_IsObject(ltype)) continue;
            const cJSON *ltype_id = cJSON_GetObjectItem(ltype, "id");
            if (!cJSON_IsString(ltype_id) || strcmp(ltype_id->valuestring, "iiif-image") != 0) continue;
            const cJSON *url = cJSON_GetObjectItem(loc, "url");
            if (!cJSON_IsString(url) || !url->valuestring[0]) continue;

            const char *u = url->valuestring;
            if (strncmp(u, prefix, prefix_len) != 0) continue;
            const char *rest = u + prefix_len;
            const char *slash = strchr(rest, '/');
            size_t vid_len = slash ? (size_t)(slash - rest) : strlen(rest);
            if (vid_len == 0 || vid_len >= out_len) continue;
            memcpy(out, rest, vid_len);
            out[vid_len] = '\0';
            return true;
        }
    }
    return false;
}

// ----- One-page fetch -----------------------------------------------------

/**
 * Fetch + parse one Wellcome /works page.
 *
 * The filter value goes straight onto the URL after percent-encoding —
 * for `workType` it's a short id (`k`/`q`/...), for the other axes it's
 * the term label itself which may contain spaces or punctuation. The
 * 32-char ceiling is enforced browser-side so we don't need to truncate
 * here; we'd see at most 32 chars before encoding.
 */
static esp_err_t wellcome_fetch_page(const char *filter_param,
                                     const char *filter_value,
                                     int page,
                                     char *response_buf,
                                     size_t response_buf_size,
                                     institution_channel_entry_t *out_entries,
                                     size_t max_entries,
                                     size_t *out_count,
                                     int *out_total_results,
                                     bool *has_more)
{
    *out_count = 0;
    *has_more = false;
    if (out_total_results) *out_total_results = 0;

    char encoded_value[256];
    ai_url_encode(filter_value, encoded_value, sizeof(encoded_value));

    char url[640];
    int wrote = snprintf(url, sizeof(url),
                         WELLCOME_API_BASE
                         "/works?page=%d&pageSize=%d"
                         "&items.locations.locationType=iiif-image"
                         "&include=items"
                         "&%s=%s",
                         page, WELLCOME_PAGE_LIMIT,
                         filter_param, encoded_value);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for filter=%s", filter_param);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching page %d (filter=%s value=%.32s)", page, filter_param, filter_value);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < WELLCOME_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retrying Wellcome page fetch in %lums (attempt %d/%d)",
                     (unsigned long)s_fetch_backoff_ms[attempt],
                     attempt + 1, WELLCOME_FETCH_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
            total_read = 0;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        esp_http_client_set_header(client, "Accept", "application/json");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            continue;
        }

        esp_http_client_fetch_headers(client);
        int64_t content_length = esp_http_client_get_content_length(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 429) {
            char *retry_after = NULL;
            uint32_t cooldown = 0;
            if (esp_http_client_get_header(client, "Retry-After", &retry_after) == ESP_OK) {
                cooldown = parse_retry_after(retry_after);
            }
            art_institution_set_rate_limited("wellcome", cooldown);
            ESP_LOGW(TAG, "Wellcome returned 429 (cooldown %us)",
                     (unsigned)(cooldown ? cooldown : 60));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (status == 401 || status == 403) {
            ESP_LOGW(TAG, "Wellcome returned %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "Wellcome status %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        total_read = drain_body(client, response_buf, response_buf_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (total_read < 0) continue;
        if (total_read == 0) continue;
        if (total_read >= (int)response_buf_size - 1) {
            ESP_LOGE(TAG, "Wellcome response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        if (content_length > 0 && total_read < (int)content_length) {
            ESP_LOGW(TAG, "Wellcome truncated: got %d/%lld bytes",
                     total_read, (long long)content_length);
            continue;
        }
        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) {
        ESP_LOGE(TAG, "Wellcome page %d fetch failed after %d attempts",
                 page, WELLCOME_FETCH_MAX_ATTEMPTS);
        return ESP_FAIL;
    }
    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Wellcome JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) {
        ESP_LOGE(TAG, "Wellcome response missing 'results' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int total_results = 0;
    const cJSON *total_node = cJSON_GetObjectItem(root, "totalResults");
    if (cJSON_IsNumber(total_node)) total_results = (int)cJSON_GetNumberValue(total_node);
    if (out_total_results) *out_total_results = total_results;

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(results);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *work = cJSON_GetArrayItem(results, i);
        if (!cJSON_IsObject(work)) continue;

        char vid[sizeof(out_entries[0].iiif_key)] = {0};
        if (!extract_wellcome_vid(work, vid, sizeof(vid))) {
            // items.locations.locationType=iiif-image filter should keep this
            // branch rare, but Wellcome isn't perfectly consistent.
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("wellcome", vid);
        e->kind = 0;
        e->extension = 3;
        e->created_at = now;
        strlcpy(e->iiif_key, vid, sizeof(e->iiif_key));
        parsed++;
    }

    int seen_so_far = (page - 1) * WELLCOME_PAGE_LIMIT + array_size;
    *has_more = (array_size > 0) && (seen_so_far < total_results);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "Wellcome page %d: parsed %zu/%d entries (totalResults=%d), has_more=%d",
             page, parsed, array_size, total_results, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_wellcome_refresh_channel(const char *channel_id,
                                                   const char *axis,
                                                   const char *term_id)
{
    if (!channel_id || !axis || !term_id) return ESP_ERR_INVALID_ARG;

    const char *filter_param = wellcome_filter_param_for_axis(axis);
    if (!filter_param) {
        ESP_LOGE(TAG, "Unknown Wellcome axis '%s'", axis);
        return ESP_ERR_INVALID_ARG;
    }

    if (art_institution_is_rate_limited("wellcome")) {
        ESP_LOGW(TAG, "Wellcome rate-limited at refresh start, skipping");
        return ESP_ERR_INVALID_RESPONSE;
    }

    channel_cache_lifecycle_lock();
    bool cache_exists = (channel_cache_registry_find(channel_id) != NULL);
    channel_cache_lifecycle_unlock();
    if (!cache_exists) {
        ESP_LOGW(TAG, "Cache not found for channel '%s'", channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t cache_size = config_store_get_ai_cache_size();
    if (cache_size == 0) cache_size = 1024;
    if (cache_size > 4096) cache_size = 4096;

    char *response_buf = heap_caps_malloc(WELLCOME_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(WELLCOME_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        WELLCOME_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(WELLCOME_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;
    int page = 1;
    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        int total_results = 0;
        bool has_more = false;
        esp_err_t err = wellcome_fetch_page(filter_param, term_id, page,
                                            response_buf, WELLCOME_RESPONSE_BUF_SIZE,
                                            page_entries, WELLCOME_PAGE_LIMIT,
                                            &page_count, &total_results, &has_more);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }
        if (page_count == 0 && !has_more) {
            ESP_LOGI(TAG, "No entries on page %d, done", page);
            break;
        }

        if (page_count > 0) {
            size_t merge_limit = cache_size * 3;
            channel_cache_lifecycle_lock();
            channel_cache_t *page_cache = channel_cache_registry_find(channel_id);
            esp_err_t merge_err = page_cache
                ? art_institution_merge_entries(page_cache, page_entries, page_count, merge_limit)
                : ESP_ERR_NOT_FOUND;
            channel_cache_lifecycle_unlock();

            if (merge_err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Cache disappeared mid-refresh for '%s'", channel_id);
                refresh_completed = false;
                break;
            }
            if (merge_err != ESP_OK) {
                ESP_LOGW(TAG, "Merge failed on page %d: %s", page, esp_err_to_name(merge_err));
                refresh_completed = false;
                break;
            }

            for (size_t i = 0; i < page_count && si_count < cache_size; i++) {
                int32_t pid = page_entries[i].post_id;
                ai_si_node_t *existing = NULL;
                HASH_FIND_INT(si_hash, &pid, existing);
                if (!existing) {
                    ai_si_node_t *n = psram_malloc(sizeof(ai_si_node_t));
                    if (n) {
                        n->post_id = pid;
                        HASH_ADD_INT(si_hash, post_id, n);
                        si_count++;
                    }
                }
            }

            total_fetched += page_count;
            ESP_LOGI(TAG, "Wellcome page %d merged: %zu entries (total %zu)",
                     page, page_count, total_fetched);
            download_manager_rescan();
        }

        page++;
        if (!has_more) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    free(page_entries);

    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "wellcome");
        }
        channel_cache_lifecycle_unlock();
    }

    {
        ai_si_node_t *node, *tmp;
        HASH_ITER(hh, si_hash, node, tmp) {
            HASH_DEL(si_hash, node);
            free(node);
        }
    }

    if ((refresh_completed || partial_with_content) && sntp_sync_is_synchronized()) {
        char channels_path[128];
        if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
            strlcpy(channels_path, "/sdcard/p3a/channel", sizeof(channels_path));
        }
        channel_metadata_t meta = { .last_refresh = time(NULL), .cursor = "" };
        esp_err_t meta_err = channel_metadata_save(channel_id, channels_path, &meta);
        if (meta_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save channel metadata: %s", esp_err_to_name(meta_err));
        }
    }

    if (refresh_completed) {
        ESP_LOGI(TAG, "Wellcome refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "Wellcome refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Wellcome refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
```

- [ ] **Step 2: Verify TLS cert bundle (pre-merge gate)**

Before this commit lands, manually verify that `esp_crt_bundle` covers `api.wellcomecollection.org` and `iiif.wellcomecollection.org`. The simplest check: open `components/esp-idf/components/mbedtls/esp_crt_bundle/cacrt_all.pem` (or wherever the active bundle lives — `$IDF_PATH/components/mbedtls/esp_crt_bundle/cacrt_all.pem` on most installs), and grep for the issuer chain of each host. If absent, document the explicit `esp_crt_bundle_attach` + custom cert workaround in a comment near `cfg.crt_bundle_attach` before committing.

Ask the user to confirm this gate is cleared before proceeding.

- [ ] **Step 3: Ask the user to build, expecting success**

Ask the user to run `idf.py build`. Expected: clean build, no warnings on the new file (besides any pre-existing warnings unrelated to this change). If the build fails, fix the C errors before continuing.

- [ ] **Step 4: Commit**

```bash
git add components/art_institution/include/art_institution_types.h \
        components/art_institution/art_institution_internal.h \
        components/art_institution/CMakeLists.txt \
        components/art_institution/art_institution.c \
        components/art_institution/museums/wellcome.c
git commit -m "$(cat <<'EOF'
feat(art-institution): add Wellcome Collection adapter (M4)

C-side scaffolding + adapter for the Wellcome catalogue API. Mirrors
the V&A adapter shape: 4 facet axes (workType, genres, subjects,
contributors), inline IIIF resolution via items[].locations[], no
lazy walk. Append-only enum value and dispatch entry; no format break.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Wellcome JS adapter (`webui/museum/wellcome.js`)

**Files:**
- Create: `webui/museum/wellcome.js`

Mirrors `webui/museum/vam.js` structure. Wellcome-specific differences:

- 4 axes (vs V&A's 3).
- Term enumeration uses one aggregations call (vs V&A's facet probe-with-fallback).
- Each axis has a different filter parameter name (vs V&A's three numeric `id_*` params).
- For non-`workType` axes, the filter value IS the label (vs V&A which always uses an id).
- The **32-char long-label gate** filters terms before returning them.
- IIIF id (`vid`) is extracted from `items[].locations[]` (vs V&A's inline `_primaryImageId`).

- [ ] **Step 1: Create the file**

```javascript
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Wellcome Collection browse adapter.
//
// 4 facet axes (workType, genres, subjects, contributors). Term
// enumeration is a single aggregations request scoped to image-bearing
// works; no per-term count probe like AIC or V&A's venue axis needs.
//
// For non-workType axes Wellcome doesn't expose a stable short id, so
// the term label IS the filter value. We filter out terms whose label
// exceeds 32 chars to fit the playset identifier[33] slot (decision
// captured in docs/deferred/wellcome-long-labels.md).
//
// IIIF id (vid) lives nested under items[].locations[] where
// locationType.id == 'iiif-image'; refresh stores the vid as the
// iiif_key and the C-side build_iiif_url prepends the standard host.

const WORKS = 'https://api.wellcomecollection.org/catalogue/v2/works';
const IIIF_HOST = 'https://iiif.wellcomecollection.org/image';

const AXES = [
    { name: 'workType',     label: 'Work types',   filter: 'workType',                  agg: 'workType',                 keyField: 'id'    },
    { name: 'genres',       label: 'Genres',       filter: 'genres.label',              agg: 'genres.label',             keyField: 'label' },
    { name: 'subjects',     label: 'Subjects',     filter: 'subjects.label',            agg: 'subjects.label',           keyField: 'label' },
    { name: 'contributors', label: 'Contributors', filter: 'contributors.agent.label',  agg: 'contributors.agent.label', keyField: 'label' },
];

const AGG_BUCKET_SIZE = 100;   // (100) suffix on aggregations=
const MAX_LABEL_CHARS = 32;    // playset identifier[33] = 32 chars + null
const PAGE_SIZE = 100;         // matches C-side refresh

const IIIF_URL_RE = /^https:\/\/iiif\.wellcomecollection\.org\/image\/([^/]+)(?:\/info\.json)?$/;

function findAxis(name) {
    for (let i = 0; i < AXES.length; i++) {
        if (AXES[i].name === name) return AXES[i];
    }
    return null;
}

async function getJson(url) {
    const r = await fetch(url);
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'wellcome',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`Wellcome 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`Wellcome ${r.status} ${url}`);
    return r.json();
}

function getTitle(work) {
    if (typeof work.title === 'string' && work.title) {
        // Wellcome titles can be paragraph-length; trim for preview captions.
        return work.title.length > 140 ? work.title.slice(0, 137) + '…' : work.title;
    }
    return '(untitled)';
}

function getArtist(work) {
    const contribs = work.contributors;
    if (Array.isArray(contribs) && contribs.length > 0) {
        const first = contribs[0];
        const agent = first && first.agent;
        if (agent && agent.label) return String(agent.label);
    }
    return '';
}

function getDate(work) {
    const prod = work.production;
    if (Array.isArray(prod) && prod.length > 0) {
        const first = prod[0];
        const dates = first && first.dates;
        if (Array.isArray(dates) && dates.length > 0 && dates[0].label) {
            return String(dates[0].label);
        }
    }
    return '';
}

function extractVid(work) {
    if (!work || !Array.isArray(work.items)) return null;
    for (const item of work.items) {
        if (!item || !Array.isArray(item.locations)) continue;
        for (const loc of item.locations) {
            const ltype = loc && loc.locationType && loc.locationType.id;
            if (ltype !== 'iiif-image') continue;
            const url = String(loc.url || '');
            const m = IIIF_URL_RE.exec(url);
            if (m) return m[1];
            // Fallback: strip trailing /info.json if present.
            if (url.startsWith(`${IIIF_HOST}/`)) {
                const tail = url.slice(`${IIIF_HOST}/`.length);
                const slash = tail.indexOf('/');
                return slash >= 0 ? tail.slice(0, slash) : tail;
            }
        }
    }
    return null;
}

export class WellcomeAdapter {
    get id()          { return 'wellcome'; }
    get displayName() { return 'Wellcome Collection'; }
    get shortName()   { return 'Wellcome'; }
    get axes() {
        return AXES.map(a => ({ name: a.name, label: a.label }));
    }

    constructor() {
        // termsByAxis: axisName -> [{ id, label, count }] cached per session.
        this._termsByAxis = Object.create(null);
        this._aggCache = null;  // raw aggregations response, lazily loaded
    }

    async _loadAggregations() {
        if (this._aggCache) return this._aggCache;
        const aggList = AXES.map(a => `${a.agg}(${AGG_BUCKET_SIZE})`).join(',');
        const url = `${WORKS}?pageSize=1`
            + `&items.locations.locationType=iiif-image`
            + `&aggregations=${encodeURIComponent(aggList)}`;
        this._aggCache = await getJson(url);
        return this._aggCache;
    }

    async listCollections({ axis = 'workType' } = {}) {
        const axisDef = findAxis(axis);
        if (!axisDef) throw new Error(`Wellcome: unknown axis ${axis}`);
        if (this._termsByAxis[axis]) return this._termsByAxis[axis];

        const data = await this._loadAggregations();
        const aggs = (data && data.aggregations) || {};
        const block = aggs[axisDef.agg] || {};
        const buckets = Array.isArray(block.buckets) ? block.buckets : [];

        const out = [];
        for (const b of buckets) {
            const d = (b && b.data) || {};
            const id = d[axisDef.keyField] != null ? String(d[axisDef.keyField]) : null;
            const label = d.label != null ? String(d.label) : id;
            const count = Number((b && b.count) || 0);
            if (!id || !label) continue;
            if (count <= 0) continue;
            if (label.length > MAX_LABEL_CHARS) continue;  // identifier[33] gate
            out.push({ id, label, count });
        }
        out.sort((a, b) => b.count - a.count);
        this._termsByAxis[axis] = out;
        return out;
    }

    async listArtworks(termId, { offset = 0, rows = PAGE_SIZE, axis = 'workType' } = {}) {
        const axisDef = findAxis(axis);
        if (!axisDef) throw new Error(`Wellcome: unknown axis ${axis}`);
        const page = Math.floor(offset / rows) + 1;
        const params = new URLSearchParams({
            page: String(page),
            pageSize: String(rows),
            'items.locations.locationType': 'iiif-image',
            include: 'items',
            [axisDef.filter]: termId,
        });
        const d = await getJson(`${WORKS}?${params}`);
        const results = Array.isArray(d.results) ? d.results : [];
        const items = [];
        for (const w of results) {
            const vid = extractVid(w);
            if (!vid) continue;
            items.push({
                id: String(w.id || ''),
                imageId: vid,
                title:  getTitle(w),
                artist: getArtist(w),
                date:   getDate(w),
            });
        }
        const total = Number(d.totalResults || 0);
        return { items, total };
    }

    thumbnailUrl(imageId, size = 64) {
        return `${IIIF_HOST}/${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }

    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }
}
```

- [ ] **Step 2: Smoke-load the module from a desktop browser**

Open a DevTools console on any modern browser pointed at a recent revision of the project, paste in the module text, and run:

```javascript
const a = new (await import('data:text/javascript;base64,' + btoa(/* paste file contents here */))).WellcomeAdapter();
await a.listCollections({ axis: 'workType' });
```

Expected: an array of `{id,label,count}` entries with `id` values like `k`, `q`, `h`, `r`, `hdig` and `count` > 0. This is a sanity check that the URL shape is correct, not a regression test — manual.

- [ ] **Step 3: Commit only if smoke-loading worked**

Don't commit yet — Task 4 registers the module; we commit them together so the registry never references a missing file.

---

## Task 4: Register `WellcomeAdapter` in `webui/museum/index.js`

**Files:**
- Modify: `webui/museum/index.js`

- [ ] **Step 1: Add import + registry entry**

Open `webui/museum/index.js`. The current imports are:

```javascript
import { ArticAdapter } from './artic.js';
import { RijksmuseumAdapter } from './rijksmuseum.js';
import { VamAdapter } from './vam.js';
```

Append after the V&A import:

```javascript
import { WellcomeAdapter } from './wellcome.js';
```

The current `ADAPTERS` array is:

```javascript
const ADAPTERS = [
    new ArticAdapter(),
    new RijksmuseumAdapter(),
    new VamAdapter(),
];
```

Append after `new VamAdapter()`:

```javascript
    new WellcomeAdapter(),
```

So the final array reads:

```javascript
const ADAPTERS = [
    new ArticAdapter(),
    new RijksmuseumAdapter(),
    new VamAdapter(),
    new WellcomeAdapter(),
];
```

- [ ] **Step 2: Commit Wellcome JS adapter + registration together**

```bash
git add webui/museum/wellcome.js webui/museum/index.js
git commit -m "$(cat <<'EOF'
feat(webui/museum): add Wellcome Collection browse adapter (M4)

Single aggregations call enumerates all four axes (workType, genres,
subjects, contributors) in one round-trip. Terms whose label > 32
chars are filtered from browse to fit the playset identifier[33]
slot; decision documented in docs/deferred/wellcome-long-labels.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Wellcome deferred-decisions doc

**Files:**
- Create: `docs/deferred/wellcome-long-labels.md`

(The `docs/deferred/README.md` and `gallica.md` are written in M6; we seed only the Wellcome entry here since that's the deferral this milestone is paying interest on.)

- [ ] **Step 1: Create the file**

```markdown
# Wellcome long-label terms

**Status:** Deferred (M4 — 2026-05-12).
**Scope:** Wellcome facet terms whose label is longer than 32 characters.

## What was deferred

Wellcome facet terms whose `label.length > 32` are hidden from the
museum browse modal. This applies to all four Wellcome axes
(`workType`, `genres`, `subjects`, `contributors`), but only matters
for the latter three: `workType` exposes a stable short id (`k`, `q`,
…) regardless of label length, and its labels are short anyway.

## Why

The playset binary format's channel `identifier` field is `char[33]`
— 32 characters plus a null terminator. For non-`workType` Wellcome
axes the catalogue API does not expose a stable short id; the term
label IS the filter value used on `genres.label=`, `subjects.label=`,
or `contributors.agent.label=`. Storing the term in the playset
identifier therefore caps the label at 32 characters.

The cheapest correct behavior is to hide longer-than-32-char terms
from the browse modal. The user never sees them, the device never
stores them. No silent truncation, no risk of two long labels sharing
a 32-char prefix colliding.

## Estimated impact

Small but non-zero. Most Wellcome subject and genre labels are well
under 32 chars (e.g. `Botany`, `Engraving and Engravings`,
`Portrait prints`). The hide rule mostly affects long-tail
contributor labels with affiliation strings appended (e.g.
`David Gregory & Debbie Marshall` — 31 chars, fits; some institutional
contributors with longer names do not).

## Revisit when

Field usage shows enough valuable Wellcome terms hidden to justify
expanding the playset `identifier` slot. The change touches:

- `components/play_scheduler/include/play_scheduler_types.h` —
  two `identifier[33]` members.
- `components/play_scheduler/include/playset_store.h` — the on-disk
  binary format (P3PS magic, would require v11 → v12).
- `components/play_scheduler/playset_store.c` — the backward-compat
  loader for v11 playsets.
- `components/play_scheduler/playset_json.c` — JSON serializer.
- `components/play_scheduler/play_scheduler.c` —
  `ps_compute_channel_id()` hash input width.

Not a small change; defer until measurement justifies it.
```

- [ ] **Step 2: Commit**

```bash
git add docs/deferred/wellcome-long-labels.md
git commit -m "$(cat <<'EOF'
docs(deferred): note Wellcome long-label gate decision

First entry in the new docs/deferred/ folder (README + gallica entry
land in M6). Captures why Wellcome terms with label > 32 chars are
hidden from browse, and what revisiting the decision would require.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: M4 manual gate — Wellcome end-to-end

This task is procedure, not code. Walk the user through the gate; do not skip ahead to Phase 2 (SMK) until the gate clears.

- [ ] **Step 1: Ask the user to flash and monitor**

```powershell
idf.py flash monitor
```

Watch for `art_institution initialized (4 museum(s))` in the boot log (count went from 3 to 4). If the count is wrong, the dispatch table change didn't land.

- [ ] **Step 2: Browser smoke test**

On a Wi-Fi-connected device, open the web UI in a desktop browser:

1. Open the playset editor.
2. Click "Add channel" → Channel Type = Museum.
3. The museum picker should now list 4 museums; "Wellcome Collection" with shortName "Wellcome" should appear last.
4. Pick "Wellcome Collection".
5. The axis picker should show 4 entries: Work types, Genres, Subjects, Contributors.
6. Pick "Genres". The term list should populate with image-bearing genres sorted by count desc (e.g. `Engraving and Engravings`, `Etching`, `Portrait prints`, …). Confirm no entry's label exceeds 32 characters.
7. Pick a term with high count (e.g. `Engraving and Engravings`).
8. Single-artwork preview should render at IIIF 400×400. Caption shows title/artist/date.
9. Click `Next →`. Image swaps, counter advances. Repeat a few times.
10. Click `Add channel`. Modal closes; the playset editor shows a new channel row labelled `Wellcome · Engraving and Engravings` (or similar — truncated at 64 chars).
11. Save the playset.

If any step fails, capture the browser console error and abort the gate.

- [ ] **Step 3: Device-side refresh check**

Activate the playset and monitor the serial console:

```
I (xxxxx) art_inst: ...                                       (component init log)
I (xxxxx) ai_wellcome: Fetching page 1 (filter=genres.label value=Engraving and Engravings)
I (xxxxx) ai_wellcome: Wellcome page 1: parsed 100/100 entries (totalResults=10967), has_more=1
I (xxxxx) ai_wellcome: Wellcome page 1 merged: 100 entries (total 100)
I (xxxxx) ai_wellcome: Fetching page 2 (filter=genres.label value=Engraving and Engravings)
I (xxxxx) ai_wellcome: Wellcome page 2: parsed 100/100 entries (totalResults=10967), has_more=1
...
I (xxxxx) ai_wellcome: Wellcome refresh complete for 'CHANNELID': 1024 fetched
```

Confirm: no 429s under normal load, JSON parse succeeds, `totalResults` matches what the browser saw, refresh completes when `total_fetched == cache_size` (default 1024).

- [ ] **Step 4: Download / playback check**

After the refresh logs an "Wellcome refresh complete" line, the download manager picks up entries one at a time. Watch for `download_to_path` logs landing files under `/sdcard/p3a/museum/wellcome/...`. The picker should start rotating the channel within ~30 s of the first successful download.

- [ ] **Step 5: 24-hour soak**

Leave the device running with at least:

- One Wellcome workType=Pictures channel, AND
- One Wellcome genres=Engraving channel,

plus the existing M2 soak playset (AIC + Rijks + V&A) if you want broader coverage. After 24 h, confirm:

- Picker still rotates across all channels (no starvation).
- No new 429 floods in the logs.
- `refresh complete` messages appear ~once per `ai_refresh_sec` window for each Wellcome channel.
- Free SD space hasn't dropped pathologically (storage_eviction should keep it bounded).

- [ ] **Step 6: M4 done — proceed to Phase 2 only if soak passes**

If soak surfaces issues, file them as follow-ups inside the spec's §15 (field-observed fixes) of the parent design or as new tickets, then return to Phase 1 to address the most blocking ones before starting SMK.

---

# Phase 2 — M5: SMK end-to-end

## Task 7: SMK scaffolding (enum, prototypes, CMakeLists, dispatch entry)

**Files:**
- Modify: `components/art_institution/include/art_institution_types.h`
- Modify: `components/art_institution/art_institution_internal.h`
- Modify: `components/art_institution/CMakeLists.txt`
- Modify: `components/art_institution/art_institution.c`

- [ ] **Step 1: Append `ART_INSTITUTION_MUSEUM_SMK` to the enum**

Edit `components/art_institution/include/art_institution_types.h`. After Task 1 the enum reads:

```c
typedef enum {
    ART_INSTITUTION_MUSEUM_ARTIC    = 0,
    ART_INSTITUTION_MUSEUM_RIJKS    = 1,
    ART_INSTITUTION_MUSEUM_VAM      = 2,
    ART_INSTITUTION_MUSEUM_WELLCOME = 3,
    ART_INSTITUTION_NUM_MUSEUMS  // sentinel; ...
} museum_id_t;
```

Append `ART_INSTITUTION_MUSEUM_SMK = 4` so it reads:

```c
typedef enum {
    ART_INSTITUTION_MUSEUM_ARTIC    = 0,
    ART_INSTITUTION_MUSEUM_RIJKS    = 1,
    ART_INSTITUTION_MUSEUM_VAM      = 2,
    ART_INSTITUTION_MUSEUM_WELLCOME = 3,
    ART_INSTITUTION_MUSEUM_SMK      = 4,
    ART_INSTITUTION_NUM_MUSEUMS  // sentinel; ...
} museum_id_t;
```

- [ ] **Step 2: Append SMK internal prototypes**

Edit `components/art_institution/art_institution_internal.h`. After the Wellcome block, append:

```c
// ============================================================================
// SMK adapter entry points (defined in museums/smk.c)
// ============================================================================

esp_err_t art_institution_smk_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id);

esp_err_t art_institution_smk_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len);
```

- [ ] **Step 3: Add `museums/smk.c` to the build**

Edit `components/art_institution/CMakeLists.txt`. After `"museums/wellcome.c"`:

```cmake
        "museums/wellcome.c"
        "museums/smk.c"
```

- [ ] **Step 4: Append SMK to the dispatch table**

Edit `components/art_institution/art_institution.c`. After the Wellcome entry's closing `},` insert:

```c
    {
        .id              = "smk",
        .display         = "Statens Museum for Kunst",
        .museum_enum     = ART_INSTITUTION_MUSEUM_SMK,
        .refresh_channel = art_institution_smk_refresh_channel,
        .build_iiif_url  = art_institution_smk_build_iiif_url,
        .resolve_entry   = NULL,  // SMK returns image_iiif_id inline; no walk.
    },
```

- [ ] **Step 5: Do not build/commit yet**

Task 8 produces `museums/smk.c`; commit after that.

---

## Task 8: SMK C adapter (`museums/smk.c`)

**Files:**
- Create: `components/art_institution/museums/smk.c`

Mirrors `museums/vam.c` structure. SMK-specific differences:

- API base `https://api.smk.dk/api/v1`; IIIF prefix `https://iip.smk.dk/iiif/jp2/`.
- One axis (`collection`); the term value is the collection name string (may contain spaces and Danish characters).
- Listing endpoint uses `filters=[collection:NAME],[has_image:true]` — a single string parameter that contains brackets/colons/commas; we build the unencoded filter expression, then `ai_url_encode` the entire string once.
- Results array is `items` (vs V&A's `records`); each item has `image_iiif_id` as a **full URL** (e.g. `https://iip.smk.dk/iiif/jp2/bc386p50w_kksgb22235.tif.jp2`). The stored `iiif_key` is the filename only — everything after the last `/jp2/`.
- Pagination metadata: `found` field. Native `offset+rows` — no page-from-offset math.
- 192 KB response buffer.

- [ ] **Step 1: Create the file**

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/smk.c
 * @brief SMK (Statens Museum for Kunst) adapter — refresh + IIIF URL build.
 *
 * SMK's search API returns results with `image_iiif_id` as a full URL
 * (e.g. `https://iip.smk.dk/iiif/jp2/bc386p50w_kksgb22235.tif.jp2`).
 * We store only the filename — everything after the last `/jp2/` — as
 * the iiif_key, and prepend the standard prefix at URL-build time.
 *
 * Single axis (`collection`). Filter syntax is
 * `filters=[collection:NAME],[has_image:true]`; the whole expression is
 * one query-value, so we build it unencoded and percent-encode the
 * entire string once.
 *
 * Note on JPEG vs WebP: SMK's IIPImage backend advertises WebP in
 * info.json but returns HTTP 400 for `.webp` requests (captured in
 * reference/museum-art/source/smk/output/report.md). Stay on JPEG.
 *
 * Reference: https://api.smk.dk/api/v1/docs/ and
 * reference/museum-art/source/smk/run.py
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "art_institution_types.h"
#include "channel_cache.h"
#include "channel_metadata.h"
#include "config_store.h"
#include "psram_alloc.h"
#include "sd_path.h"
#include "sntp_sync.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ai_smk";

#define SMK_API_BASE           "https://api.smk.dk/api/v1"
#define SMK_IIIF_PREFIX        "https://iip.smk.dk/iiif/jp2/"
#define SMK_PAGE_LIMIT         100
#define SMK_RESPONSE_BUF_SIZE  (192 * 1024)
#define SMK_FETCH_MAX_ATTEMPTS 3

static const uint32_t s_fetch_backoff_ms[SMK_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

extern void download_manager_rescan(void);

// ----- IIIF URL -----------------------------------------------------------

esp_err_t art_institution_smk_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, SMK_IIIF_PREFIX "%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- Helpers ------------------------------------------------------------

static int drain_body(esp_http_client_handle_t client, char *buf, size_t buf_size)
{
    int total = 0;
    bool read_err = false;
    while (total < (int)buf_size - 1) {
        int n = esp_http_client_read(client, buf + total, buf_size - 1 - total);
        if (n < 0) { read_err = true; break; }
        if (n == 0) break;
        total += n;
    }
    return read_err ? -1 : total;
}

static uint32_t parse_retry_after(const char *value)
{
    if (!value) return 0;
    while (*value == ' ') value++;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || v <= 0) return 0;
    if (v > 3600) v = 3600;
    return (uint32_t)v;
}

/**
 * Extract the IIIF filename from an SMK image_iiif_id URL.
 *
 * Verifies the URL contains `/iiif/jp2/`; stores the substring after the
 * last `/jp2/` as the iiif_key (e.g. `bc386p50w_kksgb22235.tif.jp2`).
 * Returns false if the URL doesn't match the expected shape or the
 * extracted filename overflows out_len.
 */
static bool extract_smk_filename(const char *image_iiif_id, char *out, size_t out_len)
{
    if (!image_iiif_id || !out || out_len == 0) return false;
    static const char marker[] = "/iiif/jp2/";
    const size_t marker_len = sizeof(marker) - 1;

    // Find the LAST occurrence of /iiif/jp2/ — defensive against URLs
    // that contain the marker as a path segment more than once.
    const char *cursor = image_iiif_id;
    const char *last = NULL;
    while ((cursor = strstr(cursor, marker)) != NULL) {
        last = cursor;
        cursor += marker_len;
    }
    if (!last) return false;

    const char *filename = last + marker_len;
    size_t filename_len = strlen(filename);
    if (filename_len == 0 || filename_len >= out_len) return false;

    memcpy(out, filename, filename_len + 1);
    return true;
}

// ----- One-page fetch -----------------------------------------------------

static esp_err_t smk_fetch_page(const char *collection_name,
                                int offset,
                                char *response_buf,
                                size_t response_buf_size,
                                institution_channel_entry_t *out_entries,
                                size_t max_entries,
                                size_t *out_count,
                                int *out_found,
                                bool *has_more)
{
    *out_count = 0;
    *has_more = false;
    if (out_found) *out_found = 0;

    // Build the unencoded filter expression, then percent-encode the
    // whole thing once. SMK's filters syntax embeds brackets/colons/commas
    // that must be encoded; ai_url_encode encodes everything outside the
    // RFC 3986 unreserved set, which is exactly what we want here.
    char filter_expr[384];
    int fwrote = snprintf(filter_expr, sizeof(filter_expr),
                          "[collection:%s],[has_image:true]", collection_name);
    if (fwrote < 0 || fwrote >= (int)sizeof(filter_expr)) {
        ESP_LOGE(TAG, "Filter expression overflow for collection='%s'", collection_name);
        return ESP_FAIL;
    }

    char encoded_filter[1024];  // 3x expansion worst case
    ai_url_encode(filter_expr, encoded_filter, sizeof(encoded_filter));

    char url[1280];
    int wrote = snprintf(url, sizeof(url),
                         SMK_API_BASE
                         "/art/search?keys=*&offset=%d&rows=%d&filters=%s",
                         offset, SMK_PAGE_LIMIT, encoded_filter);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for collection='%s'", collection_name);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching offset %d (collection=%.32s)", offset, collection_name);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < SMK_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retrying SMK page fetch in %lums (attempt %d/%d)",
                     (unsigned long)s_fetch_backoff_ms[attempt],
                     attempt + 1, SMK_FETCH_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
            total_read = 0;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        esp_http_client_set_header(client, "Accept", "application/json");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            continue;
        }

        esp_http_client_fetch_headers(client);
        int64_t content_length = esp_http_client_get_content_length(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 429) {
            char *retry_after = NULL;
            uint32_t cooldown = 0;
            if (esp_http_client_get_header(client, "Retry-After", &retry_after) == ESP_OK) {
                cooldown = parse_retry_after(retry_after);
            }
            art_institution_set_rate_limited("smk", cooldown);
            ESP_LOGW(TAG, "SMK returned 429 (cooldown %us)",
                     (unsigned)(cooldown ? cooldown : 60));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (status == 401 || status == 403) {
            ESP_LOGW(TAG, "SMK returned %d at offset %d", status, offset);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "SMK status %d at offset %d", status, offset);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        total_read = drain_body(client, response_buf, response_buf_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (total_read < 0) continue;
        if (total_read == 0) continue;
        if (total_read >= (int)response_buf_size - 1) {
            ESP_LOGE(TAG, "SMK response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        if (content_length > 0 && total_read < (int)content_length) {
            ESP_LOGW(TAG, "SMK truncated: got %d/%lld bytes",
                     total_read, (long long)content_length);
            continue;
        }
        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) {
        ESP_LOGE(TAG, "SMK offset %d fetch failed after %d attempts",
                 offset, SMK_FETCH_MAX_ATTEMPTS);
        return ESP_FAIL;
    }
    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "SMK JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        ESP_LOGE(TAG, "SMK response missing 'items' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int found = 0;
    const cJSON *found_node = cJSON_GetObjectItem(root, "found");
    if (cJSON_IsNumber(found_node)) found = (int)cJSON_GetNumberValue(found_node);
    if (out_found) *out_found = found;

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(items);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) continue;

        const cJSON *iiif_id = cJSON_GetObjectItem(item, "image_iiif_id");
        if (!cJSON_IsString(iiif_id) || !iiif_id->valuestring[0]) continue;

        char filename[sizeof(out_entries[0].iiif_key)] = {0};
        if (!extract_smk_filename(iiif_id->valuestring, filename, sizeof(filename))) {
            // Either the URL doesn't contain /iiif/jp2/ or the filename
            // exceeds the 48-byte iiif_key slot; skip defensively.
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("smk", filename);
        e->kind = 0;
        e->extension = 3;
        e->created_at = now;
        strlcpy(e->iiif_key, filename, sizeof(e->iiif_key));
        parsed++;
    }

    int seen_so_far = offset + array_size;
    *has_more = (array_size > 0) && (seen_so_far < found);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "SMK offset %d: parsed %zu/%d entries (found=%d), has_more=%d",
             offset, parsed, array_size, found, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_smk_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id)
{
    (void)axis;  // SMK has one axis (collection); axis is informational here.
    if (!channel_id || !term_id || term_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (art_institution_is_rate_limited("smk")) {
        ESP_LOGW(TAG, "SMK rate-limited at refresh start, skipping");
        return ESP_ERR_INVALID_RESPONSE;
    }

    channel_cache_lifecycle_lock();
    bool cache_exists = (channel_cache_registry_find(channel_id) != NULL);
    channel_cache_lifecycle_unlock();
    if (!cache_exists) {
        ESP_LOGW(TAG, "Cache not found for channel '%s'", channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t cache_size = config_store_get_ai_cache_size();
    if (cache_size == 0) cache_size = 1024;
    if (cache_size > 4096) cache_size = 4096;

    char *response_buf = heap_caps_malloc(SMK_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(SMK_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        SMK_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(SMK_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;
    int offset = 0;
    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        int found = 0;
        bool has_more = false;
        esp_err_t err = smk_fetch_page(term_id, offset,
                                       response_buf, SMK_RESPONSE_BUF_SIZE,
                                       page_entries, SMK_PAGE_LIMIT,
                                       &page_count, &found, &has_more);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }
        if (page_count == 0 && !has_more) {
            ESP_LOGI(TAG, "No entries at offset %d, done", offset);
            break;
        }

        if (page_count > 0) {
            size_t merge_limit = cache_size * 3;
            channel_cache_lifecycle_lock();
            channel_cache_t *page_cache = channel_cache_registry_find(channel_id);
            esp_err_t merge_err = page_cache
                ? art_institution_merge_entries(page_cache, page_entries, page_count, merge_limit)
                : ESP_ERR_NOT_FOUND;
            channel_cache_lifecycle_unlock();

            if (merge_err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Cache disappeared mid-refresh for '%s'", channel_id);
                refresh_completed = false;
                break;
            }
            if (merge_err != ESP_OK) {
                ESP_LOGW(TAG, "Merge failed at offset %d: %s", offset, esp_err_to_name(merge_err));
                refresh_completed = false;
                break;
            }

            for (size_t i = 0; i < page_count && si_count < cache_size; i++) {
                int32_t pid = page_entries[i].post_id;
                ai_si_node_t *existing = NULL;
                HASH_FIND_INT(si_hash, &pid, existing);
                if (!existing) {
                    ai_si_node_t *n = psram_malloc(sizeof(ai_si_node_t));
                    if (n) {
                        n->post_id = pid;
                        HASH_ADD_INT(si_hash, post_id, n);
                        si_count++;
                    }
                }
            }

            total_fetched += page_count;
            ESP_LOGI(TAG, "SMK offset %d merged: %zu entries (total %zu)",
                     offset, page_count, total_fetched);
            download_manager_rescan();
        }

        offset += SMK_PAGE_LIMIT;
        if (!has_more) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    free(page_entries);

    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "smk");
        }
        channel_cache_lifecycle_unlock();
    }

    {
        ai_si_node_t *node, *tmp;
        HASH_ITER(hh, si_hash, node, tmp) {
            HASH_DEL(si_hash, node);
            free(node);
        }
    }

    if ((refresh_completed || partial_with_content) && sntp_sync_is_synchronized()) {
        char channels_path[128];
        if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
            strlcpy(channels_path, "/sdcard/p3a/channel", sizeof(channels_path));
        }
        channel_metadata_t meta = { .last_refresh = time(NULL), .cursor = "" };
        esp_err_t meta_err = channel_metadata_save(channel_id, channels_path, &meta);
        if (meta_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save channel metadata: %s", esp_err_to_name(meta_err));
        }
    }

    if (refresh_completed) {
        ESP_LOGI(TAG, "SMK refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "SMK refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "SMK refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
```

- [ ] **Step 2: Verify TLS cert bundle (pre-merge gate)**

Verify that `esp_crt_bundle` covers `api.smk.dk` and `iip.smk.dk`. Same procedure as Task 2 Step 2. If absent, document the explicit attachment workaround in a comment.

- [ ] **Step 3: Ask the user to build**

Ask the user to run `idf.py build`. Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add components/art_institution/include/art_institution_types.h \
        components/art_institution/art_institution_internal.h \
        components/art_institution/CMakeLists.txt \
        components/art_institution/art_institution.c \
        components/art_institution/museums/smk.c
git commit -m "$(cat <<'EOF'
feat(art-institution): add SMK adapter (M5)

Single axis (collection); native offset+rows pagination. Stores the
JP2 filename only (everything after /iiif/jp2/) as the iiif_key and
prepends the standard prefix at URL-build time. Append-only enum
value and dispatch entry; no format break.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: SMK JS adapter (`webui/museum/smk.js`)

**Files:**
- Create: `webui/museum/smk.js`

SMK-specific differences from V&A:

- One axis only (`collection`); the modal auto-advances past the axis step (existing behavior).
- Term enumeration uses `?keys=*&rows=0&facets=collection`. SMK can return the facet pairs in three shapes (alternating array, list of pairs, dict); the adapter handles all three to match the Python reference's defensive parsing.
- Listing endpoint takes a single composite `filters=` parameter; the adapter builds the filter expression and lets `URLSearchParams` encode it.
- Image id (`imageId` for the modal's preview path) is the JP2 filename extracted from the result's `image_iiif_id` URL.

- [ ] **Step 1: Create the file**

```javascript
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// SMK (Statens Museum for Kunst) browse adapter.
//
// One axis (`collection`). The modal auto-advances past the axis step.
// SMK's facets API can return the collection pairs in three shapes
// (alternating array, list of pairs, dict); the adapter handles all
// three to match the Python reference's defensive parsing.
//
// IIIF id is the JP2 filename — everything after `/iiif/jp2/` in
// `image_iiif_id`. The C-side build_iiif_url prepends the standard
// host prefix at refresh / download time.

const SEARCH = 'https://api.smk.dk/api/v1/art/search';
const IIIF_PREFIX = 'https://iip.smk.dk/iiif/jp2';

const MAX_LABEL_CHARS = 32;
const PAGE_SIZE = 100;

async function getJson(url) {
    const r = await fetch(url);
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'smk',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`SMK 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`SMK ${r.status} ${url}`);
    return r.json();
}

function parseFacetPairs(raw) {
    // SMK returns facets in one of three shapes; mirror Python's
    // _parse_facet_pairs (reference/museum-art/source/smk/run.py).
    const out = [];
    if (raw && typeof raw === 'object' && !Array.isArray(raw)) {
        for (const k of Object.keys(raw)) {
            out.push([String(k), Number(raw[k] || 0)]);
        }
        return out;
    }
    if (!Array.isArray(raw) || raw.length === 0) return out;
    const head = raw[0];
    if ((typeof head === 'string' || typeof head === 'number') && raw.length % 2 === 0) {
        for (let i = 0; i < raw.length; i += 2) {
            out.push([String(raw[i]), Number(raw[i + 1] || 0)]);
        }
        return out;
    }
    for (const entry of raw) {
        if (entry && typeof entry === 'object' && !Array.isArray(entry)) {
            const name = entry.name || entry.value || entry.key;
            const count = entry.count || entry.doc_count || 0;
            if (name != null) out.push([String(name), Number(count || 0)]);
        } else if (Array.isArray(entry) && entry.length === 2) {
            out.push([String(entry[0]), Number(entry[1] || 0)]);
        }
    }
    return out;
}

function extractFilename(imageIiifId) {
    if (typeof imageIiifId !== 'string') return null;
    const marker = '/iiif/jp2/';
    const idx = imageIiifId.lastIndexOf(marker);
    if (idx < 0) return null;
    const filename = imageIiifId.slice(idx + marker.length);
    if (!filename) return null;
    return filename;
}

function getTitle(item) {
    const titles = item.titles;
    if (Array.isArray(titles)) {
        for (const t of titles) {
            if (t && typeof t === 'object' && t.title) return String(t.title);
            if (typeof t === 'string' && t) return t;
        }
    } else if (typeof titles === 'string') {
        return titles;
    }
    return '(untitled)';
}

function getArtist(item) {
    const prod = item.production;
    if (Array.isArray(prod)) {
        for (const p of prod) {
            if (p && typeof p === 'object') {
                const c = p.creator || p.creator_forename || p.creator_surname;
                if (c) return String(c);
            }
        }
    }
    return '';
}

function getDate(item) {
    const prod = item.production;
    if (Array.isArray(prod) && prod.length > 0) {
        const p = prod[0];
        if (p) {
            const d = p.creation_date_text || p.creation_date || p.date_of_birth;
            if (d) return String(d);
        }
    }
    return '';
}

export class SmkAdapter {
    get id()          { return 'smk'; }
    get displayName() { return 'Statens Museum for Kunst'; }
    get shortName()   { return 'SMK'; }
    get axes() {
        return [{ name: 'collection', label: 'Collections' }];
    }

    constructor() {
        this._terms = null;  // cached per session
    }

    async listCollections({ axis = 'collection' } = {}) {
        if (axis !== 'collection') throw new Error(`SMK: unknown axis ${axis}`);
        if (this._terms) return this._terms;

        const params = new URLSearchParams({
            keys: '*',
            rows: '0',
            facets: 'collection',
        });
        const data = await getJson(`${SEARCH}?${params}`);
        const raw = (data && data.facets && data.facets.collection) || [];
        const pairs = parseFacetPairs(raw);

        const out = [];
        for (const [name, count] of pairs) {
            if (!name) continue;
            if (count <= 0) continue;
            if (name.length > MAX_LABEL_CHARS) continue;  // identifier[33] gate
            out.push({ id: name, label: name, count });
        }
        out.sort((a, b) => b.count - a.count);
        this._terms = out;
        return out;
    }

    async listArtworks(termId, { offset = 0, rows = PAGE_SIZE, axis = 'collection' } = {}) {
        if (axis !== 'collection') throw new Error(`SMK: unknown axis ${axis}`);
        const params = new URLSearchParams({
            keys: '*',
            offset: String(offset),
            rows: String(rows),
            filters: `[collection:${termId}],[has_image:true]`,
        });
        const d = await getJson(`${SEARCH}?${params}`);
        const records = Array.isArray(d.items) ? d.items : [];
        const items = [];
        for (const r of records) {
            const filename = extractFilename(r && r.image_iiif_id);
            if (!filename) continue;
            items.push({
                id: String(r.object_number || r.id || ''),
                imageId: filename,
                title:  getTitle(r),
                artist: getArtist(r),
                date:   getDate(r),
            });
        }
        const total = Number(d.found || 0);
        return { items, total };
    }

    thumbnailUrl(imageId, size = 64) {
        return `${IIIF_PREFIX}/${encodeURIComponent(imageId)}/full/!${size},${size}/0/default.jpg`;
    }

    async previewUrl(item, size = 400) {
        if (!item || !item.imageId) return null;
        return this.thumbnailUrl(item.imageId, size);
    }
}
```

- [ ] **Step 2: Hold the commit for Task 10**

---

## Task 10: Register `SmkAdapter` in `webui/museum/index.js`

**Files:**
- Modify: `webui/museum/index.js`

- [ ] **Step 1: Add import + registry entry**

After the Wellcome import added in Task 4, append:

```javascript
import { SmkAdapter } from './smk.js';
```

After `new WellcomeAdapter()` in the `ADAPTERS` array, append:

```javascript
    new SmkAdapter(),
```

So the final array reads:

```javascript
const ADAPTERS = [
    new ArticAdapter(),
    new RijksmuseumAdapter(),
    new VamAdapter(),
    new WellcomeAdapter(),
    new SmkAdapter(),
];
```

- [ ] **Step 2: Commit SMK JS adapter + registration together**

```bash
git add webui/museum/smk.js webui/museum/index.js
git commit -m "$(cat <<'EOF'
feat(webui/museum): add SMK browse adapter (M5)

Single axis (collection); defensive facet-pairs parsing matches the
Python reference's three-shape handler. Hides terms with label > 32
chars to fit the playset identifier[33] slot.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: M5 manual gate — SMK end-to-end

Same procedure as Task 6, applied to SMK.

- [ ] **Step 1: Ask the user to flash and monitor**

```powershell
idf.py flash monitor
```

Watch for `art_institution initialized (5 museum(s))` at boot (count went from 4 to 5).

- [ ] **Step 2: Browser smoke test**

1. Open the playset editor → Add channel → Museum.
2. The picker should list 5 museums; "Statens Museum for Kunst" with shortName "SMK" should appear last.
3. Pick SMK. The modal should auto-advance past the axis step (one axis only) to the term list.
4. Term list should populate with collections sorted by count desc — e.g. `Gammel bestand` (~13,354), `Niels Larsen Stevns samling` (~2,363), `Richard Mortensens samling` (~1,980). Confirm no entry's label exceeds 32 chars.
5. Pick a term (e.g. `Gammel bestand`).
6. Single-artwork preview renders at IIIF 400×400. Caption shows title/artist/date.
7. `Next →` advances through items.
8. `Add channel`. Channel row reads `SMK · Gammel bestand`.
9. Save the playset.

- [ ] **Step 3: Device-side refresh check**

Watch the serial console:

```
I (xxxxx) ai_smk: Fetching offset 0 (collection=Gammel bestand)
I (xxxxx) ai_smk: SMK offset 0: parsed 100/100 entries (found=9121), has_more=1
I (xxxxx) ai_smk: SMK offset 0 merged: 100 entries (total 100)
I (xxxxx) ai_smk: Fetching offset 100 (collection=Gammel bestand)
...
I (xxxxx) ai_smk: SMK refresh complete for 'CHANNELID': 1024 fetched
```

(`found` may differ slightly from what the browser saw due to the `has_image:true` filter — that's expected.)

- [ ] **Step 4: Download / playback check**

Watch for files landing under `/sdcard/p3a/museum/smk/...`. The picker should rotate to SMK entries within ~30 s of the first download.

- [ ] **Step 5: 24-hour soak**

Append one SMK collection=Gammel-bestand channel to the existing M4 soak playset. Run 24 h; same checklist as Task 6 Step 5.

- [ ] **Step 6: M5 done — proceed to Phase 3 only if soak passes**

---

# Phase 3 — M6: Release polish

## Task 12: Settings page copy update

**Files:**
- Modify: `webui/settings.html` (the "Museums" section copy)

- [ ] **Step 1: Locate the Museums section**

Open `webui/settings.html`. Find the "Museums" section's settings-hint paragraph (the one that mentions AIC's 60-req/min limit). It is currently phrased around three museums (AIC, Rijksmuseum, V&A).

- [ ] **Step 2: Update the copy to name all five museums**

Update the section so it names the five museums and reframes the rate-limit guidance to reflect that only AIC publishes a hard limit. Replace the existing hint paragraph with text in the spirit of:

> Museum channels pull artworks from five sources: the Art Institute of
> Chicago (AIC), Rijksmuseum, the Victoria and Albert Museum (V&A),
> Wellcome Collection, and Statens Museum for Kunst (SMK). AIC enforces
> a hard rate limit of 60 requests per minute per IP — running many AIC
> channels in parallel can hit it. The other four museums don't publish
> a rate limit; p3a still applies a 60 s defensive cooldown if any of
> them returns a 429. `Refresh interval` controls how often the
> dispatcher walks each museum's listing API; `Cache size` controls how
> many artworks per channel are stored locally.

Adjust to match the existing tone and surrounding markup. Don't change controls — only the copy.

- [ ] **Step 3: Hold the commit for the M6 batch.**

---

## Task 13: HOW-TO-USE.md update

**Files:**
- Modify: `docs/HOW-TO-USE.md`

- [ ] **Step 1: Find the Museum channel section**

Open `docs/HOW-TO-USE.md`. Find the section that introduces Museum channels (added in M1/M2). It currently describes AIC, Rijksmuseum, and V&A.

- [ ] **Step 2: Extend the list of supported museums**

Add Wellcome Collection and SMK to the list, mirroring the format used for AIC/Rijks/V&A. Sample additions to keep tone consistent:

> - **Wellcome Collection.** Library/archive catalogue with four facet
>   axes (work types, genres, subjects, contributors). Image-bearing
>   works only.
> - **Statens Museum for Kunst (SMK).** Danish national gallery, one
>   facet axis (collection). Image-bearing works only.

Mention the long-label gate briefly in passing, with a pointer to
`docs/deferred/wellcome-long-labels.md` for the rationale.

- [ ] **Step 3: Hold the commit for the M6 batch.**

---

## Task 14: `docs/deferred/README.md` + `gallica.md`

**Files:**
- Create: `docs/deferred/README.md`
- Create: `docs/deferred/gallica.md`

- [ ] **Step 1: Create `docs/deferred/README.md`**

```markdown
# Deferred design decisions

Decisions we considered, intentionally didn't ship, and want to revisit.
Each entry names: what was deferred, why now isn't the right time, what
would change that.

- [Gallica integration](gallica.md) — XML/SRU parser dependency.
- [Wellcome long-label terms](wellcome-long-labels.md) — terms whose
  label exceeds the playset 32-char identifier limit.
```

- [ ] **Step 2: Create `docs/deferred/gallica.md`**

```markdown
# Gallica (BnF) integration

**Status:** Deferred (M6 — 2026-05-12).
**Scope:** Adding BnF Gallica as a sixth museum channel.

## What was deferred

Implementing a Gallica adapter alongside the existing five museums
(AIC, Rijksmuseum, V&A, Wellcome Collection, SMK).

## Why

Gallica's catalogue API is SRU (Search/Retrieve via URL) and returns
XML — specifically, an OAI-PMH-flavored Dublin Core record schema
wrapped in an SRU response envelope. The rest of the museum surface is
JSON-only; the codebase does not include an XML parser and ESP-IDF
does not ship one. Adding Gallica would require either:

1. Bundling a lightweight XML parser (mini-xml, ezXML, expat). All add
   meaningful binary-size and RAM cost; each needs its own porting and
   review effort to confirm it builds clean against ESP-IDF v5.5.x.
2. Hand-rolling an SRU-specific parser in plain C that extracts the
   fields we need (`numberOfRecords`, `record/recordData`, `creator`,
   `title`, `identifier`, `type`, `date`) from a tag stream.

Either path is multiple days of work that the existing five museums
already cover in image-content terms.

There's also a User-Agent quirk: Gallica returns HTTP 403 to default
`requests` / `curl` fingerprints, so the device would have to send a
browser-like UA string — manageable but worth noting.

## Revisit when

- A lightweight XML parser becomes available in ESP-IDF, or
- Field usage shows a content-diversity gap that Gallica would uniquely
  fill (BnF's manuscripts, maps, sheet-music coverage is strong and not
  duplicated elsewhere in the museum set), or
- A user volume justifies the integration cost.

## Reference materials kept under

`reference/museum-art/source/gallica/` — run.py, report.md, sample
images. The reference run validates the API shape (SRU pagination,
IIIF v1.1 download at 720px longest side) so a future implementer
won't need to rediscover the basics.
```

- [ ] **Step 3: Hold the commit for the M6 batch.**

---

## Task 15: Append §13 entries to `finalized-design.md`

**Files:**
- Modify: `docs/art-institutions/finalized-design.md` (§13 "Future work")

- [ ] **Step 1: Append two new bullets**

Find §13 "Future work" in `docs/art-institutions/finalized-design.md`. After the existing bullets, append:

```markdown
- **Gallica (BnF):** SRU/XML adapter. Revisit trigger: a lightweight
  XML parser becomes available in ESP-IDF, or content-diversity value
  justifies the integration cost. Deferred design notes in
  [`docs/deferred/gallica.md`](../deferred/gallica.md).
- **Wellcome long labels:** lifting the 32-char identifier limit so
  Wellcome terms with longer labels become selectable. Revisit
  trigger: enough valuable Wellcome terms get hidden in real usage to
  justify a playset format bump. Deferred design notes in
  [`docs/deferred/wellcome-long-labels.md`](../deferred/wellcome-long-labels.md).
```

- [ ] **Step 2: Commit the M6 batch together**

```bash
git add webui/settings.html docs/HOW-TO-USE.md \
        docs/deferred/README.md docs/deferred/gallica.md \
        docs/art-institutions/finalized-design.md
git commit -m "$(cat <<'EOF'
docs: M6 polish for Wellcome + SMK museum channels

- Settings page lists all five museums and clarifies that only AIC
  publishes a hard rate limit.
- HOW-TO-USE adds Wellcome and SMK alongside AIC/Rijks/V&A.
- docs/deferred/ folder seeded with README + gallica entry; the
  Wellcome long-label entry landed in M4.
- finalized-design.md §13 cross-references the two deferred entries.

No version bump (the project is already at 0.10.0 unreleased per the
brainstorming session that produced this plan).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

# Spec self-review (run after the plan is written)

This is a one-time pass after writing the plan. Already done by the plan author; recorded here for transparency.

**1. Spec coverage:**

| Spec section | Task(s) |
|---|---|
| §1 Scope (in / out) | Covered by the M4/M5/M6 task structure and Task 5/14 deferred docs. |
| §2 No format breaks | Verified across Tasks 1 + 7 (append-only enum, no struct changes). |
| §3.1 Wellcome spec | Task 2 (C) + Task 3 (JS). |
| §3.2 SMK spec | Task 8 (C) + Task 9 (JS). |
| §4 C-side implementation | Tasks 1, 2, 7, 8. |
| §4.7 TLS bundle gate | Task 2 Step 2 (Wellcome), Task 8 Step 2 (SMK). |
| §5 Web UI | Tasks 3, 4, 9, 10. |
| §6 Error handling | Inherited from V&A pattern; no per-museum exceptions needed (called out in spec §6 last paragraph). |
| §7 docs/deferred/ folder | Tasks 5 (Wellcome entry), 14 (README + gallica). |
| §8 Testing | Tasks 6, 11. |
| §9 Milestones M4/M5/M6 | Phases 1/2/3. |
| §10 Future-work cross-refs | Task 15. |

**2. Placeholder scan:** No TBDs, no TODOs, no "implement later", no "similar to Task N" without showing code, no "add error handling" placeholders. The settings.html and HOW-TO-USE.md edits in M6 use illustrative copy in the spirit of existing text rather than verbatim drop-in, because the surrounding text isn't shown — that's intentional and documented in the task.

**3. Type consistency:** Function names `art_institution_wellcome_*` and `art_institution_smk_*` used consistently from prototypes (Tasks 1, 7) through dispatch entries (Tasks 1, 7) through definitions (Tasks 2, 8). JS class names `WellcomeAdapter`/`SmkAdapter` consistent across Tasks 3+4 and 9+10. Enum values `ART_INSTITUTION_MUSEUM_WELLCOME = 3` and `ART_INSTITUTION_MUSEUM_SMK = 4` consistent. Wire ids `"wellcome"` and `"smk"` consistent across C and JS.

**4. Ambiguity check:** None spotted. The 32-char gate is enforced both browser-side (Task 3, Task 9) AND defensively re-checked C-side (the C adapters skip overlong vids/filenames via the existing `key_len >= sizeof(...)` defense). The 200 ms inter-page delay matches AIC/V&A. The 256 KB and 192 KB response buffer sizes match the spec.
