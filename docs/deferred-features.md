# Deferred Implementation Tasks

User-facing documentation (`README.md`, `docs/HOW-TO-USE.md`) currently claims a few features that the firmware does not yet implement. Rather than soften the docs, the plan is to make the docs accurate by implementing the missing pieces.

Identified during the verification reviews of `README.md` and `docs/HOW-TO-USE.md` (per-statement findings live in `readme_review/` and `howto_review/`).

---

## 1. Web UI rotation control

**Doc claims affected**:
- `README.md` — Web Interface section: "All controls are also available via the web UI and REST API"
- `README.md` — REST API section: "Every action is also exposed as a JSON API endpoint"
- `docs/HOW-TO-USE.md` — Touch Controls / Screen Rotation: "You can also set rotation via the web interface or REST API"
- `docs/HOW-TO-USE.md` — Web Interface: "Configuration — brightness, screen rotation, settings"

**Reality**: REST endpoints exist (`GET /rotation`, `POST /rotation`) and accept values 0/90/180/270. No web UI control invokes them — search of `webui/` returns no rotation controls on the dashboard or settings page.

**To implement**: Add a rotation control to `webui/settings.html` (Display tab is the natural place) that calls the existing `POST /rotation` endpoint.

**Cleanup once done**: this entry can be removed; no doc edits should be needed since the doc already promises the feature.

---

## 2. Makapix MQTT brightness command

**Doc claims affected**:
- `docs/HOW-TO-USE.md` — Device Registration: "Remote control — change artwork, adjust brightness from anywhere"
- `docs/HOW-TO-USE.md` — Makapix Club Features → Remote control: "Adjust brightness"

**Reality**: `components/http_api/http_api.c:177-426` (the Makapix MQTT command dispatcher) accepts only `swap_next`, `swap_back`, `set_background_color`, `play_channel`, `show_artwork`, `show_url`, `swap_to`, `execute_playset`. Brightness is exposed only via the local REST API.

**To implement**: Add a `set_brightness` MQTT command that dispatches to the existing brightness setter (`components/http_api/http_api_rest_settings.c:182-187` shows the REST equivalent). The makapix.club server-side will need a corresponding control in its UI.

---

## 3. Makapix MQTT pause/resume commands

**Doc claims affected**:
- `docs/HOW-TO-USE.md` — Makapix Club Features → Remote control: "Control playback (pause/resume)"

**Reality**: `CMD_PAUSE` and `CMD_RESUME` exist as REST endpoints (`/action/pause`, `/action/resume`) and emit `P3A_EVENT_PAUSE` / `P3A_EVENT_RESUME` on the event bus, but they are not wired into the Makapix MQTT command dispatcher.

**To implement**: Add `pause` and `resume` cases to the MQTT command dispatcher in `components/http_api/http_api.c` that emit the same event-bus events the REST handlers already emit.

---

## 4. Makapix swipe-reaction surface (REST and/or MQTT)

**Doc claims affected**:
- `README.md` — REST API section: "Every action is also exposed as a JSON API endpoint for scripting and automation."

**Reality**: The Makapix thumbs-up reaction (swipe up to submit, swipe down to revoke) is touch-only. `makapix_api_submit_reaction()` and `makapix_api_revoke_reaction()` are called only from `components/p3a_core/p3a_touch_router.c:253-355`. There is no REST or MQTT path to trigger them, so "every action" overstates the API surface.

**To implement**: Add `submit_reaction` and `revoke_reaction` REST endpoints (and optionally MQTT commands) that call the existing reaction APIs. Once present, the README's "every action" claim becomes literally accurate.

---

## How to use this file

When implementing one of the above:
1. Land the feature in code.
2. Verify the relevant doc claim is now true (no rewording needed since we kept the doc claims as the target spec).
3. Remove the entry from this file.

If a claim is dropped from the docs instead of implemented, also delete the entry here.
