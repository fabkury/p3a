// SPDX-License-Identifier: Apache-2.0
// A registered player's questions to Makapix Club, over HTTPS (D80).
// Private to the Makapix source.
//
//   POST https://makapix.club/api/player/rpc
//   Authorization: Bearer {the player's token}
//
// The body is one request object ({"request_type": ...}); the answer is the
// same object the server would give over MQTT. 1.x asked over MQTT and
// matched answers to questions through a table of waiters; here a question
// and its answer are one HTTP exchange, on the caller's task.
//
// The token is the registration's (makapix_store.h). A device that has none
// (a registration made before tokens, or one whose token write was lost)
// asks for one first, and a token the server no longer knows is replaced
// once: POST /api/player/{player_key}/token/rotate.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot, before the first question.
esp_err_t makapix_rpc_start(void);

// Sends `body` and reads the answer into `out` (a string). `stop` (may be
// NULL) is asked during the exchange, as http_fetch's should_abort.
//   P3A_ERR_NO_KEY        the device is not registered
//   P3A_ERR_AUTH          the server does not accept this player, even with a new token
//   P3A_ERR_RATE_LIMITED  too many questions (60 a minute for reads, 30 for reactions)
// and http_fetch's other answers. What the answer says is the caller's to read.
esp_err_t makapix_rpc(const char *body, char *out, size_t out_len, bool (*stop)(void *), void *stop_ctx);

// The same exchange with another address of the player's API (the
// certificate renewal). `body` NULL for a request with none.
esp_err_t makapix_authorized_post(const char *url, const char *body, char *out, size_t out_len, bool (*stop)(void *),
                                  void *stop_ctx);

// "p3a/{version}": what p3a says it is to Makapix. The server counts
// requests so named as a player's.
const char *makapix_user_agent(void);

#ifdef __cplusplus
}
#endif
