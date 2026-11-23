// SPDX-License-Identifier: Apache-2.0
// The one model of what is on the screen. See ui_model.h. Pure.

#include "ui_model.h"

#include <string.h>

void ui_model_init(ui_model_t *model)
{
    memset(model, 0, sizeof(*model));
    for (int i = 0; i < P3A_SCREEN_COUNT; i++) {
        model->screens[i].content.progress = -1;
    }
    for (int i = 0; i < P3A_NOTICE_COUNT; i++) {
        model->notices[i].content.progress = -1;
    }
}

static bool same_content(const p3a_ui_content_t *a, const p3a_ui_content_t *b)
{
    return a->progress == b->progress && strncmp(a->title, b->title, sizeof(a->title)) == 0 &&
           strncmp(a->text, b->text, sizeof(a->text)) == 0 && strncmp(a->hint, b->hint, sizeof(a->hint)) == 0;
}

static void copy_content(p3a_ui_content_t *to, const p3a_ui_content_t *from)
{
    *to = *from;
    to->title[sizeof(to->title) - 1] = '\0';
    to->text[sizeof(to->text) - 1] = '\0';
    to->hint[sizeof(to->hint) - 1] = '\0';
}

// The slot whose words are seen, or NULL.
static const ui_slot_t *seen_slot(const ui_model_t *model, const ui_view_t *view)
{
    if (view->screen == P3A_SCREEN_NOTICE || view->banner) {
        return &model->notices[view->notice];
    }
    return view->screen == P3A_SCREEN_ARTWORK ? NULL : &model->screens[view->screen];
}

void ui_model_view(const ui_model_t *model, ui_view_t *out)
{
    memset(out, 0, sizeof(*out));
    int notice = -1;
    for (int i = P3A_NOTICE_COUNT - 1; i >= 0 && notice < 0; i--) {
        if (model->notices[i].up) {
            notice = i;
        }
    }
    out->screen = P3A_SCREEN_ARTWORK;
    for (int i = P3A_SCREEN_COUNT - 1; i > P3A_SCREEN_NOTICE; i--) {
        if (model->screens[i].up) {
            out->screen = (p3a_screen_t)i;
            break;
        }
    }
    if (out->screen == P3A_SCREEN_ARTWORK && notice >= 0) {
        out->notice = (p3a_notice_t)notice;
        if (model->playing) {
            out->banner = true;  // the artwork stays; the notice rides on it
        } else {
            out->screen = P3A_SCREEN_NOTICE;
        }
    }
    out->covers_artwork = out->screen != P3A_SCREEN_ARTWORK;
    // A notice with nothing playing holds nothing: there is nothing to hold.
    out->holds_playback = out->screen > P3A_SCREEN_NOTICE;
}

const p3a_ui_content_t *ui_model_content(const ui_model_t *model)
{
    ui_view_t view;
    ui_model_view(model, &view);
    const ui_slot_t *slot = seen_slot(model, &view);
    return slot ? &slot->content : NULL;
}

// Runs a change and reports whether what is seen differs afterwards.
typedef struct {
    ui_view_t view;
    p3a_ui_content_t content;
    bool has_content;
} snapshot_t;

static void snapshot(const ui_model_t *model, snapshot_t *out)
{
    ui_model_view(model, &out->view);
    const ui_slot_t *slot = seen_slot(model, &out->view);
    out->has_content = slot != NULL;
    if (slot != NULL) {
        out->content = slot->content;
    }
}

static bool changed_since(ui_model_t *model, const snapshot_t *before)
{
    snapshot_t after;
    snapshot(model, &after);
    const bool changed = after.view.screen != before->view.screen || after.view.banner != before->view.banner ||
                         ((after.view.banner || after.view.screen == P3A_SCREEN_NOTICE) &&
                          after.view.notice != before->view.notice) ||
                         after.has_content != before->has_content ||
                         (after.has_content && !same_content(&after.content, &before->content));
    if (changed) {
        model->revision++;
    }
    return changed;
}

bool ui_model_screen_show(ui_model_t *model, p3a_screen_t screen, const p3a_ui_content_t *content)
{
    if (screen <= P3A_SCREEN_NOTICE || screen >= P3A_SCREEN_COUNT) {
        return false;
    }
    snapshot_t before;
    snapshot(model, &before);
    ui_slot_t *slot = &model->screens[screen];
    if (!slot->up) {
        slot->up = true;
        slot->shows++;
    }
    if (content != NULL) {
        copy_content(&slot->content, content);
    }
    return changed_since(model, &before);
}

bool ui_model_screen_hide(ui_model_t *model, p3a_screen_t screen)
{
    if (screen <= P3A_SCREEN_NOTICE || screen >= P3A_SCREEN_FATAL) {
        return false;  // nothing takes the fatal screen down but a restart
    }
    snapshot_t before;
    snapshot(model, &before);
    model->screens[screen].up = false;
    return changed_since(model, &before);
}

bool ui_model_notice_show(ui_model_t *model, p3a_notice_t notice, const p3a_ui_content_t *content, uint32_t ttl_ms,
                          uint32_t max_per_boot, int64_t now_ms)
{
    if ((unsigned)notice >= P3A_NOTICE_COUNT) {
        return false;
    }
    ui_slot_t *slot = &model->notices[notice];
    if (!slot->up && max_per_boot != 0 && slot->shows >= max_per_boot) {
        return false;
    }
    snapshot_t before;
    snapshot(model, &before);
    if (!slot->up) {
        slot->up = true;
        slot->shows++;
    }
    if (content != NULL) {
        copy_content(&slot->content, content);
    }
    slot->expires_ms = ttl_ms != 0 ? now_ms + (int64_t)ttl_ms : 0;
    return changed_since(model, &before);
}

bool ui_model_notice_hide(ui_model_t *model, p3a_notice_t notice)
{
    if ((unsigned)notice >= P3A_NOTICE_COUNT) {
        return false;
    }
    snapshot_t before;
    snapshot(model, &before);
    model->notices[notice].up = false;
    return changed_since(model, &before);
}

bool ui_model_set_playing(ui_model_t *model, bool playing)
{
    snapshot_t before;
    snapshot(model, &before);
    model->playing = playing;
    return changed_since(model, &before);
}

bool ui_model_tick(ui_model_t *model, int64_t now_ms)
{
    snapshot_t before;
    snapshot(model, &before);
    for (int i = 0; i < P3A_NOTICE_COUNT; i++) {
        ui_slot_t *slot = &model->notices[i];
        if (slot->up && slot->expires_ms != 0 && now_ms >= slot->expires_ms) {
            slot->up = false;
        }
    }
    return changed_since(model, &before);
}

const char *ui_screen_name(p3a_screen_t screen)
{
    static const char *const names[P3A_SCREEN_COUNT] = {"artwork", "notice", "intro",  "info", "registration",
                                                         "setup",   "usb",    "update", "fatal"};
    return (unsigned)screen < P3A_SCREEN_COUNT ? names[screen] : "unknown";
}

const char *ui_notice_name(p3a_notice_t notice)
{
    static const char *const names[P3A_NOTICE_COUNT] = {"loading",      "downloading",   "nothing_to_play", "source_problem",
                                                         "play_problem", "card_repaired", "card_failed"};
    return (unsigned)notice < P3A_NOTICE_COUNT ? names[notice] : "unknown";
}
