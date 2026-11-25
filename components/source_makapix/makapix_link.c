// SPDX-License-Identifier: Apache-2.0
// The link to Makapix Club: MQTT over mutual TLS, for commands, presence
// and views (D80). What is said and decided is makapix_link_logic.c (pure);
// this file is the client and its upkeep.
//
// Everything here but the client's own event handler runs on the Makapix
// task (makapix_register.c), once a second: makapix_link_tick(). The event
// handler, on the client's task, only copies what arrived and sets flags.
// 1.x ran every command on the client's task (a screen rotation included,
// audit M26) and killed its status task from the disconnect handler while
// it might be publishing (audit H13); here there is no status task, and a
// client is stopped and destroyed only by the task that made it.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "makapix_internal.h"
#include "makapix_link_logic.h"
#include "makapix_store.h"
#include "mqtt_client.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_mem.h"
#include "p3a_settings.h"
#include "psram_alloc.h"

static const char *TAG = "makapix";

#define STATUS_PERIOD_MS  30000  // the server calls a player offline after 3 minutes of silence
#define COMMAND_MAX_BYTES (16 * 1024)
#define COMMAND_QUEUE     8
#define MQTT_STACK_BYTES  16384  // the TLS handshake runs on the client's task; as 1.x

typedef struct {
    char *json;
    size_t len;
} inbound_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_commands;  // of inbound_t, filled by the client's task
static atomic_bool s_net_up, s_connected, s_dropped, s_state_dirty;
// Why the last attempt failed, set by the client's task before s_dropped.
static atomic_bool s_handshake_failed, s_broker_rejected;

// Owned by the Makapix task.
static esp_mqtt_client_handle_t s_client;
static makapix_pems_t s_pems;  // the client keeps pointers into these while it lives
static makapix_identity_t s_identity;
static char s_uri[96], s_client_id[48], s_will[96];
static char s_topics[MAKAPIX_TOPIC_COUNT][MAKAPIX_TOPIC_LEN];
static makapix_retry_t s_retry;
static int64_t s_next_attempt_ms, s_last_status_ms;
static bool s_invalid;
static source_makapix_command_fn s_handler;
static void *s_handler_ctx;

// Guarded by s_lock.
static struct {
    makapix_views_t views;
    char kind[32];
    char identifier[96];
    uint8_t channel_source;
    bool asked_for, random;
    bool paused;
} s_seen;

// A command split across MQTT messages, being put together on the client's task.
static struct {
    char *json;
    size_t total, got;
} s_partial;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t now_unix(void)
{
    return p3a_clock_is_set() ? (uint32_t)time(NULL) : 0;
}

// --- the client's task ---------------------------------------------------------------

static void take_command(const esp_mqtt_event_handle_t event)
{
    // The first piece carries the topic and the total length; the rest only data.
    if (event->current_data_offset == 0) {
        free(s_partial.json);
        s_partial.json = NULL;
        const bool ours = event->topic_len == (int)strlen(s_topics[MAKAPIX_TOPIC_COMMAND]) &&
                          memcmp(event->topic, s_topics[MAKAPIX_TOPIC_COMMAND], (size_t)event->topic_len) == 0;
        if (!ours) {
            if (event->topic_len == (int)strlen(s_topics[MAKAPIX_TOPIC_VIEW_ACK]) &&
                memcmp(event->topic, s_topics[MAKAPIX_TOPIC_VIEW_ACK], (size_t)event->topic_len) == 0) {
                ESP_LOGI(TAG, "view acknowledged: %.*s", event->data_len > 120 ? 120 : event->data_len, event->data);
            }
            return;
        }
        if (event->total_data_len <= 0 || event->total_data_len > COMMAND_MAX_BYTES) {
            ESP_LOGW(TAG, "a command of %d bytes was dropped", event->total_data_len);
            return;
        }
        s_partial.json = psram_malloc((size_t)event->total_data_len);
        s_partial.total = (size_t)event->total_data_len;
        s_partial.got = 0;
    }
    if (s_partial.json == NULL || event->data_len < 0 || (size_t)event->current_data_offset != s_partial.got ||
        s_partial.got + (size_t)event->data_len > s_partial.total) {
        free(s_partial.json);  // out of step: the message is lost, not misread
        s_partial.json = NULL;
        return;
    }
    memcpy(s_partial.json + s_partial.got, event->data, (size_t)event->data_len);
    s_partial.got += (size_t)event->data_len;
    if (s_partial.got == s_partial.total) {
        const inbound_t in = {.json = s_partial.json, .len = s_partial.total};
        s_partial.json = NULL;
        if (xQueueSend(s_commands, &in, 0) != pdTRUE) {
            ESP_LOGW(TAG, "commands are arriving faster than they are run: one dropped");
            free(in.json);
        }
    }
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t event = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        esp_mqtt_client_subscribe(event->client, s_topics[MAKAPIX_TOPIC_COMMAND], 1);
        esp_mqtt_client_subscribe(event->client, s_topics[MAKAPIX_TOPIC_VIEW_ACK], 1);
        atomic_store(&s_connected, true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s_connected, false);
        atomic_store(&s_dropped, true);
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle != NULL && event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            // A handshake that fails says the two sides do not accept each
            // other's certificates; flags on the broker's say it is the
            // broker the player rejects.
            if (event->error_handle->esp_tls_last_esp_err == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED) {
                atomic_store(&s_handshake_failed, true);
            }
            if (event->error_handle->esp_tls_cert_verify_flags != 0) {
                atomic_store(&s_handshake_failed, true);
                atomic_store(&s_broker_rejected, true);
            }
        }
        break;
    case MQTT_EVENT_DATA:
        take_command(event);
        break;
    default:
        break;
    }
}

// --- the Makapix task ----------------------------------------------------------------

static void publish(makapix_topic_t topic, const char *json, bool retain)
{
    if (s_client != NULL && atomic_load(&s_connected)) {
        // Queued for the client's task: this one never waits on the network.
        if (esp_mqtt_client_enqueue(s_client, s_topics[topic], json, 0, 1, retain ? 1 : 0, true) < 0) {
            ESP_LOGW(TAG, "a message for %s could not be queued", s_topics[topic] + 15 + 36);
        }
    }
}

static void publish_status(void)
{
    char json[256];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t post = s_seen.views.covered ? 0 : s_seen.views.post_id;
    xSemaphoreGive(s_lock);
    if (makapix_msg_status(s_identity.player_key, true, post, esp_app_get_description()->version, now_unix(), json,
                           sizeof(json)) == ESP_OK) {
        publish(MAKAPIX_TOPIC_STATUS, json, false);
    }
    s_last_status_ms = now_ms();
}

static void publish_state(void)
{
    static p3a_settings_t settings;  // large; only this task uses it
    char json[96];
    p3a_settings_get(&settings);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool paused = s_seen.paused;
    xSemaphoreGive(s_lock);
    if (makapix_msg_state(paused, (int)settings.brightness, (int)settings.rotation, json, sizeof(json)) == ESP_OK) {
        publish(MAKAPIX_TOPIC_STATE, json, true);
    }
}

static void publish_view_if_due(void)
{
    static p3a_settings_t settings;
    char kind[32], identifier[96], json[512];
    makapix_view_t view = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool due = makapix_views_due(&s_seen.views, now_ms());
    view.post_id = s_seen.views.post_id;
    view.asked_for = s_seen.asked_for;
    view.random = s_seen.random;
    const uint8_t channel_source = s_seen.channel_source;
    memcpy(kind, s_seen.kind, sizeof(kind));
    memcpy(identifier, s_seen.identifier, sizeof(identifier));
    xSemaphoreGive(s_lock);
    if (!due) {
        return;
    }
    // The channel as the server names it. A Makapix channel's kind is that
    // name; an artwork that came up elsewhere says where.
    static const char *const others[] = {"unknown", "sdcard", "", "giphy", "klipy", "institution", "pinned"};
    if (view.asked_for) {
        view.channel = "artwork";
    } else if (channel_source == P3A_SOURCE_MAKAPIX) {
        view.channel = kind;
        if (strcmp(kind, "by_user") == 0 || strcmp(kind, "reactions") == 0) {
            view.user_sqid = identifier;
        } else if (strcmp(kind, "hashtag") == 0) {
            view.hashtag = identifier;
        }
    } else {
        view.channel = channel_source < sizeof(others) / sizeof(others[0]) ? others[channel_source] : "unknown";
    }
    p3a_settings_get(&settings);
    view.want_ack = settings.makapix_view_ack;
    if (makapix_msg_view(s_identity.player_key, &view, now_unix(), json, sizeof(json)) == ESP_OK) {
        publish(MAKAPIX_TOPIC_VIEW, json, false);  // not connected: lost, as in 1.x; a view is not worth a queue
    }
}

static void run_commands(void)
{
    inbound_t in;
    while (xQueueReceive(s_commands, &in, 0) == pdTRUE) {
        makapix_command_t command;
        const esp_err_t err = makapix_parse_command(in.json, in.len, &command);
        free(in.json);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "a command could not be read: %s", p3a_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "command: %s", command.type);
        if (s_handler != NULL) {
            s_handler(command.type, command.payload, command.id, s_handler_ctx);
        }
        cJSON_Delete(command.payload);
    }
}

static void client_destroy(void)
{
    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    atomic_store(&s_connected, false);
    makapix_pems_free(&s_pems);
}

static esp_err_t client_start(void)
{
    esp_err_t err = makapix_store_identity(&s_identity);
    if (err == ESP_OK) {
        err = makapix_store_pems(&s_pems);
    }
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < MAKAPIX_TOPIC_COUNT; i++) {
        makapix_topic((makapix_topic_t)i, s_identity.player_key, s_topics[i]);
    }
    snprintf(s_uri, sizeof(s_uri), "mqtts://%s:%u", s_identity.mqtt_host, (unsigned)s_identity.mqtt_port);
    snprintf(s_client_id, sizeof(s_client_id), "p3a-%s", s_identity.player_key);
    makapix_msg_status(s_identity.player_key, false, 0, "", 0, s_will, sizeof(s_will));
    const esp_mqtt_client_config_t config = {
        .broker = {.address = {.uri = s_uri}, .verification = {.certificate = s_pems.ca}},
        .credentials = {.client_id = s_client_id,
                        .username = s_identity.player_key,
                        .authentication = {.certificate = s_pems.cert, .key = s_pems.key}},
        .session = {.keepalive = 60,
                    .last_will = {.topic = s_topics[MAKAPIX_TOPIC_STATUS], .msg = s_will, .qos = 1, .retain = 0}},
        // Reconnecting is this file's: it must be able to stop, and to renew
        // the certificates in between.
        .network = {.timeout_ms = 10000, .disable_auto_reconnect = true},
        .task = {.stack_size = MQTT_STACK_BYTES},
        .buffer = {.size = 4096},
    };
    atomic_store(&s_dropped, false);
    atomic_store(&s_handshake_failed, false);
    atomic_store(&s_broker_rejected, false);
    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL) {
        makapix_pems_free(&s_pems);
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt, NULL);
    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        client_destroy();
    }
    return err;
}

void makapix_link_tick(void)
{
    static bool was_connected;
    run_commands();  // whatever the link's state: they were taken when it was up
    if (!makapix_store_registered()) {
        if (s_client != NULL) {
            client_destroy();
        }
        was_connected = false;
        s_invalid = false;
        makapix_retry_reset(&s_retry);
        s_next_attempt_ms = 0;
        return;
    }
    const int64_t now = now_ms();
    const bool connected = atomic_load(&s_connected);
    if (connected && !was_connected) {
        ESP_LOGI(TAG, "connected to Makapix Club");
        makapix_retry_reset(&s_retry);
        makapix_renew_kick();  // a connection is a good moment to look at the certificate's dates
        char json[256];
        // Capabilities before state: the server keeps of the state only
        // what the capabilities declared.
        if (makapix_msg_capabilities(esp_app_get_description()->version, json, sizeof(json)) == ESP_OK) {
            publish(MAKAPIX_TOPIC_CAPABILITIES, json, true);
        }
        atomic_store(&s_state_dirty, false);
        publish_state();
        publish_status();
        p3a_event_emit_i32(P3A_EVENT_MAKAPIX_CHANGED, 0);
    }
    was_connected = connected;

    if (s_client != NULL && (atomic_exchange(&s_dropped, false) || !atomic_load(&s_net_up))) {
        // The attempt failed, the connection was lost, or the network went.
        const bool handshake = atomic_load(&s_handshake_failed), broker = atomic_load(&s_broker_rejected);
        client_destroy();
        was_connected = false;
        makapix_retry_verdict_t verdict = makapix_retry_failed(&s_retry, handshake, broker);
        if (verdict == MAKAPIX_RETRY_RENEW) {
            bool counted = true;
            const bool renewed = makapix_renew_now(&counted);
            verdict = makapix_retry_renewed(&s_retry, renewed, counted);
        }
        if (verdict == MAKAPIX_RETRY_INVALID) {
            ESP_LOGE(TAG, "Makapix Club does not accept this player's certificates: the registration is taken to be invalid");
            s_invalid = true;
        } else {
            const uint32_t wait = makapix_retry_delay(&s_retry, esp_random());
            s_next_attempt_ms = now + wait;
            ESP_LOGW(TAG, "not connected to Makapix Club%s; trying again in %u s",
                     handshake ? " (the TLS handshake failed)" : "", (unsigned)(wait / 1000));
        }
        p3a_event_emit_i32(P3A_EVENT_MAKAPIX_CHANGED, 0);
        return;
    }

    if (s_client == NULL) {
        // A new session is optional work: not while memory is short.
        if (!s_invalid && atomic_load(&s_net_up) && now >= s_next_attempt_ms && p3a_mem_level() == P3A_MEM_OK) {
            const esp_err_t err = client_start();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "the connection could not be started: %s", p3a_err_to_name(err));
                s_next_attempt_ms = now + makapix_retry_delay(&s_retry, esp_random());
            }
        }
        return;
    }
    if (!connected) {
        return;  // the attempt is under way
    }
    if (atomic_exchange(&s_state_dirty, false)) {
        publish_state();
    }
    if (now - s_last_status_ms >= STATUS_PERIOD_MS) {
        publish_status();
    }
    publish_view_if_due();
}

// --- what others tell the link -------------------------------------------------------

void makapix_link_shown(const p3a_record_t *record, const p3a_shown_t *how)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = now_ms();
    if (record == NULL) {
        // Covered, paused or replaced: the scheduler does not say which, and
        // "the same artwork again" must carry on counting. So it is a cover;
        // another artwork arriving resets the count by itself.
        makapix_views_cover(&s_seen.views, true, now);
    } else {
        makapix_views_show(&s_seen.views, record->id32, now);
        makapix_views_cover(&s_seen.views, false, now);
        s_seen.channel_source = how->channel_source;
        s_seen.asked_for = how->asked_for;
        s_seen.random = how->random;
        snprintf(s_seen.kind, sizeof(s_seen.kind), "%s", how->kind != NULL ? how->kind : "");
        snprintf(s_seen.identifier, sizeof(s_seen.identifier), "%s", how->identifier != NULL ? how->identifier : "");
    }
    xSemaphoreGive(s_lock);
}

void source_makapix_set_paused(bool paused)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_seen.paused != paused;
    s_seen.paused = paused;
    xSemaphoreGive(s_lock);
    if (changed) {
        atomic_store(&s_state_dirty, true);
    }
}

esp_err_t source_makapix_deliver(const char *json, size_t len)
{
    if (s_commands == NULL || json == NULL || len == 0 || len > COMMAND_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    inbound_t in = {.json = psram_malloc(len), .len = len};
    if (in.json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(in.json, json, len);
    if (xQueueSend(s_commands, &in, 0) != pdTRUE) {
        free(in.json);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void source_makapix_set_command_handler(source_makapix_command_fn handler, void *ctx)
{
    s_handler_ctx = ctx;
    s_handler = handler;
}

void source_makapix_ack(const char *command_id, const char *status, const char *error)
{
    char json[256];
    if (command_id == NULL || command_id[0] == '\0') {
        return;  // a command with no id has nobody waiting
    }
    if (makapix_msg_ack(command_id, status, error, json, sizeof(json)) == ESP_OK) {
        publish(MAKAPIX_TOPIC_ACK, json, false);
    }
}

void makapix_link_status(source_makapix_status_t *out)
{
    if (s_lock == NULL) {
        return;
    }
    out->link = !out->registered           ? SOURCE_MAKAPIX_LINK_NONE
                : s_invalid                ? SOURCE_MAKAPIX_LINK_INVALID
                : atomic_load(&s_connected) ? SOURCE_MAKAPIX_LINK_CONNECTED
                : s_client != NULL          ? SOURCE_MAKAPIX_LINK_CONNECTING
                                            : SOURCE_MAKAPIX_LINK_WAITING;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->seen_post_id = s_seen.views.covered ? 0 : s_seen.views.post_id;
    xSemaphoreGive(s_lock);
}

bool makapix_link_is_invalid(void)
{
    return s_invalid;
}

// A renewal that succeeded while the registration was taken to be invalid
// makes it worth trying again (makapix_renew.c).
void makapix_link_certificates_changed(void)
{
    s_invalid = false;
    s_next_attempt_ms = 0;
}

static void on_net(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    atomic_store(&s_net_up, event->type == P3A_EVENT_NET_UP);
}

static void on_setting(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->payload.i32 == P3A_SET_BRIGHTNESS || event->payload.i32 == P3A_SET_ROTATION) {
        atomic_store(&s_state_dirty, true);
    }
}

esp_err_t makapix_link_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(COMMAND_QUEUE, sizeof(inbound_t));
    if (s_lock == NULL || s_commands == NULL) {
        return ESP_ERR_NO_MEM;
    }
    makapix_views_init(&s_seen.views);
    makapix_retry_reset(&s_retry);
    p3a_event_subscribe(P3A_EVENT_NET_UP, on_net, NULL);
    p3a_event_subscribe(P3A_EVENT_NET_DOWN, on_net, NULL);
    p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_setting, NULL);
    return ESP_OK;
}
