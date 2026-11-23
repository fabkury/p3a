// SPDX-License-Identifier: Apache-2.0
// What any layer may ask the UI to show. See p3a_ui_hooks.h. Pure.

#include "p3a_ui_hooks.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic(const p3a_ui_hooks_t *) s_hooks;

void p3a_ui_hooks_set(const p3a_ui_hooks_t *hooks)
{
    atomic_store(&s_hooks, hooks);
}

void p3a_ui_screen_show(p3a_screen_t screen, const p3a_ui_content_t *content)
{
    const p3a_ui_hooks_t *hooks = atomic_load(&s_hooks);
    if (hooks != NULL && hooks->screen_show != NULL) {
        hooks->screen_show(screen, content);
    }
}

void p3a_ui_screen_hide(p3a_screen_t screen)
{
    const p3a_ui_hooks_t *hooks = atomic_load(&s_hooks);
    if (hooks != NULL && hooks->screen_hide != NULL) {
        hooks->screen_hide(screen);
    }
}

void p3a_ui_notice_show(p3a_notice_t notice, const p3a_ui_content_t *content, uint32_t ttl_ms, uint32_t max_per_boot)
{
    const p3a_ui_hooks_t *hooks = atomic_load(&s_hooks);
    if (hooks != NULL && hooks->notice_show != NULL) {
        hooks->notice_show(notice, content, ttl_ms, max_per_boot);
    }
}

void p3a_ui_notice_hide(p3a_notice_t notice)
{
    const p3a_ui_hooks_t *hooks = atomic_load(&s_hooks);
    if (hooks != NULL && hooks->notice_hide != NULL) {
        hooks->notice_hide(notice);
    }
}

void p3a_ui_icon_show(p3a_icon_t icon, p3a_icon_place_t place)
{
    const p3a_ui_hooks_t *hooks = atomic_load(&s_hooks);
    if (hooks != NULL && hooks->icon_show != NULL) {
        hooks->icon_show(icon, place);
    }
}

void p3a_ui_content(p3a_ui_content_t *out, const char *title, int progress, const char *format, ...)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "%s", title ? title : "");
    out->progress = (int16_t)(progress < 0 ? -1 : progress > 100 ? 100 : progress);
    if (format != NULL) {
        va_list args;
        va_start(args, format);
        vsnprintf(out->text, sizeof(out->text), format, args);
        va_end(args);
    }
}
