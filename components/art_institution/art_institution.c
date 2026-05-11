// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file art_institution.c
 * @brief Dispatch table + public helpers for the art-institution component
 *
 * Stage 1 lands the scaffolding: dispatch table, init, spec parsing,
 * post_id hashing, vault path construction. Refresh and download bodies
 * live in stage 3 (art_institution_refresh.c, art_institution_download.c)
 * once the play scheduler and download manager have institution-aware
 * branches to call them.
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "sd_path.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "art_inst";

// ----- Dispatch table -----------------------------------------------------

const art_institution_museum_t ART_INSTITUTION_MUSEUMS[] = {
    {
        .id             = "artic",
        .display        = "Art Institute of Chicago",
        .museum_enum    = ART_INSTITUTION_MUSEUM_ARTIC,
        .refresh_channel = art_institution_artic_refresh_channel,
        .build_iiif_url  = art_institution_artic_build_iiif_url,
    },
    // M2 will append Rijksmuseum here.
};

const size_t ART_INSTITUTION_MUSEUM_COUNT =
    sizeof(ART_INSTITUTION_MUSEUMS) / sizeof(ART_INSTITUTION_MUSEUMS[0]);

// ----- Lifecycle ----------------------------------------------------------

esp_err_t art_institution_init(void)
{
    art_institution_rate_limit_reset();
    ESP_LOGI(TAG, "art_institution initialized (%zu museum(s))",
             ART_INSTITUTION_MUSEUM_COUNT);
    return ESP_OK;
}

// ----- Lookups & parsing --------------------------------------------------

const art_institution_museum_t *art_institution_find(const char *museum_id)
{
    if (!museum_id || museum_id[0] == '\0') return NULL;
    for (size_t i = 0; i < ART_INSTITUTION_MUSEUM_COUNT; i++) {
        if (strcmp(ART_INSTITUTION_MUSEUMS[i].id, museum_id) == 0) {
            return &ART_INSTITUTION_MUSEUMS[i];
        }
    }
    return NULL;
}

esp_err_t art_institution_parse_spec(const char *spec_name,
                                     char *out_museum, size_t museum_len,
                                     char *out_axis, size_t axis_len)
{
    if (out_museum && museum_len > 0) out_museum[0] = '\0';
    if (out_axis && axis_len > 0) out_axis[0] = '\0';

    if (!spec_name || !out_museum || !out_axis || museum_len == 0 || axis_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *colon = strchr(spec_name, ':');
    if (!colon || colon == spec_name || colon[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t mlen = (size_t)(colon - spec_name);
    if (mlen >= museum_len) return ESP_ERR_INVALID_SIZE;
    memcpy(out_museum, spec_name, mlen);
    out_museum[mlen] = '\0';

    size_t alen = strlen(colon + 1);
    if (alen >= axis_len) return ESP_ERR_INVALID_SIZE;
    memcpy(out_axis, colon + 1, alen + 1);

    return ESP_OK;
}

// ----- Post-id hashing ----------------------------------------------------

int32_t art_institution_compute_post_id(const char *museum_id, const char *iiif_key)
{
    if (!museum_id || !iiif_key) return 0;

    uint32_t hash = ART_INSTITUTION_DJB2_SALT;
    for (const char *p = museum_id; *p; p++) {
        hash = ((hash << 5) + hash) + (unsigned char)*p;
    }
    hash = ((hash << 5) + hash) + (unsigned char)':';
    for (const char *p = iiif_key; *p; p++) {
        hash = ((hash << 5) + hash) + (unsigned char)*p;
    }

    int32_t post_id = (int32_t)(hash & 0x7FFFFFFFu);
    if (post_id == 0) post_id = 1;  // 0 reserved for "no post_id"
    return post_id;
}

// ----- Vault path ---------------------------------------------------------

esp_err_t art_institution_build_vault_path(const char *museum_id,
                                           const institution_channel_entry_t *entry,
                                           char *out_path, size_t out_len)
{
    if (!museum_id || !entry || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;

    char base[128];
    if (sd_path_get_museum(base, sizeof(base)) != ESP_OK) {
        strlcpy(base, "/sdcard/p3a/museum", sizeof(base));
    }

    uint8_t sha[32];
    if (mbedtls_sha256((const unsigned char *)entry->iiif_key,
                       strlen(entry->iiif_key), sha, 0) != 0) {
        ESP_LOGE(TAG, "SHA256 failed for iiif_key='%s'", entry->iiif_key);
        return ESP_FAIL;
    }

    // Same byte encoding as makapix_channel_entry_t.extension; AIC artwork
    // uses 3 (jpg). 0xFE/0xFF sentinels never reach here in M1 — the download
    // manager filters them out before path construction.
    const char *ext;
    switch (entry->extension) {
        case 0:  ext = ".webp"; break;
        case 1:  ext = ".gif";  break;
        case 2:  ext = ".png";  break;
        case 3:  ext = ".jpg";  break;
        default: ext = ".jpg";  break;
    }

    int n = snprintf(out_path, out_len, "%s/%s/%02x/%02x/%02x/%s%s",
                     base, museum_id,
                     (unsigned)sha[0], (unsigned)sha[1], (unsigned)sha[2],
                     entry->iiif_key, ext);
    if (n < 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t art_institution_build_vault_path_from_spec(const char *spec_name,
                                                     const institution_channel_entry_t *entry,
                                                     char *out_path, size_t out_len)
{
    if (!spec_name || !entry || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char museum[16] = {0};
    char axis[32] = {0};
    esp_err_t err = art_institution_parse_spec(spec_name,
                                               museum, sizeof(museum),
                                               axis, sizeof(axis));
    if (err != ESP_OK) return err;
    return art_institution_build_vault_path(museum, entry, out_path, out_len);
}

esp_err_t art_institution_build_iiif_url(const char *museum_id,
                                         const institution_channel_entry_t *entry,
                                         int longest_side,
                                         char *out, size_t len)
{
    if (out && len > 0) out[0] = '\0';
    if (!museum_id || !entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    const art_institution_museum_t *m = art_institution_find(museum_id);
    if (!m || !m->build_iiif_url) return ESP_ERR_NOT_FOUND;
    return m->build_iiif_url(entry, longest_side, out, len);
}
