// SPDX-License-Identifier: Apache-2.0
// The built-in /api/v2 endpoints: status, settings, jobs, and the system
// actions. Endpoints that belong to a feature (playback, channels, pins,
// updates) are added with that feature.
//
//   GET    /api/v2/status                 what the device is and how it is doing
//   GET    /api/v2/settings               every setting; secrets masked
//   PATCH  /api/v2/settings               change settings, all or nothing
//   GET    /api/v2/settings/schema        the settings schema, as written (not enveloped)
//   GET    /api/v2/secrets/museum-keys    the two keys the browser needs (decision D33)
//   GET    /api/v2/wifi                   the stored network and whether the device is in setup
//   PUT    /api/v2/wifi                   store a network (and optionally the device name), restart
//   DELETE /api/v2/wifi                   forget the network, restart into setup
//   GET    /api/v2/jobs/{id}              a job's state and progress
//   POST   /api/v2/system/reboot          restart
//   POST   /api/v2/system/factory-reset   erase settings and credentials, restart
//   GET    /api/v2/system/coredump        the last crash dump, raw (decision D34)
//   DELETE /api/v2/system/coredump        erase it

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "http_envelope.h"
#include "net_internal.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_mem.h"
#include "p3a_recovery.h"
#include "p3a_settings.h"
#include "p3a_storage.h"
#include "psram_alloc.h"
#include "sd_health.h"
#include "sd_path.h"
#include "sd_repair.h"
#include "slave_ota.h"

static const char *TAG = "api";

// --- the crash dump, summarised once ---------------------------------------------
//
// Read once at boot, never in a request: the status call is polled, and the
// summary is expensive.
//
// It must run on a task whose stack is in internal RAM. The IDF reads the
// summary by memory-mapping the dump partition, mapping flash switches the
// cache off for a moment, and a task whose stack lives in PSRAM cannot run
// with the cache off: the IDF asserts. Every p3a task class except RENDER has
// a PSRAM stack, the job runners included, so this is called from app_main's
// task and refuses to run anywhere else. (As a job it crashed on every boot
// that had a dump to read.)

typedef struct {
    bool known;    // looked for at boot
    bool present;
    size_t bytes;
    char task[16];
    uint32_t pc;
    char reason[96];
} dump_info_t;

static dump_info_t s_dump;

static void inspect_dump(void)
{
    dump_info_t info = {.known = true};
    if (esp_ptr_external_ram(&info)) {
        ESP_LOGE(TAG, "the crash dump cannot be read from a task with a PSRAM stack");
        return;
    }
    size_t addr = 0;
    if (esp_core_dump_image_check() == ESP_OK && esp_core_dump_image_get(&addr, &info.bytes) == ESP_OK) {
        info.present = true;
        // The summary carries a stack dump: too large for a task stack.
        esp_core_dump_summary_t *summary = psram_calloc(1, sizeof(*summary));
        if (summary != NULL && esp_core_dump_get_summary(summary) == ESP_OK) {
            strlcpy(info.task, summary->exc_task, sizeof(info.task));
            info.pc = summary->exc_pc;
        }
        free(summary);
        if (esp_core_dump_get_panic_reason(info.reason, sizeof(info.reason)) != ESP_OK) {
            info.reason[0] = 0;
        }
        ESP_LOGW(TAG, "a crash dump is stored (%u bytes, task %s)", (unsigned)info.bytes, info.task);
    }
    s_dump = info;
}

// --- status ---------------------------------------------------------------------

static const char *health_name(void)
{
    switch (sd_health_get_state()) {
    case SD_HEALTH_OK:
        return "ok";
    case SD_HEALTH_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static void get_status(p3a_http_request_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "version", app->version);
    cJSON_AddStringToObject(data, "idf", app->idf_ver);
    cJSON_AddStringToObject(data, "built", app->date);
    cJSON_AddNumberToObject(data, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(data, "reboot_needed", p3a_settings_reboot_needed());

    p3a_net_status_t net;
    p3a_net_status(&net);
    cJSON *network = cJSON_AddObjectToObject(data, "network");
    cJSON_AddStringToObject(network, "state", p3a_net_state_name(net.state));
    cJSON_AddStringToObject(network, "ssid", net.ssid);
    cJSON_AddStringToObject(network, "ip", net.ip);
    cJSON_AddStringToObject(network, "hostname", net.hostname);
    cJSON_AddNumberToObject(network, "rssi", net.rssi);
    cJSON_AddNumberToObject(network, "drops", net.drops);
    cJSON_AddNumberToObject(network, "reinits", net.reinits);

    cJSON *clock = cJSON_AddObjectToObject(data, "time");
    char now[P3A_CLOCK_ISO8601_LEN];
    bool have_time = p3a_clock_now_iso8601(now, sizeof(now)) == ESP_OK;
    cJSON_AddBoolToObject(clock, "set", have_time);
    if (have_time) {
        cJSON_AddStringToObject(clock, "now", now);
    }

    const p3a_storage_space_t space = p3a_storage_space();  // cached; never measured here
    cJSON *storage = cJSON_AddObjectToObject(data, "storage");
    cJSON_AddStringToObject(storage, "health", health_name());
    cJSON_AddStringToObject(storage, "root", sd_path_get_root());
    cJSON_AddNumberToObject(storage, "repairs", sd_repair_count_total());
    if (space.valid) {
        cJSON_AddNumberToObject(storage, "total_mb", (double)(space.total_bytes >> 20));
        cJSON_AddNumberToObject(storage, "free_mb", (double)(space.free_bytes >> 20));
    }

    const p3a_mem_sample_t mem = p3a_mem_sample();
    cJSON *memory = cJSON_AddObjectToObject(data, "memory");
    cJSON_AddStringToObject(memory, "level", p3a_mem_level_name(mem.level));
    cJSON_AddNumberToObject(memory, "free", mem.free_bytes);
    cJSON_AddNumberToObject(memory, "largest", mem.largest_block);
    cJSON_AddNumberToObject(memory, "min_free", mem.min_free_bytes);
    cJSON_AddNumberToObject(memory, "total", mem.total_bytes);
    // The numbers above are the internal pool, the one that runs short first.
    // The external RAM holds the indices, the decoders and the frames.
    cJSON *psram = cJSON_AddObjectToObject(memory, "psram");
    cJSON_AddNumberToObject(psram, "free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(psram, "largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(psram, "min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(psram, "total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    // What the board is. The co-processor's firmware is flashed by this one at boot.
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    char text[24];
    cJSON *chip = cJSON_AddObjectToObject(data, "chip");
    cJSON_AddStringToObject(chip, "model", CONFIG_IDF_TARGET);
    snprintf(text, sizeof(text), "v%d.%d", chip_info.revision / 100, chip_info.revision % 100);
    cJSON_AddStringToObject(chip, "revision", text);
    cJSON *coprocessor = cJSON_AddObjectToObject(data, "coprocessor");
    uint32_t major = 0, minor = 0, patch = 0;
    if (slave_ota_get_running_version(&major, &minor, &patch) == ESP_OK) {
        snprintf(text, sizeof(text), "%u.%u.%u", (unsigned)major, (unsigned)minor, (unsigned)patch);
        cJSON_AddStringToObject(coprocessor, "running", text);  // absent: it did not answer
    }
    if (slave_ota_get_embedded_version(&major, &minor, &patch) == ESP_OK) {
        snprintf(text, sizeof(text), "%u.%u.%u", (unsigned)major, (unsigned)minor, (unsigned)patch);
        cJSON_AddStringToObject(coprocessor, "embedded", text);  // what this firmware carries for it
    }
    cJSON_AddBoolToObject(coprocessor, "locked", slave_ota_is_version_locked());

    // Restarts the device gave itself as a remedy, ever. Zero is the healthy answer.
    cJSON *recovery = cJSON_AddObjectToObject(data, "recovery_restarts");
    for (int kind = 0; kind < P3A_RECOVERY_KIND_COUNT_; kind++) {
        cJSON_AddNumberToObject(recovery, p3a_recovery_name((p3a_recovery_kind_t)kind),
                                p3a_recovery_total((p3a_recovery_kind_t)kind));
    }

    const dump_info_t dump = s_dump;
    cJSON *coredump = cJSON_AddObjectToObject(data, "coredump");
    cJSON_AddBoolToObject(coredump, "checked", dump.known);
    cJSON_AddBoolToObject(coredump, "present", dump.present);
    if (dump.present) {
        char pc[12];
        snprintf(pc, sizeof(pc), "0x%08x", (unsigned)dump.pc);
        cJSON_AddNumberToObject(coredump, "bytes", (double)dump.bytes);
        cJSON_AddStringToObject(coredump, "task", dump.task);
        cJSON_AddStringToObject(coredump, "pc", pc);
        cJSON_AddStringToObject(coredump, "reason", dump.reason);
    }
    p3a_http_ok(req, data);
}

// --- settings -------------------------------------------------------------------

static void get_settings(p3a_http_request_t *req)
{
    cJSON *data = p3a_settings_json();
    if (data == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading settings");
        return;
    }
    p3a_http_ok(req, data);
}

static void patch_settings(p3a_http_request_t *req)
{
    uint64_t changed = 0;
    char why[96] = "";
    esp_err_t err = p3a_settings_set_json(p3a_http_body(req), &changed, why, sizeof(why));
    if (err == ESP_ERR_NOT_FOUND) {
        p3a_http_fail(req, 400, "UNKNOWN_SETTING", "%s", why);
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        p3a_http_fail(req, 400, "INVALID_SETTING", "%s", why);
        return;
    }
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "saving settings");
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(data, "changed");
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        if (changed & (1ULL << i)) {
            cJSON_AddItemToArray(list, cJSON_CreateString(p3a_setting_defs[i].key));
        }
    }
    cJSON_AddBoolToObject(data, "reboot_needed", p3a_settings_reboot_needed());
    p3a_http_ok(req, data);
}

// The schema is a document, served as it was written: the web UI draws the
// settings page from it. It changes only with the firmware.
static void get_settings_schema(p3a_http_request_t *req)
{
    static char etag[P3A_HTTP_ETAG_LEN];
    size_t len = 0;
    const char *text = p3a_settings_schema(&len);
    if (etag[0] == '\0') {
        p3a_http_etag(text, len, etag);
    }
    httpd_req_t *raw = p3a_http_raw(req);
    char asked[64];
    httpd_resp_set_hdr(raw, "ETag", etag);
    httpd_resp_set_hdr(raw, "Cache-Control", "no-cache");
    if (httpd_req_get_hdr_value_str(raw, "If-None-Match", asked, sizeof(asked)) == ESP_OK &&
        p3a_http_etag_matches(asked, etag)) {
        httpd_resp_set_status(raw, p3a_http_status_line(304));
        httpd_resp_send(raw, NULL, 0);
    } else {
        httpd_resp_set_type(raw, "application/json");
        httpd_resp_send(raw, text, (ssize_t)len);
    }
    p3a_http_mark_answered(req);
}

// The museum browse page calls Harvard and the Smithsonian from the browser
// and needs the user's keys for it. These two, and only here; the Giphy and
// Klipy keys never leave the device.
static void get_museum_keys(p3a_http_request_t *req)
{
    static p3a_settings_t settings;  // the server has one task, so one copy is enough
    p3a_settings_get(&settings);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "harvard_api_key", settings.harvard_api_key);
    cJSON_AddStringToObject(data, "smithsonian_api_key", settings.smithsonian_api_key);
    memset(&settings, 0, sizeof(settings));
    p3a_http_no_store(req);
    p3a_http_ok(req, data);
}

// --- jobs -----------------------------------------------------------------------

static void get_job(p3a_http_request_t *req)
{
    char text[12];
    char *end = NULL;
    unsigned long id = p3a_http_param(req, 0, text, sizeof(text)) ? strtoul(text, &end, 10) : 0;
    p3a_job_info_t info;
    if (id == 0 || end == NULL || *end != '\0' || !p3a_job_get((p3a_job_id_t)id, &info)) {
        p3a_http_fail(req, 404, "NOT_FOUND", "no such job (finished jobs are remembered only for a while)");
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", info.id);
    cJSON_AddStringToObject(data, "name", info.name);
    cJSON_AddStringToObject(data, "state", p3a_job_state_name(info.state));
    cJSON_AddNumberToObject(data, "progress", info.progress);
    if (info.state == P3A_JOB_DONE) {
        cJSON_AddBoolToObject(data, "succeeded", info.result == ESP_OK);
        if (info.result != ESP_OK) {
            cJSON_AddStringToObject(data, "error", p3a_err_to_name(info.result));
        }
    }
    p3a_http_ok(req, data);
}

// --- system ---------------------------------------------------------------------

static void restart_now(void *arg)
{
    (void)arg;
    esp_restart();
}

// Restarts shortly, after the answer has had time to leave.
static esp_err_t restart_soon(void)
{
    static esp_timer_handle_t timer;
    if (timer == NULL) {
        const esp_timer_create_args_t args = {.callback = restart_now, .name = "api_restart"};
        esp_err_t err = esp_timer_create(&args, &timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_err_t err = esp_timer_start_once(timer, 1500 * 1000);
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;  // already counting down
}

static void post_reboot(p3a_http_request_t *req)
{
    esp_err_t err = restart_soon();
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "scheduling the restart");
        return;
    }
    ESP_LOGW(TAG, "restart requested over HTTP");
    p3a_http_ok(req, NULL);
}

static esp_err_t factory_reset_job(void *arg)
{
    (void)arg;
    esp_err_t err = p3a_settings_factory_reset();
    if (err != ESP_OK) {
        return err;
    }
    return restart_soon();
}

// Erasing flash takes a while, so it is a job. The body must say so on
// purpose: {"confirm":"factory-reset"}.
static void post_factory_reset(p3a_http_request_t *req)
{
    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "confirm");
    if (!cJSON_IsString(confirm) || strcmp(confirm->valuestring, "factory-reset") != 0) {
        p3a_http_fail(req, 400, "NOT_CONFIRMED", "send {\"confirm\":\"factory-reset\"} to erase this device's settings");
        return;
    }
    p3a_job_id_t job = P3A_JOB_ID_NONE;
    esp_err_t err = p3a_job_submit(P3A_JOBS_IO, "factory_reset", factory_reset_job, NULL, &job);
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "starting the factory reset");
        return;
    }
    ESP_LOGW(TAG, "factory reset requested over HTTP");
    p3a_http_accepted(req, job);
}

// Streams the dump partition's image. It is device memory: it may hold the
// Wi-Fi password and API keys, which is why the web UI warns before offering it.
static void get_coredump(p3a_http_request_t *req)
{
    httpd_req_t *raw = p3a_http_raw(req);
    size_t addr = 0, size = 0;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!s_dump.present || part == NULL || esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0 ||
        addr < part->address || addr - part->address + size > part->size) {
        p3a_http_fail(req, 404, "NOT_FOUND", "no crash dump is stored");
        return;
    }
    const size_t chunk = 4096;
    char *buf = psram_malloc(chunk);
    if (buf == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading the crash dump");
        return;
    }
    httpd_resp_set_type(raw, "application/octet-stream");
    httpd_resp_set_hdr(raw, "Content-Disposition", "attachment; filename=\"p3a-coredump.elf\"");
    httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
    size_t offset = addr - part->address;
    esp_err_t err = ESP_OK;
    for (size_t sent = 0; sent < size && err == ESP_OK;) {
        size_t n = size - sent < chunk ? size - sent : chunk;
        err = esp_partition_read(part, offset + sent, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(raw, buf, (ssize_t)n);
        }
        sent += n;
    }
    free(buf);
    // The headers have gone, so a failure can only cut the transfer short;
    // the client sees a truncated chunked body, which it reports as an error.
    httpd_resp_send_chunk(raw, NULL, 0);
    p3a_http_mark_answered(req);
}

static void delete_coredump(p3a_http_request_t *req)
{
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "erasing the crash dump");
        return;
    }
    s_dump = (dump_info_t){.known = true};
    p3a_http_ok(req, NULL);
}

// --- Wi-Fi ----------------------------------------------------------------------

static void get_wifi(p3a_http_request_t *req)
{
    p3a_net_status_t net;
    p3a_net_status(&net);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", p3a_net_state_name(net.state));
    cJSON_AddBoolToObject(data, "setup", net.state == P3A_NET_SETUP);
    cJSON_AddBoolToObject(data, "has_credentials", net.has_credentials);
    cJSON_AddStringToObject(data, "ssid", net.ssid);
    cJSON_AddStringToObject(data, "hostname", net.hostname);
    p3a_http_ok(req, data);
}

// {"ssid": "...", "password": "...", "device_name": "..."}; the last two are
// optional. Everything is checked before anything is stored.
static void put_wifi(p3a_http_request_t *req)
{
    const cJSON *body = p3a_http_body(req);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(body, "password");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(body, "device_name");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' || strlen(ssid->valuestring) > 32) {
        p3a_http_fail(req, 400, "INVALID_SSID", "the network name must be 1 to 32 characters");
        return;
    }
    if (password != NULL && (!cJSON_IsString(password) || strlen(password->valuestring) > 64)) {
        p3a_http_fail(req, 400, "INVALID_PASSWORD", "the password must be at most 64 characters");
        return;
    }
    if (name != NULL) {
        if (!cJSON_IsString(name)) {
            p3a_http_fail(req, 400, "INVALID_SETTING", "device_name must be a string");
            return;
        }
        esp_err_t err = p3a_settings_set_str(P3A_SET_DEVICE_NAME, name->valuestring);
        if (err == ESP_ERR_INVALID_ARG) {
            p3a_http_fail(req, 400, "INVALID_SETTING",
                          "the device name may hold up to 16 of a-z, 0-9 and '-', not starting or ending with '-'");
            return;
        }
        if (err != ESP_OK) {
            p3a_http_fail_err(req, err, "saving the device name");
            return;
        }
    }
    esp_err_t err = p3a_net_set_credentials(ssid->valuestring, password != NULL ? password->valuestring : "");
    if (err == ESP_OK) {
        err = restart_soon();
    }
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "saving the network");
        return;
    }
    // Where to find the device once it has restarted and joined.
    static p3a_settings_t settings;
    p3a_settings_get(&settings);
    char hostname[24] = "p3a";
    if (settings.device_name[0] != '\0') {
        snprintf(hostname, sizeof(hostname), "p3a-%s", settings.device_name);
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "hostname", hostname);
    cJSON_AddBoolToObject(data, "restarting", true);
    p3a_http_ok(req, data);
}

static void delete_wifi(p3a_http_request_t *req)
{
    esp_err_t err = p3a_net_erase_credentials();
    if (err == ESP_OK) {
        err = restart_soon();
    }
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "forgetting the network");
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "restarting", true);
    p3a_http_ok(req, data);
}

// --- the table ------------------------------------------------------------------

// What a phone needs to keep the page on its home screen as an app. Named
// after the device, so that two of them can be told apart there.
static void get_manifest(p3a_http_request_t *req)
{
    httpd_req_t *raw = p3a_http_raw(req);
    static p3a_settings_t settings;
    p3a_settings_get(&settings);
    char name[24] = "p3a";
    if (settings.device_name[0] != '\0') {
        snprintf(name, sizeof(name), "p3a-%s", settings.device_name);
    }
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "name", name);
    cJSON_AddStringToObject(doc, "short_name", name);
    cJSON_AddStringToObject(doc, "start_url", "/");
    cJSON_AddStringToObject(doc, "scope", "/");
    cJSON_AddStringToObject(doc, "display", "standalone");
    cJSON_AddStringToObject(doc, "background_color", "#0E0A1A");
    cJSON_AddStringToObject(doc, "theme_color", "#0E0A1A");
    cJSON *icons = cJSON_AddArrayToObject(doc, "icons");
    static const struct {
        const char *src, *sizes, *purpose;
    } list[] = {{"/static/icon-192.png", "192x192", NULL},
                {"/static/icon-512.png", "512x512", NULL},
                {"/static/icon-192-maskable.png", "192x192", "maskable"},
                {"/static/icon-512-maskable.png", "512x512", "maskable"}};
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        cJSON *icon = cJSON_CreateObject();
        cJSON_AddStringToObject(icon, "src", list[i].src);
        cJSON_AddStringToObject(icon, "sizes", list[i].sizes);
        cJSON_AddStringToObject(icon, "type", "image/png");
        if (list[i].purpose != NULL) {
            cJSON_AddStringToObject(icon, "purpose", list[i].purpose);
        }
        cJSON_AddItemToArray(icons, icon);
    }
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (text == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "writing the manifest");
        return;
    }
    httpd_resp_set_type(raw, "application/manifest+json");
    httpd_resp_set_hdr(raw, "Cache-Control", "no-cache");
    httpd_resp_sendstr(raw, text);
    cJSON_free(text);
    p3a_http_mark_answered(req);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, P3A_HTTP_RAW, "/manifest.webmanifest", get_manifest},
    {P3A_HTTP_GET, 0, "/api/v2/status", get_status},
    {P3A_HTTP_GET, 0, "/api/v2/settings", get_settings},
    {P3A_HTTP_PATCH, P3A_HTTP_BODY_JSON, "/api/v2/settings", patch_settings},
    {P3A_HTTP_GET, P3A_HTTP_RAW, "/api/v2/settings/schema", get_settings_schema},
    {P3A_HTTP_GET, 0, "/api/v2/secrets/museum-keys", get_museum_keys},
    {P3A_HTTP_GET, 0, "/api/v2/wifi", get_wifi},
    {P3A_HTTP_PUT, P3A_HTTP_BODY_JSON, "/api/v2/wifi", put_wifi},
    {P3A_HTTP_DELETE, 0, "/api/v2/wifi", delete_wifi},
    {P3A_HTTP_GET, 0, "/api/v2/jobs/{id}", get_job},
    {P3A_HTTP_POST, 0, "/api/v2/system/reboot", post_reboot},
    {P3A_HTTP_POST, P3A_HTTP_BODY_JSON, "/api/v2/system/factory-reset", post_factory_reset},
    {P3A_HTTP_GET, P3A_HTTP_RAW, "/api/v2/system/coredump", get_coredump},
    {P3A_HTTP_DELETE, 0, "/api/v2/system/coredump", delete_coredump},
};

void net_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
    net_player_api_register();
    net_ui_api_register();
    net_sources_api_register();
    net_pins_api_register();
    net_makapix_api_register();
    net_media_api_register();
    net_makapix_commands_register();
#if CONFIG_P3A_FRAME_TRACE
    net_trace_api_register();
#endif
    inspect_dump();
}
