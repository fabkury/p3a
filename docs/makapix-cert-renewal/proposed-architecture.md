# Makapix Club Certificate Renewal — Proposed Architecture

**Status:** Proposal / not implemented.
**Scope:** Transparent rotation of the mTLS client certificate p3a uses to authenticate to the Makapix Club MQTT broker, before the certificate's 1-year lifetime elapses.

---

## 1. Problem

When p3a registers with Makapix Club, the server issues a TLS client certificate/private key that the device uses for mTLS authentication to the MQTT broker. The cert is valid for one year. **No renewal mechanism exists today**, so any p3a that stays in the field past its first anniversary will silently lose its connection to Makapix Club.

Working assumptions for this design:

- We have full cooperation from the Makapix Club backend team — we can request any endpoint, schema, or rate-limit policy we need.
- All currently-deployed devices will receive a firmware update before their certificate expires. We are NOT designing a migration path for devices that miss the firmware window — those will require manual re-registration (registration code → web claim).

The design goal is: **after the firmware update lands, certificate rotation happens transparently for the user, for the rest of the device's life, in the common case**.

---

## 2. Current state (snapshot)

Reference points in the codebase (paths relative to repo root):

- **Provisioning flow** — `components/makapix/makapix_provision.c`. Two HTTP endpoints today:
  - `POST /api/player/provision` → returns `player_key` (UUID, persistent identity), `registration_code` (6-char, 15-min TTL), broker host/port.
  - `GET /api/player/{player_key}/credentials` → polled every 3 s after registration; returns `{ca_pem, cert_pem, key_pem}` once the user has claimed the device.
- **Storage** — `components/makapix/makapix_store.c`. Certs, `player_key`, and broker info live in the `makapix` NVS namespace (keys `ca_cert`, `client_cert`, `client_key`, `player_key`, `mqtt_host`, `mqtt_port`).
- **MQTT connection** — `components/makapix/makapix_mqtt.c`. Static 4 KB buffers for the three PEM blobs. Reconnect loop with exponential backoff in `components/makapix/makapix_connection.c`. After 3 consecutive `MBEDTLS_SSL_HANDSHAKE_FAILED` errors, the state machine transitions to `MAKAPIX_STATE_REGISTRATION_INVALID` and stops retrying (`makapix_connection.c:226-233`).
- **Identity** — `player_key` is a UUID that survives across certificate changes. All MQTT topics, OPC retained state, and channel subscriptions are keyed on it.
- **Pre-existing design choice** — the server generates the device's private key and returns it as `key_pem`. Suboptimal from a key-hygiene standpoint, but predates this design and is out of scope for this proposal.

---

## 3. Proposed architecture

### 3.1 Three triggers, one renewal task

A single renewal worker can be invoked from three sources:

1. **Proactive (primary).** On boot (after SNTP succeeds) and once per 24 h, parse the stored client cert (`mbedtls_x509_crt_parse` → `valid_to`) and compute days remaining. If `< RENEW_THRESHOLD_DAYS` (recommend **30**), enqueue a renewal.
2. **Opportunistic.** After every successful MQTT connect, check the cached `valid_to`. If we're inside the renewal window and haven't tried in the last ~6 h, enqueue.
3. **Server-pushed (optional).** A new MQTT command `renew_credentials` on the existing command topic lets the backend force a renewal across the fleet (e.g., CA compromise, mass rotation).

All three funnel into one single-flight renewal task guarded by a mutex.

### 3.2 Two endpoints, two threat models

**Primary: mTLS-authenticated renewal.**

```
POST /api/player/credentials/renew
  Auth: mTLS using the device's current client cert
  Server derives player_key from the client cert subject (do NOT trust URL/body)
  Body (optional): {"want_recovery_token": true, "csr_pem": "..."}
  Response:
    {
      "ca_pem": "...",
      "cert_pem": "...",
      "key_pem": "...",          // omitted if device supplied a CSR
      "recovery_token": "..."    // present on first request after firmware upgrade
    }
```

Why mTLS? It's the strongest possible proof: only the device that holds the still-valid private key can authorize rotation of that key. No bearer tokens, no shared secrets.

**Fallback: recovery-token authenticated.**

```
POST /api/player/{player_key}/credentials/recover
  Auth: Authorization: Bearer <recovery_token>, server-cert TLS only (no mTLS)
  Body: empty
  Response: same shape as renew, including a NEW recovery_token (server rotates on every use)
```

Used only when mTLS is no longer possible — cert already expired, key corrupted, or server rejected renewal. The recovery token is the device's "refresh token" against the catastrophic case of being offline through its entire renewal window.

### 3.3 Atomic swap with rollback

Storage extensions to the `makapix` NVS namespace:

| Key | Purpose |
|---|---|
| `ca_cert`, `client_cert`, `client_key` | Active (unchanged) |
| `ca_cert_prev`, `client_cert_prev`, `client_key_prev` | Last-known-good, for rollback |
| `recovery_token` | Long-lived recovery secret |
| `cert_renew_state` | Tiny blob: `{generation, last_renewed_at, in_progress}` |

Swap sequence:

1. Set `cert_renew_state.in_progress = true`, commit.
2. Write new certs to scratch keys (`*_new`).
3. **Verify**: one-shot mbedTLS handshake to the broker using the new certs (connect → disconnect, no subscribe). If it fails, delete scratch and bail.
4. Promote: current → prev, scratch → current. Clear `in_progress`.
5. On next MQTT reconnect cycle, the existing reconnect task at `components/makapix/makapix_connection.c:286-291` picks up the new certs automatically.
6. After first successful MQTT connect with the new certs, erase `*_prev`.

Crash safety: on boot, if `in_progress = true` and scratch is incomplete, drop scratch. If active fails mTLS, fall back to `*_prev`.

### 3.4 Renewal trigger pseudocode

```
on_boot() and every 24h:
    if !sntp_synced: skip                        # don't trust local clock
    days_left = parse_cert(active.client_cert).notAfter - now()
    if days_left < 30: enqueue_renewal()
    publish_opc_state({cert_expires_at: ...})    # fleet visibility

on_mqtt_connected():
    if days_left < 30 and !recently_attempted: enqueue_renewal()

on_mqtt_command("renew_credentials"):
    ack_immediately()
    enqueue_renewal()

renewal_task():
    if !mutex.try_lock(): return                 # single-flight
    if try_renew_mtls() ok:
        commit_swap()
    elif recovery_token present:
        if try_recover() ok: commit_swap()
        else: log_error(); back_off()
    else:
        # No recovery token and mTLS failed.
        # Surface as MAKAPIX_STATE_REGISTRATION_INVALID (existing terminal state).
        log("re-provisioning required by user")
```

### 3.5 Identity continuity

`player_key` does not change across renewals. Therefore:

- MQTT topic tree `makapix/player/{player_key}/...` is untouched.
- OPC retained state (`makapix/player/{player_key}/state`) is untouched.
- Channel subscriptions, view-ack topics, response topics: all keep working.
- Server-side player record stays the same — only the cert/key columns get rotated.

The reconnect task already reloads certs from NVS each iteration (`makapix_connection.c:275-282`), so the hot-swap is invisible to higher layers.

---

## 4. Server-side asks (concrete)

Hand these to the Makapix Club backend team:

1. **`POST /api/player/credentials/renew`** — mTLS-authenticated; `player_key` derived from the client cert subject (do NOT trust URL/body for identity). Accepts optional `{"want_recovery_token": true, "csr_pem": "..."}`. Response includes `recovery_token` only when requested.
2. **`POST /api/player/{player_key}/credentials/recover`** — server-TLS only, `Authorization: Bearer <recovery_token>`. Server rotates the token on each successful call and returns the new one. Rate-limit aggressively (e.g., 5/hour, exponential backoff after failures, alert on N failures).
3. **Cert overlap window** — when issuing a renewal, keep the previous cert valid for **~7 days** rather than revoking immediately. Closes the race where a device commits the swap and crashes before the new cert is verified end-to-end.
4. **MQTT command `renew_credentials`** — pushed on the existing `makapix/player/{key}/command` topic, follows the OPC envelope (`command_type`, `command_id`); ack on `command/ack`. No payload required (or `{reason: "ca_rotation"}` for logging).
5. **Telemetry** — accept a `cert_expires_at` field in the OPC `state` retained message so the backend can dashboard fleet-wide expiry distribution.

---

## 5. Pros

- **Transparent** for any device that is online at least once during its renewal window. Manual user action is required only in the pathological case: device offline for >1 year AND recovery token lost (corrupt NVS).
- **No identity churn.** `player_key` is stable → no topic remapping, no broken channel subscriptions, no OPC re-init. Drops into the existing `makapix_internal.h` state machine.
- **mTLS authenticates the renewal itself.** Strongest possible auth on the primary path — only the holder of the still-valid private key can rotate.
- **Recovery token is the safety net** for the long-tail "device unplugged for 14 months" case. Without it, you eventually re-introduce the registration-code dance.
- **Composes with the existing reconnect task.** The cert swap is invisible to higher layers; the same path that loads certs today is the same path that loads them after a swap.
- **Server-push command is nearly free** but gives operations a panic button for mass rotation.
- **CSR-ready.** The renewal endpoint accepting an optional `csr_pem` body leaves room for a future firmware to do client-side keygen, closing the existing oddity that the server knows every device's private key — without another backend change.

---

## 6. Cons

- **NVS pressure.** Doubling cert storage (active + prev + recovery token) bumps the makapix namespace from ~12 KB to ~25 KB worst case, against a 24 KB NVS partition. Mitigation: move cert blobs to LittleFS or a small dedicated raw partition and keep NVS for small metadata only. This is a partition-layout decision that needs to land up front.
- **Server complexity creeps up.** Per-player rate limiting, mTLS→player mapping, recovery-token rotation, overlap window in the CA — each small, cumulatively non-trivial.
- **Recovery token is a long-lived secret.** New attack surface. NVS dump = permanent identity theft for that device. Rotate-on-use + rate-limit + per-renewal alerting are the standard mitigations; binding to an eFuse-stored device factor is the next level if the threat model demands it.
- **Verify-before-commit doubles TLS work per renewal.** Two handshakes (verification + production reconnect). Negligible in absolute terms but worth noting on fragile Wi-Fi links.
- **Clock dependency.** SNTP must succeed before the proactive trigger fires. A device on a network that blocks NTP still gets renewed eventually via the post-MQTT-connect opportunistic check (the broker handshake itself surfaces near-expiry server-side), but is at higher risk of cliff-edge behavior.

---

## 7. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Bad server deploy issues malformed certs; whole fleet's renewals fail; real expiry then hits. | High | Verify-before-commit rolls back per device. Server-side staged rollout of CA changes. Canary cohort renewing on a 60-day threshold so issues surface a month before mass impact. |
| Renewal endpoint regression rejects valid certs (cert→player mapping bug). | Medium | Per-day rate-limit floor + alert on renewal failure rate spikes. Client retries with exponential backoff. Recovery-token fallback still works because it doesn't depend on mTLS. |
| Time-bomb in the renewal mechanism itself (algorithm change, mbedTLS bug in a future ESP-IDF). | Medium | End-to-end smoke test running continuously in production against a canary device. |
| Clock-skew false positive: device thinks cert is expired, isn't, server rejects renewal as redundant. | Low | Renewal endpoint MUST be idempotent — accepting a renewal call against a not-yet-near-expiry cert either returns the existing cert or rotates anyway. Device must never erase certs purely on local-time judgment. |
| Concurrent renewals: boot-scheduled + post-connect opportunistic + server push all fire in the same minute. | Low | Single-flight mutex in the renewal task. |
| Mid-swap power loss leaves NVS inconsistent. | Low | `in_progress` flag + scratch keys + boot-time cleanup. |
| Recovery-token leak from one device → permanent identity theft for that device. | Medium | Rotate on every use. Rate-limit per `player_key`. Log + alert on recovery requests. Bind to eFuse-stored device secret on future hardware revs. |
| Server-side overlap missing: server revokes the old cert immediately; device crashes before verifying new; on reboot, neither cert works. | Low | The 7-day server-side overlap is the only reliable defense. Insist on it. |
| Recovery token never gets issued because the bootstrap (`want_recovery_token` on first post-upgrade renewal) silently fails. | Medium | After successful renewal, check `recovery_token` is present; if not, retry bootstrap on next renewal. Publish `has_recovery_token` in OPC state so backend can dashboard coverage. |
| Pre-existing private-key-server design continues — server still knows every device's private key. | Pre-existing | Out of scope for this fix. Renewal endpoint accepts optional CSR so a future firmware can opt into client-side keygen without another backend change. |

---

## 8. Repo touch surface

Minimal and mostly additive:

- **New** `components/makapix/makapix_cert_lifecycle.{c,h}` — cert parsing (mbedtls_x509), days-remaining computation, renewal trigger logic, swap state machine.
- **New** `components/makapix/makapix_renew.{c,h}` — HTTPS client for the two new endpoints (mirrors `makapix_provision.c` in shape).
- **Extend** `components/makapix/makapix_store.{c,h}` — `_prev` slot accessors, `recovery_token` accessors, `cert_renew_state` blob accessors.
- **Wire** a daily timer in `components/makapix/makapix.c` and a post-`MQTT_EVENT_CONNECTED` hook in `components/makapix/makapix_connection.c:128`.
- **Add** `renew_credentials` command dispatch alongside existing OPC handlers in `components/makapix/makapix_opc.c`.
- **Add** `cert_expires_at` field to the OPC state publisher.
- **Partition layout** — decide between expanded NVS partition (works but tight) vs. cert blobs to LittleFS or a new raw partition (recommended).

Existing provisioning, polling, MQTT, and reconnect code can stay untouched.

---

## 9. Open questions for review

1. **Partition layout** — bump NVS, or move cert blobs to LittleFS/dedicated partition? Recommended: dedicated. Decide before any code lands.
2. **Recovery-token rotation cadence** — every use (proposed) vs. every N uses vs. monthly. Every-use is simplest and strongest; flag if backend storage cost matters.
3. **Renewal threshold** — 30 days proposed. Lower (e.g., 14) means less server load but less margin for offline devices. Higher (e.g., 60) means more server load but more margin and earlier canary signal.
4. **Server-side overlap window** — 7 days proposed. Negotiable with backend team based on their CA's revocation model.
5. **CSR-based keygen for future firmware** — should the renewal endpoint be specced to accept CSRs from day one, even though the current firmware won't use it? Recommended: yes (cheap to add, expensive to retrofit).
