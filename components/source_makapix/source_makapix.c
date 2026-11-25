// SPDX-License-Identifier: Apache-2.0
// The Makapix Club source: its channels listed into records, and where each
// artwork's file is. See source_makapix.h and makapix_parse.h.

#include "source_makapix.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_fetch.h"
#include "makapix_internal.h"
#include "makapix_parse.h"
#include "makapix_rpc.h"
#include "makapix_store.h"
#include "mbedtls/sha256.h"
#include "p3a_err.h"
#include "p3a_job.h"
#include "p3a_mem.h"
#include "p3a_ui_hooks.h"
#include "p3a_settings.h"
#include "p3a_source.h"
#include "psram_alloc.h"

static const char *TAG = "makapix";

#define RESPONSE_BYTES (64 * 1024)  // a page of 50 posts is about 22 KB
#define URL_BYTES      768
// Between two pages. A player may ask 60 questions a minute; at this pace
// two channels listed one after the other stay under it. The public feed
// is served from a cache and needs less.
#define RPC_PAUSE_MS  1100
#define FEED_PAUSE_MS 200

static p3a_settings_t *settings_now(void)
{
    p3a_settings_t *settings = psram_malloc(sizeof(*settings));
    if (settings != NULL) {
        p3a_settings_get(settings);
    }
    return settings;
}

// Sleeps between pages in short steps, so that a playset change is not
// kept waiting by a pause.
static void pause_between_pages(const p3a_channel_ref_t *channel, uint32_t ms)
{
    for (uint32_t waited = 0; waited < ms && !p3a_channel_stopped(channel); waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t makapix_refresh(const p3a_channel_ref_t *channel, p3a_index_t *index)
{
    makapix_channel_t which;
    if (!makapix_channel_parse(channel->kind, channel->identifier, &which)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    // A registered player asks as itself: the server then shows what its
    // owner may see. Without a registration only the public promoted feed
    // can be listed.
    const bool registered = makapix_store_registered();
    if (!registered && which != MAKAPIX_CHANNEL_PROMOTED) {
        return P3A_ERR_NO_KEY;
    }
    const bool promoted = which == MAKAPIX_CHANNEL_PROMOTED;
    char *body = psram_malloc(RESPONSE_BYTES);
    char *ask = psram_malloc(URL_BYTES);
    p3a_record_t *records = psram_malloc(MAKAPIX_PAGE_POSTS * sizeof(p3a_record_t));
    makapix_page_t *page = psram_calloc(1, sizeof(*page));
    esp_err_t err = body != NULL && ask != NULL && records != NULL && page != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    uint32_t pages = 0;
    while (err == ESP_OK && index->count < index->capacity) {
        if (p3a_channel_stopped(channel)) {
            err = ESP_ERR_NOT_FINISHED;  // the playset changed: nobody wants this listing
            break;
        }
        const char *cursor = pages > 0 ? page->cursor : NULL;
        if (registered) {
            err = makapix_query_body(which, channel->identifier, cursor, ask, URL_BYTES);
            if (err == ESP_OK) {
                err = makapix_rpc(ask, body, RESPONSE_BYTES, channel->stop, channel->stop_ctx);
            }
        } else {
            err = makapix_feed_url(cursor, ask, URL_BYTES);
            if (err == ESP_OK) {
                const http_fetch_request_t req = {.url = ask,
                                                  .user_agent = makapix_user_agent(),
                                                  .should_abort = channel->stop,
                                                  .user_ctx = channel->stop_ctx};
                size_t got = 0;
                err = http_fetch_to_buffer(&req, body, RESPONSE_BYTES, &got, NULL);
            }
        }
        if (err == ESP_OK) {
            err = makapix_parse_page(body, promoted, records, MAKAPIX_PAGE_POSTS, page);
        }
        if (err != ESP_OK) {
            if (pages > 0 && !p3a_channel_stopped(channel)) {
                // The pages already read are a listing; the rest comes with the next one.
                ESP_LOGW(TAG, "page %u could not be read (%s); keeping %u records", (unsigned)pages + 1,
                         p3a_err_to_name(err), (unsigned)index->count);
                err = ESP_OK;
            }
            break;
        }
        pages++;
        for (uint32_t i = 0; i < page->count && index->count < index->capacity; i++) {
            // A post that moved between two pages while they were asked for comes twice: refused, and fine.
            p3a_index_append(index, &records[i], NULL);
        }
        if (!page->has_more || page->items == 0) {
            break;
        }
        pause_between_pages(channel, registered ? RPC_PAUSE_MS : FEED_PAUSE_MS);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s%s%s: %u artworks in %u pages%s", makapix_channel_name(which),
                 channel->identifier != NULL && channel->identifier[0] != '\0' ? " " : "",
                 channel->identifier != NULL ? channel->identifier : "", (unsigned)index->count, (unsigned)pages,
                 registered ? "" : " (the public feed)");
    }
    free(page);
    free(records);
    free(ask);
    free(body);
    return err;
}

static esp_err_t makapix_url(const p3a_record_t *record, char *out, size_t len)
{
    char key[MAKAPIX_UUID_TEXT];
    uint8_t digest[32];
    makapix_uuid_text(record->identity.makapix.uuid, key);
    if (mbedtls_sha256((const unsigned char *)key, strlen(key), digest, 0) != 0) {
        return ESP_FAIL;
    }
    return makapix_vault_url(record, digest, out, len);
}

static uint32_t makapix_refresh_interval_s(void)
{
    p3a_settings_t *settings = settings_now();
    const uint32_t s = settings != NULL ? (uint32_t)settings->makapix_refresh_s : 14400;
    free(settings);
    return s;
}

static uint32_t makapix_index_capacity(void)
{
    p3a_settings_t *settings = settings_now();
    const uint32_t n = settings != NULL ? (uint32_t)settings->makapix_cache_size : 2048;
    free(settings);
    return n;
}

void source_makapix_status(source_makapix_status_t *out)
{
    memset(out, 0, sizeof(*out));
    makapix_identity_t identity;
    out->registered = makapix_store_registered() && makapix_store_identity(&identity) == ESP_OK;
    if (out->registered) {
        memcpy(out->player_key, identity.player_key, sizeof(out->player_key));
    }
    makapix_register_status(out);
    makapix_link_status(out);
    makapix_renew_status(out);
}

// --- a like ---------------------------------------------------------------------------
//
// A swipe up on a Makapix artwork is a thumbs-up from the player's owner; a
// swipe down takes it back. The icon answers at once and the server is told
// by a job. If the server does not take it, the error icon says so.

typedef struct {
    uint32_t post_id;
    bool revoke;
} like_t;

static esp_err_t like_job(void *arg)
{
    like_t *like = arg;
    char body[128], answer[512];
    esp_err_t err = p3a_mem_level() == P3A_MEM_OK ? makapix_reaction_body(like->post_id, like->revoke, body, sizeof(body))
                                                  : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = makapix_rpc(body, answer, sizeof(answer), NULL, NULL);
    }
    if (err == ESP_OK) {
        err = makapix_parse_result(answer);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "the %s of post %u was not taken: %s", like->revoke ? "unlike" : "like", (unsigned)like->post_id,
                 p3a_err_to_name(err));
        p3a_ui_icon_show(P3A_ICON_ERROR, P3A_ICON_TOP_RIGHT);
    }
    free(like);
    return err;
}

static p3a_reaction_t makapix_react(const p3a_record_t *record, bool undo)
{
    if (!makapix_store_registered()) {
        return P3A_REACTION_NONE;  // nobody to like it as
    }
    like_t *like = psram_malloc(sizeof(*like));
    if (like == NULL) {
        return P3A_REACTION_NONE;
    }
    like->post_id = record->id32;
    like->revoke = undo;
    if (p3a_job_submit(P3A_JOBS_IO, undo ? "makapix unlike" : "makapix like", like_job, like, NULL) != ESP_OK) {
        free(like);
        return P3A_REACTION_NONE;
    }
    return undo ? P3A_REACTION_UNLIKED : P3A_REACTION_LIKED;
}

esp_err_t source_makapix_start(void)
{
    static const p3a_source_ops_t ops = {
        .source = P3A_SOURCE_MAKAPIX,
        .name = "makapix",
        .refresh = makapix_refresh,
        .remote = true,
        .url = makapix_url,
        .react = makapix_react,
        .shown = makapix_link_shown,
        .user_agent = NULL,  // set below: it holds the version
        .refresh_interval_s = makapix_refresh_interval_s,
        .index_capacity = makapix_index_capacity,
        .smooth = false,  // pixel art: every pixel sharp
    };
    static p3a_source_ops_t table;
    table = ops;
    table.user_agent = makapix_user_agent();
    esp_err_t err = makapix_rpc_start();
    if (err == ESP_OK) {
        err = makapix_link_start();
    }
    if (err == ESP_OK) {
        err = makapix_renew_start();
    }
    if (err == ESP_OK) {
        err = makapix_task_start();  // last: its first tick uses all of the above
    }
    return err != ESP_OK ? err : p3a_source_register(&table);
}
