// SPDX-License-Identifier: Apache-2.0
// What a command from Makapix Club does: the scheduler's calls and the
// settings, the same ones the HTTP API makes (D54). What each command asks
// for is makapix_command_map.c (pure); this file runs it.
//
// The handler runs on the Makapix task (source_makapix hands commands
// over there, never on the connection's own task). Everything it calls
// returns at once: what takes time is a job of the callee's.
//
// While the Wi-Fi co-processor's firmware is being written every command
// is dropped but the three that are acknowledged, as in 1.x: the link is
// about to go, and a playset change would start downloads into it.

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "makapix_command_map.h"
#include "net_internal.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_settings.h"
#include "play_scheduler.h"
#include "playset_store.h"
#include "psram_alloc.h"
#include "show_url.h"
#include "slave_ota.h"
#include "source_local.h"
#include "source_makapix.h"

static const char *TAG = "makapix_cmd";

static void run(const makapix_action_t *action, const char *command_id)
{
    esp_err_t err = ESP_OK;
    switch (action->kind) {
    case MAKAPIX_DO_NOTHING:
        return;  // a command this firmware does not know: the server may be newer
    case MAKAPIX_DO_REFUSE:
        ESP_LOGW(TAG, "refused: %s", action->text);
        if (action->wants_ack) {
            source_makapix_ack(command_id, "error", action->text);
        }
        return;
    case MAKAPIX_DO_UNSUPPORTED:
        source_makapix_ack(command_id, "unsupported", NULL);
        return;
    case MAKAPIX_DO_NEXT:
        err = play_scheduler_next();
        break;
    case MAKAPIX_DO_BACK:
        err = play_scheduler_back();
        break;
    case MAKAPIX_DO_PLAYSET:
        if (action->save) {
            const esp_err_t saved = playset_store_save(action->playset);
            if (saved != ESP_OK) {
                ESP_LOGW(TAG, "the playset could not be saved: %s", p3a_err_to_name(saved));
            }
        }
        err = play_scheduler_activate(action->playset);
        break;
    case MAKAPIX_DO_SHOW:
        err = play_scheduler_show_artwork(&action->record, NULL);
        break;
    case MAKAPIX_DO_SHOW_FILE: {
        p3a_record_t record;
        err = source_local_record(action->text, (uint32_t)time(NULL), &record);
        if (err == ESP_OK) {
            err = play_scheduler_show(&record);
        }
        break;
    }
    case MAKAPIX_DO_SHOW_URL:
        err = play_scheduler_show_url(action->text, NULL);
        break;
    case MAKAPIX_DO_BACKGROUND:
        err = p3a_settings_set_str(P3A_SET_BACKGROUND_COLOR, action->text);
        break;
    case MAKAPIX_DO_PAUSE:
        err = play_scheduler_set_paused(action->value != 0);
        break;
    case MAKAPIX_DO_BRIGHTNESS:
        err = p3a_settings_set_int(P3A_SET_BRIGHTNESS, action->value);
        break;
    case MAKAPIX_DO_ROTATION:
        err = p3a_settings_set_int(P3A_SET_ROTATION, action->value);
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", makapix_do_name(action->kind), p3a_err_to_name(err));
    }
    if (action->wants_ack) {
        source_makapix_ack(command_id, err == ESP_OK ? "ok" : "error", err == ESP_OK ? NULL : p3a_err_to_name(err));
    }
}

static void on_command(const char *type, const cJSON *payload, const char *command_id, void *ctx)
{
    (void)ctx;
    makapix_action_t *action = psram_calloc(1, sizeof(*action));
    if (action == NULL) {
        return;
    }
    makapix_command_map(type, payload, action);
    if (slave_ota_is_in_progress() && !action->wants_ack) {
        ESP_LOGW(TAG, "%s dropped: the Wi-Fi chip is being updated", type);
    } else {
        run(action, command_id);
    }
    makapix_action_free(action);
    free(action);
}

// The owner's page shows whether the player is paused.
static void on_player_changed(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    source_makapix_set_paused(play_scheduler_is_paused());
}

void net_makapix_commands_register(void)
{
    source_makapix_set_command_handler(on_command, NULL);
    p3a_event_subscribe(P3A_EVENT_PLAYER_CHANGED, on_player_changed, NULL);
}
