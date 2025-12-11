#include "makapix_api.h"
#include "makapix_mqtt.h"
#include "makapix_store.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "makapix_api";

#define MAKAPIX_REQUEST_TIMEOUT_MS 30000
#define MAKAPIX_MAX_RETRIES 3

typedef struct pending_request_s {
    char request_id[48];
    char *response_str;
    SemaphoreHandle_t done_sem;
    struct pending_request_s *next;
} pending_request_t;

static SemaphoreHandle_t s_pending_mutex = NULL;
static pending_request_t *s_pending_head = NULL;
static char s_player_key[37] = {0};

// Forward declarations
static void response_callback(const char *topic, const char *data, int data_len);
static void generate_request_id(char *out, size_t len);
static pending_request_t *pending_find(const char *request_id);
static void pending_add(pending_request_t *req);
static void pending_remove(pending_request_t *req);

static const char *channel_to_string(makapix_channel_type_t channel)
{
    switch (channel) {
    case MAKAPIX_CHANNEL_ALL: return "all";
    case MAKAPIX_CHANNEL_PROMOTED: return "promoted";
    case MAKAPIX_CHANNEL_USER: return "user";
    case MAKAPIX_CHANNEL_BY_USER: return "by_user";
    case MAKAPIX_CHANNEL_ARTWORK_SINGLE: return "artwork";
    default: return "all";
    }
}

static const char *sort_to_string(makapix_sort_mode_t sort)
{
    switch (sort) {
    case MAKAPIX_SORT_CREATED_AT: return "created_at";
    case MAKAPIX_SORT_RANDOM: return "random";
    case MAKAPIX_SORT_SERVER_ORDER:
    default: return "server_order";
    }
}

static const char *intent_to_string(makapix_view_intent_t intent)
{
    return (intent == MAKAPIX_VIEW_INTENT_INTENTIONAL) ? "intentional" : "automated";
}

esp_err_t makapix_api_init(void)
{
    if (!s_pending_mutex) {
        s_pending_mutex = xSemaphoreCreateMutex();
        if (!s_pending_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Load player_key from store (used for request topic)
    if (makapix_store_get_player_key(s_player_key, sizeof(s_player_key)) != ESP_OK) {
        ESP_LOGE(TAG, "Player key not available; cannot initialize API");
        return ESP_ERR_NOT_FOUND;
    }

    // Register response callback
    makapix_mqtt_set_response_callback(response_callback);
    return ESP_OK;
}

static void generate_request_id(char *out, size_t len)
{
    // 16 bytes random => 32 hex chars
    const size_t req_len = 32;
    if (len < req_len + 1) return;
    for (size_t i = 0; i < req_len/2; i++) {
        uint8_t b = (uint8_t)(esp_random() & 0xFF);
        snprintf(out + (i * 2), 3, "%02x", b);
    }
    out[req_len] = '\0';
}

static void response_callback(const char *topic, const char *data, int data_len)
{
    ESP_LOGD(TAG, "Response callback invoked: topic=%s, len=%d", topic ? topic : "(NULL)", data_len);
    
    if (!topic || !data || data_len <= 0) {
        ESP_LOGW(TAG, "Invalid callback parameters - returning");
        return;
    }

    // Copy payload to null-terminated buffer
    char *payload = malloc(data_len + 1);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate payload buffer");
        ESP_LOGI(TAG, "========================================");
        return;
    }
    memcpy(payload, data, data_len);
    payload[data_len] = '\0';

    cJSON *json = cJSON_Parse(payload);
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        free(payload);
        return;
    }

    cJSON *id = cJSON_GetObjectItem(json, "request_id");
    if (!id || !cJSON_IsString(id)) {
        ESP_LOGW(TAG, "No request_id field found in response");
        cJSON_Delete(json);
        free(payload);
        return;
    }

    const char *req_id = cJSON_GetStringValue(id);
    if (!req_id) {
        ESP_LOGW(TAG, "request_id is NULL");
        cJSON_Delete(json);
        free(payload);
        return;
    }

    ESP_LOGD(TAG, "Matching response to request_id: %s", req_id);
    pending_request_t *pending = pending_find(req_id);
    if (pending) {
        ESP_LOGI(TAG, "Response received for request %s", req_id);
        // Replace any previous payload
        if (pending->response_str) {
            free(pending->response_str);
        }
        pending->response_str = payload;
        // Signal completion
        xSemaphoreGive(pending->done_sem);
    } else {
        ESP_LOGW(TAG, "No matching pending request for %s - ignoring response", req_id);
        free(payload);
    }

    cJSON_Delete(json);
}

static pending_request_t *pending_find(const char *request_id)
{
    if (!s_pending_mutex || !request_id) return NULL;
    pending_request_t *result = NULL;

    if (xSemaphoreTake(s_pending_mutex, portMAX_DELAY) == pdTRUE) {
        pending_request_t *it = s_pending_head;
        while (it) {
            if (strcmp(it->request_id, request_id) == 0) {
                result = it;
                break;
            }
            it = it->next;
        }
        xSemaphoreGive(s_pending_mutex);
    }
    return result;
}

static void pending_add(pending_request_t *req)
{
    if (!s_pending_mutex || !req) return;
    if (xSemaphoreTake(s_pending_mutex, portMAX_DELAY) == pdTRUE) {
        req->next = s_pending_head;
        s_pending_head = req;
        ESP_LOGD(TAG, "Added pending request: request_id=%s", req->request_id);
        xSemaphoreGive(s_pending_mutex);
    }
}

static void pending_remove(pending_request_t *req)
{
    if (!s_pending_mutex || !req) return;
    if (xSemaphoreTake(s_pending_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_LOGD(TAG, "Removing pending request: request_id=%s", req->request_id);
        pending_request_t **pp = &s_pending_head;
        while (*pp) {
            if (*pp == req) {
                *pp = req->next;
                break;
            }
            pp = &((*pp)->next);
        }
        xSemaphoreGive(s_pending_mutex);
    }
}

static esp_err_t build_request_topic(const char *request_id, char *out, size_t out_len)
{
    if (!request_id || strlen(s_player_key) == 0 || out_len < 128) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, out_len, "makapix/player/%s/request/%s", s_player_key, request_id);
    return ESP_OK;
}

static esp_err_t publish_and_wait(cJSON *request_obj, cJSON **out_response)
{
    if (!request_obj || !out_response) return ESP_ERR_INVALID_ARG;

    // Check MQTT connection first
    if (!makapix_mqtt_is_connected()) {
        ESP_LOGE(TAG, "Cannot publish request: MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Wait for response topic subscription to be confirmed (max 5 seconds)
    // This is critical: server won't route responses if we're not subscribed
    if (!makapix_mqtt_is_ready()) {
        ESP_LOGI(TAG, "Waiting for response topic subscription...");
        const int max_wait_ms = 5000;
        const int poll_ms = 100;
        int waited = 0;
        while (!makapix_mqtt_is_ready() && waited < max_wait_ms) {
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
            waited += poll_ms;
        }
        if (!makapix_mqtt_is_ready()) {
            ESP_LOGE(TAG, "Response topic subscription not confirmed after %d ms", max_wait_ms);
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "Response topic subscription confirmed after %d ms", waited);
    }

    char request_id[48] = {0};
    generate_request_id(request_id, sizeof(request_id));

    cJSON_AddStringToObject(request_obj, "request_id", request_id);
    cJSON_AddStringToObject(request_obj, "player_key", s_player_key);

    char topic[160];
    ESP_RETURN_ON_ERROR(build_request_topic(request_id, topic, sizeof(topic)), TAG, "topic build failed");

    char *payload = cJSON_PrintUnformatted(request_obj);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGD(TAG, "Request topic: %s", topic);
    ESP_LOGD(TAG, "Request payload: %s", payload);

    pending_request_t pending = {0};
    memcpy(pending.request_id, request_id, sizeof(pending.request_id) - 1);
    pending.request_id[sizeof(pending.request_id) - 1] = '\0';
    pending.done_sem = xSemaphoreCreateBinary();
    if (!pending.done_sem) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    pending.response_str = NULL;
    pending.next = NULL;

    pending_add(&pending);

    esp_err_t result = ESP_FAIL;
    uint32_t delay_ms = 1000;

    for (int attempt = 0; attempt < MAKAPIX_MAX_RETRIES; attempt++) {
        // Check connection and subscription before each attempt
        if (!makapix_mqtt_is_ready()) {
            ESP_LOGW(TAG, "MQTT not ready during request %s (connected=%d), aborting", 
                     request_id, makapix_mqtt_is_connected());
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        
        ESP_LOGI(TAG, "Publishing request %s (attempt %d/%d)", request_id, attempt + 1, MAKAPIX_MAX_RETRIES);
        esp_err_t pub_err = makapix_mqtt_publish_raw(topic, payload, 1);
        if (pub_err != ESP_OK) {
            ESP_LOGE(TAG, "Publish failed: %s", esp_err_to_name(pub_err));
            ESP_LOGW(TAG, "Publish failed: %s", esp_err_to_name(pub_err));
            // If not connected, don't retry
            if (pub_err == ESP_ERR_INVALID_STATE) {
                result = pub_err;
                break;
            }
        } else {
            ESP_LOGD(TAG, "Waiting for response to %s (timeout: %d ms)", request_id, MAKAPIX_REQUEST_TIMEOUT_MS);
            TickType_t start_ticks = xTaskGetTickCount();
            BaseType_t sem_result = xSemaphoreTake(pending.done_sem, pdMS_TO_TICKS(MAKAPIX_REQUEST_TIMEOUT_MS));
            TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
            if (sem_result == pdTRUE) {
                ESP_LOGI(TAG, "Response received for %s after %lu ms", request_id, 
                         (unsigned long)(elapsed_ticks * portTICK_PERIOD_MS));
                result = ESP_OK;
                break;
            } else {
                ESP_LOGW(TAG, "Timeout waiting for response to %s (waited %lu ms)", request_id,
                         (unsigned long)(elapsed_ticks * portTICK_PERIOD_MS));
            }
        }
        // Backoff before retry
        if (attempt < MAKAPIX_MAX_RETRIES - 1) {
            ESP_LOGD(TAG, "Retrying in %lu ms...", (unsigned long)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            delay_ms = (delay_ms * 2 > 60000) ? 60000 : delay_ms * 2;
        }
    }

    if (result == ESP_OK && pending.response_str) {
        cJSON *resp_json = cJSON_Parse(pending.response_str);
        if (resp_json) {
            *out_response = resp_json;
        } else {
            ESP_LOGE(TAG, "Failed to parse response JSON");
            result = ESP_ERR_INVALID_RESPONSE;
        }
    } else if (result == ESP_OK) {
        ESP_LOGE(TAG, "Response marked OK but no response_str");
        result = ESP_ERR_INVALID_RESPONSE;
    }

    pending_remove(&pending);
    if (pending.response_str) free(pending.response_str);
    vSemaphoreDelete(pending.done_sem);
    free(payload);
    return result;
}

static void parse_post_object(cJSON *post_obj, makapix_post_t *out_post)
{
    if (!post_obj || !out_post) return;
    memset(out_post, 0, sizeof(*out_post));

    cJSON *id = cJSON_GetObjectItem(post_obj, "post_id");
    if (cJSON_IsNumber(id)) out_post->post_id = (int)id->valuedouble;
    
    // Parse kind field (artwork or playlist)
    cJSON *kind = cJSON_GetObjectItem(post_obj, "kind");
    if (cJSON_IsString(kind)) {
        const char *kind_str = cJSON_GetStringValue(kind);
        if (strcmp(kind_str, "playlist") == 0) {
            out_post->kind = MAKAPIX_POST_KIND_PLAYLIST;
        } else {
            out_post->kind = MAKAPIX_POST_KIND_ARTWORK;
        }
    } else {
        out_post->kind = MAKAPIX_POST_KIND_ARTWORK;  // Default to artwork
    }
    
    // Common fields for all posts
    cJSON *owner = cJSON_GetObjectItem(post_obj, "owner_handle");
    if (cJSON_IsString(owner)) strncpy(out_post->owner_handle, owner->valuestring, sizeof(out_post->owner_handle) - 1);

    cJSON *created = cJSON_GetObjectItem(post_obj, "created_at");
    if (cJSON_IsString(created)) strncpy(out_post->created_at, created->valuestring, sizeof(out_post->created_at) - 1);
    
    cJSON *metadata_modified = cJSON_GetObjectItem(post_obj, "metadata_modified_at");
    if (cJSON_IsString(metadata_modified)) strncpy(out_post->metadata_modified_at, metadata_modified->valuestring, sizeof(out_post->metadata_modified_at) - 1);
    
    if (out_post->kind == MAKAPIX_POST_KIND_PLAYLIST) {
        // Parse playlist-specific fields
        cJSON *total_artworks = cJSON_GetObjectItem(post_obj, "total_artworks");
        if (cJSON_IsNumber(total_artworks)) out_post->total_artworks = (int)total_artworks->valuedouble;
        
        cJSON *dwell_time = cJSON_GetObjectItem(post_obj, "dwell_time_ms");
        if (cJSON_IsNumber(dwell_time)) out_post->playlist_dwell_time_ms = (uint32_t)dwell_time->valuedouble;
        
        // Parse artworks array
        cJSON *artworks = cJSON_GetObjectItem(post_obj, "artworks");
        if (cJSON_IsArray(artworks)) {
            size_t count = cJSON_GetArraySize(artworks);
            if (count > 0) {
                out_post->artworks = (makapix_artwork_t *)calloc(count, sizeof(makapix_artwork_t));
                if (out_post->artworks) {
                    out_post->artworks_count = 0;
                    for (size_t i = 0; i < count; i++) {
                        cJSON *artwork_obj = cJSON_GetArrayItem(artworks, i);
                        if (artwork_obj) {
                            makapix_artwork_t *artwork = &out_post->artworks[out_post->artworks_count];
                            
                            cJSON *aid = cJSON_GetObjectItem(artwork_obj, "post_id");
                            if (cJSON_IsNumber(aid)) artwork->post_id = (int)aid->valuedouble;
                            
                            cJSON *storage = cJSON_GetObjectItem(artwork_obj, "storage_key");
                            if (cJSON_IsString(storage)) strncpy(artwork->storage_key, storage->valuestring, sizeof(artwork->storage_key) - 1);
                            
                            cJSON *url = cJSON_GetObjectItem(artwork_obj, "art_url");
                            if (cJSON_IsString(url)) strncpy(artwork->art_url, url->valuestring, sizeof(artwork->art_url) - 1);
                            
                            cJSON *canvas = cJSON_GetObjectItem(artwork_obj, "canvas");
                            if (cJSON_IsString(canvas)) strncpy(artwork->canvas, canvas->valuestring, sizeof(artwork->canvas) - 1);
                            
                            cJSON *w = cJSON_GetObjectItem(artwork_obj, "width");
                            if (cJSON_IsNumber(w)) artwork->width = (int)w->valuedouble;
                            
                            cJSON *h = cJSON_GetObjectItem(artwork_obj, "height");
                            if (cJSON_IsNumber(h)) artwork->height = (int)h->valuedouble;
                            
                            cJSON *fc = cJSON_GetObjectItem(artwork_obj, "frame_count");
                            if (cJSON_IsNumber(fc)) artwork->frame_count = (int)fc->valuedouble;
                            
                            cJSON *ht = cJSON_GetObjectItem(artwork_obj, "has_transparency");
                            artwork->has_transparency = cJSON_IsBool(ht) ? cJSON_IsTrue(ht) : false;
                            
                            cJSON *aowner = cJSON_GetObjectItem(artwork_obj, "owner_handle");
                            if (cJSON_IsString(aowner)) strncpy(artwork->owner_handle, aowner->valuestring, sizeof(artwork->owner_handle) - 1);
                            
                            cJSON *acreated = cJSON_GetObjectItem(artwork_obj, "created_at");
                            if (cJSON_IsString(acreated)) strncpy(artwork->created_at, acreated->valuestring, sizeof(artwork->created_at) - 1);
                            
                            cJSON *ameta_modified = cJSON_GetObjectItem(artwork_obj, "metadata_modified_at");
                            if (cJSON_IsString(ameta_modified)) strncpy(artwork->metadata_modified_at, ameta_modified->valuestring, sizeof(artwork->metadata_modified_at) - 1);
                            
                            cJSON *aart_modified = cJSON_GetObjectItem(artwork_obj, "artwork_modified_at");
                            if (cJSON_IsString(aart_modified)) strncpy(artwork->artwork_modified_at, aart_modified->valuestring, sizeof(artwork->artwork_modified_at) - 1);
                            
                            cJSON *adwell = cJSON_GetObjectItem(artwork_obj, "dwell_time_ms");
                            if (cJSON_IsNumber(adwell)) artwork->dwell_time_ms = (uint32_t)adwell->valuedouble;
                            
                            out_post->artworks_count++;
                        }
                    }
                }
            }
        }
    } else {
        // Parse artwork-specific fields
        cJSON *storage = cJSON_GetObjectItem(post_obj, "storage_key");
        if (cJSON_IsString(storage)) strncpy(out_post->storage_key, storage->valuestring, sizeof(out_post->storage_key) - 1);

        cJSON *url = cJSON_GetObjectItem(post_obj, "art_url");
        if (cJSON_IsString(url)) strncpy(out_post->art_url, url->valuestring, sizeof(out_post->art_url) - 1);

        cJSON *canvas = cJSON_GetObjectItem(post_obj, "canvas");
        if (cJSON_IsString(canvas)) strncpy(out_post->canvas, canvas->valuestring, sizeof(out_post->canvas) - 1);

        cJSON *w = cJSON_GetObjectItem(post_obj, "width");
        if (cJSON_IsNumber(w)) out_post->width = (int)w->valuedouble;

        cJSON *h = cJSON_GetObjectItem(post_obj, "height");
        if (cJSON_IsNumber(h)) out_post->height = (int)h->valuedouble;

        cJSON *fc = cJSON_GetObjectItem(post_obj, "frame_count");
        if (cJSON_IsNumber(fc)) out_post->frame_count = (int)fc->valuedouble;

        cJSON *ht = cJSON_GetObjectItem(post_obj, "has_transparency");
        out_post->has_transparency = cJSON_IsBool(ht) ? cJSON_IsTrue(ht) : false;
        
        cJSON *artwork_modified = cJSON_GetObjectItem(post_obj, "artwork_modified_at");
        if (cJSON_IsString(artwork_modified)) strncpy(out_post->artwork_modified_at, artwork_modified->valuestring, sizeof(out_post->artwork_modified_at) - 1);
        
        cJSON *dwell_time = cJSON_GetObjectItem(post_obj, "dwell_time_ms");
        if (cJSON_IsNumber(dwell_time)) out_post->dwell_time_ms = (uint32_t)dwell_time->valuedouble;
    }
}

static void parse_query_response(cJSON *resp_json, makapix_query_response_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *success = cJSON_GetObjectItem(resp_json, "success");
    out->success = cJSON_IsBool(success) ? cJSON_IsTrue(success) : false;

    cJSON *err = cJSON_GetObjectItem(resp_json, "error");
    if (cJSON_IsString(err)) strncpy(out->error, err->valuestring, sizeof(out->error) - 1);

    cJSON *err_code = cJSON_GetObjectItem(resp_json, "error_code");
    if (cJSON_IsString(err_code)) strncpy(out->error_code, err_code->valuestring, sizeof(out->error_code) - 1);

    cJSON *posts = cJSON_GetObjectItem(resp_json, "posts");
    if (cJSON_IsArray(posts)) {
        size_t count = cJSON_GetArraySize(posts);
        if (count > MAKAPIX_MAX_POSTS_PER_RESPONSE) count = MAKAPIX_MAX_POSTS_PER_RESPONSE;
        out->post_count = count;
        for (size_t i = 0; i < count; i++) {
            cJSON *post_obj = cJSON_GetArrayItem(posts, i);
            parse_post_object(post_obj, &out->posts[i]);
        }
    }

    cJSON *has_more = cJSON_GetObjectItem(resp_json, "has_more");
    out->has_more = cJSON_IsBool(has_more) ? cJSON_IsTrue(has_more) : false;

    cJSON *next_cursor = cJSON_GetObjectItem(resp_json, "next_cursor");
    if (cJSON_IsString(next_cursor)) strncpy(out->next_cursor, next_cursor->valuestring, sizeof(out->next_cursor) - 1);
}

esp_err_t makapix_api_query_posts(const makapix_query_request_t *req, makapix_query_response_t *resp)
{
    if (!req || !resp) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "request_type", "query_posts");
    cJSON_AddStringToObject(root, "channel", channel_to_string(req->channel));
    cJSON_AddStringToObject(root, "sort", sort_to_string(req->sort));

    if (req->channel == MAKAPIX_CHANNEL_BY_USER) {
        cJSON_AddStringToObject(root, "user_handle", req->user_handle);
    }

    if (req->has_cursor) {
        cJSON_AddStringToObject(root, "cursor", req->cursor);
    } else {
        cJSON_AddNullToObject(root, "cursor");
    }

    uint8_t limit = req->limit;
    if (limit == 0) limit = 30;
    if (limit > 50) limit = 50;
    cJSON_AddNumberToObject(root, "limit", limit);

    if (req->sort == MAKAPIX_SORT_RANDOM && req->random_seed_present) {
        cJSON_AddNumberToObject(root, "random_seed", req->random_seed);
    }
    
    // Add PE (playlist expansion) parameter
    if (req->pe > 0) {
        cJSON_AddNumberToObject(root, "PE", req->pe);
    }

    cJSON *response_json = NULL;
    esp_err_t err = publish_and_wait(root, &response_json);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return err;
    }

    parse_query_response(response_json, resp);
    cJSON_Delete(response_json);
    return ESP_OK;
}

esp_err_t makapix_api_submit_view(int32_t post_id, makapix_view_intent_t intent)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "request_type", "submit_view");
    cJSON_AddNumberToObject(root, "post_id", post_id);
    cJSON_AddStringToObject(root, "view_intent", intent_to_string(intent));

    cJSON *response_json = NULL;
    esp_err_t err = publish_and_wait(root, &response_json);
    cJSON_Delete(root);
    if (response_json) cJSON_Delete(response_json);
    return err;
}

// Stubs for future implementation
esp_err_t makapix_api_submit_reaction(int32_t post_id, const char *emoji)
{
    (void)post_id;
    (void)emoji;
    return ESP_OK;
}

esp_err_t makapix_api_revoke_reaction(int32_t post_id, const char *emoji)
{
    (void)post_id;
    (void)emoji;
    return ESP_OK;
}

esp_err_t makapix_api_get_comments(int32_t post_id, const char *cursor, uint8_t limit)
{
    (void)post_id;
    (void)cursor;
    (void)limit;
    return ESP_OK;
}


