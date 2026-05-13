// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists_types.h
 * @brief On-disk and in-memory record types for the pin-lists module
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/*  Constants                                                                */
/* ------------------------------------------------------------------------- */

#define PIN_LISTS_MAX_LISTS       64
#define PIN_LIST_MAX_ENTRIES      4096

/** Slug length: 8 lowercase hex chars + NUL terminator. */
#define PIN_LIST_SLUG_LEN         9

/** Maximum bytes (including NUL) for a list's user-facing name. */
#define PIN_LIST_NAME_MAX         64

/** Maximum bytes (including NUL) for a per-pin source identifier. */
#define PINNED_SOURCE_ID_MAX      56

/** Maximum bytes (including NUL) for a pin title. */
#define PINNED_TITLE_MAX          256

/** Maximum bytes (including NUL) for a pin creator handle. */
#define PINNED_CREATOR_MAX        128

/** Maximum bytes (including NUL) for an IIIF identifier. */
#define PINNED_IIIF_KEY_MAX       48

/** Maximum bytes (including NUL) for a Giphy identifier. */
#define PINNED_GIPHY_ID_MAX       24

/* ------------------------------------------------------------------------- */
/*  Format version + magic numbers                                           */
/* ------------------------------------------------------------------------- */

#define PINNED_FORMAT_VERSION     1u
#define PINNED_STATE_MAGIC        0x50535441u  /* 'PSTA' */
#define PINNED_ORDER_MAGIC        0x504F5244u  /* 'PORD' */
#define PINNED_ENTRY_MAGIC        0x50454E54u  /* 'PENT' */

/* ------------------------------------------------------------------------- */
/*  Source enum                                                              */
/* ------------------------------------------------------------------------- */

typedef enum {
    PINNED_SOURCE_NONE        = 0,
    PINNED_SOURCE_MAKAPIX     = 1,
    PINNED_SOURCE_GIPHY       = 2,
    PINNED_SOURCE_INSTITUTION = 3,
} pinned_source_t;

/* ------------------------------------------------------------------------- */
/*  state.bin (active list pointer)                                          */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint32_t magic;                 /* PINNED_STATE_MAGIC */
    uint32_t version;               /* PINNED_FORMAT_VERSION */
    char     active_slug[12];       /* 8 hex chars + NUL + 3 bytes pad */
    uint32_t crc32;                 /* CRC over preceding 20 bytes */
    uint8_t  reserved[8];
} pinned_state_t;
_Static_assert(sizeof(pinned_state_t) == 32, "pinned_state_t must be 32 bytes");

/* ------------------------------------------------------------------------- */
/*  order.bin header + entries                                               */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint32_t magic;                 /* PINNED_ORDER_MAGIC */
    uint32_t version;               /* PINNED_FORMAT_VERSION */
    uint32_t count;                 /* number of entries that follow */
    uint32_t crc32;                 /* CRC over the count * sizeof(entry) bytes */
} pinned_order_header_t;
_Static_assert(sizeof(pinned_order_header_t) == 16, "pinned_order_header_t must be 16 bytes");

/**
 * @brief One record in order.bin (also serves as the play_scheduler channel
 *        cache record).
 *
 * Lean by design: holds only what is needed to (a) sort by `pinned_at`,
 * (b) reconstruct the source URL, and (c) feed the play_scheduler picker.
 * Rich metadata (title, creator, original_post_id) lives in the per-pin
 * entry file at `entries/{source}_{key_hash}.bin`.
 */
typedef struct __attribute__((packed)) {
    int32_t  post_id;               /* list-local monotonic id */
    uint32_t pinned_at;             /* unix seconds, descending sort key */
    uint8_t  source;                /* pinned_source_t */
    uint8_t  extension;             /* 0=webp, 1=gif, 2=png, 3=jpg */
    uint16_t reserved;
    union {
        struct {
            uint8_t storage_key_uuid[16]; /* SHA prefix is derived via storage_key_sha256() */
            uint8_t pad[36];
        } makapix;
        struct {
            char    giphy_id[PINNED_GIPHY_ID_MAX];
            uint8_t pad[28];
        } giphy;
        struct {
            uint16_t museum_id;
            char     iiif_key[PINNED_IIIF_KEY_MAX];
            uint8_t  pad[2];
        } museum;
    };
} pinned_order_entry_t;
_Static_assert(sizeof(pinned_order_entry_t) == 64, "pinned_order_entry_t must be 64 bytes");

/* ------------------------------------------------------------------------- */
/*  Per-pin entry file (rich metadata, opened on demand)                     */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint32_t magic;                 /* PINNED_ENTRY_MAGIC */
    uint32_t version;               /* PINNED_FORMAT_VERSION */
    int32_t  post_id;               /* matches order.bin */
    int32_t  original_post_id;      /* for reaction routing */
    uint32_t pinned_at;
    uint32_t original_created_at;
    uint8_t  source;
    uint8_t  extension;
    uint16_t museum_id;             /* only meaningful for INSTITUTION */
    char     source_id[PINNED_SOURCE_ID_MAX];
    char     title[PINNED_TITLE_MAX];
    char     creator[PINNED_CREATOR_MAX];
    uint8_t  reserved[44];
} pinned_entry_file_t;
_Static_assert(sizeof(pinned_entry_file_t) == 512, "pinned_entry_file_t must be 512 bytes");

/* ------------------------------------------------------------------------- */
/*  Public info struct (enumeration result)                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    char     slug[PIN_LIST_SLUG_LEN];
    char     name[PIN_LIST_NAME_MAX];
    uint32_t created_at;
    uint32_t count;
    bool     is_active;
} pin_list_info_t;

#ifdef __cplusplus
}
#endif
