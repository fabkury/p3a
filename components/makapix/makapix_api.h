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
    char user_handle[64];   // Required when channel == BY_USER
    makapix_sort_mode_t sort;
    bool has_cursor;
    char cursor[64];
    uint8_t limit;          // 1-50
    bool random_seed_present;
    uint32_t random_seed;
} makapix_query_request_t;

typedef struct {
    int post_id;
    char storage_key[64];
    char art_url[256];
    char canvas[16];
    int width;
    int height;
    int frame_count;
    bool has_transparency;
    char owner_handle[64];
    char created_at[40];
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


