# Makapix mTLS Client-Certificate Renewal — Progress

## 2026-07-08 — Implementation (branch `feature/makapix-cert-renewal`)

Implemented per PLAN.md. Firmware version unchanged at `1.1.0` (Fab's call;
release versioning decided at release time), webui `2.15`.

### What was built

- **`components/makapix/makapix_renewal.c/.h` (new)** — renewal engine:
  - Periodic task (`cert_renew`, PSRAM stack): every check waits until
    Wi-Fi holds an IP **and** the clock is SNTP-synchronized (10 s poll; no
    arbitrary boot delay), then runs; repeats every
    `CONFIG_MAKAPIX_CERT_RENEW_CHECK_HOURS` (default 24), plus an early kick
    on every MQTT connect.
  - Local expiry read: `mbedtls_x509_crt_parse` on the stored client cert,
    `notAfter` → epoch via a UTC days-from-civil helper (no mktime/timezone
    traps).
  - Clock gate: no decision (and no API traffic) unless
    `time(NULL) >= 2026-01-01` — an unsynced 1970 clock cannot trigger a
    fleet renewal storm.
  - Window: `CONFIG_MAKAPIX_CERT_RENEW_WINDOW_DAYS` (default 45, inside the
    server's 90) with a fresh 0–7-day jitter draw per check; jitter is
    bypassed within 14 days of expiry and after expiry.
  - Token bootstrap: stored token, else `POST /player/{key}/token/rotate`
    (persist-before-rely; NVS write failure retries the write, never the
    rotate). Renewal: `POST /player/renew-cert` with `Authorization: Bearer`;
    one rotate-and-retry on 401; 400 = not-due; 404 = player gone; 429/5xx =
    wait for next check.
  - Persist: `makapix_store_save_renewed_certs()` — cert+key+CA in a single
    NVS commit (make-before-break; power loss leaves the old working set).
  - Adoption: disconnected devices pick the new PEMs up on the next reconnect
    cycle (which reads NVS); connected devices are left alone; a latched
    `REGISTRATION_INVALID` state is cleared and reconnection restarted when a
    forced renewal succeeds.
- **Self-heal** (`makapix_connection.c`): at the `MAX_AUTH_FAILURES`
  threshold, one forced renewal attempt per outage (flag re-armed on every
  successful connect) before latching `REGISTRATION_INVALID`. Expiry state
  per the local clock is logged with the attempt.
- **Token capture at registration** (`makapix_provision.c/.h`,
  `makapix_provision_flow.c`): the credentials response's one-time
  `api_token` is now parsed and persisted in the same NVS transaction as the
  registration (previously discarded — the reason the whole existing fleet
  needs the token/rotate bootstrap).
- **Store** (`makapix_store.c/.h`): `api_token` NVS key (get/set, cleared on
  unregister), `makapix_store_save_renewed_certs()`,
  `makapix_store_save_registration()` gained an optional `api_token` arg.
- **Status surfacing**: `/api/status` `makapix` object gains
  `cert_expires_at` (ISO 8601) + `cert_renewal` (last result); settings webui
  Makapix tab shows "Certificate valid until … (N days)" / "expired — renews
  automatically" under the health badge. No manual renew button (decision:
  Fab, 2026-07-08).
- **Config**: `MAKAPIX_CERT_RENEW_WINDOW_DAYS`, `MAKAPIX_CERT_RENEW_CHECK_HOURS`
  in `components/makapix/Kconfig`.

### Decisions recorded

- No webui "Renew now" button (owner-gated endpoint stays server-side only).
- Self-heal trigger: expiry-first logging, one unconditional attempt before
  latching — confirmed by Fab.
- Renewal never bounces a live MQTT connection; new certs apply on the next
  natural reconnect (make-before-break both ends).

### Pending

- [ ] Build + flash by Fab (per repo policy, Claude does not build).
- [ ] End-to-end test against development.makapix.club — UNBLOCKED: server
  team deployed `CERT_RENEWAL_THRESHOLD_DAYS=3650` (dev api+worker) and the
  CRL reload watcher on dev, both verified (message 0002, 2026-07-08). T4
  needs no manual broker restart. Server needs our test player_key + go/no-go
  pings before T3/T4. Results go back as message 0003.
- [x] Prod `min(cert_expires_at)` — CONFIRMED 2026-12-12 09:12 UTC (4 certs
  Dec 2026; 15/24 before Jun 2027; 9 already 3-year). Renewal window opens
  2026-09-13 = firmware release deadline. MPX plan doc updated by server team.
- [x] Prod stale-CRL check — no active outage (broker restarted 2026-07-07,
  loaded CRL valid to 2026-07-25); server team commits to deploying the CRL
  watcher to prod BEFORE 2026-07-25 (else manual broker restart Jul 18-25).
- [ ] Release + OTA rollout before 2026-09-13 (window-open) and hard-before
  2026-12-12 (first expiry); send release timeline to server team after e2e.
