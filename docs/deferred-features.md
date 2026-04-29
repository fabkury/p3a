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

## 2. Makapix swipe-reaction surface (REST and/or MQTT)

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
