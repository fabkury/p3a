// SPDX-License-Identifier: Apache-2.0
// The scheduler. See play_scheduler.h.
//
// The decisions are pure code beside this file: which channel and which
// artwork (sched_core.c, sched_pick.c), what a failure leads to and when
// the device moves on by itself (sched_turn.c), "back" (sched_history.c).
// This file is the wiring: the lock, the player, the timer, the indices.
//
// The channels are filled by the content task (sched_content.c); the state
// and its lock are in sched_state.h.

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
#include "sched_state.h"

static const char *TAG = "sched";

#define OUR_TOKENS 0x80000000u  // the scheduler's requests to the player carry this bit
#define TICK_MS    250
#define PENDING_OVERDUE_MS 30000

sched_state_t *sched;
SemaphoreHandle_t sched_lock;
static esp_timer_handle_t s_tick;

int64_t sched_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Tells whoever shows or reports the player's state that it changed.
void sched_changed(void)
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
    if (sched->shut_down) {
        return ESP_ERR_NOT_ALLOWED;
    }
    animation_player_request_t req = {0};
    esp_err_t err = record_path(record, req.path, sizeof(req.path), &req.smooth);
    if (err != ESP_OK) {
        return err;
    }
    sched->next_token = (sched->next_token + 1) & ~OUR_TOKENS;
    if (sched->next_token == 0) {
        sched->next_token = 1;
    }
    req.token = OUR_TOKENS | sched->next_token;
    // Set before the call: the player may report an older request as replaced
    // from inside it.
    sched->pending_token = req.token;
    sched->pending_kind = kind;
    sched->pending_record = *record;
    sched->pending_channel = channel;
    sched->pending_generation = sched->generation;
    sched->pending_since_ms = sched_now_ms();
    sched->pending_overdue_said = false;
    err = animation_player_play(&req);
    if (err != ESP_OK) {
        sched->pending_token = 0;
    }
    return err;
}

// A new pick that is still with the player is given up: its channel gets
// its turn back. Called with the lock held, before anything else is chosen.
static void abandon_pending_pick(void)
{
    if (sched->pending_token != 0 && sched->pending_kind == TURN_NEW && sched->pending_generation == sched->generation) {
        sched_core_undo_select(&sched->core);
    }
    sched->pending_token = 0;
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
// sched->settings, which the caller has just filled.
static uint32_t aspect_limit(uint32_t channel)
{
    const p3a_source_ops_t *ops = p3a_source_get(sched->playset.channels[channel].source);
    return (ops != NULL && ops->filter_aspect && sched->settings.aspect_filter) ? (uint32_t)sched->settings.aspect_max_tenths : 0;
}

// Picks a channel and an artwork of it and hands it to the player, as part
// of the turn under way. ESP_ERR_NOT_FOUND when nothing can be played.
// Called with the lock held.
static esp_err_t pick_new(void)
{
    p3a_settings_get(&sched->settings);
    const sched_select_mode_t select =
        sched->settings.channel_select == P3A_CHANNEL_SELECT_SWRR ? SCHED_SELECT_SWRR : SCHED_SELECT_STOCHASTIC;
    const sched_pick_mode_t mode = sched->settings.pick_mode == P3A_PICK_MODE_RECENCY ? SCHED_PICK_RECENCY : SCHED_PICK_RANDOM;
    // A channel whose every candidate the filter refused has had its turn,
    // and the next channel is asked: so a channel of nothing but elongated
    // artworks cannot stop the others.
    for (uint32_t tries = 0; tries <= sched->playset.count; tries++) {
        const uint32_t channel = sched_core_select(&sched->core, select);
        if (channel == SCHED_NONE) {
            return ESP_ERR_NOT_FOUND;
        }
        p3a_index_t *index = &sched->channels[channel].index;
        const uint32_t ordinal =
            sched_pick_artwork(&sched->core, channel, mode, &sched->recent, aspect_limit(channel), describe_candidate, index);
        if (ordinal == SCHED_NONE) {
            continue;
        }
        const int32_t position = p3a_index_nth_available(index, ordinal);
        if (position < 0) {
            // The core's count and the index disagree; put them back in step.
            sched_core_undo_select(&sched->core);
            sched_core_set_available(&sched->core, channel, index->available_count);
            return ESP_ERR_NOT_FOUND;
        }
        sched->picks++;
        const esp_err_t err = request(TURN_NEW, p3a_index_at(index, (uint32_t)position), channel);
        if (err != ESP_OK) {
            sched_core_undo_select(&sched->core);
        }
        return err;
    }
    return ESP_ERR_NOT_FOUND;
}

// Starts a turn that picks something new. Called with the lock held.
static esp_err_t begin_new(sched_turn_origin_t origin)
{
    abandon_pending_pick();
    sched_turn_begin(&sched->turn, origin);
    const esp_err_t err = pick_new();
    if (err != ESP_OK) {
        sched_turn_gave_up(&sched->turn, sched_now_ms());
    }
    return err;
}

// Starts a turn for one named artwork. Called with the lock held.
static esp_err_t begin_this(turn_kind_t kind, const p3a_record_t *record)
{
    abandon_pending_pick();
    sched_turn_begin(&sched->turn, SCHED_TURN_THIS);
    const esp_err_t err = request(kind, record, 0);
    if (err != ESP_OK) {
        sched_turn_gave_up(&sched->turn, sched_now_ms());
    }
    return err;
}

esp_err_t play_scheduler_next(void)
{
    if (sched == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!sched->paused) {
        p3a_record_t record;
        err = sched_history_forward(&sched->history, &record) ? begin_this(TURN_FORWARD, &record) : begin_new(SCHED_TURN_ANY);
    }
    UNLOCK();
    sched_changed();
    return err;
}

esp_err_t play_scheduler_back(void)
{
    if (sched == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    p3a_record_t record;
    esp_err_t err = sched->paused ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    if (!sched->paused && sched_history_back(&sched->history, &record)) {
        err = begin_this(TURN_BACK, &record);
    }
    UNLOCK();
    sched_changed();
    return err;
}

esp_err_t play_scheduler_show(const p3a_record_t *record)
{
    if (sched == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record == NULL || p3a_record_check(record) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    const esp_err_t err = sched->paused ? ESP_ERR_INVALID_STATE : begin_this(TURN_SHOW, record);
    UNLOCK();
    sched_changed();
    return err;
}

// --- the player's reports -------------------------------------------------------

// The pending pick's record in its channel's index, or NULL: when the
// request named one artwork, or the playset has changed since.
static p3a_record_t *pending_in_index(int32_t *position)
{
    if (sched->pending_kind != TURN_NEW || sched->pending_generation != sched->generation) {
        return NULL;
    }
    sched_slot_t *ch = &sched->channels[sched->pending_channel];
    const int32_t at = ch->has_index ? p3a_index_find_record(&ch->index, &sched->pending_record) : -1;
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
        sched->foreign_token = loaded->width != 0 ? token : 0;  // 0: a request to play nothing
        // Said now, not when it is shown: whoever holds the panel (a notice
        // that covers it) must let go before the request can reach it. That
        // goes for a request to play nothing too; "shown" then says which.
        sched->foreign_shown = true;
        UNLOCK();
        sched_changed();
        return sched->foreign.ready ? sched->foreign.ready(token, loaded, sched->foreign_ctx) : true;
    }
    bool wanted = true;
    LOCK();
    if (token == sched->pending_token && loaded->width != 0) {
        const uint16_t width = loaded->width > UINT16_MAX ? UINT16_MAX : (uint16_t)loaded->width;
        const uint16_t height = loaded->height > UINT16_MAX ? UINT16_MAX : (uint16_t)loaded->height;
        sched->pending_record.width = width;
        sched->pending_record.height = height;
        p3a_record_t *record = pending_in_index(NULL);
        if (record != NULL) {
            record->width = width;  // in memory only: measured again after a restart
            record->height = height;
            p3a_settings_get(&sched->settings);
            if (sched_aspect_too_long(width, height, aspect_limit(sched->pending_channel))) {
                ESP_LOGI(TAG, "passed over, too elongated: %ux%u", (unsigned)width, (unsigned)height);
                wanted = false;
                sched->pending_token = 0;
                // Not a failure: no retry is spent, nothing is set aside, and
                // the channel keeps its turn for another of its artworks.
                sched_core_undo_select(&sched->core);
                if (!sched_turn_skip(&sched->turn) || pick_new() != ESP_OK) {
                    sched_turn_gave_up(&sched->turn, sched_now_ms());
                }
            }
        }
    }
    if (wanted && token == sched->pending_token) {
        sched->staged = true;
    }
    UNLOCK();
    if (wanted) {
        sched_changed();  // see the note above: the panel must be free for it to be shown
    }
    return wanted;
}

// --- telling a source that its artwork is seen -------------------------------------
//
// "Seen" is: the scheduler's own artwork is on the panel, the player is not
// paused, and no screen covers it. What changed is worked out under the
// lock and told after it is released: a source may take its own lock.

typedef struct {
    bool hide, show;
    uint8_t hide_source;
    p3a_record_t record;
    p3a_shown_t how;
    char kind[PLAYSET_KIND_LEN];
    char identifier[PLAYSET_IDENT_LEN];
} tell_t;

// Called with the lock held.
static void tell_prepare(tell_t *t)
{
    t->hide = t->show = false;
    const bool seen = sched->playing && !sched->paused && !sched->shut_down && sched->turn.holds == 0;
    const bool same = sched->told_seen && seen && memcmp(&sched->told_record, &sched->current, sizeof(p3a_record_t)) == 0;
    if (same || (!seen && !sched->told_seen)) {
        return;
    }
    if (sched->told_seen) {
        t->hide = true;
        t->hide_source = sched->told_record.source;
    }
    if (seen) {
        t->show = true;
        t->record = sched->current;
        memcpy(t->kind, sched->current_kind, sizeof(t->kind));
        memcpy(t->identifier, sched->current_identifier, sizeof(t->identifier));
        t->how = (p3a_shown_t){.channel_source = sched->current_channel_source,
                               .asked_for = sched->current_asked,
                               .random = sched->settings.pick_mode != P3A_PICK_MODE_RECENCY};
        sched->told_record = sched->current;
    }
    sched->told_seen = seen;
}

// Called without the lock.
static void tell_deliver(tell_t *t)
{
    if (t->hide && !(t->show && t->hide_source == t->record.source)) {
        const p3a_source_ops_t *ops = p3a_source_get(t->hide_source);
        if (ops != NULL && ops->shown != NULL) {
            ops->shown(NULL, NULL);
        }
    }
    if (t->show) {
        const p3a_source_ops_t *ops = p3a_source_get(t->record.source);
        if (ops != NULL && ops->shown != NULL) {
            t->how.kind = t->kind;
            t->how.identifier = t->identifier;
            ops->shown(&t->record, &t->how);
        }
    }
}

static void on_shown(uint32_t token, void *ctx)
{
    (void)ctx;
    if (!(token & OUR_TOKENS)) {
        // Something else put a picture on the panel (a test). The scheduler
        // leaves it there until it is asked for something.
        tell_t tell;
        LOCK();
        sched->playing = false;
        sched->foreign_shown = token == sched->foreign_token;
        sched_turn_stopped(&sched->turn);
        tell_prepare(&tell);
        UNLOCK();
        tell_deliver(&tell);
        sched_changed();
        if (sched->foreign.shown) {
            sched->foreign.shown(token, sched->foreign_ctx);
        }
        return;
    }
    tell_t tell;
    LOCK();
    if (token == sched->pending_token) {
        sched->pending_token = 0;
        sched->staged = false;
        sched->current = sched->pending_record;
        // The channel it was picked from, if it was and the playset is still that one.
        sched->current_channel_source = 0;
        sched->current_kind[0] = sched->current_identifier[0] = '\0';
        if (sched->pending_kind == TURN_NEW && sched->pending_generation == sched->generation &&
            sched->pending_channel < sched->playset.count) {
            const playset_channel_t *from = &sched->playset.channels[sched->pending_channel];
            sched->current_channel_source = from->source;
            memcpy(sched->current_kind, from->kind, sizeof(sched->current_kind));
            memcpy(sched->current_identifier, from->identifier, sizeof(sched->current_identifier));
        }
        sched->current_asked = sched->pending_kind == TURN_SHOW;
        sched->playing = true;
        sched->start_wanted = false;
        sched->foreign_shown = false;
        sched->shown++;
        sched->problem = ESP_OK;
        if (sched->pending_kind == TURN_NEW || sched->pending_kind == TURN_SHOW) {
            // Only now is it history: it reached the panel.
            sched_history_add(&sched->history, &sched->current);
        }
        sched_recent_add(&sched->recent, recent_id(&sched->current));
        sched_turn_shown(&sched->turn, sched_now_ms());
    }
    tell_prepare(&tell);
    UNLOCK();
    tell_deliver(&tell);
    sched_changed();
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
        if (sched->foreign.failed) {
            sched->foreign.failed(token, err, sched->foreign_ctx);
        }
        return;
    }
    LOCK();
    if (token != sched->pending_token) {  // a request already replaced by a newer one is nobody's concern
        UNLOCK();
        return;
    }
    sched->pending_token = 0;
    sched->staged = false;
    int32_t at = -1;
    p3a_record_t *record = pending_in_index(&at);
    if (sched->pending_kind == TURN_NEW && sched->pending_generation == sched->generation) {
        sched_core_undo_select(&sched->core);  // the channel is not charged for a turn that showed nothing
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        // Replaced by something that is not the scheduler's (a test playing
        // a file directly). Nothing is wrong with the artwork.
        sched_turn_gave_up(&sched->turn, sched_now_ms());
        UNLOCK();
        return;
    }
    sched->failed++;
    sched->last_error = err;
    const sched_turn_verdict_t verdict = sched_turn_failed(&sched->turn, classify(err), sched_now_ms());
    if (verdict.delete_blob && !(sched->pending_record.flags & P3A_RECORD_IN_PLACE) &&
        p3a_record_has_blob(&sched->pending_record)) {
        // Damaged bytes in the store are of no use and would fail again.
        // A user's own file is never deleted, whatever is wrong with it.
        const esp_err_t removed = p3a_blob_remove(sched->pending_record.blob_ref, (p3a_format_t)sched->pending_record.format);
        ESP_LOGW(TAG, "a damaged blob was deleted: %s", p3a_err_to_name(removed));
        if (record != NULL) {
            memset(record->blob_ref, 0, sizeof(record->blob_ref));  // to be fetched again
        }
    }
    if (err == P3A_ERR_MISSING && record != NULL && p3a_record_has_blob(record) &&
        !(record->flags & P3A_RECORD_IN_PLACE)) {
        // Its blob is no longer in the store (it was the oldest when the
        // card filled up). Without a reference it is wanted again, and the
        // content task fetches it when there is room.
        memset(record->blob_ref, 0, sizeof(record->blob_ref));
        sched_content_wake();
    }
    if (verdict.set_aside && record != NULL) {
        // Not offered again until its channel is listed anew.
        sched_slot_t *ch = &sched->channels[sched->pending_channel];
        p3a_index_set_available(&ch->index, (uint32_t)at, false);
        sched_core_set_available(&sched->core, sched->pending_channel, ch->index.available_count);
    }
    bool report = verdict.report;
    if (verdict.retry && pick_new() != ESP_OK) {
        // Nothing else to try.
        report = sched->turn.origin != SCHED_TURN_AUTO;
        sched_turn_gave_up(&sched->turn, sched_now_ms());
    }
    if (report) {
        sched->problem = err;
        sched->problems++;
        ESP_LOGW(TAG, "nothing was shown: %s", p3a_err_to_name(err));
    }
    UNLOCK();
    sched_changed();
}

void play_scheduler_set_foreign_listener(const animation_player_listener_t *listener, void *ctx)
{
    if (sched == NULL) {
        return;
    }
    LOCK();
    sched->foreign = listener ? *listener : (animation_player_listener_t){0};
    sched->foreign_ctx = ctx;
    UNLOCK();
}

// --- playsets ------------------------------------------------------------------

void sched_start_if_wanted(void)
{
    // `start_wanted` stays until an artwork of the new playset is on the
    // panel (on_shown): a first pick that fails (its blob went from the
    // store) must not leave the old playset's artwork up for a whole dwell.
    if ((sched->start_wanted || (!sched->playing && !sched->foreign_shown)) && sched->pending_token == 0) {
        begin_new(SCHED_TURN_AUTO);
    }
}

// Puts a playset in place. Called with the lock held.
static void install(const playset_t *playset)
{
    abandon_pending_pick();
    for (uint32_t i = 0; i < PLAYSET_MAX_CHANNELS; i++) {
        if (sched->channels[i].has_index) {
            p3a_index_destroy(&sched->channels[i].index);
        }
    }
    memset(sched->channels, 0, sizeof(sched->channels));
    sched->playset = *playset;
    sched->generation++;
    sched->start_wanted = true;
    sched_core_init(&sched->core, playset->count, ((uint64_t)esp_random() << 32) | esp_random());
    uint64_t held = 0;  // bytes of index memory spoken for
    for (uint32_t i = 0; i < playset->count; i++) {
        sched_slot_t *ch = &sched->channels[i];
        playset_channel_id(&playset->channels[i], ch->id);
        sched_core_set_weight(&sched->core, i, playset->channels[i].weight);
        const p3a_source_ops_t *ops = p3a_source_get(playset->channels[i].source);
        if (ops == NULL) {
            ch->state = PLAY_CHANNEL_NO_SOURCE;
        } else if (held + SCHED_INDEX_BYTES(sched_content_capacity(ops)) > SCHED_INDEX_BUDGET) {
            ch->state = PLAY_CHANNEL_NOT_LOADED;
            ESP_LOGW(TAG, "channel %u of the playset is not loaded: the indices before it take %u MB", (unsigned)i,
                     (unsigned)(held >> 20));
        } else {
            ch->state = PLAY_CHANNEL_WAITING;
            sched_refresh_init(&ch->refresh, ops->refresh_interval_s ? ops->refresh_interval_s() : 0, ops->remote);
            held += SCHED_INDEX_BYTES(sched_content_capacity(ops));
        }
    }
    sched_content_wake();
}

esp_err_t play_scheduler_activate(const playset_t *playset)
{
    if (sched == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (playset == NULL || playset_check(playset, NULL, 0) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    install(playset);
    const esp_err_t err = ESP_OK;
    UNLOCK();
    sched_changed();
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
    if (sched == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    bool any = false;
    for (uint32_t i = 0; i < sched->playset.count; i++) {
        sched_slot_t *ch = &sched->channels[i];
        if (sched->playset.channels[i].source == source &&
            (ch->state == PLAY_CHANNEL_READY || ch->state == PLAY_CHANNEL_FAILED)) {
            ch->state = PLAY_CHANNEL_WAITING;
            sched_refresh_ask(&ch->refresh, sched_now_ms());
            any = true;
        }
    }
    UNLOCK();
    if (any) {
        sched_content_wake();
    }
    return ESP_OK;
}

esp_err_t play_scheduler_refresh_all(void)
{
    if (sched == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    for (uint32_t i = 0; i < sched->playset.count; i++) {
        sched_slot_t *ch = &sched->channels[i];
        if (ch->state == PLAY_CHANNEL_READY || ch->state == PLAY_CHANNEL_FAILED || ch->state == PLAY_CHANNEL_WAITING) {
            sched_refresh_ask(&ch->refresh, sched_now_ms());
        }
    }
    UNLOCK();
    sched_content_wake();
    return ESP_OK;
}

void play_scheduler_get_status(play_scheduler_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (sched == NULL) {
        return;
    }
    LOCK();
    memcpy(out->playset, sched->playset.name, sizeof(out->playset));
    out->channel_count = sched->playset.count;
    for (uint32_t i = 0; i < sched->playset.count; i++) {
        const sched_slot_t *ch = &sched->channels[i];
        play_channel_status_t *c = &out->channels[i];
        memcpy(c->id, ch->id, sizeof(c->id));
        c->source = sched->playset.channels[i].source;
        c->state = ch->state;
        c->error = ch->error;
        c->records = ch->has_index ? ch->index.count : 0;
        c->available = ch->has_index ? ch->index.available_count : 0;
        const p3a_source_ops_t *ops = p3a_source_get(c->source);
        c->remote = ops != NULL && ops->remote;
        c->wanted = c->remote && ch->has_index ? sched_content_wanted(&ch->index) : 0;
        c->listings = ch->listings;
        c->refresh_in_s = (int32_t)sched_refresh_wait_s(&ch->refresh, sched_now_ms());
    }
    out->downloading = sched->downloading;
    out->online = sched_content_online();
    out->downloads = sched->downloads;
    out->download_failures = sched->download_failures;
    out->playing = sched->playing;
    out->current = sched->current;
    out->loading = sched->pending_token != 0;
    out->history_count = sched->history.count;
    out->history_position = sched->history.position;
    out->picks = sched->picks;
    out->shown = sched->shown;
    out->failed = sched->failed;
    out->last_error = sched->last_error;
    out->paused = sched->paused;
    out->foreign = sched->foreign_shown;
    out->asked = sched->turn.busy && sched->turn.origin != SCHED_TURN_AUTO;
    out->staged = sched->staged && sched->pending_token != 0;
    out->advance_in_s = sched_turn_remaining_s(&sched->turn, sched_now_ms());
    out->problem = sched->problem;
    out->problems = sched->problems;
    UNLOCK();
}

bool play_scheduler_current(p3a_record_t *out)
{
    if (sched == NULL) {
        return false;
    }
    LOCK();
    const bool playing = sched->playing && !sched->foreign_shown;
    if (playing) {
        *out = sched->current;
    }
    UNLOCK();
    return playing;
}

void play_scheduler_get_playset(playset_t *out)
{
    memset(out, 0, sizeof(*out));
    if (sched == NULL) {
        return;
    }
    LOCK();
    *out = sched->playset;
    UNLOCK();
}

// --- moving on, pause, settings ---------------------------------------------------

static uint32_t playable_total(void)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < sched->core.count; i++) {
        total += sched->core.channels[i].available;
    }
    return total;
}

static esp_err_t advance_job(void *arg)
{
    (void)arg;
    LOCK();
    sched->advance_queued = false;
    // Asked again: a person may have acted since the timer saw it was time.
    if (sched_turn_due(&sched->turn, sched_now_ms(), playable_total())) {
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
    if (xSemaphoreTakeRecursive(sched_lock, 0) != pdTRUE) {
        return;  // busy; the next tick is soon enough
    }
    if (!sched->advance_queued && sched_turn_due(&sched->turn, sched_now_ms(), playable_total())) {
        sched->advance_queued = p3a_job_submit(P3A_JOBS_UI, "next artwork", advance_job, NULL, NULL) == ESP_OK;
    }
    // The player answers every request (ready and shown, or failed) within
    // seconds. One still open after half a minute is a fault somewhere
    // between the two, or something holding the panel so that a loaded
    // artwork is never drawn (a test pattern left up did that once). Said
    // once per request, with what each side believes.
    if (sched->pending_token != 0 && !sched->pending_overdue_said &&
        sched_now_ms() - sched->pending_since_ms > PENDING_OVERDUE_MS) {
        sched->pending_overdue_said = true;
        animation_player_status_t player;
        animation_player_get_status(&player);
        ESP_LOGE(TAG, "request %08x has had no answer for %d s: staged %d, foreign %d, playing %d; the player shows %08x, loading %d",
                 (unsigned)sched->pending_token, PENDING_OVERDUE_MS / 1000, sched->staged, sched->foreign_shown,
                 sched->playing, (unsigned)player.token, player.loading);
    }
    UNLOCK();
}

bool play_scheduler_known(const p3a_record_t *record, p3a_record_t *out)
{
    if (sched == NULL || record == NULL || out == NULL) {
        return false;
    }
    bool found = false;
    LOCK();
    for (uint32_t i = 0; i < sched->playset.count && !found; i++) {
        const sched_slot_t *ch = &sched->channels[i];
        const int32_t at = ch->has_index ? p3a_index_find_record(&ch->index, record) : -1;
        if (at >= 0 && p3a_record_has_blob(&ch->index.records[at])) {
            *out = ch->index.records[at];
            found = true;
        }
    }
    UNLOCK();
    return found;
}

esp_err_t play_scheduler_set_paused(bool paused)
{
    if (sched == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    if (paused != sched->paused) {
        sched->paused = paused;
        sched_turn_set_paused(&sched->turn, paused, sched_now_ms());
        if (paused) {
            display_renderer_set_blank(true);
            p3a_board_set_brightness(0);  // the backlight too: a black picture still glows
        } else {
            p3a_settings_get(&sched->settings);
            display_renderer_set_blank(false);
            p3a_board_set_brightness((int)sched->settings.brightness);
        }
        ESP_LOGI(TAG, "%s", paused ? "paused" : "resumed");
    }
    tell_t tell;
    tell_prepare(&tell);
    UNLOCK();
    tell_deliver(&tell);
    sched_changed();
    return ESP_OK;
}

void play_scheduler_shutdown(void)
{
    if (sched == NULL) {
        return;
    }
    tell_t tell;
    LOCK();
    sched->shut_down = true;
    abandon_pending_pick();
    sched->playing = false;
    sched_turn_stopped(&sched->turn);
    // Not one of the scheduler's own tokens: its report is passed on and
    // otherwise ignored.
    animation_player_stop(1);
    tell_prepare(&tell);
    UNLOCK();
    tell_deliver(&tell);
    // A download in progress gives up at its next chunk; the card is not
    // handed over under it.
    if (!sched_content_wait_idle(30000)) {
        ESP_LOGW(TAG, "the content task is still at work");
    }
    ESP_LOGW(TAG, "playback is stopped until the restart");
    sched_changed();
}

bool play_scheduler_is_paused(void)
{
    if (sched == NULL) {
        return false;
    }
    LOCK();
    const bool paused = sched->paused;
    UNLOCK();
    return paused;
}

// Holds taken before the scheduler started (the UI's boot animation is up
// first). They are applied when it starts, so that a hold and its release
// always pair up whichever side of the start they fall on.
static _Atomic int s_early_holds;

void play_scheduler_hold(void)
{
    if (sched == NULL) {
        atomic_fetch_add(&s_early_holds, 1);
        return;
    }
    tell_t tell;
    LOCK();
    sched_turn_hold(&sched->turn);
    tell_prepare(&tell);
    UNLOCK();
    tell_deliver(&tell);
}

void play_scheduler_release(void)
{
    if (sched == NULL) {
        if (atomic_load(&s_early_holds) > 0) {
            atomic_fetch_sub(&s_early_holds, 1);
        }
        return;
    }
    tell_t tell;
    LOCK();
    sched_turn_release(&sched->turn, sched_now_ms());
    tell_prepare(&tell);
    UNLOCK();
    tell_deliver(&tell);
}

// A pin list that is playing follows the list.
static void on_pins_changed(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    (void)play_scheduler_refresh_source(P3A_CHANNEL_SOURCE_PINNED);
}

// A registration made or forgotten changes what Makapix's channels may list.
static void on_makapix_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->payload.i32 == 1) {
        (void)play_scheduler_refresh_source(P3A_SOURCE_MAKAPIX);
    }
}

static void on_setting_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->payload.i32 != P3A_SET_DWELL_S && event->payload.i32 != P3A_SET_BRIGHTNESS) {
        return;
    }
    LOCK();
    p3a_settings_get(&sched->settings);
    sched_turn_set_dwell(&sched->turn, (uint32_t)sched->settings.dwell_s);
    if (event->payload.i32 == P3A_SET_BRIGHTNESS && !sched->paused) {
        p3a_board_set_brightness((int)sched->settings.brightness);  // while paused it waits for the resume
    }
    UNLOCK();
}

esp_err_t play_scheduler_start(void)
{
    if (sched != NULL) {
        return ESP_OK;
    }
    sched_lock = xSemaphoreCreateRecursiveMutex();
    sched_state_t *state = psram_calloc(1, sizeof(sched_state_t));
    playset_t *playset = psram_calloc(1, sizeof(playset_t));
    if (sched_lock == NULL || state == NULL || playset == NULL) {
        free(state);
        free(playset);
        return ESP_ERR_NO_MEM;
    }
    sched = state;
    p3a_settings_get(&sched->settings);
    sched_turn_init(&sched->turn, (uint32_t)sched->settings.dwell_s);
    for (int early = atomic_exchange(&s_early_holds, 0); early > 0; early--) {
        sched_turn_hold(&sched->turn);
    }
    p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_setting_changed, NULL);
    p3a_event_subscribe(P3A_EVENT_PINS_CHANGED, on_pins_changed, NULL);
    p3a_event_subscribe(P3A_EVENT_MAKAPIX_CHANGED, on_makapix_changed, NULL);
    const esp_timer_create_args_t timer = {.callback = tick, .name = "sched_tick"};
    if (esp_timer_create(&timer, &s_tick) == ESP_OK) {
        esp_timer_start_periodic(s_tick, TICK_MS * 1000);
    }
    static const animation_player_listener_t listener = {.ready = on_ready, .shown = on_shown, .failed = on_failed};
    animation_player_set_listener(&listener, NULL);

    const esp_err_t loaded = playset_store_load_active(playset);
    if (loaded != ESP_OK) {
        memset(playset, 0, sizeof(*playset));
        playset->count = 1;
        playset->channels[0].source = PLAYSET_SOURCE_LOCAL;
        if (loaded == P3A_ERR_MISSING) {
            // A device that was never told what to play: what is on its card, and
            // Makapix Club's promoted artworks, which need no account and no key
            // (decision D90). A new device shows art as soon as it is on Wi-Fi.
            playset->count = 2;
            playset->channels[1].source = PLAYSET_SOURCE_MAKAPIX;
            snprintf(playset->channels[1].kind, sizeof(playset->channels[1].kind), "promoted");
        } else {
            ESP_LOGW(TAG, "the active playset could not be read (%s); playing the user's own files",
                     p3a_err_to_name(loaded));
        }
    }
    const esp_err_t err = sched_content_start();
    LOCK();
    install(playset);
    UNLOCK();
    free(playset);
    return err;
}
