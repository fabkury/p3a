# Library of Congress Channel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the Library of Congress as the sixth museum source for p3a's
art-institution channel.

**Architecture:** Single-axis (format) C adapter + JS adapter following the
V&A/Wellcome shape exactly. Two LoC-specific filters at the listing level:
drop entries without an inline `tile.loc.gov/image-services/iiif/` URL, and
drop entries whose extracted IIIF id is 48 chars or longer.

**Tech Stack:** ESP-IDF v5.5.x C component, vanilla ES-module JS, cJSON,
esp_http_client, esp_crt_bundle.

**Spec:** [`loc-channel-design.md`](loc-channel-design.md)
**Investigation:** [`loc-investigation/REPORT.md`](loc-investigation/REPORT.md)

**Build & test:** Per `CLAUDE.md`, the implementing agent must NOT run
`idf.py build` itself — every task that adds C code ends with "ask the user
to run `idf.py build` and confirm a clean build, then commit." The
implementing agent stages the commit but does not run the build.

---

## File structure

**C side:**

- Create: `components/art_institution/museums/loc.c`
- Modify: `components/art_institution/include/art_institution_types.h`
- Modify: `components/art_institution/art_institution_internal.h`
- Modify: `components/art_institution/art_institution.c`
- Modify: `components/art_institution/CMakeLists.txt`

**Web UI:**

- Create: `webui/museum/loc.js`
- Modify: `webui/museum/index.js`

**Documentation:**

- Modify: `docs/art-institutions/finalized-design.md`
- Modify: `docs/HOW-TO-USE.md`

No changes to `play_scheduler_refresh.c`, `playset_store.c`, or
`channel_cache.c` — the existing institution-channel dispatcher routes by
museum id through the dispatch table.

---

## Task 1: Wire LoC into the museum enum

**Files:**
- Modify: `components/art_institution/include/art_institution_types.h:33-44`

- [ ] **Step 1: Read the existing enum block**

Open `components/art_institution/include/art_institution_types.h` and locate
the `museum_id_t` enum. It currently ends:

```c
typedef enum {
    ART_INSTITUTION_MUSEUM_ARTIC    = 0,
    ART_INSTITUTION_MUSEUM_RIJKS    = 1,
    ART_INSTITUTION_MUSEUM_VAM      = 2,
    ART_INSTITUTION_MUSEUM_WELLCOME = 3,
    ART_INSTITUTION_MUSEUM_SMK      = 4,
    ART_INSTITUTION_NUM_MUSEUMS  // sentinel; keep last. ...
} museum_id_t;
```

The header comment notes that ordinals are append-only because they index
the rate-limit table.

- [ ] **Step 2: Append `ART_INSTITUTION_MUSEUM_LOC = 5` before the sentinel**

Replace the SMK / sentinel lines with:

```c
    ART_INSTITUTION_MUSEUM_SMK      = 4,
    ART_INSTITUTION_MUSEUM_LOC      = 5,
    ART_INSTITUTION_NUM_MUSEUMS  // sentinel; keep last. Distinct from the
                                 // extern const ART_INSTITUTION_MUSEUM_COUNT
                                 // (dispatch-table size) — the two must stay
                                 // numerically equal but live in different
                                 // namespaces (enum vs. object).
```

- [ ] **Step 3: No commit yet**

The change is not buildable until the dispatch table is wired in a later
task — keep all of Task 1, 2, 3, 4 in one commit at the end of Task 4.

---

## Task 2: Declare the LoC adapter entry points in the internal header

**Files:**
- Modify: `components/art_institution/art_institution_internal.h:177-188`

- [ ] **Step 1: Append a new declaration block after the SMK section**

After the existing SMK block (ends around line 188), append:

```c
// ============================================================================
// LoC adapter entry points (defined in museums/loc.c)
// ============================================================================

esp_err_t art_institution_loc_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id);

esp_err_t art_institution_loc_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len);
```

- [ ] **Step 2: No commit yet**

Same rationale as Task 1 — wait until Task 4.

---

## Task 3: Create the stub adapter file

**Files:**
- Create: `components/art_institution/museums/loc.c`

- [ ] **Step 1: Write a minimal stub that compiles**

Create `components/art_institution/museums/loc.c` with:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/loc.c
 * @brief Library of Congress adapter — refresh + IIIF URL build.
 *
 * Single axis (`format`). LoC's `/search/` API surfaces a IIIF URL inline
 * on a fraction of results (~8% for photo/print/drawing, ~28% for
 * manuscript/mixed material). Results without an `image-services/iiif`
 * URL are silently dropped at parse time; results whose IIIF id is 48
 * chars or longer are also dropped (see docs/deferred/loc-iiif-key-48-char.md).
 *
 * Reference: docs/art-institutions/loc-channel-design.md and
 * docs/art-institutions/loc-investigation/REPORT.md.
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "art_institution_types.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "ai_loc";

esp_err_t art_institution_loc_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    (void)entry;
    (void)longest_side;
    if (out && len > 0) out[0] = '\0';
    ESP_LOGE(TAG, "build_iiif_url stub — implement in Task 5");
    return ESP_FAIL;
}

esp_err_t art_institution_loc_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id)
{
    (void)channel_id;
    (void)axis;
    (void)term_id;
    ESP_LOGE(TAG, "refresh_channel stub — implement in Task 6");
    return ESP_FAIL;
}
```

- [ ] **Step 2: No commit yet** — wait until Task 4.

---

## Task 4: Wire LoC into the CMake source list and the dispatch table; build

**Files:**
- Modify: `components/art_institution/CMakeLists.txt:1-14`
- Modify: `components/art_institution/art_institution.c:27-68`

- [ ] **Step 1: Add `museums/loc.c` to the SRCS list**

In `components/art_institution/CMakeLists.txt`, append `"museums/loc.c"`
after `"museums/smk.c"`:

```cmake
idf_component_register(
    SRCS
        "art_institution.c"
        "art_institution_rate_limit.c"
        "art_institution_refresh.c"
        "art_institution_resolve.c"
        "art_institution_download.c"
        "museums/common.c"
        "museums/artic.c"
        "museums/rijksmuseum.c"
        "museums/vam.c"
        "museums/wellcome.c"
        "museums/smk.c"
        "museums/loc.c"
    INCLUDE_DIRS "include"
    ...
```

- [ ] **Step 2: Append the LoC dispatch entry**

In `components/art_institution/art_institution.c`, the
`ART_INSTITUTION_MUSEUMS[]` array currently ends with the SMK entry. After
the SMK closing brace, before the final `};`, add:

```c
    {
        .id              = "loc",
        .display         = "Library of Congress",
        .museum_enum     = ART_INSTITUTION_MUSEUM_LOC,
        .refresh_channel = art_institution_loc_refresh_channel,
        .build_iiif_url  = art_institution_loc_build_iiif_url,
        .resolve_entry   = NULL,  // LoC returns the IIIF id inline; no walk.
    },
```

- [ ] **Step 3: Ask the user to run `idf.py build`**

Stop and tell the user:

> Tasks 1–4 staged. Please run `idf.py build` and confirm a clean build
> (no new warnings or errors). The new code compiles a stub that logs
> `build_iiif_url stub` if anyone calls it, but no caller does yet
> because no LoC channel exists.

Do not proceed until the user confirms.

- [ ] **Step 4: Commit Tasks 1–4 together**

```bash
git add components/art_institution/include/art_institution_types.h \
        components/art_institution/art_institution_internal.h \
        components/art_institution/museums/loc.c \
        components/art_institution/CMakeLists.txt \
        components/art_institution/art_institution.c
git commit -m "$(cat <<'EOF'
feat(museum): scaffold Library of Congress adapter

Add ART_INSTITUTION_MUSEUM_LOC enum value, dispatch table entry, and
a stub museums/loc.c. Build passes; the stubs log and return ESP_FAIL
if called, but no caller exists yet because the LoC channel isn't
exposed through the web UI.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Implement `art_institution_loc_build_iiif_url`

**Files:**
- Modify: `components/art_institution/museums/loc.c`

- [ ] **Step 1: Add the URL-base macro and `<stdio.h>` include**

At the top of `museums/loc.c`, after the existing `#include`s, add:

```c
#include <stdio.h>
```

After the `static const char *TAG = "ai_loc";` line, add:

```c
#define LOC_IIIF_BASE "https://tile.loc.gov/image-services/iiif"
```

- [ ] **Step 2: Replace the stub with the real implementation**

Replace the entire stub body of `art_institution_loc_build_iiif_url`
with:

```c
esp_err_t art_institution_loc_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, LOC_IIIF_BASE "/%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}
```

This is byte-for-byte equivalent to `art_institution_vam_build_iiif_url`
modulo the host constant.

- [ ] **Step 3: Ask the user to run `idf.py build`**

> Task 5 staged. Please run `idf.py build` and confirm a clean build.

- [ ] **Step 4: Commit**

```bash
git add components/art_institution/museums/loc.c
git commit -m "$(cat <<'EOF'
feat(museum): implement LoC IIIF URL builder

Mirrors V&A's build: snprintf the IIIF base + key + size segment, no
LoC-specific quirks at URL build time.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Implement `art_institution_loc_refresh_channel`

This is the largest task. It comes in five sub-steps: add includes and
constants; add the IIIF-id extraction helpers; add the per-page fetch
function; add the refresh body; build and commit.

**Files:**
- Modify: `components/art_institution/museums/loc.c`

- [ ] **Step 1: Add the remaining includes and constants**

At the top of `museums/loc.c`, **replace the entire `#include` block**
from Task 3 + Task 5 with the V&A-matching set below:

```c
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
#include "version.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
```

Below the `LOC_IIIF_BASE` macro, add:

```c
#define LOC_API_BASE              "https://www.loc.gov"
#define LOC_PAGE_LIMIT            100
#define LOC_RESPONSE_BUF_SIZE     (256 * 1024)
#define LOC_FETCH_MAX_ATTEMPTS    3
#define LOC_MAX_PAGES             500  // defensive cap; see loc-channel-design.md §3
#define LOC_IIIF_URL_PREFIX       "https://tile.loc.gov/image-services/iiif/"

static const uint32_t s_fetch_backoff_ms[LOC_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

extern void download_manager_rescan(void);
```

Add a User-Agent helper just below those:

```c
static const char *loc_user_agent(void)
{
    static char s_ua[64];
    static bool s_inited = false;
    if (!s_inited) {
        snprintf(s_ua, sizeof(s_ua), "p3a/%s (pub@kury.dev)", FW_VERSION_STRING);
        s_inited = true;
    }
    return s_ua;
}
```

(Mirrors AIC's `aic_user_agent()` — LoC doesn't require it, but no
published rate limit makes polite identification the right default.)

- [ ] **Step 2: Add the IIIF id extraction helpers**

Below the user-agent helper and above the existing `build_iiif_url`,
insert these two functions:

```c
/**
 * Extract the IIIF id from one LoC image URL.
 *
 * Returns true if @p image_url begins with `https://tile.loc.gov/image-services/iiif/`,
 * the path segment up to the next `/` is non-empty, and that segment
 * fits in (out_len - 1). Writes the segment + NUL into @p out on
 * success.
 *
 * The 48-char `iiif_key` cap is enforced by `out_len < 48` — see
 * docs/deferred/loc-iiif-key-48-char.md for context.
 */
static bool extract_loc_iiif_id(const char *image_url, char *out, size_t out_len)
{
    if (!image_url || !out || out_len == 0) return false;
    static const char prefix[] = LOC_IIIF_URL_PREFIX;
    const size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(image_url, prefix, prefix_len) != 0) return false;

    const char *rest = image_url + prefix_len;
    const char *slash = strchr(rest, '/');
    size_t id_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (id_len == 0 || id_len >= out_len) return false;

    memcpy(out, rest, id_len);
    out[id_len] = '\0';
    return true;
}

/**
 * Walk one search-result's image_url[] and resources[].image fields
 * looking for an LoC IIIF URL. Writes the extracted id into @p out
 * and returns true on the first match; false if no IIIF URL is
 * present or every candidate exceeds the iiif_key slot.
 */
static bool pick_loc_iiif_id(const cJSON *result, char *out, size_t out_len)
{
    if (!result) return false;

    const cJSON *image_url = cJSON_GetObjectItem(result, "image_url");
    if (cJSON_IsArray(image_url)) {
        int n = cJSON_GetArraySize(image_url);
        for (int i = 0; i < n; i++) {
            const cJSON *u = cJSON_GetArrayItem(image_url, i);
            if (cJSON_IsString(u) && u->valuestring &&
                extract_loc_iiif_id(u->valuestring, out, out_len)) {
                return true;
            }
        }
    } else if (cJSON_IsString(image_url) && image_url->valuestring) {
        if (extract_loc_iiif_id(image_url->valuestring, out, out_len)) {
            return true;
        }
    }

    const cJSON *resources = cJSON_GetObjectItem(result, "resources");
    if (cJSON_IsArray(resources)) {
        int n = cJSON_GetArraySize(resources);
        for (int i = 0; i < n; i++) {
            const cJSON *r = cJSON_GetArrayItem(resources, i);
            if (!cJSON_IsObject(r)) continue;
            const cJSON *img = cJSON_GetObjectItem(r, "image");
            if (cJSON_IsString(img) && img->valuestring &&
                extract_loc_iiif_id(img->valuestring, out, out_len)) {
                return true;
            }
        }
    }
    return false;
}
```

- [ ] **Step 3: Add the per-page fetch function**

Between `pick_loc_iiif_id` and the existing `art_institution_loc_build_iiif_url`,
insert:

```c
/**
 * Fetch and parse one LoC `/search/` page.
 *
 * URL shape:
 *   GET /search/?fo=json&c=100&sp=N&fa=original-format:<lowercase title>
 *
 * Returns ESP_OK with *out_count set on success (possibly 0 — pages
 * commonly yield few IIIF-bearing entries; the refresh loop continues
 * paging until cache_size entries are collected or end-of-results).
 * Returns ESP_ERR_INVALID_RESPONSE on 429 (cooldown engaged before
 * return), ESP_ERR_NOT_ALLOWED on 401/403, ESP_FAIL on parse error or
 * exhausted retries.
 */
static esp_err_t loc_fetch_page(const char *term_id,
                                int page,
                                char *response_buf,
                                size_t response_buf_size,
                                institution_channel_entry_t *out_entries,
                                size_t max_entries,
                                size_t *out_count,
                                int *out_total,
                                bool *has_more)
{
    *out_count = 0;
    *has_more = false;
    if (out_total) *out_total = 0;

    // term_id is the LoC facet value verbatim (e.g. "photo, print, drawing").
    // ai_url_encode percent-encodes everything outside the unreserved set,
    // which is what LoC's API accepts; see loc-investigation REPORT §Q1.
    char encoded_term[160];
    ai_url_encode(term_id, encoded_term, sizeof(encoded_term));

    char url[640];
    int wrote = snprintf(url, sizeof(url),
                         LOC_API_BASE
                         "/search/?fo=json&c=%d&sp=%d&fa=original-format%%3A%s",
                         LOC_PAGE_LIMIT, page, encoded_term);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for term=%s page=%d", term_id, page);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching page %d (term=%.40s)", page, term_id);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < LOC_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retrying LoC page fetch in %lums (attempt %d/%d)",
                     (unsigned long)s_fetch_backoff_ms[attempt],
                     attempt + 1, LOC_FETCH_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
            total_read = 0;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        esp_http_client_set_header(client, "User-Agent", loc_user_agent());
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
                cooldown = ai_parse_retry_after(retry_after);
            }
            art_institution_set_rate_limited("loc", cooldown);
            ESP_LOGW(TAG, "LoC returned 429 (cooldown %us)",
                     (unsigned)(cooldown ? cooldown : 60));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (status == 401 || status == 403) {
            ESP_LOGW(TAG, "LoC returned %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "LoC status %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        total_read = ai_drain_body(client, response_buf, response_buf_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (total_read < 0) continue;
        if (total_read == 0) continue;
        if (total_read >= (int)response_buf_size - 1) {
            ESP_LOGE(TAG, "LoC response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        if (content_length > 0 && total_read < (int)content_length) {
            ESP_LOGW(TAG, "LoC truncated: got %d/%lld bytes",
                     total_read, (long long)content_length);
            continue;
        }
        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) {
        ESP_LOGE(TAG, "LoC page %d fetch failed after %d attempts",
                 page, LOC_FETCH_MAX_ATTEMPTS);
        return ESP_FAIL;
    }
    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "LoC JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) {
        ESP_LOGE(TAG, "LoC response missing 'results' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int total_records = 0;
    const cJSON *pag = cJSON_GetObjectItem(root, "pagination");
    if (cJSON_IsObject(pag)) {
        const cJSON *t = cJSON_GetObjectItem(pag, "total");
        if (cJSON_IsNumber(t)) total_records = (int)cJSON_GetNumberValue(t);
    }
    if (out_total) *out_total = total_records;

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(results);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *r = cJSON_GetArrayItem(results, i);
        if (!cJSON_IsObject(r)) continue;

        char iiif_key[sizeof(out_entries[0].iiif_key)];
        if (!pick_loc_iiif_id(r, iiif_key, sizeof(iiif_key))) {
            // No IIIF URL, or extracted id >= 48 chars; silently skip
            // per the design (see loc-channel-design.md §5.2, §5.4).
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("loc", iiif_key);
        e->kind = 0;
        e->extension = 3;  // jpg — LoC's IIIF reliably serves JPEG
        e->created_at = now;
        strlcpy(e->iiif_key, iiif_key, sizeof(e->iiif_key));
        parsed++;
    }

    int seen_so_far = (page - 1) * LOC_PAGE_LIMIT + array_size;
    *has_more = (array_size > 0) && (seen_so_far < total_records);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "LoC page %d: parsed %zu/%d entries (total=%d), has_more=%d",
             page, parsed, array_size, total_records, (int)*has_more);
    return ESP_OK;
}
```

- [ ] **Step 4: Add the refresh function**

After the per-page fetch and after `art_institution_loc_build_iiif_url`,
append the refresh body:

```c
esp_err_t art_institution_loc_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id)
{
    if (!channel_id || !axis || !term_id) return ESP_ERR_INVALID_ARG;

    // LoC v1 exposes a single axis. Reject anything else so a malformed
    // playset (or a hypothetical future axis a user hand-edited in) gets
    // a clear error rather than silently fetching the wrong thing.
    if (strcmp(axis, "format") != 0) {
        ESP_LOGE(TAG, "Unknown LoC axis '%s'", axis);
        return ESP_ERR_INVALID_ARG;
    }

    if (art_institution_is_rate_limited("loc")) {
        ESP_LOGW(TAG, "LoC rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(LOC_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(LOC_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        LOC_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(LOC_PAGE_LIMIT * sizeof(institution_channel_entry_t));
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

    while (total_fetched < cache_size && page <= LOC_MAX_PAGES) {
        size_t page_count = 0;
        int total_records = 0;
        bool has_more = false;
        esp_err_t err = loc_fetch_page(term_id, page,
                                       response_buf, LOC_RESPONSE_BUF_SIZE,
                                       page_entries, LOC_PAGE_LIMIT,
                                       &page_count, &total_records, &has_more);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }
        if (page_count == 0 && !has_more) {
            ESP_LOGI(TAG, "No more entries at page %d, done", page);
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
            ESP_LOGI(TAG, "LoC page %d merged: %zu entries (total %zu)",
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
            art_institution_evict_orphans(evict_cache, si_hash, "loc");
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
        ESP_LOGI(TAG, "LoC refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "LoC refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "LoC refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
```

- [ ] **Step 5: Ask the user to run `idf.py build`**

> Task 6 staged. Please run `idf.py build` and confirm a clean build.
> No new warnings expected.

- [ ] **Step 6: Commit**

```bash
git add components/art_institution/museums/loc.c
git commit -m "$(cat <<'EOF'
feat(museum): implement LoC refresh loop

Mirrors V&A's page-loop shape with two LoC-specific filters: skip
results without an inline tile.loc.gov/image-services/iiif/ URL, and
skip results whose IIIF id is >= 48 chars. A 500-page defensive cap
bounds the page loop (no LoC format facet has enough results to hit
it; see loc-channel-design.md §3).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: TLS cert-bundle verification (pre-merge gate)

This is a check, not a code change. The output is a one-line confirmation
in the commit message of Task 6 or a follow-up commit with a
`esp_crt_bundle_attach` override.

- [ ] **Step 1: Inspect the `esp_crt_bundle` configuration**

Check `sdkconfig` for `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` and
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` (or
`_CMN`). If both are set, the Mozilla CA bundle that ships with ESP-IDF
covers DigiCert / Let's Encrypt / GlobalSign etc., which are what
LoC's CDNs use.

```bash
grep -E "CERTIFICATE_BUNDLE" sdkconfig
```

- [ ] **Step 2: Confirm by inspection of the production hosts**

The two hosts in play:

- `https://www.loc.gov` — cert issuer typically DigiCert TLS Hybrid ECC
  SHA384 2020 CA1.
- `https://tile.loc.gov` — same chain.

If `sdkconfig` shows the full bundle is enabled, no action needed.

- [ ] **Step 3: If the bundle is missing the chain, document the workaround**

If TLS handshake fails at runtime (Task 11 manual test), capture the
PEM with:

```powershell
$env:PYTHONUTF8="1"
python -c "import ssl, socket; sock = socket.create_connection(('www.loc.gov', 443)); s = ssl.create_default_context().wrap_socket(sock, server_hostname='www.loc.gov'); print(ssl.get_server_certificate(('www.loc.gov', 443)))"
```

Add the PEM as a `.pem` resource and use `esp_crt_bundle_set` to
register it. Defer until a real handshake failure surfaces — current
ESP-IDF default bundle has been verified working for the other five
museum CDNs.

- [ ] **Step 4: No commit unless a workaround is added**

---

## Task 8: Web UI LoC adapter

**Files:**
- Create: `webui/museum/loc.js`

- [ ] **Step 1: Create the LoC adapter file**

Write the full content of `webui/museum/loc.js`:

```javascript
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Library of Congress browse adapter.
//
// Single axis (`format`). Three hardcoded terms: photo/print/drawing,
// manuscript/mixed material, 3d object. Term counts are populated by
// a per-term `c=1` probe on first axis open.
//
// LoC's listing returns a IIIF URL only on a subset of results
// (~8% for photo/print/drawing). The adapter filters results client-
// side to those with a tile.loc.gov/image-services/iiif/ URL whose
// extracted id is < 48 chars — matching the C-side refresh filter
// so what the user previews matches what the device will actually
// store.
//
// The 500-page cap (matching the C-side refresh) makes listArtworks
// safe to call without offset bounds.

const SEARCH = 'https://www.loc.gov/search/';
const IIIF_HOST = 'https://tile.loc.gov/image-services/iiif';

const TERMS = [
    { id: 'photo, print, drawing',       label: 'Photo, Print, Drawing' },
    { id: 'manuscript/mixed material',   label: 'Manuscript/Mixed Material' },
    { id: '3d object',                   label: '3D Object' },
];

const MAX_IIIF_KEY_LEN = 48;

async function getJson(url) {
    const r = await fetch(url);
    if (r.status === 429) {
        try {
            const retryAfter = parseInt(r.headers.get('Retry-After') || '0', 10);
            fetch('/api/museum/rate-limits/report-429', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    museum: 'loc',
                    retry_after_sec: isFinite(retryAfter) && retryAfter > 0 ? retryAfter : 60,
                }),
            }).catch(() => {});
        } catch (_) { /* ignore */ }
        const err = new Error(`LoC 429 ${url}`);
        err.status = 429;
        throw err;
    }
    if (!r.ok) throw new Error(`LoC ${r.status} ${url}`);
    return r.json();
}

function buildSearchUrl(termId, { c, sp }) {
    const params = new URLSearchParams({
        fo: 'json',
        c: String(c),
        sp: String(sp),
        fa: `original-format:${termId}`,
    });
    return `${SEARCH}?${params}`;
}

function extractIiifId(imageUrl) {
    if (typeof imageUrl !== 'string') return null;
    const prefix = `${IIIF_HOST}/`;
    if (!imageUrl.startsWith(prefix)) return null;
    const rest = imageUrl.slice(prefix.length);
    const slash = rest.indexOf('/');
    const id = slash >= 0 ? rest.slice(0, slash) : rest;
    if (!id || id.length >= MAX_IIIF_KEY_LEN) return null;
    return id;
}

function pickIiifId(result) {
    const images = Array.isArray(result.image_url) ? result.image_url : [];
    for (const u of images) {
        const id = extractIiifId(u);
        if (id) return id;
    }
    const resources = Array.isArray(result.resources) ? result.resources : [];
    for (const res of resources) {
        if (res && typeof res === 'object') {
            const id = extractIiifId(res.image);
            if (id) return id;
        }
    }
    return null;
}

function getTitle(result) {
    return result && result.title ? String(result.title) : '(untitled)';
}

function getDate(result) {
    return result && result.date ? String(result.date) : '';
}

function getArtist(result) {
    const contribs = result && result.contributor;
    if (Array.isArray(contribs) && contribs.length > 0) {
        return String(contribs[0]);
    }
    return '';
}

export class LocAdapter {
    get id()          { return 'loc'; }
    get displayName() { return 'Library of Congress'; }
    get shortName()   { return 'LoC'; }
    get axes() {
        return [{ name: 'format', label: 'Formats' }];
    }

    constructor() {
        this._terms = null;  // cached per session
    }

    async listCollections({ axis = 'format' } = {}) {
        if (axis !== 'format') throw new Error(`LoC: unknown axis ${axis}`);
        if (this._terms) return this._terms;

        // Probe each term in parallel for a count (concurrency = 3 since
        // the list is fixed and tiny).
        const probes = TERMS.map(async (t) => {
            try {
                const j = await getJson(buildSearchUrl(t.id, { c: 1, sp: 1 }));
                const total = (j && j.pagination && j.pagination.total) | 0;
                return { id: t.id, label: t.label, count: total };
            } catch (_) {
                return { id: t.id, label: t.label, count: 0 };
            }
        });
        const out = await Promise.all(probes);
        // Keep the configured order — Photos first, then Manuscripts,
        // then 3D. (Counts are informational, not ranking signal.)
        this._terms = out;
        return out;
    }

    async listArtworks(termId, { offset = 0, rows = 20, axis = 'format' } = {}) {
        if (axis !== 'format') throw new Error(`LoC: unknown axis ${axis}`);
        // LoC uses sp (1-based page) + c (per-page). Translate offset/rows.
        // We request c = max(rows, 100) so a single fetch typically yields
        // enough IIIF-bearing items for the preview after filtering. The
        // browse modal calls back for more when its buffer drains.
        const c = Math.max(rows, 100);
        const sp = Math.floor(offset / c) + 1;
        const url = buildSearchUrl(termId, { c, sp });
        const d = await getJson(url);
        const results = Array.isArray(d.results) ? d.results : [];
        const items = [];
        for (const r of results) {
            const iiifId = pickIiifId(r);
            if (!iiifId) continue;
            items.push({
                id: iiifId,
                imageId: iiifId,
                title: getTitle(r),
                artist: getArtist(r),
                date: getDate(r),
            });
            if (items.length >= rows) break;
        }
        const total = (d && d.pagination && d.pagination.total) | 0;
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

- [ ] **Step 2: Visual code review**

Re-read the file. Confirm:
- The three TERMS entries match the design's wire identifiers exactly
  (`photo, print, drawing` lowercase, commas, no extra spaces).
- `MAX_IIIF_KEY_LEN = 48` matches the C-side filter so the browser
  and firmware agree on what's playable.
- The 429 reporting POST body uses `museum: 'loc'`.

- [ ] **Step 3: Commit**

```bash
git add webui/museum/loc.js
git commit -m "$(cat <<'EOF'
feat(museum): web UI adapter for Library of Congress

Three hardcoded format terms (photo/print/drawing, manuscript/mixed
material, 3d object). Counts populated by parallel c=1 probes on
first axis open. Filters listing results client-side to those with
a tile.loc.gov IIIF URL whose id fits in 48 chars, matching the
C-side refresh filter so the preview never shows an artwork the
device cannot actually play.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Register the LoC adapter in the browse-modal registry

**Files:**
- Modify: `webui/museum/index.js`

- [ ] **Step 1: Add the import**

In `webui/museum/index.js`, after the existing imports (last one is
`SmkAdapter`), add:

```javascript
import { LocAdapter } from './loc.js';
```

- [ ] **Step 2: Append the adapter instance**

Inside the `ADAPTERS = [...]` array, after `new SmkAdapter(),`, append:

```javascript
    new LocAdapter(),
```

- [ ] **Step 3: Ask the user to test in a browser**

Tell the user:

> Web UI staged. Please:
>
> 1. Flash the firmware with the latest changes (or just push
>    `webui/` if served from LittleFS via the dev workflow).
> 2. Open `http://p3a.local/playset-editor.html`.
> 3. Pick Channel Type → Museum → Library of Congress.
> 4. Confirm three terms appear (Photo/Print/Drawing,
>    Manuscript/Mixed Material, 3D Object) with non-zero counts.
> 5. Click a term, confirm the preview shows at least one artwork
>    (may take a few seconds because of LoC's low IIIF surface rate).
> 6. Click Previous / Next, confirm navigation works.
> 7. Click "Add channel" and confirm the playset editor receives a
>    channel spec with `name: "loc:format"`,
>    `identifier: "photo, print, drawing"` (or similar lowercase
>    value), `display_name` starting with `"LoC · "`.

Do not proceed until the user reports success.

- [ ] **Step 4: Commit**

```bash
git add webui/museum/index.js
git commit -m "$(cat <<'EOF'
feat(museum): register Library of Congress adapter

Adds LocAdapter to the museum browse modal's registry. The browse
flow already dispatches uniformly through the adapter surface, so no
modal changes are needed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Documentation polish

**Files:**
- Modify: `docs/art-institutions/finalized-design.md` (append §9.6)
- Modify: `docs/HOW-TO-USE.md`

- [ ] **Step 1: Append a §9.6 entry to `finalized-design.md`**

Open `docs/art-institutions/finalized-design.md`. Locate the end of
§9.5 (the SMK block). Append §9.6:

````markdown
### 9.6 Library of Congress

- **id:** `loc`
- **display:** `Library of Congress`
- **API base:** `https://www.loc.gov`
- **IIIF base:** `https://tile.loc.gov/image-services/iiif`
- **Required header:** `User-Agent: p3a/{version} (pub@kury.dev)` —
  LoC does not require it; sent for polite identification given no
  published rate limit.
- **Axes (filterable, in browse order):** `format` (single axis).
- **Terms (hardcoded):** `photo, print, drawing`,
  `manuscript/mixed material`, `3d object`. The wire identifier is the
  lowercase facet title with spaces preserved — LoC's `fa=` filter
  silently drops slug-form values.
- **Pagination cap:** 500 pages × 100 results = 50,000 results.
  Defensive only; the natural end-of-results limit always bites first.
- **Rate limit:** none published; default 60 s cooldown on 429 without
  `Retry-After`.
- **Listing endpoint:**
  `GET /search/?fo=json&c=100&sp=N&fa=original-format:<value>`
- **IIIF URL:** `https://tile.loc.gov/image-services/iiif/{iiif_key}/full/!720,720/0/default.jpg`
- **`iiif_key` value:** the path segment between
  `https://tile.loc.gov/image-services/iiif/` and the next `/` in any
  `image_url[]` or `resources[].image` URL on the result. Entries
  whose key is empty or ≥ 48 chars are skipped at refresh — see
  [`docs/deferred/loc-iiif-key-48-char.md`](../deferred/loc-iiif-key-48-char.md).
- **`extension`:** always 3 (jpg).
- **`resolve_entry`:** NULL — IIIF id is inline; no walk.
- **Quirks worth knowing:** most listing results have no IIIF URL
  (~92 % for photo/print/drawing) and are silently dropped. This is
  expected; refresh paginates until cache fills or end-of-results.
  See `docs/art-institutions/loc-channel-design.md` and
  `docs/art-institutions/loc-investigation/REPORT.md`.
````

- [ ] **Step 2: Update `docs/HOW-TO-USE.md`**

Open `docs/HOW-TO-USE.md`. Locate the museum-channel section (search
for "Museum" or "art institution"). Add a brief mention that the LoC
channel is now available with three format options. Keep it short —
match the existing tone of the section.

If the section reads as a flat enumeration ("AIC, Rijks, V&A,
Wellcome, SMK"), simply append "Library of Congress" to the list.

- [ ] **Step 3: Commit**

```bash
git add docs/art-institutions/finalized-design.md docs/HOW-TO-USE.md
git commit -m "$(cat <<'EOF'
docs(museum): add Library of Congress to finalized design + how-to

Appends §9.6 to finalized-design.md with the per-museum spec block
matching the format used for the prior five museums, and lists LoC in
the user-facing how-to.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Manual end-to-end test (release gate)

This is the release-gate manual test, identical in spirit to
finalized-design.md §12.4. It's a separate task because it produces
no commits — only confirmation that the soak passes.

- [ ] **Step 1: Build and flash**

> Please run `idf.py build flash monitor` and let me know the build
> + flash completed.

- [ ] **Step 2: Configure the test playset**

Using `http://p3a.local/playset-editor.html`:

1. Create a new playset (or edit an existing test playset).
2. Add an LoC Photo/Print/Drawing channel.
3. Add an LoC Manuscript/Mixed Material channel.
4. Save the playset and activate it.

- [ ] **Step 3: Observe first-refresh kickoff**

In the serial monitor, expect:

- `ai_loc` log lines within ~30 seconds of playset activation.
- Page-by-page progress logs ("Fetching page 1 (term=…)").
- Merge logs ("LoC page N merged: M entries (total K)").
- Eventually a "LoC refresh complete" or "LoC refresh partial" log.

- [ ] **Step 4: 24-hour soak**

Let the device run. After 24 hours, confirm:

- The picker has rotated through LoC entries (look for picker log
  lines that reference an `loc:` channel id, or simply confirm
  the device is displaying LoC artwork on screen).
- JPEG downloads succeed (no repeated `art_inst download failed`
  lines).
- A second refresh runs after `ai_refresh_sec` elapses (default 24 h)
  without errors.
- TLS handshakes succeed for `www.loc.gov` and `tile.loc.gov`. If a
  handshake fails, see Task 7 step 3 for the cert workaround.

- [ ] **Step 5: Report and triage**

Report the soak result. If issues surfaced:

- Capture relevant log excerpts.
- File issues against the design or implementation as appropriate.
- If the issue is in the design rather than the implementation, the
  design doc gets a §15 entry capturing the field-observed fix
  (mirrors finalized-design.md §15 for AIC/Rijks).

---

## Self-review checklist

Before declaring the plan complete:

- [ ] Every spec requirement (loc-channel-design.md §2, §3, §5-11) maps to
  a task above.
- [ ] No "TBD", "TODO", or "similar to" placeholders.
- [ ] Type/method names are consistent across tasks
  (`art_institution_loc_refresh_channel`,
  `art_institution_loc_build_iiif_url`,
  `ART_INSTITUTION_MUSEUM_LOC`, `museum_id="loc"`).
- [ ] File paths are absolute or repo-relative, not abstract.
- [ ] Commit message conventions match recent history
  (`docs(museum):`, `feat(museum):`).
