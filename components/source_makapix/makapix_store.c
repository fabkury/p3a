// SPDX-License-Identifier: Apache-2.0
// A Makapix registration in NVS. See makapix_store.h.

#include "makapix_store.h"

#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "p3a_err.h"
#include "psram_alloc.h"

#define NS          "makapix"
#define K_KEY       "player_key"
#define K_HOST      "mqtt_host"
#define K_PORT      "mqtt_port"
#define K_CA        "ca_cert"
#define K_CERT      "client_cert"
#define K_CLIENTKEY "client_key"
#define K_TOKEN     "api_token"

void makapix_pems_free(makapix_pems_t *pems)
{
    if (pems == NULL) {
        return;
    }
    // The private key is not left lying in freed memory.
    if (pems->key != NULL) {
        memset(pems->key, 0, strlen(pems->key));
    }
    free(pems->ca);
    free(pems->cert);
    free(pems->key);
    memset(pems, 0, sizeof(*pems));
}

static bool has_blob(nvs_handle_t nvs, const char *key)
{
    size_t len = 0;
    return nvs_get_blob(nvs, key, NULL, &len) == ESP_OK && len > 1 && len <= MAKAPIX_PEM_MAX;
}

static bool has_str(nvs_handle_t nvs, const char *key)
{
    size_t len = 0;
    return nvs_get_str(nvs, key, NULL, &len) == ESP_OK && len > 1;
}

bool makapix_store_registered(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    uint16_t port = 0;
    const bool all = has_str(nvs, K_KEY) && has_str(nvs, K_HOST) && nvs_get_u16(nvs, K_PORT, &port) == ESP_OK &&
                     port != 0 && has_blob(nvs, K_CA) && has_blob(nvs, K_CERT) && has_blob(nvs, K_CLIENTKEY);
    nvs_close(nvs);
    return all;
}

esp_err_t makapix_store_identity(makapix_identity_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) {
        return P3A_ERR_MISSING;
    }
    size_t key_len = sizeof(out->player_key), host_len = sizeof(out->mqtt_host);
    const bool ok = nvs_get_str(nvs, K_KEY, out->player_key, &key_len) == ESP_OK &&
                    nvs_get_str(nvs, K_HOST, out->mqtt_host, &host_len) == ESP_OK &&
                    nvs_get_u16(nvs, K_PORT, &out->mqtt_port) == ESP_OK && out->player_key[0] != '\0' &&
                    out->mqtt_host[0] != '\0' && out->mqtt_port != 0;
    nvs_close(nvs);
    if (!ok) {
        memset(out, 0, sizeof(*out));
        return P3A_ERR_MISSING;
    }
    return ESP_OK;
}

// A PEM as a string in PSRAM, or NULL. A blob that is not a terminated
// string of a sane size is treated as absent.
static char *load_pem(nvs_handle_t nvs, const char *key)
{
    size_t len = 0;
    if (nvs_get_blob(nvs, key, NULL, &len) != ESP_OK || len < 2 || len > MAKAPIX_PEM_MAX) {
        return NULL;
    }
    char *pem = psram_malloc(len);
    if (pem == NULL) {
        return NULL;
    }
    if (nvs_get_blob(nvs, key, pem, &len) != ESP_OK || pem[len - 1] != '\0' || strlen(pem) != len - 1) {
        free(pem);
        return NULL;
    }
    return pem;
}

esp_err_t makapix_store_pems(makapix_pems_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) {
        return P3A_ERR_MISSING;
    }
    out->ca = load_pem(nvs, K_CA);
    out->cert = load_pem(nvs, K_CERT);
    out->key = load_pem(nvs, K_CLIENTKEY);
    nvs_close(nvs);
    if (out->ca == NULL || out->cert == NULL || out->key == NULL) {
        makapix_pems_free(out);
        return P3A_ERR_MISSING;
    }
    return ESP_OK;
}

bool makapix_store_token(char out[MAKAPIX_TOKEN_LEN])
{
    out[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = MAKAPIX_TOKEN_LEN;
    const esp_err_t err = nvs_get_str(nvs, K_TOKEN, out, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return out[0] != '\0';
}

static bool text_ok(const char *text, size_t max)
{
    return text != NULL && text[0] != '\0' && strlen(text) < max;
}

static bool pems_ok(const makapix_pems_t *pems)
{
    return pems != NULL && text_ok(pems->ca, MAKAPIX_PEM_MAX) && text_ok(pems->cert, MAKAPIX_PEM_MAX) &&
           text_ok(pems->key, MAKAPIX_PEM_MAX);
}

static esp_err_t set_pems(nvs_handle_t nvs, const makapix_pems_t *pems)
{
    esp_err_t err = nvs_set_blob(nvs, K_CA, pems->ca, strlen(pems->ca) + 1);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, K_CERT, pems->cert, strlen(pems->cert) + 1);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, K_CLIENTKEY, pems->key, strlen(pems->key) + 1);
    }
    return err;
}

esp_err_t makapix_store_set_token(const char *token)
{
    if (!text_ok(token, MAKAPIX_TOKEN_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, K_TOKEN, token);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t makapix_store_save(const makapix_identity_t *identity, const makapix_pems_t *pems, const char *token)
{
    if (identity == NULL || strlen(identity->player_key) != MAKAPIX_PLAYER_KEY_LEN - 1 ||
        !text_ok(identity->mqtt_host, MAKAPIX_HOST_LEN) || identity->mqtt_port == 0 || !pems_ok(pems) ||
        (token != NULL && token[0] != '\0' && strlen(token) >= MAKAPIX_TOKEN_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, K_KEY, identity->player_key);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, K_HOST, identity->mqtt_host);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, K_PORT, identity->mqtt_port);
    }
    if (err == ESP_OK) {
        err = set_pems(nvs, pems);
    }
    if (err == ESP_OK && token != NULL && token[0] != '\0') {
        err = nvs_set_str(nvs, K_TOKEN, token);
    }
    // NVS has no transaction: a failure part of the way (a full partition)
    // can leave a new key beside old certificates. That registration fails
    // at the broker like any bad one and is handled as one (renewal, then
    // "invalid"); the caller reports the failure and keeps what it holds
    // in memory to try again.
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t makapix_store_save_pems(const makapix_pems_t *pems)
{
    if (!pems_ok(pems)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = set_pems(nvs, pems);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t makapix_store_clear(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
