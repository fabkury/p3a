// SPDX-License-Identifier: Apache-2.0
// The scheduler. See play_scheduler.h.
//
// One recursive lock guards everything here. It is held for arithmetic and
// for handing a request to the player, never across a listing (which reads
// the card or the network): a listing fills an index of its own, and the
// lock is taken only to swap it in.

#include "play_scheduler.h"

#include <stdlib.h>
#include <string.h>

#include "animation_player.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_blob.h"
#include "p3a_err.h"
#include "p3a_index.h"
#include "p3a_job.h"
#include "p3a_settings.h"
#include "p3a_source.h"
#include "playset_store.h"
#include "psram_alloc.h"
#include "sched_core.h"
#include "sched_history.h"

static const char *TAG = "sched";

// Indices held in memory at once. A playset may name more channels; the
// rest are not loaded (loading on demand arrives with the remote sources).
#define RESIDENT_MAX     16
#define DEFAULT_CAPACITY 1024
#define OUR_TOKENS       0x80000000u  // the scheduler's requests to the player carry this bit

typedef enum { TURN_NEW, TURN_BACK, TURN_FORWARD } turn_kind_t;

typedef struct {
    p3a_index_t index;
    bool has_index;
    uint8_t state;  // play_channel_state_t
    esp_err_t error;
    char id[PLAYSET_CHANNEL_ID_LEN];
} channel_t;

typedef struct {
    playset_t playset;
    channel_t channels[PLAYSET_MAX_CHANNELS];
    uint32_t generation;  // changes with every activation; a listing of an older one is dropped
    sched_core_t core;
    sched_history_t history;

    // The request that is with the player, if any.
    uint32_t pending_token;
    turn_kind_t pending_kind;
    p3a_record_t pending_record;
    uint32_t pending_channel;
    uint32_t pending_generation;

    bool playing;
    p3a_record_t current;
    uint32_t next_token;
    uint32_t picks, shown, failed;
    esp_err_t last_error;
    bool list_job_queued;
    bool start_wanted;  // a new playset is in place and nothing of it has been asked for yet

    animation_player_listener_t foreign;
    void *foreign_ctx;
    p3a_settings_t settings;  // scratch for reading the settings; too large for a stack
} state_t;

static state_t *s;  // in PSRAM: about 40 KB
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

// --- playing a record -----------------------------------------------------------

static esp_err_t record_path(const p3a_record_t *record, char *out, size_t len, bool *smooth)
{
    const p3a_source_ops_t *ops = p3a_source_get(record->source);
    *smooth = ops != NULL && ops->smooth;
    if (record->flags & P3A_RECORD_IN_PLACE) {
        return (ops != NULL && ops->path != NULL) ? ops->path(record, out, len) : (esp_err_t)P3A_ERR_MISSING;
    }
    return p3a_blob_path(record->blob_ref, (p3a_format_t)record->format, out, len);
}

// Hands a record to the player. Called with the lock held.
static esp_err_t request(turn_kind_t kind, const p3a_record_t *record, uint32_t channel)
{
    animation_player_request_t req = {0};
    esp_err_t err = record_path(record, req.path, sizeof(req.path), &req.smooth);
    if (err != ESP_OK) {
        return err;
    }
    s->next_token = (s->next_token + 1) & ~OUR_TOKENS;
    if (s->next_token == 0) {
        s->next_token = 1;
    }
    req.token = OUR_TOKENS | s->next_token;
    // Set before the call: the player may report an older request as replaced
    // from inside it.
    s->pending_token = req.token;
    s->pending_kind = kind;
    s->pending_record = *record;
    s->pending_channel = channel;
    s->pending_generation = s->generation;
    err = animation_player_play(&req);
    if (err != ESP_OK) {
        s->pending_token = 0;
    }
    return err;
}

// A new pick that is still with the player is given up: its channel gets
// its turn back. Called with the lock held, before anything else is chosen.
static void abandon_pending_pick(void)
{
    if (s->pending_token != 0 && s->pending_kind == TURN_NEW && s->pending_generation == s->generation) {
        sched_core_undo_select(&s->core);
    }
    s->pending_token = 0;
}

static esp_err_t pick_new(void)
{
    p3a_settings_get(&s->settings);
    const sched_select_mode_t select =
        s->settings.channel_select == P3A_CHANNEL_SELECT_SWRR ? SCHED_SELECT_SWRR : SCHED_SELECT_STOCHASTIC;
    const sched_pick_mode_t mode = s->settings.pick_mode == P3A_PICK_MODE_RECENCY ? SCHED_PICK_RECENCY : SCHED_PICK_RANDOM;
    const uint32_t channel = sched_core_select(&s->core, select);
    if (channel == SCHED_NONE) {
        return ESP_ERR_NOT_FOUND;
    }
    const p3a_index_t *index = &s->channels[channel].index;
    const uint32_t ordinal = sched_core_pick(&s->core, channel, mode);
    const int32_t position = ordinal == SCHED_NONE ? -1 : p3a_index_nth_available(index, ordinal);
    if (position < 0) {
        // The core's count and the index disagree; put them back in step.
        sched_core_undo_select(&s->core);
        sched_core_set_available(&s->core, channel, index->available_count);
        return ESP_ERR_NOT_FOUND;
    }
    s->picks++;
    const esp_err_t err = request(TURN_NEW, p3a_index_at(index, (uint32_t)position), channel);
    if (err != ESP_OK) {
        sched_core_undo_select(&s->core);
    }
    return err;
}

esp_err_t play_scheduler_next(void)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    abandon_pending_pick();
    p3a_record_t record;
    const esp_err_t err = sched_history_forward(&s->history, &record) ? request(TURN_FORWARD, &record, 0) : pick_new();
    UNLOCK();
    return err;
}

esp_err_t play_scheduler_back(void)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    p3a_record_t record;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (sched_history_back(&s->history, &record)) {
        abandon_pending_pick();
        err = request(TURN_BACK, &record, 0);
    }
    UNLOCK();
    return err;
}

// --- the player's reports -------------------------------------------------------

static void on_ready(uint32_t token, void *ctx)
{
    (void)ctx;
    if (!(token & OUR_TOKENS) && s->foreign.ready) {
        s->foreign.ready(token, s->foreign_ctx);
    }
}

static void on_shown(uint32_t token, void *ctx)
{
    (void)ctx;
    if (!(token & OUR_TOKENS)) {
        // Something else put a picture on the panel (a test, for now).
        LOCK();
        s->playing = false;
        UNLOCK();
        if (s->foreign.shown) {
            s->foreign.shown(token, s->foreign_ctx);
        }
        return;
    }
    LOCK();
    if (token == s->pending_token) {
        s->pending_token = 0;
        s->current = s->pending_record;
        s->playing = true;
        s->shown++;
        if (s->pending_kind == TURN_NEW) {
            // Only now is it history: it reached the panel.
            sched_history_add(&s->history, &s->current);
        }
    }
    UNLOCK();
}

static void on_failed(uint32_t token, esp_err_t err, void *ctx)
{
    (void)ctx;
    if (!(token & OUR_TOKENS)) {
        if (s->foreign.failed) {
            s->foreign.failed(token, err, s->foreign_ctx);
        }
        return;
    }
    LOCK();
    if (token == s->pending_token) {  // a request already replaced by a newer one is nobody's concern
        s->pending_token = 0;
        s->failed++;
        s->last_error = err;
        const bool same_playset = s->pending_generation == s->generation;
        if (s->pending_kind == TURN_NEW && same_playset) {
            sched_core_undo_select(&s->core);  // the channel is not charged for a turn that showed nothing
            channel_t *ch = &s->channels[s->pending_channel];
            if (err == P3A_ERR_MISSING && ch->has_index) {
                // Its bytes are not there after all: stop offering it until
                // the channel is listed again.
                const int32_t at = p3a_index_find_record(&ch->index, &s->pending_record);
                if (at >= 0) {
                    p3a_index_set_available(&ch->index, (uint32_t)at, false);
                    sched_core_set_available(&s->core, s->pending_channel, ch->index.available_count);
                }
            }
        }
    }
    UNLOCK();
}

void play_scheduler_set_foreign_listener(const animation_player_listener_t *listener, void *ctx)
{
    if (s == NULL) {
        return;
    }
    LOCK();
    s->foreign = listener ? *listener : (animation_player_listener_t){0};
    s->foreign_ctx = ctx;
    UNLOCK();
}

// --- listing channels -----------------------------------------------------------

// The job: lists every channel that waits for it, one after another.
static esp_err_t list_job(void *arg)
{
    (void)arg;
    LOCK();
    s->list_job_queued = false;
    UNLOCK();
    for (;;) {
        // Find a waiting channel and copy what the listing needs.
        LOCK();
        uint32_t at = SCHED_NONE;
        for (uint32_t i = 0; i < s->playset.count; i++) {
            if (s->channels[i].state == PLAY_CHANNEL_WAITING) {
                at = i;
                break;
            }
        }
        if (at == SCHED_NONE) {
            UNLOCK();
            return ESP_OK;
        }
        const uint32_t generation = s->generation;
        const playset_channel_t spec = s->playset.channels[at];
        s->channels[at].state = PLAY_CHANNEL_LISTING;
        UNLOCK();

        const p3a_source_ops_t *ops = p3a_source_get(spec.source);  // checked when the playset was activated
        const p3a_channel_ref_t ref = {.kind = spec.kind, .identifier = spec.identifier, .offset = spec.offset};
        p3a_index_t fresh = {0};
        esp_err_t err = p3a_index_create(&fresh, ops->index_capacity ? ops->index_capacity() : DEFAULT_CAPACITY);
        if (err == ESP_OK) {
            err = ops->refresh(&ref, &fresh);
        }

        LOCK();
        channel_t *ch = &s->channels[at];
        if (generation != s->generation) {
            p3a_index_destroy(&fresh);  // the playset changed meanwhile
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "channel %s (%s) could not be listed: %s", ch->id, ops->name, p3a_err_to_name(err));
            p3a_index_destroy(&fresh);
            ch->error = err;
            // What it held before, if anything, stays playable.
            ch->state = ch->has_index ? PLAY_CHANNEL_READY : PLAY_CHANNEL_FAILED;
        } else {
            if (ch->has_index) {
                p3a_index_destroy(&ch->index);
            }
            ch->index = fresh;
            ch->has_index = true;
            ch->error = ESP_OK;
            ch->state = PLAY_CHANNEL_READY;
            sched_core_set_available(&s->core, at, ch->index.available_count);
            ESP_LOGI(TAG, "channel %s (%s): %u records, %u playable", ch->id, ops->name, (unsigned)ch->index.count,
                     (unsigned)ch->index.available_count);
            if ((s->start_wanted || !s->playing) && s->pending_token == 0 && pick_new() == ESP_OK) {
                s->start_wanted = false;  // the first thing to play has arrived
            }
        }
        UNLOCK();
    }
}

// Called with the lock held.
static esp_err_t queue_listing(void)
{
    if (s->list_job_queued) {
        return ESP_OK;  // the queued job will find every waiting channel
    }
    const esp_err_t err = p3a_job_submit(P3A_JOBS_IO, "list channels", list_job, NULL, NULL);
    s->list_job_queued = err == ESP_OK;
    return err;
}

// Puts a playset in place. Called with the lock held.
static void install(const playset_t *playset)
{
    abandon_pending_pick();
    for (uint32_t i = 0; i < PLAYSET_MAX_CHANNELS; i++) {
        if (s->channels[i].has_index) {
            p3a_index_destroy(&s->channels[i].index);
        }
    }
    memset(s->channels, 0, sizeof(s->channels));
    s->playset = *playset;
    s->generation++;
    s->start_wanted = true;
    sched_core_init(&s->core, playset->count, ((uint64_t)esp_random() << 32) | esp_random());
    uint32_t resident = 0;
    for (uint32_t i = 0; i < playset->count; i++) {
        channel_t *ch = &s->channels[i];
        playset_channel_id(&playset->channels[i], ch->id);
        sched_core_set_weight(&s->core, i, playset->channels[i].weight);
        if (p3a_source_get(playset->channels[i].source) == NULL) {
            ch->state = PLAY_CHANNEL_NO_SOURCE;
        } else if (resident >= RESIDENT_MAX) {
            ch->state = PLAY_CHANNEL_NOT_LOADED;
        } else {
            ch->state = PLAY_CHANNEL_WAITING;
            resident++;
        }
    }
}

esp_err_t play_scheduler_activate(const playset_t *playset)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (playset == NULL || playset_check(playset, NULL, 0) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    install(playset);
    esp_err_t err = queue_listing();
    UNLOCK();
    // Saved after it is in force: a card that cannot be written does not
    // stop the device from playing what it was asked to.
    const esp_err_t saved = playset_store_save_active(playset);
    if (saved != ESP_OK) {
        ESP_LOGW(TAG, "the active playset could not be saved: %s", p3a_err_to_name(saved));
    }
    return err != ESP_OK ? err : saved;
}

esp_err_t play_scheduler_refresh_source(uint8_t source)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    bool any = false;
    for (uint32_t i = 0; i < s->playset.count; i++) {
        channel_t *ch = &s->channels[i];
        if (s->playset.channels[i].source == source &&
            (ch->state == PLAY_CHANNEL_READY || ch->state == PLAY_CHANNEL_FAILED)) {
            ch->state = PLAY_CHANNEL_WAITING;
            any = true;
        }
    }
    const esp_err_t err = any ? queue_listing() : ESP_OK;
    UNLOCK();
    return err;
}

void play_scheduler_get_status(play_scheduler_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s == NULL) {
        return;
    }
    LOCK();
    memcpy(out->playset, s->playset.name, sizeof(out->playset));
    out->channel_count = s->playset.count;
    for (uint32_t i = 0; i < s->playset.count; i++) {
        const channel_t *ch = &s->channels[i];
        play_channel_status_t *c = &out->channels[i];
        memcpy(c->id, ch->id, sizeof(c->id));
        c->source = s->playset.channels[i].source;
        c->state = ch->state;
        c->error = ch->error;
        c->records = ch->has_index ? ch->index.count : 0;
        c->available = ch->has_index ? ch->index.available_count : 0;
    }
    out->playing = s->playing;
    out->current = s->current;
    out->loading = s->pending_token != 0;
    out->history_count = s->history.count;
    out->history_position = s->history.position;
    out->picks = s->picks;
    out->shown = s->shown;
    out->failed = s->failed;
    out->last_error = s->last_error;
    UNLOCK();
}

void play_scheduler_get_playset(playset_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s == NULL) {
        return;
    }
    LOCK();
    *out = s->playset;
    UNLOCK();
}

esp_err_t play_scheduler_start(void)
{
    if (s != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    state_t *state = psram_calloc(1, sizeof(state_t));
    playset_t *playset = psram_calloc(1, sizeof(playset_t));
    if (s_lock == NULL || state == NULL || playset == NULL) {
        free(state);
        free(playset);
        return ESP_ERR_NO_MEM;
    }
    s = state;
    static const animation_player_listener_t listener = {.ready = on_ready, .shown = on_shown, .failed = on_failed};
    animation_player_set_listener(&listener, NULL);

    const esp_err_t loaded = playset_store_load_active(playset);
    if (loaded != ESP_OK) {
        if (loaded != P3A_ERR_MISSING) {
            ESP_LOGW(TAG, "the active playset could not be read (%s); playing the user's own files",
                     p3a_err_to_name(loaded));
        }
        // A device that was never told what to play plays what is on its card.
        memset(playset, 0, sizeof(*playset));
        playset->count = 1;
        playset->channels[0].source = PLAYSET_SOURCE_LOCAL;
    }
    LOCK();
    install(playset);
    const esp_err_t err = queue_listing();
    UNLOCK();
    free(playset);
    return err;
}
