// SPDX-License-Identifier: Apache-2.0
// What a registration is made of, in NVS. Private to the Makapix source.
//
// Namespace "makapix" (its own, not the settings': D32):
//
//   player_key   str    the player's UUID, 36 characters
//   mqtt_host    str    the broker
//   mqtt_port    u16
//   ca_cert      blob   PEM text with its terminator: what the broker's certificate is checked against
//   client_cert  blob   PEM: this player's certificate
//   client_key   blob   PEM: its private key
//   api_token    str    the bearer token certificate renewal uses
//
// A registration is written in one go and committed once; the PEMs are
// written after the key, so a device that "is registered"
// (makapix_store_registered) always has all of it. None of it is ever
// logged.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAKAPIX_PLAYER_KEY_LEN 37
#define MAKAPIX_HOST_LEN       64
#define MAKAPIX_TOKEN_LEN      128
#define MAKAPIX_PEM_MAX        4096  // each PEM, with its terminator

typedef struct {
    char player_key[MAKAPIX_PLAYER_KEY_LEN];
    char mqtt_host[MAKAPIX_HOST_LEN];
    uint16_t mqtt_port;
} makapix_identity_t;

// The three PEMs, each a string in PSRAM. Free with makapix_pems_free().
typedef struct {
    char *ca;
    char *cert;
    char *key;
} makapix_pems_t;
void makapix_pems_free(makapix_pems_t *pems);

// Whether the device has a registration it can connect with: the key, the
// broker and all three PEMs.
bool makapix_store_registered(void);

// P3A_ERR_MISSING when there is no registration.
esp_err_t makapix_store_identity(makapix_identity_t *out);
esp_err_t makapix_store_pems(makapix_pems_t *out);

// False, and "", when there is none.
bool makapix_store_token(char out[MAKAPIX_TOKEN_LEN]);
esp_err_t makapix_store_set_token(const char *token);

// Everything at once. `token` may be NULL or "": a stored token then stays.
// ESP_ERR_INVALID_ARG for a key, host or PEM that is empty or too long.
esp_err_t makapix_store_save(const makapix_identity_t *identity, const makapix_pems_t *pems, const char *token);
// Renewed certificates: the three PEMs, one commit.
esp_err_t makapix_store_save_pems(const makapix_pems_t *pems);

// Forgets the registration: every key above.
esp_err_t makapix_store_clear(void);

#ifdef __cplusplus
}
#endif
