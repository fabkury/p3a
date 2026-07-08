# Makapix mTLS Client-Certificate Renewal — Firmware Plan

**Status: IMPLEMENTED on `feature/makapix-cert-renewal` (2026-07-08) — see PROGRESS.md. Open questions resolved: no webui renew button; self-heal = expiry-first, one unconditional attempt before latching.**

## Why now

Fleet client certificates issued before 2026-05-27 carry **365-day validity** (the
server's `CERT_VALIDITY_DAYS` was raised to 1095 only in MPX commit `db146d9`).
Per the MPX team's `docs/player/cert-renewal-plan.md`, the fleet's certs expire
roughly **2026-12-12 → 2027-05-27**, and the broker rejects an expired client
cert at the TLS handshake. Today the firmware stores its cert/key/CA once at
registration (`makapix_store.c`) and never renews; on expiry a device hits
repeated `0x801a` handshake failures, latches `REGISTRATION_INVALID`
(`makapix_connection.c`), and requires manual re-registration under a **new**
player_key.

The server side shipped 2026-05-27 and is waiting on us:

- `POST /api/player/renew-cert` — bearer-token auth (`Authorization: Bearer
  mpx_live_…`), **works even after the cert has expired**, make-before-break
  (old cert not revoked), returns `cert_pem` + `key_pem` + `ca_pem` +
  `cert_expires_at` in one response. Guard: 400 unless within
  `CERT_RENEWAL_THRESHOLD_DAYS` (90) of expiry or already expired. Rate limits:
  10/day/player, 30/hour/IP.
- `POST /api/player/{player_key}/token/rotate` — token bootstrap, gated only on
  knowledge of `player_key`. Returns `api_token`. **All currently deployed p3a
  devices have no stored token** (the firmware discards the `api_token` field of
  the credentials response), so this is every device's first step.

The renewal window for the earliest certs opens **~2026-09-13**. Firmware must
be released *and OTA-adopted* well before **2026-12-12**.

> **Fleet cliff CONFIRMED from prod DB (server team, 2026-07-08, message
> 0002):** earliest expiry **2026-12-12 09:12 UTC**; 4 certs expire Dec 2026,
> 15 of 24 registered players expire before Jun 2027, the other 9 already
> carry 3-year certs. Earliest renewal window opens **2026-09-13** — that is
> the firmware-release deadline.

## Design

### New module: `components/makapix/makapix_renewal.c/.h`

Owns the renewal state machine. Everything HTTP goes through the existing
`http_fetch` component (same pattern as `makapix_provision.c`: 32 KB SPIRAM
response buffer, cJSON parse, `MAKAPIX_PEM_MAX_LEN` size guards).

### Storage (`makapix_store`)

- New NVS key `api_token` (string, ~64 B) in the existing `makapix` namespace,
  with getter/setter; erased in `makapix_store_clear()`.
- New `makapix_store_save_renewed_certs(ca_pem, cert_pem, key_pem)` — writes all
  three blobs under one handle with a **single commit**, same atomicity pattern
  as `makapix_store_save_registration()` (b692beee). player_key/host/port are
  untouched. Power loss before the commit leaves the old (still-working,
  make-before-break) credentials intact.

### Expiry tracking (local, no server call)

- Parse the stored client cert with `mbedtls_x509_crt_parse` (mbedTLS already
  linked via esp-tls) and read `valid_to`.
- Convert `mbedtls_x509_time` → epoch with a small days-since-epoch helper
  (avoid `mktime` timezone traps; cert times are UTC).
- Sanity-gate on system time: skip the check until SNTP has set a plausible
  clock (`now > 2024-01-01`), else a 1970 clock would trigger a spurious
  renewal storm across the fleet.

### When to renew

- **Window:** `now ≥ notAfter − RENEW_WINDOW_DAYS` (Kconfig, default **45**;
  server allows 90 — staying inside 90 means the server guard never 400s us in
  steady state).
- **Jitter:** add a per-check random offset (0–7 days via `esp_random()`) so the
  fleet doesn't stampede the API on the same day. Recomputed per check is fine
  at a daily cadence.
- **Cadence:** check once shortly after MQTT connects (time is synced, network
  proven) and every 24 h thereafter (esp_timer → work posted to the network
  task; never do HTTPS from the timer callback itself).

### Renewal sequence

1. Load token from NVS. If absent (all current devices): `POST
   /api/player/{player_key}/token/rotate` → persist `api_token`.
2. `POST /api/player/renew-cert` with `Authorization: Bearer <token>`.
   - **200** → parse `cert_pem`/`key_pem`/`ca_pem`, size-check against
     `MAKAPIX_PEM_MAX_LEN`, `makapix_store_save_renewed_certs()`, then
     `makapix_mqtt_deinit()` + re-init with the new PEMs (same code path the
     provisioning flow uses after saving). Old cert stays in use until the NVS
     commit succeeds — make-before-break end to end.
   - **400** (outside window) → not due; next daily check.
   - **401** → token stale/revoked: rotate token once, retry once; if still 401,
     log ERROR and retry next day.
   - **404** on token/rotate → player deleted server-side: genuine
     `REGISTRATION_INVALID`, latch as today.
   - **429 / 5xx / transport error** → back off to next daily check (server
     rate limits are far above our cadence; no tight retry loops).

### Self-heal (the December safety net)

In `makapix_connection.c`, at the `MAX_AUTH_FAILURES` threshold that today
latches `REGISTRATION_INVALID`: first check whether the stored cert is expired
per local clock, and if so (or unconditionally, once) run the renewal sequence.
Only latch `REGISTRATION_INVALID` if renewal itself says the registration is
dead (404 on rotate) or renewal succeeded yet the broker still rejects us
(true ghost registration). Result: a device that slept through its expiry
self-heals on the next boot instead of demanding re-registration — and a whole
class of future ghost-registration-style incidents becomes self-recovering.

### New registrations: keep the token from day one

`makapix_poll_credentials()` parses `ca_pem`/`cert_pem`/`key_pem` but ignores
`api_token`, which the server returns **once** on the first credentials fetch.
Parse it and pass it through to the registration save so newly registered
devices never need the rotate call. (Absent field → fall back to rotate later,
as for the existing fleet.)

### Observability

- INFO log lines for: window entered, token bootstrapped, renewal success (new
  `cert_expires_at`), each failure class. Operator-facing — keep at INFO.
- Extend the existing Makapix health surface (`http_api` status JSON + webui
  health badge from 2c502c5b) with `cert_expires_at` and last-renewal result.
  WebUI version bump per the usual rule (WEBUI_VERSION in root CMakeLists.txt).

### Kconfig

- `MAKAPIX_CERT_RENEW_WINDOW_DAYS` (default 45)
- `MAKAPIX_CERT_RENEW_CHECK_HOURS` (default 24)
- (host reuses `CONFIG_MAKAPIX_CLUB_HOST`, as provisioning does)

## Server contract (verified against MPX source 2026-07-08)

**`POST /api/player/{player_key}/token/rotate`** (`routers/player.py:400`)
- Auth: none beyond knowledge of `player_key`; player must be `registered`.
- Rate limit: 30/hour/IP (429). 404 = player not found / not registered →
  treat as registration truly dead.
- Response 200: `{"api_token": "mpx_live_…", "rotated_at": "<ISO8601>"}`.
- **Rotation revokes the previous token.** Firmware must persist the new token
  to NVS *before* relying on it; if the NVS write fails, retry the write (the
  response is still in RAM) — do not re-call rotate in a loop.

**`POST /api/player/renew-cert`** (`routers/player.py:447`)
- Auth: `Authorization: Bearer <api_token>` (`get_current_player`); works with
  an expired client cert by design.
- Response 200 (`PlayerSelfRenewResponse`, `schemas.py:1150`):
  `{"cert_pem": …, "key_pem": …, "ca_pem": …, "cert_expires_at": "<ISO8601>",
  "message": …}`. All three PEMs must be parsed; `ca_pem` refreshes the trust
  anchor in the same call.
- 400 = outside renewal window; 401 = token invalid (rotate once, retry once);
  429 = rate-limited (10/day/player, 30/hour/IP).

**`GET /api/player/{player_key}/credentials`** (registration poll)
- `api_token` is present **only on the very first fetch** (server mints it iff
  no active token exists). Re-fetching later returns `api_token: null` — which
  is why the existing fleet, having discarded it, must bootstrap via
  `token/rotate` and why new registrations should capture it immediately.

## Server-side prerequisites (MPX team — drafted, pending landing)

- Dev overlay `CERT_RENEWAL_THRESHOLD_DAYS=3650` (API + worker) so fresh
  1095-day dev certs are immediately renewable for end-to-end testing;
  prod stays 90.
- CRL reload watcher in the broker container (separate issue found during the
  audit: nightly CRL renewal wasn't visible to a long-running broker; also
  makes device revocation near-immediate).

## Test plan (against development.makapix.club)

1. Fresh registration → verify `api_token` captured and persisted.
2. In-window renewal (dev threshold makes every cert "in window"): full
   sequence, MQTT reconnects with the new cert, old cert still accepted until
   its own expiry (server does not revoke it).
3. Token bootstrap path: erase `api_token` from NVS → rotate → renew.
4. 401 recovery: owner-rotate the token via web, device renews anyway.
5. Post-expiry self-heal: simulate by corrupting/expiring the stored cert (or
   MPX hand-sets `cert_expires_at` in dev DB) → device recovers without
   re-registration and without latching `REGISTRATION_INVALID`.
6. Failure hygiene: server down / 429 → daily backoff, no tight loop; NVS
   write failure mid-save → old credentials keep working.
7. Clock-gate: boot with NTP blocked → no renewal attempt, no false latch.

## Rollout

- Target next minor release (v1.2.0). Ship early: OTA uptake lag is the real
  deadline, not the code.
- After first fleet-wide renewal every cert is 3-year; cadence drops sharply.

## Open questions

1. Confirmed earliest `cert_expires_at` from prod (Fab) — sets the hard date.
2. Should the webui also offer a manual "Renew now" button (forwarding to the
   owner-gated endpoint) for support scenarios? Cheap once status is surfaced.
3. Self-heal trigger: renewal attempt strictly on local-clock expiry, or on any
   `MAX_AUTH_FAILURES` burst? (Plan says: expiry-first, one unconditional
   attempt before latching — confirm.)
