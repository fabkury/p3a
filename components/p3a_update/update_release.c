// SPDX-License-Identifier: Apache-2.0
// Which release to offer. See update_release.h. Pure.

#include "update_release.h"

#include <string.h>

#include "cJSON.h"
#include "p3a_err.h"

// --- versions -----------------------------------------------------------------------

typedef struct {
    uint32_t number[3];
    const char *pre;  // the pre-release part, without its '-'; NULL if none
    size_t pre_len;
} version_t;

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_ident_char(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

// Dot-separated identifiers, none empty, of letters, digits and '-'.
static bool identifiers_ok(const char *s, size_t len)
{
    if (len == 0) {
        return false;
    }
    bool at_start = true;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '.') {
            if (at_start) {
                return false;
            }
            at_start = true;
        } else if (is_ident_char(s[i])) {
            at_start = false;
        } else {
            return false;
        }
    }
    return !at_start;
}

static bool parse_version(const char *text, version_t *out)
{
    if (text == NULL) {
        return false;
    }
    const size_t total = strnlen(text, UPDATE_VERSION_LEN);
    if (total == 0 || total >= UPDATE_VERSION_LEN) {
        return false;
    }
    const char *p = text;
    for (int i = 0; i < 3; i++) {
        if (!is_digit(*p)) {
            return false;
        }
        uint32_t n = 0;
        int digits = 0;
        while (is_digit(*p)) {
            if (++digits > 9) {
                return false;
            }
            n = n * 10 + (uint32_t)(*p - '0');
            p++;
        }
        out->number[i] = n;
        if (i < 2) {
            if (*p != '.') {
                return false;
            }
            p++;
        }
    }
    out->pre = NULL;
    out->pre_len = 0;
    if (*p == '-') {
        p++;
        const char *end = p;
        while (*end != '\0' && *end != '+') {
            end++;
        }
        if (!identifiers_ok(p, (size_t)(end - p))) {
            return false;
        }
        out->pre = p;
        out->pre_len = (size_t)(end - p);
        p = end;
    }
    if (*p == '+') {
        p++;
        return identifiers_ok(p, strlen(p));
    }
    return *p == '\0';
}

bool update_version_ok(const char *version)
{
    version_t v;
    return parse_version(version, &v);
}

static bool all_digits(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!is_digit(s[i])) {
            return false;
        }
    }
    return len > 0;
}

// Two pre-release identifiers: numbers by value and before words, words by
// their characters.
static int compare_identifier(const char *a, size_t a_len, const char *b, size_t b_len)
{
    const bool a_num = all_digits(a, a_len);
    const bool b_num = all_digits(b, b_len);
    if (a_num && b_num) {
        while (a_len > 1 && *a == '0') {
            a++;
            a_len--;
        }
        while (b_len > 1 && *b == '0') {
            b++;
            b_len--;
        }
        if (a_len != b_len) {
            return a_len < b_len ? -1 : 1;
        }
        const int c = memcmp(a, b, a_len);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (a_num != b_num) {
        return a_num ? -1 : 1;
    }
    const size_t shared = a_len < b_len ? a_len : b_len;
    const int c = memcmp(a, b, shared);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return a_len == b_len ? 0 : a_len < b_len ? -1 : 1;
}

static int compare_pre(const version_t *a, const version_t *b)
{
    if (a->pre == NULL || b->pre == NULL) {
        return a->pre == b->pre ? 0 : a->pre == NULL ? 1 : -1;  // the one without is the release
    }
    size_t ai = 0, bi = 0;
    while (ai < a->pre_len && bi < b->pre_len) {
        size_t ae = ai, be = bi;
        while (ae < a->pre_len && a->pre[ae] != '.') {
            ae++;
        }
        while (be < b->pre_len && b->pre[be] != '.') {
            be++;
        }
        const int c = compare_identifier(a->pre + ai, ae - ai, b->pre + bi, be - bi);
        if (c != 0) {
            return c;
        }
        ai = ae + 1;
        bi = be + 1;
    }
    const bool a_more = ai < a->pre_len;
    const bool b_more = bi < b->pre_len;
    return a_more == b_more ? 0 : a_more ? 1 : -1;  // more identifiers come later
}

int update_version_compare(const char *a, const char *b)
{
    version_t va, vb;
    const bool a_ok = parse_version(a, &va);
    const bool b_ok = parse_version(b, &vb);
    if (!a_ok || !b_ok) {
        return a_ok == b_ok ? 0 : a_ok ? 1 : -1;
    }
    for (int i = 0; i < 3; i++) {
        if (va.number[i] != vb.number[i]) {
            return va.number[i] < vb.number[i] ? -1 : 1;
        }
    }
    return compare_pre(&va, &vb);
}

// --- GitHub's list --------------------------------------------------------------------

static const char *string_of(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// "sha256:" and 64 hex digits, nothing more.
static bool parse_digest(const char *digest, uint8_t out[32])
{
    static const char prefix[] = "sha256:";
    if (digest == NULL || strncmp(digest, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *hex = digest + sizeof(prefix) - 1;
    if (strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        const int high = hex_value(hex[2 * i]);
        const int low = hex_value(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

// Copies as much of `text` as fits, never cutting a UTF-8 character in two.
static void copy_text(char *out, size_t out_len, const char *text)
{
    size_t n = text != NULL ? strlen(text) : 0;
    if (n >= out_len) {
        n = out_len - 1;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    if (n > 0) {
        memcpy(out, text, n);
    }
    out[n] = '\0';
}

// The release's image, if it has a whole one. Fills url, sha256, size.
static bool read_asset(const cJSON *release, const update_want_t *want, update_release_t *out)
{
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(release, "assets");
    const cJSON *asset = NULL;
    cJSON_ArrayForEach(asset, assets)
    {
        const char *name = string_of(asset, "name");
        if (name == NULL || strcmp(name, UPDATE_ASSET_NAME) != 0) {
            continue;
        }
        // GitHub lists an asset while it is still being uploaded ("starter", "open").
        const char *state = string_of(asset, "state");
        if (state != NULL && strcmp(state, "uploaded") != 0) {
            return false;
        }
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
        if (!cJSON_IsNumber(size) || size->valuedouble < (double)UPDATE_MIN_BYTES ||
            size->valuedouble > (double)want->max_bytes) {
            return false;
        }
        const char *url = string_of(asset, "browser_download_url");
        const size_t prefix_len = strlen(want->url_prefix);
        if (url == NULL || strlen(url) >= UPDATE_URL_LEN || strncmp(url, want->url_prefix, prefix_len) != 0) {
            return false;
        }
        for (const char *p = url; *p != '\0'; p++) {
            if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7F) {
                return false;  // an address is plain characters; anything else is not one
            }
        }
        if (!parse_digest(string_of(asset, "digest"), out->sha256)) {
            return false;
        }
        out->size = (uint32_t)size->valuedouble;
        memcpy(out->url, url, strlen(url) + 1);
        return true;
    }
    return false;
}

esp_err_t update_release_pick(const char *json, size_t len, const update_want_t *want, update_release_t *out,
                              int *considered)
{
    if (considered != NULL) {
        *considered = 0;
    }
    if (json == NULL || want == NULL || want->url_prefix == NULL || want->url_prefix[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    bool found = false;
    int seen = 0;
    const cJSON *release = NULL;
    cJSON_ArrayForEach(release, root)
    {
        seen++;
        if (!cJSON_IsObject(release) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "draft"))) {
            continue;
        }
        const char *tag = string_of(release, "tag_name");
        if (tag == NULL) {
            continue;
        }
        if (tag[0] == 'v') {
            tag++;
        }
        version_t version;
        if (!parse_version(tag, &version)) {
            continue;
        }
        const bool prerelease =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "prerelease")) || version.pre != NULL;
        if (prerelease && !want->prereleases) {
            continue;
        }
        if (found && update_version_compare(tag, out->version) <= 0) {
            continue;
        }
        update_release_t candidate = {0};
        if (!read_asset(release, want, &candidate)) {
            continue;
        }
        memcpy(candidate.version, tag, strlen(tag) + 1);
        candidate.prerelease = prerelease;
        copy_text(candidate.published, sizeof(candidate.published), string_of(release, "published_at"));
        copy_text(candidate.notes, sizeof(candidate.notes), string_of(release, "body"));
        *out = candidate;
        found = true;
    }
    cJSON_Delete(root);
    if (considered != NULL) {
        *considered = seen;
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
