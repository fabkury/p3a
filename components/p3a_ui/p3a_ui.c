// SPDX-License-Identifier: Apache-2.0
// The on-device UI. See p3a_ui.h.
//
// This file is the wiring around the model (ui_model.c): a lock, a timer
// for notices that expire, the events and the scheduler's state turned into
// facts, and the one consequence the model has so far: playback is held
// while a screen covers the artwork.

#include "p3a_ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_job.h"
#include "p3a_storage.h"
#include "play_scheduler.h"
#include "psram_alloc.h"
#include "sd_repair.h"
#include "intro/p3a_intro_anim_list.h"
#include "ui_card.h"
#include "ui_internal.h"
#include "ui_model.h"
#include "ui_player.h"

static const char *TAG = "ui";

#define TICK_MS        250
#define NOTICE_CARD_MS 20000

static SemaphoreHandle_t s_lock;
static ui_model_t s_model;  // guarded by s_lock
static bool s_holding;      // whether this file's hold on playback is in force; guarded by s_lock
static uint32_t s_problems_seen;
static int64_t s_dark_since_ms = -1;  // since when nothing has been on the panel; -1 while something is
static atomic_bool s_look_again;      // the "getting ready" notice is being held back; see player_job()
static atomic_bool s_player_job_queued;
static esp_timer_handle_t s_tick;
static p3a_ui_info_fn s_info;
static ui_card_t s_card;  // the SD format flow; guarded by s_lock
static ui_indicator_t s_indicator;  // guarded by s_lock
static atomic_bool s_info_job_queued;
static uint32_t s_ticks;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Acts on what the model now says. Called with the lock held, after any
// change; the scheduler is called after the lock is let go.
static void settle_and_unlock(bool changed)
{
    ui_view_t view;
    ui_model_view(&s_model, &view);
    const bool hold = view.holds_playback;
    const bool flip = hold != s_holding;
    s_holding = hold;
    if (changed) {
        const p3a_ui_content_t *content = ui_model_content(&s_model);
        ESP_LOGI(TAG, "on screen: %s%s%s%s%s", ui_screen_name(view.screen),
                 (view.banner || view.screen == P3A_SCREEN_NOTICE) ? ", notice " : "",
                 (view.banner || view.screen == P3A_SCREEN_NOTICE) ? ui_notice_name(view.notice) : "",
                 content && content->text[0] ? ": " : "", content ? content->text : "");
    }
    UNLOCK();
    if (flip) {
        hold ? play_scheduler_hold() : play_scheduler_release();
    }
    if (changed) {
        ui_display_changed(view.covers_artwork, view.banner);
    }
}

// --- the hook table -------------------------------------------------------------

static void hook_screen_show(p3a_screen_t screen, const p3a_ui_content_t *content)
{
    LOCK();
    settle_and_unlock(ui_model_screen_show(&s_model, screen, content));
}

static void hook_screen_hide(p3a_screen_t screen)
{
    LOCK();
    settle_and_unlock(ui_model_screen_hide(&s_model, screen));
}

static void hook_notice_show(p3a_notice_t notice, const p3a_ui_content_t *content, uint32_t ttl_ms, uint32_t max_per_boot)
{
    LOCK();
    settle_and_unlock(ui_model_notice_show(&s_model, notice, content, ttl_ms, max_per_boot, now_ms()));
}

static void hook_notice_hide(p3a_notice_t notice)
{
    LOCK();
    settle_and_unlock(ui_model_notice_hide(&s_model, notice));
}

static void hook_icon_show(p3a_icon_t icon, p3a_icon_place_t place)
{
    ui_display_icon((int)icon, (int)place);
}

const p3a_ui_hooks_t *p3a_ui_hook_table(void)
{
    static const p3a_ui_hooks_t table = {
        .screen_show = hook_screen_show,
        .screen_hide = hook_screen_hide,
        .notice_show = hook_notice_show,
        .notice_hide = hook_notice_hide,
        .icon_show = hook_icon_show,
    };
    return &table;
}

// --- playback -------------------------------------------------------------------

static void facts_from(const play_scheduler_status_t *st, ui_player_facts_t *f)
{
    memset(f, 0, sizeof(*f));
    // Whoever put it there, a picture is on the panel, or is decoded and
    // waiting to be drawn: the UI must not hold the panel against it, or it
    // would never be shown.
    f->playing = st->playing || st->foreign || st->staged;
    f->loading = st->loading;
    f->channels = st->channel_count;
    for (uint32_t i = 0; i < st->channel_count; i++) {
        const play_channel_status_t *c = &st->channels[i];
        f->playable += c->available;
        switch (c->state) {
        case PLAY_CHANNEL_WAITING:
        case PLAY_CHANNEL_LISTING:
            f->listing++;
            break;
        case PLAY_CHANNEL_FAILED:
            f->failed++;
            break;
        case PLAY_CHANNEL_NO_SOURCE:
            f->no_source++;
            break;
        default:
            break;
        }
        if (c->error == (esp_err_t)P3A_ERR_SD_FAILED) {
            f->card_failed = true;
        }
    }
    f->asked = st->asked;
    f->problem = st->problem;
    f->problems = st->problems;
}

// A job: reads the player's state and brings the notices about playback in
// line with it.
static esp_err_t player_job(void *arg)
{
    (void)arg;
    atomic_store(&s_player_job_queued, false);
    play_scheduler_status_t *st = psram_calloc(1, sizeof(*st));
    if (st == NULL) {
        return ESP_ERR_NO_MEM;
    }
    play_scheduler_get_status(st);
    ui_player_facts_t facts;
    facts_from(st, &facts);
    free(st);

    p3a_ui_content_t words;
    ui_player_standing_t standing = ui_player_standing(&facts, &words);
    LOCK();
    // "Getting ready" is for a wait a person would notice. At an ordinary
    // boot the first artwork is up in well under a second, and a notice
    // that flashes by is worse than a dark panel.
    if (facts.playing) {
        s_dark_since_ms = -1;
    } else if (s_dark_since_ms < 0) {
        s_dark_since_ms = now_ms();
    }
    atomic_store(&s_look_again, false);
    if (standing == UI_PLAYER_LOADING && now_ms() - s_dark_since_ms < UI_PLAYER_LOADING_AFTER_MS) {
        standing = UI_PLAYER_NONE;
        atomic_store(&s_look_again, true);  // the tick asks again
    }
    bool changed = ui_model_set_playing(&s_model, facts.playing);
    changed |= standing == UI_PLAYER_LOADING ? ui_model_notice_show(&s_model, P3A_NOTICE_LOADING, &words, 0, 0, now_ms())
                                             : ui_model_notice_hide(&s_model, P3A_NOTICE_LOADING);
    changed |= standing == UI_PLAYER_NOTHING
                   ? ui_model_notice_show(&s_model, P3A_NOTICE_NOTHING_TO_PLAY, &words, 0, 0, now_ms())
                   : ui_model_notice_hide(&s_model, P3A_NOTICE_NOTHING_TO_PLAY);
    if (facts.problem == 0) {
        changed |= ui_model_notice_hide(&s_model, P3A_NOTICE_PLAY_PROBLEM);
    } else if (facts.problems != s_problems_seen) {
        ui_player_problem(facts.problem, &words);
        changed |= ui_model_notice_show(&s_model, P3A_NOTICE_PLAY_PROBLEM, &words, UI_PLAYER_PROBLEM_TTL_MS, 0, now_ms());
    }
    s_problems_seen = facts.problems;
    ui_indicator_update(&s_indicator, &facts, now_ms());
    const int indicator = (int)ui_indicator_now(&s_indicator, now_ms());
    settle_and_unlock(changed);
    ui_display_set_indicator(indicator);
    return ESP_OK;
}

static void on_player_changed(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    // Several changes in a row need one look.
    if (!atomic_exchange(&s_player_job_queued, true) &&
        p3a_job_submit(P3A_JOBS_UI, "ui: playback", player_job, NULL, NULL) != ESP_OK) {
        atomic_store(&s_player_job_queued, false);
    }
}

// --- the information screen -----------------------------------------------------

void p3a_ui_set_info_source(p3a_ui_info_fn fill)
{
    s_info = fill;
}

// A job: asks for the screen's words and shows them. Used to open the
// screen and, while it is up, to keep what it says current (the uptime, the
// signal).
static esp_err_t info_job(void *arg)
{
    const bool open = arg != NULL;
    atomic_store(&s_info_job_queued, false);
    p3a_ui_content_t words;
    p3a_ui_content(&words, "p3a", -1, "%s", "");
    if (s_info != NULL) {
        s_info(&words);
    }
    if (p3a_storage_card_state() == P3A_CARD_FAILED) {
        // The card that was in use stopped taking writes. Formatting it is
        // offered here, where a person has come looking, and nowhere else.
        snprintf(words.buttons[0], sizeof(words.buttons[0]), "Format SD card");
        words.buttons_enabled = 1;
    }
    LOCK();
    if (!open && !s_model.screens[P3A_SCREEN_INFO].up) {
        UNLOCK();  // closed meanwhile: a refresh must not open it again
        return ESP_OK;
    }
    settle_and_unlock(ui_model_screen_show(&s_model, P3A_SCREEN_INFO, &words));
    return ESP_OK;
}

void ui_info_open(void)
{
    if (!atomic_exchange(&s_info_job_queued, true) &&
        p3a_job_submit(P3A_JOBS_UI, "ui: information", info_job, (void *)1, NULL) != ESP_OK) {
        atomic_store(&s_info_job_queued, false);
    }
}

// --- the SD format flow ---------------------------------------------------------
//
// The steps and the words are ui_card.c's. Here its commands are carried
// out: a look at the slot and the format itself as jobs, the restart.

static void card_run(ui_card_command_t command);

// Brings the card screen in line with the flow, lets go of the lock, and
// carries out what the flow asked for. Called with the lock held.
static void card_settle_and_unlock(ui_card_command_t command)
{
    p3a_ui_content_t words;
    const bool changed = ui_card_content(&s_card, now_ms(), &words)
                             ? ui_model_screen_show(&s_model, P3A_SCREEN_CARD, &words)
                             : ui_model_screen_hide(&s_model, P3A_SCREEN_CARD);
    settle_and_unlock(changed);
    card_run(command);
}

static esp_err_t card_probe_job(void *arg)
{
    (void)arg;
    ui_card_found_t found;
    switch (p3a_storage_card_probe()) {
    case P3A_CARD_IN_SERVICE:
        found = UI_CARD_FOUND_GOOD;
        break;
    case P3A_CARD_ABSENT:
        found = UI_CARD_FOUND_NOTHING;
        break;
    default:
        found = UI_CARD_FOUND_UNUSABLE;
        break;
    }
    LOCK();
    card_settle_and_unlock(ui_card_probed(&s_card, found, now_ms()));
    return ESP_OK;
}

static esp_err_t card_format_job(void *arg)
{
    (void)arg;
    // Nothing may be reading the card: playback stops for good, and the
    // player is given a moment to close its file.
    play_scheduler_shutdown();
    vTaskDelay(pdMS_TO_TICKS(500));
    const esp_err_t err = p3a_storage_format();
    LOCK();
    ui_card_formatted(&s_card, err == ESP_OK, now_ms());
    card_settle_and_unlock(UI_CARD_DO_NOTHING);
    return err;
}

static void card_run(ui_card_command_t command)
{
    switch (command) {
    case UI_CARD_DO_PROBE:
        p3a_job_submit(P3A_JOBS_IO, "ui: look at the card", card_probe_job, NULL, NULL);
        break;
    case UI_CARD_DO_FORMAT:
        p3a_job_submit(P3A_JOBS_IO, "ui: format the card", card_format_job, NULL, NULL);
        break;
    case UI_CARD_DO_RESTART:
        ESP_LOGW(TAG, "restarting after the card flow");
        esp_restart();  // asked for by a person, not a remedy: not counted
        break;
    default:
        break;
    }
}

void p3a_ui_card_check(void)
{
    // A card whose file tables had to be repaired at boot is a card that is
    // losing data: the person should know, once.
    const sd_repair_fat_stats_t *fat = sd_repair_fat_stats();
    if (fat->ran && (fat->repaired_fat1 != 0 || fat->repaired_fat2 != 0)) {
        p3a_ui_content_t words;
        p3a_ui_content(&words, "SD card", -1, "The SD card was repaired. Consider replacing it");
        hook_notice_show(P3A_NOTICE_CARD_REPAIRED, &words, NOTICE_CARD_MS, 1);
    }
    const p3a_card_state_t state = p3a_storage_card_state();
    if (state == P3A_CARD_NO_FILESYSTEM || state == P3A_CARD_ABSENT) {
        LOCK();
        ui_card_offer(&s_card);
        card_settle_and_unlock(UI_CARD_DO_NOTHING);
    }
}

void ui_card_press(int button)
{
    LOCK();
    card_settle_and_unlock(ui_card_button(&s_card, button, now_ms()));
}

void ui_card_cancel(void)
{
    LOCK();
    ui_card_long_press(&s_card);
    card_settle_and_unlock(UI_CARD_DO_NOTHING);
}

void ui_card_start(void)
{
    LOCK();
    ui_card_ask(&s_card, now_ms());
    card_settle_and_unlock(UI_CARD_DO_NOTHING);
}

// --- the card -------------------------------------------------------------------

static void on_card_failed(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    p3a_ui_content_t words;
    p3a_ui_content(&words, "SD card", -1, "The SD card is not working. Long press for details");
    // Said once, for a while (as 1.x did); the information screen goes on
    // saying it, and offers the format.
    hook_notice_show(P3A_NOTICE_CARD_FAILED, &words, NOTICE_CARD_MS, 1);
}

// --- the rest -------------------------------------------------------------------

static void tick(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    const bool info_up = s_model.screens[P3A_SCREEN_INFO].up;
    bool changed = ui_model_tick(&s_model, now_ms());
    // The format flow's notices end and its countdown counts by the clock.
    ui_card_command_t command = UI_CARD_DO_NOTHING;
    if (s_card.phase != UI_CARD_NONE) {
        command = ui_card_tick(&s_card, now_ms());
        p3a_ui_content_t words;
        changed |= ui_card_content(&s_card, now_ms(), &words) ? ui_model_screen_show(&s_model, P3A_SCREEN_CARD, &words)
                                                             : ui_model_screen_hide(&s_model, P3A_SCREEN_CARD);
    }
    const int indicator = (int)ui_indicator_now(&s_indicator, now_ms());  // its red runs out by the clock
    settle_and_unlock(changed);
    ui_display_set_indicator(indicator);
    ui_display_tick();
    card_run(command);
    if (atomic_load(&s_look_again)) {
        on_player_changed(NULL, NULL);
    }
    // Every two seconds while the information screen is up.
    if (info_up && ++s_ticks % 8 == 0 && !atomic_exchange(&s_info_job_queued, true) &&
        p3a_job_submit(P3A_JOBS_UI, "ui: information", info_job, NULL, NULL) != ESP_OK) {
        atomic_store(&s_info_job_queued, false);
    }
}

void p3a_ui_get_state(p3a_ui_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    ui_view_t view;
    ui_model_view(&s_model, &view);
    out->screen = view.screen;
    out->notice = view.notice;
    out->banner = view.banner;
    out->covers_artwork = view.covers_artwork;
    const p3a_ui_content_t *content = ui_model_content(&s_model);
    out->has_content = content != NULL;
    if (content != NULL) {
        out->content = *content;
    }
    out->revision = s_model.revision;
    UNLOCK();
}

int p3a_ui_intro_count(void)
{
    return p3a_intro_anim_get_count();
}

const char *p3a_ui_intro_name(int index)
{
    return p3a_intro_anim_get_name(index);
}

const char *p3a_ui_screen_name(p3a_screen_t screen)
{
    return ui_screen_name(screen);
}

const char *p3a_ui_notice_name(p3a_notice_t notice)
{
    return ui_notice_name(notice);
}

esp_err_t p3a_ui_start(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ui_model_init(&s_model);
    ui_card_init(&s_card);
    ui_display_start();
    // The boot animation is the first thing on the panel. Whatever else
    // comes up meanwhile (the first artwork, a notice) waits under it.
    if (ui_intro_arm()) {
        hook_screen_show(P3A_SCREEN_INTRO, NULL);
    }
    esp_err_t err = p3a_event_subscribe(P3A_EVENT_PLAYER_CHANGED, on_player_changed, NULL);
    if (err == ESP_OK) {
        err = p3a_event_subscribe(P3A_EVENT_SD_HEALTH_FAILED, on_card_failed, NULL);
    }
    const esp_timer_create_args_t timer = {.callback = tick, .name = "ui_tick"};
    if (err == ESP_OK) {
        err = esp_timer_create(&timer, &s_tick);
    }
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_tick, TICK_MS * 1000);
    }
    on_player_changed(NULL, NULL);
    if (err == ESP_OK) {
        err = ui_touch_start();
    }
    return err;
}
