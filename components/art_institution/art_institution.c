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
#include <stdio.h>
#include <string.h>

static const char *TAG = "art_inst";

// ----- Dispatch table -----------------------------------------------------

const art_institution_museum_t ART_INSTITUTION_MUSEUMS[] = {
    {
        .id              = "artic",
        .display         = "Art Institute of Chicago",
        .museum_enum     = ART_INSTITUTION_MUSEUM_ARTIC,
        .refresh_channel = art_institution_artic_refresh_channel,
        .build_iiif_url  = art_institution_artic_build_iiif_url,
        .resolve_entry   = NULL,  // AIC returns the image_id directly; no resolution needed.
    },
    {
        .id              = "rijks",
        .display         = "Rijksmuseum",
        .museum_enum     = ART_INSTITUTION_MUSEUM_RIJKS,
        .refresh_channel = art_institution_rijks_refresh_channel,
        .build_iiif_url  = art_institution_rijks_build_iiif_url,
        .resolve_entry   = art_institution_rijks_resolve_entry,
    },
    {
        .id              = "vam",
        .display         = "Victoria and Albert Museum",
        .museum_enum     = ART_INSTITUTION_MUSEUM_VAM,
        .refresh_channel = art_institution_vam_refresh_channel,
        .build_iiif_url  = art_institution_vam_build_iiif_url,
        .resolve_entry   = NULL,  // V&A returns _primaryImageId in the listing — no resolution.
    },
    {
        .id              = "wellcome",
        .display         = "Wellcome Collection",
        .museum_enum     = ART_INSTITUTION_MUSEUM_WELLCOME,
        .refresh_channel = art_institution_wellcome_refresh_channel,
        .build_iiif_url  = art_institution_wellcome_build_iiif_url,
        .resolve_entry   = NULL,  // Wellcome returns the IIIF id inline; no walk.
    },
    {
        .id              = "smk",
        .display         = "Statens Museum for Kunst",
        .museum_enum     = ART_INSTITUTION_MUSEUM_SMK,
        .refresh_channel = art_institution_smk_refresh_channel,
        .build_iiif_url  = art_institution_smk_build_iiif_url,
        .resolve_entry   = NULL,  // SMK returns image_iiif_id inline; no walk.
    },
    {
        .id              = "ham",
        .display         = "Harvard Art Museums",
        .museum_enum     = ART_INSTITUTION_MUSEUM_HAM,
        .refresh_channel = art_institution_ham_refresh_channel,
        .build_iiif_url  = art_institution_ham_build_iiif_url,
        .resolve_entry   = NULL,  // HAM's images[0].baseimageurl is inline; the
                                  // NRS→IDS 303 redirect is handled by the
                                  // download path's redirect shim, not a
                                  // resolve_entry walk.
    },
    {
        .id              = "si",
        .display         = "Smithsonian",
        .museum_enum     = ART_INSTITUTION_MUSEUM_SI,
        .refresh_channel = art_institution_si_refresh_channel,
        .build_iiif_url  = art_institution_si_build_iiif_url,
        .resolve_entry   = NULL,  // Smithsonian returns idsId inline in the
                                  // search response; the IIIF URL is built
                                  // from it directly, no walk needed.
    },
};

const size_t ART_INSTITUTION_MUSEUM_COUNT =
    sizeof(ART_INSTITUTION_MUSEUMS) / sizeof(ART_INSTITUTION_MUSEUMS[0]);

// Build-time guard: the dispatch table above must list one entry per museum
// in the enum. If you add a value to museum_id_t, append the corresponding
// row to ART_INSTITUTION_MUSEUMS — this static_assert will fail otherwise.
_Static_assert(
    sizeof(ART_INSTITUTION_MUSEUMS) / sizeof(ART_INSTITUTION_MUSEUMS[0])
        == (size_t)ART_INSTITUTION_NUM_MUSEUMS,
    "ART_INSTITUTION_MUSEUMS must have one entry per museum_id_t value");

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

int art_institution_enum_from_id(const char *museum_id)
{
    const art_institution_museum_t *m = art_institution_find(museum_id);
    return m ? (int)m->museum_enum : -1;
}

const char *art_institution_id_from_enum(uint16_t museum_enum)
{
    for (size_t i = 0; i < ART_INSTITUTION_MUSEUM_COUNT; i++) {
        if ((uint16_t)ART_INSTITUTION_MUSEUMS[i].museum_enum == museum_enum) {
            return ART_INSTITUTION_MUSEUMS[i].id;
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

// The FAT-safe filename sanitizer used to live here (local static
// sanitize_filename). It was promoted to a shared sd_path helper so the
// pin-storage path builder in pin_lists.c can apply the same substitution
// without duplicating the logic. See sd_path_sanitize_filename().

esp_err_t art_institution_build_vault_path(const char *museum_id,
                                           const institution_channel_entry_t *entry,
                                           char *out_path, size_t out_len)
{
    if (!museum_id || !entry || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;

    char museum_root[128];
    if (sd_path_get_museum(museum_root, sizeof(museum_root)) != ESP_OK) {
        strlcpy(museum_root, "/sdcard/p3a/museum", sizeof(museum_root));
    }
    // The shard base carries the per-museum segment: {museum_root}/{museum_id}.
    char base[160];
    snprintf(base, sizeof(base), "%s/%s", museum_root, museum_id);

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

    // iiif_key is stored in canonical (un-sanitized) form; the shared builder
    // sanitizes it for FAT internally (HAM's URN-shaped keys carry colons that
    // land on disk as '_') and derives the shard directories from that
    // sanitized leaf, so the on-disk location is always re-derivable from the
    // filename alone.
    return sd_path_build_sharded(base, entry->iiif_key, ext,
                                 out_path, out_len);
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
