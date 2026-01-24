// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MAKAPIX_CHANNEL_ALL,
    MAKAPIX_CHANNEL_PROMOTED,
    MAKAPIX_CHANNEL_USER,
    MAKAPIX_CHANNEL_BY_USER,
    MAKAPIX_CHANNEL_HASHTAG,        // Used for hashtag channels
    MAKAPIX_CHANNEL_ARTWORK_SINGLE, // Used for show_artwork
} makapix_channel_type_t;

typedef enum {
    MAKAPIX_SORT_SERVER_ORDER,
    MAKAPIX_SORT_CREATED_AT,
    MAKAPIX_SORT_RANDOM,
} makapix_sort_mode_t;

typedef enum {
    MAKAPIX_VIEW_INTENT_AUTOMATED,
    MAKAPIX_VIEW_INTENT_INTENTIONAL,
} makapix_view_intent_t;

typedef struct {
    makapix_channel_type_t channel;
    char user_sqid[64];     // Required when channel == BY_USER
    char hashtag[64];       // Required when channel == HASHTAG
    makapix_sort_mode_t sort;
    bool has_cursor;
    char cursor[64];
    uint8_t limit;          // 1-50
    bool random_seed_present;
    uint32_t random_seed;
    bool pe_present;        // True if PE should be sent (including PE=0)
    uint16_t pe;            // Playlist expansion: 0-1023 (0 = all)
} makapix_query_request_t;

/**
 * @brief Post kind enumeration
 */
typedef enum {
    MAKAPIX_POST_KIND_ARTWORK,
    MAKAPIX_POST_KIND_PLAYLIST,
} makapix_post_kind_t;

/**
 * @brief Artwork within a playlist
 */
typedef struct {
    int post_id;
    char storage_key[64];
    char art_url[256];
    char owner_handle[64];
    char created_at[40];
    char artwork_modified_at[40];
} makapix_artwork_t;

/**
 * @brief Post (can be artwork or playlist)
 */
typedef struct {
    int post_id;
    makapix_post_kind_t kind;
    char owner_handle[64];
    char created_at[40];

    // For artwork posts:
    char storage_key[64];
    char art_url[256];
    char artwork_modified_at[40];

    // For playlist posts:
    int total_artworks;              // Total artworks in playlist
    makapix_artwork_t *artworks;     // Array of artworks (up to PE count)
    size_t artworks_count;           // Number of artworks in array
} makapix_post_t;

#define MAKAPIX_MAX_POSTS_PER_RESPONSE 50

typedef struct {
    bool success;
    char error[96];
    char error_code[48];
    makapix_post_t posts[MAKAPIX_MAX_POSTS_PER_RESPONSE];
    size_t post_count;
    bool has_more;
    char next_cursor[64];
} makapix_query_response_t;

/**
 * @brief Initialize Makapix MQTT API layer
 */
esp_err_t makapix_api_init(void);

/**
 * @brief Query posts (channels) via MQTT
 */
esp_err_t makapix_api_query_posts(const makapix_query_request_t *req, makapix_query_response_t *resp);

/**
 * @brief Fetch a single post by post_id (artwork or playlist)
 *
 * If the returned post is a playlist, the caller must free `out_post->artworks`
 * when done.
 */
esp_err_t makapix_api_get_post(int32_t post_id, bool pe_present, uint16_t pe, makapix_post_t *out_post);

/**
 * @brief Submit a view event
 */
esp_err_t makapix_api_submit_view(int32_t post_id, makapix_view_intent_t intent);

/**
 * @brief Stub: submit reaction
 */
esp_err_t makapix_api_submit_reaction(int32_t post_id, const char *emoji);

/**
 * @brief Stub: revoke reaction
 */
esp_err_t makapix_api_revoke_reaction(int32_t post_id, const char *emoji);

/**
 * @brief Stub: get comments
 */
esp_err_t makapix_api_get_comments(int32_t post_id, const char *cursor, uint8_t limit);

#ifdef __cplusplus
}
#endif


