// SPDX-License-Identifier: Apache-2.0
// The scheduler. See play_scheduler.h.
//
// The decisions are pure code beside this file: which channel and which
// artwork (sched_core.c, sched_pick.c), what a failure leads to and when
// the device moves on by itself (sched_turn.c), "back" (sched_history.c).
// This file is the wiring: the lock, the player, the timer, the indices.
//
// One recursive lock guards everything here. It is held for arithmetic and
// for handing a request to the player, never across a listing (which reads
// the card or the network): a listing fills an index of its own, and the
// lock is taken only to swap it in.

#include "play_scheduler.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "animation_player.h"
#include "display_renderer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_blob.h"
#include "p3a_board.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_index.h"
#include "p3a_job.h"
#include "p3a_settings.h"
#include "p3a_source.h"
#include "playset_store.h"
#include "psram_alloc.h"
#include "sched_core.h"
#include "sched_history.h"
#include "sched_pick.h"
#include "sched_turn.h"

static const char *TAG = "sched";

// Indices held in memory at once. A playset may name more channels; the
// rest are not loaded (loading on demand arrives with the remote sources).
#define RESIDENT_MAX     16
#define DEFAULT_CAPACITY 1024
#define OUR_TOKENS       0x80000000u  // the scheduler's requests to the player carry this bit
#define TICK_MS          250

// What a request is for. NEW was picked from a channel; the others name one
// artwork. NEW and SHOW enter the history when shown.
typedef enum { TURN_NEW, TURN_BACK, TURN_FORWARD, TURN_SHOW } turn_kind_t;

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
    sched_recent_t recent;
    sched_turn_t turn;
    bool paused;
    bool advance_queued;     // the job that moves on is waiting to run
    esp_err_t problem;       // why the last turn a person asked for showed nothing; ESP_OK once something is shown
    uint32_t problems;

    // The request that is with the player, if any.
    uint32_t pending_token;
    turn_kind_t pending_kind;
    p3a_record_t pending_record;
    uint32_t pending_channel;
    uint32_t pending_generation;

    bool playing;
    p3a_record_t current;
    bool foreign_shown;      // a picture that is not the scheduler's is on the panel, or about to be
    bool staged;             // the pending request is loaded and waits for the display to draw it
    uint32_t foreign_token;  // the foreign request last reported ready with a picture
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
static esp_timer_handle_t s_tick;

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Tells whoever shows or reports the player's state that it changed.
static void changed(void)
{
    p3a_event_emit(P3A_EVENT_PLAYER_CHANGED);
}

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

// An artwork's name in the list of what was shown lately.
static uint32_t recent_id(const p3a_record_t *record)
{
    return record->id32 * 2654435761u + record->source;
}

static bool describe_candidate(uint32_t ordinal, sched_candidate_t *out, void *ctx)
{
    const p3a_index_t *index = ctx;
    const int32_t position = p3a_index_nth_available(index, ordinal);
    if (position < 0) {
        return false;
    }
    const p3a_record_t *record = p3a_index_at(index, (uint32_t)position);
    out->id = recent_id(record);
    out->width = record->width;
    out->height = record->height;
    return true;
}

// The limit on elongation for a channel, in tenths; 0 for none. Reads
// s->settings, which the caller has just filled.
static uint32_t aspect_limit(uint32_t channel)
{
    const p3a_source_ops_t *ops = p3a_source_get(s->playset.channels[channel].source);
    return (ops != NULL && ops->filter_aspect && s->settings.aspect_filter) ? (uint32_t)s->settings.aspect_max_tenths : 0;
}

// Picks a channel and an artwork of it and hands it to the player, as part
// of the turn under way. ESP_ERR_NOT_FOUND when nothing can be played.
// Called with the lock held.
static esp_err_t pick_new(void)
{
    p3a_settings_get(&s->settings);
    const sched_select_mode_t select =
        s->settings.channel_select == P3A_CHANNEL_SELECT_SWRR ? SCHED_SELECT_SWRR : SCHED_SELECT_STOCHASTIC;
    const sched_pick_mode_t mode = s->settings.pick_mode == P3A_PICK_MODE_RECENCY ? SCHED_PICK_RECENCY : SCHED_PICK_RANDOM;
    // A channel whose every candidate the filter refused has had its turn,
    // and the next channel is asked: so a channel of nothing but elongated
    // artworks cannot stop the others.
    for (uint32_t tries = 0; tries <= s->playset.count; tries++) {
        const uint32_t channel = sched_core_select(&s->core, select);
        if (channel == SCHED_NONE) {
            return ESP_ERR_NOT_FOUND;
        }
        p3a_index_t *index = &s->channels[channel].index;
        const uint32_t ordinal =
            sched_pick_artwork(&s->core, channel, mode, &s->recent, aspect_limit(channel), describe_candidate, index);
        if (ordinal == SCHED_NONE) {
            continue;
        }
        const int32_t position = p3a_index_nth_available(index, ordinal);
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
    return ESP_ERR_NOT_FOUND;
}

// Starts a turn that picks something new. Called with the lock held.
static esp_err_t begin_new(sched_turn_origin_t origin)
{
    abandon_pending_pick();
    sched_turn_begin(&s->turn, origin);
    const esp_err_t err = pick_new();
    if (err != ESP_OK) {
        sched_turn_gave_up(&s->turn, now_ms());
    }
    return err;
}

// Starts a turn for one named artwork. Called with the lock held.
static esp_err_t begin_this(turn_kind_t kind, const p3a_record_t *record)
{
    abandon_pending_pick();
    sched_turn_begin(&s->turn, SCHED_TURN_THIS);
    const esp_err_t err = request(kind, record, 0);
    if (err != ESP_OK) {
        sched_turn_gave_up(&s->turn, now_ms());
    }
    return err;
}

esp_err_t play_scheduler_next(void)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s->paused) {
        p3a_record_t record;
        err = sched_history_forward(&s->history, &record) ? begin_this(TURN_FORWARD, &record) : begin_new(SCHED_TURN_ANY);
    }
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
    esp_err_t err = s->paused ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    if (!s->paused && sched_history_back(&s->history, &record)) {
        err = begin_this(TURN_BACK, &record);
    }
    UNLOCK();
    return err;
}

esp_err_t play_scheduler_show(const p3a_record_t *record)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record == NULL || p3a_record_check(record) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    const esp_err_t err = s->paused ? ESP_ERR_INVALID_STATE : begin_this(TURN_SHOW, record);
    UNLOCK();
    return err;
}

// --- the player's reports -------------------------------------------------------

// The pending pick's record in its channel's index, or NULL: when the
// request named one artwork, or the playset has changed since.
static p3a_record_t *pending_in_index(int32_t *position)
{
    if (s->pending_kind != TURN_NEW || s->pending_generation != s->generation) {
        return NULL;
    }
    channel_t *ch = &s->channels[s->pending_channel];
    const int32_t at = ch->has_index ? p3a_index_find_record(&ch->index, &s->pending_record) : -1;
    if (position != NULL) {
        *position = at;
    }
    return at >= 0 ? p3a_index_at(&ch->index, (uint32_t)at) : NULL;
}

// "ready": the picture is decoded and its size known, and it is not on the
// panel yet. The size is remembered, and an artwork too elongated for its
// channel's filter is passed over here.
static bool on_ready(uint32_t token, const animation_player_loaded_t *loaded, void *ctx)
{
    (void)ctx;
    if (!(token & OUR_TOKENS)) {
        LOCK();
        s->foreign_token = loaded->width != 0 ? token : 0;  // 0: a request to play nothing
        // Said now, not when it is shown: whoever holds the panel (a notice
        // that covers it) must let go before the request can reach it. That
        // goes for a request to play nothing too; "shown" then says which.
        s->foreign_shown = true;
        UNLOCK();
        changed();
        return s->foreign.ready ? s->foreign.ready(token, loaded, s->foreign_ctx) : true;
    }
    bool wanted = true;
    LOCK();
    if (token == s->pending_token && loaded->width != 0) {
        const uint16_t width = loaded->width > UINT16_MAX ? UINT16_MAX : (uint16_t)loaded->width;
        const uint16_t height = loaded->height > UINT16_MAX ? UINT16_MAX : (uint16_t)loaded->height;
        s->pending_record.width = width;
        s->pending_record.height = height;
        p3a_record_t *record = pending_in_index(NULL);
        if (record != NULL) {
            record->width = width;  // in memory only: measured again after a restart
            record->height = height;
            p3a_settings_get(&s->settings);
            if (sched_aspect_too_long(width, height, aspect_limit(s->pending_channel))) {
                ESP_LOGI(TAG, "passed over, too elongated: %ux%u", (unsigned)width, (unsigned)height);
                wanted = false;
                s->pending_token = 0;
                // Not a failure: no retry is spent, nothing is set aside, and
                // the channel keeps its turn for another of its artworks.
                sched_core_undo_select(&s->core);
                if (!sched_turn_skip(&s->turn) || pick_new() != ESP_OK) {
                    sched_turn_gave_up(&s->turn, now_ms());
                }
            }
        }
    }
    if (wanted && token == s->pending_token) {
        s->staged = true;
    }
    UNLOCK();
    if (wanted) {
        changed();  // see the note above: the panel must be free for it to be shown
    }
    return wanted;
}

static void on_shown(uint32_t token, void *ctx)
{
    (void)ctx;
    if (!(token & OUR_TOKENS)) {
        // Something else put a picture on the panel (a test). The scheduler
        // leaves it there until it is asked for something.
        LOCK();
        s->playing = false;
        s->foreign_shown = token == s->foreign_token;
        sched_turn_stopped(&s->turn);
        UNLOCK();
        changed();
        if (s->foreign.shown) {
            s->foreign.shown(token, s->foreign_ctx);
        }
        return;
    }
    LOCK();
    if (token == s->pending_token) {
        s->pending_token = 0;
        s->staged = false;
        s->current = s->pending_record;
        s->playing = true;
        s->foreign_shown = false;
        s->shown++;
        s->problem = ESP_OK;
        if (s->pending_kind == TURN_NEW || s->pending_kind == TURN_SHOW) {
            // Only now is it history: it reached the panel.
            sched_history_add(&s->history, &s->current);
        }
        sched_recent_add(&s->recent, recent_id(&s->current));
        sched_turn_shown(&s->turn, now_ms());
    }
    UNLOCK();
    changed();
}

static sched_fail_t classify(esp_err_t err)
{
    switch (err) {
    case P3A_ERR_MISSING:
        return SCHED_FAIL_MISSING;
    case P3A_ERR_CORRUPT:
        return SCHED_FAIL_CORRUPT;
    case P3A_ERR_NOT_PLAYABLE:
        return SCHED_FAIL_NOT_PLAYABLE;
    default:
        return SCHED_FAIL_OTHER;
    }
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
    if (token != s->pending_token) {  // a request already replaced by a newer one is nobody's concern
        UNLOCK();
        return;
    }
    s->pending_token = 0;
    s->staged = false;
    int32_t at = -1;
    p3a_record_t *record = pending_in_index(&at);
    if (s->pending_kind == TURN_NEW && s->pending_generation == s->generation) {
        sched_core_undo_select(&s->core);  // the channel is not charged for a turn that showed nothing
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        // Replaced by something that is not the scheduler's (a test playing
        // a file directly). Nothing is wrong with the artwork.
        sched_turn_gave_up(&s->turn, now_ms());
        UNLOCK();
        return;
    }
    s->failed++;
    s->last_error = err;
    const sched_turn_verdict_t verdict = sched_turn_failed(&s->turn, classify(err), now_ms());
    if (verdict.delete_blob && !(s->pending_record.flags & P3A_RECORD_IN_PLACE) &&
        p3a_record_has_blob(&s->pending_record)) {
        // Damaged bytes in the store are of no use and would fail again.
        // A user's own file is never deleted, whatever is wrong with it.
        const esp_err_t removed = p3a_blob_remove(s->pending_record.blob_ref, (p3a_format_t)s->pending_record.format);
        ESP_LOGW(TAG, "a damaged blob was deleted: %s", p3a_err_to_name(removed));
        if (record != NULL) {
            memset(record->blob_ref, 0, sizeof(record->blob_ref));  // to be fetched again
        }
    }
    if (verdict.set_aside && record != NULL) {
        // Not offered again until its channel is listed anew.
        channel_t *ch = &s->channels[s->pending_channel];
        p3a_index_set_available(&ch->index, (uint32_t)at, false);
        sched_core_set_available(&s->core, s->pending_channel, ch->index.available_count);
    }
    bool report = verdict.report;
    if (verdict.retry && pick_new() != ESP_OK) {
        // Nothing else to try.
        report = s->turn.origin != SCHED_TURN_AUTO;
        sched_turn_gave_up(&s->turn, now_ms());
    }
    if (report) {
        s->problem = err;
        s->problems++;
        ESP_LOGW(TAG, "nothing was shown: %s", p3a_err_to_name(err));
    }
    UNLOCK();
    changed();
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
        changed();

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
            if ((s->start_wanted || (!s->playing && !s->foreign_shown)) && s->pending_token == 0 &&
                begin_new(SCHED_TURN_AUTO) == ESP_OK) {
                s->start_wanted = false;  // the first thing to play has arrived
            }
        }
        UNLOCK();
        changed();
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
    changed();
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
    out->paused = s->paused;
    out->foreign = s->foreign_shown;
    out->staged = s->staged && s->pending_token != 0;
    out->advance_in_s = sched_turn_remaining_s(&s->turn, now_ms());
    out->problem = s->problem;
    out->problems = s->problems;
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

// --- moving on, pause, settings ---------------------------------------------------

static uint32_t playable_total(void)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < s->core.count; i++) {
        total += s->core.channels[i].available;
    }
    return total;
}

static esp_err_t advance_job(void *arg)
{
    (void)arg;
    LOCK();
    s->advance_queued = false;
    // Asked again: a person may have acted since the timer saw it was time.
    if (sched_turn_due(&s->turn, now_ms(), playable_total())) {
        begin_new(SCHED_TURN_AUTO);
    }
    UNLOCK();
    return ESP_OK;
}

// On the timer task, four times a second: only looks, and leaves the work
// to a job.
static void tick(void *arg)
{
    (void)arg;
    if (xSemaphoreTakeRecursive(s_lock, 0) != pdTRUE) {
        return;  // busy; the next tick is soon enough
    }
    if (!s->advance_queued && sched_turn_due(&s->turn, now_ms(), playable_total())) {
        s->advance_queued = p3a_job_submit(P3A_JOBS_UI, "next artwork", advance_job, NULL, NULL) == ESP_OK;
    }
    UNLOCK();
}

esp_err_t play_scheduler_set_paused(bool paused)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    if (paused != s->paused) {
        s->paused = paused;
        sched_turn_set_paused(&s->turn, paused, now_ms());
        if (paused) {
            display_renderer_set_blank(true);
            p3a_board_set_brightness(0);  // the backlight too: a black picture still glows
        } else {
            p3a_settings_get(&s->settings);
            display_renderer_set_blank(false);
            p3a_board_set_brightness((int)s->settings.brightness);
        }
        ESP_LOGI(TAG, "%s", paused ? "paused" : "resumed");
    }
    UNLOCK();
    changed();
    return ESP_OK;
}

bool play_scheduler_is_paused(void)
{
    if (s == NULL) {
        return false;
    }
    LOCK();
    const bool paused = s->paused;
    UNLOCK();
    return paused;
}

// Holds taken before the scheduler started (the UI's boot animation is up
// first). They are applied when it starts, so that a hold and its release
// always pair up whichever side of the start they fall on.
static _Atomic int s_early_holds;

void play_scheduler_hold(void)
{
    if (s == NULL) {
        atomic_fetch_add(&s_early_holds, 1);
        return;
    }
    LOCK();
    sched_turn_hold(&s->turn);
    UNLOCK();
}

void play_scheduler_release(void)
{
    if (s == NULL) {
        if (atomic_load(&s_early_holds) > 0) {
            atomic_fetch_sub(&s_early_holds, 1);
        }
        return;
    }
    LOCK();
    sched_turn_release(&s->turn, now_ms());
    UNLOCK();
}

static void on_setting_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->payload.i32 != P3A_SET_DWELL_S && event->payload.i32 != P3A_SET_BRIGHTNESS) {
        return;
    }
    LOCK();
    p3a_settings_get(&s->settings);
    sched_turn_set_dwell(&s->turn, (uint32_t)s->settings.dwell_s);
    if (event->payload.i32 == P3A_SET_BRIGHTNESS && !s->paused) {
        p3a_board_set_brightness((int)s->settings.brightness);  // while paused it waits for the resume
    }
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
    p3a_settings_get(&s->settings);
    sched_turn_init(&s->turn, (uint32_t)s->settings.dwell_s);
    for (int early = atomic_exchange(&s_early_holds, 0); early > 0; early--) {
        sched_turn_hold(&s->turn);
    }
    p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_setting_changed, NULL);
    const esp_timer_create_args_t timer = {.callback = tick, .name = "sched_tick"};
    if (esp_timer_create(&timer, &s_tick) == ESP_OK) {
        esp_timer_start_periodic(s_tick, TICK_MS * 1000);
    }
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
