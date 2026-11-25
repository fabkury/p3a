// SPDX-License-Identifier: Apache-2.0
// The content task: the one task that fills the scheduler's channels. It
// lists a channel when its time has come (sched_refresh.c says when), keeps
// each remote channel's index on the card so that the channel plays at the
// next boot before the network is there, and downloads the artworks the
// indices name, one transfer at a time (D63).
//
// One pass does at most one thing, in this order: load an index from the
// card, list a channel, save an index, make room on the card, download an
// artwork. Then it looks
// again. With nothing to do it sleeps until something wakes it, or a
// second.
//
// The scheduler's lock is taken to choose the work and to put the result
// in, never across the card or the network.

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "animation_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_fetch.h"
#include "p3a_blob.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_evict.h"
#include "p3a_frame.h"
#include "p3a_limits.h"
#include "p3a_mem.h"
#include "p3a_storage.h"
#include "p3a_task.h"
#include "psram_alloc.h"
#include "sched_state.h"
#include "sd_health.h"
#include "sd_path.h"

static const char *TAG = "content";

#define SAVE_AFTER_MS   30000                // an index changed by downloads is saved this long after the first change
#define FAILS_MAX       3                    // downloads of one artwork before it is left alone until the restart
#define FREE_MIN_BYTES  (16ull * 1024 * 1024)  // no download onto a card with less room than this
#define SOURCE_BLOCK_MS (15 * 60 * 1000)     // after a source refuses a download (rate limit, key)
#define PAUSE_MIN_MS    10000                // after a download that failed for another reason; doubles, up to
#define PAUSE_MAX_MS    300000
#define IDLE_MS         1000
#define MEASURE_EVERY   32                   // downloads between two measurements of the card's free space
#define EVICT_RETRY_MS  (10 * 60 * 1000)     // after an eviction that could not free what was wanted
#define STALE_INDEX_S   (60 * 86400)         // the index of a channel no playset has played for this long is dropped

static TaskHandle_t s_task;
static atomic_bool s_net_up, s_busy;
static int64_t s_source_blocked_until[P3A_CHANNEL_SOURCE_END_];
static int64_t s_pause_until;
static uint32_t s_fail_streak;
static uint32_t s_next_channel;
static uint32_t s_unmeasured;     // downloads since the free space was last measured
static int64_t s_evict_not_before;
static char s_url[1024];  // one task: no need for a stack this deep

void sched_content_wake(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

bool sched_content_wait_idle(uint32_t timeout_ms)
{
    sched_content_wake();
    for (uint32_t waited = 0; atomic_load(&s_busy); waited += 20) {
        if (waited >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static esp_err_t index_path(const char *id, char *out, size_t len)
{
    char dir[128];
    const esp_err_t err = sd_path_get_subdir("channels", dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    return snprintf(out, len, "%s/%s.idx", dir, id) < (int)len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static uint32_t capacity_of(const p3a_source_ops_t *ops)
{
    return ops->index_capacity ? ops->index_capacity() : SCHED_DEFAULT_CAPACITY;
}

static bool channel_is_live(const sched_slot_t *ch)
{
    return ch->state == PLAY_CHANNEL_WAITING || ch->state == PLAY_CHANNEL_READY || ch->state == PLAY_CHANNEL_FAILED;
}

// --- an index from the card -------------------------------------------------------

static bool load_one(void)
{
    char id[PLAYSET_CHANNEL_ID_LEN];
    const p3a_source_ops_t *ops = NULL;
    uint32_t at = SCHED_NONE, generation = 0;
    LOCK();
    for (uint32_t i = 0; i < sched->playset.count && at == SCHED_NONE; i++) {
        sched_slot_t *ch = &sched->channels[i];
        if (!channel_is_live(ch) || ch->load_tried) {
            continue;
        }
        ch->load_tried = true;
        ops = p3a_source_get(sched->playset.channels[i].source);
        if (ops->remote && !ch->has_index) {
            at = i;
            memcpy(id, ch->id, sizeof(id));
            generation = sched->generation;
        }
    }
    UNLOCK();
    if (at == SCHED_NONE) {
        return false;
    }

    char path[P3A_FS_PATH_MAX];
    p3a_index_t index = {0};
    esp_err_t err = index_path(id, path, sizeof(path));
    if (err == ESP_OK) {
        err = p3a_index_create(&index, capacity_of(ops));
    }
    if (err == ESP_OK) {
        err = p3a_index_load(&index, path);
    }
    int64_t age_s = -1;  // unknown unless the clock is right
    struct stat st;
    if (err == ESP_OK && p3a_clock_is_set() && stat(path, &st) == 0) {
        age_s = (int64_t)time(NULL) - (int64_t)st.st_mtime;
    }
    if (err == ESP_OK) {
        // What failed before the restart is tried again after it.
        for (uint32_t i = 0; i < index.count; i++) {
            index.records[i].download_fails = 0;
            index.records[i].resolve_fails = 0;
        }
    } else if (err != P3A_ERR_MISSING) {
        ESP_LOGW(TAG, "channel %s: its index on the card could not be read (%s); it is listed afresh", id,
                 p3a_err_to_name(err));
    }

    LOCK();
    sched_slot_t *ch = &sched->channels[at];
    if (err == ESP_OK && generation == sched->generation && !ch->has_index) {
        ch->index = index;
        ch->has_index = true;
        if (ch->state == PLAY_CHANNEL_WAITING) {
            ch->state = PLAY_CHANNEL_READY;
        }
        sched_core_set_available(&sched->core, at, ch->index.available_count);
        sched_refresh_known(&ch->refresh, age_s, sched_now_ms());
        ESP_LOGI(TAG, "channel %s (%s): %u records known, %u playable", id, ops->name, (unsigned)ch->index.count,
                 (unsigned)ch->index.available_count);
        sched_start_if_wanted();
    } else {
        p3a_index_destroy(&index);
    }
    UNLOCK();
    sched_changed();
    return true;
}

// --- listing a channel --------------------------------------------------------------

static sched_list_result_t list_result(esp_err_t err)
{
    switch (err) {
    case ESP_OK:               return SCHED_LISTED;
    case P3A_ERR_RATE_LIMITED: return SCHED_LIST_RATE_LIMITED;
    case P3A_ERR_NO_KEY:
    case P3A_ERR_AUTH:         return SCHED_LIST_NO_KEY;
    case P3A_ERR_SD_FAILED:    return SCHED_LIST_CARD;
    default:                   return SCHED_LIST_FAILED;
    }
}

static bool list_one(void)
{
    LOCK();
    uint32_t at = SCHED_NONE;
    const int64_t now = sched_now_ms();
    for (uint32_t i = 0; i < sched->playset.count; i++) {
        sched_slot_t *ch = &sched->channels[i];
        // A listing over the network is optional work: not begun while internal memory is short.
        if (channel_is_live(ch) && ch->load_tried && sched_refresh_due(&ch->refresh, now, atomic_load(&s_net_up)) &&
            (!ch->refresh.needs_network || p3a_mem_level() == P3A_MEM_OK)) {
            at = i;
            break;
        }
    }
    if (at == SCHED_NONE) {
        UNLOCK();
        return false;
    }
    const uint32_t generation = sched->generation;
    const playset_channel_t spec = sched->playset.channels[at];
    sched->channels[at].state = PLAY_CHANNEL_LISTING;
    sched_refresh_started(&sched->channels[at].refresh);
    UNLOCK();
    sched_changed();

    const p3a_source_ops_t *ops = p3a_source_get(spec.source);  // checked when the playset was activated
    const p3a_channel_ref_t ref = {.kind = spec.kind, .identifier = spec.identifier, .offset = spec.offset};
    p3a_index_t fresh = {0};
    esp_err_t err = p3a_index_create(&fresh, capacity_of(ops));
    if (err == ESP_OK) {
        err = ops->refresh(&ref, &fresh);
    }

    LOCK();
    sched_slot_t *ch = &sched->channels[at];
    if (generation != sched->generation) {
        p3a_index_destroy(&fresh);  // the playset changed meanwhile
    } else {
        sched_refresh_finished(&ch->refresh, list_result(err), 0, sched_now_ms());
        ch->listings++;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "channel %s (%s) could not be listed: %s", ch->id, ops->name, p3a_err_to_name(err));
            p3a_index_destroy(&fresh);
            ch->error = err;
            // What it held before, if anything, stays playable.
            ch->state = ch->has_index ? PLAY_CHANNEL_READY : PLAY_CHANNEL_FAILED;
        } else {
            p3a_index_merge_t merge = {0};
            p3a_index_carry_over(&fresh, ch->has_index ? &ch->index : NULL, &merge);
            if (ch->has_index) {
                p3a_index_destroy(&ch->index);
            }
            ch->index = fresh;
            ch->has_index = true;
            ch->error = ESP_OK;
            ch->state = PLAY_CHANNEL_READY;
            ch->cursor = 0;
            if (ops->remote) {
                ch->dirty = true;
                ch->save_at_ms = 0;  // a fresh listing is saved at once
            }
            sched_core_set_available(&sched->core, at, ch->index.available_count);
            ESP_LOGI(TAG, "channel %s (%s): %u records (%u new, %u no longer listed), %u playable", ch->id, ops->name,
                     (unsigned)ch->index.count, (unsigned)merge.added, (unsigned)merge.dropped,
                     (unsigned)ch->index.available_count);
            sched_start_if_wanted();
        }
    }
    UNLOCK();
    sched_changed();
    return true;
}

// --- an index to the card -----------------------------------------------------------

static bool save_one(void)
{
    char id[PLAYSET_CHANNEL_ID_LEN];
    p3a_record_t *copy = NULL;
    uint32_t count = 0;
    bool found = false;
    LOCK();
    const int64_t now = sched_now_ms();
    for (uint32_t i = 0; i < sched->playset.count && !found; i++) {
        sched_slot_t *ch = &sched->channels[i];
        if (!ch->dirty || !ch->has_index || now < ch->save_at_ms) {
            continue;
        }
        // Copied under the lock and written without it: a write to the card
        // can take a long time, and the player's reports wait on this lock.
        count = ch->index.count;
        copy = psram_malloc((size_t)(count > 0 ? count : 1) * sizeof(p3a_record_t));
        if (copy == NULL) {
            break;  // tried again on a later pass
        }
        memcpy(copy, ch->index.records, (size_t)count * sizeof(p3a_record_t));
        memcpy(id, ch->id, sizeof(id));
        ch->dirty = false;
        found = true;
    }
    UNLOCK();
    if (!found) {
        return false;
    }
    char path[P3A_FS_PATH_MAX];
    esp_err_t err = index_path(id, path, sizeof(path));
    if (err == ESP_OK) {
        err = sd_path_ensure_parent_dirs(path);
    }
    if (err == ESP_OK) {
        const p3a_index_t snapshot = {.records = copy, .count = count};
        err = p3a_index_save(&snapshot, path, P3A_FRAME_REBUILDABLE);
    }
    free(copy);
    if (err != ESP_OK) {
        // Not tried again by itself: the next change to the index will.
        ESP_LOGW(TAG, "channel %s: its index could not be saved: %s", id, p3a_err_to_name(err));
    }
    return true;
}

// --- old indices ----------------------------------------------------------------------

// Once per boot, when the date is known: the indices of channels that no
// playset has played for two months are deleted. They are caches of what a
// source lists, and a channel that comes back is listed afresh. The active
// playset's are kept whatever their age.
static bool drop_stale_indices(void)
{
    static bool done;
    if (done || !p3a_clock_is_set()) {
        return false;
    }
    done = true;
    char path[P3A_FS_PATH_MAX];
    if (sd_path_get_subdir("channels", path, sizeof(path)) != ESP_OK) {
        return false;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return false;
    }
    const size_t base = strlen(path);
    const time_t now = time(NULL);
    uint32_t dropped = 0, seen = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        const size_t len = strlen(e->d_name);
        if (len != PLAYSET_CHANNEL_ID_LEN - 1 + 4 || strcmp(e->d_name + len - 4, ".idx") != 0) {
            continue;  // not an index this code wrote
        }
        bool active = false;
        LOCK();
        for (uint32_t i = 0; i < sched->playset.count && !active; i++) {
            active = strncmp(sched->channels[i].id, e->d_name, PLAYSET_CHANNEL_ID_LEN - 1) == 0;
        }
        UNLOCK();
        struct stat st;
        snprintf(path + base, sizeof(path) - base, "/%s", e->d_name);
        if (!active && stat(path, &st) == 0 && now - st.st_mtime > STALE_INDEX_S && unlink(path) == 0) {
            dropped++;
        }
        if (++seen % 32 == 0) {
            vTaskDelay(1);
        }
    }
    closedir(dir);
    if (dropped > 0) {
        ESP_LOGI(TAG, "%u indices of channels not played for two months were dropped", (unsigned)dropped);
    }
    return true;
}

// --- room on the card ---------------------------------------------------------------

typedef struct {
    uint8_t current[P3A_RECORD_BLOB_REF_LEN], pending[P3A_RECORD_BLOB_REF_LEN];
} keep_t;

// What must stay whatever its age: the artwork on the panel and the one
// being loaded. (The user's pins join them with the pin lists.)
static bool keep_blob(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], void *ctx)
{
    const keep_t *keep = ctx;
    return memcmp(ref, keep->current, P3A_RECORD_BLOB_REF_LEN) == 0 ||
           memcmp(ref, keep->pending, P3A_RECORD_BLOB_REF_LEN) == 0;
}

// Measures the free space when enough has been written since the last
// time, and deletes the oldest blobs when it has fallen below the mark.
static bool make_room(void)
{
    p3a_storage_space_t space = p3a_storage_space();
    if (sd_health_is_failed() || !space.valid) {
        return false;
    }
    if (s_unmeasured >= MEASURE_EVERY) {
        s_unmeasured = 0;
        p3a_storage_space_refresh();
        return true;
    }
    uint64_t start = 0, stop = 0;
    const int64_t now = sched_now_ms();
    if (!p3a_evict_marks(space.total_bytes, &start, &stop) || space.free_bytes >= start || now < s_evict_not_before ||
        !p3a_clock_is_set()) {
        return false;
    }
    keep_t keep = {0};
    LOCK();
    if (sched->playing) {
        memcpy(keep.current, sched->current.blob_ref, sizeof(keep.current));
    }
    if (sched->pending_token != 0) {
        memcpy(keep.pending, sched->pending_record.blob_ref, sizeof(keep.pending));
    }
    UNLOCK();
    const uint64_t need = stop - space.free_bytes;
    p3a_blob_evict_t done = {0};
    const esp_err_t err = p3a_blob_evict(need, P3A_EVICT_MIN_AGE_S, keep_blob, &keep, &done);
    if (err != ESP_OK || done.freed_bytes < need) {
        // The card is full of what may not go yet. Not walked again at once.
        s_evict_not_before = now + EVICT_RETRY_MS;
    }
    p3a_storage_space_refresh();
    // A record whose blob went is found out when it is next played: it is
    // then set aside and fetched again (play_scheduler.c, on_failed).
    return true;
}

// --- downloading an artwork ---------------------------------------------------------

static bool wanted(const p3a_record_t *r)
{
    return !p3a_record_has_blob(r) && (r->flags & (P3A_RECORD_IN_PLACE | P3A_RECORD_GONE)) == 0 &&
           r->download_fails < FAILS_MAX;
}

uint32_t sched_content_wanted(const p3a_index_t *index)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < index->count; i++) {
        n += wanted(&index->records[i]) ? 1 : 0;
    }
    return n;
}

bool sched_content_online(void)
{
    return atomic_load(&s_net_up);
}

static bool should_abort(void *ctx)
{
    const uint32_t generation = *(const uint32_t *)ctx;
    return sched->shut_down || sched->generation != generation || p3a_storage_is_handed_over();
}

// The format of a downloaded file, by its first bytes.
static p3a_format_t format_of(const char *path)
{
    uint8_t head[32];
    FILE *f = fopen(path, "rb");
    const size_t n = f != NULL ? fread(head, 1, sizeof(head), f) : 0;
    if (f != NULL) {
        fclose(f);
    }
    return (p3a_format_t)animation_player_format_of(head, n);
}

static esp_err_t fetch(const p3a_source_ops_t *ops, const p3a_record_t *record, uint32_t generation,
                       uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t *format)
{
    esp_err_t err = ops->url(record, s_url, sizeof(s_url));
    if (err != ESP_OK) {
        return err;
    }
    char dir[128], path[192];
    err = sd_path_get_temporary(dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    snprintf(path, sizeof(path), "%s/download.part", dir);
    const http_fetch_request_t req = {.url = s_url,
                                      .user_agent = ops->user_agent ? ops->user_agent : "p3a",
                                      .max_size = P3A_MAX_ARTWORK_BYTES,
                                      .leave_temp = true,  // the blob store moves it into place
                                      .redirect_mode = ops->redirects ? HTTP_FETCH_REDIRECT_MANUAL : HTTP_FETCH_REDIRECT_AUTO,
                                      .max_redirects = 5,
                                      .treat_empty_as_not_found = true,
                                      .wait_on_sdio = true,  // not while the Wi-Fi chip's firmware is being written
                                      .hash_file = true,
                                      .should_abort = should_abort,
                                      .user_ctx = &generation};
    http_fetch_result_t result = {0};
    err = http_fetch_to_file(&req, path, &result);
    if (err == ESP_OK) {
        *format = format_of(path);
        // Something that is not a picture (an error page served with 200) is not kept.
        err = *format == P3A_FORMAT_UNKNOWN ? (esp_err_t)P3A_ERR_NOT_PLAYABLE
                                            : p3a_blob_adopt_hashed(path, *format, result.sha256, ref, NULL);
    }
    if (err != ESP_OK) {
        unlink(path);
    }
    return err;
}

static bool download_one(void)
{
    const int64_t now = sched_now_ms();
    if (!atomic_load(&s_net_up) || now < s_pause_until || p3a_mem_level() != P3A_MEM_OK || sd_health_is_failed()) {
        return false;
    }
    const p3a_storage_space_t space = p3a_storage_space();
    if (space.valid && space.free_bytes < FREE_MIN_BYTES) {
        return false;
    }
    p3a_record_t record;
    const p3a_source_ops_t *ops = NULL;
    uint32_t at = SCHED_NONE, generation = 0;
    uint8_t source = 0;
    LOCK();
    const uint32_t channels = sched->playset.count;
    for (uint32_t k = 0; k < channels && at == SCHED_NONE; k++) {
        const uint32_t i = (s_next_channel + k) % channels;
        sched_slot_t *ch = &sched->channels[i];
        source = sched->playset.channels[i].source;
        const p3a_source_ops_t *candidate = p3a_source_get(source);
        if (!ch->has_index || candidate == NULL || !candidate->remote || candidate->url == NULL ||
            now < s_source_blocked_until[source]) {
            continue;
        }
        for (uint32_t n = 0; n < ch->index.count; n++) {
            const uint32_t pos = (ch->cursor + n) % ch->index.count;
            if (wanted(&ch->index.records[pos])) {
                record = ch->index.records[pos];
                ch->cursor = pos + 1;
                at = i;
                ops = candidate;
                generation = sched->generation;
                s_next_channel = i + 1;  // the next download is another channel's, if one wants any
                break;
            }
        }
    }
    if (at != SCHED_NONE) {
        sched->downloading = true;
    }
    UNLOCK();
    if (at == SCHED_NONE) {
        return false;
    }
    sched_changed();

    uint8_t ref[P3A_RECORD_BLOB_REF_LEN] = {0};
    p3a_format_t format = P3A_FORMAT_UNKNOWN;
    const esp_err_t err = fetch(ops, &record, generation, ref, &format);

    LOCK();
    sched->downloading = false;
    sched_slot_t *ch = &sched->channels[at];
    const int32_t pos = generation == sched->generation && ch->has_index ? p3a_index_find_record(&ch->index, &record) : -1;
    p3a_record_t *r = pos >= 0 ? p3a_index_at(&ch->index, (uint32_t)pos) : NULL;
    const int64_t after = sched_now_ms();
    if (err == ESP_OK) {
        sched->downloads++;
        s_unmeasured++;
        s_fail_streak = 0;
        if (r != NULL) {
            memcpy(r->blob_ref, ref, sizeof(r->blob_ref));
            r->format = (uint8_t)format;
            r->download_fails = 0;
            p3a_index_set_available(&ch->index, (uint32_t)pos, true);
            sched_core_set_available(&sched->core, at, ch->index.available_count);
            sched_start_if_wanted();
        }
    } else if (should_abort(&generation)) {
        // Nobody's fault: the playset changed, or the card is being taken away.
    } else if (err == P3A_ERR_MISSING) {
        sched->download_failures++;
        if (r != NULL) {
            r->flags |= P3A_RECORD_GONE;  // the source no longer has it: never asked for again
        }
    } else if (err == P3A_ERR_RATE_LIMITED || err == P3A_ERR_AUTH || err == P3A_ERR_NO_KEY) {
        sched->download_failures++;
        s_source_blocked_until[source] = after + SOURCE_BLOCK_MS;
        ESP_LOGW(TAG, "%s refused a download (%s): none from it for 15 minutes", ops->name, p3a_err_to_name(err));
    } else if (err == P3A_ERR_SD_FAILED || !atomic_load(&s_net_up)) {
        // The card, or the network went away: the artwork is not to blame.
    } else {
        sched->download_failures++;
        if (r != NULL && r->download_fails < UINT8_MAX) {
            r->download_fails++;
        }
        const uint32_t doublings = s_fail_streak < 5 ? s_fail_streak : 5;
        const int64_t pause = (int64_t)PAUSE_MIN_MS << doublings;
        s_pause_until = after + (pause > PAUSE_MAX_MS ? PAUSE_MAX_MS : pause);
        s_fail_streak++;
        ESP_LOGW(TAG, "a download from %s failed: %s", ops->name, p3a_err_to_name(err));
    }
    if (r != NULL && !ch->dirty && !should_abort(&generation)) {
        ch->dirty = true;
        ch->save_at_ms = after + SAVE_AFTER_MS;
    }
    UNLOCK();
    sched_changed();
    return true;
}

// --- the task -------------------------------------------------------------------------

static void content_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool worked = false;
        atomic_store(&s_busy, true);
        if (!sched->shut_down && !p3a_storage_is_handed_over()) {
            worked = load_one() || list_one() || save_one() || make_room() || download_one() || drop_stale_indices();
        }
        atomic_store(&s_busy, false);
        if (!worked) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_MS));
        }
    }
}

// The network is back: what failed for want of it is tried now.
static void on_net(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    const bool up = event->type == P3A_EVENT_NET_UP;
    atomic_store(&s_net_up, up);
    if (up) {
        LOCK();
        const int64_t now = sched_now_ms();
        for (uint32_t i = 0; i < sched->playset.count; i++) {
            sched_slot_t *ch = &sched->channels[i];
            if (ch->refresh.needs_network && ch->refresh.failures > 0 && ch->refresh.last == SCHED_LIST_FAILED) {
                sched_refresh_ask(&ch->refresh, now);
            }
        }
        s_pause_until = 0;
        s_fail_streak = 0;
        UNLOCK();
        sched_content_wake();
    }
}

// A setting changed: it may be the key a source was waiting for.
static void on_setting(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    LOCK();
    const int64_t now = sched_now_ms();
    bool any = false;
    for (uint32_t i = 0; i < sched->playset.count; i++) {
        sched_slot_t *ch = &sched->channels[i];
        if (ch->refresh.failures > 0 && ch->refresh.last == SCHED_LIST_NO_KEY) {
            sched_refresh_ask(&ch->refresh, now);
            any = true;
        }
    }
    if (any) {
        memset(s_source_blocked_until, 0, sizeof(s_source_blocked_until));
    }
    UNLOCK();
    if (any) {
        sched_content_wake();
    }
}

esp_err_t sched_content_start(void)
{
    p3a_event_subscribe(P3A_EVENT_NET_UP, on_net, NULL);
    p3a_event_subscribe(P3A_EVENT_NET_DOWN, on_net, NULL);
    p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_setting, NULL);
    const p3a_task_config_t config = P3A_TASK_CONFIG("content", content_task, NULL, 8192, P3A_TASK_NETWORK);
    return p3a_task_create(&config, &s_task);
}
